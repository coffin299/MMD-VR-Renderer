#pragma once
#include <d3d9.h>

namespace mmdvr {

void HookDirect3D9(IDirect3D9* d3d);
void HookDevice(IDirect3DDevice9* device);

// When true, SetTransform may replace VIEW/PROJECTION (VR face capture only).
// Must stay false during normal viewing or ray-mmd shading breaks.
void SetTransformOverrideEnabled(bool on);
bool IsTransformOverrideEnabled();

// Skip MMD 9.32's lower-right Local X/Y/Z model-operation gizmo draws while
// a capture is armed. Pixel cleanup remains as a fallback if the RVA hook
// cannot be installed (non-9.32 build / signature mismatch).
void SetMmdLocalGizmoSuppress(bool on);

} // namespace mmdvr
