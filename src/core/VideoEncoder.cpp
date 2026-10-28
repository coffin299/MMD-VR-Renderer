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

static bool runFfmpeg(const std::wstring& cmd, const std::wstring& workDir, std::wstring* errorOut);

// Concrete FFmpeg encoder picked at encode time (UI still uses EncodeCodec).
enum class EncImpl : int {
  HevcNvenc = 0,
  HevcQsv,
  HevcAmf,
  Libx265,
  H264Nvenc,
  H264Qsv,
  H264Amf,
  Libx264
};

static const wchar_t* implLabel(EncImpl i) {
  switch (i) {
  case EncImpl::HevcNvenc: return L"HEVC NVENC";
  case EncImpl::HevcQsv: return L"HEVC QSV";
  case EncImpl::HevcAmf: return L"HEVC AMF";
  case EncImpl::Libx265: return L"libx265";
  case EncImpl::H264Nvenc: return L"H264 NVENC";
  case EncImpl::H264Qsv: return L"H264 QSV";
  case EncImpl::H264Amf: return L"H264 AMF";
  case EncImpl::Libx264:
  default: return L"libx264";
  }
}

static std::wstring implArgs(EncImpl i) {
  switch (i) {
  case EncImpl::HevcNvenc:
    return L" -c:v hevc_nvenc -pix_fmt yuv420p -preset p5 -cq 19 -b:v 0 -tag:v hvc1 ";
  case EncImpl::HevcQsv:
    return L" -c:v hevc_qsv -pix_fmt yuv420p -global_quality 22 -tag:v hvc1 ";
  case EncImpl::HevcAmf:
    return L" -c:v hevc_amf -pix_fmt yuv420p -rc cqp -qp_i 18 -qp_p 18 -tag:v hvc1 ";
  case EncImpl::Libx265:
    return L" -c:v libx265 -pix_fmt yuv420p -crf 20 -preset medium -tag:v hvc1 ";
  case EncImpl::H264Nvenc:
    return L" -c:v h264_nvenc -pix_fmt yuv420p -preset p5 -cq 19 -b:v 0 ";
  case EncImpl::H264Qsv:
    return L" -c:v h264_qsv -pix_fmt yuv420p -global_quality 22 ";
  case EncImpl::H264Amf:
    return L" -c:v h264_amf -pix_fmt yuv420p -rc cqp -qp_i 18 -qp_p 18 ";
  case EncImpl::Libx264:
  default:
    return L" -c:v libx264 -pix_fmt yuv420p -crf 18 ";
  }
}

// Conservative HW max edge. Soft encoders (libx265/libx264) are unlimited here.
static int implMaxEdge(EncImpl i) {
  switch (i) {
  case EncImpl::HevcNvenc:
    return 8192; // common NVENC HEVC cap; 8K equirect falls to libx265
  case EncImpl::HevcQsv:
  case EncImpl::HevcAmf:
    return 8192;
  case EncImpl::H264Nvenc:
    return 8192;
  case EncImpl::H264Qsv:
  case EncImpl::H264Amf:
    return 4096;
  default:
    return 0x7fffffff;
  }
}

static bool implSupportsSize(EncImpl i, int frameW, int frameH) {
  if (frameW <= 0 || frameH <= 0) return true;
  if (i == EncImpl::Libx265 || i == EncImpl::Libx264) return true;
  if ((frameW & 1) || (frameH & 1)) return false;
  const int maxEdge = implMaxEdge(i);
  return frameW <= maxEdge && frameH <= maxEdge;
}

static bool implAvailable(EncImpl i, const FfmpegCaps& caps) {
  switch (i) {
  case EncImpl::HevcNvenc: return caps.hasHevcNvenc;
  case EncImpl::HevcQsv: return caps.hasHevcQsv;
  case EncImpl::HevcAmf: return caps.hasHevcAmf;
  case EncImpl::Libx265: return caps.hasLibx265;
  case EncImpl::H264Nvenc: return caps.hasH264Nvenc;
  case EncImpl::H264Qsv: return caps.hasH264Qsv;
  case EncImpl::H264Amf: return caps.hasH264Amf;
  case EncImpl::Libx264: return true;
  }
  return false;
}

static bool implListed(EncImpl i, const FfmpegCaps& caps) {
  switch (i) {
  case EncImpl::HevcNvenc: return caps.listedHevcNvenc;
  case EncImpl::HevcQsv: return caps.listedHevcQsv;
  case EncImpl::HevcAmf: return caps.listedHevcAmf;
  case EncImpl::Libx265: return caps.listedLibx265;
  case EncImpl::H264Nvenc: return caps.listedH264Nvenc;
  case EncImpl::H264Qsv: return caps.listedH264Qsv;
  case EncImpl::H264Amf: return caps.listedH264Amf;
  case EncImpl::Libx264: return true;
  }
  return false;
}

