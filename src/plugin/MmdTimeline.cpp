#include "MmdTimeline.hpp"
#include "mmdvr/Diagnostics.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <vector>

namespace mmdvr {
namespace {

constexpr std::array<int, 7> kCameraEditIds = {
    544, 545, 546, 547, 548, 549, 550}; // X/Y/Z, Rx/Ry/Rz, distance (MMD 9.32 x64)

// MMD 9.32 x64 viewport-overlay controls (Present includes these when active).
constexpr int kEditTargetComboId = 436;   // カメラ･照明･アクセサリ | models...
constexpr int kModelEditButtonId = 536;   // モデル編 / カメラ編
constexpr int kBoneSelectButtonId = 490;  // 選択 — toggles bone ○/□ markers
constexpr int kAxisButtonId = 557;        // 座標軸

struct FindCtx {
  HWND best = nullptr;
  int bestScore = -1;
  std::vector<HWND> digitEdits;
};

struct CameraFindCtx {
  std::array<HWND, 7>* edits = nullptr;
};

bool windowClassIsEdit(HWND hwnd) {
  wchar_t cls[64]{};
  GetClassNameW(hwnd, cls, 64);
  return _wcsicmp(cls, L"Edit") == 0;
}

int cameraEditIndex(HWND hwnd) {
  const int id = GetDlgCtrlID(hwnd);
  const auto it = std::find(kCameraEditIds.begin(), kCameraEditIds.end(), id);
  return it == kCameraEditIds.end() ? -1 : static_cast<int>(it - kCameraEditIds.begin());
}

bool allFinite(const MmdCameraUiState& state) {
  return std::isfinite(state.x) && std::isfinite(state.y) && std::isfinite(state.z) &&
         std::isfinite(state.rotX) && std::isfinite(state.rotY) &&
         std::isfinite(state.rotZ) && std::isfinite(state.distance);
}

struct OrbitOffset {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

// MMD's camera center is its orbit target. This matches the Direct3D-style
// row-vector rotations MMD applies to the camera-distance vector.
OrbitOffset orbitOffset(const MmdCameraUiState& state) {
  constexpr double kPi = 3.14159265358979323846;
  const double pitch = state.rotX * kPi / 180.0;
  const double yaw = state.rotY * kPi / 180.0;
  const double roll = state.rotZ * kPi / 180.0;

  OrbitOffset v{0.0, 0.0, -state.distance};
  const double cx = std::cos(pitch), sx = std::sin(pitch);
  v = {v.x, v.y * cx + v.z * sx, -v.y * sx + v.z * cx};
  const double cy = std::cos(yaw), sy = std::sin(yaw);
  v = {v.x * cy + v.z * sy, v.y, -v.x * sy + v.z * cy};
  const double cz = std::cos(roll), sz = std::sin(roll);
  return {v.x * cz + v.y * sz, -v.x * sz + v.y * cz, v.z};
}

} // namespace

bool MmdTimeline::isDigitText(const std::wstring& s) {
  if (s.empty() || s.size() > 8) return false;
  for (wchar_t c : s) {
    if (c < L'0' || c > L'9') return false;
  }
  return true;
}

BOOL CALLBACK MmdTimeline::enumFindFrameEdit(HWND hwnd, LPARAM lp) {
  auto* ctx = reinterpret_cast<FindCtx*>(lp);
  if (!windowClassIsEdit(hwnd)) {
    EnumChildWindows(hwnd, enumFindFrameEdit, lp);
    return TRUE;
  }

  wchar_t text[64]{};
  GetWindowTextW(hwnd, text, 64);
  std::wstring t(text);
  if (!isDigitText(t)) {
    EnumChildWindows(hwnd, enumFindFrameEdit, lp);
    return TRUE;
  }

  RECT rc{};
  GetWindowRect(hwnd, &rc);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  // Frame box is a small single-line edit (typical ~40-80px wide, ~18-28px tall).
  int score = 0;
  if (w > 20 && w < 120) score += 3;
  if (h > 12 && h < 36) score += 3;
  if (t.size() <= 6) score += 1;
  ctx->digitEdits.push_back(hwnd);
  if (score > ctx->bestScore) {
    ctx->bestScore = score;
    ctx->best = hwnd;
  }
  EnumChildWindows(hwnd, enumFindFrameEdit, lp);
  return TRUE;
}

BOOL CALLBACK MmdTimeline::enumFindCameraEdit(HWND hwnd, LPARAM lp) {
  auto* ctx = reinterpret_cast<CameraFindCtx*>(lp);
  if (ctx && ctx->edits && windowClassIsEdit(hwnd)) {
    const int index = cameraEditIndex(hwnd);
    if (index >= 0)
      (*ctx->edits)[static_cast<size_t>(index)] = hwnd;
  }
  return TRUE;
}

bool MmdTimeline::bind(HWND mmdMain) {
  mmdMain_ = mmdMain;
  frameEdit_ = nullptr;
  cameraEdits_.fill(nullptr);
  if (!mmdMain || !IsWindow(mmdMain)) return false;

  // MMD Macro map (9.26+ x64): Edit id 9 ~= frame_input. Try direct ID first.
  HWND byId = GetDlgItem(mmdMain, 9);
  if (byId && windowClassIsEdit(byId)) {
    wchar_t text[64]{};
    GetWindowTextW(byId, text, 64);
    if (isDigitText(text) || text[0] == 0) {
      frameEdit_ = byId;
      Diagnostics::instance().info("MmdTimeline: bound frame Edit via dlg id 9");
      return true;
    }
  }

  FindCtx ctx;
  EnumChildWindows(mmdMain, enumFindFrameEdit, reinterpret_cast<LPARAM>(&ctx));
  if (ctx.best) {
    frameEdit_ = ctx.best;
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "MmdTimeline: bound frame Edit via enum (score=%d, candidates=%zu)",
                  ctx.bestScore, ctx.digitEdits.size());
    Diagnostics::instance().info(buf);
    return true;
  }

