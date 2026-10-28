#pragma once
#include "Types.hpp"
#include "CameraController.hpp"
#include "AsyncImageWriter.hpp"
#include "JobState.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace mmdvr {

// Abstract capture of a single rendered view after MME Present.
class IFrameCapture {
public:
  virtual ~IFrameCapture() = default;
  // maxSide>0: GPU-downscale longest side before CPU readback (faster / less RAM).
  virtual bool captureViewport(ImageBuffer& out, int maxSide = 0) = 0;
};

// Abstract request to redraw with overridden camera (plugin implements).
class IRedrawRequest {
public:
  virtual ~IRedrawRequest() = default;
  // Apply camera, request one presentable frame, then capture into out.
  virtual bool renderView(const CameraState& cam, ImageBuffer& out) = 0;
  // Capture next Present as-is (no SetTransform override). ray-mmd safe.
  virtual bool captureView(ImageBuffer& out) = 0;
  // Move MMD's playhead to frameIndex and evaluate motion (UI frame Edit).
  virtual bool seekAnimationFrame(int frameIndex) = 0;
  // Restore the native MMD camera after all faces for one source frame.
  virtual void restoreSceneCamera() = 0;
  // Free temporary capture GPU surfaces (and optionally hint the driver to
  // reclaim managed resources). Safe to call from the job worker after a face.
  virtual void trimCaptureResources() {}
};

class RenderController {
public:
  RenderController();

  void setCapture(std::shared_ptr<IFrameCapture> capture);
  void setRedraw(std::shared_ptr<IRedrawRequest> redraw);
  // cancel(): wake capture waits only (must be UI-thread safe — no MMD UI writes).
  void setAbortHandler(std::function<void()> fn);
  // start() end: restore camera / release capture pool on the job worker thread.
  void setCleanupHandler(std::function<void()> fn);
  CameraController& camera() { return camera_; }

  void setSettings(const RenderSettings& s);
  RenderSettings settings() const;

  bool start(ProgressCallback cb = nullptr);
  void cancel();
  void pause();
  void resume();
  bool isPaused() const { return paused_.load(); }
  bool isCancelled() const { return cancel_.load(); }
  bool isRunning() const { return running_.load(); }
  JobProgress progress() const;

  // Last job artifacts (for “where did my mp4 go?” UI).
  std::wstring lastOutputDir() const;
  std::wstring lastOutputMp4() const;
  bool lastEncodeOk() const;
  std::wstring lastEncodeError() const;
  // Empty unless HW→software (or similar) fallback ran, e.g. L"NVENC → libx264".
  std::wstring lastEncodeFallback() const;

  // Process one animation frame index (multi-view). Used by plugin host loop.
  bool processAnimationFrame(int frameIndex);

  // Offline path when faces are already captured externally.
  static ImageBuffer composeFromFaces(
      const std::array<ImageBuffer, 6>& leftFaces,
      const std::array<ImageBuffer, 6>* rightFaces,
      const RenderSettings& settings);

private:
  bool captureCubemapEye(bool leftEye, std::array<ImageBuffer, 6>& faces);
  bool renderEye(bool leftEye, ImageBuffer& outEye);
  void applyQualityDefaults(RenderSettings& s);
  void notifyStage(const wchar_t* stage, const std::wstring& note = {});

  mutable std::mutex mu_;
  RenderSettings settings_{};
  CameraController camera_{};
  std::shared_ptr<IFrameCapture> capture_;
  std::shared_ptr<IRedrawRequest> redraw_;
  ProgressCallback progressCb_;
  std::function<void()> abortHandler_;
  std::function<void()> cleanupHandler_;
  JobProgress progress_{};
  void waitIfPaused();
  void updateTimingLocked(int completedCount);

  std::atomic<bool> running_{false};
  std::atomic<bool> cancel_{false};
  std::atomic<bool> paused_{false};
  std::unique_ptr<AsyncImageWriter> asyncWriter_;
  std::chrono::steady_clock::time_point runStart_{};
  double pausedAccumSec_ = 0;
  std::chrono::steady_clock::time_point pauseBegan_{};
  bool pauseClockRunning_ = false;
  std::wstring lastOutputDir_;
  std::wstring lastOutputMp4_;
  bool lastEncodeOk_ = false;
  std::wstring lastEncodeError_;
  std::wstring lastEncodeFallback_;
  // Always save the first left-eye cubemap set for post-mortem inspection.
  // Additional frames require MMDVR_DUMP_FACES=1.
  bool firstFaceDebugDumped_ = false;
};

} // namespace mmdvr
