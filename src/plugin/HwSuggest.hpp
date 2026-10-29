#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <dxgi1_4.h>
#include "mmdvr/Types.hpp"
#include "UiStrings.hpp"
#include <algorithm>
#include <cstdio>
#include <string>

#pragma comment(lib, "dxgi.lib")

namespace mmdvr {

struct HwSuggest {
  int logicalCores = 4;
  unsigned long long ramMb = 8192;
  unsigned long long ramFreeMb = 4096;
  unsigned long long vramTotalMb = 0;
  unsigned long long vramFreeMb = 0;
  QualityPreset quality = QualityPreset::Balanced;
  ResolutionPreset resolution = ResolutionPreset::Res4K;
  int workerThreads = 0;
  int faceSize = 0;
  int writeThreads = 2;
  int writeQueueMax = 2;
  double fps = 60.0;
  std::wstring summary;
};

namespace {

inline void queryGpuMemory(unsigned long long& totalMb, unsigned long long& freeMb) {
  totalMb = 0;
  freeMb = 0;
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) ||
      !factory)
    return;

  unsigned long long bestTotal = 0;
  unsigned long long bestFree = 0;
  for (UINT i = 0;; ++i) {
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
      break;
    if (!adapter) continue;

    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
      adapter->Release();
      continue;
    }

    const unsigned long long dedicated =
        static_cast<unsigned long long>(desc.DedicatedVideoMemory) / (1024ull * 1024ull);
    unsigned long long freeLocal = 0;

    IDXGIAdapter3* adapter3 = nullptr;
    if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3),
                                          reinterpret_cast<void**>(&adapter3))) &&
        adapter3) {
      DXGI_QUERY_VIDEO_MEMORY_INFO info{};
      if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        if (info.Budget >= info.CurrentUsage)
          freeLocal = (info.Budget - info.CurrentUsage) / (1024ull * 1024ull);
      }
      adapter3->Release();
    }
    // Fallback when QueryVideoMemoryInfo is unavailable: treat ~55% of dedicated as free.
    if (freeLocal == 0 && dedicated > 0)
      freeLocal = (dedicated * 55ull) / 100ull;

    if (dedicated > bestTotal || (dedicated == bestTotal && freeLocal > bestFree)) {
      bestTotal = dedicated;
      bestFree = freeLocal;
    }
    adapter->Release();
  }
  factory->Release();
  totalMb = bestTotal;
  freeMb = bestFree;
}