  Diagnostics::instance().error(
      "MmdTimeline: could not find frame Edit control — animation will not advance");
  return false;
}

bool MmdTimeline::bindCameraEdits() {
  if (!mmdMain_ || !IsWindow(mmdMain_)) return false;
  bool ready = true;
  for (HWND edit : cameraEdits_)
    ready = ready && edit && IsWindow(edit) && windowClassIsEdit(edit);
  if (ready) return true;

  cameraEdits_.fill(nullptr);
  CameraFindCtx ctx{&cameraEdits_};
  EnumChildWindows(mmdMain_, enumFindCameraEdit, reinterpret_cast<LPARAM>(&ctx));
  for (HWND edit : cameraEdits_) {
    if (!edit || !IsWindow(edit)) {
      Diagnostics::instance().error(
          "MmdTimeline: MMD camera Edit controls not found (requires MMD 9.32 x64 UI)");
      return false;
    }
  }
  Diagnostics::instance().info("MmdTimeline: bound MMD native camera controls");
  return true;
}

int MmdTimeline::readFrame() const {
  if (!frameEdit_ || !IsWindow(frameEdit_)) return -1;
  wchar_t text[64]{};
  GetWindowTextW(frameEdit_, text, 64);
  if (!isDigitText(text)) return -1;
  return _wtoi(text);
}

bool MmdTimeline::readDouble(HWND edit, double& out) {
  if (!edit || !IsWindow(edit)) return false;
  wchar_t text[128]{};
  if (GetWindowTextW(edit, text, static_cast<int>(std::size(text))) <= 0)
    return false;
  wchar_t* end = nullptr;
  const double value = std::wcstod(text, &end);
  while (end && *end == L' ') ++end;
  if (!end || end == text || *end != L'\0' || !std::isfinite(value))
    return false;
  out = value;
  return true;
}

bool MmdTimeline::writeDouble(HWND edit, double value) {
  if (!edit || !IsWindow(edit) || !std::isfinite(value)) return false;
  wchar_t text[64]{};
  std::swprintf(text, std::size(text), L"%.6f", value);
  return SetWindowTextW(edit, text) != FALSE;
}

void MmdTimeline::notifyEditChanged(HWND edit) {
  if (!edit) return;
  const int id = GetDlgCtrlID(edit);
  HWND parent = GetParent(edit);
  if (!parent) return;
  SendMessageW(edit, EM_SETMODIFY, TRUE, 0);
  SendMessageW(parent, WM_COMMAND, MAKEWPARAM(id, EN_CHANGE), reinterpret_cast<LPARAM>(edit));
  SendMessageW(parent, WM_COMMAND, MAKEWPARAM(id, EN_KILLFOCUS), reinterpret_cast<LPARAM>(edit));
  // MMD commits camera edits when Return is pressed. Avoid WM_CHAR because a
  // standard Edit control beeps for it, but keep the key transition that its
  // native camera-control handler consumes.
  SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
  SendMessageW(edit, WM_KEYUP, VK_RETURN, 0);
}

bool MmdTimeline::readCamera(MmdCameraUiState& out) {
  out = {};
  if (!bindCameraEdits()) return false;
  std::array<double*, 7> values = {
      &out.x, &out.y, &out.z, &out.rotX, &out.rotY, &out.rotZ, &out.distance};
  for (size_t i = 0; i < cameraEdits_.size(); ++i) {
    if (!readDouble(cameraEdits_[i], *values[i])) {
      Diagnostics::instance().error(
          "MmdTimeline: cannot read MMD native camera controls; cubemap cancelled safely");
      return false;
    }
  }
  out.valid = allFinite(out);
  return out.valid;
}

bool MmdTimeline::applyCamera(const MmdCameraUiState& state) {
  if (!state.valid || !allFinite(state) || !bindCameraEdits()) return false;
  const std::array<double, 7> values = {
      state.x, state.y, state.z, state.rotX, state.rotY, state.rotZ, state.distance};
  for (size_t i = 0; i < cameraEdits_.size(); ++i) {
    if (!writeDouble(cameraEdits_[i], values[i])) return false;
    // MMD updates the camera state in the edit notification handler. Commit
    // each field immediately, exactly as the native UI does while the user
    // changes a value. Deferring every notification until after all seven
    // WM_SETTEXT calls caused its pitch channel to remain stale for ray-mmd.
    notifyEditChanged(cameraEdits_[i]);
  }
  if (mmdMain_) {
    InvalidateRect(mmdMain_, nullptr, FALSE);
    PostMessageW(mmdMain_, WM_PAINT, 0, 0);
  }
  // Let MMD consume every changed input before Present is requested.
  Sleep(45);
  return true;
}

HWND MmdTimeline::findChildById(HWND root, int id) {
  if (!root || !IsWindow(root) || id == 0) return nullptr;
  struct Ctx { int id; HWND found; };
  Ctx ctx{id, nullptr};
  EnumChildWindows(
      root,
      [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (GetDlgCtrlID(hwnd) == c->id) {
          c->found = hwnd;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  return ctx.found;
}

bool MmdTimeline::suppressViewportOverlays() {
  if (!mmdMain_ || !IsWindow(mmdMain_)) return false;

  HWND combo = findChildById(mmdMain_, kEditTargetComboId);
  if (!combo) {
    Diagnostics::instance().warn(
        "MmdTimeline: edit-target combo not found; camera ◎ may remain in capture");
    return false;
  }

  const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
  const int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
  if (!editTargetSaved_) {
    savedEditTarget_ = sel;
    editTargetSaved_ = true;
  }

  auto notifyCombo = [&](HWND cb) {
    HWND parent = GetParent(cb);
    const WPARAM wp = MAKEWPARAM(kEditTargetComboId, CBN_SELCHANGE);
    if (parent) SendMessageW(parent, WM_COMMAND, wp, reinterpret_cast<LPARAM>(cb));
    SendMessageW(mmdMain_, WM_COMMAND, wp, reinterpret_cast<LPARAM>(cb));
  };

  // Index 0 is カメラ･照明･アクセサリ — that mode draws the red look-at ◎.
  if (count > 1 && sel <= 0) {
    SendMessageW(combo, CB_SETCURSEL, 1, 0);
    notifyCombo(combo);
  }

  // If the panel toggle still says モデル編, click it to leave camera mode.
  if (HWND modelBtn = findChildById(mmdMain_, kModelEditButtonId)) {
    if (IsWindowVisible(modelBtn)) {
      wchar_t text[64]{};
      GetWindowTextW(modelBtn, text, static_cast<int>(std::size(text)));
      if (wcsstr(text, L"モデル") || wcsstr(text, L"Model")) {
        SendMessageW(modelBtn, BM_CLICK, 0, 0);
      }
    }
  }

  Sleep(35);

  // Bone ○ markers appear in model mode while 選択 is latched on.
  if (HWND selectBtn = findChildById(mmdMain_, kBoneSelectButtonId)) {
    if (IsWindowVisible(selectBtn)) {
      const LRESULT st = SendMessageW(selectBtn, BM_GETSTATE, 0, 0);
      if (st & BST_CHECKED) {
        SendMessageW(selectBtn, BM_CLICK, 0, 0);
      }
    }
  }

  // Hide world axes if the toolbar button is latched on.
  if (HWND axisBtn = findChildById(mmdMain_, kAxisButtonId)) {
    if (IsWindowVisible(axisBtn)) {
      const LRESULT st = SendMessageW(axisBtn, BM_GETSTATE, 0, 0);
      if (st & BST_CHECKED) {
        SendMessageW(axisBtn, BM_CLICK, 0, 0);
      }
    }
  }

  Sleep(20);
  Diagnostics::instance().info("MmdTimeline: viewport overlays suppressed for capture");
  return true;
}

void MmdTimeline::restoreEditTarget() {
  if (!editTargetSaved_) return;
  editTargetSaved_ = false;
  if (!mmdMain_ || !IsWindow(mmdMain_)) {
    savedEditTarget_ = -1;
    return;
  }
  HWND combo = findChildById(mmdMain_, kEditTargetComboId);
  if (!combo || savedEditTarget_ < 0) {
    savedEditTarget_ = -1;
    return;
  }
  const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
  if (savedEditTarget_ >= count) {
    savedEditTarget_ = -1;
    return;
  }
  SendMessageW(combo, CB_SETCURSEL, savedEditTarget_, 0);
  HWND parent = GetParent(combo);
  const WPARAM wp = MAKEWPARAM(kEditTargetComboId, CBN_SELCHANGE);
  if (parent) SendMessageW(parent, WM_COMMAND, wp, reinterpret_cast<LPARAM>(combo));
  SendMessageW(mmdMain_, WM_COMMAND, wp, reinterpret_cast<LPARAM>(combo));
  savedEditTarget_ = -1;
  Diagnostics::instance().info("MmdTimeline: edit target restored");
}

bool MmdTimeline::applyCameraFace(
    const MmdCameraUiState& base, const CameraState& desired) {
  if (!base.valid || !desired.valid) return false;
  MmdCameraUiState state = base;
  const OrbitOffset baseOrbit = orbitOffset(base);

  switch (desired.cubeFace) {
  case CubeFace::PosZ: break;
  case CubeFace::NegX: state.rotY += 90.0; break;
  case CubeFace::PosX: state.rotY -= 90.0; break;
  case CubeFace::PosY: state.rotX -= 90.0; break;
  case CubeFace::NegY: state.rotX += 90.0; break;
  case CubeFace::NegZ: state.rotY += 180.0; break;
  default: return false;
  }

  // Preserve the original eye. The former implementation rotated only the
  // angles; that makes MMD orbit about a stationary target, keeping the
  // model centered in every face. Compute both orbit offsets exclusively in
  // MMD UI coordinates because the observed D3D camera can be ray-mmd's
  // internal pass camera rather than MMD's scene camera.
  const OrbitOffset faceOrbit = orbitOffset(state);

  // Parallel stereo: shift center (and therefore eye) along the BASE camera's
  // right axis. nativeEyeOffsetX is signed half-IPD (±ipd/2). World-X alone is
  // wrong whenever the shot has yaw — side faces then slide along the view axis
  // and lose parallax.
  double eyeOx = 0.0, eyeOy = 0.0, eyeOz = 0.0;
  const double eyeShift = static_cast<double>(desired.nativeEyeOffsetX);
  if (eyeShift != 0.0) {
    const double fx = -baseOrbit.x;
    const double fy = -baseOrbit.y;
    const double fz = -baseOrbit.z;
    const double fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    double rx = 0.0, ry = 0.0, rz = 0.0;
    if (fl > 1e-8) {
      const double inv = 1.0 / fl;
      const double nfx = fx * inv, nfy = fy * inv, nfz = fz * inv;
      // right = forward × worldUp
      rx = -nfz;
      ry = 0.0;
      rz = nfx;
      const double rl = std::sqrt(rx * rx + ry * ry + rz * rz);
      if (rl > 1e-8) {
        rx = (rx / rl) * eyeShift;
        ry = (ry / rl) * eyeShift;
        rz = (rz / rl) * eyeShift;
      } else {
        // Looking nearly straight up/down: fall back to yaw-horizontal right.
        constexpr double kPi = 3.14159265358979323846;
        const double yaw = base.rotY * kPi / 180.0;
        rx = std::cos(yaw) * eyeShift;
        rz = -std::sin(yaw) * eyeShift;
      }
    } else {
      rx = eyeShift;
    }
    eyeOx = rx;
    eyeOy = ry;
    eyeOz = rz;
  }

  state.x = base.x + baseOrbit.x - faceOrbit.x + eyeOx;
  state.y = base.y + baseOrbit.y - faceOrbit.y + eyeOy;
  state.z = base.z + baseOrbit.z - faceOrbit.z + eyeOz;
  if (!applyCamera(state)) return false;

  MmdCameraUiState applied;
  if (readCamera(applied)) {
    char msg[384];
    std::snprintf(
        msg, sizeof(msg),
        "MmdTimeline: native face=%d requested center=(%.3f,%.3f,%.3f) "
        "stereo=(%.3f,%.3f,%.3f) rot=(%.3f,%.3f,%.3f), "
        "readback center=(%.3f,%.3f,%.3f) rot=(%.3f,%.3f,%.3f)",
        static_cast<int>(desired.cubeFace),
        state.x, state.y, state.z,
        eyeOx, eyeOy, eyeOz,
        state.rotX, state.rotY, state.rotZ,
        applied.x, applied.y, applied.z,
        applied.rotX, applied.rotY, applied.rotZ);
    Diagnostics::instance().info(msg);
  } else {
    Diagnostics::instance().warn("MmdTimeline: cannot read back applied native camera face");
  }
  return true;
}

bool MmdTimeline::seekFrame(int frame) {
  if (frame < 0) frame = 0;
  if (!frameEdit_ || !IsWindow(frameEdit_)) {
    if (!bind(mmdMain_)) return false;
  }
  if (!frameEdit_) return false;

  // Parent may be disabled by a modal dialog — re-enable for seek.
  const bool parentEnabled = mmdMain_ ? IsWindowEnabled(mmdMain_) != FALSE : true;
  if (mmdMain_ && !parentEnabled)
    EnableWindow(mmdMain_, TRUE);

  wchar_t buf[32];
  _snwprintf_s(buf, _TRUNCATE, L"%d", frame);

  // Avoid SetFocus from a worker thread (focus is thread-affined). SendMessage
  // calls marshal onto MMD's UI thread (progress dialog message pump).
  SendMessageW(frameEdit_, EM_SETSEL, 0, -1);
  SetWindowTextW(frameEdit_, buf);
  SendMessageW(frameEdit_, EM_SETSEL, 0, -1);

  const int id = GetDlgCtrlID(frameEdit_);
  HWND parent = GetParent(frameEdit_);
  if (parent) {
    SendMessageW(parent, WM_COMMAND, MAKEWPARAM(id, EN_CHANGE), (LPARAM)frameEdit_);
  }
  // MMD commits the frame from the Return key-down handler. Do not send
  // WM_CHAR: the standard single-line Edit beeps on that character, which
  // previously played the Windows notification sound once per output frame.
  SendMessageW(frameEdit_, WM_KEYDOWN, VK_RETURN, 0);
  SendMessageW(frameEdit_, WM_KEYUP, VK_RETURN, 0);
  if (parent) {
    SendMessageW(parent, WM_COMMAND, MAKEWPARAM(id, EN_KILLFOCUS), (LPARAM)frameEdit_);
  }

  // Brief yield so bones / MME can evaluate before the next Present capture.
  Sleep(30);

  int got = readFrame();
  if (got >= 0 && got != frame) {
    char msg[128];
    std::snprintf(msg, sizeof(msg),
                  "MmdTimeline: seek requested %d but UI shows %d (continuing)", frame, got);
    Diagnostics::instance().warn(msg);
  } else {
    char msg[96];
    std::snprintf(msg, sizeof(msg), "MmdTimeline: seek -> %d (readback=%d)", frame, got);
    Diagnostics::instance().info(msg);
  }

  if (mmdMain_ && !parentEnabled)
    EnableWindow(mmdMain_, FALSE);

  // Write+Enter was delivered; mismatched readback is non-fatal (UI lag).
  return true;
}

} // namespace mmdvr
