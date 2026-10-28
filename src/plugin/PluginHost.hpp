#pragma once
#include "mmdvr/RenderController.hpp"
#include "mmdvr/D3D9CaptureBackend.hpp"
#include "MmdTimeline.hpp"
#include <d3d9.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <chrono>

namespace mmdvr {

class PluginHost : public IRedrawRequest {
public:
  static PluginHost& instance();

  void onDeviceCreated(IDirect3DDevice9* device);
  void onDeviceReset();
  void onBeforePresent(IDirect3DDevice9* device);
  // Called immediately before an application GetRenderTargetData call. MMD's
  // screen-capture copy occurs before it draws editor overlays.
  void onBeforeGetRenderTargetData(IDirect3DDevice9* device, IDirect3DSurface9* source);
  void onBeforeStretchRect(IDirect3DDevice9* device, IDirect3DSurface9* source,
                           IDirect3DSurface9* destination);
  void onBeginScene(IDirect3DDevice9* device);
  void onSetRenderTarget(IDirect3DSurface9* rt);
  void onMainPassDraw();
  // Records MMD's live main-camera pair while no cubemap override is active.
  void onObservedTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix);
  void onSetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix, bool& replace, D3DMATRIX& outMatrix);
  // Mirror SetTransform replace rules (main RT + perspective only) for ray-mmd GetTransform reads.
  void onGetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* matrix);

  RenderController& controller() { return controller_; }
  D3D9CaptureBackend& capture() { return capture_; }
  CameraController& camera() { return controller_.camera(); }

  void openUi(HWND mmdHwnd);
  void requestSingleCapture();
  void runCameraProbe();
  void startRenderJob(const RenderSettings& settings);

  bool renderView(const CameraState& cam, ImageBuffer& out) override;
  bool captureView(ImageBuffer& out) override;
  bool seekAnimationFrame(int frameIndex) override;
  void restoreSceneCamera() override;

  // UI-thread safe: drop capture request and wake renderView waiters only.
  void interruptCaptureWait(const char* reason);
  // Job-worker / device-reset: restore MMD camera, drop hooks, release pool.
  void releaseRenderSession(const char* reason);

  std::wstring moduleDir() const;

private:
  PluginHost() = default;
  void restoreObservedTransforms(IDirect3DDevice9* device);
  void applyCaptureIsolateFromEnv();

  RenderController controller_;
  D3D9CaptureBackend capture_;
  MmdTimeline timeline_;
  MmdCameraUiState nativeCameraBase_{};
  bool nativeCameraApplied_ = false;
  bool mmdOutputSizeConfigured_ = false;
  IDirect3DDevice9* device_ = nullptr;
  HWND mmdHwnd_ = nullptr;

  std::mutex redrawMu_;
  std::condition_variable redrawCv_;
  bool wantCapture_ = false;
  bool captureReady_ = false;
  bool preOverlayCandidateLogged_ = false;
  ImageBuffer pendingCapture_;
  CameraState pendingCam_{};
  int skipCaptures_ = 0;

  int bbW_ = 0;
  int bbH_ = 0;
  int loggedViewportX_ = -1;
  int loggedViewportY_ = -1;
  int loggedViewportW_ = -1;
  int loggedViewportH_ = -1;
  bool onMainRt_ = true;
  bool mainPassDrawn_ = false;
  bool didReplaceThisFrame_ = false;
  D3DMATRIX pendingObservedView_{};
  bool havePendingObservedView_ = false;

  std::atomic<bool> jobThreadRunning_{false};

  void refreshBackbufferSize();
  void observeCameraFromDevice(IDirect3DDevice9* device);
  void stagePreOverlaySource(IDirect3DDevice9* device, IDirect3DSurface9* source,
                             const char* operation);
  static bool isPerspectiveProjection(const D3DMATRIX* m);
};

} // namespace mmdvr
