#include "RenderDialog.hpp"
#include "UiStrings.hpp"
#include "WinUiLayout.hpp"
#include "HwSuggest.hpp"
#include "mmdvr/VideoEncoder.hpp"
#include <CommCtrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <string>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace mmdvr {

enum {
  IDC_VRMODE = 1001,
  IDC_STEREO = 1002,
  IDC_QUALITY = 1003,
  IDC_RESOLUTION = 1004,
  IDC_SIZEINFO = 1005,
  IDC_FPS = 1006,
  IDC_IPD = 1007,
  IDC_FSTART = 1008,
  IDC_FEND = 1009,
  IDC_OUTDIR = 1010,
  IDC_BROWSE = 1011,
  IDC_ENCODE = 1012,
  IDC_START = 1013,
  IDC_CANCEL = 1014,
  IDC_CAPTURE = 1015,
  IDC_ISSUES_LEAD = 1016,
  IDC_ISSUES_LINK = 1017,
  IDC_THREADS = 1019,
  IDC_FACE = 1020,
  IDC_WRITE_THREADS = 1021,
  IDC_WRITE_QUEUE = 1022,
  IDC_AUTOFILL = 1023,
  IDC_AUTOFILL_STATUS = 1024,
  IDC_LANG_JA = 1025,
  IDC_LANG_EN = 1026,
  IDC_ENCODE_CODEC = 1027,
  IDC_CUBEMAP = 1028,
  IDC_LBL_CUBEMAP_HINT = 1029,
  IDC_AUDIO = 1030,
  IDC_BROWSE_AUDIO = 1031,
  IDC_LBL_AUDIO = 1032,
  IDC_LBL_AUDIO_HINT = 1033,
  IDC_PROJECT = 1034,
  IDC_LBL_VRMODE = 1101,
  IDC_LBL_STEREO = 1102,
  IDC_LBL_QUALITY = 1103,
  IDC_LBL_RES = 1104,
  IDC_LBL_SIZE = 1105,
  IDC_LBL_FPS = 1106,
  IDC_LBL_THREADS = 1107,
  IDC_LBL_THREADS_HINT = 1108,
  IDC_LBL_FACE = 1109,
  IDC_LBL_WRITE = 1110,
  IDC_LBL_QUEUE = 1111,
  IDC_LBL_QUEUE_HINT = 1112,
  IDC_LBL_IPD = 1113,
  IDC_LBL_FSTART = 1114,
  IDC_LBL_FEND = 1115,
  IDC_LBL_OUT = 1116,
  IDC_LBL_HINT = 1117,
  IDC_LBL_RESNOTE = 1118,
  IDC_LBL_SPEC = 1119,
  IDC_LBL_PR = 1120,
  IDC_LBL_ENCODE_CODEC = 1121,
  IDC_LBL_FACE_HINT = 1122,
  IDC_LBL_SAVE_WARN = 1123,
  IDC_LBL_PROJECT = 1124
};

static RenderSettings* g_settings = nullptr;
static bool g_ok = false;
static HBRUSH g_yellowBrush = nullptr;

static void setText(HWND h, int id, const std::wstring& t) {
  SetDlgItemTextW(h, id, t.c_str());
}
static std::wstring getText(HWND h, int id) {
  wchar_t buf[1024]{};
  GetDlgItemTextW(h, id, buf, 1024);
  return buf;
}

static void refillCombo(HWND hDlg, int id, const wchar_t* const* items, int count, int sel) {
  SendDlgItemMessageW(hDlg, id, CB_RESETCONTENT, 0, 0);
  for (int i = 0; i < count; ++i)
    SendDlgItemMessageW(hDlg, id, CB_ADDSTRING, 0, (LPARAM)items[i]);
  if (sel < 0) sel = 0;
  if (sel >= count) sel = count - 1;
  SendDlgItemMessageW(hDlg, id, CB_SETCURSEL, sel, 0);
}

// Cubemap face edge presets. 0 = auto from resolution/quality.
// 7680/8192 ≈ 8K face; GPU/D3D9 MaxTexture (often 8192) may still reject the top end.
constexpr int kFaceSizePresets[] = {
    0, 512, 1024, 1536, 2048, 3072, 4096, 6144, 7680, 8192};
constexpr int kFaceSizePresetCount =
    static_cast<int>(sizeof(kFaceSizePresets) / sizeof(kFaceSizePresets[0]));
constexpr int kWriteThreadPresets[] = {1, 2, 3, 4, 6, 8};
constexpr int kWriteThreadPresetCount =
    static_cast<int>(sizeof(kWriteThreadPresets) / sizeof(kWriteThreadPresets[0]));
