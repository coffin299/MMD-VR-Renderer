#include "mmdvr/VideoEncoder.hpp"
#include "mmdvr/SphericalMp4Inject.hpp"
#include "mmdvr/Diagnostics.hpp"
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <filesystem>
#endif

namespace fs = std::filesystem;

namespace mmdvr {

static std::wstring quote(const std::wstring& s) {
  return L"\"" + s + L"\"";
}

static std::wstring moduleDirFromExe() {
#ifdef _WIN32
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring p(path);
  auto slash = p.find_last_of(L"\\/");
  if (slash != std::wstring::npos) p.resize(slash);
  return p;
#else
  return L".";
#endif
}

static bool fileExists(const std::wstring& p) {
  std::error_code ec;
  return fs::exists(p, ec) && fs::is_regular_file(p, ec);
}

static void addCandidate(std::vector<std::wstring>& out, const std::wstring& p) {
  if (p.empty()) return;
  for (auto& e : out) if (_wcsicmp(e.c_str(), p.c_str()) == 0) return;
  out.push_back(p);
}

static void collectUserFileFfmpeg(const std::wstring& mmdRoot, std::vector<std::wstring>& out) {
  std::error_code ec;
  auto user = fs::path(mmdRoot) / L"UserFile";
  if (!fs::exists(user, ec)) return;

  // Common EquirectangularX / aviwork layouts
  addCandidate(out, (user / L"aviwork" / L"ffmpeg.exe").wstring());
  addCandidate(out, (user / L"ffmpeg" / L"ffmpeg.exe").wstring());
  addCandidate(out, (user / L"tools" / L"ffmpeg.exe").wstring());

  // Shallow search under UserFile (depth-limited) for ffmpeg.exe placed by users/MMD tools
  try {
    for (auto it = fs::recursive_directory_iterator(user,
             fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); ++it) {
      if (ec) { ec.clear(); continue; }
      if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
      if (!it->is_regular_file(ec)) continue;
      if (_wcsicmp(it->path().filename().c_str(), L"ffmpeg.exe") == 0)
        addCandidate(out, it->path().wstring());
    }
  } catch (...) {}
}

std::wstring VideoEncoder::resolveFfmpeg(const std::wstring& preferred) {
  if (!preferred.empty() && fileExists(preferred)) return preferred;

  std::wstring mmd = moduleDirFromExe();
  std::vector<std::wstring> candidates;
  addCandidate(candidates, mmd + L"\\ffmpeg.exe");
  addCandidate(candidates, mmd + L"\\tools\\ffmpeg\\ffmpeg.exe");
  addCandidate(candidates, L"tools\\ffmpeg\\ffmpeg.exe");
  addCandidate(candidates, L"..\\tools\\ffmpeg\\ffmpeg.exe");
  collectUserFileFfmpeg(mmd, candidates);
  addCandidate(candidates, L"ffmpeg.exe");

  for (auto& c : candidates) {
    if (fileExists(c)) {
      std::error_code ec;
      return fs::absolute(c, ec).wstring();
    }
  }
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, buf, nullptr))
    return buf;
#endif
  return L"ffmpeg";
}

