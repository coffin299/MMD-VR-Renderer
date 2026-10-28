#include "test_harness.hpp"
#include "mmdvr/StereoEngine.hpp"
#include "mmdvr/ProjectionEngine.hpp"

TEST(sbs_doubles_width) {
  auto faces = mmdvr::ProjectionEngine::makeTestCubemap(8);
  auto L = mmdvr::ProjectionEngine::cubemapToEquirect180(faces, 64, 64);
  auto R = L;
  auto sbs = mmdvr::StereoEngine::composeSideBySide(L, R);
  CHECK(sbs.width == 128);
  CHECK(sbs.height == 64);
}
