// Simple blocking thread pool. Architecture section 3.
#include "net/thread_pool.hpp"

namespace campfire::net {

ThreadPool::ThreadPool(std::size_t threads) {
  if (threads == 0) threads = 1;
  for (std::size_t i = 0; i < threads; ++i) threads_.emplace_back([this] { run(); });
}

ThreadPool::~ThreadPool() {
  {
    const std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  ready_.notify_all();
  for (std::thread& t : threads_) t.join();
}

void ThreadPool::submit(std::move_only_function<void()> job) {
  {
    const std::lock_guard lock(mutex_);
    jobs_.push_back(std::move(job));
  }
  ready_.notify_one();
}

void ThreadPool::run() {
  while (true) {
    std::move_only_function<void()> job;
    {
      std::unique_lock lock(mutex_);
      ready_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
      if (jobs_.empty()) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    job();
  }
}

}  // namespace campfire::net
