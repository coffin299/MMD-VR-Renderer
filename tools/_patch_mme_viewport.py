from pathlib import Path
import re

# --- hpp ---
hp = Path(r"src/plugin/PluginHost.hpp")
h = hp.read_text(encoding="utf-8")
if "setNativeCaptureViewport" not in h:
    h = h.replace(
        "  bool nativeCaptureSurfaceActive() const {\n"
        "    return nativeCaptureState_.load(std::memory_order_acquire) == 3;\n"
        "  }\n",
        "  bool nativeCaptureSurfaceActive() const {\n"
        "    return nativeCaptureState_.load(std::memory_order_acquire) == 3;\n"
        "  }\n"
        "  // faceSized=false → backbuffer viewport (what MME SasRefreshViewport must see).\n"
        "  // faceSized=true  → face viewport for Clear / model draw into captureSurface.\n"
        "  void bindNativeCaptureTarget(IDirect3DDevice9* device, bool faceSizedViewport);\n",
        1,
    )
    hp.write_text(h, encoding="utf-8")

# --- DeviceHooks BeginScene order ---
dh = Path(r"src/plugin/DeviceHooks.cpp")
d = dh.read_text(encoding="utf-8")
old_bs = """static HRESULT WINAPI Hook_BeginScene(IDirect3DDevice9* self) {
  HRESULT hr = g_BeginScene(self);
  if (SUCCEEDED(hr))
    PluginHost::instance().onBeginScene(self);
  return hr;
}"""
new_bs = """static HRESULT WINAPI Hook_BeginScene(IDirect3DDevice9* self) {
  // MME sizes offscreen effect RTs from GetViewport(). Keep that at the real
  // backbuffer size; expand to the face viewport only after BeginScene returns.
  if (PluginHost::instance().nativeCaptureSurfaceActive())
    PluginHost::instance().bindNativeCaptureTarget(self, false);
  HRESULT hr = g_BeginScene(self);
  if (SUCCEEDED(hr)) {
    if (PluginHost::instance().nativeCaptureSurfaceActive())
      PluginHost::instance().bindNativeCaptureTarget(self, true);
    PluginHost::instance().onBeginScene(self);
  }
  return hr;
}"""
if old_bs not in d:
    raise SystemExit("Hook_BeginScene missing")
dh.write_text(d.replace(old_bs, new_bs, 1), encoding="utf-8")

# --- PluginHost.cpp ---
pp = Path(r"src/plugin/PluginHost.cpp")
s = pp.read_text(encoding="utf-8")

# Replace onMmdAppObserved bind
old_obs = """void PluginHost::onMmdAppObserved(void* app) {
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
new_obs = """void PluginHost::bindNativeCaptureTarget(IDirect3DDevice9* device, bool faceSizedViewport) {
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
  // Re-bind capture RT and face viewport before Clear. BeginScene will briefly
  // shrink the viewport to the backbuffer so MME does not allocate 8K effect RTs.
  if (device_)
    bindNativeCaptureTarget(device_, true);
}
"""
if old_obs not in s:
    raise SystemExit("onMmdAppObserved missing")
s = s.replace(old_obs, new_obs, 1)

# Activate: do not poke screen/output sizes or refreshViewport
old_act = """  // Output/screen size drive projection + recorder viewport. Leave the real
  // swapchain PresentParameters alone so MME never sees a device Reset.
  mmdField<std::uint32_t>(app, kMmdAppOutputWidth) = static_cast<std::uint32_t>(face);
  mmdField<std::uint32_t>(app, kMmdAppOutputHeight) = static_cast<std::uint32_t>(face);
  mmdField<std::uint32_t>(renderer, kRendererScreenWidth) = static_cast<std::uint32_t>(face);
  mmdField<std::uint32_t>(renderer, kRendererScreenHeight) = static_cast<std::uint32_t>(face);
  mmdField<float>(renderer, kRendererAspectRatio) = 1.0f;
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
"""
new_act = """  // Keep screenWidth/output/presentParameters at the window backbuffer size so
  // MME's GetViewport-based effect RTs stay small. Only the offscreen capture
  // RT+depth are face-sized; cubemap projection is supplied by our SetTransform
  // override, not by RefreshMainWindowViewport.
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
                "Native capture surface active (no Reset, MME viewport=%dx%d): capture=%ux%u",
                bbW_, bbH_,
                static_cast<unsigned>(captureDesc.Width), static_cast<unsigned>(captureDesc.Height));
  Diagnostics::instance().info(msg);
  return true;
}
"""
if old_act not in s:
    raise SystemExit("activate tail missing")
s = s.replace(old_act, new_act, 1)

# Restore: don't rewrite screen/output (unchanged); still restore gate/window/surfaces
old_rest = """  if (app && renderer && mmd && mmdNativeRecorderFunctionsMatch(mmd)) {
    mmdField<void*>(app, kMmdAppRecordWindow) = nativeCaptureRestore_.recordingWindow;
    mmdField<std::uint32_t>(app, kMmdAppOutputWidth) = nativeCaptureRestore_.outputWidth;
    mmdField<std::uint32_t>(app, kMmdAppOutputHeight) = nativeCaptureRestore_.outputHeight;
    mmdField<std::uint32_t>(renderer, kRendererScreenWidth) = nativeCaptureRestore_.screenWidth;
    mmdField<std::uint32_t>(renderer, kRendererScreenHeight) = nativeCaptureRestore_.screenHeight;
    mmdField<float>(renderer, kRendererAspectRatio) = nativeCaptureRestore_.aspectRatio;
    mmdField<std::uint8_t>(app, kMmdAppResetPending) = nativeCaptureRestore_.resetPending;
    mmdField<std::uint8_t>(renderer, kRendererMultisampleGate) =
        nativeCaptureRestore_.multisampleGate;

    IDirect3DDevice9* dev = device_ ? device_ : nativeCaptureRestore_.device;
    if (dev) {
      IDirect3DSurface9* bb = nullptr;
      if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        dev->SetRenderTarget(0, bb);
        bb->Release();
      }
      if (nativeCaptureRestore_.previousDepth)
        dev->SetDepthStencilSurface(nativeCaptureRestore_.previousDepth);
    }

    using NativeAppFn = void(__fastcall*)(void*);
    auto refreshViewport = reinterpret_cast<NativeAppFn>(
        reinterpret_cast<std::uint8_t*>(mmd) + kMmdRefreshViewportRva);
    refreshViewport(app);
  }
