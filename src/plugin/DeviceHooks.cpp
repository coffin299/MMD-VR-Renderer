#include "DeviceHooks.hpp"
#include "PluginHost.hpp"
#include "mmdvr/Diagnostics.hpp"
#include <atomic>
#include <cstring>

#if MMDVR_HAS_MINHOOK
#include <MinHook.h>
#endif

namespace mmdvr {

#if MMDVR_HAS_MINHOOK

using CreateDevice_t = HRESULT (WINAPI*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using Present_t = HRESULT (WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using Reset_t = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using GetRenderTargetData_t = HRESULT (WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*, IDirect3DSurface9*);
using StretchRect_t = HRESULT (WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*,
                                        IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
using SetTransform_t = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
using GetTransform_t = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, D3DMATRIX*);
using SetRenderTarget_t = HRESULT (WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using BeginScene_t = HRESULT (WINAPI*)(IDirect3DDevice9*);
using DrawPrimitive_t = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitive_t = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using MmdDrawLocalGizmo_t = void (*)();

static CreateDevice_t g_CreateDevice = nullptr;
static Present_t g_Present = nullptr;
static Reset_t g_Reset = nullptr;
static GetRenderTargetData_t g_GetRenderTargetData = nullptr;
static StretchRect_t g_StretchRect = nullptr;
static SetTransform_t g_SetTransform = nullptr;
static GetTransform_t g_GetTransform = nullptr;
static SetRenderTarget_t g_SetRenderTarget = nullptr;
static BeginScene_t g_BeginScene = nullptr;
static DrawPrimitive_t g_DrawPrimitive = nullptr;
static DrawIndexedPrimitive_t g_DrawIndexedPrimitive = nullptr;
static MmdDrawLocalGizmo_t g_MmdDrawLocalGizmo = nullptr;
static bool g_hooksInstalled = false;
static bool g_mmdGizmoHookInstalled = false;
static int g_presentDepth = 0;

// VR face capture: rewrite main-camera VIEW/PROJ for the whole Present cycle.
static std::atomic<bool> g_transformOverride{false};
static std::atomic<bool> g_suppressMmdLocalGizmo{false};

// MMD 9.32 x64 — drawLocalTransformGizmo (lower-right Local X/Y/Z icons).
// RVA is stable for this build; MMD is effectively frozen.
constexpr uintptr_t kMmdLocalGizmoRva = 0x225C0;
constexpr unsigned char kMmdLocalGizmoSig[] = {
    0x40, 0x55, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xB1,
    0x48, 0x81, 0xEC, 0xD8, 0x00, 0x00, 0x00, 0x48};

void SetTransformOverrideEnabled(bool on) {
  g_transformOverride.store(on, std::memory_order_release);
}

bool IsTransformOverrideEnabled() {
  return g_transformOverride.load(std::memory_order_acquire);
}

void SetMmdLocalGizmoSuppress(bool on) {
  g_suppressMmdLocalGizmo.store(on, std::memory_order_release);
}

static void Hook_MmdDrawLocalGizmo() {
  if (g_suppressMmdLocalGizmo.load(std::memory_order_acquire))
    return;
  g_MmdDrawLocalGizmo();
}

static void tryInstallMmdLocalGizmoHook() {
  if (g_mmdGizmoHookInstalled) return;
  HMODULE mmd = GetModuleHandleW(nullptr);
  if (!mmd) return;
  auto* target = reinterpret_cast<unsigned char*>(
      reinterpret_cast<uintptr_t>(mmd) + kMmdLocalGizmoRva);
  // MMD 9.32 x64 image is large enough that this RVA stays inside .text.
  if (std::memcmp(target, kMmdLocalGizmoSig, sizeof(kMmdLocalGizmoSig)) != 0) {
    Diagnostics::instance().warn(
        "MMD Local XYZ gizmo hook skipped (signature mismatch; pixel cleanup only)");
    g_mmdGizmoHookInstalled = true; // do not retry every Present
    return;
  }
  if (MH_CreateHook(target, &Hook_MmdDrawLocalGizmo,
                    reinterpret_cast<LPVOID*>(&g_MmdDrawLocalGizmo)) != MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    Diagnostics::instance().warn("MMD Local XYZ gizmo MinHook install failed");
    g_mmdGizmoHookInstalled = true;
    return;
  }
  g_mmdGizmoHookInstalled = true;
  Diagnostics::instance().info(
      "Hooked MMD Local XYZ gizmo draw (skip while capture armed)");
}

static void* vtable_slot(void* obj, size_t index) {
  return (*(void***)obj)[index];
}

static HRESULT WINAPI Hook_Present(IDirect3DDevice9* self, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
  ++g_presentDepth;
  if (g_presentDepth == 1)
    PluginHost::instance().onBeforePresent(self);
  HRESULT hr = g_Present(self, a, b, c, d);
  --g_presentDepth;
  return hr;
}

static HRESULT WINAPI Hook_Reset(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pp) {
  HRESULT hr = g_Reset(self, pp);
  if (SUCCEEDED(hr)) PluginHost::instance().onDeviceReset();
  return hr;
}

static HRESULT WINAPI Hook_GetRenderTargetData(
    IDirect3DDevice9* self, IDirect3DSurface9* source, IDirect3DSurface9* destination) {
  PluginHost::instance().onBeforeGetRenderTargetData(self, source);
  return g_GetRenderTargetData(self, source, destination);
}

static HRESULT WINAPI Hook_StretchRect(
    IDirect3DDevice9* self, IDirect3DSurface9* source, const RECT* sourceRect,
    IDirect3DSurface9* destination, const RECT* destinationRect, D3DTEXTUREFILTERTYPE filter) {
  PluginHost::instance().onBeforeStretchRect(self, source, destination);
  return g_StretchRect(self, source, sourceRect, destination, destinationRect, filter);
}

static HRESULT WINAPI Hook_SetRenderTarget(IDirect3DDevice9* self, DWORD index, IDirect3DSurface9* rt) {
  if (index == 0)
    PluginHost::instance().onBeforeSetRenderTarget(self, index);
  HRESULT hr = g_SetRenderTarget(self, index, rt);
  if (index == 0)
    PluginHost::instance().onSetRenderTarget(rt);
  return hr;
}

static HRESULT WINAPI Hook_BeginScene(IDirect3DDevice9* self) {
  HRESULT hr = g_BeginScene(self);
  if (SUCCEEDED(hr))
    PluginHost::instance().onBeginScene(self);
  return hr;
}

static HRESULT WINAPI Hook_SetTransform(IDirect3DDevice9* self, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) {
  if (!g_transformOverride.load(std::memory_order_acquire)) {
    PluginHost::instance().onObservedTransform(state, matrix);
    return g_SetTransform(self, state, matrix);
  }

  bool replace = false;
  D3DMATRIX out{};
  PluginHost::instance().onSetTransform(state, matrix, replace, out);
  if (replace) return g_SetTransform(self, state, &out);
  return g_SetTransform(self, state, matrix);
}

static HRESULT WINAPI Hook_GetTransform(IDirect3DDevice9* self, D3DTRANSFORMSTATETYPE state, D3DMATRIX* matrix) {
  HRESULT hr = g_GetTransform(self, state, matrix);
  if (SUCCEEDED(hr) && matrix && g_transformOverride.load(std::memory_order_acquire))
    PluginHost::instance().onGetTransform(state, matrix);
  return hr;
}

static void noteDraw() {
  if (g_transformOverride.load(std::memory_order_acquire))
    PluginHost::instance().onMainPassDraw();
}

static HRESULT WINAPI Hook_DrawPrimitive(IDirect3DDevice9* self, D3DPRIMITIVETYPE t, UINT s, UINT c) {
  noteDraw();
  return g_DrawPrimitive(self, t, s, c);
}

static HRESULT WINAPI Hook_DrawIndexedPrimitive(
    IDirect3DDevice9* self, D3DPRIMITIVETYPE t, INT b, UINT m, UINT n, UINT start, UINT count) {
  noteDraw();
  return g_DrawIndexedPrimitive(self, t, b, m, n, start, count);
}

void HookDevice(IDirect3DDevice9* device) {
  if (!device) return;
  // 16 Reset, 17 Present, 32 GetRenderTargetData, 34 StretchRect,
  // 37 SetRenderTarget, 41 BeginScene,
  // 44 SetTransform, 45 GetTransform, 81 DrawPrimitive, 82 DrawIndexedPrimitive
  void* pReset = vtable_slot(device, 16);
  void* pPresent = vtable_slot(device, 17);
  void* pGetRenderTargetData = vtable_slot(device, 32);
  void* pStretchRect = vtable_slot(device, 34);
  void* pSetRT = vtable_slot(device, 37);
  void* pBegin = vtable_slot(device, 41);
  void* pSetTransform = vtable_slot(device, 44);
  void* pGetTransform = vtable_slot(device, 45);
  void* pDrawPrim = vtable_slot(device, 81);
  void* pDrawIndexed = vtable_slot(device, 82);

  if (!g_hooksInstalled) {
    g_hooksInstalled = true;
    MH_CreateHook(pPresent, &Hook_Present, (LPVOID*)&g_Present);
    MH_CreateHook(pReset, &Hook_Reset, (LPVOID*)&g_Reset);
    MH_CreateHook(pGetRenderTargetData, &Hook_GetRenderTargetData, (LPVOID*)&g_GetRenderTargetData);
    MH_CreateHook(pStretchRect, &Hook_StretchRect, (LPVOID*)&g_StretchRect);
    MH_CreateHook(pSetRT, &Hook_SetRenderTarget, (LPVOID*)&g_SetRenderTarget);
    MH_CreateHook(pBegin, &Hook_BeginScene, (LPVOID*)&g_BeginScene);
    MH_CreateHook(pSetTransform, &Hook_SetTransform, (LPVOID*)&g_SetTransform);
    MH_CreateHook(pGetTransform, &Hook_GetTransform, (LPVOID*)&g_GetTransform);
    MH_CreateHook(pDrawPrim, &Hook_DrawPrimitive, (LPVOID*)&g_DrawPrimitive);
    MH_CreateHook(pDrawIndexed, &Hook_DrawIndexedPrimitive, (LPVOID*)&g_DrawIndexedPrimitive);
    MH_EnableHook(MH_ALL_HOOKS);
    Diagnostics::instance().info(
        "Device hooks installed (Present/Reset/GetRenderTargetData/StretchRect/SetRT/BeginScene/Set|GetTransform/Draw*)");
  }
  PluginHost::instance().onDeviceCreated(device);
}

static HRESULT WINAPI Hook_CreateDevice(
    IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND hwnd, DWORD behavior,
    D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** outDevice) {
  HRESULT hr = g_CreateDevice(self, adapter, type, hwnd, behavior, pp, outDevice);
  if (SUCCEEDED(hr) && outDevice && *outDevice) {
    HookDevice(*outDevice);
  }
  return hr;
}

void HookDirect3D9(IDirect3D9* d3d) {
  static bool once = false;
  if (once || !d3d) return;
  once = true;
  MH_Initialize();
  tryInstallMmdLocalGizmoHook();
  void* pCreate = vtable_slot(d3d, 16);
  MH_CreateHook(pCreate, &Hook_CreateDevice, (LPVOID*)&g_CreateDevice);
  MH_EnableHook(pCreate);
  Diagnostics::instance().info("Hooked IDirect3D9::CreateDevice");
}

#else

void SetTransformOverrideEnabled(bool) {}
bool IsTransformOverrideEnabled() { return false; }
void SetMmdLocalGizmoSuppress(bool) {}
void HookDirect3D9(IDirect3D9*) {
  Diagnostics::instance().error("Built without MinHook; hooks disabled");
}
void HookDevice(IDirect3DDevice9*) {}

#endif

} // namespace mmdvr