// 0 = unlimited pending frames in RAM.
constexpr int kWriteQueuePresets[] = {0, 1, 2, 3, 4, 6, 8};
constexpr int kWriteQueuePresetCount =
    static_cast<int>(sizeof(kWriteQueuePresets) / sizeof(kWriteQueuePresets[0]));

static int presetToComboIndex(const int* presets, int count, int value, int zeroIndex = 0) {
  if (value <= 0 && zeroIndex >= 0) return zeroIndex;
  int best = (zeroIndex == 0 && count > 1) ? 1 : 0;
  int bestDist = 0x7fffffff;
  for (int i = 0; i < count; ++i) {
    if (presets[i] == value) return i;
    if (presets[i] <= 0 && value <= 0) return i;
    if (presets[i] <= 0) continue;
    const int d = abs(presets[i] - value);
    if (d < bestDist) {
      bestDist = d;
      best = i;
    }
  }
  return best;
}

static int comboIndexToPreset(const int* presets, int count, int idx, int fallback) {
  if (idx < 0 || idx >= count) return fallback;
  return presets[idx];
}

static int faceSizeToComboIndex(int faceSize) {
  return presetToComboIndex(kFaceSizePresets, kFaceSizePresetCount, faceSize, 0);
}
static int comboIndexToFaceSize(int idx) {
  return comboIndexToPreset(kFaceSizePresets, kFaceSizePresetCount, idx, 0);
}
static int writeThreadsToComboIndex(int n) {
  return presetToComboIndex(kWriteThreadPresets, kWriteThreadPresetCount, n < 1 ? 1 : n, -1);
}
static int comboIndexToWriteThreads(int idx) {
  const int v = comboIndexToPreset(kWriteThreadPresets, kWriteThreadPresetCount, idx, 2);
  return v < 1 ? 1 : v;
}
static int writeQueueToComboIndex(int n) {
  return presetToComboIndex(kWriteQueuePresets, kWriteQueuePresetCount, n, 0);
}
static int comboIndexToWriteQueue(int idx) {
  return comboIndexToPreset(kWriteQueuePresets, kWriteQueuePresetCount, idx, 0);
}

static void refillFaceCombo(HWND hDlg, int faceSize) {
  const auto& s = ui::t();
  SendDlgItemMessageW(hDlg, IDC_FACE, CB_RESETCONTENT, 0, 0);
  for (int i = 0; i < kFaceSizePresetCount; ++i) {
    if (kFaceSizePresets[i] <= 0) {
      SendDlgItemMessageW(hDlg, IDC_FACE, CB_ADDSTRING, 0, (LPARAM)s.comboFaceAuto);
    } else {
      wchar_t buf[32];
      swprintf(buf, 32, L"%d", kFaceSizePresets[i]);
      SendDlgItemMessageW(hDlg, IDC_FACE, CB_ADDSTRING, 0, (LPARAM)buf);
    }
  }
  SendDlgItemMessageW(hDlg, IDC_FACE, CB_SETCURSEL, faceSizeToComboIndex(faceSize), 0);
}

static void refillWriteThreadsCombo(HWND hDlg, int writeThreads) {
  SendDlgItemMessageW(hDlg, IDC_WRITE_THREADS, CB_RESETCONTENT, 0, 0);
  for (int i = 0; i < kWriteThreadPresetCount; ++i) {
    wchar_t buf[16];
    swprintf(buf, 16, L"%d", kWriteThreadPresets[i]);
    SendDlgItemMessageW(hDlg, IDC_WRITE_THREADS, CB_ADDSTRING, 0, (LPARAM)buf);
  }
  SendDlgItemMessageW(
      hDlg, IDC_WRITE_THREADS, CB_SETCURSEL, writeThreadsToComboIndex(writeThreads), 0);
}

static void refillWriteQueueCombo(HWND hDlg, int writeQueueMax) {
  const auto& s = ui::t();
  SendDlgItemMessageW(hDlg, IDC_WRITE_QUEUE, CB_RESETCONTENT, 0, 0);
  for (int i = 0; i < kWriteQueuePresetCount; ++i) {
    if (kWriteQueuePresets[i] <= 0) {
      SendDlgItemMessageW(hDlg, IDC_WRITE_QUEUE, CB_ADDSTRING, 0, (LPARAM)s.comboQueueUnlimited);
    } else {
      wchar_t buf[16];
      swprintf(buf, 16, L"%d", kWriteQueuePresets[i]);
      SendDlgItemMessageW(hDlg, IDC_WRITE_QUEUE, CB_ADDSTRING, 0, (LPARAM)buf);
    }
  }
  SendDlgItemMessageW(
      hDlg, IDC_WRITE_QUEUE, CB_SETCURSEL, writeQueueToComboIndex(writeQueueMax), 0);
}

