from pathlib import Path

# --- DeviceHooks: call onAfterPresent after Present returns ---
dh = Path(r"src/plugin/DeviceHooks.cpp")
s = dh.read_text(encoding="utf-8")
old = """static HRESULT WINAPI Hook_Present(IDirect3DDevice9* self, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
  ++g_presentDepth;
  if (g_presentDepth == 1)
    PluginHost::instance().onBeforePresent(self);
  HRESULT hr = g_Present(self, a, b, c, d);
  --g_presentDepth;
  return hr;
}"""
new = """static HRESULT WINAPI Hook_Present(IDirect3DDevice9* self, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
  ++g_presentDepth;
  if (g_presentDepth == 1)
    PluginHost::instance().onBeforePresent(self);
  HRESULT hr = g_Present(self, a, b, c, d);
  if (g_presentDepth == 1)
    PluginHost::instance().onAfterPresent(self);
  --g_presentDepth;
  return hr;
}"""
if old not in s:
    raise SystemExit("Hook_Present missing")
dh.write_text(s.replace(old, new, 1), encoding="utf-8")
print("DeviceHooks: onAfterPresent")

# --- PluginHost.hpp ---
hp = Path(r"src/plugin/PluginHost.hpp")
s = hp.read_text(encoding="utf-8")
if "void onAfterPresent" not in s:
    s = s.replace(
        "  void onBeforePresent(IDirect3DDevice9* device);\n",
        "  void onBeforePresent(IDirect3DDevice9* device);\n"
        "  // Runs after Present returns — safe point for device Reset (MME).\n"
        "  void onAfterPresent(IDirect3DDevice9* device);\n",
        1,
    )
    hp.write_text(s, encoding="utf-8")
    print("hpp: onAfterPresent")
else:
    print("hpp: already has onAfterPresent")

# --- PluginHost.cpp: move request processing to after Present ---
pp = Path(r"src/plugin/PluginHost.cpp")
s = pp.read_text(encoding="utf-8")

old = """  // Reset/recreate only outside FrameDriver, on the Present path so MME's
  // hooked device participates and the current frame is not half-reset.
  const int nativeStateBefore = nativeCaptureState_.load(std::memory_order_acquire);
  processNativeCaptureSurfaceRequests();
  const int nativeStateAfter = nativeCaptureState_.load(std::memory_order_acquire);
  if (nativeStateBefore != nativeStateAfter &&
      (nativeStateAfter == 3 || nativeStateAfter == -1 || nativeStateAfter == 0)) {
    // This Present was interrupted by Reset. Do not capture it; the job worker
    // continues after MME has recreated resources on the next paint.
    if (mmdHwnd_ && IsWindow(mmdHwnd_)) {
      InvalidateRect(mmdHwnd_, nullptr, FALSE);
      PostMessageW(mmdHwnd_, WM_PAINT, 0, 0);
    }
    return;
  }

  if (wantReleasePool_.exchange(false))
    capture_.releasePool();
"""
new = """  if (wantReleasePool_.exchange(false))
    capture_.releasePool();
"""
if old not in s:
    raise SystemExit("onBeforePresent native block missing")
s = s.replace(old, new, 1)

# Insert onAfterPresent just before onBeforePresent definition
marker = "void PluginHost::onBeforePresent(IDirect3DDevice9* device) {"
after_fn = """void PluginHost::onAfterPresent(IDirect3DDevice9* device) {
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

"""
if "void PluginHost::onAfterPresent" not in s:
    if marker not in s:
        raise SystemExit("onBeforePresent def missing")
    s = s.replace(marker, after_fn + marker, 1)
else:
    print("onAfterPresent already in cpp")

# Fix activate: RecWindow class + restore original device after Reset + CreateRT via MME
old_win = """  nativeCaptureWindow_ = CreateWindowExW(
      WS_EX_TOOLWINDOW, L"STATIC", L"MMDVR Native Capture",
      WS_POPUP, -32000, -32000, face, face, appWindow, nullptr,
      GetModuleHandleW(nullptr), nullptr);"""
