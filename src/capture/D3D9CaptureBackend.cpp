#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "mmdvr/D3D9CaptureBackend.hpp"
#include "mmdvr/Diagnostics.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace mmdvr {

void D3D9CaptureBackend::releasePoolUnlocked() {
  if (poolGpu_) { poolGpu_->Release(); poolGpu_ = nullptr; }
  if (poolSys_) { poolSys_->Release(); poolSys_ = nullptr; }
  poolW_ = poolH_ = 0;
  poolFmt_ = D3DFMT_UNKNOWN;
}

void D3D9CaptureBackend::releaseStagedUnlocked() {
  if (stagedGpu_) { stagedGpu_->Release(); stagedGpu_ = nullptr; }
  stagedW_ = stagedH_ = 0;
  stagedFmt_ = D3DFMT_UNKNOWN;
  stagedReady_ = false;
}

void D3D9CaptureBackend::setDevice(IDirect3DDevice9* device) {
  std::lock_guard<std::mutex> lock(mu_);
  if (device_ != device) {
    releasePoolUnlocked();
    releaseStagedUnlocked();
  }
  device_ = device;
}

void D3D9CaptureBackend::setViewportCrop(int x, int y, int w, int h) {
  std::lock_guard<std::mutex> lock(mu_);
  crop_ = true; cx_ = x; cy_ = y; cw_ = w; ch_ = h;
}

void D3D9CaptureBackend::clearCrop() {
  std::lock_guard<std::mutex> lock(mu_);
  crop_ = false;
}

void D3D9CaptureBackend::setDefaultMaxSide(int maxSide) {
  std::lock_guard<std::mutex> lock(mu_);
  defaultMaxSide_ = maxSide > 0 ? maxSide : 0;
}

int D3D9CaptureBackend::defaultMaxSide() const {
  std::lock_guard<std::mutex> lock(mu_);
  return defaultMaxSide_;
}

void D3D9CaptureBackend::setSquareCrop(bool enabled) {
  std::lock_guard<std::mutex> lock(mu_);
  squareCrop_ = enabled;
}

bool D3D9CaptureBackend::squareCrop() const {
  std::lock_guard<std::mutex> lock(mu_);
  return squareCrop_;
}

void D3D9CaptureBackend::setIsolate(CaptureIsolate level) {
  std::lock_guard<std::mutex> lock(mu_);
  isolate_ = level;
}

CaptureIsolate D3D9CaptureBackend::isolate() const {
  std::lock_guard<std::mutex> lock(mu_);
  return isolate_;
}

void D3D9CaptureBackend::releasePool() {
  std::lock_guard<std::mutex> lock(mu_);
  releasePoolUnlocked();
  releaseStagedUnlocked();
}

bool D3D9CaptureBackend::ensurePool(IDirect3DDevice9* dev, int w, int h, D3DFORMAT fmt) {
  // Caller must hold mu_.
  if (poolGpu_ && poolSys_ && poolW_ == w && poolH_ == h && poolFmt_ == fmt)
    return true;
  releasePoolUnlocked();

  if (FAILED(dev->CreateRenderTarget(w, h, fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &poolGpu_, nullptr)) || !poolGpu_) {
    Diagnostics::instance().error("CreateRenderTarget (capture pool) failed");
    return false;
  }
  if (FAILED(dev->CreateOffscreenPlainSurface(w, h, fmt, D3DPOOL_SYSTEMMEM, &poolSys_, nullptr)) || !poolSys_) {
    if (poolSys_) { poolSys_->Release(); poolSys_ = nullptr; }
    if (FAILED(dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &poolSys_, nullptr)) || !poolSys_) {
      Diagnostics::instance().error("CreateOffscreenPlainSurface (capture pool) failed");
      poolGpu_->Release(); poolGpu_ = nullptr;
      return false;
    }
    fmt = D3DFMT_A8R8G8B8;
    // GPU RT must match sys format for GetRenderTargetData.
    poolGpu_->Release(); poolGpu_ = nullptr;
    if (FAILED(dev->CreateRenderTarget(w, h, fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &poolGpu_, nullptr)) || !poolGpu_) {
      Diagnostics::instance().error("CreateRenderTarget (capture pool retry) failed");
      poolSys_->Release(); poolSys_ = nullptr;
      return false;
    }
  }
  poolW_ = w;
  poolH_ = h;
  poolFmt_ = fmt;
  return true;
}

