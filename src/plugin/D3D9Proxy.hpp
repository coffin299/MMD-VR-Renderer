#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d9.h>

namespace mmdvr {

bool InitD3D9Proxy();
void ShutdownD3D9Proxy();

using Direct3DCreate9_t = IDirect3D9* (WINAPI*)(UINT);

} // namespace mmdvr
