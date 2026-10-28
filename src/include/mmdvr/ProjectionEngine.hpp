#pragma once
#include "Types.hpp"
#include <array>

namespace mmdvr {

class ProjectionEngine {
public:
  // Convert 6 cubemap faces (RGBA) into equirectangular VR360.
  // bilinear=false → nearest (Fast quality): much cheaper on huge outputs.
  static ImageBuffer cubemapToEquirect(
      const std::array<ImageBuffer, 6>& faces,
      int outW, int outH,
      bool bilinear = true);

  // Convert forward-facing cubemap subset into VR180 equirect (longitude ±90).
  static ImageBuffer cubemapToEquirect180(
      const std::array<ImageBuffer, 6>& faces,
      int outW, int outH,
      bool bilinear = true);

  static void sampleCubemap(
      const std::array<ImageBuffer, 6>& faces,
      float dx, float dy, float dz,
      uint8_t outRgba[4],
      bool bilinear = true);

  // Generate synthetic face colors for unit tests.
  static std::array<ImageBuffer, 6> makeTestCubemap(int faceSize);
};

} // namespace mmdvr
