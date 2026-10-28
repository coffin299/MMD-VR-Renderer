#include "ProgressDialog.hpp"
#include "UiStrings.hpp"
#include "WinUiLayout.hpp"
#include "mmdvr/Diagnostics.hpp"
#include <CommCtrl.h>
#include <atomic>
#include <string>
#include <thread>

namespace mmdvr {

namespace {
enum {
  IDC_STATUS = 2001,
  IDC_DETAIL = 2002,
  IDC_BAR = 2003,
  IDC_PAUSE = 2004,
  IDC_CANCEL = 2005,
  IDC_TITLE = 2006
};

constexpr UINT WM_VR_PROGRESS = WM_APP + 40;
constexpr UINT WM_VR_FINISHED = WM_APP + 41;
constexpr UINT WM_VR_FIT = WM_APP + 42;

constexpr int kClientW = 560;
constexpr int kClientH = 360;
constexpr int kMargin = 24;
constexpr int kContentW = kClientW - kMargin * 2;
constexpr int kBtnW = 200;
constexpr int kBtnH = 36;
constexpr int kBtnGap = 24;
constexpr int kBtnY = 290;

struct ProgressUiState {
  RenderController* controller = nullptr;
  HWND hwnd = nullptr;
  HWND owner = nullptr;
  bool finishedOk = false;
  std::atomic<bool> workerDone{false};
};

ProgressUiState* g_prog = nullptr;

static std::wstring formatDuration(double sec) {
  if (sec < 0) return L"—";
  int s = (int)(sec + 0.5);
  int h = s / 3600;
  int m = (s % 3600) / 60;
  int r = s % 60;
  wchar_t buf[64];
  if (h > 0) swprintf(buf, 64, L"%d:%02d:%02d", h, m, r);
  else swprintf(buf, 64, L"%d:%02d", m, r);
  return buf;
}

static bool isPostFrameStage(const std::wstring& stage) {
  return stage == L"flushing_png" || stage == L"encoding" || stage == L"vr_meta" ||
         stage == L"cleanup" || stage == L"done";
}

static void applyProgress(HWND hWnd, const JobProgress& p) {
  int done = p.completedCount;
  int total = p.totalFrames > 0 ? p.totalFrames : 1;
  int pct = (int)((100.0 * done) / total);
  if (pct > 100) pct = 100;
  // Keep the bar at 100% during post steps so it doesn't look stuck mid-job.
  if (isPostFrameStage(p.stage) && !p.cancelled) pct = 100;
  SendDlgItemMessageW(hWnd, IDC_BAR, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
  SendDlgItemMessageW(hWnd, IDC_BAR, PBM_SETPOS, pct, 0);

  const auto& t = ui::t();
  wchar_t status[256];
  if (p.paused) {
    swprintf(status, 256, L"%s", t.progressPaused);
  } else if (p.stage == L"flushing_png") {
    swprintf(status, 256, L"%s", t.progressFlushingPng);
  } else if (p.stage == L"encoding") {
    swprintf(status, 256, L"%s", t.progressEncoding);
  } else if (p.stage == L"vr_meta") {
    swprintf(status, 256, L"%s", t.progressVrMeta);
  } else if (p.stage == L"cleanup") {
    swprintf(status, 256, L"%s", t.progressCleanup);
  } else if (p.stage == L"done") {
    swprintf(status, 256, L"%s", t.progressDone);
  } else if (p.stage == L"cancelled" || p.cancelled) {
    swprintf(status, 256, L"%s", t.progressCancelled);
  } else if (p.stage == L"cancelling") {
    swprintf(status, 256, L"%s", t.progressCancelling);
  } else {
    swprintf(status, 256, t.progressStatusFmt, done, total, pct);
  }
  SetDlgItemTextW(hWnd, IDC_STATUS, status);

  wchar_t detail[768];
  if (p.stage == L"flushing_png") {
    swprintf(detail, 768, t.progressPostDetailFmt,
             formatDuration(p.elapsedSec).c_str(), t.progressFlushingDetail,
             p.stageNote.c_str());
  } else if (p.stage == L"encoding") {
    swprintf(detail, 768, t.progressPostDetailFmt,
             formatDuration(p.elapsedSec).c_str(), t.progressEncodingDetail,
             p.stageNote.c_str());
  } else if (p.stage == L"vr_meta") {
    swprintf(detail, 768, t.progressPostDetailFmt,
             formatDuration(p.elapsedSec).c_str(), t.progressVrMetaDetail,
             p.stageNote.c_str());
  } else if (p.stage == L"cleanup") {
    swprintf(detail, 768, t.progressPostDetailFmt,
             formatDuration(p.elapsedSec).c_str(), t.progressCleanupDetail,
             p.stageNote.c_str());
  } else {
    swprintf(detail, 768, t.progressDetailFmt,
             p.currentFrame,
             formatDuration(p.elapsedSec).c_str(),
             p.secPerFrame,
             formatDuration(p.etaSec).c_str());
  }
  SetDlgItemTextW(hWnd, IDC_DETAIL, detail);

  SetDlgItemTextW(hWnd, IDC_PAUSE, p.paused ? t.btnResume : t.btnPause);
  // Post steps aren't pausable; cancel still works until cleanup finishes.
  const bool post = isPostFrameStage(p.stage) && p.stage != L"done";
  EnableWindow(GetDlgItem(hWnd, IDC_PAUSE), !post && !p.cancelled);
}

static LRESULT CALLBACK ProgressWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
  case WM_CREATE: {
    const DWORD st = WS_CHILD | WS_VISIBLE;
    const DWORD stMulti = WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX;
    CreateWindowW(L"STATIC", ui::t().progressTitle, st, kMargin, 16, kContentW, 22, hWnd, (HMENU)IDC_TITLE, 0, 0);
    CreateWindowW(L"STATIC", L"", stMulti, kMargin, 48, kContentW, 40, hWnd, (HMENU)IDC_STATUS, 0, 0);
    CreateWindowExW(0, PROGRESS_CLASSW, L"", st, kMargin, 96, kContentW, 22, hWnd, (HMENU)IDC_BAR, 0, 0);
    CreateWindowW(L"STATIC", L"", stMulti, kMargin, 130, kContentW, 140, hWnd, (HMENU)IDC_DETAIL, 0, 0);

    const int pairW = kBtnW * 2 + kBtnGap;
    const int btnX0 = (kClientW - pairW) / 2;
    CreateWindowW(L"BUTTON", ui::t().btnPause, st, btnX0, kBtnY, kBtnW, kBtnH, hWnd, (HMENU)IDC_PAUSE, 0, 0);
    CreateWindowW(L"BUTTON", ui::t().btnCancelRender, st, btnX0 + kBtnW + kBtnGap, kBtnY, kBtnW, kBtnH,
                  hWnd, (HMENU)IDC_CANCEL, 0, 0);
    PostMessageW(hWnd, WM_VR_FIT, 0, 0);
    return 0;
  }
  case WM_VR_FIT:
    ui_layout::FitClientSize(hWnd, kClientW, kClientH);
    return 0;
  case WM_VR_PROGRESS: {
    auto* p = reinterpret_cast<JobProgress*>(lParam);
    if (p) {
      applyProgress(hWnd, *p);
      delete p;
    }
    return 0;
  }
  case WM_VR_FINISHED: {
    bool ok = wParam != 0;
    if (g_prog) g_prog->finishedOk = ok;
    JobProgress snap = g_prog && g_prog->controller ? g_prog->controller->progress() : JobProgress{};
    applyProgress(hWnd, snap);
    EnableWindow(GetDlgItem(hWnd, IDC_PAUSE), FALSE);
    EnableWindow(GetDlgItem(hWnd, IDC_CANCEL), FALSE);
    DestroyWindow(hWnd);
    return 0;
  }
  case WM_COMMAND:
    if (!g_prog || !g_prog->controller) break;
    switch (LOWORD(wParam)) {
    case IDC_PAUSE:
      if (g_prog->controller->isPaused()) g_prog->controller->resume();
      else g_prog->controller->pause();
      {
        auto snap = g_prog->controller->progress();
        applyProgress(hWnd, snap);
      }
      return 0;
    case IDC_CANCEL:
      g_prog->controller->cancel();
      SetDlgItemTextW(hWnd, IDC_STATUS, ui::t().progressCancelling);
      return 0;
    }
    break;
  case WM_CLOSE:
    if (g_prog && g_prog->controller && g_prog->controller->isRunning()) {
      g_prog->controller->cancel();
      return 0;
    }
    DestroyWindow(hWnd);
    return 0;
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hWnd, msg, wParam, lParam);
}
} // namespace

