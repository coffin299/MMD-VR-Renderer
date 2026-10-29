#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <atomic>
#include <functional>

namespace mmdvr {

enum class VrMode : int {
  VR180 = 0,
  VR360 = 1
};

enum class StereoMode : int {
  Mono = 0,
  Stereo = 1
};

enum class QualityPreset : int {
  High = 0,   // default: max quality
  Balanced = 1,
  Fast = 2
};

// MP4 encoder for FFmpeg. Default is Auto (hardware H.265 when available).
// UI values keep legacy names; runtime prefers HEVC (hevc_nvenc / libx265) over H.264.
enum class EncodeCodec : int {
  Libx264 = 0, // CPU soft: libx265 → libx264
  Auto = 1,    // default: HEVC HW → libx265 → H.264 HW → libx264
  Nvenc = 2,   // hevc_nvenc → h264_nvenc → soft
  Qsv = 3,     // hevc_qsv → h264_qsv → soft
  Amf = 4      // hevc_amf → h264_amf → soft
};

// Frame sequence on disk (under outputDir\\frames).
// Auto: JPEG q92 for large frames, PNG for smaller; resume sticks to existing files.
enum class SequenceFormat : int {
  Auto = 0,
  Png = 1,
  Jpeg = 2
};

// Equirect size as multiples of 1920×960 (≈1920×1080 at 2:1 VR aspect).
// Stereo SBS doubles the final width.
enum class ResolutionPreset : int {
  Res2K = 0,  // 3840×1920
  Res3K = 1,  // 5760×2880
  Res4K = 2,  // 7680×3840
  Res8K = 3   // 15360×7680 (YouTube VR top-end default)
};

inline void resolutionSize(ResolutionPreset p, int& outW, int& outH) {
  switch (p) {
  case ResolutionPreset::Res2K: outW = 3840;  outH = 1920;  break;
  case ResolutionPreset::Res3K: outW = 5760;  outH = 2880;  break;
  case ResolutionPreset::Res4K: outW = 7680;  outH = 3840;  break;
  case ResolutionPreset::Res8K:
  default:                      outW = 15360; outH = 7680;  break;
  }
}

inline int suggestedFaceSize(ResolutionPreset p, QualityPreset q) {
  int base = 1024;
  switch (p) {
  case ResolutionPreset::Res2K: base = 1024; break;
  case ResolutionPreset::Res3K: base = 1536; break;
  case ResolutionPreset::Res4K: base = 2048; break;
  case ResolutionPreset::Res8K:
  default:                      base = 4096; break;
  }
  if (q == QualityPreset::Balanced) base = (base * 3) / 4;
  if (q == QualityPreset::Fast) base = base / 2;
  if (base < 512) base = 512;
  return base;
}

enum class CubeFace : int {
  PosX = 0,
  NegX = 1,
  PosY = 2,
  NegY = 3,
  PosZ = 4,
  NegZ = 5,
  Count = 6
};

struct Vec3 {
  float x = 0, y = 0, z = 0;
};

struct Mat4 {
  float m[16] = {
    1,0,0,0,
    0,1,0,0,
    0,0,1,0,
    0,0,0,1
  };
};

struct CameraState {
  Mat4 view{};
  Mat4 projection{};
  Vec3 eye{};
  Vec3 target{};
  Vec3 up{0, 1, 0};
  // Native MMD camera UI path: this is the cubemap direction to apply through
  // MMD itself before MME/ray-mmd evaluates a full frame.
  CubeFace cubeFace = CubeFace::PosZ;
  // Signed half-IPD (±ipd/2). applyCameraFace shifts along the base camera's
  // right axis (not world X), so yawed shots keep lateral parallax.
  float nativeEyeOffsetX = 0.f;
  float fovYDeg = 90.f;
  float aspect = 1.f;
  float zNear = 0.1f;
  float zFar = 10000.f;
  bool valid = false;
};

struct ImageBuffer {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba; // tightly packed RGBA8

