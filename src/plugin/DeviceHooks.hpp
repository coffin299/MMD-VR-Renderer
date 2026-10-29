#pragma once
#include <d3d9.h>

namespace mmdvr {

void HookDirect3D9(IDirect3D9* d3d);
void HookDevice(IDirect3DDevice9* device);

// When true, SetTransform may replace VIEW/PROJECTION (VR face capture only).
// Must stay false during normal viewing or ray-mmd shading breaks.
void SetTransformOverrideEnabled(bool on);
bool IsTransformOverrideEnabled();

// While capture is armed, force MMD 9.32's playbackActive gate so the Local
// XYZ sprite strip is not produced (same path as real playback). Pixel cleanup
// remains as a fallback if the app singleton is not ready yet.
void SetMmdLocalGizmoSuppress(bool on);

} // namespace mmdvr