"""
new_rest = """  if (app && renderer) {
    mmdField<void*>(app, kMmdAppRecordWindow) = nativeCaptureRestore_.recordingWindow;
    mmdField<std::uint8_t>(renderer, kRendererMultisampleGate) =
        nativeCaptureRestore_.multisampleGate;

    IDirect3DDevice9* dev = device_ ? device_ : nativeCaptureRestore_.device;
    if (dev) {
      IDirect3DSurface9* bb = nullptr;
      if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        dev->SetRenderTarget(0, bb);
        bb->Release();
      }
      if (nativeCaptureRestore_.previousDepth)
        dev->SetDepthStencilSurface(nativeCaptureRestore_.previousDepth);
      if (bbW_ > 0 && bbH_ > 0) {
        D3DVIEWPORT9 vp{};
        vp.Width = static_cast<DWORD>(bbW_);
        vp.Height = static_cast<DWORD>(bbH_);
        vp.MaxZ = 1.0f;
        dev->SetViewport(&vp);
      }
    }

    if (mmd && mmdNativeRecorderFunctionsMatch(mmd)) {
      using NativeAppFn = void(__fastcall*)(void*);
      auto refreshViewport = reinterpret_cast<NativeAppFn>(
          reinterpret_cast<std::uint8_t*>(mmd) + kMmdRefreshViewportRva);
      refreshViewport(app);
    }
  }
"""
if old_rest not in s:
    raise SystemExit("restore block missing")
s = s.replace(old_rest, new_rest, 1)

pp.write_text(s, encoding="utf-8")

b = Path(r"src/plugin/D3D9Proxy.cpp")
bs = b.read_text(encoding="utf-8")
bs2, n = re.subn(r"build=2026-10-29[a-z]", "build=2026-10-29x", bs, count=1)
if n != 1:
    raise SystemExit(f"banner {n}")
b.write_text(bs2, encoding="utf-8")

c = Path("CHANGELOG.txt")
t = c.read_text(encoding="utf-8")
if "MME viewport" not in t and "MME の作業RT" not in t:
    t = t.replace(
        "### 修正 / Fixed\n",
        "### 修正 / Fixed\n"
        "- JA: **ネイティブ8KでVRAMが16GBまで膨れる**: screenWidthを8Kにせず、MMEが見るviewportはバックバッファ寸法のまま／capture RTだけ8K\n"
        "  EN: **Native 8K blew VRAM to 16GB**: leave screenWidth alone so MME keeps backbuffer-sized viewports; only the capture RT is 8K\n",
        1,
    )
    c.write_text(t, encoding="utf-8")

print("ok")
