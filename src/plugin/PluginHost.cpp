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
  const int rw = right - left + 1;
  const int rh = bottom - top + 1;
  // ROI-only mask (~37²). Never allocate W×H temporaries on 8K Present path.
  std::vector<uint8_t> mask(static_cast<size_t>(rw) * static_cast<size_t>(rh), 0);
  int marked = 0;

  auto roi = [&](int x, int y) {
    return static_cast<size_t>(y - top) * static_cast<size_t>(rw) +
           static_cast<size_t>(x - left);
  };
  auto pix = [&](int x, int y) {
    return (static_cast<size_t>(y) * static_cast<size_t>(image.width) +
            static_cast<size_t>(x)) *
           4u;
  };

  for (int y = top; y <= bottom; ++y) {
    for (int x = left; x <= right; ++x) {
      const size_t i = pix(x, y);
      const int r = image.rgba[i + 0];
      const int g = image.rgba[i + 1];
      const int b = image.rgba[i + 2];
      // Covers MMD 9.32's #ED145B marker plus its alpha-blended edge.
      const bool lightMarker =
          r >= 220 && g <= 190 && b >= 120 && b <= 225 &&
          r - g >= 45 && r - b >= 25;
      const bool darkMarker =
          r >= 130 && r <= 240 && g <= 140 && b <= 180 &&
          r - g >= 55 && r - b >= 25;
      if (lightMarker || darkMarker) {
        mask[roi(x, y)] = 1;
        ++marked;
      }
    }
  }
  if (marked < 12) return;

  const std::vector<uint8_t> detected = mask;
  for (int y = top; y <= bottom; ++y) {
    for (int x = left; x <= right; ++x) {
      if (!detected[roi(x, y)]) continue;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int nx = x + dx;
          const int ny = y + dy;
          if (nx >= left && nx <= right && ny >= top && ny <= bottom)
            mask[roi(nx, ny)] = 1;
        }
      }
    }
  }

  // In-place fill: only write masked pixels, only sample unmasked → safe.
  constexpr int kDirs[][2] = {
      {-1, 0}, {1, 0}, {0, -1}, {0, 1},
      {-1, -1}, {1, -1}, {-1, 1}, {1, 1},
  };
  for (int y = top; y <= bottom; ++y) {
    for (int x = left; x <= right; ++x) {
      if (!mask[roi(x, y)]) continue;

      int sum[4] = {0, 0, 0, 0};
      int samples = 0;
      for (const auto& dir : kDirs) {
        for (int step = 1; step <= 12; ++step) {
          const int sx = x + dir[0] * step;
          const int sy = y + dir[1] * step;
          if (sx < 0 || sx >= image.width || sy < 0 || sy >= image.height) break;
          const bool inRoi = sx >= left && sx <= right && sy >= top && sy <= bottom;
          if (inRoi && mask[roi(sx, sy)]) continue;
          const size_t qi = pix(sx, sy);
          for (int c = 0; c < 4; ++c) sum[c] += image.rgba[qi + c];
          ++samples;
          break;
        }
      }
      if (samples <= 0) continue;
      const size_t i = pix(x, y);
      for (int c = 0; c < 4; ++c)
        image.rgba[i + c] = static_cast<uint8_t>(sum[c] / samples);
    }
  }
}

