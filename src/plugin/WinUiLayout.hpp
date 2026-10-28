#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace mmdvr::ui_layout {

// Ensure outer window size yields at least the requested client area
// (avoids clipped buttons when caption/frame or DPI eats space).
inline void FitClientSize(HWND hwnd, int clientW, int clientH) {
  if (!hwnd || clientW <= 0 || clientH <= 0) return;
  DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
  DWORD ex = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  RECT rc{0, 0, clientW, clientH};
  AdjustWindowRectEx(&rc, style, FALSE, ex);
  SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

  RECT cr{};
  GetClientRect(hwnd, &cr);
  const int gotW = cr.right - cr.left;
  const int gotH = cr.bottom - cr.top;
  if (gotW < clientW || gotH < clientH) {
    RECT outer{};
    GetWindowRect(hwnd, &outer);
    const int addW = (clientW - gotW) > 0 ? (clientW - gotW) : 0;
    const int addH = (clientH - gotH) > 0 ? (clientH - gotH) : 0;
    SetWindowPos(hwnd, nullptr, 0, 0,
                 (outer.right - outer.left) + addW,
                 (outer.bottom - outer.top) + addH,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

} // namespace mmdvr::ui_layout