inline void applyVramCap(HwSuggest& out) {
  // Face autofill by installed VRAM (16GB-class can push MaxTexture 8192):
  //   ≥16GB → 8192,  ~8–15GB → 4096,  smaller → tighter caps.
  const unsigned long long total = out.vramTotalMb;
  const unsigned long long freeMb = out.vramFreeMb;
  if (total == 0 && freeMb == 0) return;

  auto downgrade = [&](ResolutionPreset res, int face, QualityPreset q) {
    out.resolution = res;
    out.faceSize = face;
    out.quality = q;
  };

  // 16GB-class: allow MaxTexture-class face for 8K.
  if (total >= 15360) {
    if (out.resolution == ResolutionPreset::Res8K || out.faceSize >= 4096) {
      if (out.faceSize < 8192) out.faceSize = 8192;
      out.quality = QualityPreset::High;
    }
    if (freeMb > 0 && freeMb < 2048)
      out.faceSize = (std::min)(out.faceSize, 4096);
    if (freeMb > 0 && freeMb < 1536)
      downgrade(ResolutionPreset::Res4K, 2048, QualityPreset::Balanced);
    return;
  }

  // 8–15GB class: face 4096 is the sweet spot (not 7680).
  if (total >= 7168) {
    if (out.faceSize > 4096) out.faceSize = 4096;
    if (out.resolution == ResolutionPreset::Res8K && out.faceSize < 4096)
      out.faceSize = 4096;
    if (freeMb > 0 && freeMb < 2048)
      downgrade(ResolutionPreset::Res4K, 2048, QualityPreset::Balanced);
    return;
  }

  // Smaller GPUs: free VRAM still matters.
  const unsigned long long v = freeMb > 0 ? freeMb : total;
  if (out.faceSize > 2048) out.faceSize = 2048;
  if (out.resolution == ResolutionPreset::Res8K && v < 6144) {
    if (v >= 4096)
      downgrade(ResolutionPreset::Res4K, 2048, QualityPreset::Balanced);
    else if (v >= 2560)
      downgrade(ResolutionPreset::Res3K, 1536, QualityPreset::Balanced);
    else
      downgrade(ResolutionPreset::Res2K, 1024, QualityPreset::Fast);
  } else if (out.resolution == ResolutionPreset::Res4K && v < 3072) {
    if (v >= 2048)
      downgrade(ResolutionPreset::Res3K, 1536, QualityPreset::Balanced);
    else
      downgrade(ResolutionPreset::Res2K, 1024, QualityPreset::Fast);
  } else if (out.resolution == ResolutionPreset::Res3K && v < 1536) {
    downgrade(ResolutionPreset::Res2K, 1024, QualityPreset::Fast);
  }

  if (freeMb > 0 && freeMb < 1536) {
    out.resolution = ResolutionPreset::Res2K;
    out.faceSize = 1024;
    out.quality = QualityPreset::Fast;
    out.writeThreads = 1;
    out.writeQueueMax = 1;
  }
}

} // namespace

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
  out.ramFreeMb = ms.ullAvailPhys / (1024ull * 1024ull);
  out.workerThreads = out.logicalCores;
  out.fps = 60.0;
  queryGpuMemory(out.vramTotalMb, out.vramFreeMb);

  const auto& tr = ui::t();
  const wchar_t* tier = tr.hwSuggestMid;
  // Primary tier from installed RAM + cores, then clamp by free VRAM.
  if (out.ramMb >= 48 && out.logicalCores >= 12) {
    out.quality = QualityPreset::High;
    out.resolution = ResolutionPreset::Res8K;
    // Face from VRAM: 16GB→8192, 8GB-class→4096 (applyVramCap refines).
    out.faceSize = (out.vramTotalMb >= 15360) ? 8192 : 4096;
    out.writeThreads = (std::min)(6, (std::max)(2, out.logicalCores / 4));
    out.writeQueueMax = (out.ramMb >= 48) ? 6 : 4;
    tier = tr.hwSuggestHigh;
  } else if (out.vramTotalMb >= 15360 && out.ramMb >= 32 && out.logicalCores >= 8) {
    // Strong GPU even if RAM tier is mid-high.
    out.quality = QualityPreset::High;
    out.resolution = ResolutionPreset::Res8K;
    out.faceSize = 8192;
    out.writeThreads = (std::min)(6, (std::max)(2, out.logicalCores / 4));
    out.writeQueueMax = (out.ramMb >= 48) ? 6 : 4;
    tier = tr.hwSuggestHigh;
  } else if (out.vramTotalMb >= 7168 && out.ramMb >= 24 && out.logicalCores >= 8) {
    out.quality = QualityPreset::High;
    out.resolution = ResolutionPreset::Res8K;
    out.faceSize = 4096;
    out.writeThreads = (std::min)(4, (std::max)(2, out.logicalCores / 4));
    out.writeQueueMax = 4;
    tier = tr.hwSuggestMidHigh;
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

  const ResolutionPreset beforeVram = out.resolution;
  applyVramCap(out);
  if (out.resolution != beforeVram) {
    // Retarget tier label after VRAM clamp.
    if (out.resolution == ResolutionPreset::Res8K) tier = tr.hwSuggestHigh;
    else if (out.resolution == ResolutionPreset::Res4K) tier = tr.hwSuggestMidHigh;
    else if (out.resolution == ResolutionPreset::Res3K) tier = tr.hwSuggestMid;
    else tier = tr.hwSuggestLight;
  }

  // Low free system RAM → smaller PNG queue (only when actually tight).
  if (out.ramFreeMb > 0 && out.ramFreeMb < 12288)
    out.writeQueueMax = (std::min)(out.writeQueueMax, 4);
  if (out.ramFreeMb > 0 && out.ramFreeMb < 6144)
    out.writeQueueMax = (std::min)(out.writeQueueMax, 2);
  if (out.ramFreeMb > 0 && out.ramFreeMb < 3072)
    out.writeQueueMax = 1;

  const unsigned ramGb = (unsigned)(out.ramMb / 1024ull);
  const unsigned vramFreeGb = (unsigned)((out.vramFreeMb + 512ull) / 1024ull); // round
  wchar_t detail[320];
  std::swprintf(detail, 320, tr.hwSuggestDetailFmt, out.logicalCores, ramGb, vramFreeGb, tier);
  out.summary = detail;
  return out;
}

} // namespace mmdvr