bool D3D9CaptureBackend::ensureStaged(IDirect3DDevice9* dev, int w, int h, D3DFORMAT fmt) {
  // Caller must hold mu_.
  if (stagedGpu_ && stagedW_ == w && stagedH_ == h && stagedFmt_ == fmt)
    return true;

  releaseStagedUnlocked();
  if (FAILED(dev->CreateRenderTarget(w, h, fmt, D3DMULTISAMPLE_NONE, 0, FALSE,
                                     &stagedGpu_, nullptr)) || !stagedGpu_) {
    Diagnostics::instance().error("CreateRenderTarget (pre-overlay stage) failed");
    return false;
  }
  stagedW_ = w;
  stagedH_ = h;
  stagedFmt_ = fmt;
  return true;
}

static D3DFORMAT pickReadableFormat(D3DFORMAT src) {
  switch (src) {
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8R8G8B8:
  case D3DFMT_A8B8G8R8:
  case D3DFMT_X8B8G8R8:
    return src;
  default:
    return D3DFMT_A8R8G8B8;
  }
}

bool D3D9CaptureBackend::stageSurface(IDirect3DSurface9* source, int preferW, int preferH) {
  if (!source) return false;

  IDirect3DDevice9* dev = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    dev = device_;
  }
  if (!dev) return false;

  D3DSURFACE_DESC desc{};
  if (FAILED(source->GetDesc(&desc)) || desc.Width == 0 || desc.Height == 0)
    return false;

  const D3DFORMAT fmt = pickReadableFormat(desc.Format);
  const int srcW = static_cast<int>(desc.Width);
  const int srcH = static_cast<int>(desc.Height);
  const int dstW = preferW > 0 ? preferW : srcW;
  const int dstH = preferH > 0 ? preferH : srcH;
  bool stageOk = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    stageOk = ensureStaged(dev, dstW, dstH, fmt);
  }
  if (!stageOk) return false;

  IDirect3DSurface9* staged = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    staged = stagedGpu_;
    if (staged) staged->AddRef();
  }
  if (!staged) return false;

  const D3DTEXTUREFILTERTYPE filter =
      (dstW != srcW || dstH != srcH) ? D3DTEXF_LINEAR : D3DTEXF_NONE;
  const HRESULT hr = dev->StretchRect(source, nullptr, staged, nullptr, filter);
  staged->Release();
  if (FAILED(hr)) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "StretchRect pre-overlay stage failed hr=0x%08lX",
                  static_cast<unsigned long>(hr));
    Diagnostics::instance().error(buf);
    clearStagedSurface();
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  stagedReady_ = true;
  return true;
}

bool D3D9CaptureBackend::hasStagedSurface() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stagedReady_ && stagedGpu_;
}

bool D3D9CaptureBackend::stagedSize(int& outW, int& outH) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (!stagedReady_ || !stagedGpu_) {
    outW = outH = 0;
    return false;
  }
  outW = stagedW_;
  outH = stagedH_;
  return true;
}

bool D3D9CaptureBackend::ownsSurface(IDirect3DSurface9* surface) const {
  std::lock_guard<std::mutex> lock(mu_);
  return surface && (surface == poolGpu_ || surface == poolSys_ || surface == stagedGpu_);
}

void D3D9CaptureBackend::clearStagedSurface() {
  std::lock_guard<std::mutex> lock(mu_);
  // Keep the staged RT allocated for the job. Create/Release of 4K–8K
  // surfaces every cubemap face fragments the D3D9 driver and crashes MMD
  // after a while even when Task Manager still shows free VRAM.
  stagedReady_ = false;
}

