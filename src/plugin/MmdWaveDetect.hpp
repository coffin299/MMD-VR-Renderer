#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <string>

namespace mmdvr {

// Best-effort: find a .wav/.mp3/.ogg path shown in MMD's UI (loaded BGM).
// Returns empty if nothing usable is found.
std::wstring detectMmdWavePath(HWND mmdMain);

} // namespace mmdvr
