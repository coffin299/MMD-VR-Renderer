from pathlib import Path

# --- offsets + activate/restore rewrite in PluginHost.cpp ---
pp = Path(r"src/plugin/PluginHost.cpp")
s = pp.read_text(encoding="utf-8")

old_consts = """constexpr uintptr_t kRendererDevice = 0x3A9C0;
constexpr uintptr_t kRendererScreenWidth = 0x3A9C8;
constexpr uintptr_t kRendererScreenHeight = 0x3A9CC;
constexpr uintptr_t kRendererPresentParameters = 0x3A9E8;
constexpr uintptr_t kRendererCaptureSurface = 0x3AA28;
constexpr uintptr_t kRendererCaptureFormat = 0x3AA40;
constexpr uintptr_t kMmdApplyOutputRva = 0xA5EA0;
constexpr uintptr_t kMmdPostDeviceResetRva = 0xA6160;
constexpr unsigned char kMmdApplyOutputSig[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
    0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x80};
constexpr unsigned char kMmdPostDeviceResetSig[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
    0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x89};
"""
new_consts = """constexpr uintptr_t kRendererDevice = 0x3A9C0;
constexpr uintptr_t kRendererScreenWidth = 0x3A9C8;
constexpr uintptr_t kRendererScreenHeight = 0x3A9CC;
constexpr uintptr_t kRendererAspectRatio = 0x3A9D0;
constexpr uintptr_t kRendererMultisampleGate = 0x3A9E0;
constexpr uintptr_t kRendererPresentParameters = 0x3A9E8;
constexpr uintptr_t kRendererCaptureSurface = 0x3AA28;
constexpr uintptr_t kRendererBackbufferSurface = 0x3AA30;
constexpr uintptr_t kRendererDepthStencil = 0x3AA38;
constexpr uintptr_t kRendererCaptureFormat = 0x3AA40;
constexpr uintptr_t kMmdApplyOutputRva = 0xA5EA0;
constexpr uintptr_t kMmdPostDeviceResetRva = 0xA6160;
constexpr uintptr_t kMmdRefreshViewportRva = 0x216A0;
constexpr unsigned char kMmdApplyOutputSig[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
    0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x80};
constexpr unsigned char kMmdPostDeviceResetSig[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
    0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x89};
"""
if old_consts not in s:
    raise SystemExit("consts block missing")
s = s.replace(old_consts, new_consts, 1)

start = s.find("bool PluginHost::activateNativeCaptureSurface() {")
rest = s.find("void PluginHost::processNativeCaptureSurfaceRequests() {")
if start < 0 or rest < 0:
    raise SystemExit(f"markers {start} {rest}")

