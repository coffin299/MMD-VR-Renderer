#include "PluginHost.hpp"
#include "DeviceHooks.hpp"
#include "MenuUI.hpp"
#include "RenderDialog.hpp"
#include "ProgressDialog.hpp"
#include "UiStrings.hpp"
#include "MmdWaveDetect.hpp"
#include "mmdvr/Diagnostics.hpp"
#include "mmdvr/ImageSequenceWriter.hpp"
#include "mmdvr/JobState.hpp"
#include "mmdvr/MathUtil.hpp"
#include <shellapi.h>
#include <thread>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace mmdvr {
namespace {

// MMD 9.32 draws the camera target ◎ into the center of the active camera
// view, even when the Camera/Light selector is moved away from Camera. It is
// a small, fixed magenta UI overlay (approximately #ED145B), not scene data.
// Repair only matching pixels in a tight center region so the 3D render stays
// untouched. This runs before cubemap projection, keeping the mark out of the
// final equirectangular image as well as debug face dumps.
void removeMmdCameraTargetMarker(ImageBuffer& image) {
  if (image.empty() || image.width < 32 || image.height < 32) return;

  const int cx = image.width / 2;
  const int cy = image.height / 2;
  const int radius = 18;
  const int left = (std::max)(0, cx - radius);
  const int top = (std::max)(0, cy - radius);
  const int right = (std::min)(image.width - 1, cx + radius);
  const int bottom = (std::min)(image.height - 1, cy + radius);
  const size_t pixelCount = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
  std::vector<uint8_t> mask(pixelCount, 0);
  int marked = 0;

  auto index = [&](int x, int y) {
    return static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x);
  };
  for (int y = top; y <= bottom; ++y) {
    for (int x = left; x <= right; ++x) {
      const size_t p = index(x, y);
      const size_t i = p * 4;
      const int r = image.rgba[i + 0];
      const int g = image.rgba[i + 1];
      const int b = image.rgba[i + 2];
      // Covers MMD 9.32's #ED145B marker plus its alpha-blended edge. The
      // readback preserves the overlay's alpha, so a white background yields
      // light pink values around (247, 170, 200), not just the solid color.
      const bool lightMarker =
          r >= 220 && g <= 190 && b >= 120 && b <= 225 &&
          r - g >= 45 && r - b >= 25;
      const bool darkMarker =
          r >= 130 && r <= 240 && g <= 140 && b <= 180 &&
          r - g >= 55 && r - b >= 25;
      if (lightMarker || darkMarker) {
        mask[p] = 1;
        ++marked;
      }
    }
  }
  // A handful of pink scene pixels near the middle are not enough evidence.
  if (marked < 12) return;

  // The marker has anti-aliased translucent pixels whose color is the
  // underlying scene rather than magenta. Include its immediate boundary so
  // a thin red ring cannot survive the color-only pass.
  const std::vector<uint8_t> detected = mask;
  for (int y = top; y <= bottom; ++y) {
    for (int x = left; x <= right; ++x) {
      if (!detected[index(x, y)]) continue;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int nx = x + dx;
          const int ny = y + dy;
          if (nx >= left && nx <= right && ny >= top && ny <= bottom)
            mask[index(nx, ny)] = 1;
        }
      }
    }
  }

  const std::vector<uint8_t> source = image.rgba;
  constexpr int kDirs[][2] = {
      {-1, 0}, {1, 0}, {0, -1}, {0, 1},
      {-1, -1}, {1, -1}, {-1, 1}, {1, 1},
  };
  for (int y = top; y <= bottom; ++y) {
    for (int x = left; x <= right; ++x) {
      const size_t p = index(x, y);
      if (!mask[p]) continue;

      int sum[4] = {0, 0, 0, 0};
      int samples = 0;
      for (const auto& dir : kDirs) {
        for (int step = 1; step <= 12; ++step) {
          const int sx = x + dir[0] * step;
          const int sy = y + dir[1] * step;
          if (sx < 0 || sx >= image.width || sy < 0 || sy >= image.height) break;
          const size_t q = index(sx, sy);
          if (mask[q]) continue;
          const size_t qi = q * 4;
          for (int c = 0; c < 4; ++c) sum[c] += source[qi + c];
          ++samples;
          break;
        }
      }
      if (samples <= 0) continue;
      const size_t i = p * 4;
      for (int c = 0; c < 4; ++c)
        image.rgba[i + c] = static_cast<uint8_t>(sum[c] / samples);
    }
  }
}

} // namespace

PluginHost& PluginHost::instance() {
  static PluginHost h;
  return h;
}

std::wstring PluginHost::moduleDir() const {
  HMODULE hm = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCWSTR)&PluginHost::instance, &hm);
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(hm, path, MAX_PATH);
  fs::path p(path);
  return p.parent_path().wstring();
}