static void updateSizeInfo(HWND hDlg) {
  int resIdx = (int)SendDlgItemMessageW(hDlg, IDC_RESOLUTION, CB_GETCURSEL, 0, 0);
  if (resIdx < 0) resIdx = (int)ResolutionPreset::Res8K;
  int stereoIdx = (int)SendDlgItemMessageW(hDlg, IDC_STEREO, CB_GETCURSEL, 0, 0);
  int w = 0, h = 0;
  resolutionSize((ResolutionPreset)resIdx, w, h);
  wchar_t buf[192];
  if (stereoIdx == 1)
    swprintf(buf, 192, ui::t().sizeStereoFmt, w, h, w * 2, h);
  else
    swprintf(buf, 192, ui::t().sizeMonoFmt, w, h);
  setText(hDlg, IDC_SIZEINFO, buf);
}

static void applyLabels(HWND hDlg) {
  const auto& s = ui::t();
  SetWindowTextW(hDlg, s.dialogTitle);
  setText(hDlg, IDC_LBL_VRMODE, s.labelVrMode);
  setText(hDlg, IDC_LBL_STEREO, s.labelStereo);
  setText(hDlg, IDC_LBL_QUALITY, s.labelQuality);
  setText(hDlg, IDC_LBL_RES, s.labelResolution);
  setText(hDlg, IDC_LBL_SIZE, s.labelSizeHint);
  setText(hDlg, IDC_LBL_FPS, s.labelFps);
  setText(hDlg, IDC_LBL_THREADS, s.labelThreads);
  setText(hDlg, IDC_LBL_THREADS_HINT, s.threadsAutoHint);
  setText(hDlg, IDC_LBL_FACE, s.labelFace);
  setText(hDlg, IDC_LBL_FACE_HINT, s.faceHint);
  setText(hDlg, IDC_LBL_WRITE, s.labelWriteThreads);
  setText(hDlg, IDC_LBL_QUEUE, s.labelWriteQueue);
  setText(hDlg, IDC_LBL_QUEUE_HINT, s.queueUnlimitedHint);
  setText(hDlg, IDC_LBL_IPD, s.labelIpd);
  setText(hDlg, IDC_LBL_FSTART, s.labelFrameStart);
  setText(hDlg, IDC_LBL_FEND, s.labelFrameEnd);
  setText(hDlg, IDC_LBL_PROJECT, s.labelProjectName);
  setText(hDlg, IDC_LBL_OUT, s.labelOutDir);
  setText(hDlg, IDC_BROWSE, s.btnBrowse);
  setText(hDlg, IDC_ENCODE, s.chkEncode);
  setText(hDlg, IDC_LBL_ENCODE_CODEC, s.labelEncodeCodec);
  setText(hDlg, IDC_LBL_AUDIO, s.labelAudio);
  setText(hDlg, IDC_LBL_AUDIO_HINT, s.audioHint);
  setText(hDlg, IDC_CUBEMAP, s.chkCubemap);
  setText(hDlg, IDC_LBL_CUBEMAP_HINT, s.cubemapHint);
  setText(hDlg, IDC_AUTOFILL, s.btnAutoFill);
  setText(hDlg, IDC_START, s.btnStart);
  setText(hDlg, IDC_CAPTURE, s.btnCaptureOnly);
  setText(hDlg, IDC_CANCEL, s.btnCancel);
  setText(hDlg, IDC_LBL_HINT, s.hint);
  setText(hDlg, IDC_LBL_RESNOTE, s.resNote);
  setText(hDlg, IDC_ISSUES_LEAD, s.issuesLead);
  setText(hDlg, IDC_ISSUES_LINK, s.issuesSysLink);
  setText(hDlg, IDC_LBL_SPEC, s.recommendedSpec);
  setText(hDlg, IDC_LBL_PR, s.prWelcome);
  setText(hDlg, IDC_LANG_JA, s.btnLangJa);
  setText(hDlg, IDC_LANG_EN, s.btnLangEn);
  setText(hDlg, IDC_LBL_SAVE_WARN, s.saveBeforeRenderWarn);

  int stereoSel = (int)SendDlgItemMessageW(hDlg, IDC_STEREO, CB_GETCURSEL, 0, 0);
  int qualitySel = (int)SendDlgItemMessageW(hDlg, IDC_QUALITY, CB_GETCURSEL, 0, 0);
  int resSel = (int)SendDlgItemMessageW(hDlg, IDC_RESOLUTION, CB_GETCURSEL, 0, 0);
  int codecSel = (int)SendDlgItemMessageW(hDlg, IDC_ENCODE_CODEC, CB_GETCURSEL, 0, 0);
  const wchar_t* stereoItems[] = { s.comboMono, s.comboStereo };
  const wchar_t* qualityItems[] = { s.comboHigh, s.comboBalanced, s.comboFast };
  const wchar_t* resItems[] = { ui::kRes2K, ui::kRes3K, ui::kRes4K, ui::kRes8K };
  const wchar_t* codecItems[] = {
    s.comboCodecLibx264, s.comboCodecAuto, s.comboCodecNvenc, s.comboCodecQsv, s.comboCodecAmf
  };
  refillCombo(hDlg, IDC_STEREO, stereoItems, 2, stereoSel);
  refillCombo(hDlg, IDC_QUALITY, qualityItems, 3, qualitySel);
  refillCombo(hDlg, IDC_RESOLUTION, resItems, 4, resSel);
  refillCombo(hDlg, IDC_ENCODE_CODEC, codecItems, 5, codecSel < 0 ? 0 : codecSel);
  const int faceSel = (int)SendDlgItemMessageW(hDlg, IDC_FACE, CB_GETCURSEL, 0, 0);
  refillFaceCombo(hDlg, comboIndexToFaceSize(faceSel));
  const int writeSel = (int)SendDlgItemMessageW(hDlg, IDC_WRITE_THREADS, CB_GETCURSEL, 0, 0);
  refillWriteThreadsCombo(hDlg, comboIndexToWriteThreads(writeSel));
  const int queueSel = (int)SendDlgItemMessageW(hDlg, IDC_WRITE_QUEUE, CB_GETCURSEL, 0, 0);
  refillWriteQueueCombo(hDlg, comboIndexToWriteQueue(queueSel));
  updateSizeInfo(hDlg);
}

