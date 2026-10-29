from pathlib import Path
import re

hp = Path(r"src/plugin/PluginHost.hpp")
h = hp.read_text(encoding="utf-8")
if "nativeCaptureMmeViewportSize" not in h:
    h = h.replace(
        "  void bindNativeCaptureTarget(IDirect3DDevice9* device, bool faceSizedViewport);\n",
        "  void bindNativeCaptureTarget(IDirect3DDevice9* device, bool faceSizedViewport);\n"
        "  bool nativeCaptureMmeViewportSize(int& outW, int& outH) const;\n",
        1,
    )
    hp.write_text(h, encoding="utf-8")
    print("hpp ok")

pp = Path(r"src/plugin/PluginHost.cpp")
s = pp.read_text(encoding="utf-8")
if "nativeCaptureMmeViewportSize" not in s:
    s = s.replace(
        "void PluginHost::bindNativeCaptureTarget(IDirect3DDevice9* device, bool faceSizedViewport) {",
        "bool PluginHost::nativeCaptureMmeViewportSize(int& outW, int& outH) const {\n"
        "  if (!nativeCaptureSurfaceActive()) return false;\n"
        "  outW = bbW_;\n"
        "  outH = bbH_;\n"
        "  return outW > 0 && outH > 0;\n"
        "}\n"
        "\n"
        "void PluginHost::bindNativeCaptureTarget(IDirect3DDevice9* device, bool faceSizedViewport) {",
        1,
    )

s = re.sub(
    r'std::snprintf\(msg, sizeof\(msg\),\s*\n\s*"Native capture surface active[^"]*",\s*\n\s*[^;]+;',
    "std::snprintf(msg, sizeof(msg),\n"
    '                "Native capture surface active (no Reset): capture=%ux%u; GetViewport lies %dx%d to MME",\n'
    "                static_cast<unsigned>(captureDesc.Width), static_cast<unsigned>(captureDesc.Height),\n"
    "                bbW_, bbH_);",
    s,
    count=1,
)
pp.write_text(s, encoding="utf-8")
print("PluginHost.cpp ok")

dh = Path(r"src/plugin/DeviceHooks.cpp")
d = dh.read_text(encoding="utf-8")
if "Hook_GetViewport" not in d:
    d = d.replace(
        "using BeginScene_t = HRESULT (WINAPI*)(IDirect3DDevice9*);\n",
        "using BeginScene_t = HRESULT (WINAPI*)(IDirect3DDevice9*);\n"
        "using GetViewport_t = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DVIEWPORT9*);\n",
        1,
    )
    d = d.replace(
        "static BeginScene_t g_BeginScene = nullptr;\n",
        "static BeginScene_t g_BeginScene = nullptr;\n"
        "static GetViewport_t g_GetViewport = nullptr;\n",
        1,
    )
    old_bs = """static HRESULT WINAPI Hook_BeginScene(IDirect3DDevice9* self) {
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
    new_bs = """static HRESULT WINAPI Hook_GetViewport(IDirect3DDevice9* self, D3DVIEWPORT9* vp) {
  HRESULT hr = g_GetViewport(self, vp);
  // MME/ray size work RTs from GetViewport. During native 8K capture the raster
  // viewport is face-sized; report the backbuffer size so effect buffers stay
  // near window resolution while geometry still fills the 8K capture RT.
  int lieW = 0, lieH = 0;
  if (SUCCEEDED(hr) && vp &&
      PluginHost::instance().nativeCaptureMmeViewportSize(lieW, lieH)) {
    if (vp->Width > static_cast<DWORD>(lieW) ||
        vp->Height > static_cast<DWORD>(lieH)) {
      vp->X = 0;
      vp->Y = 0;
      vp->Width = static_cast<DWORD>(lieW);
      vp->Height = static_cast<DWORD>(lieH);
    }
  }
  return hr;
}