void PluginHost::refreshBackbufferSize() {
  if (!device_) return;
  IDirect3DSurface9* back = nullptr;
  if (FAILED(device_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back)
    return;
  D3DSURFACE_DESC d{};
  back->GetDesc(&d);
  bbW_ = (int)d.Width;
  bbH_ = (int)d.Height;
  back->Release();
  onMainRt_ = true;
}

void PluginHost::onDeviceCreated(IDirect3DDevice9* device) {
  device_ = device;
  capture_.setDevice(device);
  controller_.setCapture(std::shared_ptr<IFrameCapture>(&capture_, [](IFrameCapture*){}));
  controller_.setRedraw(std::shared_ptr<IRedrawRequest>(this, [](IRedrawRequest*){}));
  // Cancel comes from the progress dialog UI thread — only wake waiters here.
  // Camera restore / pool release run on the job worker via cleanupHandler.
  controller_.setAbortHandler([this]() { interruptCaptureWait("abortHandler"); });
  controller_.setCleanupHandler([this]() { releaseRenderSession("jobCleanup"); });
  applyCaptureIsolateFromEnv();
  refreshBackbufferSize();
  Diagnostics::instance().setLogPath(moduleDir() + L"\\mmd_vr_renderer.log");
  Diagnostics::instance().info("Device created; MMD-VR-Renderer active");
  Diagnostics::instance().compatibilityNote("MME", "chained", "Expect d3d9_mme.dll beside exe");
}

void PluginHost::onDeviceReset() {
  Diagnostics::instance().warn("Device reset");
  releaseRenderSession("deviceReset");
  capture_.setDevice(device_);
  refreshBackbufferSize();
}

void PluginHost::onBeforeGetRenderTargetData(
    IDirect3DDevice9* device, IDirect3DSurface9* source) {
  stagePreOverlaySource(device, source, "GetRenderTargetData");
}

void PluginHost::onBeforeStretchRect(
    IDirect3DDevice9* device, IDirect3DSurface9* source, IDirect3DSurface9* destination) {
  // The plugin itself uses StretchRect for staging/scaling. Those copies occur
  // while a request is armed too, so they must never become candidates.
  if (capture_.ownsSurface(source) || capture_.ownsSurface(destination)) return;
  stagePreOverlaySource(device, source, "StretchRect");
}

void PluginHost::stagePreOverlaySource(
    IDirect3DDevice9* device, IDirect3DSurface9* source, const char* operation) {
  if (!device || !source || device != device_) return;
  {
    std::lock_guard<std::mutex> lock(redrawMu_);
    // The first Present after camera edits is deliberately discarded. Its
    // screen-capture copy is likewise stale and must not become a cubemap face.
    if (!wantCapture_ || skipCaptures_ > 0) return;
  }
  // Do not recursively capture our own staging/readback surfaces.
  if (capture_.ownsSurface(source)) return;

  D3DSURFACE_DESC desc{};
  if (FAILED(source->GetDesc(&desc)) || desc.Width == 0 || desc.Height == 0)
    return;

  // Prefer MMD output-size square (faceSize) when cubemap is configured. Matching
  // only the window backbuffer caused 7680 jobs to silently stage 1920×1080.
  const auto settings = controller_.settings();
  const int faceTarget =
      (settings.cubemapFaces && mmdOutputSizeConfigured_) ? settings.resolvedFaceSize() : 0;

  auto scoreCandidate = [&](int w, int h) -> int {
    if (faceTarget > 0) {
      const int tol = (std::max)(32, faceTarget / 16);
      if (std::abs(w - faceTarget) <= tol && std::abs(h - faceTarget) <= tol)
        return 100000 + w; // true cubemap face render target
    }
    if (bbW_ > 0 && bbH_ > 0) {
      const int toleranceW = (std::max)(32, bbW_ / 8);
      const int toleranceH = (std::max)(32, bbH_ / 8);
      if (std::abs(w - bbW_) <= toleranceW && std::abs(h - bbH_) <= toleranceH)
        return 1000 + w; // window / presentation fallback
    }
    return -1;
  };

  const int score = scoreCandidate(static_cast<int>(desc.Width), static_cast<int>(desc.Height));
  if (score < 0) return;

  if (capture_.hasStagedSurface()) {
    int sw = 0, sh = 0;
    if (capture_.stagedSize(sw, sh) && scoreCandidate(sw, sh) >= score)
      return; // keep equal-or-better staged copy
  }

  if (!capture_.stageSurface(source)) return;

  bool logCandidate = false;
  {
    std::lock_guard<std::mutex> lock(redrawMu_);
    if (!preOverlayCandidateLogged_) {
      preOverlayCandidateLogged_ = true;
      logCandidate = true;
    } else if (score >= 100000) {
      logCandidate = true; // log upgrade to true face-sized RT
    }
  }
  if (logCandidate) {
    char msg[192];
    std::snprintf(msg, sizeof(msg),
                  "Pre-overlay source staged from %s: %ux%u fmt=%u%s",
                  operation ? operation : "?",
                  static_cast<unsigned>(desc.Width), static_cast<unsigned>(desc.Height),
                  static_cast<unsigned>(desc.Format),
                  score >= 100000 ? " (face-sized)" : " (backbuffer-sized)");
    Diagnostics::instance().info(msg);
  }
}

void PluginHost::applyCaptureIsolateFromEnv() {
  const char* env = std::getenv("MMDVR_ISOLATE");
  int level = env ? std::atoi(env) : 0;
  if (level < 0) level = 0;
  if (level > 4) level = 4;
  capture_.setIsolate(static_cast<CaptureIsolate>(level));
  if (level != 0) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "MMDVR_ISOLATE=%d (capture binary-search mode)", level);
    Diagnostics::instance().warn(buf);
  }
}

void PluginHost::restoreObservedTransforms(IDirect3DDevice9* device) {
  if (!device) return;
  // Must run with override flag off so SetTransform hook does not re-replace.
  SetTransformOverrideEnabled(false);
  CameraState snap = controller_.camera().snapshot();
  if (!snap.valid) return;
  D3DMATRIX view{}, proj{};
  std::memcpy(view.m, snap.view.m, sizeof(float) * 16);
  std::memcpy(proj.m, snap.projection.m, sizeof(float) * 16);
  device->SetTransform(D3DTS_VIEW, &view);
  device->SetTransform(D3DTS_PROJECTION, &proj);
}

