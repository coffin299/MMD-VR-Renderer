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
#include <sstream>
#include <iomanip>

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

bool ImageSequenceWriter::writePng(const ImageBuffer& image, const std::wstring& path) const {
  if (image.empty()) return false;
  ensureDir();
  std::string utf8 = toUtf8(path);
  // Default stb level 8 is very slow on 4K/8K VR frames; 3 is much faster with little size cost.
  stbi_write_png_compression_level = 3;
  int ok = stbi_write_png(utf8.c_str(), image.width, image.height, 4, image.rgba.data(), image.width * 4);
  if (!ok) {
    Diagnostics::instance().error("stbi_write_png failed: " + utf8);
    return false;
  }
  return true;
}

bool ImageSequenceWriter::writeFrame(const ImageBuffer& image, int frameIndex, const wchar_t* suffix) const {
  return writePng(image, framePath(frameIndex, suffix));
}

} // namespace mmdvr