// MMD's lower-right "local X/Y/Z" model-operation gizmo is not controlled by
// 表示 > 座標軸表示. It is rendered directly into the capture target even after
// FPS/info and coordinate-axis menus are disabled. Remove only its distinctive
// red/green/blue/purple pixels in the lower-right overlay area, then fill from
// neighboring scene pixels. Requiring all three primary colors avoids touching
// ordinary scene colors in that corner.
void removeMmdLocalTransformGizmo(ImageBuffer& image) {
  if (image.empty() || image.width < 128 || image.height < 128) return;

  // This editor control is a fixed-size 3×2 grid in MMD (about 180×140 px),
  // independent of the requested screen-capture size. Keep its search tight:
  // a broad lower-right scan can confuse colorful scene geometry for the gizmo.
  const int roiW = (std::min)(240, image.width);
  const int roiH = (std::min)(180, image.height);
  const int left = image.width - roiW;
  const int top = image.height - roiH;
  std::vector<uint8_t> mask(static_cast<size_t>(roiW) * static_cast<size_t>(roiH), 0);
  auto roi = [&](int x, int y) {
    return static_cast<size_t>(y - top) * static_cast<size_t>(roiW) +
           static_cast<size_t>(x - left);
  };
  auto pix = [&](int x, int y) {
    return (static_cast<size_t>(y) * static_cast<size_t>(image.width) +
            static_cast<size_t>(x)) * 4u;
  };

  int redCount = 0, greenCount = 0, blueCount = 0;
  for (int y = top; y < image.height; ++y) {
    for (int x = left; x < image.width; ++x) {
      const size_t i = pix(x, y);
      const int r = image.rgba[i + 0], g = image.rgba[i + 1], b = image.rgba[i + 2];
      const bool red = r >= 115 && r > g * 13 / 10 && r > b * 13 / 10;
      const bool green = g >= 105 && g > r * 12 / 10 && g > b * 12 / 10;
      const bool blue = b >= 105 && b > r * 12 / 10 && b > g * 12 / 10;
      const bool purple = r >= 75 && b >= 75 && r > g * 13 / 10 && b > g * 13 / 10;
      if (red) ++redCount;
      if (green) ++greenCount;
      if (blue) ++blueCount;
      if (red || green || blue || purple)
        mask[roi(x, y)] = 1;
    }
  }
  // The supplied reference confirms this fixed lower-right template has all
  // three saturated primaries. The ROI is only 240×180, so this is precise
  // enough without assuming its exact DPI-scaled icon-grid coordinates.
  if (redCount < 6 || greenCount < 6 || blueCount < 6) return;

  const std::vector<uint8_t> detected = mask;
  // The glyphs have dark/white outlines several pixels beyond their colored
  // cores. A small dilation left those outlines behind; expand enough to cover
  // each complete Local XYZ icon before scene-pixel reconstruction.
  constexpr int kGizmoOutlineRadius = 10;
  for (int y = top; y < image.height; ++y) {
    for (int x = left; x < image.width; ++x) {
      if (!detected[roi(x, y)]) continue;
      for (int dy = -kGizmoOutlineRadius; dy <= kGizmoOutlineRadius; ++dy) {
        for (int dx = -kGizmoOutlineRadius; dx <= kGizmoOutlineRadius; ++dx) {
          const int nx = x + dx, ny = y + dy;
          if (nx >= left && nx < image.width && ny >= top && ny < image.height)
            mask[roi(nx, ny)] = 1;
        }
      }
    }
  }

  constexpr int kDirs[][2] = {
      {-1, 0}, {1, 0}, {0, -1}, {0, 1},
      {-1, -1}, {1, -1}, {-1, 1}, {1, 1},
  };
  for (int y = top; y < image.height; ++y) {
    for (int x = left; x < image.width; ++x) {
      if (!mask[roi(x, y)]) continue;
      int sum[4] = {0, 0, 0, 0};
      int samples = 0;
      for (const auto& dir : kDirs) {
        for (int step = 1; step <= 16; ++step) {
          const int sx = x + dir[0] * step, sy = y + dir[1] * step;
          if (sx < 0 || sx >= image.width || sy < 0 || sy >= image.height) break;
          if (sx >= left && sy >= top && mask[roi(sx, sy)]) continue;
          const size_t qi = pix(sx, sy);
          for (int c = 0; c < 4; ++c) sum[c] += image.rgba[qi + c];
          ++samples;
          break;
        }
      }
      if (samples == 0) continue;
      const size_t i = pix(x, y);
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
    // Stage even while skipCaptures_ > 0: the discarded first Present often
    // carries MMD's high-res screen-capture StretchRect. Blocking staging there
    // forced Present fallback (~772² + editor UI).
    if (!wantCapture_) return;
  }
  // Do not recursively capture our own staging/readback surfaces.
  if (capture_.ownsSurface(source)) return;

  D3DSURFACE_DESC desc{};
  if (FAILED(source->GetDesc(&desc)) || desc.Width == 0 || desc.Height == 0)
    return;
  // ray-mmd allocates large R32F/G32R32F targets for depth/effect passes.
  // They are not final color images and were overwriting the staged scene.
  if (desc.Format == D3DFMT_R32F || desc.Format == D3DFMT_G32R32F ||
      desc.Format == D3DFMT_A32B32G32R32F)
    return;

  // Prefer the square we set via MMD「出力サイズ」(mmdOutputFaceSize_).
  const auto settings = controller_.settings();
  const int faceTarget =
      (settings.cubemapFaces && mmdOutputSizeConfigured_ && mmdOutputFaceSize_ > 0)
          ? mmdOutputFaceSize_
          : 0;

  const int w = static_cast<int>(desc.Width);
  const int h = static_cast<int>(desc.Height);
  auto scoreCandidate = [&](int cw, int ch) -> int {
    if (faceTarget > 0) {
      const int tol = (std::max)(64, faceTarget / 8);
      // Exact / near face-size square (primary).
      if (std::abs(cw - faceTarget) <= tol && std::abs(ch - faceTarget) <= tol)
        return 100000 + cw;
      // Large square close to target (e.g. 7680 when asking 8192).
      if (cw == ch && cw >= faceTarget * 3 / 4 && cw <= faceTarget + tol)
        return 50000 + cw;
      // Any large square (≥2048): better than window fallback for VR faces.
      if (cw == ch && cw >= 2048)
        return 20000 + cw;
      return -1; // ignore window-sized while face output size is configured
    }
    if (bbW_ > 0 && bbH_ > 0) {
      const int toleranceW = (std::max)(32, bbW_ / 8);
      const int toleranceH = (std::max)(32, bbH_ / 8);
      if (std::abs(cw - bbW_) <= toleranceW && std::abs(ch - bbH_) <= toleranceH)
        return 1000 + cw;
    }
    return -1;
  };

  const int score = scoreCandidate(w, h);
  if (score < 0) {
    // One-shot diagnostics: why face-sized staging never fires.
    static int s_rejectLog = 0;
    if (faceTarget > 0 && s_rejectLog < 8 && w >= 256 && h >= 256) {
      ++s_rejectLog;
      char msg[160];
      std::snprintf(msg, sizeof(msg),
                    "Pre-overlay reject %s %dx%d (want face~%d)",
                    operation ? operation : "?", w, h, faceTarget);
      Diagnostics::instance().info(msg);
    }
    return;
  }

  if (capture_.hasStagedSurface()) {
    int sw = 0, sh = 0;
    if (capture_.stagedSize(sw, sh) && scoreCandidate(sw, sh) >= score)
      return; // keep equal-or-better staged copy
  }

  // Stage at source size when we accepted a near-face square; only force
  // exact faceTarget for the top score band.
  const bool okStage = (score >= 100000)
                           ? capture_.stageSurface(source, faceTarget, faceTarget)
                           : capture_.stageSurface(source);
  if (!okStage) return;

  bool logCandidate = false;
  {
    std::lock_guard<std::mutex> lock(redrawMu_);
    if (!preOverlayCandidateLogged_ || score >= 100000) {
      preOverlayCandidateLogged_ = true;
      logCandidate = true;
    }
  }
  if (logCandidate) {
    char msg[192];
    std::snprintf(msg, sizeof(msg),
                  "Pre-overlay source staged from %s: %ux%u fmt=%u score=%d",
                  operation ? operation : "?",
                  static_cast<unsigned>(desc.Width), static_cast<unsigned>(desc.Height),
                  static_cast<unsigned>(desc.Format), score);
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
  // D3D9 COM Release must run on the device thread. Reset hook is already
  // there; job-worker cleanup only arms a flag for the next Present.
  if (isDeviceReset) {
    capture_.releasePool();
    wantReleasePool_.store(false);
  } else {
    wantReleasePool_.store(true);
    if (mmdHwnd_ && IsWindow(mmdHwnd_)) {
      InvalidateRect(mmdHwnd_, nullptr, FALSE);
      PostMessageW(mmdHwnd_, WM_PAINT, 0, 0);
    }
  }
  capture_.setSquareCrop(true);
  mainPassDrawn_ = false;
  didReplaceThisFrame_ = false;
  onMainRt_ = true;
  // Changing MMD's output size causes a D3D Reset. Do not restore it from
  // that nested reset callback; restore only when the actual job ends.
  if (!isDeviceReset && mmdOutputSizeConfigured_) {
    timeline_.restoreOutputSize();
    mmdOutputSizeConfigured_ = false;
    mmdOutputFaceSize_ = 0;
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

void PluginHost::onBeforeSetRenderTarget(IDirect3DDevice9* device, DWORD index) {
  if (!device || index != 0) return;
  {
    std::lock_guard<std::mutex> lock(redrawMu_);
    if (!wantCapture_) return;
  }
  // Hook_SetRenderTarget runs before MMD changes RT 0. At this point its
  // previous target contains a completed pass; staging the *new* target after
  // the call grabbed ray-mmd's empty R32F effect/depth surfaces instead.
  IDirect3DSurface9* outgoing = nullptr;
  if (SUCCEEDED(device->GetRenderTarget(0, &outgoing)) && outgoing) {
    stagePreOverlaySource(device, outgoing, "SetRenderTarget(outgoing)");
    outgoing->Release();
  }
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

  if (wantReleasePool_.exchange(false))
    capture_.releasePool();

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
    // Drop stale evict requests; never call EvictManagedResources mid-job.
    wantEvict_.store(false);
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
  ImageBuffer img;
  bool usedPreOverlay = capture_.hasStagedSurface();
  // Window viewport crop belongs on the backbuffer fallback only. Applying it
  // to a face-sized staged RT punches a tiny 772² hole out of a 4K–8K face.
  D3DVIEWPORT9 viewport{};
  const bool haveViewport = SUCCEEDED(device->GetViewport(&viewport)) &&
                            viewport.Width > 0 && viewport.Height > 0;
  if (!usedPreOverlay && haveViewport) {
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
  bool ok = usedPreOverlay ? capture_.captureStaged(img) : capture_.captureViewport(img);
  if (!ok && usedPreOverlay) {
    // A driver may reject the staged copy's format. Preserve rendering rather
    // than failing the face: read the Present backbuffer as the old path did.
    capture_.clearStagedSurface();
    usedPreOverlay = false;
    ok = capture_.captureViewport(img);
  }
  capture_.clearCrop();
  // Mark staged unread only — do not Release the RT here (driver thrash).
  // Do not EvictManagedResources during a long job; MME/ray-mmd often crash
  // after repeated evicts even with free VRAM.
  capture_.clearStagedSurface();
  wantEvict_.store(false);
  // The fallback contains MMD's center marker. The pre-overlay color path can
  // still contain MMD's lower-right model-operation gizmo (local X/Y/Z).
  if (ok && !usedPreOverlay) removeMmdCameraTargetMarker(img);
  if (ok) removeMmdLocalTransformGizmo(img);
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
    // Drive MMD「出力サイズ」so cubemap faces are real high-res RTs — not the
    // ~772² window viewport upscaled into an 8K equirect (soft mush).
    // Cap at 8192 (common D3D9 MaxTexture). GPU-heavy but intentional.
    int face = settings.resolvedFaceSize();
    if (face > 8192) face = 8192;
    if (face < 512) face = 512;
    refreshBackbufferSize();
    char attempt[128];
    std::snprintf(attempt, sizeof(attempt),
                  "seekAnimationFrame: setting MMD output size %dx%d for cubemap faces",
                  face, face);
    Diagnostics::instance().info(attempt);
    if (timeline_.setOutputSize(face, face)) {
      mmdOutputSizeConfigured_ = true;
      mmdOutputFaceSize_ = face;
      refreshBackbufferSize();
      // Let Reset settle and hide editor gizmos before the first face Present.
      timeline_.suppressViewportOverlays();
      for (int i = 0; i < 4 && mmdHwnd_; ++i) {
        InvalidateRect(mmdHwnd_, nullptr, FALSE);
        PostMessageW(mmdHwnd_, WM_PAINT, 0, 0);
        Sleep(40);
      }
      Diagnostics::instance().info(
          "seekAnimationFrame: MMD output size OK — expect face-sized captures");
    } else {
      mmdOutputSizeConfigured_ = false;
      mmdOutputFaceSize_ = 0;
      Diagnostics::instance().warn(
          "seekAnimationFrame: setOutputSize failed; falling back to window viewport "
          "(equirect will look soft at 8K)");
    }
  }
  if (!timeline_.frameEdit() || !IsWindow(timeline_.frameEdit())) {
    if (!timeline_.bind(mmdHwnd_)) return false;
  }
  if (!timeline_.enableScreenCaptureMode1()) {
    Diagnostics::instance().warn(
        "seekAnimationFrame: MMD screen-capture mode unavailable; using Present fallback");
  }
  // Axes hide is async (menu WM_COMMAND); reinforce each seek.
  timeline_.suppressViewportOverlays();
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
  // Intentionally a no-op during the job. Releasing + recreating large RTs
  // every animation frame (and EvictManagedResources) was a top suspect for
  // "plenty of RAM/VRAM but crashes after a while" on D3D9 + MME.
  // releaseRenderSession() still drops the pool when the job ends / cancels.
  wantEvict_.store(false);
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
  // applyCameraFace posts a paint request. Hide MMD's FPS/local text and
  // coordinate axes before that request can produce a candidate capture.
  timeline_.suppressViewportOverlays();
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
    // Discard one Present after camera UI edits so ray-mmd/MME finish evaluating
    // (skip=0 caused soft/wrong faces and broken right-eye PosZ). Staging still
    // runs during the skipped Present — see stagePreOverlaySource.
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
  nativeCameraApplied_ = true;
  if (controller_.isCancelled()) {
    std::lock_guard<std::mutex> lock(redrawMu_);
    wantCapture_ = false;
    skipCaptures_ = 0;
    return false;
  }
  std::unique_lock<std::mutex> lock(redrawMu_);
  bool ok = false;
  for (int i = 0; i < 200; ++i) {
    if (controller_.isCancelled()) break;
    // Nudge occasionally — flooding WM_PAINT re-enters Present under MME.
    if (mmdHwnd_ && (i % 4) == 0) {
      lock.unlock();
      InvalidateRect(mmdHwnd_, nullptr, FALSE);
      PostMessageW(mmdHwnd_, WM_PAINT, 0, 0);
      lock.lock();
    }
    ok = redrawCv_.wait_for(lock, std::chrono::milliseconds(50), [&]{ return captureReady_; });
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
    if (out.empty()) {
      Diagnostics::instance().info("renderView: empty capture");
    } else {
      // Log size only when it changes (avoids 10×/frame log spam).
      static int s_lastW = 0, s_lastH = 0;
      if (out.width != s_lastW || out.height != s_lastH) {
        s_lastW = out.width;
        s_lastH = out.height;
        char msg[96];
        std::snprintf(msg, sizeof(msg), "renderView: face ok %dx%d", out.width, out.height);
        Diagnostics::instance().info(msg);
      }
    }
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
