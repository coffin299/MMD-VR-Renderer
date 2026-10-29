#pragma once
#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace mmdvr {

// Free bytes on the volume that contains `path` (file or directory). 0 = unknown.
inline std::uint64_t diskFreeBytes(const std::wstring& path) {
#ifdef _WIN32
  if (path.empty()) return 0;
  ULARGE_INTEGER freeBytes{};
  std::wstring root = path;
  if (root.size() >= 2 && root[1] == L':') {
    root.resize(3); // "G:\"
    if (root.back() != L'\\' && root.back() != L'/') root.push_back(L'\\');
  } else {
    wchar_t full[MAX_PATH]{};
    DWORD n = GetFullPathNameW(path.c_str(), MAX_PATH, full, nullptr);
    if (n == 0 || n >= MAX_PATH) return 0;
    if (full[0] && full[1] == L':') {
      root = std::wstring(full, 3);
    } else {
      return 0;
    }
  }
  if (!GetDiskFreeSpaceExW(root.c_str(), &freeBytes, nullptr, nullptr)) return 0;
  return static_cast<std::uint64_t>(freeBytes.QuadPart);
#else
  (void)path;
  return 0;
#endif
}

inline double bytesToGiB(std::uint64_t bytes) {
  return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
}

// Capture/encode headroom: pause or refuse before the volume is completely full.
inline constexpr std::uint64_t kMinFreeBytesCapture = 2ull << 30; // 2 GiB
inline constexpr std::uint64_t kMinFreeBytesEncode = 4ull << 30;  // 4 GiB

} // namespace mmdvr