  bool empty() const { return width <= 0 || height <= 0 || rgba.empty(); }
  void resize(int w, int h) {
    width = w; height = h;
    rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
  }
};

struct RenderSettings {
  VrMode vrMode = VrMode::VR180;
  StereoMode stereoMode = StereoMode::Stereo;
  QualityPreset quality = QualityPreset::High;
  ResolutionPreset resolution = ResolutionPreset::Res8K; // YouTube VR default
  int outputWidth = 15360;   // mono equirect; filled from resolution preset
  int outputHeight = 7680;
  // Cubemap face capture size. 0 = auto from resolution/quality (user may set any max).
  int faceSize = 0;
  double fps = 60.0; // VR default: higher FPS reduces nausea
  int frameStart = 0;
  int frameEnd = -1;         // -1 = use project end / unknown
  float ipd = 0.065f;        // meters (UI/docs only; runtime uses ipdUnits)
  // MMD world-unit IPD. ~0.8 ≈ real 6.5cm at classic ~20-unit models; default
  // is stronger so depth reads clearly at typical camera distances (30–50).
  float ipdUnits = 2.0f;
  std::wstring outputDir;
  std::wstring ffmpegPath;
  // Optional: mux this audio into the MP4 (MMD-loaded WAV path, or manual browse).
  std::wstring audioPath;
  bool encodeVideo = true;
  EncodeCodec encodeCodec = EncodeCodec::Auto; // HW H.265 first when FFmpeg/GPU allow it
  // true (default): multi-face 90° cameras → equirect (real VR180/360 FOV).
  // false: live MMD camera only (no VR FOV; ray-mmd fallback if cubemap misbehaves).
  bool cubemapFaces = true;
  bool writeSphericalMeta = true;
  std::wstring jobName = L"mmdvr_job";
  // CPU workers for projection / convert. 0 = all logical cores (no artificial cap).
  int workerThreads = 0;
  // Parallel sequence writers (each holds a full frame in RAM). End-user chooses how hard to push.
  int writeThreads = 2;
  // Max pending sequence frames in RAM before backpressure.
  // 0 = auto-cap from free RAM / frame size (never truly unlimited for huge VR frames).
  int writeQueueMax = 0;
  // JPEG vs PNG for the frames\\ sequence (UI). Resume prefers files already on disk.
  SequenceFormat sequenceFormat = SequenceFormat::Auto;

  void applyResolutionPreset() {
    resolutionSize(resolution, outputWidth, outputHeight);
  }

  int resolvedFaceSize() const {
    if (faceSize > 0) return faceSize;
    return suggestedFaceSize(resolution, quality);
  }

  // PNG sequence lives under outputDir\frames\ so the job root stays lean (mp4/job.json).
  std::wstring pngSequenceDir() const { return outputDir + L"\\frames"; }
};

// Strip \\ / : * ? " < > | and trim; empty → mmdvr_job.
inline std::wstring sanitizeProjectName(std::wstring name) {
  while (!name.empty() && (name.front() == L' ' || name.front() == L'\t'))
    name.erase(name.begin());
  while (!name.empty() && (name.back() == L' ' || name.back() == L'\t'))
    name.pop_back();
  for (wchar_t& c : name) {
    if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
        c == L'"' || c == L'<' || c == L'>' || c == L'|')
      c = L'_';
  }
  if (name.empty()) name = L"mmdvr_job";
  return name;
}

// UI stores the parent root; the job writes under root\projectName\.
inline std::wstring joinJobOutputDir(const std::wstring& root, const std::wstring& projectName) {
  const std::wstring name = sanitizeProjectName(projectName);
  if (root.empty()) return name;
  if (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
    return root + name;
  return root + L"\\" + name;
}

inline std::wstring parentOutputRoot(const std::wstring& jobDir, const std::wstring& projectName) {
  const std::wstring name = sanitizeProjectName(projectName);
  const std::wstring suffix = L"\\" + name;
  if (jobDir.size() >= suffix.size() &&
      jobDir.compare(jobDir.size() - suffix.size(), suffix.size(), suffix) == 0) {
    return jobDir.substr(0, jobDir.size() - suffix.size());
  }
  return jobDir;
}

struct JobProgress {
  int currentFrame = 0;
  int totalFrames = 0;
  int lastCompletedFrame = -1;
  int completedCount = 0;      // frames finished in this run
  double elapsedSec = 0;       // active time (pause excluded)
  double secPerFrame = 0;      // rolling average
  double etaSec = -1;          // -1 = unknown
  // Machine keys: starting / rendering / frame_done / flushing_png /
  // encoding / vr_meta / cleanup / done / cancelled / error / paused / cancelling
  std::wstring stage;
  // Optional technical note (path, codec) shown under the status.
  std::wstring stageNote;
  bool cancelled = false;
  bool paused = false;
  bool running = false;
  std::wstring lastError;
};

using ProgressCallback = std::function<void(const JobProgress&)>;

} // namespace mmdvr
