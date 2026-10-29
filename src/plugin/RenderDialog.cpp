#include "RenderDialog.hpp"
#include "UiStrings.hpp"
#include "WinUiLayout.hpp"
#include "HwSuggest.hpp"
#include "mmdvr/Diagnostics.hpp"
#include "mmdvr/VideoEncoder.hpp"
#include <CommCtrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace fs = std::filesystem;

namespace mmdvr {

enum {
  IDC_TABS = 1090,
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
  IDC_LBL_PROJECT = 1124,
  IDC_REENCODE_HINT = 1301,
  IDC_REENCODE_LBL_PROJECT = 1302,
  IDC_REENCODE_PROJECT = 1303,
  IDC_REENCODE_BROWSE = 1304,
  IDC_REENCODE_LBL_FPS = 1305,
  IDC_REENCODE_FPS = 1306,
  IDC_REENCODE_LBL_CODEC = 1307,
  IDC_REENCODE_CODEC = 1308,
  IDC_REENCODE_LBL_AUDIO = 1309,
  IDC_REENCODE_AUDIO = 1310,
  IDC_REENCODE_BROWSE_AUDIO = 1311,
  IDC_REENCODE_SPHERICAL = 1312,
  IDC_REENCODE_LBL_VR = 1313,
  IDC_REENCODE_VRMODE = 1314,
  IDC_REENCODE_LBL_STEREO = 1315,
  IDC_REENCODE_STEREO = 1316,
  IDC_REENCODE_GO = 1317,
  IDC_REENCODE_STATUS = 1318
};

constexpr UINT WM_REENCODE_DONE = WM_APP + 41;

static RenderSettings* g_settings = nullptr;
static bool g_ok = false;
static HBRUSH g_yellowBrush = nullptr;
static HFONT g_uiFont = nullptr;
static std::atomic<bool> g_reencodeBusy{false};
static std::wstring g_reencodeResult;
static std::wstring g_reencodeError;
static bool g_reencodeOk = false;

static const int kRenderPageIds[] = {
    IDC_LBL_VRMODE, IDC_VRMODE, IDC_LBL_STEREO, IDC_STEREO, IDC_LBL_QUALITY, IDC_QUALITY,
    IDC_LBL_RES, IDC_RESOLUTION, IDC_LBL_SIZE, IDC_SIZEINFO, IDC_LBL_FPS, IDC_FPS,
    IDC_LBL_THREADS, IDC_THREADS, IDC_LBL_THREADS_HINT, IDC_LBL_FACE, IDC_FACE,
    IDC_LBL_WRITE, IDC_WRITE_THREADS, IDC_LBL_QUEUE, IDC_WRITE_QUEUE, IDC_LBL_FACE_HINT,
    IDC_LBL_IPD, IDC_IPD, IDC_LBL_FSTART, IDC_FSTART, IDC_LBL_FEND, IDC_FEND,
    IDC_LBL_PROJECT, IDC_PROJECT, IDC_LBL_OUT, IDC_OUTDIR, IDC_BROWSE, IDC_ENCODE,
    IDC_LBL_ENCODE_CODEC, IDC_ENCODE_CODEC, IDC_LBL_AUDIO, IDC_AUDIO, IDC_BROWSE_AUDIO,
    IDC_LBL_AUDIO_HINT, IDC_CUBEMAP, IDC_LBL_CUBEMAP_HINT, IDC_AUTOFILL, IDC_AUTOFILL_STATUS,
    IDC_START, IDC_CAPTURE, IDC_LBL_HINT, IDC_LBL_RESNOTE, IDC_ISSUES_LEAD, IDC_ISSUES_LINK,
    IDC_LBL_SPEC, IDC_LBL_PR, IDC_LBL_SAVE_WARN};
static const int kReencodePageIds[] = {
    IDC_REENCODE_HINT, IDC_REENCODE_LBL_PROJECT, IDC_REENCODE_PROJECT, IDC_REENCODE_BROWSE,
    IDC_REENCODE_LBL_FPS, IDC_REENCODE_FPS, IDC_REENCODE_LBL_CODEC, IDC_REENCODE_CODEC,
    IDC_REENCODE_LBL_AUDIO, IDC_REENCODE_AUDIO, IDC_REENCODE_BROWSE_AUDIO,
    IDC_REENCODE_SPHERICAL, IDC_REENCODE_LBL_VR, IDC_REENCODE_VRMODE,
    IDC_REENCODE_LBL_STEREO, IDC_REENCODE_STEREO, IDC_REENCODE_GO, IDC_REENCODE_STATUS};

static void showDialogPage(HWND hDlg, int page) {
  for (int id : kRenderPageIds) {
    HWND c = GetDlgItem(hDlg, id);
    if (c) ShowWindow(c, page == 0 ? SW_SHOW : SW_HIDE);
  }
  for (int id : kReencodePageIds) {
    HWND c = GetDlgItem(hDlg, id);
    if (c) ShowWindow(c, page == 1 ? SW_SHOW : SW_HIDE);
  }
  // Cancel stays visible on both pages.
  HWND cancel = GetDlgItem(hDlg, IDC_CANCEL);
  if (cancel) ShowWindow(cancel, SW_SHOW);
}

static std::wstring resolveFramesDir(std::wstring path) {
  while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
    path.pop_back();
  if (path.empty()) return {};
  std::error_code ec;
  auto hasFrames = [&](const fs::path& dir) {
    if (!fs::is_directory(dir, ec)) return false;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); ++it) {
      if (!it->is_regular_file(ec)) continue;
      auto name = it->path().filename().wstring();
      if (name.rfind(L"frame_", 0) != 0) continue;
      auto ext = it->path().extension().wstring();
      if (_wcsicmp(ext.c_str(), L".png") == 0 || _wcsicmp(ext.c_str(), L".jpg") == 0 ||
          _wcsicmp(ext.c_str(), L".jpeg") == 0)
        return true;
    }
    return false;
  };
  fs::path p(path);
  if (hasFrames(p)) return p.wstring();
  if (hasFrames(p / L"frames")) return (p / L"frames").wstring();
  if (p.filename() == L"frames" && hasFrames(p)) return p.wstring();
  return {};
}

