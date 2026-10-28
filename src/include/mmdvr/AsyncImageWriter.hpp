#pragma once
#include "Types.hpp"
#include "ImageSequenceWriter.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace mmdvr {

// Overlap PNG encode/write with the next frame's GPU/CPU work.
// writeThreads / queueMax are end-user knobs for RAM/CPU pressure.
class AsyncImageWriter {
public:
  AsyncImageWriter(std::wstring outputDir, int writeThreads = 2, int queueMax = 0)
      : sync_(std::move(outputDir)), queueMax_(queueMax < 0 ? 0 : queueMax) {
    sync_.ensureDir();
    int n = writeThreads < 1 ? 1 : writeThreads;
    workers_.reserve((size_t)n);
    for (int i = 0; i < n; ++i)
      workers_.emplace_back([this] { loop(); });
  }

  ~AsyncImageWriter() { shutdown(); }

  AsyncImageWriter(const AsyncImageWriter&) = delete;
  AsyncImageWriter& operator=(const AsyncImageWriter&) = delete;

  void enqueue(ImageBuffer&& image, int frameIndex) {
    if (cancel_.load()) return;
    std::unique_lock<std::mutex> lock(mu_);
    if (queueMax_ > 0) {
      cvSpace_.wait(lock, [&] {
        return cancel_.load() || (int)queue_.size() < queueMax_;
      });
    }
    if (cancel_.load()) return;
    queue_.push_back({std::move(image), frameIndex});
    lock.unlock();
    cv_.notify_one();
  }

  void cancelPending() {
    cancel_.store(true);
    {
      std::lock_guard<std::mutex> lock(mu_);
      queue_.clear();
    }
    cv_.notify_all();
    cvSpace_.notify_all();
    cvDone_.notify_all();
  }

  bool flush() {
    std::unique_lock<std::mutex> lock(mu_);
    cvDone_.wait(lock, [&] {
      return cancel_.load() || (queue_.empty() && busy_ == 0);
    });
    return !failed_.load();
  }

  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (stop_) return;
      stop_ = true;
      queue_.clear();
    }
    cv_.notify_all();
    cvSpace_.notify_all();
    for (auto& w : workers_) {
      if (w.joinable()) w.join();
    }
    workers_.clear();
  }

  ImageSequenceWriter& syncWriter() { return sync_; }

private:
  struct Job {
    ImageBuffer image;
    int frame = 0;
  };

  void loop() {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
        if (stop_ && queue_.empty()) return;
        if (queue_.empty()) continue;
        job = std::move(queue_.front());
        queue_.pop_front();
        ++busy_;
      }
      cvSpace_.notify_all();
      bool ok = true;
      if (!cancel_.load())
        ok = sync_.writeFrame(job.image, job.frame);
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (!ok) failed_.store(true);
        --busy_;
      }
      cvDone_.notify_all();
    }
  }

  ImageSequenceWriter sync_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::condition_variable cvSpace_;
  std::condition_variable cvDone_;
  std::deque<Job> queue_;
  std::vector<std::thread> workers_;
  int queueMax_ = 0;
  bool stop_ = false;
  int busy_ = 0;
  std::atomic<bool> failed_{false};
  std::atomic<bool> cancel_{false};
};

} // namespace mmdvr
