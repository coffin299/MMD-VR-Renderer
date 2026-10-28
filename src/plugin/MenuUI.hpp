#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace mmdvr {

// Returns false while MMD's main window has not been created yet.
bool InstallMenu(HWND hwnd);
void UninstallMenu(HWND hwnd);

enum : UINT {
  IDM_VR_OPEN = 0xE101,
  IDM_VR_CAPTURE = 0xE102,
  IDM_VR_ABOUT = 0xE103,
  IDM_VR_CAMERA_PROBE = 0xE104
};

} // namespace mmdvr
