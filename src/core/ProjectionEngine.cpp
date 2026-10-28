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

static void sampleBilinear(const ImageBuffer& img, float u, float v, uint8_t out[4]) {
  if (img.empty()) { out[0]=out[1]=out[2]=0; out[3]=255; return; }
  u = u * (img.width - 1);
  v = v * (img.height - 1);
  int x0 = (int)std::floor(u), y0 = (int)std::floor(v);
  int x1 = x0 + 1, y1 = y0 + 1;
  float fx = u - x0, fy = v - y0;
  uint8_t c00[4], c10[4], c01[4], c11[4];
  readPixel(img, x0, y0, c00); readPixel(img, x1, y0, c10);
  readPixel(img, x0, y1, c01); readPixel(img, x1, y1, c11);
  for (int k = 0; k < 4; ++k) {
    float a = c00[k] * (1 - fx) + c10[k] * fx;
    float b = c01[k] * (1 - fx) + c11[k] * fx;
    out[k] = (uint8_t)std::round(a * (1 - fy) + b * fy);
  }
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
  float u = 0.5f * (sc / ma + 1.f);
  float v = 0.5f * (tc / ma + 1.f);
  if (bilinear) sampleBilinear(faces[face], u, v, outRgba);
  else sampleNearest(faces[face], u, v, outRgba);
}

ImageBuffer ProjectionEngine::cubemapToEquirect(
    const std::array<ImageBuffer, 6>& faces, int outW, int outH, bool bilinear) {
  ImageBuffer out; out.resize(outW, outH);
  const float pi = 3.14159265358979323846f;
  Parallel::forRows(outH, [&](int y) {
    float vv = (y + 0.5f) / outH;
    float lat = (0.5f - vv) * pi;
    float cosLat = std::cos(lat);
    float sinLat = std::sin(lat);
    for (int x = 0; x < outW; ++x) {
      float uu = (x + 0.5f) / outW;
      float lon = (uu - 0.5f) * 2.f * pi;
      float dx = cosLat * std::sin(lon);
      float dy = sinLat;
      float dz = cosLat * std::cos(lon);
      uint8_t px[4];
      sampleCubemap(faces, dx, dy, dz, px, bilinear);
      size_t i = (static_cast<size_t>(y) * outW + x) * 4;
      out.rgba[i] = px[0]; out.rgba[i+1] = px[1]; out.rgba[i+2] = px[2]; out.rgba[i+3] = px[3];
    }
  });
  if (Parallel::cancelled()) { out = {}; }
  return out;
}

ImageBuffer ProjectionEngine::cubemapToEquirect180(
    const std::array<ImageBuffer, 6>& faces, int outW, int outH, bool bilinear) {
  ImageBuffer out; out.resize(outW, outH);
  const float pi = 3.14159265358979323846f;
  Parallel::forRows(outH, [&](int y) {
    float vv = (y + 0.5f) / outH;
    float lat = (0.5f - vv) * pi;
    float cosLat = std::cos(lat);
    float sinLat = std::sin(lat);
    for (int x = 0; x < outW; ++x) {
      float uu = (x + 0.5f) / outW;
      float lon = (uu - 0.5f) * pi;
      float dx = cosLat * std::sin(lon);
      float dy = sinLat;
      float dz = cosLat * std::cos(lon);
      uint8_t px[4];
      sampleCubemap(faces, dx, dy, dz, px, bilinear);
      size_t i = (static_cast<size_t>(y) * outW + x) * 4;
      out.rgba[i] = px[0]; out.rgba[i+1] = px[1]; out.rgba[i+2] = px[2]; out.rgba[i+3] = px[3];
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
