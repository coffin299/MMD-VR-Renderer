#pragma once
#include "Types.hpp"
#include <cmath>

namespace mmdvr {

inline float deg2rad(float d) { return d * 0.017453292519943295f; }
inline float rad2deg(float r) { return r * 57.29577951308232f; }

Vec3 normalize(const Vec3& v);
Vec3 cross(const Vec3& a, const Vec3& b);
float dot(const Vec3& a, const Vec3& b);
Vec3 add(const Vec3& a, const Vec3& b);
Vec3 sub(const Vec3& a, const Vec3& b);
Vec3 scale(const Vec3& a, float s);

Mat4 identity();
Mat4 multiply(const Mat4& a, const Mat4& b);
Mat4 lookAtRH(const Vec3& eye, const Vec3& target, const Vec3& up);
Mat4 perspectiveRH(float fovYDeg, float aspect, float zNear, float zFar);
Mat4 translation(const Vec3& t);

// Extract approximate eye/target/up from a D3D-style view matrix (RH).
bool decomposeView(const Mat4& view, Vec3& eye, Vec3& target, Vec3& up);

// Recover near/far from a D3D RH perspective projection (fails for ortho).
bool extractPerspectiveNearFar(const Mat4& proj, float& zNear, float& zFar);

// Cubemap face camera: eye fixed; faces are relative to camera forward/up (not world axes).
// PosZ = forward (matches equirect lon=0), PosX = right, PosY = up.
CameraState makeCubeFaceCamera(const Vec3& eye, const Vec3& forward, const Vec3& worldUp,
                               CubeFace face, float nearZ, float farZ, int faceSize);

// Apply stereo IPD offset along camera right vector.
CameraState offsetCameraForEye(const CameraState& base, float ipdUnits, bool leftEye);

} // namespace mmdvr
