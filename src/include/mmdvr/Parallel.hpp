#pragma once
#include <algorithm>
#include <atomic>
#include <functional>
#include <thread>
#include <vector>

namespace mmdvr {

// Shared CPU parallelism + cooperative cancel for long output stages.
class Parallel {
public:
  // 0 = use all hardware threads (no artificial cap).
  static void setThreadLimit(int n) { threadLimit_.store(n < 0 ? 0 : n); }
  static int threadLimit() { return threadLimit_.load(); }

  static void setCancelFlag(std::atomic<bool>* flag) { cancelFlag_.store(flag); }
  static bool cancelled() {
    auto* f = cancelFlag_.load();
    return f && f->load();
  }

  // User-specified limit is honored as-is. Auto uses all logical cores.
  static int resolveWorkers(int /*unusedCap*/ = 0) {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    int lim = threadLimit_.load();
    int n = lim > 0 ? lim : (int)hw;
    if (n < 1) n = 1;
    return n;
  }

  static void forRows(int rows, const std::function<void(int)>& fn) {
    if (rows <= 0) return;
    const int workers = resolveWorkers();
    if (rows < 32 || workers <= 1) {
      for (int y = 0; y < rows; ++y) {
        if (cancelled()) return;
        fn(y);
      }
      return;
    }
    std::vector<std::thread> threads;
    threads.reserve((size_t)workers);
    const int chunk = (rows + workers - 1) / workers;
    for (int t = 0; t < workers; ++t) {
      const int y0 = t * chunk;
      const int y1 = std::min(rows, y0 + chunk);
      if (y0 >= y1) break;
      threads.emplace_back([&, y0, y1]() {
        for (int y = y0; y < y1; ++y) {
          if (cancelled()) return;
          fn(y);
        }
      });
    }
    for (auto& th : threads) th.join();
  }

private:
  static inline std::atomic<int> threadLimit_{0};
  static inline std::atomic<std::atomic<bool>*> cancelFlag_{nullptr};
};

} // namespace mmdvr
