from pathlib import Path
import re

hp = Path(r"src/plugin/PluginHost.hpp")
h = hp.read_text(encoding="utf-8")
old = """    std::uint8_t resetPending = 0;
    void* recordingWindow = nullptr;"""
new = """    std::uint8_t resetPending = 0;
    std::uint8_t multisampleGate = 0;
    void* recordingWindow = nullptr;"""
if "multisampleGate" not in h:
    if old not in h:
        raise SystemExit("hpp field missing")
    hp.write_text(h.replace(old, new, 1), encoding="utf-8")

pp = Path(r"src/plugin/PluginHost.cpp")
s = pp.read_text(encoding="utf-8")

old_gate = """  const std::uint8_t gate = mmdField<std::uint8_t>(renderer, kRendererMultisampleGate);
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
"""

new_gate = """  nativeCaptureRestore_ = {};
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
"""
if old_gate in s:
    s = s.replace(old_gate, new_gate, 1)
elif "nativeCaptureRestore_.multisampleGate" not in s:
    raise SystemExit("gate block missing and not already patched")

old_bind = """  mmdField<IDirect3DSurface9*>(renderer, kRendererCaptureSurface) = captureSurface;
  mmdField<IDirect3DSurface9*>(renderer, kRendererDepthStencil) = depthSurface;
  mmdField<void*>(app, kMmdAppRecordWindow) = nativeCaptureWindow_;
"""
new_bind = """  mmdField<IDirect3DSurface9*>(renderer, kRendererCaptureSurface) = captureSurface;
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
"""
if old_bind in s:
    s = s.replace(old_bind, new_bind, 1)
elif "forcing captureSurface gate 0" not in s:
    raise SystemExit("bind block missing")

old_rest = """    mmdField<float>(renderer, kRendererAspectRatio) = nativeCaptureRestore_.aspectRatio;
    mmdField<std::uint8_t>(app, kMmdAppResetPending) = nativeCaptureRestore_.resetPending;
"""
new_rest = """    mmdField<float>(renderer, kRendererAspectRatio) = nativeCaptureRestore_.aspectRatio;
    mmdField<std::uint8_t>(app, kMmdAppResetPending) = nativeCaptureRestore_.resetPending;
    mmdField<std::uint8_t>(renderer, kRendererMultisampleGate) =
        nativeCaptureRestore_.multisampleGate;
"""
if old_rest in s and "nativeCaptureRestore_.multisampleGate;" not in s.split("restoreNativeCaptureSurface")[1][:1500]:
    s = s.replace(old_rest, new_rest, 1)
elif "kRendererMultisampleGate) =\n        nativeCaptureRestore_.multisampleGate" not in s:
    # try exact once more if restore section lacks it
    if "nativeCaptureRestore_.multisampleGate;" not in s:
        if old_rest not in s:
            raise SystemExit("restore site missing")
        s = s.replace(old_rest, new_rest, 1)

pp.write_text(s, encoding="utf-8")

b = Path(r"src/plugin/D3D9Proxy.cpp")
bs = b.read_text(encoding="utf-8")
bs2, n = re.subn(r"build=2026-10-29[a-z]", "build=2026-10-29v", bs, count=1)
if n != 1:
    raise SystemExit(f"banner n={n}")
b.write_text(bs2, encoding="utf-8")

c = Path("CHANGELOG.txt")
t = c.read_text(encoding="utf-8")
if "キャプチャ中だけ captureSurface ゲート" not in t:
    t = t.replace(
        "### 修正 / Fixed\n",
        "### 修正 / Fixed\n"
        "- JA: **MSAA有効時にネイティブ8Kが gate busy で起動しない**: キャプチャ中だけ captureSurface ゲートを0に退避／復元\n"
        "  EN: **Native 8K refused with MSAA (gate busy)**: temporarily force the captureSurface gate to 0 during capture and restore it\n",
        1,
    )
    c.write_text(t, encoding="utf-8")

print("patched ok")
