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

  // The colored cores identify the template, but its anti-aliased dark/white
  // outline and drop shadow extend irregularly beyond them. Once identified,
  // fill the complete template bounding box rather than leaving fragments.
  int minX = image.width, minY = image.height, maxX = -1, maxY = -1;
  for (int y = top; y < image.height; ++y) {
    for (int x = left; x < image.width; ++x) {
      if (!mask[roi(x, y)]) continue;
      minX = (std::min)(minX, x);
      minY = (std::min)(minY, y);
      maxX = (std::max)(maxX, x);
      maxY = (std::max)(maxY, y);
    }
  }
  if (maxX >= minX && maxY >= minY) {
    constexpr int kTemplateMargin = 4;
    minX = (std::max)(left, minX - kTemplateMargin);
    minY = (std::max)(top, minY - kTemplateMargin);
    maxX = (std::min)(image.width - 1, maxX + kTemplateMargin);
    maxY = (std::min)(image.height - 1, maxY + kTemplateMargin);
    for (int y = minY; y <= maxY; ++y)
      for (int x = minX; x <= maxX; ++x)
        mask[roi(x, y)] = 1;
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

// MMD 9.32 x64 recorder path, verified against the native AVI-start routine.
// The recorder's internal surfaces are recreated by these two native methods;
// no AVI muxer or output file is started by this integration.
constexpr uintptr_t kMmdAppOutputWidth = 0xA18F8;
constexpr uintptr_t kMmdAppOutputHeight = 0xA18FC;
constexpr uintptr_t kMmdAppMainWindow = 0xA16C8;
constexpr uintptr_t kMmdAppRenderer = 0xA16E0;
constexpr uintptr_t kMmdAppRecordWindow = 0xA1DC0;
constexpr uintptr_t kMmdAppResetPending = 0xA11E4;
constexpr uintptr_t kRendererDevice = 0x3A9C0;
constexpr uintptr_t kRendererScreenWidth = 0x3A9C8;
constexpr uintptr_t kRendererScreenHeight = 0x3A9CC;
constexpr uintptr_t kRendererAspectRatio = 0x3A9D0;
constexpr uintptr_t kRendererMultisampleGate = 0x3A9E0;
constexpr uintptr_t kRendererPresentParameters = 0x3A9E8;
constexpr uintptr_t kRendererCaptureSurface = 0x3AA28;
constexpr uintptr_t kRendererBackbufferSurface = 0x3AA30;
constexpr uintptr_t kRendererDepthStencil = 0x3AA38;
constexpr uintptr_t kRendererCaptureFormat = 0x3AA40;
constexpr uintptr_t kRendererDepthFormat = 0x3AA18;
constexpr uintptr_t kMmdApplyOutputRva = 0xA5EA0;
constexpr uintptr_t kMmdPostDeviceResetRva = 0xA6160;
constexpr uintptr_t kMmdRefreshViewportRva = 0x216A0;
constexpr unsigned char kMmdApplyOutputSig[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
    0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x80};
constexpr unsigned char kMmdPostDeviceResetSig[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
    0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x89};

template <typename T>
T& mmdField(void* base, uintptr_t offset) {
  return *reinterpret_cast<T*>(reinterpret_cast<std::uint8_t*>(base) + offset);
}

bool mmdNativeRecorderFunctionsMatch(HMODULE mmd) {
  if (!mmd) return false;
  const auto* base = reinterpret_cast<const unsigned char*>(mmd);
  return std::memcmp(base + kMmdApplyOutputRva, kMmdApplyOutputSig,
                     sizeof(kMmdApplyOutputSig)) == 0 &&
         std::memcmp(base + kMmdPostDeviceResetRva, kMmdPostDeviceResetSig,
                     sizeof(kMmdPostDeviceResetSig)) == 0;
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

void PluginHost::setCaptureArmed(bool armed) {
  wantCapture_ = armed;
  SetMmdLocalGizmoSuppress(armed);
}

void PluginHost::deferCapturePresents(bool defer) {
  captureDeferPresents_.store(defer, std::memory_order_release);
  if (defer) capture_.clearStagedSurface();
}

bool PluginHost::expandNativeCaptureViewport(D3DVIEWPORT9& vp) const {
  if (!nativeCaptureSurfaceActive() || mmdOutputFaceSize_ <= 0) return false;
  if (bbW_ <= 0 || bbH_ <= 0) return false;
  const int face = mmdOutputFaceSize_;
  if (vp.Width >= static_cast<DWORD>(face * 3 / 4) &&
      vp.Height >= static_cast<DWORD>(face * 3 / 4))
    return false;
  const int tolW = bbW_ / 8 + 8;
  const int tolH = bbH_ / 8 + 8;
  if (std::abs(static_cast<int>(vp.Width) - bbW_) > tolW ||
      std::abs(static_cast<int>(vp.Height) - bbH_) > tolH)
    return false;
  vp.X = 0;
  vp.Y = 0;
  vp.Width = static_cast<DWORD>(face);
  vp.Height = static_cast<DWORD>(face);
  return true;
}

void PluginHost::bindNativeCaptureTarget(IDirect3DDevice9* device, bool faceSizedViewport) {
  if (!device || !nativeCaptureSurfaceActive() || !nativeCaptureRestore_.ownedCapture)
    return;
  device->SetRenderTarget(0, nativeCaptureRestore_.ownedCapture);
  if (nativeCaptureRestore_.ownedDepth)
    device->SetDepthStencilSurface(nativeCaptureRestore_.ownedDepth);
  D3DVIEWPORT9 vp{};
  vp.X = 0;
  vp.Y = 0;
  vp.MinZ = 0.0f;
  vp.MaxZ = 1.0f;
  if (faceSizedViewport && mmdOutputFaceSize_ > 0) {
    vp.Width = static_cast<DWORD>(mmdOutputFaceSize_);
    vp.Height = static_cast<DWORD>(mmdOutputFaceSize_);
  } else {
    if (bbW_ <= 0 || bbH_ <= 0)
      refreshBackbufferSize();
    vp.Width = static_cast<DWORD>(bbW_ > 0 ? bbW_ : 1);
    vp.Height = static_cast<DWORD>(bbH_ > 0 ? bbH_ : 1);
  }
  device->SetViewport(&vp);
}

void PluginHost::onMmdAppObserved(void* app) {
  if (!app) return;
  mmdApp_.store(app, std::memory_order_release);
  // Re-bind capture RT and the face-sized viewport before Clear so ray-mmd
  // sizes G-buffer / lighting to the square capture, not the window.
  if (device_)
    bindNativeCaptureTarget(device_, true);
}

void PluginHost::onDeviceCreated(IDirect3DDevice9* device) {
  device_ = device;
  capture_.setDevice(device);
  controller_.setCapture(std::shared_ptr<IFrameCapture>(&capture_, [](IFrameCapture*){}));
  controller_.setRedraw(std::shared_ptr<IRedrawRequest>(this, [](IRedrawRequest*){}));
  // Cancel comes from the progress dialog UI thread — only wake waiters here.
  // Capture RT/gate restore runs after the last frame (before encode) and again
  // at job end; both go through releaseRenderSession on the job worker.
  controller_.setAbortHandler([this]() { interruptCaptureWait("abortHandler"); });
  controller_.setCaptureTeardownHandler(
      [this]() { releaseRenderSession("captureDone"); });
  controller_.setCleanupHandler([this]() { releaseRenderSession("jobCleanup"); });
  applyCaptureIsolateFromEnv();
  refreshBackbufferSize();
  Diagnostics::instance().setLogPath(moduleDir() + L"\\mmd_vr_renderer.log");
  Diagnostics::instance().info("Device created; MMD-VR-Renderer active");
  Diagnostics::instance().compatibilityNote("MME", "chained", "Expect d3d9_mme.dll beside exe");
}

void PluginHost::onDeviceReset() {
  if (nativeCaptureResetInProgress_.load(std::memory_order_acquire)) {
    // PostDeviceReset calls IDirect3DDevice9::Reset internally. It is part of
    // the requested high-res transition, not a lost device, so do not cancel
    // the render job from its Reset hook.
    capture_.setDevice(device_);
    refreshBackbufferSize();
    Diagnostics::instance().info("MMD native high-resolution Reset completed");
    return;
  }
  Diagnostics::instance().warn("Device reset");
  releaseRenderSession("deviceReset");
  capture_.setDevice(device_);
  refreshBackbufferSize();
}

bool PluginHost::activateNativeCaptureSurface() {
  const int face = nativeCaptureFaceSize_.load(std::memory_order_acquire);
  void* app = mmdApp_.load(std::memory_order_acquire);
  HMODULE mmd = GetModuleHandleW(nullptr);
  if (!app || face < 512 || face > 8192 || !mmd ||
      !mmdNativeRecorderFunctionsMatch(mmd)) {
    Diagnostics::instance().error(
        "Native 8K capture unavailable: MMD 9.32 recorder signature/app state mismatch");
    return false;
  }

  const HWND appWindow = mmdField<HWND>(app, kMmdAppMainWindow);
  void* renderer = mmdField<void*>(app, kMmdAppRenderer);
  if (!renderer || !appWindow || !IsWindow(appWindow) ||
      (mmdHwnd_ && appWindow != mmdHwnd_)) {
    Diagnostics::instance().error(
        "Native 8K capture unavailable: MMD app/renderer pointers are invalid");
    return false;
  }

  // MME's Reset interceptor crashes on an 8192 swapchain (builds s/t). Resetting
  // only the inner device (build r) then breaks MME reload. Skip device Reset
  // entirely: keep the window backbuffer, and hang an 8192 offscreen RT+depth
  // on the native recorder slots instead.
  IDirect3DDevice9* createDev = device_ ? device_ : mmdField<IDirect3DDevice9*>(renderer, kRendererDevice);
  if (!createDev) {
    Diagnostics::instance().error(
        "Native 8K capture unavailable: no D3D9 device for CreateRenderTarget");
    return false;
  }

  nativeCaptureRestore_ = {};
  nativeCaptureRestore_.app = app;
  nativeCaptureRestore_.renderer = renderer;
  nativeCaptureRestore_.device = mmdField<IDirect3DDevice9*>(renderer, kRendererDevice);
  nativeCaptureRestore_.outputWidth = mmdField<std::uint32_t>(app, kMmdAppOutputWidth);
  nativeCaptureRestore_.outputHeight = mmdField<std::uint32_t>(app, kMmdAppOutputHeight);
  nativeCaptureRestore_.screenWidth = mmdField<std::uint32_t>(renderer, kRendererScreenWidth);
  nativeCaptureRestore_.screenHeight = mmdField<std::uint32_t>(renderer, kRendererScreenHeight);
  nativeCaptureRestore_.aspectRatio = mmdField<float>(renderer, kRendererAspectRatio);
  nativeCaptureRestore_.backbufferWidth =
      mmdField<std::uint32_t>(renderer, kRendererPresentParameters);
  nativeCaptureRestore_.backbufferHeight =
      mmdField<std::uint32_t>(renderer, kRendererPresentParameters + sizeof(std::uint32_t));
  nativeCaptureRestore_.resetPending = mmdField<std::uint8_t>(app, kMmdAppResetPending);
  // 0 = lockable captureSurface path; 1 = MSAA/backbuffer path. Force 0 for the
  // duration of our offscreen 8K RT so FrameDriver binds captureSurface.
  nativeCaptureRestore_.multisampleGate =
      mmdField<std::uint8_t>(renderer, kRendererMultisampleGate);
  nativeCaptureRestore_.recordingWindow = mmdField<void*>(app, kMmdAppRecordWindow);
  nativeCaptureRestore_.previousCapture =
      mmdField<IDirect3DSurface9*>(renderer, kRendererCaptureSurface);
  nativeCaptureRestore_.previousDepth =
      mmdField<IDirect3DSurface9*>(renderer, kRendererDepthStencil);
  if (nativeCaptureRestore_.recordingWindow) {
    Diagnostics::instance().error(
        "Native 8K capture refused: MMD already owns an active recording window");
    nativeCaptureRestore_ = {};
    return false;
  }

  static bool recClassRegistered = false;
  if (!recClassRegistered) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RecWindow";
    RegisterClassW(&wc);
    recClassRegistered = true;
  }
  nativeCaptureWindow_ = CreateWindowExW(
      WS_EX_TOOLWINDOW, L"RecWindow", L"MMDVR Native Capture",
      WS_POPUP, -32000, -32000, face, face, nullptr, nullptr,
      GetModuleHandleW(nullptr), nullptr);
  if (!nativeCaptureWindow_ || !IsWindow(nativeCaptureWindow_)) {
    nativeCaptureWindow_ = nullptr;
    Diagnostics::instance().error(
        "Native 8K capture unavailable: could not create recorder destination window");
    nativeCaptureRestore_ = {};
    return false;
  }

  const D3DFORMAT format = mmdField<D3DFORMAT>(renderer, kRendererCaptureFormat);
  IDirect3DSurface9* captureSurface = nullptr;
  HRESULT hr = createDev->CreateRenderTarget(
      static_cast<UINT>(face), static_cast<UINT>(face), format,
      D3DMULTISAMPLE_NONE, 0, TRUE, &captureSurface, nullptr);
  if (FAILED(hr) || !captureSurface) {
    char msg[160];
    std::snprintf(msg, sizeof(msg),
                  "Native 8K capture CreateRenderTarget(%d) failed hr=0x%08lx",
                  face, static_cast<unsigned long>(hr));
    Diagnostics::instance().error(msg);
    DestroyWindow(nativeCaptureWindow_);
    nativeCaptureWindow_ = nullptr;
    nativeCaptureRestore_ = {};
    return false;
  }

  D3DFORMAT depthFmt = mmdField<D3DFORMAT>(renderer, kRendererDepthFormat);
  if (depthFmt == D3DFMT_UNKNOWN)
    depthFmt = D3DFMT_D24S8;
  if (nativeCaptureRestore_.previousDepth) {
    D3DSURFACE_DESC depthDesc{};
    if (SUCCEEDED(nativeCaptureRestore_.previousDepth->GetDesc(&depthDesc)) &&
        depthDesc.MultiSampleType == D3DMULTISAMPLE_NONE)
      depthFmt = depthDesc.Format;
  }
  IDirect3DSurface9* depthSurface = nullptr;
  hr = createDev->CreateDepthStencilSurface(
      static_cast<UINT>(face), static_cast<UINT>(face), depthFmt,
      D3DMULTISAMPLE_NONE, 0, TRUE, &depthSurface, nullptr);
  if (FAILED(hr) || !depthSurface) {
    char msg[160];
    std::snprintf(msg, sizeof(msg),
                  "Native 8K capture CreateDepthStencilSurface(%d) failed hr=0x%08lx",
                  face, static_cast<unsigned long>(hr));
    Diagnostics::instance().error(msg);
    captureSurface->Release();
    DestroyWindow(nativeCaptureWindow_);
    nativeCaptureWindow_ = nullptr;
    nativeCaptureRestore_ = {};
    return false;
  }

  nativeCaptureRestore_.ownedCapture = captureSurface;
  nativeCaptureRestore_.ownedDepth = depthSurface;

  // Keep the swapchain at the window size (no Reset). The capture RT is
  // face-sized; SetViewport rewrites the window viewport to that square so
  // ray-mmd fills it. Shadow/effect viewports smaller than the window stay.
  mmdField<IDirect3DSurface9*>(renderer, kRendererCaptureSurface) = captureSurface;
  mmdField<IDirect3DSurface9*>(renderer, kRendererDepthStencil) = depthSurface;
  mmdField<std::uint8_t>(renderer, kRendererMultisampleGate) = 0;
  mmdField<void*>(app, kMmdAppRecordWindow) = nativeCaptureWindow_;
  if (nativeCaptureRestore_.multisampleGate != 0) {
    char gateMsg[144];
    std::snprintf(gateMsg, sizeof(gateMsg),
                  "Native 8K capture: forcing captureSurface gate 0 (was %u; MSAA backbuffer path)",
                  static_cast<unsigned>(nativeCaptureRestore_.multisampleGate));
    Diagnostics::instance().info(gateMsg);
  }

  capture_.setDevice(createDev);
  refreshBackbufferSize();
  mmdOutputFaceSize_ = face;
  mmdOutputSizeConfigured_ = true;
  bindNativeCaptureTarget(createDev, true);

  D3DSURFACE_DESC captureDesc{};
  captureSurface->GetDesc(&captureDesc);
  char msg[240];
  std::snprintf(msg, sizeof(msg),
                "Native capture surface active (no Reset): capture=%ux%u; window viewport %dx%d expanded to face",
                static_cast<unsigned>(captureDesc.Width), static_cast<unsigned>(captureDesc.Height),
                bbW_, bbH_);
  Diagnostics::instance().info(msg);
  return true;
}

void PluginHost::restoreNativeCaptureSurface() {
  void* app = nativeCaptureRestore_.app;
  void* renderer = nativeCaptureRestore_.renderer;
  HMODULE mmd = GetModuleHandleW(nullptr);
  const std::uint8_t savedGate = nativeCaptureRestore_.multisampleGate;

  Diagnostics::instance().info("Native capture restore: begin (no Device Reset)");

  if (app)
    mmdField<void*>(app, kMmdAppRecordWindow) = nativeCaptureRestore_.recordingWindow;

  if (renderer) {
    auto*& captureSlot = mmdField<IDirect3DSurface9*>(renderer, kRendererCaptureSurface);
    auto*& depthSlot = mmdField<IDirect3DSurface9*>(renderer, kRendererDepthStencil);
    if (captureSlot == nativeCaptureRestore_.ownedCapture)
      captureSlot = nativeCaptureRestore_.previousCapture;
    if (depthSlot == nativeCaptureRestore_.ownedDepth)
      depthSlot = nativeCaptureRestore_.previousDepth;
    if (nativeCaptureRestore_.ownedCapture) {
      nativeCaptureRestore_.ownedCapture->Release();
      nativeCaptureRestore_.ownedCapture = nullptr;
    }
    if (nativeCaptureRestore_.ownedDepth) {
      nativeCaptureRestore_.ownedDepth->Release();
      nativeCaptureRestore_.ownedDepth = nullptr;
    }
    mmdField<std::uint8_t>(renderer, kRendererMultisampleGate) = savedGate;
  }

  if (nativeCaptureWindow_ && IsWindow(nativeCaptureWindow_))
    DestroyWindow(nativeCaptureWindow_);
  nativeCaptureWindow_ = nullptr;

  IDirect3DDevice9* dev = device_ ? device_ : nativeCaptureRestore_.device;
  if (dev) {
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
      dev->SetRenderTarget(0, bb);
      bb->Release();
    }
    if (renderer) {
      if (auto* depth = mmdField<IDirect3DSurface9*>(renderer, kRendererDepthStencil))
        dev->SetDepthStencilSurface(depth);
    }
    if (bbW_ > 0 && bbH_ > 0) {
      D3DVIEWPORT9 vp{};
      vp.Width = static_cast<DWORD>(bbW_);
      vp.Height = static_cast<DWORD>(bbH_);
      vp.MaxZ = 1.0f;
      dev->SetViewport(&vp);
    }
  }

  // Soft refresh only — PostDeviceReset with MME was killing the process on the
  // next record (no log). Nudge viewport by 2px so SasRefreshViewport recreates
  // ray/MME effect RTs (same W/H as capture-time lie would otherwise skip).
  if (app && mmd && mmdNativeRecorderFunctionsMatch(mmd)) {
    using NativeAppFn = void(__fastcall*)(void*);
    auto refreshViewport = reinterpret_cast<NativeAppFn>(
        reinterpret_cast<std::uint8_t*>(mmd) + kMmdRefreshViewportRva);
    refreshViewport(app);
  }
  if (dev && bbW_ > 4 && bbH_ > 4) {
    D3DVIEWPORT9 nudge{};
    nudge.Width = static_cast<DWORD>(bbW_ - 2);
    nudge.Height = static_cast<DWORD>(bbH_ - 2);
    nudge.MaxZ = 1.0f;
    dev->SetViewport(&nudge);
  }
  if (app && mmd && mmdNativeRecorderFunctionsMatch(mmd)) {
    using NativeAppFn = void(__fastcall*)(void*);
    auto refreshViewport = reinterpret_cast<NativeAppFn>(
        reinterpret_cast<std::uint8_t*>(mmd) + kMmdRefreshViewportRva);
    refreshViewport(app);
  }
  if (dev && bbW_ > 0 && bbH_ > 0) {
    D3DVIEWPORT9 vp{};
    vp.Width = static_cast<DWORD>(bbW_);
    vp.Height = static_cast<DWORD>(bbH_);
    vp.MaxZ = 1.0f;
    dev->SetViewport(&vp);
  }

  capture_.setDevice(device_);
  refreshBackbufferSize();
  nativeCaptureRestore_ = {};
  mmdOutputSizeConfigured_ = false;
  mmdOutputFaceSize_ = 0;
  // Mark idle only after surfaces/gate are restored (waiters key off this).
  nativeCaptureState_.store(0, std::memory_order_release);
  Diagnostics::instance().info("Native capture surface released; MMD output restored (no Reset)");
}