static void applyHwSuggestToUi(HWND hDlg) {
  auto sug = suggestFromThisPc();
  SendDlgItemMessageW(hDlg, IDC_QUALITY, CB_SETCURSEL, (WPARAM)sug.quality, 0);
  SendDlgItemMessageW(hDlg, IDC_RESOLUTION, CB_SETCURSEL, (WPARAM)sug.resolution, 0);
  setText(hDlg, IDC_FPS, std::to_wstring((int)sug.fps));
  setText(hDlg, IDC_THREADS, std::to_wstring(sug.workerThreads));
  SendDlgItemMessageW(hDlg, IDC_FACE, CB_SETCURSEL, faceSizeToComboIndex(sug.faceSize), 0);
  SendDlgItemMessageW(
      hDlg, IDC_WRITE_THREADS, CB_SETCURSEL, writeThreadsToComboIndex(sug.writeThreads), 0);
  SendDlgItemMessageW(
      hDlg, IDC_WRITE_QUEUE, CB_SETCURSEL, writeQueueToComboIndex(sug.writeQueueMax), 0);
  updateSizeInfo(hDlg);
  setText(hDlg, IDC_AUTOFILL_STATUS, sug.summary);
}

static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM) {
  switch (msg) {
  case WM_INITDIALOG: {
    auto& s = *g_settings;
    s.applyResolutionPreset();
    SendDlgItemMessageW(hDlg, IDC_VRMODE, CB_RESETCONTENT, 0, 0);
    SendDlgItemMessageW(hDlg, IDC_VRMODE, CB_ADDSTRING, 0, (LPARAM)L"VR180");
    SendDlgItemMessageW(hDlg, IDC_VRMODE, CB_ADDSTRING, 0, (LPARAM)L"VR360");
    applyLabels(hDlg);
    SendDlgItemMessageW(hDlg, IDC_VRMODE, CB_SETCURSEL, (s.vrMode == VrMode::VR360) ? 1 : 0, 0);
    SendDlgItemMessageW(hDlg, IDC_STEREO, CB_SETCURSEL, (s.stereoMode == StereoMode::Stereo) ? 1 : 0, 0);
    SendDlgItemMessageW(hDlg, IDC_QUALITY, CB_SETCURSEL, (int)s.quality, 0);
    SendDlgItemMessageW(hDlg, IDC_RESOLUTION, CB_SETCURSEL, (int)s.resolution, 0);
    updateSizeInfo(hDlg);
    setText(hDlg, IDC_FPS, std::to_wstring((int)s.fps));
    setText(hDlg, IDC_THREADS, std::to_wstring(s.workerThreads));
    refillFaceCombo(hDlg, s.faceSize);
    refillWriteThreadsCombo(hDlg, s.writeThreads);
    refillWriteQueueCombo(hDlg, s.writeQueueMax);
    setText(hDlg, IDC_IPD, std::to_wstring(s.ipdUnits));
    setText(hDlg, IDC_FSTART, std::to_wstring(s.frameStart));
    setText(hDlg, IDC_FEND, std::to_wstring(s.frameEnd < 0 ? s.frameStart : s.frameEnd));
    {
      const std::wstring project =
          s.jobName.empty() || s.jobName == L"__capture_only__" ? L"mmdvr_job" : s.jobName;
      setText(hDlg, IDC_PROJECT, project);
      setText(hDlg, IDC_OUTDIR, parentOutputRoot(s.outputDir, project));
    }
    CheckDlgButton(hDlg, IDC_ENCODE, s.encodeVideo ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(hDlg, IDC_ENCODE_CODEC, CB_SETCURSEL, (int)s.encodeCodec, 0);
    setText(hDlg, IDC_AUDIO, s.audioPath);
    CheckDlgButton(hDlg, IDC_CUBEMAP, s.cubemapFaces ? BST_CHECKED : BST_UNCHECKED);
    // Probe in background of init so Auto/NVENC labels reflect this PC's FFmpeg (same as MMD bats).
    VideoEncoder::probe(s.ffmpegPath);
    return TRUE;
  }
  case WM_COMMAND:
    switch (LOWORD(wParam)) {
    case IDC_LANG_JA:
      ui::setLang(ui::Lang::Ja);
      applyLabels(hDlg);
      return TRUE;
    case IDC_LANG_EN:
      ui::setLang(ui::Lang::En);
      applyLabels(hDlg);
      return TRUE;
    case IDC_RESOLUTION:
    case IDC_STEREO:
      if (HIWORD(wParam) == CBN_SELCHANGE) updateSizeInfo(hDlg);
      return TRUE;
    case IDC_AUTOFILL:
      applyHwSuggestToUi(hDlg);
      return TRUE;
    case IDC_BROWSE: {
      wchar_t file[MAX_PATH] = L"output";
      OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn);
      ofn.hwndOwner = hDlg; ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
      ofn.lpstrFilter = ui::kBrowseFilter;
      ofn.Flags = OFN_PATHMUSTEXIST;
      ofn.lpstrTitle = ui::t().browseTitle;
      if (GetSaveFileNameW(&ofn)) {
        std::wstring p(file);
        auto slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos) p = p.substr(0, slash);
        setText(hDlg, IDC_OUTDIR, p);
      }
      return TRUE;
    }
    case IDC_BROWSE_AUDIO: {
      wchar_t file[MAX_PATH] = L"";
      OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn);
      ofn.hwndOwner = hDlg; ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
      ofn.lpstrFilter = ui::kAudioFilter;
      ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
      ofn.lpstrTitle = ui::t().browseAudioTitle;
      if (GetOpenFileNameW(&ofn))
        setText(hDlg, IDC_AUDIO, file);
      return TRUE;
    }
    case IDC_CAPTURE:
      g_settings->jobName = L"__capture_only__";
      g_ok = true;
      DestroyWindow(hDlg);
      return TRUE;
    case IDC_START: {
      auto& s = *g_settings;
      s.vrMode = SendDlgItemMessageW(hDlg, IDC_VRMODE, CB_GETCURSEL, 0, 0) == 1 ? VrMode::VR360 : VrMode::VR180;
      s.stereoMode = SendDlgItemMessageW(hDlg, IDC_STEREO, CB_GETCURSEL, 0, 0) == 1 ? StereoMode::Stereo : StereoMode::Mono;
      s.quality = (QualityPreset)SendDlgItemMessageW(hDlg, IDC_QUALITY, CB_GETCURSEL, 0, 0);
      int r = (int)SendDlgItemMessageW(hDlg, IDC_RESOLUTION, CB_GETCURSEL, 0, 0);
      if (r < 0) r = (int)ResolutionPreset::Res8K;
      s.resolution = (ResolutionPreset)r;
      s.applyResolutionPreset();
      s.fps = _wtof(getText(hDlg, IDC_FPS).c_str());
      if (s.fps < 1.0) s.fps = 60.0;
      s.workerThreads = _wtoi(getText(hDlg, IDC_THREADS).c_str());
      if (s.workerThreads < 0) s.workerThreads = 0;
      s.faceSize = comboIndexToFaceSize(
          (int)SendDlgItemMessageW(hDlg, IDC_FACE, CB_GETCURSEL, 0, 0));
      s.writeThreads = comboIndexToWriteThreads(
          (int)SendDlgItemMessageW(hDlg, IDC_WRITE_THREADS, CB_GETCURSEL, 0, 0));
      s.writeQueueMax = comboIndexToWriteQueue(
          (int)SendDlgItemMessageW(hDlg, IDC_WRITE_QUEUE, CB_GETCURSEL, 0, 0));
      s.ipdUnits = (float)_wtof(getText(hDlg, IDC_IPD).c_str());
      s.frameStart = _wtoi(getText(hDlg, IDC_FSTART).c_str());
      s.frameEnd = _wtoi(getText(hDlg, IDC_FEND).c_str());
      s.jobName = sanitizeProjectName(getText(hDlg, IDC_PROJECT));
      s.outputDir = joinJobOutputDir(getText(hDlg, IDC_OUTDIR), s.jobName);
      s.encodeVideo = IsDlgButtonChecked(hDlg, IDC_ENCODE) == BST_CHECKED;
      s.audioPath = getText(hDlg, IDC_AUDIO);
      s.cubemapFaces = IsDlgButtonChecked(hDlg, IDC_CUBEMAP) == BST_CHECKED;
      int codecIdx = (int)SendDlgItemMessageW(hDlg, IDC_ENCODE_CODEC, CB_GETCURSEL, 0, 0);
      if (codecIdx < 0 || codecIdx > (int)EncodeCodec::Amf)
        codecIdx = (int)EncodeCodec::Auto;
      s.encodeCodec = (EncodeCodec)codecIdx;
      g_ok = true;
      DestroyWindow(hDlg);
      return TRUE;
    }
    case IDC_CANCEL:
    case IDCANCEL:
      g_ok = false;
      DestroyWindow(hDlg);
      return TRUE;
    }
    break;
  }
  return FALSE;
}

