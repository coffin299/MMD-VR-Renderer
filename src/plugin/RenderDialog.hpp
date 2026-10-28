#pragma once
#include "mmdvr/Types.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace mmdvr {

// Returns true if user confirmed. settings in/out.
bool ShowRenderDialog(HWND owner, RenderSettings& settings);

} // namespace mmdvr