body = r'''bool PluginHost::activateNativeCaptureSurface() {
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

  const std::uint8_t gate = mmdField<std::uint8_t>(renderer, kRendererMultisampleGate);
  if (gate != 0) {
    Diagnostics::instance().error(
        "Native 8K capture unavailable: recorder captureSurface gate is busy");
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

  D3DFORMAT depthFmt = D3DFMT_D24S8;
  if (nativeCaptureRestore_.previousDepth) {
    D3DSURFACE_DESC depthDesc{};
    if (SUCCEEDED(nativeCaptureRestore_.previousDepth->GetDesc(&depthDesc)))
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

  // Output/screen size drive projection + recorder viewport. Leave the real
  // swapchain PresentParameters alone so MME never sees a device Reset.
  mmdField<std::uint32_t>(app, kMmdAppOutputWidth) = static_cast<std::uint32_t>(face);
  mmdField<std::uint32_t>(app, kMmdAppOutputHeight) = static_cast<std::uint32_t>(face);
  mmdField<std::uint32_t>(renderer, kRendererScreenWidth) = static_cast<std::uint32_t>(face);
  mmdField<std::uint32_t>(renderer, kRendererScreenHeight) = static_cast<std::uint32_t>(face);
  mmdField<float>(renderer, kRendererAspectRatio) = 1.0f;
  mmdField<IDirect3DSurface9*>(renderer, kRendererCaptureSurface) = captureSurface;
  mmdField<IDirect3DSurface9*>(renderer, kRendererDepthStencil) = depthSurface;
  mmdField<void*>(app, kMmdAppRecordWindow) = nativeCaptureWindow_;

  createDev->SetRenderTarget(0, captureSurface);
  createDev->SetDepthStencilSurface(depthSurface);

  using NativeAppFn = void(__fastcall*)(void*);
  auto refreshViewport = reinterpret_cast<NativeAppFn>(
      reinterpret_cast<std::uint8_t*>(mmd) + kMmdRefreshViewportRva);
  refreshViewport(app);

  capture_.setDevice(createDev);
  refreshBackbufferSize();
  mmdOutputFaceSize_ = face;
  mmdOutputSizeConfigured_ = true;

  D3DSURFACE_DESC captureDesc{};
  captureSurface->GetDesc(&captureDesc);
  char msg[220];
  std::snprintf(msg, sizeof(msg),
                "Native capture surface active (no device Reset): %ux%u depth=%ux%u outerBackbuffer=%dx%d",
                static_cast<unsigned>(captureDesc.Width), static_cast<unsigned>(captureDesc.Height),
                static_cast<unsigned>(face), static_cast<unsigned>(face), bbW_, bbH_);
  Diagnostics::instance().info(msg);
  return true;
}

void PluginHost::restoreNativeCaptureSurface() {
  void* app = nativeCaptureRestore_.app;
  void* renderer = nativeCaptureRestore_.renderer;
  HMODULE mmd = GetModuleHandleW(nullptr);

  if (renderer) {
    auto*& captureSlot = mmdField<IDirect3DSurface9*>(renderer, kRendererCaptureSurface);
    auto*& depthSlot = mmdField<IDirect3DSurface9*>(renderer, kRendererDepthStencil);
    captureSlot = nativeCaptureRestore_.previousCapture;
    depthSlot = nativeCaptureRestore_.previousDepth;
    if (nativeCaptureRestore_.ownedCapture) {
      nativeCaptureRestore_.ownedCapture->Release();
      nativeCaptureRestore_.ownedCapture = nullptr;
    }
    if (nativeCaptureRestore_.ownedDepth) {
      nativeCaptureRestore_.ownedDepth->Release();
      nativeCaptureRestore_.ownedDepth = nullptr;
    }
  }

  if (app && renderer && mmd && mmdNativeRecorderFunctionsMatch(mmd)) {
    mmdField<void*>(app, kMmdAppRecordWindow) = nativeCaptureRestore_.recordingWindow;
    mmdField<std::uint32_t>(app, kMmdAppOutputWidth) = nativeCaptureRestore_.outputWidth;
    mmdField<std::uint32_t>(app, kMmdAppOutputHeight) = nativeCaptureRestore_.outputHeight;
    mmdField<std::uint32_t>(renderer, kRendererScreenWidth) = nativeCaptureRestore_.screenWidth;
    mmdField<std::uint32_t>(renderer, kRendererScreenHeight) = nativeCaptureRestore_.screenHeight;
    mmdField<float>(renderer, kRendererAspectRatio) = nativeCaptureRestore_.aspectRatio;
    mmdField<std::uint8_t>(app, kMmdAppResetPending) = nativeCaptureRestore_.resetPending;

    IDirect3DDevice9* dev = device_ ? device_ : nativeCaptureRestore_.device;
    if (dev) {
      auto* bb = mmdField<IDirect3DSurface9*>(renderer, kRendererBackbufferSurface);
      if (!bb)
        dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
      if (bb)
        dev->SetRenderTarget(0, bb);
      if (nativeCaptureRestore_.previousDepth)
        dev->SetDepthStencilSurface(nativeCaptureRestore_.previousDepth);
    }

    using NativeAppFn = void(__fastcall*)(void*);
    auto refreshViewport = reinterpret_cast<NativeAppFn>(
        reinterpret_cast<std::uint8_t*>(mmd) + kMmdRefreshViewportRva);
    refreshViewport(app);
  }

  capture_.setDevice(device_);
  refreshBackbufferSize();
  if (nativeCaptureWindow_ && IsWindow(nativeCaptureWindow_))
    DestroyWindow(nativeCaptureWindow_);
  nativeCaptureWindow_ = nullptr;
  nativeCaptureRestore_ = {};
  nativeCaptureState_.store(0, std::memory_order_release);
  mmdOutputSizeConfigured_ = false;
  mmdOutputFaceSize_ = 0;
  Diagnostics::instance().info("Native capture surface released; MMD output restored (no Reset)");
}

'''

s = s[:start] + body + s[rest:]

# FrameDriver rebind while native capture is active
old_obs = """void PluginHost::onMmdAppObserved(void* app) {
  if (!app) return;
  mmdApp_.store(app, std::memory_order_release);
  // FrameDriver is on the stack here. Never PostDeviceReset/Reset from this
  // hook — MME loses its resources and MMD continues into a half-reset frame.
}
"""
new_obs = """void PluginHost::onMmdAppObserved(void* app) {
  if (!app) return;
  mmdApp_.store(app, std::memory_order_release);
  // Re-bind the offscreen 8K RT/depth at FrameDriver entry. Shadow passes may
  // temporarily replace RT0; with recordingWindow set they restore to
  // captureSurface, but keep this explicit so Clear hits the face target.
  if (nativeCaptureState_.load(std::memory_order_acquire) != 3) return;
  if (!nativeCaptureRestore_.ownedCapture || !device_) return;
  device_->SetRenderTarget(0, nativeCaptureRestore_.ownedCapture);
  if (nativeCaptureRestore_.ownedDepth)
    device_->SetDepthStencilSurface(nativeCaptureRestore_.ownedDepth);
}
"""
if old_obs not in s:
    raise SystemExit("onMmdAppObserved missing")
s = s.replace(old_obs, new_obs, 1)

pp.write_text(s, encoding="utf-8")
print("PluginHost.cpp activate/restore no-Reset")

