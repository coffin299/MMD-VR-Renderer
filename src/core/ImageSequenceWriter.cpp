#include "mmdvr/ImageSequenceWriter.hpp"
#include "mmdvr/Diagnostics.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <filesystem>
#else
#include <filesystem>
#endif
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <vector>

namespace fs = std::filesystem;

namespace mmdvr {

static std::string toUtf8(const std::wstring& w) {
#ifdef _WIN32
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
#else
  return std::string(w.begin(), w.end());
#endif
}

// Sample alpha; VR equirect from D3D capture is almost always opaque.
static bool looksOpaqueRgba(const ImageBuffer& image) {
  if (image.empty()) return true;
  const size_t n = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
  if (n == 0) return true;
  const size_t step = (std::max)(size_t(1), n / 4096);
  for (size_t p = 0; p < n; p += step) {
    if (image.rgba[p * 4 + 3] != 255) return false;
  }
  // Also check a few corners/center.
  const size_t corners[] = {0, n - 1, n / 2,
                            static_cast<size_t>(image.width / 2) +
                                static_cast<size_t>(image.height / 2) * static_cast<size_t>(image.width)};
  for (size_t p : corners) {
    if (p < n && image.rgba[p * 4 + 3] != 255) return false;
  }
  return true;
}

// Pack RGBA→RGB in the same buffer (destructive). Lossless for opaque frames.
static bool packRgbaToRgbInPlace(ImageBuffer& image) {
  const size_t n = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
  if (n == 0 || image.rgba.size() < n * 4) return false;
  uint8_t* p = image.rgba.data();
  for (size_t i = 0; i < n; ++i) {
    const size_t s = i * 4;
    const size_t d = i * 3;
    p[d] = p[s];
    p[d + 1] = p[s + 1];
    p[d + 2] = p[s + 2];
  }
  image.rgba.resize(n * 3);
  image.rgba.shrink_to_fit();
  return true;
}

ImageSequenceWriter::ImageSequenceWriter(std::wstring outputDir) : outputDir_(std::move(outputDir)) {}

bool ImageSequenceWriter::ensureDir() const {
  std::error_code ec;
  fs::create_directories(fs::path(outputDir_), ec);
  return !ec;
}

std::wstring ImageSequenceWriter::framePath(int frameIndex, const wchar_t* suffix) const {
  std::wstringstream ss;
  ss << outputDir_ << L"\\frame_" << std::setw(6) << std::setfill(L'0') << frameIndex;
  if (suffix && *suffix) ss << suffix;
  ss << L".png";
  return ss.str();
}

bool ImageSequenceWriter::writePng(ImageBuffer& image, const std::wstring& path) const {
  if (image.empty()) return false;
  ensureDir();
  std::string utf8 = toUtf8(path);

  int comps = 4;
  int stride = image.width * 4;
  // Opaque VR frames: RGB PNG is lossless vs RGBA and typically 20–40% smaller.
  if (looksOpaqueRgba(image) && packRgbaToRgbInPlace(image)) {
    comps = 3;
    stride = image.width * 3;
  }

  // PNG is lossless at every level — lower zlib level only trades file size
  // for encode speed. High-quality jobs were bottlenecked more on write than
  // on visual fidelity here (RGB opaque path already cuts size a lot).
  const size_t px = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
  stbi_write_png_compression_level = (px >= 8000ull * 4000ull) ? 5 : 4;

  const int ok = stbi_write_png(utf8.c_str(), image.width, image.height, comps,
                                image.rgba.data(), stride);
  if (!ok) {
    Diagnostics::instance().error("stbi_write_png failed: " + utf8);
    return false;
  }
  return true;
}

bool ImageSequenceWriter::writeFrame(ImageBuffer& image, int frameIndex, const wchar_t* suffix) const {
  return writePng(image, framePath(frameIndex, suffix));
}

bool ImageSequenceWriter::writePng(const ImageBuffer& image, const std::wstring& path) const {
  ImageBuffer copy = image;
  return writePng(copy, path);
}

bool ImageSequenceWriter::writeFrame(const ImageBuffer& image, int frameIndex, const wchar_t* suffix) const {
  ImageBuffer copy = image;
  return writeFrame(copy, frameIndex, suffix);
}

} // namespace mmdvr
