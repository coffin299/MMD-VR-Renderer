#pragma once
#include "RenderController.hpp"
#include <d3d9.h>
#include <mutex>

namespace mmdvr {

// MMDVR_ISOLATE env (binary-search capture side effects):
//   0 / unset = normal capture
//   1 = no D3D readback (dummy magenta) — isolate start/cancel plumbing
//   2 = GetBackBuffer + Release only
//   3 = StretchRect resolve, skip GetRenderTargetData
//   4 = full path (same as 0)
enum class CaptureIsolate : int {
  Normal = 0,
  NoD3D = 1,
  BackBufferOnly = 2,
  StretchRectOnly = 3,
  Full = 4
};

class D3D9CaptureBackend : public IFrameCapture {
public:
  void setDevice(IDirect3DDevice9* device);
  IDirect3DDevice9* device() const { return device_; }

  bool captureViewport(ImageBuffer& out, int maxSide = 0) override;
  void setViewportCrop(int x, int y, int w, int h);
  void clearCrop();
  void setDefaultMaxSide(int maxSide);
  int defaultMaxSide() const;
  // Cubemap faces need a square; live/safe capture should keep the full rectangle.
  void setSquareCrop(bool enabled);
  bool squareCrop() const;

  void setIsolate(CaptureIsolate level);
  CaptureIsolate isolate() const;
  // Drop pooled GPU/sys surfaces (call on Reset / cancel).
  void releasePool();

private:
  IDirect3DDevice9* device_ = nullptr;
  mutable std::mutex mu_;
  bool crop_ = false;
  int cx_ = 0, cy_ = 0, cw_ = 0, ch_ = 0;
  int defaultMaxSide_ = 0;
  bool squareCrop_ = true;
  CaptureIsolate isolate_ = CaptureIsolate::Normal;

  // Reused across captures to avoid Create/Release thrash during Present.
  IDirect3DSurface9* poolGpu_ = nullptr;
  IDirect3DSurface9* poolSys_ = nullptr;
  int poolW_ = 0, poolH_ = 0;
  D3DFORMAT poolFmt_ = D3DFMT_UNKNOWN;

  bool ensurePool(IDirect3DDevice9* dev, int w, int h, D3DFORMAT fmt);
  void releasePoolUnlocked();
};

} // namespace mmdvr
