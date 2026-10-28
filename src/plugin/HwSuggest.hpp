#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include "mmdvr/Types.hpp"
#include "UiStrings.hpp"
#include <algorithm>
#include <cstdio>
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

  const auto& tr = ui::t();
  // Rough tiers from RAM (+cores). GPU VRAM is hard without DXGI; RAM is a good proxy for 8K frames.
  const wchar_t* tier = tr.hwSuggestMid;
  if (out.ramMb >= 48 && out.logicalCores >= 12) {
    out.quality = QualityPreset::High;
    out.resolution = ResolutionPreset::Res8K;
    out.faceSize = 4096;
    out.writeThreads = (std::min)(6, (std::max)(2, out.logicalCores / 4));
    out.writeQueueMax = 4;
    tier = tr.hwSuggestHigh;
  } else if (out.ramMb >= 24 && out.logicalCores >= 8) {
    out.quality = QualityPreset::Balanced;
    out.resolution = ResolutionPreset::Res4K;
    out.faceSize = 2048;
    out.writeThreads = (std::min)(4, (std::max)(2, out.logicalCores / 4));
    out.writeQueueMax = 3;
    tier = tr.hwSuggestMidHigh;
  } else if (out.ramMb >= 12) {
    out.quality = QualityPreset::Balanced;
    out.resolution = ResolutionPreset::Res3K;
    out.faceSize = 1536;
    out.writeThreads = 2;
    out.writeQueueMax = 2;
    tier = tr.hwSuggestMid;
  } else {
    out.quality = QualityPreset::Fast;
    out.resolution = ResolutionPreset::Res2K;
    out.faceSize = 1024;
    out.writeThreads = 1;
    out.writeQueueMax = 1;
    tier = tr.hwSuggestLight;
  }

  // Use %u (not %llu) so MSVC swprintf matches the same way in EN/JA builds.
  const unsigned ramGb = (unsigned)(out.ramMb / 1024ull);
  wchar_t detail[256];
  std::swprintf(detail, 256, tr.hwSuggestDetailFmt, out.logicalCores, ramGb, tier);
  out.summary = detail;
  return out;
}

} // namespace mmdvr
