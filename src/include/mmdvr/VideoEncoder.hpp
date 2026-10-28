#pragma once
#include "Types.hpp"
#include <string>
#include <vector>

namespace mmdvr {

struct FfmpegCaps {
  std::wstring path;
  bool hasNvenc = false;
  bool hasQsv = false;
  bool hasAmf = false;
  bool ok = false;
};

class VideoEncoder {
public:
  // Resolve ffmpeg: preferred -> MMD folder / UserFile aviwork / PATH / winget links
  static std::wstring resolveFfmpeg(const std::wstring& preferred);

  // Probe encoders once for the resolved ffmpeg binary.
  static FfmpegCaps probe(const std::wstring& preferred = {});

  // Resolve EncodeCodec::Auto to a concrete codec based on caps.
  static EncodeCodec resolveCodec(EncodeCodec requested, const FfmpegCaps& caps);

  // FFmpeg -c:v ... args (includes trailing space).
  static std::wstring codecArgs(EncodeCodec codec);

  // Encode frame_%06d.png in sequenceDir to outMp4.
  // If audioPath is set, mux AAC from that file; audioStartSec seeks into the WAV
  // (use frameStart/fps so the clip matches the rendered range).
  static bool encodeSequence(
      const std::wstring& sequenceDir,
      const std::wstring& pattern, // e.g. frame_%06d.png
      const std::wstring& outMp4,
      double fps,
      const std::wstring& ffmpegPath,
      EncodeCodec codec = EncodeCodec::Auto,
      const std::wstring& audioPath = {},
      double audioStartSec = 0.0,
      std::wstring* errorOut = nullptr);

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
