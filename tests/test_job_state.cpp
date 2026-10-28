#include "test_harness.hpp"
#include "mmdvr/JobState.hpp"
#include <filesystem>

TEST(job_state_roundtrip) {
  namespace fs = std::filesystem;
  auto dir = fs::temp_directory_path() / "mmdvr_test_job";
  fs::create_directories(dir);
  auto path = (dir / "job.json").wstring();

  mmdvr::JobStateData in;
  in.settings.vrMode = mmdvr::VrMode::VR360;
  in.settings.stereoMode = mmdvr::StereoMode::Stereo;
  in.settings.outputWidth = 1920;
  in.settings.outputHeight = 960;
  in.settings.outputDir = dir.wstring();
  in.lastCompletedFrame = 3;
  in.totalFrames = 10;
  CHECK(mmdvr::JobState::save(path, in));

  mmdvr::JobStateData out;
  CHECK(mmdvr::JobState::load(path, out));
  CHECK(out.lastCompletedFrame == 3);
  CHECK(out.settings.vrMode == mmdvr::VrMode::VR360);
  CHECK(out.settings.outputWidth == 1920);
}
