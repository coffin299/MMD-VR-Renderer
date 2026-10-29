#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include "mmdvr/Types.hpp"
#include <array>
#include <string>

namespace mmdvr {

struct MmdCameraUiState {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double rotX = 0.0;
  double rotY = 0.0;
  double rotZ = 0.0;
  double distance = 0.0;
  bool valid = false;
};

// Drive MMD's on-screen frame counter (Edit) so animation advances during offline capture.
// Uses UI automation only — no MMD binary patching.
class MmdTimeline {
public:
  // Resolve / refresh the frame Edit HWND under MMD's main window.
  bool bind(HWND mmdMain);
  HWND frameEdit() const { return frameEdit_; }

  // Read current frame shown in the UI (-1 on failure).
  int readFrame() const;

  // Seek to absolute frame and ask MMD to evaluate. Returns true if UI accepted
  // the value (readback matches, or write succeeded when readback unavailable).
  bool seekFrame(int frame);

  // Read and write MMD's built-in camera controls. Keeping ray-mmd on this
  // path makes it re-evaluate its own shader/deferred camera state per face.
  bool readCamera(MmdCameraUiState& out);
  bool applyCamera(const MmdCameraUiState& state);
  // MMD's camera angles orbit around the camera-center controls. Preserve its
  // native eye by moving that center as each cubemap direction is applied.
  bool applyCameraFace(const MmdCameraUiState& base, const CameraState& desired);

  // Hide MMD viewport-only overlays while the renderer is armed. Camera ◎
  // remains pixel-repaired as a fallback; coordinate axes can be disabled at
  // their source and are restored when the session ends.
  bool suppressViewportOverlays();
  // Restore the camera/light editor selection and viewport-overlay state.
  void restoreEditTarget();

  // MMD's 「表示 > 出力サイズ」 controls the real D3D render target. The
  // cubemap face capture must raise this before any Present is sampled; merely
  // scaling the captured 1080p viewport cannot produce an 8K-quality result.
  bool setOutputSize(int width, int height);
  void restoreOutputSize();
  // MMD's screen-capture mode runs before editor overlays. Keep the user's
  // selected mode and restore it when the render session ends.
  bool enableScreenCaptureMode1();
  void restoreScreenCaptureMode();

private:
  HWND mmdMain_ = nullptr;
  HWND frameEdit_ = nullptr;
  std::array<HWND, 7> cameraEdits_{};
  int savedCameraLightTarget_ = -1;
  int savedOutputWidth_ = 0;
  int savedOutputHeight_ = 0;
  int savedScreenCaptureCommand_ = -1;
  int savedInformationVisible_ = -1;
  int savedCoordinateAxisVisible_ = -1;
  bool informationCaptureHidden_ = false;
  bool coordinateAxisCaptureHidden_ = false;

  static bool isDigitText(const std::wstring& s);
  static BOOL CALLBACK enumFindFrameEdit(HWND hwnd, LPARAM lp);
  static BOOL CALLBACK enumFindCameraEdit(HWND hwnd, LPARAM lp);
  static HWND findChildById(HWND root, int id);
  bool bindCameraEdits();
  bool selectCameraLightTarget(int selection);
  bool ensureCameraTargetSelected();
  bool setInformationVisible(bool visible);
  bool setCoordinateAxisVisible(bool visible);
  bool setOutputSizeInternal(int width, int height, bool saveCurrent);
  static bool readDouble(HWND edit, double& out);
  static bool writeDouble(HWND edit, double value);
  static void notifyEditChanged(HWND edit, bool commitKey);
};

} // namespace mmdvr
