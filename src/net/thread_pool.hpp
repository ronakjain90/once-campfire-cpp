// Simple blocking thread pool for `ctx.offload`. Architecture section 3 ("Job pools").
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace campfire::net {

class ThreadPool {
 public:
  explicit ThreadPool(std::size_t threads);
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  // Finishes the queued jobs, then joins the threads.
  ~ThreadPool();

  void submit(std::move_only_function<void()> job);

 private:
  void run();

  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::move_only_function<void()>> jobs_;
  bool stopping_ = false;
  std::vector<std::thread> threads_;
};

}  // namespace campfire::net
