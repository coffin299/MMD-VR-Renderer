#include "mmdvr/RenderController.hpp"
#include "mmdvr/ImageSequenceWriter.hpp"
#include "mmdvr/ProjectionEngine.hpp"
#include "mmdvr/StereoEngine.hpp"
#include "mmdvr/VideoEncoder.hpp"
#include "mmdvr/Diagnostics.hpp"
#include "mmdvr/MathUtil.hpp"
#include "mmdvr/Parallel.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace mmdvr {

namespace {

size_t estimateComposedRgbaBytes(const RenderSettings& s) {
  int w = s.outputWidth > 0 ? s.outputWidth : 1;
  int h = s.outputHeight > 0 ? s.outputHeight : 1;
  if (s.cubemapFaces && s.stereoMode == StereoMode::Stereo)
    w *= 2; // side-by-side
  return static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
}

unsigned long long availablePhysMb() {
#ifdef _WIN32
  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof(ms);
  if (!GlobalMemoryStatusEx(&ms)) return 0;
  return ms.ullAvailPhys / (1024ull * 1024ull);
#else
  return 0;
#endif
}

// Cap PNG queue so pending RGBA frames stay within a fraction of free RAM.
// 8K stereo SBS ≈ 900MB/frame — unlimited queues are still risky, but high-RAM
// PCs (48–64 GB) can keep several frames in flight.
int resolvePngQueueMax(const RenderSettings& s) {
  const size_t frameBytes = estimateComposedRgbaBytes(s);
  const unsigned long long availMb = availablePhysMb();
  int byRam = 2;
  if (frameBytes > 0 && availMb > 0) {
    // ~20% of free RAM for the wait queue (writers still hold extra copies).
    const size_t budget =
        static_cast<size_t>((availMb * 1024ull * 1024ull) * 20ull / 100ull);
    byRam = static_cast<int>(budget / frameBytes);
  }
  if (byRam < 1) byRam = 1;
  if (byRam > 12) byRam = 12;

  // Soft caps for huge frames — scale with how much RAM is actually free.
  if (frameBytes >= 400ull * 1024ull * 1024ull) {
    if (availMb >= 49152) byRam = (std::min)(byRam, 4);
    else if (availMb >= 32768) byRam = (std::min)(byRam, 3);
    else if (availMb >= 20480) byRam = (std::min)(byRam, 2);
    else byRam = 1;
  } else if (frameBytes >= 200ull * 1024ull * 1024ull) {
    if (availMb >= 32768) byRam = (std::min)(byRam, 6);
    else if (availMb >= 16384) byRam = (std::min)(byRam, 4);
    else byRam = (std::min)(byRam, 2);
  }

  if (s.writeQueueMax > 0)
    return (std::min)(s.writeQueueMax, byRam);
  return byRam;
}

} // namespace

RenderController::RenderController() = default;

void RenderController::setCapture(std::shared_ptr<IFrameCapture> capture) { capture_ = std::move(capture); }
void RenderController::setRedraw(std::shared_ptr<IRedrawRequest> redraw) { redraw_ = std::move(redraw); }
void RenderController::setAbortHandler(std::function<void()> fn) { abortHandler_ = std::move(fn); }
void RenderController::setCleanupHandler(std::function<void()> fn) { cleanupHandler_ = std::move(fn); }

void RenderController::notifyStage(const wchar_t* stage, const std::wstring& note) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    progress_.stage = stage ? stage : L"";
    progress_.stageNote = note;
    updateTimingLocked(progress_.completedCount);
  }
  if (progressCb_) progressCb_(progress());
}

void RenderController::applyQualityDefaults(RenderSettings& s) {
  // Resolution presets are 2:1 YouTube VR sizes. Face size is user-owned (0 = auto).
  s.applyResolutionPreset();
}

void RenderController::setSettings(const RenderSettings& s) {
  std::lock_guard<std::mutex> lock(mu_);
  settings_ = s;
  applyQualityDefaults(settings_);
}

RenderSettings RenderController::settings() const {
  std::lock_guard<std::mutex> lock(mu_);
  return settings_;
}

