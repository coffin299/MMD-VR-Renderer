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

// Camera/Light/Accessory editor target: Camera, Light, Self-shadow, ...
// Selecting Light hides the red camera target ◎ while retaining the scene view.
constexpr int kCameraLightTargetComboId = 434;
constexpr int kCameraTargetSelection = 0;
// 表示(&V) > 出力サイズ(&O), then the dialog's width/height edit controls.
constexpr int kOutputSizeMenuId = 212;
constexpr int kOutputWidthEditId = 621;
constexpr int kOutputHeightEditId = 622;
// 背景 > スクリーン用キャプチャ. These command IDs are fixed in MMD 9.32.
constexpr int kScreenCaptureOffMenuId = 243;
constexpr int kScreenCaptureMode1MenuId = 244;
constexpr int kScreenCaptureMode3MenuId = 246;
constexpr int kCoordinateAxisMenuId = 215; // 表示 > 座標軸表示

struct OutputSizeDialogCtx {
  HWND hwnd = nullptr;
};

BOOL CALLBACK enumOutputSizeDialog(HWND hwnd, LPARAM lp) {
  auto* ctx = reinterpret_cast<OutputSizeDialogCtx*>(lp);
  if (GetDlgItem(hwnd, kOutputWidthEditId) && GetDlgItem(hwnd, kOutputHeightEditId)) {
    ctx->hwnd = hwnd;
    return FALSE;
  }
  return TRUE;
}

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

HMENU findMenuContainingCommand(HMENU menu, UINT command) {
  if (!menu) return nullptr;
  const int count = GetMenuItemCount(menu);
  for (int i = 0; i < count; ++i) {
    if (GetMenuItemID(menu, i) == command) return menu;
    if (HMENU child = GetSubMenu(menu, i)) {
      if (HMENU found = findMenuContainingCommand(child, command)) return found;
    }
  }
  return nullptr;
}

int checkedScreenCaptureCommand(HWND mmdMain) {
  const HMENU root = GetMenu(mmdMain);
  for (UINT command = kScreenCaptureOffMenuId; command <= kScreenCaptureMode3MenuId; ++command) {
    HMENU menu = findMenuContainingCommand(root, command);
    if (menu && (GetMenuState(menu, command, MF_BYCOMMAND) & MF_CHECKED))
      return static_cast<int>(command);
  }
  return 0;
}

