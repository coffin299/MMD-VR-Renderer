#include "mmdvr/Diagnostics.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace mmdvr {

Diagnostics& Diagnostics::instance() {
  static Diagnostics d;
  return d;
}

void Diagnostics::setLogPath(const std::wstring& path) {
  std::lock_guard<std::mutex> lock(mu_);
  logPath_ = path;
}

static std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
#ifdef _WIN32
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
#else
  return std::string(w.begin(), w.end());
#endif
}

void Diagnostics::write(const char* level, const std::string& msg) {
  using clock = std::chrono::system_clock;
  auto now = clock::now();
  auto t = clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << " [" << level << "] " << msg << "\n";
  const std::string line = oss.str();

  std::lock_guard<std::mutex> lock(mu_);
#ifdef _WIN32
  OutputDebugStringA(line.c_str());
#endif
  std::ofstream f(narrow(logPath_), std::ios::app | std::ios::binary);
  if (f) f << line;
}

void Diagnostics::info(const std::string& msg) { write("INFO", msg); }
void Diagnostics::warn(const std::string& msg) { write("WARN", msg); }
void Diagnostics::error(const std::string& msg) { write("ERROR", msg); }
void Diagnostics::infoW(const std::wstring& msg) { info(narrow(msg)); }

void Diagnostics::compatibilityNote(const std::string& component, const std::string& status, const std::string& detail) {
  write("COMPAT", component + " | " + status + " | " + detail);
}

} // namespace mmdvr
