#include "mmdvr/ProjectionEngine.hpp"
#include "mmdvr/Parallel.hpp"
#include <algorithm>
#include <cmath>

namespace mmdvr {

static void readPixel(const ImageBuffer& img, int x, int y, uint8_t out[4]) {
  x = std::clamp(x, 0, img.width - 1);
  y = std::clamp(y, 0, img.height - 1);
  size_t i = (static_cast<size_t>(y) * img.width + x) * 4;
  out[0] = img.rgba[i]; out[1] = img.rgba[i + 1]; out[2] = img.rgba[i + 2]; out[3] = img.rgba[i + 3];
}

static void sampleNearest(const ImageBuffer& img, float u, float v, uint8_t out[4]) {
  if (img.empty()) { out[0]=out[1]=out[2]=0; out[3]=255; return; }
  int x = (int)std::lround(u * (img.width - 1));
  int y = (int)std::lround(v * (img.height - 1));
  readPixel(img, x, y, out);
}

// Fixed-point bilinear (8.8). Opaque VR faces skip alpha blend — same look, less work.
static void sampleBilinear(const ImageBuffer& img, float u, float v, uint8_t out[4]) {
  if (img.empty()) { out[0]=out[1]=out[2]=0; out[3]=255; return; }
  const int w = img.width;
  const int h = img.height;
  float uf = u * (float)(w - 1);
  float vf = v * (float)(h - 1);
  int x0 = (int)uf;
  int y0 = (int)vf;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x0 >= w) x0 = w - 1;
  if (y0 >= h) y0 = h - 1;
  int x1 = x0 + 1;
  int y1 = y0 + 1;
  if (x1 >= w) x1 = w - 1;
  if (y1 >= h) y1 = h - 1;

  const int fx = (int)((uf - (float)x0) * 256.f);
  const int fy = (int)((vf - (float)y0) * 256.f);
  const int ifx = 256 - fx;
  const int ify = 256 - fy;

  const uint8_t* p = img.rgba.data();
  const size_t row = (size_t)w * 4;
  const uint8_t* r0 = p + (size_t)y0 * row;
  const uint8_t* r1 = p + (size_t)y1 * row;
  const uint8_t* c00 = r0 + (size_t)x0 * 4;
  const uint8_t* c10 = r0 + (size_t)x1 * 4;
  const uint8_t* c01 = r1 + (size_t)x0 * 4;
  const uint8_t* c11 = r1 + (size_t)x1 * 4;

  for (int k = 0; k < 3; ++k) {
    const int a = c00[k] * ifx + c10[k] * fx;
    const int b = c01[k] * ifx + c11[k] * fx;
    out[k] = (uint8_t)((a * ify + b * fy + 32768) >> 16);
  }
  out[3] = 255;
}

void ProjectionEngine::sampleCubemap(
    const std::array<ImageBuffer, 6>& faces,
    float dx, float dy, float dz,
    uint8_t outRgba[4],
    bool bilinear) {
  float ax = std::fabs(dx), ay = std::fabs(dy), az = std::fabs(dz);
  int face = 0;
  float sc = 0, tc = 0, ma = 1;
  if (ax >= ay && ax >= az) {
    ma = ax;
    if (dx > 0) { face = (int)CubeFace::PosX; sc = -dz; tc = -dy; }
    else        { face = (int)CubeFace::NegX; sc =  dz; tc = -dy; }
  } else if (ay >= ax && ay >= az) {
    ma = ay;
    if (dy > 0) { face = (int)CubeFace::PosY; sc = dx; tc =  dz; }
    else        { face = (int)CubeFace::NegY; sc = dx; tc = -dz; }
  } else {
    ma = az;
    if (dz > 0) { face = (int)CubeFace::PosZ; sc =  dx; tc = -dy; }
    else        { face = (int)CubeFace::NegZ; sc = -dx; tc = -dy; }
  }
  const float inv = 0.5f / ma;
  const float u = sc * inv + 0.5f;
  const float v = tc * inv + 0.5f;
  if (bilinear) sampleBilinear(faces[face], u, v, outRgba);
  else sampleNearest(faces[face], u, v, outRgba);
}

ImageBuffer ProjectionEngine::cubemapToEquirect(
    const std::array<ImageBuffer, 6>& faces, int outW, int outH, bool bilinear) {
  ImageBuffer out; out.resize(outW, outH);
  const float pi = 3.14159265358979323846f;
  const float invW = 1.f / (float)outW;
  const float invH = 1.f / (float)outH;
  Parallel::forRows(outH, [&](int y) {
    float vv = (y + 0.5f) * invH;
    float lat = (0.5f - vv) * pi;
    float cosLat = std::cos(lat);
    float sinLat = std::sin(lat);
    uint8_t* row = out.rgba.data() + (size_t)y * (size_t)outW * 4;
    for (int x = 0; x < outW; ++x) {
      float uu = (x + 0.5f) * invW;
      float lon = (uu - 0.5f) * 2.f * pi;
      float dx = cosLat * std::sin(lon);
      float dy = sinLat;
      float dz = cosLat * std::cos(lon);
      sampleCubemap(faces, dx, dy, dz, row + (size_t)x * 4, bilinear);
    }
  });
  if (Parallel::cancelled()) { out = {}; }
  return out;
}

ImageBuffer ProjectionEngine::cubemapToEquirect180(
    const std::array<ImageBuffer, 6>& faces, int outW, int outH, bool bilinear) {
  ImageBuffer out; out.resize(outW, outH);
  const float pi = 3.14159265358979323846f;
  const float invW = 1.f / (float)outW;
  const float invH = 1.f / (float)outH;
  Parallel::forRows(outH, [&](int y) {
    float vv = (y + 0.5f) * invH;
    float lat = (0.5f - vv) * pi;
    float cosLat = std::cos(lat);
    float sinLat = std::sin(lat);
    uint8_t* row = out.rgba.data() + (size_t)y * (size_t)outW * 4;
    for (int x = 0; x < outW; ++x) {
      float uu = (x + 0.5f) * invW;
      float lon = (uu - 0.5f) * pi;
      float dx = cosLat * std::sin(lon);
      float dy = sinLat;
      float dz = cosLat * std::cos(lon);
      sampleCubemap(faces, dx, dy, dz, row + (size_t)x * 4, bilinear);
    }
  });
  if (Parallel::cancelled()) { out = {}; }
  return out;
}

std::array<ImageBuffer, 6> ProjectionEngine::makeTestCubemap(int faceSize) {
  std::array<ImageBuffer, 6> faces;
  const uint8_t colors[6][3] = {
    {255,0,0}, {0,255,0}, {0,0,255}, {255,255,0}, {255,0,255}, {0,255,255}
  };
  for (int f = 0; f < 6; ++f) {
    faces[f].resize(faceSize, faceSize);
    for (int y = 0; y < faceSize; ++y) {
      for (int x = 0; x < faceSize; ++x) {
        size_t i = (static_cast<size_t>(y) * faceSize + x) * 4;
        bool border = x < 2 || y < 2 || x >= faceSize - 2 || y >= faceSize - 2;
        faces[f].rgba[i] = border ? 0 : colors[f][0];
        faces[f].rgba[i+1] = border ? 0 : colors[f][1];
        faces[f].rgba[i+2] = border ? 0 : colors[f][2];
        faces[f].rgba[i+3] = 255;
      }
    }
  }
  return faces;
}

} // namespace mmdvr
