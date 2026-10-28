#pragma once
#include "Types.hpp"
#include <string>

namespace mmdvr {

class ImageSequenceWriter {
public:
  explicit ImageSequenceWriter(std::wstring outputDir);

  bool ensureDir() const;
  std::wstring framePath(int frameIndex, const wchar_t* suffix = L"") const;
  // May pack RGBA→RGB in-place (VR frames are opaque) to shrink PNGs losslessly.
  bool writePng(ImageBuffer& image, const std::wstring& path) const;
  bool writeFrame(ImageBuffer& image, int frameIndex, const wchar_t* suffix = L"") const;
  bool writePng(const ImageBuffer& image, const std::wstring& path) const;
  bool writeFrame(const ImageBuffer& image, int frameIndex, const wchar_t* suffix = L"") const;

  const std::wstring& outputDir() const { return outputDir_; }

private:
  std::wstring outputDir_;
};

} // namespace mmdvr
