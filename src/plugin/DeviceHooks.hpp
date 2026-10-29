#pragma once
#include <d3d9.h>

namespace mmdvr {

void HookDirect3D9(IDirect3D9* d3d);
void HookDevice(IDirect3DDevice9* device);

// When true, SetTransform may replace VIEW/PROJECTION (VR face capture only).
// Must stay false during normal viewing or ray-mmd shading breaks.
void SetTransformOverrideEnabled(bool on);
bool IsTransformOverrideEnabled();

// While capture is armed, use MMD 9.32's playbackActive gate at the sprite
// producer so the Local XYZ strip is not generated (same path as playback).
// Pixel cleanup remains as a fallback on a signature mismatch.
void SetMmdLocalGizmoSuppress(bool on);

} // namespace mmdvr
