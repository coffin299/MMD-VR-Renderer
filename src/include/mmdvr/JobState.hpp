#pragma once
#include "Types.hpp"
#include <string>

namespace mmdvr {

struct JobStateData {
  RenderSettings settings{};
  int lastCompletedFrame = -1;
  int totalFrames = 0;
  bool finished = false;
  std::string version = "1";
};

class JobState {
public:
  static std::wstring defaultPath(const std::wstring& outputDir);

  static bool save(const std::wstring& path, const JobStateData& data);
  static bool load(const std::wstring& path, JobStateData& out);

  // First missing frame_XXXXXX.png under pngDir (crash-safe resume point).
  // Returns frameEnd+1 when the range is complete.
  static int firstMissingPngFrame(const std::wstring& pngDir, int frameStart, int frameEnd);
};

} // namespace mmdvr
