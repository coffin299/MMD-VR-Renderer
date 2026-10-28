#include "mmdvr/StereoEngine.hpp"
#include "mmdvr/Parallel.hpp"
#include <algorithm>
#include <cstring>

namespace mmdvr {

static void blitRows(const ImageBuffer& src, ImageBuffer& out, int xOff, int yOff) {
  Parallel::forRows(src.height, [&](int y) {
    const uint8_t* srow = src.rgba.data() + (size_t)y * (size_t)src.width * 4;
    uint8_t* drow = out.rgba.data() + ((size_t)(y + yOff) * (size_t)out.width + (size_t)xOff) * 4;
    std::memcpy(drow, srow, (size_t)src.width * 4);
  });
}

ImageBuffer StereoEngine::composeSideBySide(const ImageBuffer& left, const ImageBuffer& right) {
  ImageBuffer out;
  if (left.empty() || right.empty()) return out;
  int h = std::max(left.height, right.height);
  out.resize(left.width + right.width, h);
  blitRows(left, out, 0, 0);
  blitRows(right, out, left.width, 0);
  return out;
}

ImageBuffer StereoEngine::composeTopBottom(const ImageBuffer& left, const ImageBuffer& right) {
  ImageBuffer out;
  if (left.empty() || right.empty()) return out;
  int w = std::max(left.width, right.width);
  out.resize(w, left.height + right.height);
  blitRows(left, out, 0, 0);
  blitRows(right, out, 0, left.height);
  return out;
}

float StereoEngine::defaultIpdUnits(QualityPreset) {
  return 0.65f;
}

} // namespace mmdvr
