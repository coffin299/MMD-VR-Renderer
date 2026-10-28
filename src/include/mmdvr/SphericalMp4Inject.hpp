#pragma once
#include "Types.hpp"
#include <string>

namespace mmdvr {

// Inject Google Spherical Video V1 UUID metadata so YouTube recognizes VR360/VR180.
// frameW/frameH = final encoded frame size (stereo SBS = 2× eye width).
bool injectSphericalMp4(
    const std::wstring& inMp4,
    const std::wstring& outMp4,
    VrMode vrMode,
    StereoMode stereoMode,
    int frameW,
    int frameH,
    std::wstring* errorOut = nullptr);

} // namespace mmdvr
