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

private:
  HWND mmdMain_ = nullptr;
  HWND frameEdit_ = nullptr;
  std::array<HWND, 7> cameraEdits_{};

  static bool isDigitText(const std::wstring& s);
  static BOOL CALLBACK enumFindFrameEdit(HWND hwnd, LPARAM lp);
  static BOOL CALLBACK enumFindCameraEdit(HWND hwnd, LPARAM lp);
  bool bindCameraEdits();
  static bool readDouble(HWND edit, double& out);
  static bool writeDouble(HWND edit, double value);
  static void notifyEditChanged(HWND edit);
};

} // namespace mmdvr