bool menuCommandChecked(HWND mmdMain, UINT command, bool& checked) {
  HMENU menu = findMenuContainingCommand(GetMenu(mmdMain), command);
  if (!menu) return false;
  const UINT state = GetMenuState(menu, command, MF_BYCOMMAND);
  if (state == static_cast<UINT>(-1)) return false;
  checked = (state & MF_CHECKED) != 0;
  return true;
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
  if (!ensureCameraTargetSelected()) return false;
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
  if (!state.valid || !allFinite(state) || !ensureCameraTargetSelected() || !bindCameraEdits())
    return false;
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

bool MmdTimeline::selectCameraLightTarget(int selection) {
  HWND combo = findChildById(mmdMain_, kCameraLightTargetComboId);
  if (!combo) {
    Diagnostics::instance().error("MmdTimeline: Camera/Light target combo not found");
    return false;
  }
  const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
  if (selection < 0 || selection >= count) return false;

  const int current = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
  if (savedCameraLightTarget_ < 0) savedCameraLightTarget_ = current;
  if (current == selection) return true;

  SendMessageW(combo, CB_SETCURSEL, selection, 0);
  HWND parent = GetParent(combo);
  const WPARAM wp = MAKEWPARAM(kCameraLightTargetComboId, CBN_SELCHANGE);
  if (parent) SendMessageW(parent, WM_COMMAND, wp, reinterpret_cast<LPARAM>(combo));
  SendMessageW(mmdMain_, WM_COMMAND, wp, reinterpret_cast<LPARAM>(combo));
  return true;
}

bool MmdTimeline::ensureCameraTargetSelected() {
  return selectCameraLightTarget(kCameraTargetSelection);
}

bool MmdTimeline::setOutputSizeInternal(int width, int height, bool saveCurrent) {
  if (!mmdMain_ || !IsWindow(mmdMain_) || width < 64 || height < 64) return false;

  // The dialog is modal, so post the command then wait for MMD's UI thread to
  // create it. This function is called from the render worker while the
  // progress window's message loop keeps MMD responsive.
  if (!PostMessageW(mmdMain_, WM_COMMAND, static_cast<WPARAM>(kOutputSizeMenuId), 0)) {
    Diagnostics::instance().error("MmdTimeline: could not open MMD output-size dialog");
    return false;
  }

  HWND dialog = nullptr;
  const DWORD uiThread = GetWindowThreadProcessId(mmdMain_, nullptr);
  for (int i = 0; i < 100 && !dialog; ++i) {
    OutputSizeDialogCtx ctx;
    EnumThreadWindows(uiThread, enumOutputSizeDialog, reinterpret_cast<LPARAM>(&ctx));
    dialog = ctx.hwnd;
    if (!dialog) Sleep(20);
  }
  if (!dialog) {
    Diagnostics::instance().error("MmdTimeline: timed out opening MMD output-size dialog");
    return false;
  }

  HWND widthEdit = GetDlgItem(dialog, kOutputWidthEditId);
  HWND heightEdit = GetDlgItem(dialog, kOutputHeightEditId);
  double oldW = 0.0, oldH = 0.0;
  if (!widthEdit || !heightEdit || !readDouble(widthEdit, oldW) || !readDouble(heightEdit, oldH)) {
    PostMessageW(dialog, WM_CLOSE, 0, 0);
    Diagnostics::instance().error("MmdTimeline: cannot read MMD output-size dialog");
    return false;
  }
  if (saveCurrent && savedOutputWidth_ <= 0 && savedOutputHeight_ <= 0) {
    savedOutputWidth_ = static_cast<int>(std::lround(oldW));
    savedOutputHeight_ = static_cast<int>(std::lround(oldH));
  }

  wchar_t text[32];
  std::swprintf(text, std::size(text), L"%d", width);
  if (!SetWindowTextW(widthEdit, text)) {
    PostMessageW(dialog, WM_CLOSE, 0, 0);
    return false;
  }
  std::swprintf(text, std::size(text), L"%d", height);
  if (!SetWindowTextW(heightEdit, text)) {
    PostMessageW(dialog, WM_CLOSE, 0, 0);
    return false;
  }

  // IDOK makes MMD recreate its actual output render target, unlike changing
  // only our CPU projection target.
  SendMessageW(dialog, WM_COMMAND, IDOK, 0);
  for (int i = 0; i < 100 && IsWindow(dialog); ++i) Sleep(20);
  if (IsWindow(dialog)) {
    PostMessageW(dialog, WM_CLOSE, 0, 0);
    Diagnostics::instance().error("MmdTimeline: MMD output-size dialog did not close");
    return false;
  }

  char msg[160];
  std::snprintf(msg, sizeof(msg), "MmdTimeline: MMD output size %dx%d", width, height);
  Diagnostics::instance().info(msg);
  Sleep(150); // allow D3D Reset / ray-mmd targets to settle
  return true;
}

bool MmdTimeline::setOutputSize(int width, int height) {
  return setOutputSizeInternal(width, height, true);
}

void MmdTimeline::restoreOutputSize() {
  if (savedOutputWidth_ <= 0 || savedOutputHeight_ <= 0) return;
  const int width = savedOutputWidth_;
  const int height = savedOutputHeight_;
  savedOutputWidth_ = 0;
  savedOutputHeight_ = 0;
  if (!setOutputSizeInternal(width, height, false))
    Diagnostics::instance().warn("MmdTimeline: failed to restore MMD output size");
}

bool MmdTimeline::enableScreenCaptureMode1() {
  if (!mmdMain_ || !IsWindow(mmdMain_)) return false;
  const int current = checkedScreenCaptureCommand(mmdMain_);
  if (savedScreenCaptureCommand_ < 0) savedScreenCaptureCommand_ = current;
  if (current == kScreenCaptureMode1MenuId) return true;

  if (!PostMessageW(mmdMain_, WM_COMMAND, static_cast<WPARAM>(kScreenCaptureMode1MenuId), 0)) {
    Diagnostics::instance().warn("MmdTimeline: could not enable screen-capture mode 1");
    return false;
  }
  Diagnostics::instance().info("MmdTimeline: enabled MMD screen-capture mode 1 for pre-overlay capture");
  return true;
}

void MmdTimeline::restoreScreenCaptureMode() {
  if (savedScreenCaptureCommand_ < 0) return;
  const int saved = savedScreenCaptureCommand_;
  savedScreenCaptureCommand_ = -1;
  if (saved == 0 || !mmdMain_ || !IsWindow(mmdMain_)) return;
  if (checkedScreenCaptureCommand(mmdMain_) == saved) return;
  if (!PostMessageW(mmdMain_, WM_COMMAND, static_cast<WPARAM>(saved), 0)) {
    Diagnostics::instance().warn("MmdTimeline: failed to restore MMD screen-capture mode");
    return;
  }
  Diagnostics::instance().info("MmdTimeline: restored MMD screen-capture mode");
}

bool MmdTimeline::setCoordinateAxisVisible(bool visible) {
  if (!mmdMain_ || !IsWindow(mmdMain_)) return false;
  // The menu command is asynchronous. Once capture has queued the hide, do
  // not post it again for every cubemap face and accidentally toggle it back.
  if (!visible && coordinateAxisCaptureHidden_) return true;
  bool current = false;
  if (!menuCommandChecked(mmdMain_, kCoordinateAxisMenuId, current)) {
    Diagnostics::instance().warn("MmdTimeline: MMD coordinate-axis menu not found");
    return false;
  }
  if (savedCoordinateAxisVisible_ < 0)
    savedCoordinateAxisVisible_ = current ? 1 : 0;
  if (current == visible) {
    coordinateAxisCaptureHidden_ = !visible;
    return true;
  }

  if (!PostMessageW(mmdMain_, WM_COMMAND, static_cast<WPARAM>(kCoordinateAxisMenuId), 0)) {
    Diagnostics::instance().warn("MmdTimeline: could not toggle MMD coordinate axes");
    return false;
  }
  Diagnostics::instance().info(visible
      ? "MmdTimeline: restored MMD coordinate axes for viewport"
      : "MmdTimeline: disabled MMD coordinate axes for capture");
  coordinateAxisCaptureHidden_ = !visible;
  return true;
}

bool MmdTimeline::suppressViewportOverlays() {
  if (!mmdMain_ || !IsWindow(mmdMain_)) return false;
  // MMD 9.32 renders ◎ even with the Light editor selected. Keep Camera
  // selected so face controls commit reliably; remove ◎ in capture fallback.
  // Unlike it, the lower-right coordinate gizmo has a supported MMD toggle.
  const bool axesHidden = setCoordinateAxisVisible(false);
  Diagnostics::instance().info("MmdTimeline: camera target retained for face capture");
  return axesHidden;
}

void MmdTimeline::restoreEditTarget() {
  if (savedCoordinateAxisVisible_ >= 0 && mmdMain_ && IsWindow(mmdMain_)) {
    const bool wantVisible = savedCoordinateAxisVisible_ != 0;
    bool current = false;
    if (menuCommandChecked(mmdMain_, kCoordinateAxisMenuId, current) &&
        current != wantVisible &&
        PostMessageW(mmdMain_, WM_COMMAND, static_cast<WPARAM>(kCoordinateAxisMenuId), 0)) {
      Diagnostics::instance().info("MmdTimeline: coordinate-axis viewport state restored");
    }
    savedCoordinateAxisVisible_ = -1;
    coordinateAxisCaptureHidden_ = false;
  }

  if (savedCameraLightTarget_ < 0 || !mmdMain_ || !IsWindow(mmdMain_)) return;
  const int saved = savedCameraLightTarget_;
  savedCameraLightTarget_ = -1;
  if (selectCameraLightTarget(saved)) {
    // selectCameraLightTarget saves the transient Light selection when no
    // session value is present; restoration must leave no pending state.
    savedCameraLightTarget_ = -1;
    Diagnostics::instance().info("MmdTimeline: Camera/Light target restored");
  }
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
      const double nfx = fx * inv, nfz = fz * inv;
      // Screen-right = worldUp × forward. Using forward × up flips L/R when
      // looking +Z (left eye on +X) and SBS feels inverted / not VR-like.
      // up=(0,1,0) × forward → (nfz, 0, -nfx); yaw0 look+Z → (+1,0,0).
      rx = nfz;
      ry = 0.0;
      rz = -nfx;
      const double rl = std::sqrt(rx * rx + ry * ry + rz * rz);
      if (rl > 1e-8) {
        rx = (rx / rl) * eyeShift;
        ry = (ry / rl) * eyeShift;
        rz = (rz / rl) * eyeShift;
      } else {
        // Looking nearly straight up/down: fall back to yaw-horizontal right.
        constexpr double kPi = 3.14159265358979323846;
        const double yaw = base.rotY * kPi / 180.0;
        // yaw 0 → +X (screen right while looking +Z)
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
