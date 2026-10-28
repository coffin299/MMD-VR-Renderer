#include "test_harness.hpp"
#include "mmdvr/ProjectionEngine.hpp"

TEST(cubemap_to_equirect_size) {
  auto faces = mmdvr::ProjectionEngine::makeTestCubemap(32);
  auto eq = mmdvr::ProjectionEngine::cubemapToEquirect(faces, 128, 64);
  CHECK(eq.width == 128);
  CHECK(eq.height == 64);
  CHECK(eq.rgba.size() == 128ull * 64ull * 4ull);
}

TEST(cubemap_to_equirect180_size) {
  auto faces = mmdvr::ProjectionEngine::makeTestCubemap(32);
  auto eq = mmdvr::ProjectionEngine::cubemapToEquirect180(faces, 256, 256);
  CHECK(eq.width == 256);
  CHECK(eq.height == 256);
}

TEST(sample_forward_hits_posz) {
  auto faces = mmdvr::ProjectionEngine::makeTestCubemap(16);
  uint8_t px[4];
  // +Z forward
  mmdvr::ProjectionEngine::sampleCubemap(faces, 0, 0, 1, px);
  CHECK(px[0] == 255 && px[1] == 0 && px[2] == 255); // PosZ magenta in makeTestCubemap
}
