#pragma once
#include "Types.hpp"
#include <mutex>
#include <vector>

namespace mmdvr {

// Observes and optionally overrides D3D9 VIEW/PROJECTION transforms.
class CameraController {
public:
  void onSetTransform(int state /*D3DTS_*/, const float* matrix16);
  void onGetTransform(int state, float* matrix16) const;

  CameraState snapshot() const;
  void restore(const CameraState& state);

  // While override active, SetTransform for VIEW/PROJECTION is replaced.
  void beginOverride(const CameraState& state);
  void endOverride();
  bool overrideActive() const { return overrideActive_; }
  CameraState overrideState() const;

  // Build face cameras for current base eye.
  std::vector<CameraState> buildFaceCameras(
      VrMode mode,
      StereoMode stereo,
      bool leftEye,
      float ipdUnits,
      int faceSize,
      float zNear,
      float zFar) const;

  static const int kD3DTS_VIEW = 2;
  static const int kD3DTS_PROJECTION = 3;

private:
  mutable std::mutex mu_;
  CameraState observed_{};
  CameraState override_{};
  bool overrideActive_ = false;
  bool haveView_ = false;
  bool haveProj_ = false;
};

} // namespace mmdvr
