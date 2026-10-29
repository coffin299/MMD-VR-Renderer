#include "mmdvr/JobState.hpp"
#include "mmdvr/Diagnostics.hpp"
#include "mmdvr/ImageSequenceWriter.hpp"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace mmdvr {

using json = nlohmann::json;

static std::string narrow(const std::wstring& w) {
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
static std::wstring widen(const std::string& s) {
#ifdef _WIN32
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
  return w;
#else
  return std::wstring(s.begin(), s.end());
#endif
}

std::wstring JobState::defaultPath(const std::wstring& outputDir) {
  return outputDir + L"\\job.json";
}

bool JobState::save(const std::wstring& path, const JobStateData& data) {
  json j;
  j["version"] = data.version;
  j["lastCompletedFrame"] = data.lastCompletedFrame;
  j["totalFrames"] = data.totalFrames;
  j["finished"] = data.finished;
  j["settings"] = {
    {"vrMode", (int)data.settings.vrMode},
    {"stereoMode", (int)data.settings.stereoMode},
    {"quality", (int)data.settings.quality},
    {"resolution", (int)data.settings.resolution},
    {"outputWidth", data.settings.outputWidth},
    {"outputHeight", data.settings.outputHeight},
    {"faceSize", data.settings.faceSize},
    {"fps", data.settings.fps},
    {"frameStart", data.settings.frameStart},
    {"frameEnd", data.settings.frameEnd},
    {"ipdUnits", data.settings.ipdUnits},
    {"outputDir", narrow(data.settings.outputDir)},
    {"encodeVideo", data.settings.encodeVideo},
    {"encodeCodec", (int)data.settings.encodeCodec},
    {"cubemapFaces", data.settings.cubemapFaces},
    {"audioPath", narrow(data.settings.audioPath)},
    {"jobName", narrow(data.settings.jobName)},
    {"workerThreads", data.settings.workerThreads},
    {"writeThreads", data.settings.writeThreads},
    {"writeQueueMax", data.settings.writeQueueMax},
    {"sequenceFormat", (int)data.settings.sequenceFormat}
  };
  std::ofstream f(narrow(path), std::ios::binary);
  if (!f) {
    Diagnostics::instance().error("Failed to write job.json");
    return false;
  }
  f << j.dump(2);
  return true;
}

bool JobState::load(const std::wstring& path, JobStateData& out) {
  std::ifstream f(narrow(path), std::ios::binary);
  if (!f) return false;
  json j;
  try { f >> j; }
  catch (...) {
    Diagnostics::instance().error("Invalid job.json");
    return false;
  }
  out.version = j.value("version", "1");
  out.lastCompletedFrame = j.value("lastCompletedFrame", -1);
  out.totalFrames = j.value("totalFrames", 0);
  out.finished = j.value("finished", false);
  if (j.contains("settings")) {
    auto& s = j["settings"];
    out.settings.vrMode = (VrMode)s.value("vrMode", 0);
    out.settings.stereoMode = (StereoMode)s.value("stereoMode", 1);
    out.settings.quality = (QualityPreset)s.value("quality", 0);
    out.settings.resolution = (ResolutionPreset)s.value("resolution", (int)ResolutionPreset::Res8K);
    out.settings.applyResolutionPreset();
    // Prefer explicit sizes from job.json when present (resume / round-trip).
    if (s.contains("outputWidth"))
      out.settings.outputWidth = s.value("outputWidth", out.settings.outputWidth);
    if (s.contains("outputHeight"))
      out.settings.outputHeight = s.value("outputHeight", out.settings.outputHeight);
    if (s.contains("faceSize"))
      out.settings.faceSize = s.value("faceSize", out.settings.faceSize);
    out.settings.fps = s.value("fps", 60.0);
    out.settings.frameStart = s.value("frameStart", 0);
    out.settings.frameEnd = s.value("frameEnd", -1);
    out.settings.ipdUnits = (float)s.value("ipdUnits", 2.0);
    out.settings.outputDir = widen(s.value("outputDir", std::string{}));
    out.settings.encodeVideo = s.value("encodeVideo", true);
    out.settings.encodeCodec = (EncodeCodec)s.value("encodeCodec", (int)EncodeCodec::Auto);
    out.settings.cubemapFaces = s.value("cubemapFaces", false);
    out.settings.audioPath = widen(s.value("audioPath", std::string{}));
    out.settings.jobName = widen(s.value("jobName", std::string{"mmdvr_job"}));
    out.settings.workerThreads = s.value("workerThreads", 0);
    out.settings.writeThreads = s.value("writeThreads", 2);
    out.settings.writeQueueMax = s.value("writeQueueMax", 0);
    out.settings.sequenceFormat =
        (SequenceFormat)s.value("sequenceFormat", (int)SequenceFormat::Auto);
  }
  return true;
}

int JobState::firstMissingPngFrame(const std::wstring& pngDir, int frameStart, int frameEnd) {
  if (frameStart < 0) frameStart = 0;
  const int hardCap = (frameEnd >= frameStart) ? frameEnd : (frameStart + 200000);
  for (int f = frameStart; f <= hardCap; ++f) {
    if (!frameSequenceFileExists(pngDir, f))
      return f;
  }
  return hardCap + 1;
}

} // namespace mmdvr
