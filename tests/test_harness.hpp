#pragma once
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

using TestFn = void(*)();

inline std::vector<std::pair<std::string, TestFn>>& testRegistry() {
  static std::vector<std::pair<std::string, TestFn>> r;
  return r;
}

struct AutoReg {
  AutoReg(const char* name, TestFn fn) { testRegistry().push_back({name, fn}); }
};

#define TEST(name) \
  void test_##name(); \
  static AutoReg reg_##name(#name, test_##name); \
  void test_##name()

#define CHECK(cond) do { if (!(cond)) { \
  std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
  throw std::runtime_error("fail"); } } while(0)
