#pragma once
#include <string>
#include <mutex>

namespace mmdvr {

class Diagnostics {
public:
  static Diagnostics& instance();

  void setLogPath(const std::wstring& path);
  void info(const std::string& msg);
  void warn(const std::string& msg);
  void error(const std::string& msg);
  void infoW(const std::wstring& msg);
  void compatibilityNote(const std::string& component, const std::string& status, const std::string& detail);

private:
  Diagnostics() = default;
  void write(const char* level, const std::string& msg);
  std::mutex mu_;
  std::wstring logPath_ = L"mmd_vr_renderer.log";
};

} // namespace mmdvr
