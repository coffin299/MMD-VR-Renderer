from pathlib import Path

p = Path(r"src/plugin/PluginHost.cpp")
s = p.read_text(encoding="utf-8")

start = s.find("bool PluginHost::activateNativeCaptureSurface() {")
rest_end = s.find("void PluginHost::processNativeCaptureSurfaceRequests() {")
if start < 0 or rest_end < 0:
    raise SystemExit(f"markers missing {start} {rest_end}")

new_body = r'''bool PluginHost::activateNativeCaptureSurface() {
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
  auto*& rendererDeviceSlot =
      mmdField<IDirect3DDevice9*>(renderer, kRendererDevice);
  IDirect3DDevice9* rendererDevice = rendererDeviceSlot;
  if (!rendererDevice) {
    Diagnostics::instance().error(
        "Native 8K capture unavailable: MMD renderer has no D3D9 device");
    return false;
  }

  // MME returns a proxy device to CreateDevice (what we hook). MMD may keep
  // the underlying device pointer. PostDeviceReset must Reset through the
  // hooked proxy so MME can drop/recreate its resources; resetting only the
  // inner device leaves MME holding destroyed objects and crashes next frame.
  IDirect3DDevice9* resetDevice = device_ ? device_ : rendererDevice;
  if (device_ && rendererDevice != device_) {
    char msg[192];
    std::snprintf(msg, sizeof(msg),
                  "Native 8K capture: Reset via MME device %p (MMD stored %p)",
                  static_cast<void*>(device_), static_cast<void*>(rendererDevice));
    Diagnostics::instance().info(msg);
  }

  nativeCaptureRestore_ = {};
  nativeCaptureRestore_.app = app;
  nativeCaptureRestore_.renderer = renderer;
  nativeCaptureRestore_.device = rendererDevice;
  nativeCaptureRestore_.outputWidth = mmdField<std::uint32_t>(app, kMmdAppOutputWidth);
  nativeCaptureRestore_.outputHeight = mmdField<std::uint32_t>(app, kMmdAppOutputHeight);
  nativeCaptureRestore_.screenWidth = mmdField<std::uint32_t>(renderer, kRendererScreenWidth);
  nativeCaptureRestore_.screenHeight = mmdField<std::uint32_t>(renderer, kRendererScreenHeight);
  nativeCaptureRestore_.backbufferWidth =
      mmdField<std::uint32_t>(renderer, kRendererPresentParameters);
  nativeCaptureRestore_.backbufferHeight =
      mmdField<std::uint32_t>(renderer, kRendererPresentParameters + sizeof(std::uint32_t));
  nativeCaptureRestore_.resetPending = mmdField<std::uint8_t>(app, kMmdAppResetPending);
  nativeCaptureRestore_.recordingWindow = mmdField<void*>(app, kMmdAppRecordWindow);
  if (nativeCaptureRestore_.recordingWindow) {
    Diagnostics::instance().error(
        "Native 8K capture refused: MMD already owns an active recording window");
    nativeCaptureRestore_ = {};
    return false;
  }
  nativeCaptureWindow_ = CreateWindowExW(
      WS_EX_TOOLWINDOW, L"STATIC", L"MMDVR Native Capture",
      WS_POPUP, -32000, -32000, face, face, appWindow, nullptr,
      GetModuleHandleW(nullptr), nullptr);
  if (!nativeCaptureWindow_ || !IsWindow(nativeCaptureWindow_)) {
    nativeCaptureWindow_ = nullptr;
    Diagnostics::instance().error(
        "Native 8K capture unavailable: could not create recorder destination window");
    nativeCaptureRestore_ = {};
    return false;
  }

  // Native AVI-start GrowRenderTarget path without the DirectShow writer.
  mmdField<std::uint32_t>(app, kMmdAppOutputWidth) = static_cast<std::uint32_t>(face);
  mmdField<std::uint32_t>(app, kMmdAppOutputHeight) = static_cast<std::uint32_t>(face);
  mmdField<std::uint32_t>(renderer, kRendererScreenWidth) = static_cast<std::uint32_t>(face);
  mmdField<std::uint32_t>(renderer, kRendererScreenHeight) = static_cast<std::uint32_t>(face);
  mmdField<std::uint32_t>(renderer, kRendererPresentParameters) = static_cast<std::uint32_t>(face);
  mmdField<std::uint32_t>(renderer, kRendererPresentParameters + sizeof(std::uint32_t)) =
      static_cast<std::uint32_t>(face);
  mmdField<std::uint8_t>(app, kMmdAppResetPending) = 1;

  using NativeAppFn = void(__fastcall*)(void*);
  auto applyOutput = reinterpret_cast<NativeAppFn>(
      reinterpret_cast<std::uint8_t*>(mmd) + kMmdApplyOutputRva);
  auto postDeviceReset = reinterpret_cast<NativeAppFn>(
      reinterpret_cast<std::uint8_t*>(mmd) + kMmdPostDeviceResetRva);
  applyOutput(app);

  IDirect3DDevice9* previousDevice = rendererDeviceSlot;
  rendererDeviceSlot = resetDevice;
  nativeCaptureResetInProgress_.store(true, std::memory_order_release);
  postDeviceReset(app);
  nativeCaptureResetInProgress_.store(false, std::memory_order_release);
  // Keep the device MMD will Present/Reset with as the hooked MME proxy.
  if (device_)
    rendererDeviceSlot = device_;
  else
    rendererDeviceSlot = previousDevice;
  rendererDevice = rendererDeviceSlot;

  // A real HWND is required for the recording Present path. DirectShow graph
  // setup is intentionally not invoked.
  mmdField<void*>(app, kMmdAppRecordWindow) = nativeCaptureWindow_;
  auto*& captureSurface =
      mmdField<IDirect3DSurface9*>(renderer, kRendererCaptureSurface);
  if (!captureSurface) {
    const D3DFORMAT format = mmdField<D3DFORMAT>(renderer, kRendererCaptureFormat);
    const HRESULT hr = rendererDevice->CreateRenderTarget(
        static_cast<UINT>(face), static_cast<UINT>(face), format,
        D3DMULTISAMPLE_NONE, 0, TRUE, &captureSurface, nullptr);
    if (FAILED(hr) || !captureSurface) {
      char msg[160];
      std::snprintf(msg, sizeof(msg),
                    "Native 8K capture CreateRenderTarget(%d) failed hr=0x%08lx",
                    face, static_cast<unsigned long>(hr));
      Diagnostics::instance().error(msg);
      restoreNativeCaptureSurface();
      return false;
    }
  }
  capture_.setDevice(rendererDevice);

  // Do not SetRenderTarget/RefreshViewport here. Native AVI start leaves the
  // next FrameDriver bind to captureSurface when recordingWindow != null.
  // Forcing RT0 mid-Present after Reset races MME effect reload.

  D3DSURFACE_DESC captureDesc{};
  if (FAILED(captureSurface->GetDesc(&captureDesc)) ||
      captureDesc.Width < static_cast<UINT>(face * 3 / 4) ||
      captureDesc.Height < static_cast<UINT>(face * 3 / 4)) {
    Diagnostics::instance().error(
        "Native 8K capture target is undersized after Reset; refusing upscale");
    restoreNativeCaptureSurface();
    return false;
  }
  refreshBackbufferSize();
  mmdOutputFaceSize_ = face;
  mmdOutputSizeConfigured_ = true;
  char msg[192];
  std::snprintf(msg, sizeof(msg),
                "Native capture surface active: %ux%u outerBackbuffer=%dx%d (AVI writer not started)",
                static_cast<unsigned>(captureDesc.Width), static_cast<unsigned>(captureDesc.Height),
                bbW_, bbH_);
  Diagnostics::instance().info(msg);
  return true;
}

void PluginHost::restoreNativeCaptureSurface() {
  void* app = nativeCaptureRestore_.app;
  void* renderer = nativeCaptureRestore_.renderer;
  HMODULE mmd = GetModuleHandleW(nullptr);
  if (!app || !renderer || !mmd || !mmdNativeRecorderFunctionsMatch(mmd)) {
    if (nativeCaptureWindow_ && IsWindow(nativeCaptureWindow_))
      DestroyWindow(nativeCaptureWindow_);
    nativeCaptureWindow_ = nullptr;
    nativeCaptureRestore_ = {};
    nativeCaptureState_.store(0, std::memory_order_release);
    mmdOutputSizeConfigured_ = false;
    mmdOutputFaceSize_ = 0;
    return;
  }

  mmdField<void*>(app, kMmdAppRecordWindow) = nativeCaptureRestore_.recordingWindow;
  mmdField<std::uint32_t>(app, kMmdAppOutputWidth) = nativeCaptureRestore_.outputWidth;
  mmdField<std::uint32_t>(app, kMmdAppOutputHeight) = nativeCaptureRestore_.outputHeight;
  mmdField<std::uint32_t>(renderer, kRendererScreenWidth) = nativeCaptureRestore_.screenWidth;
  mmdField<std::uint32_t>(renderer, kRendererScreenHeight) = nativeCaptureRestore_.screenHeight;
  mmdField<std::uint32_t>(renderer, kRendererPresentParameters) =
      nativeCaptureRestore_.backbufferWidth;
  mmdField<std::uint32_t>(renderer, kRendererPresentParameters + sizeof(std::uint32_t)) =
      nativeCaptureRestore_.backbufferHeight;
  mmdField<std::uint8_t>(app, kMmdAppResetPending) = nativeCaptureRestore_.resetPending;

  using NativeAppFn = void(__fastcall*)(void*);
  auto applyOutput = reinterpret_cast<NativeAppFn>(
      reinterpret_cast<std::uint8_t*>(mmd) + kMmdApplyOutputRva);
  auto postDeviceReset = reinterpret_cast<NativeAppFn>(
      reinterpret_cast<std::uint8_t*>(mmd) + kMmdPostDeviceResetRva);
  applyOutput(app);

  auto*& rendererDeviceSlot =
      mmdField<IDirect3DDevice9*>(renderer, kRendererDevice);
  IDirect3DDevice9* resetDevice = device_ ? device_ : (
      nativeCaptureRestore_.device ? nativeCaptureRestore_.device : rendererDeviceSlot);
  if (resetDevice)
    rendererDeviceSlot = resetDevice;
  nativeCaptureResetInProgress_.store(true, std::memory_order_release);
  postDeviceReset(app);
  nativeCaptureResetInProgress_.store(false, std::memory_order_release);
  if (device_)
    rendererDeviceSlot = device_;
  else if (nativeCaptureRestore_.device)
    rendererDeviceSlot = nativeCaptureRestore_.device;

  capture_.setDevice(device_);
  refreshBackbufferSize();
  if (nativeCaptureWindow_ && IsWindow(nativeCaptureWindow_))
    DestroyWindow(nativeCaptureWindow_);
  nativeCaptureWindow_ = nullptr;
  nativeCaptureRestore_ = {};
  nativeCaptureState_.store(0, std::memory_order_release);
  mmdOutputSizeConfigured_ = false;
  mmdOutputFaceSize_ = 0;
  Diagnostics::instance().info("Native capture surface released; MMD output restored");
}

'''

s = s[:start] + new_body + s[rest_end:]
p.write_text(s, encoding="utf-8")
print("activate/restore rewritten")
