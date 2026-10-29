#include "D3D9Proxy.hpp"
#include "DeviceHooks.hpp"
#include "mmdvr/Diagnostics.hpp"
#include <string>

namespace mmdvr {

static HMODULE g_real = nullptr;
static Direct3DCreate9_t g_Direct3DCreate9 = nullptr;
using Direct3DCreate9Ex_t = HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);
static Direct3DCreate9Ex_t g_Direct3DCreate9Ex = nullptr;

static std::wstring moduleDir() {
  HMODULE hm = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCWSTR)&InitD3D9Proxy, &hm);
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(hm, path, MAX_PATH);
  std::wstring p(path);
  auto slash = p.find_last_of(L"\\/");
  if (slash != std::wstring::npos) p.resize(slash);
  return p;
}

static HMODULE loadRealD3D9() {
  // Prefer MME chain
  std::wstring mme = moduleDir() + L"\\d3d9_mme.dll";
  HMODULE h = LoadLibraryW(mme.c_str());
  if (h) {
    Diagnostics::instance().info("Loaded d3d9_mme.dll (MME chain)");
    return h;
  }
  // System d3d9
  wchar_t sys[MAX_PATH]{};
  GetSystemDirectoryW(sys, MAX_PATH);
  std::wstring path = std::wstring(sys) + L"\\d3d9.dll";
  h = LoadLibraryW(path.c_str());
  if (h) Diagnostics::instance().info("Loaded system d3d9.dll (no MME chain)");
  return h;
}

bool InitD3D9Proxy() {
  const std::wstring dir = moduleDir();
  Diagnostics::instance().setLogPath(dir + L"\\mmd_vr_renderer.log");
  // Loud banner so "is the new DLL loaded?" is one glance in the log.
  Diagnostics::instance().infoW(L"=== MMD-VR-Renderer DLL LOADED === build=2026-10-29i faceCap=8192 dir=" + dir);
  Diagnostics::instance().info("InitD3D9Proxy");
  g_real = loadRealD3D9();
  if (!g_real) {
    Diagnostics::instance().error("Failed to load real d3d9");
    return false;
  }
  g_Direct3DCreate9 = (Direct3DCreate9_t)GetProcAddress(g_real, "Direct3DCreate9");
  g_Direct3DCreate9Ex = (Direct3DCreate9Ex_t)GetProcAddress(g_real, "Direct3DCreate9Ex");
  if (!g_Direct3DCreate9) {
    Diagnostics::instance().error("Direct3DCreate9 missing");
    return false;
  }
  return true;
}

void ShutdownD3D9Proxy() {
  if (g_real) {
    FreeLibrary(g_real);
    g_real = nullptr;
  }
}

Direct3DCreate9_t RealDirect3DCreate9() { return g_Direct3DCreate9; }
Direct3DCreate9Ex_t RealDirect3DCreate9Ex() { return g_Direct3DCreate9Ex; }

} // namespace mmdvr

using mmdvr::RealDirect3DCreate9;
using mmdvr::RealDirect3DCreate9Ex;

// Provide implementations matching d3d9.h declarations (same linkage).
IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion) {
  auto fn = RealDirect3DCreate9();
  if (!fn) return nullptr;
  IDirect3D9* d3d = fn(SDKVersion);
  if (d3d) mmdvr::HookDirect3D9(d3d);
  return d3d;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** pp) {
  auto fn = RealDirect3DCreate9Ex();
  if (!fn) return E_FAIL;
  HRESULT hr = fn(SDKVersion, pp);
  if (SUCCEEDED(hr) && pp && *pp) mmdvr::HookDirect3D9(*pp);
  return hr;
}

// MMD uses Direct3DCreate9; optional D3DPERF_* are not required for MMD/MME boot.