bool RunProgressDialog(HWND owner, RenderController& controller) {
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&icc);

  ProgressUiState state;
  state.controller = &controller;
  state.owner = owner;
  g_prog = &state;

  WNDCLASSW wc{};
  wc.lpfnWndProc = ProgressWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"MMDVRProgressDialog";
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&wc);

  DWORD style = WS_CAPTION | WS_POPUP | WS_SYSMENU;
  DWORD ex = WS_EX_DLGMODALFRAME | WS_EX_TOPMOST;
  RECT rc{0, 0, kClientW, kClientH};
  AdjustWindowRectEx(&rc, style, FALSE, ex);

  state.hwnd = CreateWindowExW(ex, L"MMDVRProgressDialog", ui::t().progressWindowTitle, style,
                               CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                               owner, nullptr, wc.hInstance, nullptr);
  ui_layout::FitClientSize(state.hwnd, kClientW, kClientH);
  ShowWindow(state.hwnd, SW_SHOW);
  // Do NOT disable MMD: frame seeking needs the timeline Edit to evaluate motion.
  // Progress stays topmost; user should avoid manual edits during the job.

  std::thread worker([&controller, hwnd = state.hwnd]() {
    bool ok = controller.start([hwnd](const JobProgress& p) {
      auto* copy = new JobProgress(p);
      if (!PostMessageW(hwnd, WM_VR_PROGRESS, 0, (LPARAM)copy)) {
        delete copy;
      }
    });
    PostMessageW(hwnd, WM_VR_FINISHED, ok ? 1 : 0, 0);
  });

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  if (worker.joinable()) worker.join();
  if (owner) {
    SetForegroundWindow(owner);
  }
  bool result = state.finishedOk;
  g_prog = nullptr;
  return result;
}

} // namespace mmdvr