# --- hpp restore fields + Present clamp helper ---
hp = Path(r"src/plugin/PluginHost.hpp")
h = hp.read_text(encoding="utf-8")
if "adjustPresentRects" not in h:
    h = h.replace(
        "  void onAfterPresent(IDirect3DDevice9* device);\n",
        "  void onAfterPresent(IDirect3DDevice9* device);\n"
        "  // When native 8K is active, Present source rects may exceed the real\n"
        "  // backbuffer; clamp them so D3D/MME do not enter device-lost Reset.\n"
        "  bool adjustPresentRects(const RECT*& source, const RECT*& dest,\n"
        "                          RECT& localSource, RECT& localDest);\n",
        1,
    )
old_restore_struct = """  struct NativeCaptureRestoreState {
    void* app = nullptr;
    void* renderer = nullptr;
    IDirect3DDevice9* device = nullptr;
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    std::uint32_t screenWidth = 0;
    std::uint32_t screenHeight = 0;
    std::uint32_t backbufferWidth = 0;
    std::uint32_t backbufferHeight = 0;
    std::uint8_t resetPending = 0;
    void* recordingWindow = nullptr;
  } nativeCaptureRestore_{};
"""
new_restore_struct = """  struct NativeCaptureRestoreState {
    void* app = nullptr;
    void* renderer = nullptr;
    IDirect3DDevice9* device = nullptr;
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    std::uint32_t screenWidth = 0;
    std::uint32_t screenHeight = 0;
    float aspectRatio = 0.f;
    std::uint32_t backbufferWidth = 0;
    std::uint32_t backbufferHeight = 0;
    std::uint8_t resetPending = 0;
    void* recordingWindow = nullptr;
    IDirect3DSurface9* previousCapture = nullptr;
    IDirect3DSurface9* previousDepth = nullptr;
    IDirect3DSurface9* ownedCapture = nullptr;
    IDirect3DSurface9* ownedDepth = nullptr;
  } nativeCaptureRestore_{};
"""
if old_restore_struct not in h:
    raise SystemExit("restore struct missing")
h = h.replace(old_restore_struct, new_restore_struct, 1)
hp.write_text(h, encoding="utf-8")
print("hpp updated")

# --- adjustPresentRects implementation + DeviceHooks ---
# Insert before onAfterPresent
s = pp.read_text(encoding="utf-8")
if "bool PluginHost::adjustPresentRects" not in s:
    marker = "void PluginHost::onAfterPresent(IDirect3DDevice9* device) {"
    fn = """bool PluginHost::adjustPresentRects(const RECT*& source, const RECT*& dest,
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

"""
    if marker not in s:
        raise SystemExit("onAfterPresent missing for insert")
    s = s.replace(marker, fn + marker, 1)
    pp.write_text(s, encoding="utf-8")
    print("adjustPresentRects added")

dh = Path(r"src/plugin/DeviceHooks.cpp")
d = dh.read_text(encoding="utf-8")
old_present = """static HRESULT WINAPI Hook_Present(IDirect3DDevice9* self, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
  ++g_presentDepth;
  if (g_presentDepth == 1)
    PluginHost::instance().onBeforePresent(self);
  HRESULT hr = g_Present(self, a, b, c, d);
  if (g_presentDepth == 1)
    PluginHost::instance().onAfterPresent(self);
  --g_presentDepth;
  return hr;
}"""
new_present = """static HRESULT WINAPI Hook_Present(IDirect3DDevice9* self, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
  ++g_presentDepth;
  if (g_presentDepth == 1)
    PluginHost::instance().onBeforePresent(self);
  RECT localA{}, localB{};
  const RECT* src = a;
  const RECT* dst = b;
  if (g_presentDepth == 1)
    PluginHost::instance().adjustPresentRects(src, dst, localA, localB);
  HRESULT hr = g_Present(self, src, dst, c, d);
  if (g_presentDepth == 1)
    PluginHost::instance().onAfterPresent(self);
  --g_presentDepth;
  return hr;
}"""
if old_present not in d:
    raise SystemExit("Hook_Present missing")
dh.write_text(d.replace(old_present, new_present, 1), encoding="utf-8")
print("Present clamp")

# banner
banner = Path(r"src/plugin/D3D9Proxy.cpp")
bs = banner.read_text(encoding="utf-8")
import re
bs2, n = re.subn(r"build=2026-10-29[a-z]", "build=2026-10-29u", bs, count=1)
if n != 1:
    raise SystemExit("banner not updated")
banner.write_text(bs2, encoding="utf-8")
print("banner u")

# changelog
c = Path("CHANGELOG.txt")
t = c.read_text(encoding="utf-8")
if "デバイスResetせずオフスクリーン" not in t:
    t = t.replace(
        "### 修正 / Fixed\n",
        "### 修正 / Fixed\n"
        "- JA: **ネイティブ8KがMME Resetで即落ちする**: スワップチェインは触らず、8192のオフスクリーンRT+Depthだけを録画スロットへ載せる\n"
        "  EN: **Native 8K instant-crashed on MME Reset**: leave the swapchain alone; attach an 8192 offscreen RT+depth to the recorder slots only\n",
        1,
    )
    c.write_text(t, encoding="utf-8")
    print("changelog")