static std::string runCaptureOutput(const std::wstring& cmd) {
#ifdef _WIN32
  auto outPath = fs::temp_directory_path() / L"mmdvr_ffmpeg_probe.txt";
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE hOut = CreateFileW(outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  STARTUPINFOW si{}; si.cb = sizeof(si);
  if (hOut && hOut != INVALID_HANDLE_VALUE) {
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hOut;
    si.hStdError = hOut;
  }
  PROCESS_INFORMATION pi{};
  std::wstring mutableCmd = cmd;
  BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  if (hOut && hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
  if (!ok) return {};
  WaitForSingleObject(pi.hProcess, 15000);
  CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

  std::ifstream f(outPath, std::ios::binary);
  if (!f) return {};
  return std::string(std::istreambuf_iterator<char>(f), {});
#else
  (void)cmd;
  return {};
#endif
}

FfmpegCaps VideoEncoder::probe(const std::wstring& preferred) {
  static std::mutex mu;
  static FfmpegCaps cached{};
  static bool haveCache = false;
  static std::wstring cachedPreferred;

  std::lock_guard<std::mutex> lock(mu);
  if (haveCache && cachedPreferred == preferred) return cached;

  FfmpegCaps caps;
  caps.path = resolveFfmpeg(preferred);
  std::wstring cmd = quote(caps.path) + L" -hide_banner -encoders";
  auto text = runCaptureOutput(cmd);
  if (!text.empty()) {
    caps.ok = true;
    caps.hasNvenc = text.find("h264_nvenc") != std::string::npos;
    caps.hasQsv = text.find("h264_qsv") != std::string::npos;
    caps.hasAmf = text.find("h264_amf") != std::string::npos;
    Diagnostics::instance().infoW(
        L"FFmpeg probe: " + caps.path +
        L" nvenc=" + (caps.hasNvenc ? L"1" : L"0") +
        L" qsv=" + (caps.hasQsv ? L"1" : L"0") +
        L" amf=" + (caps.hasAmf ? L"1" : L"0"));
  } else {
    Diagnostics::instance().warn("FFmpeg probe failed (encoders list empty)");
  }

  cached = caps;
  cachedPreferred = preferred;
  haveCache = true;
  return caps;
}

EncodeCodec VideoEncoder::resolveCodec(EncodeCodec requested, const FfmpegCaps& caps) {
  auto pickHw = [&]() -> EncodeCodec {
    if (caps.hasNvenc) return EncodeCodec::Nvenc;
    if (caps.hasQsv) return EncodeCodec::Qsv;
    if (caps.hasAmf) return EncodeCodec::Amf;
    return EncodeCodec::Libx264;
  };

  switch (requested) {
  case EncodeCodec::Auto:
    return pickHw();
  case EncodeCodec::Nvenc:
    return caps.hasNvenc ? EncodeCodec::Nvenc : EncodeCodec::Libx264;
  case EncodeCodec::Qsv:
    return caps.hasQsv ? EncodeCodec::Qsv : EncodeCodec::Libx264;
  case EncodeCodec::Amf:
    return caps.hasAmf ? EncodeCodec::Amf : EncodeCodec::Libx264;
  case EncodeCodec::Libx264:
  default:
    return EncodeCodec::Libx264;
  }
}

std::wstring VideoEncoder::codecArgs(EncodeCodec codec) {
  // Keep libx264 identical to the previous hard-coded default.
  switch (codec) {
  case EncodeCodec::Nvenc:
    // Same family as EquirectangularX call_ffmpeg_*.bat (-vcodec h264_nvenc)
    return L" -c:v h264_nvenc -pix_fmt yuv420p -preset p5 -cq 19 -b:v 0 ";
  case EncodeCodec::Qsv:
    return L" -c:v h264_qsv -pix_fmt yuv420p -global_quality 22 ";
  case EncodeCodec::Amf:
    return L" -c:v h264_amf -pix_fmt yuv420p -rc cqp -qp_i 18 -qp_p 18 ";
  case EncodeCodec::Auto:
  case EncodeCodec::Libx264:
  default:
    return L" -c:v libx264 -pix_fmt yuv420p -crf 18 ";
  }
}

static int findFirstSequenceIndex(const std::wstring& sequenceDir) {
  std::error_code ec;
  int best = -1;
  for (auto& ent : fs::directory_iterator(sequenceDir, ec)) {
    if (!ent.is_regular_file()) continue;
    auto name = ent.path().filename().string();
    if (name.size() < 16) continue;
    if (name.rfind("frame_", 0) != 0) continue;
    if (ent.path().extension() != ".png") continue;
    try {
      int n = std::stoi(name.substr(6, 6));
      if (best < 0 || n < best) best = n;
    } catch (...) {}
  }
  return best < 0 ? 0 : best;
}

static bool runFfmpeg(const std::wstring& cmd, const std::wstring& workDir, std::wstring* errorOut) {
#ifdef _WIN32
  auto errPath = fs::temp_directory_path() / L"mmdvr_ffmpeg_err.txt";
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE hErr = CreateFileW(errPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

  STARTUPINFOW si{}; si.cb = sizeof(si);
  if (hErr && hErr != INVALID_HANDLE_VALUE) {
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hErr;
    si.hStdError = hErr;
  }
  PROCESS_INFORMATION pi{};
  std::wstring mutableCmd = cmd;
  BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW, nullptr,
                           workDir.empty() ? nullptr : workDir.c_str(),
                           &si, &pi);
  if (hErr && hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
  if (!ok) {
    if (errorOut) *errorOut = L"CreateProcess failed for ffmpeg (is ffmpeg installed?)";
    return false;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

  std::string errTail;
  {
    std::ifstream f(errPath, std::ios::binary);
    if (f) {
      f.seekg(0, std::ios::end);
      auto sz = (size_t)f.tellg();
      if (sz > 2000) f.seekg((std::streamoff)(sz - 2000));
      else f.seekg(0);
      errTail.assign(std::istreambuf_iterator<char>(f), {});
    }
  }
  if (code != 0) {
    Diagnostics::instance().warn(std::string("ffmpeg stderr tail: ") + errTail);
    if (errorOut) {
      *errorOut = L"ffmpeg exit " + std::to_wstring((long)code);
      if (!errTail.empty()) {
        std::wstring w;
        int n = MultiByteToWideChar(CP_UTF8, 0, errTail.c_str(), (int)errTail.size(), nullptr, 0);
        w.resize(n);
        MultiByteToWideChar(CP_UTF8, 0, errTail.c_str(), (int)errTail.size(), w.data(), n);
        if (w.size() > 400) w = w.substr(w.size() - 400);
        *errorOut += L"\n" + w;
      }
    }
    return false;
  }
  return true;
#else
  (void)cmd; (void)workDir; (void)errorOut;
  return false;
#endif
}

bool VideoEncoder::encodeSequence(
    const std::wstring& sequenceDir,
    const std::wstring& pattern,
    const std::wstring& outMp4,
    double fps,
    const std::wstring& ffmpegPath,
    EncodeCodec codec,
    const std::wstring& audioPath,
    double audioStartSec,
    std::wstring* errorOut) {
  auto caps = probe(ffmpegPath);
  auto resolved = resolveCodec(codec, caps);
  auto ff = caps.path.empty() ? resolveFfmpeg(ffmpegPath) : caps.path;
  int startNum = findFirstSequenceIndex(sequenceDir);
  std::wstring input = sequenceDir + L"\\" + pattern;
  const bool hasAudio = !audioPath.empty() && fileExists(audioPath);
  if (!audioPath.empty() && !hasAudio) {
    Diagnostics::instance().warn("audioPath set but file missing; encoding video-only");
    if (errorOut) *errorOut += L"audio missing: " + audioPath + L"\n";
  }

  std::wstringstream cmd;
  cmd << quote(ff)
      << L" -y -framerate " << fps
      << L" -start_number " << startNum
      << L" -i " << quote(input);
  if (hasAudio) {
    if (audioStartSec > 0.0)
      cmd << L" -ss " << audioStartSec;
    cmd << L" -i " << quote(audioPath);
  }
  cmd << codecArgs(resolved);
  if (hasAudio) {
    // Map video from input0, audio from input1; cut to the shorter stream (usually video).
    cmd << L" -map 0:v:0 -map 1:a:0? -c:a aac -b:a 192k -shortest ";
  }
  cmd << quote(outMp4);
  Diagnostics::instance().infoW(L"FFmpeg: " + cmd.str());
  Diagnostics::instance().info("FFmpeg start_number=" + std::to_string(startNum) +
                               " codec=" + std::to_string((int)resolved) +
                               (hasAudio ? " +audio" : " video-only"));
  return runFfmpeg(cmd.str(), sequenceDir, errorOut);
}

bool VideoEncoder::injectVrMetadata(
    const std::wstring& inMp4,
    const std::wstring& outMp4,
    VrMode vrMode,
    StereoMode stereoMode,
    int frameW,
    int frameH,
    const std::wstring& /*ffmpegPath*/,
    std::wstring* errorOut) {
  // YouTube needs the Google Spherical UUID box — plain ffmpeg -metadata tags are ignored.
  if (injectSphericalMp4(inMp4, outMp4, vrMode, stereoMode, frameW, frameH, errorOut))
    return true;
  Diagnostics::instance().warn("Spherical UUID inject failed");
  return false;
}

} // namespace mmdvr
