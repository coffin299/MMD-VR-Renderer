#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include "mmdvr/Types.hpp"
#include "UiStrings.hpp"
#include <algorithm>
#include <string>

namespace mmdvr {

struct HwSuggest {
  int logicalCores = 4;
  unsigned long long ramMb = 8192;
  QualityPreset quality = QualityPreset::Balanced;
  ResolutionPreset resolution = ResolutionPreset::Res4K;
  int workerThreads = 0;
  int faceSize = 0;
  int writeThreads = 2;
  int writeQueueMax = 2;
  double fps = 60.0;
  std::wstring summary;
};

inline HwSuggest suggestFromThisPc() {
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof(ms);
  GlobalMemoryStatusEx(&ms);

  HwSuggest out;
  out.logicalCores = (int)si.dwNumberOfProcessors;
  if (out.logicalCores < 1) out.logicalCores = 4;
  out.ramMb = ms.ullTotalPhys / (1024ull * 1024ull);
  out.workerThreads = out.logicalCores;
  out.fps = 60.0;

  // Rough tiers from RAM (+cores). GPU VRAM is hard without DXGI; RAM is a good proxy for 8K frames.
  const bool ja = ui::isJa();
  if (out.ramMb >= 48 && out.logicalCores >= 12) {
    out.quality = QualityPreset::High;
    out.resolution = ResolutionPreset::Res8K;
    out.faceSize = 3072;
    out.writeThreads = (std::min)(6, (std::max)(2, out.logicalCores / 4));
    out.writeQueueMax = 4;
    out.summary = ja ? L"高スペック寄り → 8K / 高画質 / 面3072"
                     : L"High-end → 8K / High / face 3072";
  } else if (out.ramMb >= 24 && out.logicalCores >= 8) {
    out.quality = QualityPreset::Balanced;
    out.resolution = ResolutionPreset::Res4K;
    out.faceSize = 2048;
    out.writeThreads = (std::min)(4, (std::max)(2, out.logicalCores / 4));
    out.writeQueueMax = 3;
    out.summary = ja ? L"中〜高 → 4K / 標準 / 面2048"
                     : L"Mid-high → 4K / Balanced / face 2048";
  } else if (out.ramMb >= 12) {
    out.quality = QualityPreset::Balanced;
    out.resolution = ResolutionPreset::Res3K;
    out.faceSize = 1536;
    out.writeThreads = 2;
    out.writeQueueMax = 2;
    out.summary = ja ? L"中スペック → 3K / 標準 / 面1536"
                     : L"Mid → 3K / Balanced / face 1536";
  } else {
    out.quality = QualityPreset::Fast;
    out.resolution = ResolutionPreset::Res2K;
    out.faceSize = 1024;
    out.writeThreads = 1;
    out.writeQueueMax = 1;
    out.summary = ja ? L"軽め → 2K / 高速 / 面1024"
                     : L"Light → 2K / Fast / face 1024";
  }

  wchar_t detail[180];
  if (ja) {
    swprintf(detail, 180, L"検出: CPU %d スレッド / RAM %llu GB · %s",
             out.logicalCores, (unsigned long long)(out.ramMb / 1024), out.summary.c_str());
  } else {
    swprintf(detail, 180, L"Detected: %d CPU threads / %llu GB RAM · %s",
             out.logicalCores, (unsigned long long)(out.ramMb / 1024), out.summary.c_str());
  }
  out.summary = detail;
  return out;
}

} // namespace mmdvr
