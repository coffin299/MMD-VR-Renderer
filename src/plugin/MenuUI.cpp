#include "MenuUI.hpp"
#include "PluginHost.hpp"
#include "UiStrings.hpp"
#include "mmdvr/Diagnostics.hpp"

namespace mmdvr {

static WNDPROC g_oldProc = nullptr;
static HWND g_hwnd = nullptr;

static LRESULT CALLBACK SubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_COMMAND) {
    switch (LOWORD(wParam)) {
    case IDM_VR_OPEN:
      PluginHost::instance().openUi(hWnd);
      return 0;
    case IDM_VR_CAPTURE:
      PluginHost::instance().requestSingleCapture();
      return 0;
    case IDM_VR_CAMERA_PROBE:
      PluginHost::instance().runCameraProbe();
      return 0;
    case IDM_VR_ABOUT:
      MessageBoxW(hWnd, ui::t().aboutBody, ui::t().aboutTitle, MB_OK);
      return 0;
    }
  }
  return CallWindowProcW(g_oldProc, hWnd, msg, wParam, lParam);
}

struct EnumData { DWORD pid; HWND hwnd; };

static BOOL CALLBACK EnumProc(HWND hWnd, LPARAM lp) {
  auto* d = (EnumData*)lp;
  DWORD pid = 0;
  GetWindowThreadProcessId(hWnd, &pid);
  if (pid == d->pid && GetMenu(hWnd) && IsWindowVisible(hWnd)) {
    d->hwnd = hWnd;
    return FALSE;
  }
  return TRUE;
}

bool InstallMenu(HWND) {
  ui::loadLangPreference();
  if (g_hwnd && IsWindow(g_hwnd) && g_oldProc)
    return true;
  EnumData data{GetCurrentProcessId(), nullptr};
  EnumWindows(EnumProc, (LPARAM)&data);
  if (!data.hwnd) {
    Diagnostics::instance().warn("MMD main window not found for menu install");
    return false;
  }
  g_hwnd = data.hwnd;
  HMENU menu = GetMenu(g_hwnd);
  if (!menu) return false;

  HMENU sub = CreatePopupMenu();
  AppendMenuW(sub, MF_STRING, IDM_VR_OPEN, ui::t().menuOpen);
  AppendMenuW(sub, MF_STRING, IDM_VR_CAPTURE, ui::t().menuCapture);
  AppendMenuW(sub, MF_STRING, IDM_VR_CAMERA_PROBE, ui::t().menuProbe);
  AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(sub, MF_STRING, IDM_VR_ABOUT, ui::t().menuAbout);
  AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, ui::t().menuRoot);
  DrawMenuBar(g_hwnd);

  g_oldProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)SubclassProc);
  Diagnostics::instance().info("VR Renderer menu installed");
  return g_oldProc != nullptr;
}

void UninstallMenu(HWND) {
  if (g_hwnd && g_oldProc) {
    SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_oldProc);
    g_oldProc = nullptr;
  }
}

} // namespace mmdvr