static bool smokeTestImpl(const std::wstring& ff, EncImpl impl) {
  if (impl == EncImpl::Libx264) return true;
  if (impl == EncImpl::Libx265) {
    // Presence in -encoders is enough for soft; skip slow smoke.
    return true;
  }
  auto outPath = fs::temp_directory_path() / L"mmdvr_hw_smoke.mp4";
  std::error_code ec;
  fs::remove(outPath, ec);
  std::wstringstream cmd;
  cmd << quote(ff)
      << L" -hide_banner -loglevel error -y"
      << L" -f lavfi -i color=c=black:s=128x128:d=0.2:r=30 -frames:v 1"
      << implArgs(impl)
      << quote(outPath.wstring());
  std::wstring err;
  const bool ok = runFfmpeg(cmd.str(), outPath.parent_path().wstring(), &err);
  fs::remove(outPath, ec);
  if (!ok) {
    Diagnostics::instance().warn(
        std::string("HW encoder smoke-test failed impl=") + std::to_string((int)impl));
  }
  return ok;
}

static void appendUnique(std::vector<EncImpl>& out, EncImpl i) {
  for (auto e : out)
    if (e == i) return;
  out.push_back(i);
}

// Ordered candidates: H.265 first, then H.264, soft last.
static std::vector<EncImpl> buildCandidates(EncodeCodec requested, const FfmpegCaps& caps,
                                            int frameW, int frameH, bool ignoreSize) {
  std::vector<EncImpl> raw;
  auto addFamily = [&](EncImpl hevc, EncImpl h264) {
    appendUnique(raw, hevc);
    appendUnique(raw, h264);
  };

  switch (requested) {
  case EncodeCodec::Nvenc:
    addFamily(EncImpl::HevcNvenc, EncImpl::H264Nvenc);
    appendUnique(raw, EncImpl::Libx265);
    appendUnique(raw, EncImpl::Libx264);
    break;
  case EncodeCodec::Qsv:
    addFamily(EncImpl::HevcQsv, EncImpl::H264Qsv);
    appendUnique(raw, EncImpl::Libx265);
    appendUnique(raw, EncImpl::Libx264);
    break;
  case EncodeCodec::Amf:
    addFamily(EncImpl::HevcAmf, EncImpl::H264Amf);
    appendUnique(raw, EncImpl::Libx265);
    appendUnique(raw, EncImpl::Libx264);
    break;
  case EncodeCodec::Libx264:
    appendUnique(raw, EncImpl::Libx265);
    appendUnique(raw, EncImpl::Libx264);
    break;
  case EncodeCodec::Auto:
  default:
    // HEVC HW → libx265 → H.264 HW → libx264
    appendUnique(raw, EncImpl::HevcNvenc);
    appendUnique(raw, EncImpl::HevcQsv);
    appendUnique(raw, EncImpl::HevcAmf);
    appendUnique(raw, EncImpl::Libx265);
    appendUnique(raw, EncImpl::H264Nvenc);
    appendUnique(raw, EncImpl::H264Qsv);
    appendUnique(raw, EncImpl::H264Amf);
    appendUnique(raw, EncImpl::Libx264);
    break;
  }

  std::vector<EncImpl> out;
  for (auto i : raw) {
    if (!implAvailable(i, caps) && !(i == EncImpl::Libx264)) continue;
    if (!ignoreSize && !implSupportsSize(i, frameW, frameH)) continue;
    appendUnique(out, i);
  }
  if (out.empty()) appendUnique(out, EncImpl::Libx264);
  return out;
}

static EncImpl preferredImpl(EncodeCodec requested, const FfmpegCaps& caps) {
  // For UI fallback text: first preference ignoring size (listed counts too).
  auto tryPrefer = [&](EncImpl i) -> bool {
    return implAvailable(i, caps) || implListed(i, caps) || i == EncImpl::Libx264;
  };
  auto ordered = buildCandidates(requested, caps, 0, 0, true);
  // buildCandidates already filters to available; also consider listed-only names.
  std::vector<EncImpl> wish;
  switch (requested) {
  case EncodeCodec::Nvenc:
    wish = {EncImpl::HevcNvenc, EncImpl::H264Nvenc, EncImpl::Libx265, EncImpl::Libx264};
    break;
  case EncodeCodec::Qsv:
    wish = {EncImpl::HevcQsv, EncImpl::H264Qsv, EncImpl::Libx265, EncImpl::Libx264};
    break;
  case EncodeCodec::Amf:
    wish = {EncImpl::HevcAmf, EncImpl::H264Amf, EncImpl::Libx265, EncImpl::Libx264};
    break;
  case EncodeCodec::Libx264:
    wish = {EncImpl::Libx265, EncImpl::Libx264};
    break;
  case EncodeCodec::Auto:
  default:
    wish = {EncImpl::HevcNvenc, EncImpl::HevcQsv, EncImpl::HevcAmf, EncImpl::Libx265,
            EncImpl::H264Nvenc, EncImpl::Libx264};
    break;
  }
  for (auto i : wish)
    if (tryPrefer(i)) return i;
  if (!ordered.empty()) return ordered.front();
  return EncImpl::Libx264;
}

