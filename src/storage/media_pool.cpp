// See media_pool.hpp.
#include "storage/media_pool.hpp"

#include <vips/vips.h>

#include <algorithm>

#include "storage/vips.hpp"

namespace campfire::storage {

MediaPool::MediaPool(size_t threads) {
  if (threads == 0) threads = std::max<size_t>(2, std::thread::hardware_concurrency());
  threads = std::clamp<size_t>(threads, 1, kMaxMediaThreads);
  for (size_t i = 0; i < threads; ++i) workers_.emplace_back([this] { run(); });
}

MediaPool::~MediaPool() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  for (auto& w : workers_) w.join();
}

void MediaPool::post(std::function<void()> job) {
  {
    std::lock_guard lock(mutex_);
    queue_.push_back(std::move(job));
  }
  wake_.notify_one();
}

void MediaPool::run() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) break;  // stopping, and the queue is drained
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    job();
  }
  // libvips keeps per-thread state: release it before the thread ends.
  if (vips::init()) vips_thread_shutdown();
}

}  // namespace campfire::storage
