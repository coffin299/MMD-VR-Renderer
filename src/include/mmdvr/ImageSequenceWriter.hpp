#pragma once
#include "Types.hpp"
#include <string>

namespace mmdvr {

class ImageSequenceWriter {
public:
  explicit ImageSequenceWriter(std::wstring outputDir,
                               SequenceFormat format = SequenceFormat::Auto);

  bool ensureDir() const;
  // Large VR frames (stereo 8K etc.) use high-quality JPEG (~50MB target);
  // smaller frames stay lossless PNG with strong zlib compression.
  static bool preferCompactJpeg(int width, int height);
  static const wchar_t* frameExtension(int width, int height,
                                       SequenceFormat format = SequenceFormat::Auto);
  static int compactJpegQuality() { return 92; }

  void setSequenceFormat(SequenceFormat format) { format_ = format; }
  SequenceFormat sequenceFormat() const { return format_; }

  std::wstring framePath(int frameIndex, const wchar_t* suffix = L"") const;
  std::wstring framePath(int frameIndex, int width, int height, const wchar_t* suffix = L"") const;

  // May pack RGBA→RGB in-place (VR frames are opaque).
  bool writePng(ImageBuffer& image, const std::wstring& path) const;
  bool writeJpeg(ImageBuffer& image, const std::wstring& path, int quality) const;
  bool writeFrame(ImageBuffer& image, int frameIndex, const wchar_t* suffix = L"") const;
  bool writePng(const ImageBuffer& image, const std::wstring& path) const;
  bool writeFrame(const ImageBuffer& image, int frameIndex, const wchar_t* suffix = L"") const;

  const std::wstring& outputDir() const { return outputDir_; }

private:
  std::wstring outputDir_;
  SequenceFormat format_ = SequenceFormat::Auto;
};

// Detect FFmpeg pattern under a frames dir (jpg preferred if present).
std::wstring detectFrameSequencePattern(const std::wstring& sequenceDir);
// True if frame_XXXXXX.png or .jpg exists.
bool frameSequenceFileExists(const std::wstring& sequenceDir, int frameIndex);

} // namespace mmdvr
