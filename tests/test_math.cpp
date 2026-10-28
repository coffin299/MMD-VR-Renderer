#include "test_harness.hpp"
#include "mmdvr/CameraController.hpp"
#include "mmdvr/MathUtil.hpp"
#include <cmath>

TEST(lookAt_roundtrip_eye) {
  using namespace mmdvr;
  Vec3 eye{1,2,3}, target{1,2,4}, up{0,1,0};
  Mat4 v = lookAtRH(eye, target, up);
  Vec3 e2, t2, u2;
  CHECK(decomposeView(v, e2, t2, u2));
  CHECK(std::fabs(e2.x - eye.x) < 0.05f);
  CHECK(std::fabs(e2.y - eye.y) < 0.05f);
  CHECK(std::fabs(e2.z - eye.z) < 0.05f);
}

TEST(lookAt_matches_d3d_row_major_layout) {
  using namespace mmdvr;
  // Camera looking along +X. D3D's row-major view matrix has the camera
  // backward axis in the third column, so _13=-1 and _31=+1.
  Mat4 v = lookAtRH({0, 0, 0}, {1, 0, 0}, {0, 1, 0});
  CHECK(std::fabs(v.m[2] + 1.f) < 1e-5f);
  CHECK(std::fabs(v.m[8] - 1.f) < 1e-5f);

  Vec3 eye, target, up;
  CHECK(decomposeView(v, eye, target, up));
  CHECK(std::fabs(target.x - 1.f) < 1e-5f);
  CHECK(std::fabs(target.y) < 1e-5f);
  CHECK(std::fabs(target.z) < 1e-5f);
}

TEST(stereo_offset_moves_opposite) {
  using namespace mmdvr;
  CameraState base;
  base.eye = {0,0,0}; base.target = {0,0,1}; base.up = {0,1,0};
  base.view = lookAtRH(base.eye, base.target, base.up);
  base.valid = true;
  auto L = offsetCameraForEye(base, 2.f, true);
  auto R = offsetCameraForEye(base, 2.f, false);
  // Eyes must be opposite along camera right; separation == IPD.
  CHECK(std::fabs(L.eye.x - R.eye.x) > 1.5f);
  CHECK(std::fabs(std::fabs(R.eye.x - L.eye.x) - 2.f) < 1e-3f);
  CHECK(L.eye.x * R.eye.x < 0.f);
}

TEST(cube_face_posz_follows_camera_forward) {
  using namespace mmdvr;
  Vec3 eye{10, 5, -20};
  Vec3 forward = normalize(Vec3{-1.f, 0.f, 0.f}); // look -X
  auto cam = makeCubeFaceCamera(eye, forward, {0, 1, 0}, CubeFace::PosZ, 0.1f, 1000.f, 64);
  Vec3 look = normalize(sub(cam.target, cam.eye));
  CHECK(std::fabs(look.x - forward.x) < 1e-4f);
  CHECK(std::fabs(look.y - forward.y) < 1e-4f);
  CHECK(std::fabs(look.z - forward.z) < 1e-4f);
}

TEST(native_mmd_faces_have_expected_offsets) {
  using namespace mmdvr;
  CameraController camera;
  Mat4 view = lookAtRH({0, 0, -45}, {0, 0, 0}, {0, 1, 0});
  Mat4 proj = perspectiveRH(45.f, 1.f, 0.1f, 1000.f);
  camera.onSetTransform(CameraController::kD3DTS_VIEW, view.m);
  camera.onSetTransform(CameraController::kD3DTS_PROJECTION, proj.m);

  auto faces = camera.buildFaceCameras(
      VrMode::VR180, StereoMode::Stereo, true, 0.64f, 1024, 0.1f, 1000.f);
  CHECK(faces.size() == 5);
  CHECK(faces[0].cubeFace == CubeFace::PosZ);
  CHECK(faces[1].cubeFace == CubeFace::NegX);
  CHECK(faces[2].cubeFace == CubeFace::PosX);
  CHECK(faces[3].cubeFace == CubeFace::PosY);
  CHECK(faces[4].cubeFace == CubeFace::NegY);
  CHECK(std::fabs(faces[0].nativeEyeOffsetX + 0.32f) < 1e-5f);
}
