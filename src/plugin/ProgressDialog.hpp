#pragma once
#include "mmdvr/RenderController.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace mmdvr {

// Modeless progress UI. Blocks (message loop) until render finishes.
// Returns true if completed successfully (not cancelled / not error).
bool RunProgressDialog(HWND owner, RenderController& controller);

} // namespace mmdvr