void PluginHost::processNativeCaptureSurfaceRequests() {
  if (nativeCaptureRestoreRequested_.exchange(false, std::memory_order_acq_rel)) {
    restoreNativeCaptureSurface();
    std::lock_guard<std::mutex> lock(redrawMu_);
    redrawCv_.notify_all();
    return;
  }
  int expected = 1;
  if (nativeCaptureState_.compare_exchange_strong(expected, 2, std::memory_order_acq_rel)) {
    if (activateNativeCaptureSurface())
      nativeCaptureState_.store(3, std::memory_order_release);
    else
      nativeCaptureState_.store(-1, std::memory_order_release);
    std::lock_guard<std::mutex> lock(redrawMu_);
    redrawCv_.notify_all();
  }
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
  if (captureDeferPresents_.load(std::memory_order_acquire)) return;
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
      return -1; // never upscale a window/auxiliary RT as a native face
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
  captureDeferPresents_.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(redrawMu_);
    setCaptureArmed(false);
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
  const int nativeCaptureState = nativeCaptureState_.load(std::memory_order_acquire);
  const bool nativeCaptureActive =
      nativeCaptureState == 1 || nativeCaptureState == 2 || nativeCaptureState == 3 ||
      nativeCaptureRestoreRequested_.load(std::memory_order_acquire);
  {
    char st[128];
    std::snprintf(st, sizeof(st),
                  "releaseRenderSession(%s): nativeState=%d active=%d",
                  reason ? reason : "?", nativeCaptureState, nativeCaptureActive ? 1 : 0);
    Diagnostics::instance().info(st);
  }
  if (!isDeviceReset && nativeCaptureActive) {
    // Restore on the D3D thread (after Present). Worker waits for state→0 only
    // after restoreNativeCaptureSurface finishes — never mid-teardown.
    nativeCaptureRestoreRequested_.store(true, std::memory_order_release);
    if (mmdHwnd_ && IsWindow(mmdHwnd_)) {
      InvalidateRect(mmdHwnd_, nullptr, FALSE);
      PostMessageW(mmdHwnd_, WM_PAINT, 0, 0);
    }
    std::unique_lock<std::mutex> lock(redrawMu_);
    bool restored = false;
    for (int i = 0; i < 200; ++i) {
      restored = redrawCv_.wait_for(lock, std::chrono::milliseconds(50), [&] {
        return nativeCaptureState_.load(std::memory_order_acquire) == 0 &&
               !nativeCaptureRestoreRequested_.load(std::memory_order_acquire);
      });
      if (restored) break;
      if (mmdHwnd_ && IsWindow(mmdHwnd_) && (i % 4) == 0) {
        InvalidateRect(mmdHwnd_, nullptr, FALSE);
        PostMessageW(mmdHwnd_, WM_PAINT, 0, 0);
      }
    }
    if (!restored) {
      Diagnostics::instance().warn(
          "releaseRenderSession: native capture restore timed out (D3D thread did not Present)");
    }
  }
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
  if (!isDeviceReset && mmdOutputSizeConfigured_ &&
      nativeCaptureState_.load(std::memory_order_acquire) == 0) {
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

bool PluginHost::adjustPresentRects(const RECT*& source, const RECT*& dest,
                                         RECT& localSource, RECT& localDest) {
  if (nativeCaptureState_.load(std::memory_order_acquire) != 3)
    return false;
  if (bbW_ <= 0 || bbH_ <= 0)
    refreshBackbufferSize();
  if (bbW_ <= 0 || bbH_ <= 0)
    return false;
  const bool srcOverflow = source && (source->right > bbW_ || source->bottom > bbH_ ||
                                      source->left < 0 || source->top < 0);
  const bool dstOverflow = dest && (dest->right > bbW_ || dest->bottom > bbH_ ||
                                    dest->left < 0 || dest->top < 0);
  if (!srcOverflow && !dstOverflow)
    return false;
  localSource = {0, 0, bbW_, bbH_};
  localDest = localSource;
  if (source) source = &localSource;
  if (dest) dest = &localDest;
  return true;
}

void PluginHost::onAfterPresent(IDirect3DDevice9* device) {
  if (device) device_ = device;
  // Device Reset is illegal while Present is on the stack (build s crash).
  // MME's Reset interceptor (OnLost → real Reset → OnReset) only runs when
  // Reset is invoked on the hooked proxy after Present has returned.
  const int nativeStateBefore = nativeCaptureState_.load(std::memory_order_acquire);
  processNativeCaptureSurfaceRequests();
  const int nativeStateAfter = nativeCaptureState_.load(std::memory_order_acquire);
  if (nativeStateBefore != nativeStateAfter &&
      (nativeStateAfter == 3 || nativeStateAfter == -1 || nativeStateAfter == 0)) {
    if (mmdHwnd_ && IsWindow(mmdHwnd_)) {
      InvalidateRect(mmdHwnd_, nullptr, FALSE);
      PostMessageW(mmdHwnd_, WM_PAINT, 0, 0);
    }
  }
}

void PluginHost::onBeforePresent(IDirect3DDevice9* device) {
  if (device) device_ = device;
  if (captureDeferPresents_.load(std::memory_order_acquire)) {
    capture_.clearStagedSurface();
    return;
  }
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
  const bool requireNativeFace =
      nativeCaptureState_.load(std::memory_order_acquire) == 3 &&
      mmdOutputFaceSize_ > 0;
  if (requireNativeFace && !capture_.hasStagedSurface() &&
      nativeCaptureRestore_.renderer) {
    // The recording target belongs to MMD's inner device under MME. Capture
    // it explicitly here rather than waiting for a proxy SetRenderTarget hook
    // that may only observe the outer device.
    auto* nativeSurface = mmdField<IDirect3DSurface9*>(
        nativeCaptureRestore_.renderer, kRendererCaptureSurface);
    if (nativeSurface) {
      D3DSURFACE_DESC nativeDesc{};
      if (SUCCEEDED(nativeSurface->GetDesc(&nativeDesc)) &&
          nativeDesc.Width >= static_cast<UINT>(mmdOutputFaceSize_ * 3 / 4) &&
          nativeDesc.Height >= static_cast<UINT>(mmdOutputFaceSize_ * 3 / 4)) {
        capture_.stageSurface(nativeSurface, mmdOutputFaceSize_, mmdOutputFaceSize_);
      }
    }
  }
  bool usedPreOverlay = capture_.hasStagedSurface();
  if (requireNativeFace) {
    int sourceW = bbW_;
    int sourceH = bbH_;
    if (usedPreOverlay)
      capture_.stagedSize(sourceW, sourceH);
    const int minimum = mmdOutputFaceSize_ * 3 / 4;
    if (sourceW < minimum || sourceH < minimum) {
      char msg[192];
      std::snprintf(msg, sizeof(msg),
                    "Native capture rejected undersized source %dx%d (need at least %dx%d); refusing upscale",
                    sourceW, sourceH, minimum, minimum);
      Diagnostics::instance().error(msg);
      capture_.clearStagedSurface();
      std::lock_guard<std::mutex> lock(redrawMu_);
      captureReady_ = true; // empty pendingCapture_ signals a failed face
      wantCapture_ = false;
      redrawCv_.notify_all();
      return;
    }
  }
  // Window viewport crop belongs on the backbuffer fallback only. Applying it
  // to a face-sized staged RT punches a tiny 772² hole out of a 4K–8K face.
  D3DVIEWPORT9 viewport{};
  const bool haveViewport = SUCCEEDED(device->GetViewport(&viewport)) &&
                            viewport.Width > 0 && viewport.Height > 0;
  if (!usedPreOverlay && haveViewport && !requireNativeFace) {
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
  // Fallback Present path still has the camera ◎. Local XYZ is skipped via
  // MinHook while armed; pixel cleanup remains for signature-mismatch builds.
  if (ok && !usedPreOverlay) removeMmdCameraTargetMarker(img);
  if (ok) removeMmdLocalTransformGizmo(img);
  {
    std::lock_guard<std::mutex> lock(redrawMu_);
    if (ok) pendingCapture_ = std::move(img);
    captureReady_ = ok;
    // Stop this request before another Present can consume it, but keep the
    // Local XYZ draw hook armed until the worker has consumed this face.
    // Otherwise an editor repaint between consecutive cubemap faces redraws
    // the gizmo into the next face's staged high-resolution source.
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
    // MMD's output-size dialog alone leaves the live renderer at the viewport
    // size. Use its native AVI recorder target setup (without AVI writing) so
    // faces are rendered into a real high-resolution color surface.
    int face = settings.resolvedFaceSize();
    if (face > 8192) face = 8192;
    if (face < 512) face = 512;
    char attempt[128];
    std::snprintf(attempt, sizeof(attempt),
                  "seekAnimationFrame: requesting native MMD capture surface %dx%d",
                  face, face);
    Diagnostics::instance().info(attempt);
    nativeCaptureFaceSize_.store(face, std::memory_order_release);
    nativeCaptureState_.store(1, std::memory_order_release);
    timeline_.suppressViewportOverlays();
    InvalidateRect(mmdHwnd_, nullptr, FALSE);
    PostMessageW(mmdHwnd_, WM_PAINT, 0, 0);

    std::unique_lock<std::mutex> lock(redrawMu_);
    bool settled = false;
    for (int i = 0; i < 300 && !controller_.isCancelled(); ++i) {
      settled = redrawCv_.wait_for(lock, std::chrono::milliseconds(100), [&] {
        const int state = nativeCaptureState_.load(std::memory_order_acquire);
        return state == 3 || state == -1;
      });
      if (settled) break;
    }
    const int state = nativeCaptureState_.load(std::memory_order_acquire);
    lock.unlock();
    if (state != 3 || !mmdOutputSizeConfigured_) {
      Diagnostics::instance().error(
          "seekAnimationFrame: native MMD high-resolution target was not activated; job cancelled");
      return false;
    }
    Diagnostics::instance().info(
        "seekAnimationFrame: native MMD high-resolution target ready");
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
    setCaptureArmed(true);
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
  setCaptureArmed(false);
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
    setCaptureArmed(true);
    // ray-mmd latches the camera on the frame after the edit commit. The first
    // Present is still the previous direction (character repeated on side faces).
    skipCaptures_ = 1;
    pendingCam_ = cam;
    preOverlayCandidateLogged_ = false;
  }

  if (controller_.isCancelled()) {
    std::lock_guard<std::mutex> lock(redrawMu_);
    setCaptureArmed(false);
    skipCaptures_ = 0;
    return false;
  }

  if (!timeline_.applyCameraFace(nativeCameraBase_, cam)) {
    std::lock_guard<std::mutex> lock(redrawMu_);
    setCaptureArmed(false);
    skipCaptures_ = 0;
    Diagnostics::instance().error("renderView: failed to apply native MMD camera face");
    return false;
  }
  if (controller_.isCancelled()) {
    std::lock_guard<std::mutex> lock(redrawMu_);
    setCaptureArmed(false);
    skipCaptures_ = 0;
    return false;
  }
  // Keep Camera selected so native face values apply reliably; the red ◎ is
  // removed from the captured pixels after Present.
  nativeCameraApplied_ = true;
  if (controller_.isCancelled()) {
    std::lock_guard<std::mutex> lock(redrawMu_);
    setCaptureArmed(false);
    skipCaptures_ = 0;
    return false;
  }
  std::unique_lock<std::mutex> lock(redrawMu_);
  bool ok = false;
  // An 8192² ray-mmd/MME frame can take well beyond the normal 10-second
  // viewport paint interval before it reaches Present. The first face after
  // an output-size Reset is particularly expensive. Keep smaller jobs
  // responsive while allowing the requested full-resolution frame to finish.
  const int waitBudgetMs = faceMax >= 8192 ? 60000 : faceMax >= 4096 ? 30000 : 10000;
  constexpr int kWaitSliceMs = 50;
  const int maxWaits = waitBudgetMs / kWaitSliceMs;
  for (int i = 0; i < maxWaits; ++i) {
    if (controller_.isCancelled()) break;
    // One nudge up front, then only if Present has not arrived (~2s). Extra
    // paints were queuing full ray-mmd frames behind the one we wait for.
    if (mmdHwnd_ && (i == 0 || (i % 40) == 0)) {
      lock.unlock();
      InvalidateRect(mmdHwnd_, nullptr, FALSE);
      PostMessageW(mmdHwnd_, WM_PAINT, 0, 0);
      lock.lock();
    }
    ok = redrawCv_.wait_for(lock, std::chrono::milliseconds(kWaitSliceMs),
                            [&]{ return captureReady_; });
    if (ok) break;
  }

  setCaptureArmed(false);
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

  char timeout[192];
  std::snprintf(timeout, sizeof(timeout),
                "renderView timed out after %d ms waiting for Present (face=%d; "
                "MMD may be paused / not redrawing). Face skipped.",
                waitBudgetMs, faceMax);
  Diagnostics::instance().error(timeout);
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
      setCaptureArmed(true);
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
      setCaptureArmed(false);
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
      setCaptureArmed(true);
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
      setCaptureArmed(false);
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