void PluginHost::interruptCaptureWait(const char* reason) {
  {
    std::lock_guard<std::mutex> lock(redrawMu_);
    wantCapture_ = false;
    skipCaptures_ = 0;
    preOverlayCandidateLogged_ = false;
    pendingCapture_ = {};
    captureReady_ = true; // unblock waiters (empty pending → treat as fail)
    redrawCv_.notify_all();
  }
  char buf[160];
  std::snprintf(buf, sizeof(buf), "interruptCaptureWait(%s)", reason ? reason : "?");
  Diagnostics::instance().info(buf);
}

void PluginHost::releaseRenderSession(const char* reason) {
  interruptCaptureWait(reason);
  const bool isDeviceReset = reason && std::strcmp(reason, "deviceReset") == 0;
  const bool hadTransformOverride =
      controller_.camera().overrideActive() || IsTransformOverrideEnabled();
  controller_.camera().endOverride();
  SetTransformOverrideEnabled(false);
  restoreSceneCamera();
  if (hadTransformOverride)
    restoreObservedTransforms(device_);
  capture_.releasePool();
  capture_.setSquareCrop(true);
  mainPassDrawn_ = false;
  didReplaceThisFrame_ = false;
  onMainRt_ = true;
  // Changing MMD's output size causes a D3D Reset. Do not restore it from
  // that nested reset callback; restore only when the actual job ends.
  if (!isDeviceReset && mmdOutputSizeConfigured_) {
    timeline_.restoreOutputSize();
    mmdOutputSizeConfigured_ = false;
  }
  if (!isDeviceReset) timeline_.restoreScreenCaptureMode();
  char buf[160];
  std::snprintf(buf, sizeof(buf),
                "releaseRenderSession(%s) overrideHook=%d",
                reason ? reason : "?",
                IsTransformOverrideEnabled() ? 1 : 0);
  Diagnostics::instance().info(buf);
}

bool PluginHost::isPerspectiveProjection(const D3DMATRIX* m) {
  if (!m) return false;
  const float m34 = m->m[2][3];
  const float m44 = m->m[3][3];
  return (std::fabs(m44) < 1e-5f) && (std::fabs(m34) > 1e-5f);
}

void PluginHost::observeCameraFromDevice(IDirect3DDevice9* device) {
  if (!device || controller_.camera().overrideActive() || controller_.camera().snapshot().valid) return;
  D3DMATRIX view{}, proj{};
  if (SUCCEEDED(device->GetTransform(D3DTS_VIEW, &view)))
    controller_.camera().onSetTransform(CameraController::kD3DTS_VIEW, &view.m[0][0]);
  if (SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &proj)))
    controller_.camera().onSetTransform(CameraController::kD3DTS_PROJECTION, &proj.m[0][0]);
}

void PluginHost::onSetRenderTarget(IDirect3DSurface9* rt) {
  if (!rt || bbW_ <= 0 || bbH_ <= 0) {
    onMainRt_ = true;
    return;
  }
  D3DSURFACE_DESC d{};
  if (FAILED(rt->GetDesc(&d))) {
    onMainRt_ = true;
    return;
  }
  const int tolW = bbW_ / 10 + 8;
  const int tolH = bbH_ / 10 + 8;
  onMainRt_ = (std::abs((int)d.Width - bbW_) <= tolW) && (std::abs((int)d.Height - bbH_) <= tolH);
}

void PluginHost::onBeginScene(IDirect3DDevice9* device) {
  mainPassDrawn_ = false;
  didReplaceThisFrame_ = false;
  onMainRt_ = true;
  havePendingObservedView_ = false;
  // Seed the main scene with the face camera. The hook is active here, so the
  // calls below still take the normal MinHook dispatch route.
  if (device && controller_.camera().overrideActive() && IsTransformOverrideEnabled()) {
    auto ov = controller_.camera().overrideState();
    D3DMATRIX view{}, proj{};
    memcpy(view.m, ov.view.m, sizeof(float) * 16);
    memcpy(proj.m, ov.projection.m, sizeof(float) * 16);
    device->SetTransform(D3DTS_VIEW, &view);
    device->SetTransform(D3DTS_PROJECTION, &proj);
    didReplaceThisFrame_ = true;
  }
}

void PluginHost::onMainPassDraw() {
  // Kept for hook compatibility; full-frame override no longer locks after first draw.
  if (onMainRt_ && didReplaceThisFrame_) mainPassDrawn_ = true;
}

void PluginHost::onObservedTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) {
  // Store MMD's real camera before a render job begins. Reading transforms at
  // Present is too late: ray-mmd may already have installed post-process or
  // shadow matrices there, which put the cubemap origin away from MMD's camera.
  if (!matrix || !onMainRt_) return;
  const int st = (int)state;
  if (st == CameraController::kD3DTS_VIEW) {
    pendingObservedView_ = *matrix;
    havePendingObservedView_ = true;
    return;
  }
  if (st != CameraController::kD3DTS_PROJECTION || !isPerspectiveProjection(matrix))
    return;

  // MMD emits the camera pair as VIEW then perspective PROJECTION. Commit the
  // pair together, so an orthographic shadow projection cannot overwrite it.
  if (havePendingObservedView_) {
    controller_.camera().onSetTransform(
        CameraController::kD3DTS_VIEW, &pendingObservedView_.m[0][0]);
  }
  controller_.camera().onSetTransform(
      CameraController::kD3DTS_PROJECTION, &matrix->m[0][0]);
}