bool D3D9CaptureBackend::captureViewport(ImageBuffer& out, int maxSide) {
  IDirect3DDevice9* dev = nullptr;
  bool crop = false; int cx, cy, cw, ch;
  int defMax = 0;
  bool wantSquare = true;
  CaptureIsolate iso = CaptureIsolate::Normal;
  {
    std::lock_guard<std::mutex> lock(mu_);
    dev = device_;
    crop = crop_; cx = cx_; cy = cy_; cw = cw_; ch = ch_;
    defMax = defaultMaxSide_;
    wantSquare = squareCrop_;
    iso = isolate_;
  }
  if (!dev) return false;
  if (maxSide <= 0) maxSide = defMax;

  // --- Binary-search stages (do not touch MME RTs / transforms) ---
  if (iso == CaptureIsolate::NoD3D) {
    out.resize(64, 64);
    for (size_t i = 0; i + 3 < out.rgba.size(); i += 4) {
      out.rgba[i + 0] = 255; out.rgba[i + 1] = 0; out.rgba[i + 2] = 255; out.rgba[i + 3] = 255;
    }
    return true;
  }

  // Snapshot only what capture must not leave altered. StretchRect/GetRenderTargetData
  // are not supposed to change these; if a driver does, put them back.
  IDirect3DSurface9* prevRt = nullptr;
  IDirect3DSurface9* prevDs = nullptr;
  D3DVIEWPORT9 prevVp{};
  BOOL prevScissorEnabled = FALSE;
  RECT prevScissor{};
  const bool haveRt = SUCCEEDED(dev->GetRenderTarget(0, &prevRt));
  const bool haveDs = SUCCEEDED(dev->GetDepthStencilSurface(&prevDs));
  const bool haveVp = SUCCEEDED(dev->GetViewport(&prevVp));
  dev->GetRenderState(D3DRS_SCISSORTESTENABLE, (DWORD*)&prevScissorEnabled);
  const bool haveScissor = SUCCEEDED(dev->GetScissorRect(&prevScissor));

  auto restoreDeviceBindings = [&]() {
    if (haveRt) {
      IDirect3DSurface9* cur = nullptr;
      if (SUCCEEDED(dev->GetRenderTarget(0, &cur))) {
        if (cur != prevRt) dev->SetRenderTarget(0, prevRt);
        if (cur) cur->Release();
      } else {
        dev->SetRenderTarget(0, prevRt);
      }
    }
    if (haveDs) {
      IDirect3DSurface9* cur = nullptr;
      if (SUCCEEDED(dev->GetDepthStencilSurface(&cur))) {
        if (cur != prevDs) dev->SetDepthStencilSurface(prevDs);
        if (cur) cur->Release();
      } else {
        dev->SetDepthStencilSurface(prevDs);
      }
    }
    if (haveVp) {
      D3DVIEWPORT9 cur{};
      if (FAILED(dev->GetViewport(&cur)) ||
          cur.X != prevVp.X || cur.Y != prevVp.Y || cur.Width != prevVp.Width ||
          cur.Height != prevVp.Height || cur.MinZ != prevVp.MinZ || cur.MaxZ != prevVp.MaxZ) {
        dev->SetViewport(&prevVp);
      }
    }
    if (haveScissor) {
      RECT cur{};
      if (FAILED(dev->GetScissorRect(&cur)) ||
          cur.left != prevScissor.left || cur.top != prevScissor.top ||
          cur.right != prevScissor.right || cur.bottom != prevScissor.bottom) {
        dev->SetScissorRect(&prevScissor);
      }
    }
    DWORD curScissor = 0;
    if (SUCCEEDED(dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &curScissor)) &&
        (BOOL)curScissor != prevScissorEnabled) {
      dev->SetRenderState(D3DRS_SCISSORTESTENABLE, prevScissorEnabled);
    }
    if (prevRt) { prevRt->Release(); prevRt = nullptr; }
    if (prevDs) { prevDs->Release(); prevDs = nullptr; }
  };

  IDirect3DSurface9* back = nullptr;
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) {
    Diagnostics::instance().error("GetBackBuffer failed");
    restoreDeviceBindings();
    return false;
  }

  if (iso == CaptureIsolate::BackBufferOnly) {
    back->Release();
    restoreDeviceBindings();
    out.resize(16, 16);
    return true;
  }

  D3DSURFACE_DESC desc{};
  back->GetDesc(&desc);
  D3DFORMAT fmt = pickReadableFormat(desc.Format);

  int srcX0 = 0, srcY0 = 0, srcW = (int)desc.Width, srcH = (int)desc.Height;
  if (crop && cw > 0 && ch > 0) {
    srcX0 = (std::max)(0, cx);
    srcY0 = (std::max)(0, cy);
    srcW = (std::min)(cw, (int)desc.Width - srcX0);
    srcH = (std::min)(ch, (int)desc.Height - srcY0);
  }

  int copyX = srcX0, copyY = srcY0, copyW = srcW, copyH = srcH;
  if (wantSquare) {
    int side = (std::min)(srcW, srcH);
    copyX = srcX0 + (srcW - side) / 2;
    copyY = srcY0 + (srcH - side) / 2;
    copyW = side;
    copyH = side;
  }

  int dstW = copyW, dstH = copyH;
  if (maxSide > 0) {
    int longSide = (std::max)(copyW, copyH);
    if (longSide > maxSide) {
      dstW = (std::max)(1, copyW * maxSide / longSide);
      dstH = (std::max)(1, copyH * maxSide / longSide);
    }
  }

  bool poolOk = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    poolOk = ensurePool(dev, dstW, dstH, fmt);
    fmt = poolFmt_;
  }
  if (!poolOk) {
    back->Release();
    restoreDeviceBindings();
    return false;
  }

  IDirect3DSurface9* gpu = nullptr;
  IDirect3DSurface9* sys = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    gpu = poolGpu_;
    sys = poolSys_;
  }

  RECT srcRect{copyX, copyY, copyX + copyW, copyY + copyH};
  RECT dstRect{0, 0, dstW, dstH};
  D3DTEXTUREFILTERTYPE filter = (dstW != copyW || dstH != copyH) ? D3DTEXF_LINEAR : D3DTEXF_NONE;
  HRESULT hrS = dev->StretchRect(back, &srcRect, gpu, &dstRect, filter);
  back->Release();
  if (FAILED(hrS)) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "StretchRect scale failed hr=0x%08lX", (unsigned long)hrS);
    Diagnostics::instance().error(buf);
    restoreDeviceBindings();
    return false;
  }

  if (iso == CaptureIsolate::StretchRectOnly) {
    restoreDeviceBindings();
    out.resize(dstW, dstH);
    return true;
  }

  HRESULT hr = dev->GetRenderTargetData(gpu, sys);
  if (FAILED(hr)) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "GetRenderTargetData failed hr=0x%08lX", (unsigned long)hr);
    Diagnostics::instance().error(buf);
    restoreDeviceBindings();
    return false;
  }

  D3DLOCKED_RECT lr{};
  if (FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
    restoreDeviceBindings();
    return false;
  }

  // Single-threaded convert: Present thread must not spawn worker pools.
  const int w = dstW, h = dstH;
  out.resize(w, h);
  auto* srcBase = static_cast<const uint8_t*>(lr.pBits);
  const int pitch = lr.Pitch;
  for (int y = 0; y < h; ++y) {
    const uint8_t* row = srcBase + y * pitch;
    uint8_t* dst = out.rgba.data() + (size_t)y * (size_t)w * 4;
    for (int x = 0; x < w; ++x) {
      uint8_t b = row[x * 4 + 0];
      uint8_t g = row[x * 4 + 1];
      uint8_t r = row[x * 4 + 2];
      uint8_t a = row[x * 4 + 3];
      dst[x * 4 + 0] = r;
      dst[x * 4 + 1] = g;
      dst[x * 4 + 2] = b;
      dst[x * 4 + 3] = a ? a : 255;
    }
  }

  sys->UnlockRect();
  restoreDeviceBindings();
  return true;
}

