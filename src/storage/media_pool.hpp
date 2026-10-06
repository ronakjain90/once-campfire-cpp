// A bounded pool for media work (libvips, ffmpeg, analysis): at most 4 threads, off the writer
// (architecture section 12; Rust: process_media in crates/campfire/src/active_storage.rs).
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace campfire::storage {

inline constexpr size_t kMaxMediaThreads = 4;

class MediaPool {
 public:
  // `threads` is clamped to 1..kMaxMediaThreads. The default is the CPU count, at least 2.
  explicit MediaPool(size_t threads = 0);
  ~MediaPool();
  MediaPool(const MediaPool&) = delete;
  MediaPool& operator=(const MediaPool&) = delete;

  size_t thread_count() const { return workers_.size(); }

  // Runs `job` on a pool thread. The job must not throw.
  void post(std::function<void()> job);

  // Runs `work` on a pool thread. The future gets its result.
  template <class F>
  auto submit(F work) -> std::future<decltype(work())> {
    using R = decltype(work());
    auto task = std::make_shared<std::packaged_task<R()>>(std::move(work));
    auto future = task->get_future();
    post([task] { (*task)(); });
    return future;
  }

 private:
  void run();

  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::function<void()>> queue_;
  bool stopping_ = false;
  std::vector<std::thread> workers_;
};

}  // namespace campfire::storage
