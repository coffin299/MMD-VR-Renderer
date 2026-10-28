#include "mmdvr/CameraController.hpp"
#include "mmdvr/MathUtil.hpp"
#include <cstring>

namespace mmdvr {

void CameraController::onSetTransform(int state, const float* matrix16) {
  if (!matrix16) return;
  std::lock_guard<std::mutex> lock(mu_);
  if (overrideActive_) return; // ignore game writes while we override (hook replaces before call)
  if (state == kD3DTS_VIEW) {
    std::memcpy(observed_.view.m, matrix16, sizeof(float) * 16);
    haveView_ = true;
    Vec3 eye, target, up;
    if (decomposeView(observed_.view, eye, target, up)) {
      observed_.eye = eye;
      observed_.target = target;
      observed_.up = up;
    }
    observed_.valid = haveView_ && haveProj_;
  } else if (state == kD3DTS_PROJECTION) {
    std::memcpy(observed_.projection.m, matrix16, sizeof(float) * 16);
    haveProj_ = true;
    observed_.valid = haveView_ && haveProj_;
  }
}

void CameraController::onGetTransform(int state, float* matrix16) const {
  if (!matrix16) return;
  std::lock_guard<std::mutex> lock(mu_);
  if (!overrideActive_) return;
  if (state == kD3DTS_VIEW)
    std::memcpy(matrix16, override_.view.m, sizeof(float) * 16);
  else if (state == kD3DTS_PROJECTION)
    std::memcpy(matrix16, override_.projection.m, sizeof(float) * 16);
}

CameraState CameraController::snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  return observed_;
}

void CameraController::restore(const CameraState& state) {
  std::lock_guard<std::mutex> lock(mu_);
  observed_ = state;
}

void CameraController::beginOverride(const CameraState& state) {
  std::lock_guard<std::mutex> lock(mu_);
  override_ = state;
  overrideActive_ = true;
}

void CameraController::endOverride() {
  std::lock_guard<std::mutex> lock(mu_);
  overrideActive_ = false;
}

CameraState CameraController::overrideState() const {
  std::lock_guard<std::mutex> lock(mu_);
  return override_;
}

std::vector<CameraState> CameraController::buildFaceCameras(
    VrMode mode,
    StereoMode stereo,
    bool leftEye,
    float ipdUnits,
    int faceSize,
    float zNear,
    float zFar) const {
  CameraState base = snapshot();
  if (!base.valid) {
    base.eye = {0, 10, -30};
    base.target = {0, 10, 0};
    base.up = {0, 1, 0};
    base.zNear = zNear;
    base.zFar = zFar;
    base.valid = true;
  }
  if (stereo == StereoMode::Stereo) {
    base = offsetCameraForEye(base, ipdUnits, leftEye);
  }

  // Prefer MMD's near/far so ray-mmd depth / soft particles stay consistent.
  float zn = zNear > 0 ? zNear : 0.1f;
  float zf = zFar > 0 ? zFar : 10000.f;
  float obsN = 0.f, obsF = 0.f;
  if (extractPerspectiveNearFar(base.projection, obsN, obsF)) {
    zn = obsN;
    zf = obsF;
  }

  std::vector<CubeFace> faces;
  if (mode == VrMode::VR360) {
    faces = {CubeFace::PosX, CubeFace::NegX, CubeFace::PosY, CubeFace::NegY, CubeFace::PosZ, CubeFace::NegZ};
  } else {
    // VR180: forward hemisphere
    faces = {CubeFace::PosZ, CubeFace::NegX, CubeFace::PosX, CubeFace::PosY, CubeFace::NegY};
  }

  const Vec3 forward = normalize(sub(base.target, base.eye));
  const Vec3 up = (dot(base.up, base.up) > 1e-8f) ? base.up : Vec3{0, 1, 0};

  std::vector<CameraState> out;
  out.reserve(faces.size());
  for (auto f : faces) {
    auto cam = makeCubeFaceCamera(base.eye, forward, up, f, zn, zf, faceSize);
    cam.cubeFace = f;
    cam.nativeEyeOffsetX =
        stereo == StereoMode::Stereo ? (leftEye ? -0.5f : 0.5f) * ipdUnits : 0.f;
    out.push_back(cam);
  }
  return out;
}

} // namespace mmdvr