static HFONT ensureUiFont() {
  if (g_uiFont) return g_uiFont;
  // Compact 9pt — default system UI font reads large on this dense form.
  g_uiFont = CreateFontW(
      -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  if (!g_uiFont)
    g_uiFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  return g_uiFont;
}

static void applyUiFont(HWND root) {
  HFONT font = ensureUiFont();
  EnumChildWindows(
      root,
      [](HWND child, LPARAM lp) -> BOOL {
        SendMessageW(child, WM_SETFONT, (WPARAM)lp, TRUE);
        return TRUE;
      },
      (LPARAM)font);
}

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
// 0 = auto-cap pending frames from free RAM / frame size.
constexpr int kWriteQueuePresets[] = {0, 1, 2, 3, 4, 6, 8, 10, 12};
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
  wchar_t buf[256];
  if (stereoIdx == 1)
    swprintf(buf, 256, ui::t().sizeStereoFmt, w, h, w * 2, h);
  else
    swprintf(buf, 256, ui::t().sizeMonoFmt, w, h);
  // Append localized 8K storage warning on the size line.
  if (resIdx == (int)ResolutionPreset::Res8K) {
    wcsncat_s(buf, 256, L"  · ", _TRUNCATE);
    wcsncat_s(buf, 256, ui::t().res8KWarn, _TRUNCATE);
  }
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
  setText(hDlg, IDC_REENCODE_HINT, s.reencodeHint);
  setText(hDlg, IDC_REENCODE_LBL_PROJECT, s.labelReencodeProject);
  setText(hDlg, IDC_REENCODE_BROWSE, s.btnBrowse);
  setText(hDlg, IDC_REENCODE_LBL_FPS, s.labelFps);
  setText(hDlg, IDC_REENCODE_LBL_CODEC, s.labelEncodeCodec);
  setText(hDlg, IDC_REENCODE_LBL_AUDIO, s.labelAudio);
  setText(hDlg, IDC_REENCODE_BROWSE_AUDIO, s.btnBrowse);
  setText(hDlg, IDC_REENCODE_SPHERICAL, s.chkReencodeSpherical);
  setText(hDlg, IDC_REENCODE_LBL_VR, s.labelVrMode);
  setText(hDlg, IDC_REENCODE_LBL_STEREO, s.labelStereo);
  setText(hDlg, IDC_REENCODE_GO, s.btnReencode);

  HWND tabs = GetDlgItem(hDlg, IDC_TABS);
  if (tabs) {
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    wchar_t t0[64], t1[64];
    wcsncpy_s(t0, s.tabRender, _TRUNCATE);
    wcsncpy_s(t1, s.tabReencode, _TRUNCATE);
    item.pszText = t0;
    TabCtrl_SetItem(tabs, 0, &item);
    item.pszText = t1;
    TabCtrl_SetItem(tabs, 1, &item);
  }

  int stereoSel = (int)SendDlgItemMessageW(hDlg, IDC_STEREO, CB_GETCURSEL, 0, 0);
  int qualitySel = (int)SendDlgItemMessageW(hDlg, IDC_QUALITY, CB_GETCURSEL, 0, 0);
  int resSel = (int)SendDlgItemMessageW(hDlg, IDC_RESOLUTION, CB_GETCURSEL, 0, 0);
  int codecSel = (int)SendDlgItemMessageW(hDlg, IDC_ENCODE_CODEC, CB_GETCURSEL, 0, 0);
  const wchar_t* stereoItems[] = { s.comboMono, s.comboStereo };
  const wchar_t* qualityItems[] = { s.comboHigh, s.comboBalanced, s.comboFast };
  const wchar_t* resItems[] = { s.comboRes2K, s.comboRes3K, s.comboRes4K, s.comboRes8K };
  const wchar_t* codecItems[] = {
    s.comboCodecLibx264, s.comboCodecAuto, s.comboCodecNvenc, s.comboCodecQsv, s.comboCodecAmf
  };
  refillCombo(hDlg, IDC_STEREO, stereoItems, 2, stereoSel);
  refillCombo(hDlg, IDC_QUALITY, qualityItems, 3, qualitySel);
  refillCombo(hDlg, IDC_RESOLUTION, resItems, 4, resSel);
  refillCombo(hDlg, IDC_ENCODE_CODEC, codecItems, 5, codecSel < 0 ? 0 : codecSel);
  int reCodecSel = (int)SendDlgItemMessageW(hDlg, IDC_REENCODE_CODEC, CB_GETCURSEL, 0, 0);
  int reStereoSel = (int)SendDlgItemMessageW(hDlg, IDC_REENCODE_STEREO, CB_GETCURSEL, 0, 0);
  refillCombo(hDlg, IDC_REENCODE_CODEC, codecItems, 5, reCodecSel < 0 ? (int)EncodeCodec::Auto : reCodecSel);
  refillCombo(hDlg, IDC_REENCODE_STEREO, stereoItems, 2, reStereoSel < 0 ? 1 : reStereoSel);
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
    applyUiFont(hDlg);
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
    // Prefer the job folder (…\project) so frames\ resolves automatically.
    if (!s.outputDir.empty())
      setText(hDlg, IDC_REENCODE_PROJECT, s.outputDir);
    else {
      const std::wstring project =
          s.jobName.empty() || s.jobName == L"__capture_only__" ? L"mmdvr_job" : s.jobName;
      setText(hDlg, IDC_REENCODE_PROJECT, joinJobOutputDir(getText(hDlg, IDC_OUTDIR), project));
    }
    setText(hDlg, IDC_REENCODE_FPS, std::to_wstring((int)(s.fps > 0 ? s.fps : 60)));
    SendDlgItemMessageW(hDlg, IDC_REENCODE_CODEC, CB_SETCURSEL, (int)s.encodeCodec, 0);
    setText(hDlg, IDC_REENCODE_AUDIO, s.audioPath);
    CheckDlgButton(hDlg, IDC_REENCODE_SPHERICAL, BST_CHECKED);
    SendDlgItemMessageW(hDlg, IDC_REENCODE_VRMODE, CB_RESETCONTENT, 0, 0);
    SendDlgItemMessageW(hDlg, IDC_REENCODE_VRMODE, CB_ADDSTRING, 0, (LPARAM)L"VR180");
    SendDlgItemMessageW(hDlg, IDC_REENCODE_VRMODE, CB_ADDSTRING, 0, (LPARAM)L"VR360");
    SendDlgItemMessageW(hDlg, IDC_REENCODE_VRMODE, CB_SETCURSEL, (s.vrMode == VrMode::VR360) ? 1 : 0, 0);
    SendDlgItemMessageW(hDlg, IDC_REENCODE_STEREO, CB_SETCURSEL, (s.stereoMode == StereoMode::Stereo) ? 1 : 0, 0);
    showDialogPage(hDlg, 0);
    // Probe so Auto/NVENC reflect this PC's FFmpeg (smoke uses 512² for NVENC min size).
    VideoEncoder::probe(s.ffmpegPath);
    return TRUE;
  }
  case WM_COMMAND:
    switch (LOWORD(wParam)) {
    case IDC_LANG_JA:
      ui::setLang(ui::Lang::Ja);
      applyLabels(hDlg);
      // Refresh autofill blurb in the new language if it was already shown.
      if (GetWindowTextLengthW(GetDlgItem(hDlg, IDC_AUTOFILL_STATUS)) > 0)
        setText(hDlg, IDC_AUTOFILL_STATUS, suggestFromThisPc().summary);
      return TRUE;
    case IDC_LANG_EN:
      ui::setLang(ui::Lang::En);
      applyLabels(hDlg);
      if (GetWindowTextLengthW(GetDlgItem(hDlg, IDC_AUTOFILL_STATUS)) > 0)
        setText(hDlg, IDC_AUTOFILL_STATUS, suggestFromThisPc().summary);
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
    case IDC_REENCODE_BROWSE: {
      wchar_t file[MAX_PATH] = L"project";
      OPENFILENAMEW ofn{};
      ofn.lStructSize = sizeof(ofn);
      ofn.hwndOwner = hDlg;
      ofn.lpstrFile = file;
      ofn.nMaxFile = MAX_PATH;
      ofn.lpstrFilter = ui::kBrowseFilter;
      ofn.Flags = OFN_PATHMUSTEXIST;
      ofn.lpstrTitle = ui::t().browseProjectTitle;
      if (GetSaveFileNameW(&ofn)) {
        std::wstring p(file);
        auto slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos) p = p.substr(0, slash);
        setText(hDlg, IDC_REENCODE_PROJECT, p);
      }
      return TRUE;
    }
    case IDC_REENCODE_BROWSE_AUDIO: {
      wchar_t file[MAX_PATH] = L"";
      OPENFILENAMEW ofn{};
      ofn.lStructSize = sizeof(ofn);
      ofn.hwndOwner = hDlg;
      ofn.lpstrFile = file;
      ofn.nMaxFile = MAX_PATH;
      ofn.lpstrFilter = ui::kAudioFilter;
      ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
      ofn.lpstrTitle = ui::t().browseAudioTitle;
      if (GetOpenFileNameW(&ofn))
        setText(hDlg, IDC_REENCODE_AUDIO, file);
      return TRUE;
    }
    case IDC_REENCODE_GO: {
      if (g_reencodeBusy.load()) {
        MessageBoxW(hDlg, ui::t().busy, ui::t().appName, MB_OK);
        return TRUE;
      }
      const std::wstring projectPath = getText(hDlg, IDC_REENCODE_PROJECT);
      const std::wstring frames = resolveFramesDir(projectPath);
      if (frames.empty()) {
        MessageBoxW(hDlg, ui::t().reencodeNoFrames, ui::t().appName, MB_OK | MB_ICONWARNING);
        return TRUE;
      }
      fs::path framesPath(frames);
      fs::path jobRoot =
          (framesPath.filename() == L"frames") ? framesPath.parent_path() : framesPath;
      const std::wstring jobName =
          jobRoot.filename().empty() ? L"mmdvr_job" : jobRoot.filename().wstring();
      const std::wstring outMp4 = (jobRoot / (jobName + L".mp4")).wstring();
      const std::wstring tmpMp4 = (jobRoot / (jobName + L"_raw.mp4")).wstring();
      double fps = _wtof(getText(hDlg, IDC_REENCODE_FPS).c_str());
      if (fps < 1.0) fps = 60.0;
      int codecIdx = (int)SendDlgItemMessageW(hDlg, IDC_REENCODE_CODEC, CB_GETCURSEL, 0, 0);
      if (codecIdx < 0 || codecIdx > (int)EncodeCodec::Amf)
        codecIdx = (int)EncodeCodec::Auto;
      const EncodeCodec codec = (EncodeCodec)codecIdx;
      const std::wstring audio = getText(hDlg, IDC_REENCODE_AUDIO);
      const bool wantMeta = IsDlgButtonChecked(hDlg, IDC_REENCODE_SPHERICAL) == BST_CHECKED;
      const VrMode vr =
          SendDlgItemMessageW(hDlg, IDC_REENCODE_VRMODE, CB_GETCURSEL, 0, 0) == 1
              ? VrMode::VR360
              : VrMode::VR180;
      const StereoMode stereo =
          SendDlgItemMessageW(hDlg, IDC_REENCODE_STEREO, CB_GETCURSEL, 0, 0) == 1
              ? StereoMode::Stereo
              : StereoMode::Mono;
      const std::wstring ffmpeg = g_settings ? g_settings->ffmpegPath : L"";

      g_reencodeBusy = true;
      g_reencodeOk = false;
      g_reencodeResult.clear();
      g_reencodeError.clear();
      setText(hDlg, IDC_REENCODE_STATUS, ui::t().reencodeRunning);
      EnableWindow(GetDlgItem(hDlg, IDC_REENCODE_GO), FALSE);

      HWND hwnd = hDlg;
      std::thread([=]() {
        std::wstring err, fallback;
        int encW = 0, encH = 0;
        bool ok = VideoEncoder::encodeSequence(
            frames, L"frame_%06d.png", tmpMp4, fps, ffmpeg, codec, audio, 0.0, &err, &fallback,
            &encW, &encH);
        std::wstring finalOut = outMp4;
        if (ok && wantMeta) {
          std::wstring metaErr;
          if (!VideoEncoder::injectVrMetadata(tmpMp4, finalOut, vr, stereo, encW, encH, ffmpeg,
                                              &metaErr)) {
            Diagnostics::instance().warn("Re-encode VR metadata inject failed; keeping raw mp4");
            finalOut = tmpMp4;
            if (!metaErr.empty()) err += L"\n" + metaErr;
          } else {
            std::error_code ec;
            fs::remove(tmpMp4, ec);
          }
        } else if (ok) {
          std::error_code ec;
          fs::remove(finalOut, ec);
          fs::rename(tmpMp4, finalOut, ec);
          if (ec) finalOut = tmpMp4;
        }
        g_reencodeOk = ok;
        g_reencodeResult = finalOut;
        g_reencodeError = err;
        if (!fallback.empty()) {
          if (!g_reencodeError.empty()) g_reencodeError += L"\n";
          g_reencodeError += fallback;
        }
        PostMessageW(hwnd, WM_REENCODE_DONE, ok ? 1 : 0, 0);
      }).detach();
      return TRUE;
    }
    case IDC_CANCEL:
    case IDCANCEL:
      if (g_reencodeBusy.load()) {
        MessageBoxW(hDlg, ui::t().reencodeRunning, ui::t().appName, MB_OK);
        return TRUE;
      }
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

  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LINK_CLASS | ICC_TAB_CLASSES};
  InitCommonControlsEx(&icc);

  constexpr int kClientW = 700;
  constexpr int kClientH = 920;
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
    if (m == WM_REENCODE_DONE) {
      g_reencodeBusy = false;
      EnableWindow(GetDlgItem(hWnd, IDC_REENCODE_GO), TRUE);
      wchar_t buf[1024]{};
      if (w) {
        std::swprintf(buf, 1024, ui::t().reencodeOkFmt, g_reencodeResult.c_str());
        setText(hWnd, IDC_REENCODE_STATUS, buf);
        MessageBoxW(hWnd, buf, ui::t().appName, MB_OK | MB_ICONINFORMATION);
      } else {
        std::swprintf(buf, 1024, ui::t().reencodeFailFmt, g_reencodeError.c_str());
        setText(hWnd, IDC_REENCODE_STATUS, buf);
        MessageBoxW(hWnd, buf, ui::t().appName, MB_OK | MB_ICONERROR);
      }
      return 0;
    }
    if (m == WM_CREATE) {
      const DWORD st = WS_CHILD | WS_VISIBLE;
      const DWORD stHidden = WS_CHILD; // re-encode page; shown on tab switch
      const DWORD stMulti = WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX;
      const DWORD stMultiH = WS_CHILD | SS_LEFT | SS_NOPREFIX;
      const DWORD stChk = st | BS_AUTOCHECKBOX | BS_MULTILINE | BS_TOP;
      const DWORD stChkH = stHidden | BS_AUTOCHECKBOX | BS_MULTILINE | BS_TOP;

      CreateWindowW(WC_TABCONTROLW, L"", st, 16, 6, 490, 26, hWnd, (HMENU)IDC_TABS, 0, 0);
      {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(L"Render");
        TabCtrl_InsertItem(GetDlgItem(hWnd, IDC_TABS), 0, &item);
        item.pszText = const_cast<wchar_t*>(L"Re-encode");
        TabCtrl_InsertItem(GetDlgItem(hWnd, IDC_TABS), 1, &item);
      }
      CreateWindowW(L"BUTTON", L"日本語", st, 520, 8, 70, 22, hWnd, (HMENU)IDC_LANG_JA, 0, 0);
      CreateWindowW(L"BUTTON", L"English", st, 598, 8, 70, 22, hWnd, (HMENU)IDC_LANG_EN, 0, 0);

      CreateWindowW(L"STATIC", L"", st, 16, 36, 140, 18, hWnd, (HMENU)IDC_LBL_VRMODE, 0, 0);
      CreateWindowW(L"COMBOBOX", L"", st | CBS_DROPDOWNLIST, 170, 34, 500, 200, hWnd, (HMENU)IDC_VRMODE, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 16, 60, 140, 18, hWnd, (HMENU)IDC_LBL_STEREO, 0, 0);
      CreateWindowW(L"COMBOBOX", L"", st | CBS_DROPDOWNLIST, 170, 58, 500, 200, hWnd, (HMENU)IDC_STEREO, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 16, 84, 140, 18, hWnd, (HMENU)IDC_LBL_QUALITY, 0, 0);
      CreateWindowW(L"COMBOBOX", L"", st | CBS_DROPDOWNLIST, 170, 82, 500, 200, hWnd, (HMENU)IDC_QUALITY, 0, 0);

      CreateWindowW(L"STATIC", L"", st, 16, 108, 140, 18, hWnd, (HMENU)IDC_LBL_RES, 0, 0);
      CreateWindowW(L"COMBOBOX", L"", st | CBS_DROPDOWNLIST, 170, 106, 500, 200, hWnd, (HMENU)IDC_RESOLUTION, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 16, 132, 140, 32, hWnd, (HMENU)IDC_LBL_SIZE, 0, 0);
      CreateWindowW(L"STATIC", L"", stMulti, 170, 132, 500, 32, hWnd, (HMENU)IDC_SIZEINFO, 0, 0);

      CreateWindowW(L"STATIC", L"", st, 16, 170, 140, 18, hWnd, (HMENU)IDC_LBL_FPS, 0, 0);
      CreateWindowW(L"EDIT", L"", st | WS_BORDER, 170, 168, 64, 20, hWnd, (HMENU)IDC_FPS, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 250, 170, 90, 18, hWnd, (HMENU)IDC_LBL_THREADS, 0, 0);
      CreateWindowW(L"EDIT", L"", st | WS_BORDER, 350, 168, 50, 20, hWnd, (HMENU)IDC_THREADS, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 410, 170, 90, 18, hWnd, (HMENU)IDC_LBL_THREADS_HINT, 0, 0);

      CreateWindowW(L"STATIC", L"", st, 16, 196, 140, 18, hWnd, (HMENU)IDC_LBL_FACE, 0, 0);
      CreateWindowW(L"COMBOBOX", L"", st | CBS_DROPDOWNLIST, 170, 194, 160, 200, hWnd, (HMENU)IDC_FACE, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 345, 196, 60, 18, hWnd, (HMENU)IDC_LBL_WRITE, 0, 0);
      CreateWindowW(L"COMBOBOX", L"", st | CBS_DROPDOWNLIST, 410, 194, 50, 200, hWnd, (HMENU)IDC_WRITE_THREADS, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 475, 196, 45, 18, hWnd, (HMENU)IDC_LBL_QUEUE, 0, 0);
      CreateWindowW(L"COMBOBOX", L"", st | CBS_DROPDOWNLIST, 525, 194, 110, 200, hWnd, (HMENU)IDC_WRITE_QUEUE, 0, 0);
      CreateWindowW(L"STATIC", L"", stMulti, 16, 218, 650, 30, hWnd, (HMENU)IDC_LBL_FACE_HINT, 0, 0);

      CreateWindowW(L"STATIC", L"", st, 16, 254, 140, 18, hWnd, (HMENU)IDC_LBL_IPD, 0, 0);
      CreateWindowW(L"EDIT", L"", st | WS_BORDER, 170, 252, 64, 20, hWnd, (HMENU)IDC_IPD, 0, 0);

      CreateWindowW(L"STATIC", L"", st, 16, 278, 140, 18, hWnd, (HMENU)IDC_LBL_FSTART, 0, 0);
      CreateWindowW(L"EDIT", L"", st | WS_BORDER, 170, 276, 90, 20, hWnd, (HMENU)IDC_FSTART, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 290, 278, 140, 18, hWnd, (HMENU)IDC_LBL_FEND, 0, 0);
      CreateWindowW(L"EDIT", L"", st | WS_BORDER, 440, 276, 90, 20, hWnd, (HMENU)IDC_FEND, 0, 0);

      CreateWindowW(L"STATIC", L"", st, 16, 304, 140, 18, hWnd, (HMENU)IDC_LBL_PROJECT, 0, 0);
      CreateWindowW(L"EDIT", L"", st | WS_BORDER | ES_AUTOHSCROLL, 170, 302, 500, 20, hWnd, (HMENU)IDC_PROJECT, 0, 0);

      CreateWindowW(L"STATIC", L"", st, 16, 330, 140, 18, hWnd, (HMENU)IDC_LBL_OUT, 0, 0);
      CreateWindowW(L"EDIT", L"", st | WS_BORDER | ES_AUTOHSCROLL, 170, 328, 460, 20, hWnd, (HMENU)IDC_OUTDIR, 0, 0);
      CreateWindowW(L"BUTTON", L"...", st, 640, 328, 32, 20, hWnd, (HMENU)IDC_BROWSE, 0, 0);

      CreateWindowW(L"BUTTON", L"", stChk, 170, 356, 500, 32, hWnd, (HMENU)IDC_ENCODE, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 16, 396, 140, 18, hWnd, (HMENU)IDC_LBL_ENCODE_CODEC, 0, 0);
      CreateWindowW(L"COMBOBOX", L"", st | CBS_DROPDOWNLIST, 170, 394, 500, 200, hWnd, (HMENU)IDC_ENCODE_CODEC, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 16, 422, 140, 18, hWnd, (HMENU)IDC_LBL_AUDIO, 0, 0);
      CreateWindowW(L"EDIT", L"", st | WS_BORDER | ES_AUTOHSCROLL, 170, 420, 430, 20, hWnd, (HMENU)IDC_AUDIO, 0, 0);
      CreateWindowW(L"BUTTON", L"...", st, 610, 420, 32, 20, hWnd, (HMENU)IDC_BROWSE_AUDIO, 0, 0);
      CreateWindowW(L"STATIC", L"", stMulti, 16, 446, 650, 28, hWnd, (HMENU)IDC_LBL_AUDIO_HINT, 0, 0);
      CreateWindowW(L"BUTTON", L"", stChk, 170, 480, 500, 32, hWnd, (HMENU)IDC_CUBEMAP, 0, 0);
      CreateWindowW(L"STATIC", L"", stMulti, 16, 516, 650, 28, hWnd, (HMENU)IDC_LBL_CUBEMAP_HINT, 0, 0);

      HWND autoBtn = CreateWindowW(L"BUTTON", L"", st | BS_MULTILINE, 16, 552, 650, 34, hWnd, (HMENU)IDC_AUTOFILL, 0, 0);
      SetWindowTheme(autoBtn, L"", L"");
      CreateWindowW(L"STATIC", L"", stMulti, 16, 590, 650, 30, hWnd, (HMENU)IDC_AUTOFILL_STATUS, 0, 0);

      CreateWindowW(L"BUTTON", L"", st, 16, 628, 200, 32, hWnd, (HMENU)IDC_START, 0, 0);
      CreateWindowW(L"BUTTON", L"", st, 230, 628, 210, 32, hWnd, (HMENU)IDC_CAPTURE, 0, 0);
      CreateWindowW(L"BUTTON", L"", st, 454, 628, 212, 32, hWnd, (HMENU)IDC_CANCEL, 0, 0);

      CreateWindowW(L"STATIC", L"", stMulti, 16, 670, 650, 28, hWnd, (HMENU)IDC_LBL_HINT, 0, 0);
      CreateWindowW(L"STATIC", L"", stMulti, 16, 700, 650, 28, hWnd, (HMENU)IDC_LBL_RESNOTE, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 16, 734, 130, 16, hWnd, (HMENU)IDC_ISSUES_LEAD, 0, 0);
      CreateWindowW(WC_LINK, L"", st, 150, 734, 200, 16, hWnd, (HMENU)IDC_ISSUES_LINK, 0, 0);
      CreateWindowW(L"STATIC", L"", stMulti, 16, 756, 650, 28, hWnd, (HMENU)IDC_LBL_SPEC, 0, 0);
      CreateWindowW(L"STATIC", L"", st, 16, 788, 650, 16, hWnd, (HMENU)IDC_LBL_PR, 0, 0);
      CreateWindowW(L"STATIC", L"", stMulti, 16, 808, 650, 32, hWnd, (HMENU)IDC_LBL_SAVE_WARN, 0, 0);

      // --- Re-encode tab (hidden until selected) ---
      CreateWindowW(L"STATIC", L"", stMultiH, 16, 44, 650, 40, hWnd, (HMENU)IDC_REENCODE_HINT, 0, 0);
      CreateWindowW(L"STATIC", L"", stHidden, 16, 96, 140, 18, hWnd, (HMENU)IDC_REENCODE_LBL_PROJECT, 0, 0);
      CreateWindowW(L"EDIT", L"", stHidden | WS_BORDER | ES_AUTOHSCROLL, 170, 94, 460, 20, hWnd,
                    (HMENU)IDC_REENCODE_PROJECT, 0, 0);
      CreateWindowW(L"BUTTON", L"...", stHidden, 640, 94, 32, 20, hWnd, (HMENU)IDC_REENCODE_BROWSE, 0, 0);
      CreateWindowW(L"STATIC", L"", stHidden, 16, 128, 140, 18, hWnd, (HMENU)IDC_REENCODE_LBL_FPS, 0, 0);
      CreateWindowW(L"EDIT", L"", stHidden | WS_BORDER, 170, 126, 64, 20, hWnd, (HMENU)IDC_REENCODE_FPS, 0, 0);
      CreateWindowW(L"STATIC", L"", stHidden, 16, 160, 140, 18, hWnd, (HMENU)IDC_REENCODE_LBL_CODEC, 0, 0);
      CreateWindowW(L"COMBOBOX", L"", stHidden | CBS_DROPDOWNLIST, 170, 158, 500, 200, hWnd,
                    (HMENU)IDC_REENCODE_CODEC, 0, 0);
      CreateWindowW(L"STATIC", L"", stHidden, 16, 192, 140, 18, hWnd, (HMENU)IDC_REENCODE_LBL_AUDIO, 0, 0);
      CreateWindowW(L"EDIT", L"", stHidden | WS_BORDER | ES_AUTOHSCROLL, 170, 190, 430, 20, hWnd,
                    (HMENU)IDC_REENCODE_AUDIO, 0, 0);
      CreateWindowW(L"BUTTON", L"...", stHidden, 610, 190, 32, 20, hWnd, (HMENU)IDC_REENCODE_BROWSE_AUDIO,
                    0, 0);
      CreateWindowW(L"BUTTON", L"", stChkH, 170, 224, 500, 28, hWnd, (HMENU)IDC_REENCODE_SPHERICAL, 0, 0);
      CreateWindowW(L"STATIC", L"", stHidden, 16, 264, 140, 18, hWnd, (HMENU)IDC_REENCODE_LBL_VR, 0, 0);
      CreateWindowW(L"COMBOBOX", L"", stHidden | CBS_DROPDOWNLIST, 170, 262, 200, 200, hWnd,
                    (HMENU)IDC_REENCODE_VRMODE, 0, 0);
      CreateWindowW(L"STATIC", L"", stHidden, 390, 264, 80, 18, hWnd, (HMENU)IDC_REENCODE_LBL_STEREO, 0, 0);
      CreateWindowW(L"COMBOBOX", L"", stHidden | CBS_DROPDOWNLIST, 480, 262, 190, 200, hWnd,
                    (HMENU)IDC_REENCODE_STEREO, 0, 0);
      CreateWindowW(L"BUTTON", L"", stHidden, 16, 310, 300, 32, hWnd, (HMENU)IDC_REENCODE_GO, 0, 0);
      CreateWindowW(L"STATIC", L"", stMultiH, 16, 356, 650, 80, hWnd, (HMENU)IDC_REENCODE_STATUS, 0, 0);

      DlgProc(hWnd, WM_INITDIALOG, 0, 0);
      ui_layout::FitClientSize(hWnd, 700, 850);
      return 0;
    }
    if (m == WM_NOTIFY) {
      auto* hdr = reinterpret_cast<LPNMHDR>(l);
      if (hdr && hdr->idFrom == IDC_TABS && hdr->code == TCN_SELCHANGE) {
        showDialogPage(hWnd, TabCtrl_GetCurSel(hdr->hwndFrom));
        return 0;
      }
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
    if (m == WM_DESTROY) {
      if (g_uiFont) {
        DeleteObject(g_uiFont);
        g_uiFont = nullptr;
      }
      PostQuitMessage(0);
      return 0;
    }
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
