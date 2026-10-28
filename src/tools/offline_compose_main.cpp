#include "mmdvr/ProjectionEngine.hpp"
#include "mmdvr/StereoEngine.hpp"
#include "mmdvr/ImageSequenceWriter.hpp"
#include "mmdvr/RenderController.hpp"
#include "mmdvr/JobState.hpp"
#include "mmdvr/VideoEncoder.hpp"
#include "mmdvr/Diagnostics.hpp"
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static std::wstring widen(const std::string& s) {
  return std::wstring(s.begin(), s.end());
}

int wmain(int argc, wchar_t** argv) {
  using namespace mmdvr;
  Diagnostics::instance().setLogPath(L"mmdvr_offline.log");

  std::wstring outDir = L"dist\\sample_output";
  VrMode mode = VrMode::VR360;
  StereoMode stereo = StereoMode::Stereo;
  int face = 128;
  int w = 1024, h = 512;

  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    if (a == L"--vr180") mode = VrMode::VR180;
    else if (a == L"--vr360") mode = VrMode::VR360;
    else if (a == L"--mono") stereo = StereoMode::Mono;
    else if (a == L"--stereo") stereo = StereoMode::Stereo;
    else if (a == L"--out" && i + 1 < argc) outDir = argv[++i];
  }

  auto facesL = ProjectionEngine::makeTestCubemap(face);
  auto facesR = ProjectionEngine::makeTestCubemap(face);
  // Tint right eye slightly for visible stereo difference
  for (auto& f : facesR) {
    for (size_t i = 0; i + 3 < f.rgba.size(); i += 4) f.rgba[i] = (uint8_t)(f.rgba[i] * 0.7f);
  }

  RenderSettings s;
  s.vrMode = mode;
  s.stereoMode = stereo;
  s.outputWidth = w;
  s.outputHeight = h;
  s.outputDir = outDir;
  s.faceSize = face;
  s.encodeVideo = false;
  s.frameStart = 0;
  s.frameEnd = 0;

  ImageBuffer frame = RenderController::composeFromFaces(facesL, stereo == StereoMode::Stereo ? &facesR : nullptr, s);
  ImageSequenceWriter writer(s.pngSequenceDir());
  writer.ensureDir();
  writer.writeFrame(frame, 0);

  JobStateData js;
  js.settings = s;
  js.lastCompletedFrame = 0;
  js.totalFrames = 1;
  js.finished = true;
  JobState::save(JobState::defaultPath(outDir), js);

  std::wcout << L"Wrote " << s.pngSequenceDir() << L"\\frame_000000.png\n";
  return frame.empty() ? 1 : 0;
}