bool VideoEncoder::codecSupportsSize(EncodeCodec codec, int frameW, int frameH) {
  if (frameW <= 0 || frameH <= 0) return true;
  if (codec == EncodeCodec::Libx264 || codec == EncodeCodec::Auto) return true;
  EncImpl hevc = EncImpl::HevcNvenc;
  EncImpl h264 = EncImpl::H264Nvenc;
  if (codec == EncodeCodec::Qsv) {
    hevc = EncImpl::HevcQsv;
    h264 = EncImpl::H264Qsv;
  } else if (codec == EncodeCodec::Amf) {
    hevc = EncImpl::HevcAmf;
    h264 = EncImpl::H264Amf;
  }
  return implSupportsSize(hevc, frameW, frameH) || implSupportsSize(h264, frameW, frameH) ||
         implSupportsSize(EncImpl::Libx265, frameW, frameH);
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
    caps.listedHevcNvenc = text.find("hevc_nvenc") != std::string::npos;
    caps.listedHevcQsv = text.find("hevc_qsv") != std::string::npos;
    caps.listedHevcAmf = text.find("hevc_amf") != std::string::npos;
    caps.listedLibx265 = text.find("libx265") != std::string::npos;
    caps.listedH264Nvenc = text.find("h264_nvenc") != std::string::npos;
    caps.listedH264Qsv = text.find("h264_qsv") != std::string::npos;
    caps.listedH264Amf = text.find("h264_amf") != std::string::npos;

    caps.hasLibx265 = caps.listedLibx265;
    caps.hasHevcNvenc =
        caps.listedHevcNvenc && smokeTestImpl(caps.path, EncImpl::HevcNvenc);
    caps.hasHevcQsv = caps.listedHevcQsv && smokeTestImpl(caps.path, EncImpl::HevcQsv);
    caps.hasHevcAmf = caps.listedHevcAmf && smokeTestImpl(caps.path, EncImpl::HevcAmf);
    caps.hasH264Nvenc =
        caps.listedH264Nvenc && smokeTestImpl(caps.path, EncImpl::H264Nvenc);
    caps.hasH264Qsv = caps.listedH264Qsv && smokeTestImpl(caps.path, EncImpl::H264Qsv);
    caps.hasH264Amf = caps.listedH264Amf && smokeTestImpl(caps.path, EncImpl::H264Amf);

    Diagnostics::instance().infoW(
        L"FFmpeg probe: " + caps.path +
        L" hevc_nvenc=" + (caps.hasHevcNvenc ? L"1" : (caps.listedHevcNvenc ? L"listed-fail" : L"0")) +
        L" hevc_qsv=" + (caps.hasHevcQsv ? L"1" : (caps.listedHevcQsv ? L"listed-fail" : L"0")) +
        L" hevc_amf=" + (caps.hasHevcAmf ? L"1" : (caps.listedHevcAmf ? L"listed-fail" : L"0")) +
        L" libx265=" + (caps.hasLibx265 ? L"1" : L"0") +
        L" h264_nvenc=" + (caps.hasH264Nvenc ? L"1" : (caps.listedH264Nvenc ? L"listed-fail" : L"0")));
  } else {
    Diagnostics::instance().warn("FFmpeg probe failed (encoders list empty)");
  }

  cached = caps;
  cachedPreferred = preferred;
  haveCache = true;
  return caps;
}

EncodeCodec VideoEncoder::resolveCodec(EncodeCodec requested, const FfmpegCaps& caps,
                                       int frameW, int frameH) {
  auto cands = buildCandidates(requested, caps, frameW, frameH, false);
  if (cands.empty()) return EncodeCodec::Libx264;
  switch (cands.front()) {
  case EncImpl::HevcNvenc:
  case EncImpl::H264Nvenc: return EncodeCodec::Nvenc;
  case EncImpl::HevcQsv:
  case EncImpl::H264Qsv: return EncodeCodec::Qsv;
  case EncImpl::HevcAmf:
  case EncImpl::H264Amf: return EncodeCodec::Amf;
  case EncImpl::Libx265:
  case EncImpl::Libx264:
  default: return EncodeCodec::Libx264;
  }
}

