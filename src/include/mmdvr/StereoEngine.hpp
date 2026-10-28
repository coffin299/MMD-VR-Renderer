#pragma once
#include "Types.hpp"

namespace mmdvr {

class StereoEngine {
public:
  // Side-by-side: left | right. Output size = (left.w + right.w) x max(h).
  static ImageBuffer composeSideBySide(const ImageBuffer& left, const ImageBuffer& right);

  // Top-bottom: left on top, right on bottom.
  static ImageBuffer composeTopBottom(const ImageBuffer& left, const ImageBuffer& right);

  static float defaultIpdUnits(QualityPreset q);
};

} // namespace mmdvr
