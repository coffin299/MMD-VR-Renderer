#include "D3D9Proxy.hpp"
#include "MenuUI.hpp"
#include "UiStrings.hpp"
#include "mmdvr/Diagnostics.hpp"
#include <process.h>

static unsigned __stdcall MenuThread(void*) {
  // MMD may load a PMM (and ray-mmd) for several seconds after d3d9.dll is
  // loaded. Retry until its main window exists instead of permanently losing
  // the renderer menu when the first check races startup.
  for (int attempt = 0; attempt < 120; ++attempt) {
    Sleep(500);
    if (mmdvr::InstallMenu(nullptr))
      break;
  }
  return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hModule);
    mmdvr::ui::loadLangPreference();
    mmdvr::InitD3D9Proxy();
    _beginthreadex(nullptr, 0, MenuThread, nullptr, 0, nullptr);
  } else if (reason == DLL_PROCESS_DETACH) {
    mmdvr::UninstallMenu(nullptr);
    mmdvr::ShutdownD3D9Proxy();
  }
  return TRUE;
}