void PluginHost::onSetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix, bool& replace, D3DMATRIX& outMatrix) {
  replace = false;
  if (!matrix || !controller_.camera().overrideActive()) return;

  int st = (int)state;
  if (st != CameraController::kD3DTS_VIEW && st != CameraController::kD3DTS_PROJECTION)
    return;
  // Do not alter ray-mmd's shadow or auxiliary render targets. In particular,
  // never issue a direct original-vtable SetTransform from inside this hook:
  // MinHook's trampoline may be active and causes a stack-protection crash.
  if (!onMainRt_)
    return;
  if (st == CameraController::kD3DTS_PROJECTION && !isPerspectiveProjection(matrix))
    return;

  auto ov = controller_.camera().overrideState();
  replace = true;
  didReplaceThisFrame_ = true;
  if (st == CameraController::kD3DTS_VIEW)
    memcpy(outMatrix.m, ov.view.m, sizeof(float) * 16);
  else
    memcpy(outMatrix.m, ov.projection.m, sizeof(float) * 16);
}

void PluginHost::onGetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* matrix) {
  if (!matrix || !controller_.camera().overrideActive()) return;
  int st = (int)state;
  if (st != CameraController::kD3DTS_VIEW && st != CameraController::kD3DTS_PROJECTION)
    return;
  if (!onMainRt_)
    return;
  if (st == CameraController::kD3DTS_PROJECTION && !isPerspectiveProjection(matrix))
    return;
  auto ov = controller_.camera().overrideState();
  if (st == CameraController::kD3DTS_VIEW)
    memcpy(matrix->m, ov.view.m, sizeof(float) * 16);
  else
    memcpy(matrix->m, ov.projection.m, sizeof(float) * 16);
}

void PluginHost::onBeforePresent(IDirect3DDevice9* device) {
  if (device) device_ = device;
  onMainRt_ = true;

  bool doCapture = false;
  {
    std::unique_lock<std::mutex> lock(redrawMu_);
    if (wantCapture_ && skipCaptures_ > 0) {
      --skipCaptures_;
      // Keep override; ask for another paint so the *next* Present is a full
      // face frame we can capture (MMD often only Presents once per nudge).
      if (mmdHwnd_ && IsWindow(mmdHwnd_)) {
        InvalidateRect(mmdHwnd_, nullptr, FALSE);
        PostMessageW(mmdHwnd_, WM_PAINT, 0, 0);
      }
      return;
    }
    doCapture = wantCapture_;
  }

  if (!doCapture) {
    if (wantEvict_.exchange(false) && device)
      device->EvictManagedResources();
    if (controller_.camera().overrideActive() || IsTransformOverrideEnabled()) {
      controller_.camera().endOverride();
      SetTransformOverrideEnabled(false);
      restoreObservedTransforms(device);
      Diagnostics::instance().info(
          "onBeforePresent: cleared stale override (no capture pending)");
    }
    observeCameraFromDevice(device);
    return;
  }

  // Face capture must wait until this Present's scene actually consumed our
  // replacement matrices. Otherwise we grab a flip of the live MMD camera
  // (or a half-updated deferred frame) and stitch shreds into the equirect.
  if (controller_.camera().overrideActive() && !didReplaceThisFrame_) {
    if (mmdHwnd_ && IsWindow(mmdHwnd_)) {
      InvalidateRect(mmdHwnd_, nullptr, FALSE);
      PostMessageW(mmdHwnd_, WM_PAINT, 0, 0);
    }
    return;
  }

  // Capture with whatever matrices the scene already used. Then immediately
  // drop override and put the last observed MMD camera back on the device so
  // face matrices never remain as live device state after Present returns.
  // MMD renders into a viewport inside its D3D backbuffer. The remainder may
  // contain black editor/UI space; cropping the whole backbuffer then making it
  // square turns that editor space into black cubemap faces and appears as a
  // ray-mmd panorama failure.
  D3DVIEWPORT9 viewport{};
  const bool haveViewport = SUCCEEDED(device->GetViewport(&viewport)) &&
                            viewport.Width > 0 && viewport.Height > 0;
  if (haveViewport) {
    capture_.setViewportCrop(
        static_cast<int>(viewport.X), static_cast<int>(viewport.Y),
        static_cast<int>(viewport.Width), static_cast<int>(viewport.Height));
    const int x = static_cast<int>(viewport.X);
    const int y = static_cast<int>(viewport.Y);
    const int w = static_cast<int>(viewport.Width);
    const int h = static_cast<int>(viewport.Height);
    if (x != loggedViewportX_ || y != loggedViewportY_ ||
        w != loggedViewportW_ || h != loggedViewportH_) {
      char msg[160];
      std::snprintf(msg, sizeof(msg),
                    "Capture crop: viewport=%d,%d %dx%d backbuffer=%dx%d",
                    x, y, w, h, bbW_, bbH_);
      Diagnostics::instance().info(msg);
      loggedViewportX_ = x;
      loggedViewportY_ = y;
      loggedViewportW_ = w;
      loggedViewportH_ = h;
    }
  } else {
    capture_.clearCrop();
  }
  ImageBuffer img;
  bool usedPreOverlay = capture_.hasStagedSurface();
  bool ok = usedPreOverlay ? capture_.captureStaged(img) : capture_.captureViewport(img);
  if (!ok && usedPreOverlay) {
    // A driver may reject the staged copy's format. Preserve rendering rather
    // than failing the face: read the Present backbuffer as the old path did.
    capture_.clearStagedSurface();
    usedPreOverlay = false;
    ok = capture_.captureViewport(img);
  }
  capture_.clearCrop();
  // Free the full-size staged RT (often 100s of MB). Keep the readback pool
  // so the next face does not re-CreateRenderTarget every Present — that
  // thrash dominated High-quality face time at 4K/8K.
  capture_.clearStagedSurface();
  if (wantEvict_.exchange(false) && device)
    device->EvictManagedResources();
  // The fallback contains MMD's editor overlay. The native pre-overlay path
  // intentionally leaves scene pixels completely untouched.
  if (ok && !usedPreOverlay) removeMmdCameraTargetMarker(img);
  {
    std::lock_guard<std::mutex> lock(redrawMu_);
    if (ok) pendingCapture_ = std::move(img);
    captureReady_ = ok;
    wantCapture_ = false;
    redrawCv_.notify_all();
  }
  // Native camera capture leaves MMD/MME/ray-mmd state untouched. Restoring
  // fixed-function transforms here would reintroduce a camera mismatch after
  // a correctly evaluated ray-mmd frame.
  if (controller_.camera().overrideActive() || IsTransformOverrideEnabled()) {
    controller_.camera().endOverride();
    SetTransformOverrideEnabled(false);
    restoreObservedTransforms(device);
  } else {
    observeCameraFromDevice(device);
  }
}