bool D3D9CaptureBackend::captureStaged(ImageBuffer& out, int maxSide) {
  IDirect3DDevice9* dev = nullptr;
  IDirect3DSurface9* staged = nullptr;
  int stagedW = 0, stagedH = 0;
  D3DFORMAT stagedFmt = D3DFMT_UNKNOWN;
  bool crop = false;
  int cx = 0, cy = 0, cw = 0, ch = 0;
  int defMax = 0;
  bool wantSquare = true;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!stagedReady_ || !stagedGpu_ || !device_) return false;
    dev = device_;
    staged = stagedGpu_;
    staged->AddRef();
    stagedW = stagedW_;
    stagedH = stagedH_;
    stagedFmt = stagedFmt_;
    crop = crop_; cx = cx_; cy = cy_; cw = cw_; ch = ch_;
    defMax = defaultMaxSide_;
    wantSquare = squareCrop_;
  }
  if (maxSide <= 0) maxSide = defMax;

  int srcX0 = 0, srcY0 = 0, srcW = stagedW, srcH = stagedH;
  if (crop && cw > 0 && ch > 0) {
    srcX0 = (std::max)(0, cx);
    srcY0 = (std::max)(0, cy);
    srcW = (std::min)(cw, stagedW - srcX0);
    srcH = (std::min)(ch, stagedH - srcY0);
  }
  if (srcW <= 0 || srcH <= 0) {
    staged->Release();
    return false;
  }

  int copyX = srcX0, copyY = srcY0, copyW = srcW, copyH = srcH;
  if (wantSquare) {
    const int side = (std::min)(srcW, srcH);
    copyX = srcX0 + (srcW - side) / 2;
    copyY = srcY0 + (srcH - side) / 2;
    copyW = copyH = side;
  }

  int dstW = copyW, dstH = copyH;
  if (maxSide > 0) {
    const int longSide = (std::max)(copyW, copyH);
    if (longSide > maxSide) {
      dstW = (std::max)(1, copyW * maxSide / longSide);
      dstH = (std::max)(1, copyH * maxSide / longSide);
    }
  }

  bool poolOk = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    poolOk = ensurePool(dev, dstW, dstH, stagedFmt);
  }
  if (!poolOk) {
    staged->Release();
    return false;
  }

  IDirect3DSurface9* gpu = nullptr;
  IDirect3DSurface9* sys = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    gpu = poolGpu_;
    sys = poolSys_;
    if (gpu) gpu->AddRef();
    if (sys) sys->AddRef();
  }
  if (!gpu || !sys) {
    if (gpu) gpu->Release();
    if (sys) sys->Release();
    staged->Release();
    return false;
  }

  const RECT srcRect{copyX, copyY, copyX + copyW, copyY + copyH};
  const RECT dstRect{0, 0, dstW, dstH};
  const D3DTEXTUREFILTERTYPE filter =
      (dstW != copyW || dstH != copyH) ? D3DTEXF_LINEAR : D3DTEXF_NONE;
  const HRESULT stretchHr = dev->StretchRect(staged, &srcRect, gpu, &dstRect, filter);
  staged->Release();
  if (FAILED(stretchHr)) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "StretchRect staged capture failed hr=0x%08lX",
                  static_cast<unsigned long>(stretchHr));
    Diagnostics::instance().error(buf);
    gpu->Release();
    sys->Release();
    return false;
  }

  const HRESULT readHr = dev->GetRenderTargetData(gpu, sys);
  gpu->Release();
  if (FAILED(readHr)) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "GetRenderTargetData staged capture failed hr=0x%08lX",
                  static_cast<unsigned long>(readHr));
    Diagnostics::instance().error(buf);
    sys->Release();
    return false;
  }

  D3DLOCKED_RECT lr{};
  if (FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
    sys->Release();
    return false;
  }
  out.resize(dstW, dstH);
  const auto* srcBase = static_cast<const uint8_t*>(lr.pBits);
  for (int y = 0; y < dstH; ++y) {
    const uint8_t* row = srcBase + y * lr.Pitch;
    uint8_t* dst = out.rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(dstW) * 4;
    for (int x = 0; x < dstW; ++x) {
      const uint8_t b = row[x * 4 + 0];
      const uint8_t g = row[x * 4 + 1];
      const uint8_t r = row[x * 4 + 2];
      const uint8_t a = row[x * 4 + 3];
      dst[x * 4 + 0] = r;
      dst[x * 4 + 1] = g;
      dst[x * 4 + 2] = b;
      dst[x * 4 + 3] = a ? a : 255;
    }
  }
  sys->UnlockRect();
  sys->Release();
  return true;
}

} // namespace mmdvr
