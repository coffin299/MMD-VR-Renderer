#include "mmdvr/MathUtil.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace mmdvr {

Vec3 normalize(const Vec3& v) {
  float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  if (len < 1e-8f) return {0, 1, 0};
  return {v.x / len, v.y / len, v.z / len};
}
Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 scale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }

Mat4 identity() { return Mat4{}; }

Mat4 multiply(const Mat4& a, const Mat4& b) {
  Mat4 r{};
  for (int c = 0; c < 4; ++c) {
    for (int row = 0; row < 4; ++row) {
      r.m[c * 4 + row] =
          a.m[0 * 4 + row] * b.m[c * 4 + 0] +
          a.m[1 * 4 + row] * b.m[c * 4 + 1] +
          a.m[2 * 4 + row] * b.m[c * 4 + 2] +
          a.m[3 * 4 + row] * b.m[c * 4 + 3];
    }
  }
  return r;
}

Mat4 lookAtRH(const Vec3& eye, const Vec3& target, const Vec3& up) {
  Vec3 z = normalize(sub(eye, target)); // forward towards -Z in view space
  Vec3 x = normalize(cross(up, z));
  Vec3 y = cross(z, x);
  Mat4 m = identity();
  // D3DTRANSFORMSTATE matrices are row-major. MMD hands us D3DMATRIX in
  // this layout, so face cameras must use the same convention. The old
  // column-major arrangement transposed every cubemap face: it put ray-mmd
  // reflections/shadows in a different direction and often lost the model.
  m.m[0] = x.x; m.m[1] = y.x; m.m[2] = z.x; m.m[3] = 0;
  m.m[4] = x.y; m.m[5] = y.y; m.m[6] = z.y; m.m[7] = 0;
  m.m[8] = x.z; m.m[9] = y.z; m.m[10] = z.z; m.m[11] = 0;
  m.m[12] = -dot(x, eye); m.m[13] = -dot(y, eye); m.m[14] = -dot(z, eye); m.m[15] = 1;
  return m;
}

Mat4 perspectiveRH(float fovYDeg, float aspect, float zNear, float zFar) {
  float f = 1.f / std::tan(deg2rad(fovYDeg) * 0.5f);
  Mat4 m{};
  std::fill(std::begin(m.m), std::end(m.m), 0.f);
  m.m[0] = f / aspect;
  m.m[5] = f;
  m.m[10] = zFar / (zNear - zFar);
  m.m[11] = -1.f;
  m.m[14] = (zNear * zFar) / (zNear - zFar);
  return m;
}

Mat4 translation(const Vec3& t) {
  Mat4 m = identity();
  m.m[12] = t.x; m.m[13] = t.y; m.m[14] = t.z;
  return m;
}

bool extractPerspectiveNearFar(const Mat4& p, float& zNear, float& zFar) {
  // D3D RH perspective: m22 = zf/(zn-zf), m32 = zn*zf/(zn-zf)
  const float m22 = p.m[10];
  const float m32 = p.m[14];
  if (std::fabs(m22) < 1e-12f) return false;
  const float zn = m32 / m22;
  if (!(zn > 1e-6f) || !std::isfinite(zn)) return false;
  const float denom = 1.f + m22;
  if (std::fabs(denom) < 1e-12f) return false;
  const float zf = m22 * zn / denom;
  if (!(zf > zn) || !std::isfinite(zf)) return false;
  zNear = zn;
  zFar = zf;
  return true;
}

bool decomposeView(const Mat4& view, Vec3& eye, Vec3& target, Vec3& up) {
  // Columns of rotation part (row-major access of D3D column-major stored as m[col*4+row])
  Vec3 x{view.m[0], view.m[4], view.m[8]};
  Vec3 y{view.m[1], view.m[5], view.m[9]};
  Vec3 z{view.m[2], view.m[6], view.m[10]};
  eye = {
    -(x.x * view.m[12] + y.x * view.m[13] + z.x * view.m[14]),
    -(x.y * view.m[12] + y.y * view.m[13] + z.y * view.m[14]),
    -(x.z * view.m[12] + y.z * view.m[13] + z.z * view.m[14])
  };
  // Better: eye = -R^T * t where t is translation column
  eye = {
    -(view.m[0] * view.m[12] + view.m[1] * view.m[13] + view.m[2] * view.m[14]),
    -(view.m[4] * view.m[12] + view.m[5] * view.m[13] + view.m[6] * view.m[14]),
    -(view.m[8] * view.m[12] + view.m[9] * view.m[13] + view.m[10] * view.m[14])
  };
  up = normalize(y);
  Vec3 forward = scale(z, -1.f); // camera looks down -Z in view space
  target = add(eye, forward);
  return true;
}

CameraState makeCubeFaceCamera(const Vec3& eye, const Vec3& forward, const Vec3& worldUp,
                               CubeFace face, float nearZ, float farZ, int faceSize) {
  Vec3 f = normalize(forward);
  Vec3 r = normalize(cross(f, worldUp));
  // Degenerate if looking parallel to up — pick a stable side axis.
  if (dot(r, r) < 1e-10f) {
    Vec3 alt = (std::fabs(f.y) < 0.99f) ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    r = normalize(cross(f, alt));
  }
  Vec3 u = normalize(cross(r, f));

  // Local cubemap axes → world. Equirect lon=0 samples +Z face (= camera forward).
  Vec3 dir = f;
  Vec3 up = u;
  switch (face) {
  case CubeFace::PosZ: dir = f; up = u; break;
  case CubeFace::NegZ: dir = scale(f, -1.f); up = u; break;
  case CubeFace::PosX: dir = r; up = u; break;
  case CubeFace::NegX: dir = scale(r, -1.f); up = u; break;
  case CubeFace::PosY: dir = u; up = scale(f, -1.f); break;
  case CubeFace::NegY: dir = scale(u, -1.f); up = f; break;
  default: break;
  }

  CameraState c;
  c.eye = eye;
  c.target = add(eye, dir);
  c.up = up;
  c.fovYDeg = 90.f;
  c.aspect = 1.f;
  c.zNear = nearZ;
  c.zFar = farZ;
  c.view = lookAtRH(c.eye, c.target, c.up);
  c.projection = perspectiveRH(90.f, 1.f, nearZ, farZ);
  c.valid = true;
  (void)faceSize;
  return c;
}

CameraState offsetCameraForEye(const CameraState& base, float ipdUnits, bool leftEye) {
  CameraState c = base;
  Vec3 forward = normalize(sub(base.target, base.eye));
  // Screen-right = up × forward (matches native MMD stereo; not forward × up).
  Vec3 right = normalize(cross(base.up, forward));
  float sign = leftEye ? -0.5f : 0.5f;
  Vec3 offset = scale(right, ipdUnits * sign);
  c.eye = add(base.eye, offset);
  c.target = add(base.target, offset);
  c.view = lookAtRH(c.eye, c.target, c.up);
  c.projection = base.projection;
  c.valid = true;
  return c;
}

} // namespace mmdvr