bool PluginHost::seekAnimationFrame(int frameIndex) {
  if (!mmdHwnd_ || !IsWindow(mmdHwnd_)) {
    Diagnostics::instance().error("seekAnimationFrame: no MMD hwnd");
    return false;
  }
  const auto settings = controller_.settings();
  if (settings.cubemapFaces && !mmdOutputSizeConfigured_) {
    // The final 8K output is equirectangular. Each source cubemap face needs
    // its own true MMD render target, normally 3072×3072 at the 8K preset;
    // upscaling an on-screen 1080p viewport only produces soft 8K PNGs.
    const int faceSize = settings.resolvedFaceSize();
    if (!timeline_.setOutputSize(faceSize, faceSize)) {
      Diagnostics::instance().error(
          "seekAnimationFrame: failed to set MMD output size for cubemap face capture");
      return false;
    }
    mmdOutputSizeConfigured_ = true;
    refreshBackbufferSize();
  }
  if (!timeline_.frameEdit() || !IsWindow(timeline_.frameEdit())) {
    if (!timeline_.bind(mmdHwnd_)) return false;
  }
  if (!timeline_.enableScreenCaptureMode1()) {
    Diagnostics::instance().warn(
        "seekAnimationFrame: MMD screen-capture mode unavailable; using Present fallback");
  }
  if (!timeline_.seekFrame(frameIndex)) return false;
  if (controller_.settings().cubemapFaces) {
    if (!timeline_.readCamera(nativeCameraBase_)) {
      Diagnostics::instance().error(
          "seekAnimationFrame: native MMD camera controls unavailable; cubemap cancelled safely");
      return false;
    }
    nativeCameraApplied_ = false;
  }

  // Nudge a redraw so Present delivers the evaluated pose.
  InvalidateRect(mmdHwnd_, nullptr, FALSE);
  PostMessageW(mmdHwnd_, WM_PAINT, 0, 0);
  return true;
}

void PluginHost::restoreSceneCamera() {
  if (!nativeCameraApplied_) {
    timeline_.restoreEditTarget();
    return;
  }
  if (!nativeCameraBase_.valid || !timeline_.applyCamera(nativeCameraBase_)) {
    Diagnostics::instance().warn("restoreSceneCamera: failed to restore MMD camera controls");
  } else {
    Diagnostics::instance().info("restoreSceneCamera: MMD camera restored");
  }
  timeline_.restoreEditTarget();
  nativeCameraApplied_ = false;
}

void PluginHost::trimCaptureResources() {
  // Drop pool between animation frames (not every cubemap face) so High/8K
  // reuses the same readback RTs within a frame, then returns VRAM to MME.
  capture_.releasePool();
  wantEvict_.store(true);
}

bool PluginHost::captureView(ImageBuffer& out) {
  // No SetTransform override — live MMD camera only.
  auto s = controller_.settings();
  // Cap readback to backbuffer scale; do not allocate 8K intermediate RTs.
  const int longSide = (std::min)((std::max)(s.outputWidth, s.outputHeight), 4096);
  capture_.setSquareCrop(false);
  capture_.setDefaultMaxSide(longSide > 0 ? longSide : 0);
  refreshBackbufferSize();
  controller_.camera().endOverride();
  SetTransformOverrideEnabled(false);
  capture_.clearStagedSurface();
  {
    std::lock_guard<std::mutex> lock(redrawMu_);
    captureReady_ = false;
    wantCapture_ = true;
    skipCaptures_ = 0;
    preOverlayCandidateLogged_ = false;
  }
  // One paint nudge only — flooding WM_PAINT re-enters Present under MME.
  if (mmdHwnd_) {
    InvalidateRect(mmdHwnd_, nullptr, FALSE);
    PostMessageW(mmdHwnd_, WM_PAINT, 0, 0);
  }

  std::unique_lock<std::mutex> lock(redrawMu_);
  bool ok = false;
  for (int i = 0; i < 100; ++i) {
    if (controller_.isCancelled()) break;
    ok = redrawCv_.wait_for(lock, std::chrono::milliseconds(100), [&]{ return captureReady_; });
    if (ok) break;
  }
  wantCapture_ = false;
  skipCaptures_ = 0;
  lock.unlock();
  capture_.setSquareCrop(true);
  if (controller_.isCancelled()) {
    Diagnostics::instance().info("captureView: cancelled");
    return false;
  }
  if (ok) {
    out = std::move(pendingCapture_);
    return !out.empty();
  }
  Diagnostics::instance().error(
      "captureView timed out waiting for Present "
      "(MMD may be paused / not redrawing).");
  return false;
}