static HRESULT WINAPI Hook_BeginScene(IDirect3DDevice9* self) {
  HRESULT hr = g_BeginScene(self);
  if (SUCCEEDED(hr)) {
    if (PluginHost::instance().nativeCaptureSurfaceActive())
      PluginHost::instance().bindNativeCaptureTarget(self, true);
    PluginHost::instance().onBeginScene(self);
  }
  return hr;
}"""
    if old_bs not in d:
        raise SystemExit("BeginScene block missing")
    d = d.replace(old_bs, new_bs, 1)
    d = d.replace(
        "  void* pBegin = vtable_slot(device, 41);\n"
        "  void* pSetTransform = vtable_slot(device, 44);\n",
        "  void* pBegin = vtable_slot(device, 41);\n"
        "  void* pGetViewport = vtable_slot(device, 47);\n"
        "  void* pSetTransform = vtable_slot(device, 44);\n",
        1,
    )
    d = d.replace(
        "    MH_CreateHook(pBegin, &Hook_BeginScene, (LPVOID*)&g_BeginScene);\n"
        "    MH_CreateHook(pSetTransform, &Hook_SetTransform, (LPVOID*)&g_SetTransform);\n",
        "    MH_CreateHook(pBegin, &Hook_BeginScene, (LPVOID*)&g_BeginScene);\n"
        "    MH_CreateHook(pGetViewport, &Hook_GetViewport, (LPVOID*)&g_GetViewport);\n"
        "    MH_CreateHook(pSetTransform, &Hook_SetTransform, (LPVOID*)&g_SetTransform);\n",
        1,
    )
    d = d.replace(
        "\"Device hooks installed (Present/Reset/GetRenderTargetData/StretchRect/SetRT/BeginScene/Set|GetTransform/Draw*+UP)\"",
        "\"Device hooks installed (Present/Reset/GetRenderTargetData/StretchRect/SetRT/BeginScene/GetViewport/Set|GetTransform/Draw*+UP)\"",
        1,
    )
    dh.write_text(d, encoding="utf-8")
    print("DeviceHooks ok")

hw = Path(r"src/plugin/HwSuggest.hpp")
hs = hw.read_text(encoding="utf-8")
if "applyMmeFaceCap" not in hs:
    if "#include <string>" not in hs:
        hs = hs.replace("#include <dxgi1_4.h>\n", "#include <dxgi1_4.h>\n#include <string>\n", 1)
    helper = r'''
static bool mmeDllPresentBesideExe() {
  wchar_t path[MAX_PATH]{};
  if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
  std::wstring dir(path);
  const auto slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) dir.resize(slash);
  return GetFileAttributesW((dir + L"\\d3d9_mme.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Heavy MME/ray scenes already fill a 16GB card; native 8K face RTs on top
// commonly exhaust VRAM. Prefer 4096 when MME is chained.
static void applyMmeFaceCap(HwSuggest& out) {
  if (!mmeDllPresentBesideExe()) return;
  if (out.faceSize > 4096) out.faceSize = 4096;
}

'''
    if "inline void applyVramCap" not in hs:
        raise SystemExit("applyVramCap missing")
    hs = hs.replace("inline void applyVramCap", helper + "inline void applyVramCap", 1)
    if "applyVramCap(out);" not in hs:
        raise SystemExit("applyVramCap(out) call missing")
    hs = hs.replace("applyVramCap(out);", "applyVramCap(out);\n  applyMmeFaceCap(out);")
    hw.write_text(hs, encoding="utf-8")
    print("HwSuggest ok")

b = Path(r"src/plugin/D3D9Proxy.cpp")
bs = b.read_text(encoding="utf-8")
bs2, n = re.subn(r"build=2026-10-29[a-z]", "build=2026-10-29y", bs, count=1)
if n != 1:
    raise SystemExit(f"banner {n}")
b.write_text(bs2, encoding="utf-8")

c = Path("CHANGELOG.txt")
t = c.read_text(encoding="utf-8")
if "GetViewportをバックバッファ" not in t:
    t = t.replace(
        "### 修正 / Fixed\n",
        "### 修正 / Fixed\n"
        "- JA: **8Kキャプチャ中にMMEがGetViewportで8K作業RTを再確保する**: GetViewportをバックバッファ寸法に偽る。MME連鎖時の推奨面は4096止め\n"
        "  EN: **MME re-allocated 8K work RTs from GetViewport during capture**: lie GetViewport as backbuffer size; MME-chained face suggest caps at 4096\n",
        1,
    )
    c.write_text(t, encoding="utf-8")

print("done")
