from pathlib import Path
import re

# --- PluginHost.hpp: public query ---
hp = Path(r"src/plugin/PluginHost.hpp")
h = hp.read_text(encoding="utf-8")
if "nativeCaptureSurfaceActive" not in h:
    h = h.replace(
        "  bool adjustPresentRects(const RECT*& source, const RECT*& dest,\n"
        "                          RECT& localSource, RECT& localDest);\n",
        "  bool adjustPresentRects(const RECT*& source, const RECT*& dest,\n"
        "                          RECT& localSource, RECT& localDest);\n"
        "  bool nativeCaptureSurfaceActive() const {\n"
        "    return nativeCaptureState_.load(std::memory_order_acquire) == 3;\n"
        "  }\n",
        1,
    )
    hp.write_text(h, encoding="utf-8")

# --- DeviceHooks: skip DownsampleCaptureSurface during native 8K ---
dh = Path(r"src/plugin/DeviceHooks.cpp")
d = dh.read_text(encoding="utf-8")
if "kMmdDownsampleCaptureRva" not in d:
    if "#include <atomic>" in d and "#include <cstring>" not in d:
        d = d.replace("#include <atomic>\n", "#include <atomic>\n#include <cstring>\n", 1)

    d = d.replace(
        "static MmdPrepareSpriteOverlay_t g_MmdPrepareSpriteOverlay = nullptr;\n"
        "static bool g_hooksInstalled = false;\n"
        "static bool g_mmdSpriteOverlayHookInstalled = false;\n",
        "static MmdPrepareSpriteOverlay_t g_MmdPrepareSpriteOverlay = nullptr;\n"
        "using MmdDownsampleCapture_t = unsigned long (*)(void*);\n"
        "static MmdDownsampleCapture_t g_MmdDownsampleCapture = nullptr;\n"
        "static bool g_hooksInstalled = false;\n"
        "static bool g_mmdSpriteOverlayHookInstalled = false;\n"
        "static bool g_mmdDownsampleHookInstalled = false;\n",
        1,
    )

    d = d.replace(
        "constexpr uintptr_t kMmdPrepareSpriteOverlayRva = 0x274F0;\n"
        "constexpr uintptr_t kMmdPlaybackActiveOff = 0x368;\n"
        "constexpr unsigned char kMmdPrepareSpriteOverlaySig[] = {\n"
        "    0x40, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41,\n"
        "    0x57, 0x48, 0x8D, 0xAC, 0x24, 0x30, 0xE9, 0xFF};\n",
        "constexpr uintptr_t kMmdPrepareSpriteOverlayRva = 0x274F0;\n"
        "constexpr uintptr_t kMmdPlaybackActiveOff = 0x368;\n"
        "constexpr unsigned char kMmdPrepareSpriteOverlaySig[] = {\n"
        "    0x40, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41,\n"
        "    0x57, 0x48, 0x8D, 0xAC, 0x24, 0x30, 0xE9, 0xFF};\n"
        "// DownsampleCaptureSurface: 8192^2 system-mem readback every recorder frame.\n"
        "constexpr uintptr_t kMmdDownsampleCaptureRva = 0x9F6A0;\n"
        "constexpr unsigned char kMmdDownsampleCaptureSig[] = {\n"
        "    0x4C, 0x8B, 0xDC, 0x48, 0x81, 0xEC, 0xC8, 0x00,\n"
        "    0x00, 0x00, 0x48, 0x8B, 0x05};\n",
        1,
    )

    insert_after = """  Diagnostics::instance().info(
      "Hooked MMD Local XYZ sprite producer (playback gate during capture)");
}

static void* vtable_slot(void* obj, size_t index) {
"""
    hook_fn = """  Diagnostics::instance().info(
      "Hooked MMD Local XYZ sprite producer (playback gate during capture)");
}

static unsigned long Hook_MmdDownsampleCapture(void* app) {
  // Native 8K keeps the swapchain small and reads captureSurface directly.
  // MMD's recorder downsample would allocate another face-sized SYSTEMMEM
  // surface every frame (~256MB at 8K) and burn a full CPU box filter.
  if (PluginHost::instance().nativeCaptureSurfaceActive())
    return 0;
  return g_MmdDownsampleCapture(app);
}

static void tryInstallMmdDownsampleHook() {
  if (g_mmdDownsampleHookInstalled) return;
  HMODULE mmd = GetModuleHandleW(nullptr);
  if (!mmd) return;
  auto* target = reinterpret_cast<unsigned char*>(
      reinterpret_cast<uintptr_t>(mmd) + kMmdDownsampleCaptureRva);
  if (std::memcmp(target, kMmdDownsampleCaptureSig,
                  sizeof(kMmdDownsampleCaptureSig)) != 0) {
    Diagnostics::instance().warn(
        "MMD capture downsample hook skipped (signature mismatch)");
    g_mmdDownsampleHookInstalled = true;
    return;
  }
  if (MH_CreateHook(target, &Hook_MmdDownsampleCapture,
                    reinterpret_cast<LPVOID*>(&g_MmdDownsampleCapture)) != MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    Diagnostics::instance().warn("MMD capture downsample MinHook install failed");
    g_mmdDownsampleHookInstalled = true;
    return;
  }
  g_mmdDownsampleHookInstalled = true;
  Diagnostics::instance().info(
      "Hooked MMD capture downsample (skipped during native 8K offscreen capture)");
}

static void* vtable_slot(void* obj, size_t index) {
"""
    if insert_after not in d:
        raise SystemExit("insert point missing")
    d = d.replace(insert_after, hook_fn, 1)

    d = d.replace(
        "  tryInstallMmdSpriteOverlayHook();\n",
        "  tryInstallMmdSpriteOverlayHook();\n"
        "  tryInstallMmdDownsampleHook();\n",
        1,
    )
    dh.write_text(d, encoding="utf-8")

