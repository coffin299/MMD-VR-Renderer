#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace mmdvr {

// Shared CPU parallelism + cooperative cancel for long output stages.
// Persistent workers avoid CreateThread/join thrash on every equirect frame
// (a common "crashes after a while with free RAM" failure mode on Windows).
class Parallel {
public:
  static void setThreadLimit(int n) { threadLimit_.store(n < 0 ? 0 : n); }
  static int threadLimit() { return threadLimit_.load(); }

  static void setCancelFlag(std::atomic<bool>* flag) { cancelFlag_.store(flag); }
  static bool cancelled() {
    auto* f = cancelFlag_.load();
    return f && f->load();
  }

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
    // Pool job state is single-flight. Serialize so a stray overlap (or future
    // caller) cannot clobber fn_/epoch_ and crash worker threads.
    std::lock_guard<std::mutex> gate(runGate());
    const int workers = resolveWorkers();
    if (rows < 32 || workers <= 1) {
      for (int y = 0; y < rows; ++y) {
        if (cancelled()) return;
        fn(y);
      }
      return;
    }
    pool().run(rows, workers, fn);
  }

  static void shutdown() { pool().shutdown(); }

private:
  struct Pool {
    std::mutex mu_;
    std::condition_variable cvStart_;
    std::condition_variable cvFinished_;
    std::vector<std::thread> threads_;

    std::function<void(int)> fn_;
    int rows_ = 0;
    int epoch_ = 0;          // bumped each job
    int startedEpoch_ = 0;   // workers wait until epoch_ advances
    std::atomic<int> nextY_{0};
    std::atomic<int> remaining_{0};
    bool stop_ = false;

    void ensureSize(int n) {
      while ((int)threads_.size() < n) {
        const int idx = (int)threads_.size();
        threads_.emplace_back([this, idx] { workerLoop(idx); });
      }
    }

    void workerLoop(int /*idx*/) {
      int seen = 0;
      for (;;) {
        {
          std::unique_lock<std::mutex> lock(mu_);
          cvStart_.wait(lock, [&] { return stop_ || epoch_ != seen; });
          if (stop_) return;
          seen = epoch_;
        }
        // Snapshot job fields (stable until remaining_ hits 0 and run() returns).
        auto fn = fn_;
        const int rows = rows_;
        for (;;) {
          const int y = nextY_.fetch_add(1, std::memory_order_relaxed);
          if (y >= rows) break;
          if ((y & 15) == 0 && cancelled()) break;
          fn(y);
        }
        if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          std::lock_guard<std::mutex> lock(mu_);
          cvFinished_.notify_one();
        }
      }
    }

    void run(int rows, int workers, const std::function<void(int)>& fn) {
      std::unique_lock<std::mutex> lock(mu_);
      ensureSize(workers);
      const int n = (int)threads_.size(); // may be > workers if pool grew earlier
      fn_ = fn;
      rows_ = rows;
      nextY_.store(0, std::memory_order_relaxed);
      remaining_.store(n, std::memory_order_relaxed);
      ++epoch_;
      cvStart_.notify_all();
      cvFinished_.wait(lock, [&] { return remaining_.load(std::memory_order_acquire) == 0; });
      fn_ = {};
    }

    void shutdown() {
      {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
        ++epoch_; // wake waiters
      }
      cvStart_.notify_all();
      for (auto& t : threads_) {
        if (t.joinable()) t.join();
      }
      threads_.clear();
      stop_ = false;
      epoch_ = 0;
    }
  };

  static Pool& pool() {
    static Pool p;
    return p;
  }

  static std::mutex& runGate() {
    static std::mutex m;
    return m;
  }

  static inline std::atomic<int> threadLimit_{0};
  static inline std::atomic<std::atomic<bool>*> cancelFlag_{nullptr};
};

} // namespace mmdvr