JobProgress RenderController::progress() const {
  std::lock_guard<std::mutex> lock(mu_);
  return progress_;
}

std::wstring RenderController::lastOutputDir() const {
  std::lock_guard<std::mutex> lock(mu_);
  return lastOutputDir_;
}

std::wstring RenderController::lastOutputMp4() const {
  std::lock_guard<std::mutex> lock(mu_);
  return lastOutputMp4_;
}

bool RenderController::lastEncodeOk() const {
  std::lock_guard<std::mutex> lock(mu_);
  return lastEncodeOk_;
}

std::wstring RenderController::lastEncodeError() const {
  std::lock_guard<std::mutex> lock(mu_);
  return lastEncodeError_;
}

std::wstring RenderController::lastEncodeFallback() const {
  std::lock_guard<std::mutex> lock(mu_);
  return lastEncodeFallback_;
}

void RenderController::cancel() {
  cancel_.store(true);
  paused_.store(false);
  if (asyncWriter_) asyncWriter_->cancelPending();
  if (abortHandler_) abortHandler_();
  std::lock_guard<std::mutex> lock(mu_);
  progress_.cancelled = true;
  progress_.paused = false;
  progress_.stage = L"cancelling";
}

void RenderController::pause() {
  if (!running_.load() || cancel_.load()) return;
  if (paused_.exchange(true)) return;
  std::lock_guard<std::mutex> lock(mu_);
  pauseBegan_ = std::chrono::steady_clock::now();
  pauseClockRunning_ = true;
  progress_.paused = true;
  progress_.stage = L"paused";
}

void RenderController::resume() {
  if (!paused_.exchange(false)) return;
  std::lock_guard<std::mutex> lock(mu_);
  if (pauseClockRunning_) {
    auto now = std::chrono::steady_clock::now();
    pausedAccumSec_ += std::chrono::duration<double>(now - pauseBegan_).count();
    pauseClockRunning_ = false;
  }
  progress_.paused = false;
  progress_.stage = L"rendering";
}