# --- depth format from AutoDepthStencilFormat ---
pp = Path(r"src/plugin/PluginHost.cpp")
s = pp.read_text(encoding="utf-8")
if "kRendererDepthFormat" not in s:
    s = s.replace(
        "constexpr uintptr_t kRendererCaptureFormat = 0x3AA40;\n",
        "constexpr uintptr_t kRendererCaptureFormat = 0x3AA40;\n"
        "constexpr uintptr_t kRendererDepthFormat = 0x3AA18;\n",
        1,
    )
old_depth = """  D3DFORMAT depthFmt = D3DFMT_D24S8;
  if (nativeCaptureRestore_.previousDepth) {
    D3DSURFACE_DESC depthDesc{};
    if (SUCCEEDED(nativeCaptureRestore_.previousDepth->GetDesc(&depthDesc)))
      depthFmt = depthDesc.Format;
  }
"""
new_depth = """  D3DFORMAT depthFmt = mmdField<D3DFORMAT>(renderer, kRendererDepthFormat);
  if (depthFmt == D3DFMT_UNKNOWN)
    depthFmt = D3DFMT_D24S8;
  if (nativeCaptureRestore_.previousDepth) {
    D3DSURFACE_DESC depthDesc{};
    if (SUCCEEDED(nativeCaptureRestore_.previousDepth->GetDesc(&depthDesc)) &&
        depthDesc.MultiSampleType == D3DMULTISAMPLE_NONE)
      depthFmt = depthDesc.Format;
  }
"""
if old_depth in s:
    s = s.replace(old_depth, new_depth, 1)
pp.write_text(s, encoding="utf-8")

# banner
b = Path(r"src/plugin/D3D9Proxy.cpp")
bs = b.read_text(encoding="utf-8")
bs2, n = re.subn(r"build=2026-10-29[a-z]", "build=2026-10-29w", bs, count=1)
if n != 1:
    raise SystemExit(f"banner n={n}")
b.write_text(bs2, encoding="utf-8")

c = Path("CHANGELOG.txt")
t = c.read_text(encoding="utf-8")
if "downsample" not in t.lower() and "ダウンサンプル" not in t:
    t = t.replace(
        "### 修正 / Fixed\n",
        "### 修正 / Fixed\n"
        "- JA: **ネイティブ8K中の毎フレーム8Kダウンサンプルを抑止**: MMD録画用の巨大SYSTEMMEM読取をフックでスキップ\n"
        "  EN: **Skip per-frame 8K downsample during native capture**: hook out MMD's huge SYSTEMMEM recorder readback\n",
        1,
    )
    c.write_text(t, encoding="utf-8")

print("ok")