bool PluginHost::renderView(const CameraState& cam, ImageBuffer& out) {
  // Let MMD change its real camera controls for each face. This is essential
  // for ray-mmd: it re-evaluates HLSL camera constants, G-buffer, lighting and
  // post-processes through its normal path instead of seeing a late D3D9
  // fixed-function matrix override.
  if (!nativeCameraBase_.valid) {
    Diagnostics::instance().error("renderView: native MMD camera base is unavailable");
    return false;
  }

  const int faceMax = controller_.settings().resolvedFaceSize();
  capture_.setSquareCrop(true);
  capture_.setDefaultMaxSide(faceMax);
  refreshBackbufferSize();
  onMainRt_ = true;
  mainPassDrawn_ = false;
  didReplaceThisFrame_ = false;
  controller_.camera().endOverride();
  SetTransformOverrideEnabled(false);
  capture_.clearStagedSurface();
  {
    std::lock_guard<std::mutex> lock(redrawMu_);
    captureReady_ = false;
    wantCapture_ = true;
    skipCaptures_ = 1;
    pendingCam_ = cam;
    preOverlayCandidateLogged_ = false;
  }

  if (controller_.isCancelled()) {
    std::lock_guard<std::mutex> lock(redrawMu_);
    wantCapture_ = false;
    skipCaptures_ = 0;
    return false;
  }

  if (!timeline_.applyCameraFace(nativeCameraBase_, cam)) {
    std::lock_guard<std::mutex> lock(redrawMu_);
    wantCapture_ = false;
    skipCaptures_ = 0;
    Diagnostics::instance().error("renderView: failed to apply native MMD camera face");
    return false;
  }
  if (controller_.isCancelled()) {
    std::lock_guard<std::mutex> lock(redrawMu_);
    wantCapture_ = false;
    skipCaptures_ = 0;
    return false;
  }
  // Keep Camera selected so native face values apply reliably; the red ◎ is
  // removed from the captured pixels after Present.
  timeline_.suppressViewportOverlays();
  nativeCameraApplied_ = true;
  if (controller_.isCancelled()) {
    std::lock_guard<std::mutex> lock(redrawMu_);
    wantCapture_ = false;
    skipCaptures_ = 0;
    return false;
  }
  char armed[128];
  std::snprintf(armed, sizeof(armed), "renderView: native face=%d halfIpd=%.3f armed (skip=1)",
                static_cast<int>(cam.cubeFace), cam.nativeEyeOffsetX);
  Diagnostics::instance().info(armed);

  std::unique_lock<std::mutex> lock(redrawMu_);
  bool ok = false;
  for (int i = 0; i < 150; ++i) {
    if (controller_.isCancelled()) break;
    // Keep nudging MMD so a fully evaluated post-camera-change frame arrives.
    if (mmdHwnd_ && (i % 2) == 0) {
      lock.unlock();
      InvalidateRect(mmdHwnd_, nullptr, FALSE);
      PostMessageW(mmdHwnd_, WM_PAINT, 0, 0);
      lock.lock();
    }
    ok = redrawCv_.wait_for(lock, std::chrono::milliseconds(100), [&]{ return captureReady_; });
    if (ok) break;
  }

  wantCapture_ = false;
  skipCaptures_ = 0;
  if (controller_.isCancelled()) {
    Diagnostics::instance().info("renderView: cancelled");
    return false;
  }
  if (ok) {
    out = std::move(pendingCapture_);
    Diagnostics::instance().info(out.empty() ? "renderView: empty capture" : "renderView: face ok");
    return !out.empty();
  }

  Diagnostics::instance().error(
      "renderView timed out waiting for Present with native MMD camera "
      "(MMD may be paused / not redrawing). Face skipped.");
  return false;
}

void PluginHost::requestSingleCapture() {
  // Must capture on the Present thread — D3D9 device is not free-threaded.
  // Calling GetRenderTargetData from the UI thread races MME/ray-mmd and corrupts drawing.
  if (jobThreadRunning_.exchange(true)) {
    MessageBoxW(mmdHwnd_, ui::t().busy, ui::t().appName, MB_OK);
    return;
  }
  HWND hwnd = mmdHwnd_;
  std::thread([this, hwnd]() {
    capture_.setSquareCrop(false);
    capture_.setDefaultMaxSide(0);
    if ((!timeline_.frameEdit() || !IsWindow(timeline_.frameEdit())) && hwnd)
      timeline_.bind(hwnd);
    timeline_.enableScreenCaptureMode1();
    ImageBuffer img;
    capture_.clearStagedSurface();
    {
      std::lock_guard<std::mutex> lock(redrawMu_);
      captureReady_ = false;
      wantCapture_ = true;
      skipCaptures_ = 0;
      preOverlayCandidateLogged_ = false;
    }
    if (hwnd) {
      InvalidateRect(hwnd, nullptr, FALSE);
      PostMessageW(hwnd, WM_PAINT, 0, 0);
    }
    bool ok = false;
    {
      std::unique_lock<std::mutex> lock(redrawMu_);
      for (int i = 0; i < 50; ++i) {
        if (hwnd && (i % 2) == 0) {
          InvalidateRect(hwnd, nullptr, FALSE);
          PostMessageW(hwnd, WM_PAINT, 0, 0);
        }
        ok = redrawCv_.wait_for(lock, std::chrono::milliseconds(100), [&]{ return captureReady_; });
        if (ok) break;
      }
      wantCapture_ = false;
      if (ok) img = std::move(pendingCapture_);
    }
    capture_.setSquareCrop(true);
    timeline_.restoreScreenCaptureMode();
    if (ok && !img.empty()) {
      auto dir = moduleDir() + L"\\mmdvr_captures";
      ImageSequenceWriter w(dir);
      w.writeFrame(img, 0, L"_preview");
      Diagnostics::instance().info("Saved preview capture");
      MessageBoxW(hwnd, ui::t().captureSaved, ui::t().appName, MB_OK);
    } else {
      MessageBoxW(hwnd, ui::t().captureFailed, ui::t().appName, MB_ICONERROR);
    }
    jobThreadRunning_.store(false);
  }).detach();
}