void RenderController::waitIfPaused() {
  while (paused_.load() && !cancel_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void RenderController::updateTimingLocked(int completedCount) {
  auto now = std::chrono::steady_clock::now();
  double pauseExtra = pausedAccumSec_;
  if (pauseClockRunning_) {
    pauseExtra += std::chrono::duration<double>(now - pauseBegan_).count();
  }
  double elapsed = std::chrono::duration<double>(now - runStart_).count() - pauseExtra;
  if (elapsed < 0) elapsed = 0;
  progress_.elapsedSec = elapsed;
  progress_.completedCount = completedCount;
  if (completedCount > 0) {
    progress_.secPerFrame = elapsed / completedCount;
    int remaining = progress_.totalFrames - completedCount;
    if (remaining < 0) remaining = 0;
    progress_.etaSec = progress_.secPerFrame * remaining;
  } else {
    progress_.secPerFrame = 0;
    progress_.etaSec = -1;
  }
}

ImageBuffer RenderController::composeFromFaces(
    const std::array<ImageBuffer, 6>& leftFaces,
    const std::array<ImageBuffer, 6>* rightFaces,
    const RenderSettings& settings) {
  const bool bilinear = settings.quality != QualityPreset::Fast;

  auto project = [&](const std::array<ImageBuffer, 6>& faces) {
    if (settings.vrMode == VrMode::VR360)
      return ProjectionEngine::cubemapToEquirect(faces, settings.outputWidth, settings.outputHeight, bilinear);
    return ProjectionEngine::cubemapToEquirect180(faces, settings.outputWidth, settings.outputHeight, bilinear);
  };

  if (settings.stereoMode == StereoMode::Mono || !rightFaces)
    return project(leftFaces);

  // Burn both eyes on CPU in parallel (each already multi-threaded by rows).
  ImageBuffer leftEq, rightEq;
  std::thread t([&] { leftEq = project(leftFaces); });
  rightEq = project(*rightFaces);
  t.join();
  return StereoEngine::composeSideBySide(leftEq, rightEq);
}

namespace {

const wchar_t* cubeFaceName(CubeFace face) {
  switch (face) {
  case CubeFace::PosX: return L"posx";
  case CubeFace::NegX: return L"negx";
  case CubeFace::PosY: return L"posy";
  case CubeFace::NegY: return L"negy";
  case CubeFace::PosZ: return L"posz";
  case CubeFace::NegZ: return L"negz";
  default: return L"unknown";
  }
}

bool dumpFacesEnabled() {
  const char* value = std::getenv("MMDVR_DUMP_FACES");
  return value && value[0] != '\0' && value[0] != '0';
}

ImageBuffer scaleImage(const ImageBuffer& src, int dstW, int dstH, bool bilinear) {
  ImageBuffer out;
  if (src.empty() || dstW <= 0 || dstH <= 0) return out;
  out.resize(dstW, dstH);
  if (src.width == dstW && src.height == dstH) {
    out.rgba = src.rgba;
    return out;
  }
  for (int y = 0; y < dstH; ++y) {
    for (int x = 0; x < dstW; ++x) {
      float u = ((float)x + 0.5f) * (float)src.width / (float)dstW - 0.5f;
      float v = ((float)y + 0.5f) * (float)src.height / (float)dstH - 0.5f;
      uint8_t* dst = out.rgba.data() + ((size_t)y * (size_t)dstW + (size_t)x) * 4;
      if (!bilinear) {
        int ix = (std::max)(0, (std::min)(src.width - 1, (int)std::lround(u)));
        int iy = (std::max)(0, (std::min)(src.height - 1, (int)std::lround(v)));
        const uint8_t* p = src.rgba.data() + ((size_t)iy * (size_t)src.width + (size_t)ix) * 4;
        dst[0] = p[0]; dst[1] = p[1]; dst[2] = p[2]; dst[3] = p[3];
        continue;
      }
      int x0 = (int)std::floor(u), y0 = (int)std::floor(v);
      float fx = u - (float)x0, fy = v - (float)y0;
      auto sample = [&](int sx, int sy) -> const uint8_t* {
        sx = (std::max)(0, (std::min)(src.width - 1, sx));
        sy = (std::max)(0, (std::min)(src.height - 1, sy));
        return src.rgba.data() + ((size_t)sy * (size_t)src.width + (size_t)sx) * 4;
      };
      const uint8_t* p00 = sample(x0, y0);
      const uint8_t* p10 = sample(x0 + 1, y0);
      const uint8_t* p01 = sample(x0, y0 + 1);
      const uint8_t* p11 = sample(x0 + 1, y0 + 1);
      for (int c = 0; c < 4; ++c) {
        float a = p00[c] * (1.f - fx) + p10[c] * fx;
        float b = p01[c] * (1.f - fx) + p11[c] * fx;
        dst[c] = (uint8_t)std::lround(a * (1.f - fy) + b * fy);
      }
    }
  }
  return out;
}

} // namespace

bool RenderController::captureCubemapEye(bool leftEye, std::array<ImageBuffer, 6>& faces) {
  if (!redraw_) {
    Diagnostics::instance().error("No redraw backend; cannot multi-view render");
    return false;
  }
  auto s = settings();
  faces = {};

  const CameraState base = camera_.snapshot();
  // Opt-in only (MMDVR_DUMP_FACES=1). Auto-dump used the non-const PNG writer
  // which packRgbaToRgbInPlace()'d the live face buffers; the following
  // equirect pass then read past the end → c0000005 inside our d3d9.dll.
  const bool dumpFaces = dumpFacesEnabled();
  ImageSequenceWriter faceWriter(s.outputDir + L"\\debug_faces");
  const int debugFrame = progress().currentFrame;
  if (dumpFaces && leftEye && !firstFaceDebugDumped_) {
    char msg[256];
    std::snprintf(
        msg, sizeof(msg),
        "Cubemap base camera: valid=%d eye=(%.3f,%.3f,%.3f) target=(%.3f,%.3f,%.3f)",
        base.valid ? 1 : 0,
        base.eye.x, base.eye.y, base.eye.z,
        base.target.x, base.target.y, base.target.z);
    Diagnostics::instance().info(msg);

    ImageBuffer live;
    if (redraw_->captureView(live)) {
      if (!faceWriter.writeFrame(static_cast<const ImageBuffer&>(live), debugFrame,
                                 L"_L_live_mmd_camera"))
        Diagnostics::instance().error("Failed to write debug live MMD camera frame");
    } else {
      Diagnostics::instance().warn("Failed to capture debug live MMD camera frame");
    }
  }
  auto cams = camera_.buildFaceCameras(
      s.vrMode, s.stereoMode, leftEye, s.ipdUnits, s.faceSize, 0.1f, 10000.f);

  std::vector<CubeFace> order;
  if (s.vrMode == VrMode::VR360) {
    order = {CubeFace::PosX, CubeFace::NegX, CubeFace::PosY, CubeFace::NegY, CubeFace::PosZ, CubeFace::NegZ};
  } else {
    order = {CubeFace::PosZ, CubeFace::NegX, CubeFace::PosX, CubeFace::PosY, CubeFace::NegY};
  }
  for (size_t i = 0; i < cams.size() && i < order.size(); ++i) {
    waitIfPaused();
    if (cancel_.load()) return false;
    ImageBuffer face;
    if (!redraw_->renderView(cams[i], face)) {
      Diagnostics::instance().error("renderView failed for face");
      return false;
    }
    faces[(int)order[i]] = std::move(face);
    if (dumpFaces) {
      std::wstring suffix = leftEye ? L"_L_" : L"_R_";
      suffix += cubeFaceName(order[i]);
      // const overload copies first — never destroy face RGBA used by compose.
      if (!faceWriter.writeFrame(static_cast<const ImageBuffer&>(faces[(int)order[i]]),
                                 debugFrame, suffix.c_str()))
        Diagnostics::instance().error("Failed to write debug cubemap face");
    }
  }
  if (dumpFaces && leftEye && !firstFaceDebugDumped_) {
    firstFaceDebugDumped_ = true;
    Diagnostics::instance().info("Saved first left-eye cubemap faces under debug_faces");
  }
  return true;
}

bool RenderController::renderEye(bool leftEye, ImageBuffer& outEye) {
  if (!redraw_) {
    Diagnostics::instance().error("No redraw backend; cannot multi-view render");
    return false;
  }
  auto s = settings();

  // Default safe path: capture live MMD camera (no SetTransform). ray-mmd OK.
  if (!s.cubemapFaces) {
    waitIfPaused();
    if (cancel_.load()) return false;
    ImageBuffer live;
    if (!redraw_->captureView(live)) {
      Diagnostics::instance().error("captureView failed (safe / live camera)");
      return false;
    }
    const bool bilinear = s.quality != QualityPreset::Fast;
    outEye = scaleImage(live, s.outputWidth, s.outputHeight, bilinear);
    return !outEye.empty();
  }

  std::array<ImageBuffer, 6> faces{};
  if (!captureCubemapEye(leftEye, faces)) return false;

  outEye = composeFromFaces(faces, nullptr, [&]{
    RenderSettings mono = s; mono.stereoMode = StereoMode::Mono; return mono;
  }());
  return !outEye.empty();
}

bool RenderController::processAnimationFrame(int frameIndex) {
  waitIfPaused();
  if (cancel_.load()) return false;
  auto s = settings();
  {
    std::lock_guard<std::mutex> lock(mu_);
    progress_.currentFrame = frameIndex;
    progress_.stage = L"rendering";
    progress_.running = true;
    progress_.paused = paused_.load();
    updateTimingLocked(progress_.completedCount);
  }
  if (progressCb_) progressCb_(progress());

  // Advance MMD's playhead before capture — otherwise every PNG is the same pose.
  if (redraw_ && !redraw_->seekAnimationFrame(frameIndex)) {
    Diagnostics::instance().error(
        "seekAnimationFrame failed; MMD frame Edit not found or not updating");
    return false;
  }

  CameraState saved = camera_.snapshot();
  auto restoreSceneCamera = [&] {
    if (redraw_) redraw_->restoreSceneCamera();
  };

  ImageBuffer composed;
  // Stereo High: project left equirect on a worker while capturing right-eye
  // faces — bilinear 8K compose often rivals several MMD presents.
  if (s.cubemapFaces && s.stereoMode == StereoMode::Stereo) {
    // Never project the left eye on a worker while the right eye still drives
    // MMD Present. Overlap looked fine on free-RAM math, but WER shows
    // c0000005 inside our d3d9.dll mid right-eye faces every time (e.g. face
    // 7680 jobs dying after ~7 faces with no ERROR line). Parallel CPU
    // equirect + live D3D9/MME is not safe on this stack.
    std::array<ImageBuffer, 6> facesL{};
    if (!captureCubemapEye(true, facesL)) {
      restoreSceneCamera();
      camera_.restore(saved);
      camera_.endOverride();
      return false;
    }

    ImageBuffer left, right;
    RenderSettings mono = s;
    mono.stereoMode = StereoMode::Mono;
    {
      std::lock_guard<std::mutex> lock(mu_);
      progress_.stage = L"projecting";
    }
    if (progressCb_) progressCb_(progress());
    left = composeFromFaces(facesL, nullptr, mono);
    facesL = {};

    std::array<ImageBuffer, 6> facesR{};
    if (!captureCubemapEye(false, facesR)) {
      restoreSceneCamera();
      camera_.restore(saved);
      camera_.endOverride();
      return false;
    }

    restoreSceneCamera();
    camera_.endOverride();
    camera_.restore(saved);

    {
      std::lock_guard<std::mutex> lock(mu_);
      progress_.stage = L"projecting";
    }
    if (progressCb_) progressCb_(progress());

    right = composeFromFaces(facesR, nullptr, mono);
    facesR = {};

    if (left.empty() || right.empty()) {
      Diagnostics::instance().error("stereo equirect compose failed");
      return false;
    }
    composed = StereoEngine::composeSideBySide(left, right);
    left = {};
    right = {};
  } else {
    ImageBuffer left;
    if (!renderEye(true, left)) {
      restoreSceneCamera();
      camera_.restore(saved);
      camera_.endOverride();
      return false;
    }
    composed = std::move(left);
    restoreSceneCamera();
    camera_.endOverride();
    camera_.restore(saved);
  }

  if (!asyncWriter_) {
    const int q = resolvePngQueueMax(s);
    asyncWriter_ = std::make_unique<AsyncImageWriter>(s.pngSequenceDir(), s.writeThreads, q);
  }
  asyncWriter_->enqueue(std::move(composed), frameIndex);
  composed = {};

  // Free capture RTs between animation frames (faceSize²×4×2 can be hundreds of MB).
  if (redraw_) redraw_->trimCaptureResources();

  // Under memory pressure, drain PNG writers before capturing the next frame.
  const unsigned long long availMb = availablePhysMb();
  if (asyncWriter_ && availMb > 0 && availMb < 4096) {
    const int keep = availMb < 2048 ? 0 : 1;
    asyncWriter_->waitUntilPendingAtMost(keep);
    if (availMb < 1536) {
      Diagnostics::instance().warn(
          "Low free RAM (" + std::to_string((unsigned long long)availMb) +
          " MB); drained PNG queue before next frame");
    }
  }

  JobStateData js;
  js.settings = s;
  js.lastCompletedFrame = frameIndex;
  js.totalFrames = progress_.totalFrames;
  js.finished = false;
  JobState::save(JobState::defaultPath(s.outputDir), js);

  {
    std::lock_guard<std::mutex> lock(mu_);
    progress_.lastCompletedFrame = frameIndex;
    progress_.stage = L"frame_done";
    updateTimingLocked(progress_.completedCount + 1);
  }
  if (progressCb_) progressCb_(progress());
  return true;
}

bool RenderController::start(ProgressCallback cb) {
  if (running_.exchange(true)) return false;
  cancel_.store(false);
  paused_.store(false);
  firstFaceDebugDumped_ = false;
  progressCb_ = std::move(cb);
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (settings_.faceSize <= 0)
      settings_.faceSize = suggestedFaceSize(settings_.resolution, settings_.quality);
  }
  auto s = settings();
  Parallel::setThreadLimit(s.workerThreads);
  Parallel::setCancelFlag(&cancel_);
  const int pngQueue = resolvePngQueueMax(s);
  asyncWriter_ = std::make_unique<AsyncImageWriter>(s.pngSequenceDir(), s.writeThreads, pngQueue);
  const size_t frameBytes = estimateComposedRgbaBytes(s);
  Diagnostics::instance().info(
      std::string("mode=") + (s.cubemapFaces ? "cubemap(experimental)" : "live-camera(safe)") +
      " CPU threads=" + std::to_string(Parallel::resolveWorkers()) +
      " face=" + std::to_string(s.faceSize) +
      " pngWriters=" + std::to_string((std::max)(1, s.writeThreads)) +
      " pngQueue=" + std::to_string(pngQueue) +
      (s.writeQueueMax > 0 ? " (user cap)" : " (auto RAM cap)") +
      " frameRGBA≈" + std::to_string(frameBytes / (1024ull * 1024ull)) + "MB" +
      " freeRAM≈" + std::to_string(availablePhysMb()) + "MB");
  if (s.cubemapFaces && s.faceSize > 4096) {
    Diagnostics::instance().warn(
        "faceSize > 4096 is very heavy on MMD/MME and has caused long-job crashes; "
        "prefer 2048–4096 unless you have verified stability");
  }
  if (frameBytes >= 400ull * 1024ull * 1024ull) {
    Diagnostics::instance().warn(
        "Composed frame is very large; PNG queue forced to 1 and low-RAM drain is enabled");
  }

  int start = s.frameStart;
  int end = s.frameEnd;
  // Crash-safe resume: prefer first missing PNG (async writers may lag job.json).
  JobStateData existing;
  const bool haveJob = JobState::load(JobState::defaultPath(s.outputDir), existing) && !existing.finished;
  const int missingPng =
      JobState::firstMissingPngFrame(s.pngSequenceDir(), s.frameStart, end);
  int resumeAt = missingPng;
  if (haveJob && existing.lastCompletedFrame >= s.frameStart) {
    // If JSON is behind PNGs (rare), keep PNG; if JSON is ahead of flushed PNGs, use PNG.
    const int afterJson = existing.lastCompletedFrame + 1;
    resumeAt = (std::min)(missingPng, afterJson);
    if (missingPng < afterJson) {
      Diagnostics::instance().info(
          "Resume: job.json ahead of PNG flush; continuing from first missing PNG " +
          std::to_string(missingPng));
    }
  }
  if (resumeAt > start) {
    start = resumeAt;
    Diagnostics::instance().info("Resuming job from frame " + std::to_string(start));
  }
  if (end < start) end = start;

  runStart_ = std::chrono::steady_clock::now();
  pausedAccumSec_ = 0;
  pauseClockRunning_ = false;

  {
    std::lock_guard<std::mutex> lock(mu_);
    progress_ = {};
    progress_.running = true;
    progress_.totalFrames = end - s.frameStart + 1;
    progress_.stage = L"starting";
    progress_.etaSec = -1;
  }
  if (progressCb_) progressCb_(progress());

  bool ok = true;
  for (int f = start; f <= end; ++f) {
    waitIfPaused();
    if (cancel_.load()) {
      ok = false;
      break;
    }
    if (!processAnimationFrame(f)) {
      ok = false;
      std::lock_guard<std::mutex> lock(mu_);
      progress_.lastError = L"frame failed";
      break;
    }
  }

  if (asyncWriter_) {
    notifyStage(L"flushing_png", s.pngSequenceDir());
    if (!asyncWriter_->flush()) {
      ok = false;
      Diagnostics::instance().error("Async PNG write failed");
    }
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    lastOutputDir_ = s.outputDir;
    lastOutputMp4_.clear();
    lastEncodeOk_ = false;
    lastEncodeError_.clear();
    lastEncodeFallback_.clear();
  }

  if (ok && !cancel_.load() && s.encodeVideo) {
    std::wstring err;
    std::wstring fallback;
    std::wstring mp4 = s.outputDir + L"\\" + s.jobName + L".mp4";
    std::wstring tmp = s.outputDir + L"\\" + s.jobName + L"_raw.mp4";
    notifyStage(L"encoding", tmp);
    const double audioStartSec =
        (s.fps > 0.0 && s.frameStart > 0) ? (double)s.frameStart / s.fps : 0.0;
    int encW = 0, encH = 0;
    if (VideoEncoder::encodeSequence(s.pngSequenceDir(), L"frame_%06d.png", tmp, s.fps, s.ffmpegPath,
                                     s.encodeCodec, s.audioPath, audioStartSec, &err, &fallback,
                                     &encW, &encH)) {
      std::wstring finalOut = mp4;
      // YouTube VR recognition: inject Spherical UUID when we produced equirect (cubemap)
      // or the user left writeSphericalMeta on for a custom pipeline.
      const bool wantMeta = s.writeSphericalMeta && s.cubemapFaces;
      int metaW = encW > 0 ? encW : s.outputWidth;
      int metaH = encH > 0 ? encH : s.outputHeight;
      if (encW <= 0 && s.cubemapFaces && s.stereoMode == StereoMode::Stereo)
        metaW = s.outputWidth * 2;
      if (wantMeta) {
        notifyStage(L"vr_meta", finalOut);
        if (!VideoEncoder::injectVrMetadata(tmp, finalOut, s.vrMode, s.stereoMode, metaW, metaH,
                                            s.ffmpegPath, &err)) {
          Diagnostics::instance().warn("VR metadata inject failed; keeping raw mp4");
          finalOut = tmp;
        } else {
          std::error_code ec;
          std::filesystem::remove(tmp, ec);
        }
      } else {
        std::error_code ec;
        std::filesystem::remove(finalOut, ec);
        std::filesystem::rename(tmp, finalOut, ec);
        if (ec) finalOut = tmp;
      }
      {
        std::lock_guard<std::mutex> lock(mu_);
        lastOutputMp4_ = finalOut;
        lastEncodeOk_ = true;
        lastEncodeError_.clear();
        lastEncodeFallback_ = std::move(fallback);
      }
      Diagnostics::instance().infoW(L"MP4 written: " + finalOut);
    } else {
      Diagnostics::instance().warn("encode failed (see wide error in log)");
      Diagnostics::instance().infoW(L"encode error: " + err);
      {
        std::lock_guard<std::mutex> lock(mu_);
        lastOutputMp4_ = mp4;
        lastEncodeOk_ = false;
        lastEncodeError_ = err;
        lastEncodeFallback_ = std::move(fallback);
      }
    }
  } else if (ok && !cancel_.load()) {
    Diagnostics::instance().infoW(L"PNG sequence in: " + s.pngSequenceDir());
  }

  if (asyncWriter_) {
    asyncWriter_->shutdown();
    asyncWriter_.reset();
  }
  Parallel::setCancelFlag(nullptr);
  Parallel::shutdown();
  notifyStage(L"cleanup");
  // Full teardown on the job worker only — never from the progress-dialog UI thread.
  if (cleanupHandler_) cleanupHandler_();

  JobStateData js;
  js.settings = s;
  js.lastCompletedFrame = progress_.lastCompletedFrame;
  js.totalFrames = progress_.totalFrames;
  js.finished = ok && !cancel_.load();
  JobState::save(JobState::defaultPath(s.outputDir), js);

  {
    std::lock_guard<std::mutex> lock(mu_);
    progress_.running = false;
    progress_.paused = false;
    progress_.stage = ok ? L"done" : (cancel_.load() ? L"cancelled" : L"error");
    progress_.stageNote.clear();
    updateTimingLocked(progress_.completedCount);
  }
  if (progressCb_) progressCb_(progress());
  running_.store(false);
  paused_.store(false);
  return ok && !cancel_.load();
}

} // namespace mmdvr