const wchar_t* VideoEncoder::codecLabel(EncodeCodec codec) {
  switch (codec) {
  case EncodeCodec::Nvenc: return L"HEVC NVENC";
  case EncodeCodec::Qsv: return L"HEVC QSV";
  case EncodeCodec::Amf: return L"HEVC AMF";
  case EncodeCodec::Auto: return L"Auto";
  case EncodeCodec::Libx264:
  default: return L"libx265";
  }
}

std::wstring VideoEncoder::codecArgs(EncodeCodec codec) {
  // Representative args for smoke/legacy callers; encodeSequence uses EncImpl.
  switch (codec) {
  case EncodeCodec::Nvenc: return implArgs(EncImpl::HevcNvenc);
  case EncodeCodec::Qsv: return implArgs(EncImpl::HevcQsv);
  case EncodeCodec::Amf: return implArgs(EncImpl::HevcAmf);
  case EncodeCodec::Auto:
  case EncodeCodec::Libx264:
  default: return implArgs(EncImpl::Libx265);
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

static bool readPngSize(const std::wstring& path, int& outW, int& outH) {
  outW = outH = 0;
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  unsigned char hdr[24]{};
  f.read(reinterpret_cast<char*>(hdr), 24);
  if (!f || f.gcount() < 24) return false;
  static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  for (int i = 0; i < 8; ++i)
    if (hdr[i] != sig[i]) return false;
  // IHDR width/height are big-endian at bytes 16..23
  outW = (hdr[16] << 24) | (hdr[17] << 16) | (hdr[18] << 8) | hdr[19];
  outH = (hdr[20] << 24) | (hdr[21] << 16) | (hdr[22] << 8) | hdr[23];
  return outW > 0 && outH > 0;
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
    std::wstring* errorOut,
    std::wstring* fallbackOut) {
  if (fallbackOut) fallbackOut->clear();
  auto caps = probe(ffmpegPath);
  auto ff = caps.path.empty() ? resolveFfmpeg(ffmpegPath) : caps.path;
  int startNum = findFirstSequenceIndex(sequenceDir);

  int frameW = 0, frameH = 0;
  {
    wchar_t firstName[64];
    std::swprintf(firstName, 64, L"frame_%06d.png", startNum);
    readPngSize(sequenceDir + L"\\" + firstName, frameW, frameH);
    if (frameW > 0) {
      Diagnostics::instance().info(
          "FFmpeg input frame size " + std::to_string(frameW) + "x" + std::to_string(frameH));
    }
  }

  const EncImpl preferred = preferredImpl(codec, caps);
  auto candidates = buildCandidates(codec, caps, frameW, frameH, false);
  auto setFallback = [&](EncImpl from, EncImpl to) {
    if (!fallbackOut || from == to) return;
    *fallbackOut = std::wstring(implLabel(from)) + L" → " + implLabel(to);
    Diagnostics::instance().infoW(L"FFmpeg encoder fallback: " + *fallbackOut);
  };
  if (!candidates.empty() && preferred != candidates.front())
    setFallback(preferred, candidates.front());

  std::wstring input = sequenceDir + L"\\" + pattern;
  const bool hasAudio = !audioPath.empty() && fileExists(audioPath);
  if (!audioPath.empty() && !hasAudio) {
    Diagnostics::instance().warn("audioPath set but file missing; encoding video-only");
    if (errorOut) *errorOut += L"audio missing: " + audioPath + L"\n";
  }

  auto buildCmd = [&](EncImpl useImpl) {
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
    cmd << implArgs(useImpl);
    if (hasAudio) {
      // Map video from input0, audio from input1; cut to the shorter stream (usually video).
      cmd << L" -map 0:v:0 -map 1:a:0? -c:a aac -b:a 192k -shortest ";
    }
    cmd << quote(outMp4);
    return cmd.str();
  };

  auto tryEncode = [&](EncImpl useImpl) {
    const auto cmd = buildCmd(useImpl);
    Diagnostics::instance().infoW(L"FFmpeg: " + cmd);
    Diagnostics::instance().info(
        std::string("FFmpeg start_number=") + std::to_string(startNum) +
        " impl=" + std::to_string((int)useImpl) + (hasAudio ? " +audio" : " video-only"));
    return runFfmpeg(cmd, sequenceDir, errorOut);
  };

  EncImpl tried = candidates.empty() ? EncImpl::Libx264 : candidates.front();
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (i > 0) {
      Diagnostics::instance().warn(
          std::string("encode failed; trying next encoder impl=") +
          std::to_string((int)candidates[i]));
      std::error_code ec;
      fs::remove(outMp4, ec);
      setFallback(tried, candidates[i]);
      if (errorOut) *errorOut += L"\n(retrying next encoder)\n";
    }
    tried = candidates[i];
    if (tryEncode(tried)) return true;
  }
  return false;
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