void PluginHost::runCameraProbe() {
  // Run off the UI thread so Present can proceed.
  if (jobThreadRunning_.exchange(true)) {
    MessageBoxW(mmdHwnd_, ui::t().busy, ui::t().probeTitle, MB_OK);
    return;
  }
  HWND hwnd = mmdHwnd_;
  std::thread([this, hwnd]() {
    CameraState base = controller_.camera().snapshot();
    if (!base.valid) {
      base.eye = {0, 14, -30};
      base.target = {0, 14, 0};
      base.up = {0, 1, 0};
      base.view = lookAtRH(base.eye, base.target, base.up);
      base.projection = perspectiveRH(40.f, 1.333f, 0.1f, 10000.f);
      base.valid = true;
    }

    auto dir = moduleDir() + L"\\mmdvr_captures";
    ImageSequenceWriter w(dir);
    ImageBuffer a, b;

    // Baseline also via Present sync (never call D3D from this worker directly).
    capture_.clearStagedSurface();
    {
      std::lock_guard<std::mutex> lock(redrawMu_);
      captureReady_ = false;
      wantCapture_ = true;
      preOverlayCandidateLogged_ = false;
    }
    bool baseOk = false;
    {
      std::unique_lock<std::mutex> lock(redrawMu_);
      for (int i = 0; i < 50; ++i) {
        if (hwnd && (i % 2) == 0) {
          InvalidateRect(hwnd, nullptr, FALSE);
          PostMessageW(hwnd, WM_PAINT, 0, 0);
        }
        baseOk = redrawCv_.wait_for(lock, std::chrono::milliseconds(100), [&]{ return captureReady_; });
        if (baseOk) break;
      }
      wantCapture_ = false;
      if (baseOk) a = std::move(pendingCapture_);
    }
    if (!baseOk || a.empty()) {
      jobThreadRunning_.store(false);
      MessageBoxW(hwnd, ui::t().probeBaselineFailed, ui::t().probeTitle, MB_ICONERROR);
      return;
    }
    w.writeFrame(a, 0, L"_probe_base");

    CameraState yawed = base;
    float ang = deg2rad(35.f);
    Vec3 f = normalize(sub(base.target, base.eye));
    float c = std::cos(ang), s = std::sin(ang);
    Vec3 f2{f.x * c + f.z * s, f.y, -f.x * s + f.z * c};
    yawed.target = add(base.eye, f2);
    yawed.view = lookAtRH(yawed.eye, yawed.target, yawed.up);
    yawed.projection = (base.projection.m[0] != 0.f || base.projection.m[5] != 0.f)
                           ? base.projection
                           : perspectiveRH(40.f, 1.333f, 0.1f, 10000.f);
    yawed.valid = true;

    bool ok = renderView(yawed, b);
    if (ok) w.writeFrame(b, 0, L"_probe_yaw35");

    controller_.camera().endOverride();
    controller_.camera().restore(base);

    double diff = 0;
    if (ok && a.width == b.width && a.height == b.height && !a.empty()) {
      size_t n = a.rgba.size();
      for (size_t i = 0; i < n; ++i) diff += std::abs((int)a.rgba[i] - (int)b.rgba[i]);
      diff /= (double)n;
    }
    wchar_t msg[400];
    if (ui::isJa()) {
      std::swprintf(msg, 400,
                    L"カメラ上書きプローブ完了\n"
                    L"平均絶対差分=%.3f\n"
                    L"mmdvr_captures\\*_probe_*.png を確認してください。\n%s",
                    diff,
                    diff > 1.0 ? L"視点が変化した可能性が高いです（上書き有効）。"
                               : L"変化が小さいです。MMD/MMEがカメラを再設定している可能性があります。");
    } else {
      std::swprintf(msg, 400,
                    L"Camera probe done.\n"
                    L"Mean abs channel diff=%.3f\n"
                    L"Check mmdvr_captures\\*_probe_*.png\n%s",
                    diff,
                    diff > 1.0 ? L"Viewpoint likely changed (override OK)."
                               : L"Little change — camera may be rebuilt by MMD/MME.");
    }
    Diagnostics::instance().infoW(msg);
    jobThreadRunning_.store(false);
    MessageBoxW(hwnd, msg, ui::t().probeTitle, MB_OK);
  }).detach();
}

