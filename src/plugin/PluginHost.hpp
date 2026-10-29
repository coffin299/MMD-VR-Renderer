#pragma once
#include "mmdvr/RenderController.hpp"
#include "mmdvr/D3D9CaptureBackend.hpp"
#include "MmdTimeline.hpp"
#include <d3d9.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
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
  // Runs after Present returns — safe point for device Reset (MME).
  void onAfterPresent(IDirect3DDevice9* device);
  // When native 8K is active, Present source rects may exceed the real
  // backbuffer; clamp them so D3D/MME do not enter device-lost Reset.
  bool adjustPresentRects(const RECT*& source, const RECT*& dest,
                          RECT& localSource, RECT& localDest);
  bool nativeCaptureSurfaceActive() const {
    return nativeCaptureState_.load(std::memory_order_acquire) == 3;
  }
  // faceSized=false → backbuffer viewport. faceSized=true → face square.
  void bindNativeCaptureTarget(IDirect3DDevice9* device, bool faceSizedViewport);
  // Rewrite a window-sized SetViewport to the capture face so ray-mmd fills the
  // square RT. Smaller effect/shadow viewports are left alone.
  bool expandNativeCaptureViewport(D3DVIEWPORT9& vp) const;
  // Observed in MMD's native sprite producer on the D3D thread. This is the
  // authoritative app instance used by its recorder/reset routines.
  void onMmdAppObserved(void* app);
  // Called immediately before an application GetRenderTargetData call. MMD's
  // screen-capture copy occurs before it draws editor overlays.
  void onBeforeGetRenderTargetData(IDirect3DDevice9* device, IDirect3DSurface9* source);
  void onBeforeStretchRect(IDirect3DDevice9* device, IDirect3DSurface9* source,
                           IDirect3DSurface9* destination);
  // Called before MMD switches RT 0 so the completed, outgoing color target
  // can be staged before the next pass begins.
  void onBeforeSetRenderTarget(IDirect3DDevice9* device, DWORD index);
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
  void trimCaptureResources() override;

  // While set, Presents still draw but are not stored as cubemap faces.
  void deferCapturePresents(bool defer);
  // UI-thread safe: drop capture request and wake renderView waiters only.
  void interruptCaptureWait(const char* reason);
  // Job-worker / device-reset: restore MMD camera, drop hooks, release pool.
  void releaseRenderSession(const char* reason);

  std::wstring moduleDir() const;

private:
  PluginHost() = default;
  void restoreObservedTransforms(IDirect3DDevice9* device);
  void applyCaptureIsolateFromEnv();
  bool activateNativeCaptureSurface();
  void restoreNativeCaptureSurface();
  void processNativeCaptureSurfaceRequests();

  RenderController controller_;
  D3D9CaptureBackend capture_;
  MmdTimeline timeline_;
  MmdCameraUiState nativeCameraBase_{};
  bool nativeCameraApplied_ = false;
  bool mmdOutputSizeConfigured_ = false;
  // Actual MMD「出力サイズ」we applied (may be capped below UI faceSize).
  int mmdOutputFaceSize_ = 0;
  IDirect3DDevice9* device_ = nullptr;
  HWND mmdHwnd_ = nullptr;

  std::mutex redrawMu_;
  std::condition_variable redrawCv_;
  bool wantCapture_ = false;
  std::atomic<bool> captureDeferPresents_{false};
  bool captureReady_ = false;
  std::atomic<bool> wantEvict_{false};
  // Armed by job worker; executed on Present (D3D thread) only.
  std::atomic<bool> wantReleasePool_{false};
  // 0=idle, 1=activation requested, 2=activation in progress,
  // 3=active, -1=activation failed.
  std::atomic<int> nativeCaptureState_{0};
  std::atomic<int> nativeCaptureFaceSize_{0};
  std::atomic<bool> nativeCaptureRestoreRequested_{false};
  std::atomic<bool> nativeCaptureResetInProgress_{false};
  std::atomic<void*> mmdApp_{nullptr};
  HWND nativeCaptureWindow_ = nullptr;
  struct NativeCaptureRestoreState {
    void* app = nullptr;
    void* renderer = nullptr;
    IDirect3DDevice9* device = nullptr;
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    std::uint32_t screenWidth = 0;
    std::uint32_t screenHeight = 0;
    float aspectRatio = 0.f;
    std::uint32_t backbufferWidth = 0;
    std::uint32_t backbufferHeight = 0;
    std::uint8_t resetPending = 0;
    std::uint8_t multisampleGate = 0;
    void* recordingWindow = nullptr;
    IDirect3DSurface9* previousCapture = nullptr;
    IDirect3DSurface9* previousDepth = nullptr;
    IDirect3DSurface9* ownedCapture = nullptr;
    IDirect3DSurface9* ownedDepth = nullptr;
  } nativeCaptureRestore_{};
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
  // Arms Present/pre-overlay capture and skips MMD's Local XYZ gizmo draw.
  void setCaptureArmed(bool armed);
  static bool isPerspectiveProjection(const D3DMATRIX* m);
};

} // namespace mmdvr
