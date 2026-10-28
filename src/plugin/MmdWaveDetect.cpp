#include "MmdWaveDetect.hpp"
#include "mmdvr/Diagnostics.hpp"
#include <algorithm>
#include <vector>

namespace mmdvr {
namespace {

bool looksLikeAudioPath(const std::wstring& t) {
  if (t.size() < 5 || t.size() > 500) return false;
  // Prefer absolute paths with a drive or UNC.
  const bool abs = (t.size() >= 3 && t[1] == L':' && (t[2] == L'\\' || t[2] == L'/')) ||
                   (t.size() >= 2 && t[0] == L'\\' && t[1] == L'\\');
  if (!abs && t.find(L'\\') == std::wstring::npos && t.find(L'/') == std::wstring::npos)
    return false;
  auto lower = t;
  for (auto& c : lower) c = (wchar_t)towlower(c);
  return lower.find(L".wav") != std::wstring::npos ||
         lower.find(L".mp3") != std::wstring::npos ||
         lower.find(L".ogg") != std::wstring::npos ||
         lower.find(L".flac") != std::wstring::npos ||
         lower.find(L".m4a") != std::wstring::npos;
}

bool fileExists(const std::wstring& p) {
  DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

struct Ctx {
  std::wstring best;
};

BOOL CALLBACK enumCb(HWND hwnd, LPARAM lp) {
  auto* ctx = reinterpret_cast<Ctx*>(lp);
  wchar_t text[1024]{};
  GetWindowTextW(hwnd, text, 1024);
  std::wstring t(text);
  if (looksLikeAudioPath(t) && fileExists(t)) {
    ctx->best = t;
    return FALSE; // stop
  }
  EnumChildWindows(hwnd, enumCb, lp);
  return TRUE;
}

} // namespace

std::wstring detectMmdWavePath(HWND mmdMain) {
  if (!mmdMain || !IsWindow(mmdMain)) return {};
  Ctx ctx;
  EnumChildWindows(mmdMain, enumCb, reinterpret_cast<LPARAM>(&ctx));
  if (!ctx.best.empty()) {
    Diagnostics::instance().infoW(L"Detected MMD audio: " + ctx.best);
  }
  return ctx.best;
}

} // namespace mmdvr