void PluginHost::startRenderJob(const RenderSettings& settings) {
  if (jobThreadRunning_.exchange(true)) {
    MessageBoxW(mmdHwnd_, ui::t().renderBusy, ui::t().appName, MB_OK);
    return;
  }
  RenderSettings s = settings;
  HWND hwnd = mmdHwnd_;

  // Crash / cancel resume: same project folder + unfinished job.json → ask.
  {
    JobStateData existing;
    const std::wstring jobPath = JobState::defaultPath(s.outputDir);
    if (JobState::load(jobPath, existing) && !existing.finished &&
        existing.lastCompletedFrame >= s.frameStart) {
      const int missingPng =
          JobState::firstMissingPngFrame(s.pngSequenceDir(), s.frameStart, s.frameEnd);
      const int afterJson = existing.lastCompletedFrame + 1;
      const int resumeAt = (std::min)(missingPng, afterJson);
      const int planned =
          existing.totalFrames > 0
              ? existing.totalFrames
              : (s.frameEnd >= s.frameStart ? (s.frameEnd - s.frameStart + 1) : 0);
      if (resumeAt > s.frameStart) {
        wchar_t prompt[900];
        std::swprintf(prompt, 900, ui::t().resumeJobPromptFmt, resumeAt,
                      existing.lastCompletedFrame, planned, s.outputDir.c_str());
        const int choice =
            MessageBoxW(hwnd, prompt, ui::t().appName, MB_YESNOCANCEL | MB_ICONQUESTION);
        if (choice == IDCANCEL) {
          jobThreadRunning_.store(false);
          return;
        }
        if (choice == IDYES) {
          // Keep user's new end frame / encode toggles, restore capture-critical settings.
          const int frameEndUi = s.frameEnd;
          const bool encodeUi = s.encodeVideo;
          const EncodeCodec codecUi = s.encodeCodec;
          const std::wstring audioUi = s.audioPath;
          const std::wstring ffmpegUi = s.ffmpegPath;
          s = existing.settings;
          s.outputDir = settings.outputDir;
          s.jobName = settings.jobName;
          if (frameEndUi >= 0) s.frameEnd = frameEndUi;
          s.encodeVideo = encodeUi;
          s.encodeCodec = codecUi;
          s.audioPath = audioUi;
          s.ffmpegPath = ffmpegUi;
          Diagnostics::instance().infoW(
              L"User chose resume; next frame ≈ " + std::to_wstring(resumeAt));
        } else {
          // Restart: drop checkpoint so RenderController starts at frameStart.
          std::error_code ec;
          fs::remove(jobPath, ec);
          Diagnostics::instance().info("User chose restart; cleared job.json checkpoint");
        }
      }
    }
  }

  controller_.setSettings(s);
  // Progress dialog owns a worker thread + message loop (cancel / pause / ETA).
  bool ok = RunProgressDialog(hwnd, controller_);
  jobThreadRunning_.store(false);

  std::wstring dir = controller_.lastOutputDir();
  std::wstring mp4 = controller_.lastOutputMp4();
  const std::wstring fallback = controller_.lastEncodeFallback();
  wchar_t fallbackLine[256]{};
  if (!fallback.empty())
    std::swprintf(fallbackLine, 256, ui::t().encodeFallbackFmt, fallback.c_str());
  wchar_t msg[1400];
  if (ok) {
    if (controller_.lastEncodeOk() && !mp4.empty()) {
      if (ui::isJa()) {
        std::swprintf(msg, 1400,
                      L"レンダリング完了\n\nMP4:\n%s\n\nPNG連番も同じフォルダです。%s%s",
                      mp4.c_str(),
                      fallback.empty() ? L"" : L"\n\n",
                      fallback.empty() ? L"" : fallbackLine);
      } else {
        std::swprintf(msg, 1400,
                      L"Done.\n\nMP4:\n%s\n\nPNG sequence is in the same folder.%s%s",
                      mp4.c_str(),
                      fallback.empty() ? L"" : L"\n\n",
                      fallback.empty() ? L"" : fallbackLine);
      }
    } else if (!dir.empty()) {
      auto encErr = controller_.lastEncodeError();
      if (!encErr.empty()) {
        if (ui::isJa()) {
          std::swprintf(msg, 1400,
                        L"PNGは出力済みですが MP4 化に失敗しました。\n\nフォルダ:\n%s%s%s\n\nFFmpeg:\n%.500s",
                        dir.c_str(),
                        fallback.empty() ? L"" : L"\n\n",
                        fallback.empty() ? L"" : fallbackLine,
                        encErr.c_str());
        } else {
          std::swprintf(msg, 1400,
                        L"PNG written, but MP4 encode failed.\n\nFolder:\n%s%s%s\n\nFFmpeg:\n%.500s",
                        dir.c_str(),
                        fallback.empty() ? L"" : L"\n\n",
                        fallback.empty() ? L"" : fallbackLine,
                        encErr.c_str());
        }
      } else {
        if (ui::isJa()) {
          std::swprintf(msg, 1400, L"レンダリング完了（PNGのみ）。\n\nフォルダ:\n%s", dir.c_str());
        } else {
          std::swprintf(msg, 1400, L"Done (PNG only).\n\nFolder:\n%s", dir.c_str());
        }
      }
    } else {
      std::swprintf(msg, 1400, L"%s", ui::t().renderDone);
    }
    MessageBoxW(hwnd, msg, ui::t().appName, MB_OK);
    if (!dir.empty() &&
        MessageBoxW(hwnd, ui::t().openOutputFolder, ui::t().appName, MB_YESNO | MB_ICONQUESTION) == IDYES) {
      ShellExecuteW(hwnd, L"explore", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
  } else {
    MessageBoxW(hwnd, ui::t().renderFailed, ui::t().appName, MB_ICONWARNING);
  }
}

void PluginHost::openUi(HWND mmdHwnd) {
  mmdHwnd_ = mmdHwnd;
  timeline_.bind(mmdHwnd);
  RenderSettings s = controller_.settings();
  if (s.outputDir.empty())
    s.outputDir = moduleDir() + L"\\mmdvr_output";
  if (s.audioPath.empty()) {
    auto wav = detectMmdWavePath(mmdHwnd);
    if (!wav.empty()) s.audioPath = std::move(wav);
  }
  if (ShowRenderDialog(mmdHwnd, s)) {
    if (s.jobName == L"__capture_only__") {
      requestSingleCapture();
      return;
    }
    startRenderJob(s);
  }
}

} // namespace mmdvr