new_win = """  // MME probes for a top-level window whose class name is exactly "RecWindow".
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
      GetModuleHandleW(nullptr), nullptr);"""
if old_win not in s:
    raise SystemExit("CreateWindow block missing")
s = s.replace(old_win, new_win, 1)

old_reset = """  IDirect3DDevice9* previousDevice = rendererDeviceSlot;
  rendererDeviceSlot = resetDevice;
  nativeCaptureResetInProgress_.store(true, std::memory_order_release);
  postDeviceReset(app);
  nativeCaptureResetInProgress_.store(false, std::memory_order_release);
  // Keep the device MMD will Present/Reset with as the hooked MME proxy.
  if (device_)
    rendererDeviceSlot = device_;
  else
    rendererDeviceSlot = previousDevice;
  rendererDevice = rendererDeviceSlot;"""
new_reset = """  // Force PostDeviceReset through the hooked MME proxy so its Reset
  // interceptor runs OnLostDevice → real Reset → OnResetDevice. Resetting only
  // the inner device (build r) completes then crashes with MME reload failure.
  IDirect3DDevice9* previousDevice = rendererDeviceSlot;
  rendererDeviceSlot = resetDevice;
  Diagnostics::instance().info("Native 8K capture: entering PostDeviceReset via MME proxy");
  nativeCaptureResetInProgress_.store(true, std::memory_order_release);
  postDeviceReset(app);
  nativeCaptureResetInProgress_.store(false, std::memory_order_release);
  // Restore MMD's stored pointer; CreateRenderTarget below uses device_ (MME).
  rendererDeviceSlot = previousDevice;
  rendererDevice = resetDevice ? resetDevice : previousDevice;"""
if old_reset not in s:
    raise SystemExit("reset block missing")
s = s.replace(old_reset, new_reset, 1)

# restore path: same pattern — temporary MME swap, restore after
old_restore = """  auto*& rendererDeviceSlot =
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
    rendererDeviceSlot = nativeCaptureRestore_.device;"""
new_restore = """  auto*& rendererDeviceSlot =
      mmdField<IDirect3DDevice9*>(renderer, kRendererDevice);
  IDirect3DDevice9* previousDevice = rendererDeviceSlot;
  IDirect3DDevice9* resetDevice = device_ ? device_ : (
      nativeCaptureRestore_.device ? nativeCaptureRestore_.device : rendererDeviceSlot);
  if (resetDevice)
    rendererDeviceSlot = resetDevice;
  nativeCaptureResetInProgress_.store(true, std::memory_order_release);
  postDeviceReset(app);
  nativeCaptureResetInProgress_.store(false, std::memory_order_release);
  rendererDeviceSlot = previousDevice;"""
if old_restore not in s:
    raise SystemExit("restore reset block missing")
s = s.replace(old_restore, new_restore, 1)

pp.write_text(s, encoding="utf-8")
print("PluginHost.cpp patched")

# banner
banner = Path(r"src/plugin/D3D9Proxy.cpp")
bs = banner.read_text(encoding="utf-8")
for oldb, newb in [("build=2026-10-29s", "build=2026-10-29t"),
                   ("build=2026-10-29r", "build=2026-10-29t")]:
    if oldb in bs:
        bs = bs.replace(oldb, newb)
        break
else:
    raise SystemExit("banner missing")
banner.write_text(bs, encoding="utf-8")
print("banner t")

# changelog
c = Path("CHANGELOG.txt")
t = c.read_text(encoding="utf-8")
if "Present完了後にMME" not in t:
    needle = "### 修正 / Fixed\n"
    bullet = """### 修正 / Fixed
- JA: **ネイティブ8KのMME ResetがPresent中に即落ちする**: ResetをPresent完了後へ移し、フック済みMMEプロキシ経由でのみ実行
  EN: **Native 8K MME Reset crashed inside Present**: run Reset only after Present returns, and only through the hooked MME proxy
"""
    t = t.replace(needle, bullet, 1)
    c.write_text(t, encoding="utf-8")
    print("changelog")
