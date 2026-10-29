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
#include <cstdio>
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

bool ImageSequenceWriter::preferCompactJpeg(int width, int height) {
  if (width <= 0 || height <= 0) return false;
  // ~4K mono and up: lossless PNG balloons (stereo 8K was ~80–120MB/frame).
  // High-quality JPEG targets ~50MB for stereo 8K SBS.
  const long long px = static_cast<long long>(width) * static_cast<long long>(height);
  return px >= 3840ll * 2160ll;
}

const wchar_t* ImageSequenceWriter::frameExtension(int width, int height) {
  return preferCompactJpeg(width, height) ? L".jpg" : L".png";
}

ImageSequenceWriter::ImageSequenceWriter(std::wstring outputDir) : outputDir_(std::move(outputDir)) {}

bool ImageSequenceWriter::ensureDir() const {
  std::error_code ec;
  fs::create_directories(fs::path(outputDir_), ec);
  return !ec;
}

std::wstring ImageSequenceWriter::framePath(int frameIndex, const wchar_t* suffix) const {
  // Legacy overload: assume PNG (debug dumps / unknown size).
  return framePath(frameIndex, 0, 0, suffix);
}

std::wstring ImageSequenceWriter::framePath(int frameIndex, int width, int height,
                                           const wchar_t* suffix) const {
  std::wstringstream ss;
  ss << outputDir_ << L"\\frame_" << std::setw(6) << std::setfill(L'0') << frameIndex;
  if (suffix && *suffix) ss << suffix;
  if (width > 0 && height > 0)
    ss << frameExtension(width, height);
  else
    ss << L".png";
  return ss.str();
}

bool ImageSequenceWriter::writePng(ImageBuffer& image, const std::wstring& path) const {
  if (image.empty()) return false;
  ensureDir();
  std::string utf8 = toUtf8(path);

  int comps = 4;
  int stride = image.width * 4;
  if (looksOpaqueRgba(image) && packRgbaToRgbInPlace(image)) {
    comps = 3;
    stride = image.width * 3;
  }

  const size_t px = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
  // Stronger zlib (stb default is 8). Huge frames use 9 — overnight jobs can afford it.
  stbi_write_png_compression_level = (px >= 3840ull * 2160ull) ? 9 : 8;

  const int ok = stbi_write_png(utf8.c_str(), image.width, image.height, comps,
                                image.rgba.data(), stride);
  if (!ok) {
    Diagnostics::instance().error("stbi_write_png failed: " + utf8);
    return false;
  }
  return true;
}

bool ImageSequenceWriter::writeJpeg(ImageBuffer& image, const std::wstring& path, int quality) const {
  if (image.empty()) return false;
  ensureDir();
  if (quality < 50) quality = 50;
  if (quality > 100) quality = 100;

  int comps = 4;
  if (looksOpaqueRgba(image) && packRgbaToRgbInPlace(image))
    comps = 3;
  else if (image.rgba.size() < static_cast<size_t>(image.width) * image.height * 4)
    comps = 3;

  std::string utf8 = toUtf8(path);
  const int ok = stbi_write_jpg(utf8.c_str(), image.width, image.height, comps,
                                image.rgba.data(), quality);
  if (!ok) {
    Diagnostics::instance().error("stbi_write_jpg failed: " + utf8);
    return false;
  }
  return true;
}

bool ImageSequenceWriter::writeFrame(ImageBuffer& image, int frameIndex, const wchar_t* suffix) const {
  // Debug dumps always PNG. For the main sequence: if this folder already has
  // frame_*.png (resume of an older job), keep PNG so FFmpeg sees one pattern.
  // Otherwise large frames use JPEG q92 (~50MB target).
  const bool debugDump = suffix && *suffix;
  bool compact = !debugDump && preferCompactJpeg(image.width, image.height);
  if (compact) {
    std::error_code ec;
    for (auto& ent : fs::directory_iterator(outputDir_, ec)) {
      if (!ent.is_regular_file(ec)) continue;
      auto name = ent.path().filename().wstring();
      if (name.rfind(L"frame_", 0) != 0) continue;
      auto ext = ent.path().extension().wstring();
      if (_wcsicmp(ext.c_str(), L".png") == 0) {
        compact = false; // continue existing PNG job
        break;
      }
      if (_wcsicmp(ext.c_str(), L".jpg") == 0 || _wcsicmp(ext.c_str(), L".jpeg") == 0)
        break; // already a JPEG job
    }
  }
  if (compact) {
    const auto path = framePath(frameIndex, image.width, image.height, suffix);
    static bool logged = false;
    if (!logged) {
      logged = true;
      Diagnostics::instance().info(
          "Sequence frames: JPEG q" + std::to_string(compactJpegQuality()) +
          " (compact ~50MB target for large VR). Path example: " + toUtf8(path));
    }
    return writeJpeg(image, path, compactJpegQuality());
  }
  return writePng(image, framePath(frameIndex, image.width, image.height, suffix));
}

bool ImageSequenceWriter::writePng(const ImageBuffer& image, const std::wstring& path) const {
  ImageBuffer copy = image;
  return writePng(copy, path);
}

bool ImageSequenceWriter::writeFrame(const ImageBuffer& image, int frameIndex, const wchar_t* suffix) const {
  ImageBuffer copy = image;
  return writeFrame(copy, frameIndex, suffix);
}

bool frameSequenceFileExists(const std::wstring& sequenceDir, int frameIndex) {
  wchar_t name[64];
  std::error_code ec;
  std::swprintf(name, 64, L"frame_%06d.jpg", frameIndex);
  if (fs::is_regular_file(fs::path(sequenceDir) / name, ec)) return true;
  std::swprintf(name, 64, L"frame_%06d.png", frameIndex);
  return fs::is_regular_file(fs::path(sequenceDir) / name, ec);
}

std::wstring detectFrameSequencePattern(const std::wstring& sequenceDir) {
  std::error_code ec;
  bool anyJpg = false, anyPng = false;
  if (fs::exists(sequenceDir, ec)) {
    for (auto& ent : fs::directory_iterator(sequenceDir, ec)) {
      if (!ent.is_regular_file()) continue;
      auto ext = ent.path().extension().wstring();
      auto name = ent.path().filename().wstring();
      if (name.rfind(L"frame_", 0) != 0) continue;
      if (_wcsicmp(ext.c_str(), L".jpg") == 0 || _wcsicmp(ext.c_str(), L".jpeg") == 0)
        anyJpg = true;
      else if (_wcsicmp(ext.c_str(), L".png") == 0)
        anyPng = true;
      if (anyJpg && anyPng) break;
    }
  }
  // Prefer PNG when mixed/ambiguous so resumed legacy jobs stay FFmpeg-friendly.
  if (anyPng) return L"frame_%06d.png";
  if (anyJpg) return L"frame_%06d.jpg";
  return L"frame_%06d.png";
}

} // namespace mmdvr
