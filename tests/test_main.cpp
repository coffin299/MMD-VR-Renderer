#include "test_harness.hpp"

int main() {
  int failed = 0;
  for (auto& t : testRegistry()) {
    try {
      std::printf("[ RUN  ] %s\n", t.first.c_str());
      t.second();
      std::printf("[ PASS ] %s\n", t.first.c_str());
    } catch (...) {
      std::printf("[ FAIL ] %s\n", t.first.c_str());
      ++failed;
    }
  }
  std::printf("%d failed / %zu total\n", failed, testRegistry().size());
  return failed ? 1 : 0;
}
