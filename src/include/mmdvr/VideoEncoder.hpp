#pragma once
#include "Types.hpp"
#include <string>
#include <vector>

namespace mmdvr {

struct FfmpegCaps {
  std::wstring path;
  // True only after a tiny encode smoke-test succeeds (not just -encoders list).
  bool hasHevcNvenc = false;
  bool hasHevcQsv = false;
  bool hasHevcAmf = false;
  bool hasLibx265 = false;
  bool hasAv1Nvenc = false;
  bool hasAv1Qsv = false;
  bool hasAv1Amf = false;
  bool hasLibSvtAv1 = false;
  bool hasH264Nvenc = false;
  bool hasH264Qsv = false;
  bool hasH264Amf = false;
  bool listedHevcNvenc = false;
  bool listedHevcQsv = false;
  bool listedHevcAmf = false;
  bool listedLibx265 = false;
  bool listedAv1Nvenc = false;
  bool listedAv1Qsv = false;
  bool listedAv1Amf = false;
  bool listedLibSvtAv1 = false;
  bool listedH264Nvenc = false;
  bool listedH264Qsv = false;
  bool listedH264Amf = false;
  bool ok = false;
};

class VideoEncoder {
public:
  // Resolve ffmpeg: preferred -> MMD folder / UserFile aviwork / PATH / winget links
  static std::wstring resolveFfmpeg(const std::wstring& preferred);

  // Probe encoders once for the resolved ffmpeg binary.
  static FfmpegCaps probe(const std::wstring& preferred = {});

  // Resolve EncodeCodec::Auto to a concrete codec based on caps.
  // If frameW/H > 0, skip HW encoders that cannot handle that resolution.
  // Prefers H.265 (HEVC) over H.264.
  static EncodeCodec resolveCodec(EncodeCodec requested, const FfmpegCaps& caps,
                                  int frameW = 0, int frameH = 0);

  // FFmpeg -c:v ... args (includes trailing space). HEVC-first for Auto/HW.
  static std::wstring codecArgs(EncodeCodec codec);

  // True when this encoder choice is known-safe for the frame size.
  static bool codecSupportsSize(EncodeCodec codec, int frameW, int frameH);

  // Short UI name (HEVC NVENC / libx265 / …).
  static const wchar_t* codecLabel(EncodeCodec codec);

  static bool encodeSequence(
      const std::wstring& sequenceDir,
      const std::wstring& pattern, // e.g. frame_%06d.png
      const std::wstring& outMp4,
      double fps,
      const std::wstring& ffmpegPath,
      EncodeCodec codec = EncodeCodec::Auto,
      const std::wstring& audioPath = {},
      double audioStartSec = 0.0,
      std::wstring* errorOut = nullptr,
      // Set when encoder was skipped or retried, e.g. L"HEVC NVENC → libx265".
      std::wstring* fallbackOut = nullptr,
      // Actual encoded frame size (may be scaled down for HEVC Level 7.2).
      int* encodedW = nullptr,
      int* encodedH = nullptr);

  // Inject YouTube-recognizable Spherical Video metadata (Google UUID atom).
  // frameW/H = final encoded frame (stereo SBS width is already doubled).
  static bool injectVrMetadata(
      const std::wstring& inMp4,
      const std::wstring& outMp4,
      VrMode vrMode,
      StereoMode stereoMode,
      int frameW,
      int frameH,
      const std::wstring& ffmpegPath = {},
      std::wstring* errorOut = nullptr);
};

} // namespace mmdvr