bool ShowRenderDialog(HWND owner, RenderSettings& settings) {
  g_settings = &settings;
  g_ok = false;
  settings.applyResolutionPreset();
  ui::loadLangPreference();
  if (!g_yellowBrush)
    g_yellowBrush = CreateSolidBrush(RGB(255, 220, 60));

  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LINK_CLASS};
  InitCommonControlsEx(&icc);

  constexpr int kClientW = 660;
  constexpr int kClientH = 800;
  const DWORD style = WS_CAPTION | WS_POPUP | WS_SYSMENU;
  const DWORD exStyle = WS_EX_DLGMODALFRAME;

  WNDCLASSW wc{};
  wc.lpfnWndProc = [](HWND hWnd, UINT m, WPARAM w, LPARAM l) -> LRESULT {
    if (m == WM_CTLCOLORBTN) {
      HWND ctl = (HWND)l;
      if (ctl && GetDlgCtrlID(ctl) == IDC_AUTOFILL) {
        HDC hdc = (HDC)w;
        SetBkColor(hdc, RGB(255, 220, 60));
        SetTextColor(hdc, RGB(20, 20, 20));
        return (LRESULT)g_yellowBrush;
      }
    }
    if (m == WM_CTLCOLORSTATIC) {
      HWND ctl = (HWND)l;
      if (ctl && GetDlgCtrlID(ctl) == IDC_LBL_SAVE_WARN) {
        HDC hdc = (HDC)w;
        SetTextColor(hdc, RGB(200, 0, 0));
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
      }
    }
    if (m == WM_CREATE) {
      const DWORD st = WS_CHILD | WS_VISIBLE;
      const DWORD stMulti = WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX;

      CreateWindowW(L"BUTTON", L"日本語", st, 480, 10, 70, 24, hWnd, (HMENU)IDC_LANG_JA, 0, 0);
      CreateWindowW(L"BUTTON", L"English", st, 560, 10, 70, 24, hWnd, (HMENU)IDC_LANG_EN, 0, 0);

      CreateWindowW(L"STATIC", L"", st, 20, 40, 150, 20, hWnd, (HMENU)IDC_LBL_VRMODE, 0, 0);
      CreateWindowW(L"COMBOBOX", L"", st | CBS_DROPDOWNLIST, 180, 38, 440, 200, hWnd, (HMENU)IDC_VRMODE, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 20, 68, 150, 20, hWnd, (HMENU)IDC_LBL_STEREO, 0, 0);
      CreateWindowW(L"COMBOBOX", L"", st | CBS_DROPDOWNLIST, 180, 66, 440, 200, hWnd, (HMENU)IDC_STEREO, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 20, 96, 150, 20, hWnd, (HMENU)IDC_LBL_QUALITY, 0, 0);
      CreateWindowW(L"COMBOBOX", L"", st | CBS_DROPDOWNLIST, 180, 94, 440, 200, hWnd, (HMENU)IDC_QUALITY, 0, 0);

      CreateWindowW(L"STATIC", L"", st, 20, 124, 150, 20, hWnd, (HMENU)IDC_LBL_RES, 0, 0);
      CreateWindowW(L"COMBOBOX", L"", st | CBS_DROPDOWNLIST, 180, 122, 440, 200, hWnd, (HMENU)IDC_RESOLUTION, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 20, 152, 150, 20, hWnd, (HMENU)IDC_LBL_SIZE, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 180, 152, 440, 20, hWnd, (HMENU)IDC_SIZEINFO, 0, 0);

      CreateWindowW(L"STATIC", L"", st, 20, 180, 150, 20, hWnd, (HMENU)IDC_LBL_FPS, 0, 0);
      CreateWindowW(L"EDIT", L"", st | WS_BORDER, 180, 178, 70, 22, hWnd, (HMENU)IDC_FPS, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 260, 180, 100, 20, hWnd, (HMENU)IDC_LBL_THREADS, 0, 0);
      CreateWindowW(L"EDIT", L"", st | WS_BORDER, 365, 178, 55, 22, hWnd, (HMENU)IDC_THREADS, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 428, 180, 90, 20, hWnd, (HMENU)IDC_LBL_THREADS_HINT, 0, 0);

      CreateWindowW(L"STATIC", L"", st, 20, 208, 150, 20, hWnd, (HMENU)IDC_LBL_FACE, 0, 0);
      CreateWindowW(L"COMBOBOX", L"", st | CBS_DROPDOWNLIST, 180, 206, 160, 200, hWnd, (HMENU)IDC_FACE, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 350, 208, 70, 20, hWnd, (HMENU)IDC_LBL_WRITE, 0, 0);
      CreateWindowW(L"COMBOBOX", L"", st | CBS_DROPDOWNLIST, 425, 206, 60, 200, hWnd, (HMENU)IDC_WRITE_THREADS, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 495, 208, 55, 20, hWnd, (HMENU)IDC_LBL_QUEUE, 0, 0);
      CreateWindowW(L"COMBOBOX", L"", st | CBS_DROPDOWNLIST, 555, 206, 90, 200, hWnd, (HMENU)IDC_WRITE_QUEUE, 0, 0);
      CreateWindowW(L"STATIC", L"", stMulti, 20, 232, 620, 20, hWnd, (HMENU)IDC_LBL_FACE_HINT, 0, 0);

      CreateWindowW(L"STATIC", L"", st, 20, 258, 150, 20, hWnd, (HMENU)IDC_LBL_IPD, 0, 0);
      CreateWindowW(L"EDIT", L"", st | WS_BORDER, 180, 256, 70, 22, hWnd, (HMENU)IDC_IPD, 0, 0);

      CreateWindowW(L"STATIC", L"", st, 20, 286, 150, 20, hWnd, (HMENU)IDC_LBL_FSTART, 0, 0);
      CreateWindowW(L"EDIT", L"", st | WS_BORDER, 180, 284, 100, 22, hWnd, (HMENU)IDC_FSTART, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 300, 286, 150, 20, hWnd, (HMENU)IDC_LBL_FEND, 0, 0);
      CreateWindowW(L"EDIT", L"", st | WS_BORDER, 460, 284, 100, 22, hWnd, (HMENU)IDC_FEND, 0, 0);

      CreateWindowW(L"STATIC", L"", st, 20, 314, 150, 20, hWnd, (HMENU)IDC_LBL_PROJECT, 0, 0);
      CreateWindowW(L"EDIT", L"", st | WS_BORDER | ES_AUTOHSCROLL, 180, 312, 440, 22, hWnd, (HMENU)IDC_PROJECT, 0, 0);

      CreateWindowW(L"STATIC", L"", st, 20, 342, 150, 20, hWnd, (HMENU)IDC_LBL_OUT, 0, 0);
      CreateWindowW(L"EDIT", L"", st | WS_BORDER | ES_AUTOHSCROLL, 180, 340, 400, 22, hWnd, (HMENU)IDC_OUTDIR, 0, 0);
      CreateWindowW(L"BUTTON", L"...", st, 590, 340, 36, 22, hWnd, (HMENU)IDC_BROWSE, 0, 0);

      CreateWindowW(L"BUTTON", L"", st | BS_AUTOCHECKBOX, 180, 370, 440, 22, hWnd, (HMENU)IDC_ENCODE, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 20, 398, 150, 20, hWnd, (HMENU)IDC_LBL_ENCODE_CODEC, 0, 0);
      CreateWindowW(L"COMBOBOX", L"", st | CBS_DROPDOWNLIST, 180, 396, 440, 200, hWnd, (HMENU)IDC_ENCODE_CODEC, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 20, 426, 150, 20, hWnd, (HMENU)IDC_LBL_AUDIO, 0, 0);
      CreateWindowW(L"EDIT", L"", st | WS_BORDER | ES_AUTOHSCROLL, 180, 424, 370, 22, hWnd, (HMENU)IDC_AUDIO, 0, 0);
      CreateWindowW(L"BUTTON", L"...", st, 560, 424, 36, 22, hWnd, (HMENU)IDC_BROWSE_AUDIO, 0, 0);
      CreateWindowW(L"STATIC", L"", stMulti, 20, 450, 620, 18, hWnd, (HMENU)IDC_LBL_AUDIO_HINT, 0, 0);
      CreateWindowW(L"BUTTON", L"", st | BS_AUTOCHECKBOX, 180, 472, 440, 22, hWnd, (HMENU)IDC_CUBEMAP, 0, 0);
      CreateWindowW(L"STATIC", L"", stMulti, 20, 496, 620, 18, hWnd, (HMENU)IDC_LBL_CUBEMAP_HINT, 0, 0);

      HWND autoBtn = CreateWindowW(L"BUTTON", L"", st, 20, 522, 620, 34, hWnd, (HMENU)IDC_AUTOFILL, 0, 0);
      SetWindowTheme(autoBtn, L"", L"");
      CreateWindowW(L"STATIC", L"", stMulti, 20, 560, 620, 20, hWnd, (HMENU)IDC_AUTOFILL_STATUS, 0, 0);

      CreateWindowW(L"BUTTON", L"", st, 20, 588, 190, 36, hWnd, (HMENU)IDC_START, 0, 0);
      CreateWindowW(L"BUTTON", L"", st, 220, 588, 210, 36, hWnd, (HMENU)IDC_CAPTURE, 0, 0);
      CreateWindowW(L"BUTTON", L"", st, 440, 588, 200, 36, hWnd, (HMENU)IDC_CANCEL, 0, 0);

      CreateWindowW(L"STATIC", L"", st, 20, 636, 620, 18, hWnd, (HMENU)IDC_LBL_HINT, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 20, 656, 620, 18, hWnd, (HMENU)IDC_LBL_RESNOTE, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 20, 682, 140, 18, hWnd, (HMENU)IDC_ISSUES_LEAD, 0, 0);
      CreateWindowW(WC_LINK, L"", st, 165, 682, 200, 18, hWnd, (HMENU)IDC_ISSUES_LINK, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 20, 708, 620, 18, hWnd, (HMENU)IDC_LBL_SPEC, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 20, 730, 620, 18, hWnd, (HMENU)IDC_LBL_PR, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 20, 756, 620, 22, hWnd, (HMENU)IDC_LBL_SAVE_WARN, 0, 0);

      DlgProc(hWnd, WM_INITDIALOG, 0, 0);
      ui_layout::FitClientSize(hWnd, 660, 800);
      return 0;
    }
    if (m == WM_NOTIFY) {
      auto* hdr = reinterpret_cast<LPNMHDR>(l);
      if (hdr && hdr->idFrom == IDC_ISSUES_LINK &&
          (hdr->code == NM_CLICK || hdr->code == NM_RETURN)) {
        auto* link = reinterpret_cast<PNMLINK>(l);
        ShellExecuteW(hWnd, L"open", link->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
      }
    }
    if (m == WM_COMMAND) {
      if (DlgProc(hWnd, WM_COMMAND, w, 0)) {
        if (g_ok || LOWORD(w) == IDC_CANCEL || LOWORD(w) == IDCANCEL)
          DestroyWindow(hWnd);
        return 0;
      }
    }
    if (m == WM_CLOSE) { g_ok = false; DestroyWindow(hWnd); return 0; }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hWnd, m, w, l);
  };
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"MMDVRRenderDialog";
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&wc);

  RECT rc{0, 0, kClientW, kClientH};
  AdjustWindowRectEx(&rc, style, FALSE, exStyle);

  HWND hwnd = CreateWindowExW(exStyle, L"MMDVRRenderDialog", ui::t().dialogTitle, style,
                              CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                              owner, nullptr, wc.hInstance, nullptr);
  ui_layout::FitClientSize(hwnd, kClientW, kClientH);
  ShowWindow(hwnd, SW_SHOW);
  EnableWindow(owner, FALSE);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  EnableWindow(owner, TRUE);
  SetForegroundWindow(owner);
  return g_ok;
}

} // namespace mmdvr
