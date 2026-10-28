#pragma once
// UI language: Japanese or English (toggle in dialog). Compile with /utf-8.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <string>

namespace mmdvr::ui {

enum class Lang { Ja = 0, En = 1 };

void setLang(Lang lang);
Lang lang();
bool isJa();
void loadLangPreference();
void saveLangPreference();

struct Bundle {
  const wchar_t* appName;
  const wchar_t* menuRoot;
  const wchar_t* menuOpen;
  const wchar_t* menuCapture;
  const wchar_t* menuProbe;
  const wchar_t* menuAbout;
  const wchar_t* aboutTitle;
  const wchar_t* aboutBody;
  const wchar_t* dialogTitle;
  const wchar_t* labelVrMode;
  const wchar_t* labelStereo;
  const wchar_t* labelQuality;
  const wchar_t* labelResolution;
  const wchar_t* comboRes2K;
  const wchar_t* comboRes3K;
  const wchar_t* comboRes4K;
  const wchar_t* comboRes8K;
  const wchar_t* labelSizeHint;
  const wchar_t* labelFps;
  const wchar_t* labelIpd;
  const wchar_t* labelThreads;
  const wchar_t* labelFace;
  const wchar_t* faceHint;
  const wchar_t* labelWriteThreads;
  const wchar_t* labelWriteQueue;
  const wchar_t* labelFrameStart;
  const wchar_t* labelFrameEnd;
  const wchar_t* labelProjectName;
  const wchar_t* labelOutDir;
  const wchar_t* btnBrowse;
  const wchar_t* chkEncode;
  const wchar_t* labelEncodeCodec;
  const wchar_t* labelAudio;
  const wchar_t* audioHint;
  const wchar_t* browseAudioTitle;
  const wchar_t* chkCubemap;
  const wchar_t* cubemapHint;
  const wchar_t* comboCodecLibx264;
  const wchar_t* comboCodecAuto;
  const wchar_t* comboCodecNvenc;
  const wchar_t* comboCodecQsv;
  const wchar_t* comboCodecAmf;
  const wchar_t* btnStart;
  const wchar_t* btnCaptureOnly;
  const wchar_t* btnCancel;
  const wchar_t* hint;
  const wchar_t* resNote;
  // Shown in size line when 8K is selected (disk / RAM warning).
  const wchar_t* res8KWarn;
  const wchar_t* issuesLead;
  const wchar_t* issuesSysLink;
  const wchar_t* recommendedSpec;
  const wchar_t* prWelcome;
  const wchar_t* btnAutoFill;
  // Autofill status: cores, ramGb, vramFreeGb, tierSummary
  const wchar_t* hwSuggestDetailFmt;
  const wchar_t* hwSuggestHigh;
  const wchar_t* hwSuggestMidHigh;
  const wchar_t* hwSuggestMid;
  const wchar_t* hwSuggestLight;
  const wchar_t* threadsAutoHint;
  const wchar_t* queueUnlimitedHint;
  const wchar_t* progressWindowTitle;
  const wchar_t* progressTitle;
  const wchar_t* progressStatusFmt; // done,total,pct
  const wchar_t* progressPaused;
  const wchar_t* progressFlushingPng;
  const wchar_t* progressEncoding;
  const wchar_t* progressVrMeta;
  const wchar_t* progressCleanup;
  const wchar_t* progressDone;
  const wchar_t* progressCancelled;
  const wchar_t* progressCancelling;
  const wchar_t* progressDetailFmt; // frame, elapsed, secPerFrame, eta
  // Post-frame stages: elapsed, note (may be empty)
  const wchar_t* progressPostDetailFmt;
  const wchar_t* progressFlushingDetail;
  const wchar_t* progressEncodingDetail;
  const wchar_t* progressVrMetaDetail;
  const wchar_t* progressCleanupDetail;
  const wchar_t* btnPause;
  const wchar_t* btnResume;
  const wchar_t* btnCancelRender;
  const wchar_t* comboMono;
  const wchar_t* comboStereo;
  const wchar_t* comboHigh;
  const wchar_t* comboBalanced;
  const wchar_t* comboFast;
  const wchar_t* comboFaceAuto;
  const wchar_t* comboQueueUnlimited;
  const wchar_t* browseTitle;
  const wchar_t* sizeStereoFmt; // w,h,w2,h
  const wchar_t* sizeMonoFmt;   // w,h
  const wchar_t* captureSaved;
  const wchar_t* captureFailed;
  const wchar_t* busy;
  const wchar_t* probeTitle;
  const wchar_t* probeBaselineFailed;
  const wchar_t* renderBusy;
  const wchar_t* renderDone;
  const wchar_t* renderFailed;
  const wchar_t* openOutputFolder;
  const wchar_t* btnLangJa;
  const wchar_t* btnLangEn;
  const wchar_t* saveBeforeRenderWarn;
  // %s = e.g. "NVENC → libx264"
  const wchar_t* encodeFallbackFmt;
  // Resume prompt: nextFrame, lastDone, total, projectFolder
  const wchar_t* resumeJobPromptFmt;
  const wchar_t* tabRender;
  const wchar_t* tabReencode;
  const wchar_t* reencodeHint;
  const wchar_t* labelReencodeProject;
  const wchar_t* browseProjectTitle;
  const wchar_t* btnReencode;
  const wchar_t* chkReencodeSpherical;
  const wchar_t* reencodeNoFrames;
  const wchar_t* reencodeRunning;
  const wchar_t* reencodeOkFmt;   // %s = mp4 path
  const wchar_t* reencodeFailFmt; // %s = error
};

const Bundle& t();

// Stable across languages
inline constexpr const wchar_t* kIssuesUrl =
    L"https://github.com/coffin299/MMD-VR-Renderer/issues";
inline constexpr const wchar_t* kBrowseFilter = L"Folder\0*\0\0";
inline constexpr const wchar_t* kAudioFilter =
    L"Audio (*.wav;*.mp3;*.ogg;*.flac;*.m4a)\0*.wav;*.mp3;*.ogg;*.flac;*.m4a\0All\0*.*\0\0";

} // namespace mmdvr::ui
