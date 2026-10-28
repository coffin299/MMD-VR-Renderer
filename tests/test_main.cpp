#include "test_harness.hpp"
#include "mmdvr/Parallel.hpp"

int main() {
  int failed = 0;
  for (auto& t : testRegistry()) {
    try {
      std::printf("[ RUN  ] %s\n", t.first.c_str());
      std::fflush(stdout);
      t.second();
      std::printf("[ PASS ] %s\n", t.first.c_str());
    } catch (...) {
      std::printf("[ FAIL ] %s\n", t.first.c_str());
      ++failed;
    }
    std::fflush(stdout);
  }
  std::printf("%d failed / %zu total\n", failed, testRegistry().size());
  std::fflush(stdout);
  // Equirect/SBS tests spin up the persistent pool; join before CRT teardown.
  mmdvr::Parallel::shutdown();
  return failed ? 1 : 0;
}
