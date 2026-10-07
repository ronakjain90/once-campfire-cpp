// Rails: reference/app/jobs/**. Rust: crates/campfire/src/jobs.rs.
#include "jobs/queue.hpp"

#include <exception>

#include "core/log.hpp"

namespace campfire::jobs {

std::string_view job_name(JobKind kind) noexcept {
  switch (kind) {
    case JobKind::PushMessage: return "Room::PushMessageJob";
    case JobKind::DeliverWebhook: return "Bot::WebhookJob";
    case JobKind::RemoveBannedContent: return "RemoveBannedContentJob";
    case JobKind::PurgeBlob: return "ActiveStorage::PurgeJob";
  }
  return "Job";
}

JobQueues::JobQueues(std::size_t concurrency, std::size_t capacity, ThreadHooks hooks)
    : capacity_(capacity == 0 ? 1 : capacity), hooks_(std::move(hooks)) {
  if (concurrency == 0) concurrency = 1;
  for (std::size_t kind = 0; kind < kJobKinds; ++kind) {
    for (std::size_t i = 0; i < concurrency; ++i) {
      threads_.emplace_back([this, kind] { run(static_cast<JobKind>(kind)); });
    }
  }
}

JobQueues::~JobQueues() {
  shutdown(std::chrono::seconds(10));
}

bool JobQueues::enqueue(JobKind kind, Work work) {
  Queue& q = queues_[static_cast<std::size_t>(kind)];
  {
    const std::lock_guard lock(q.mutex);
    if (q.stopping) {
      log_warn("job runner stopped, dropping job {}", job_name(kind));
      return false;
    }
    if (q.jobs.size() >= capacity_) {
      log_error("job queue is full, dropping job {}", job_name(kind));
      return false;
    }
    q.jobs.push_back(std::move(work));
  }
  q.ready.notify_one();
  return true;
}

void JobQueues::shutdown(std::chrono::milliseconds deadline) {
  if (joined_) return;
  joined_ = true;
  const auto until = std::chrono::steady_clock::now() + deadline;
  for (Queue& q : queues_) {
    std::unique_lock lock(q.mutex);
    q.stopping = true;
    if (!q.idle.wait_until(lock, until, [&q] { return q.jobs.empty() && q.running == 0; })) {
      log_warn("jobs still queued at shutdown were dropped");
      q.jobs.clear();
    }
    lock.unlock();
    q.ready.notify_all();
  }
  for (std::thread& t : threads_) t.join();
  threads_.clear();
}

std::size_t JobQueues::pending(JobKind kind) const {
  const Queue& q = queues_[static_cast<std::size_t>(kind)];
  const std::lock_guard lock(q.mutex);
  return q.jobs.size() + q.running;
}

void JobQueues::run(JobKind kind) {
  Queue& q = queues_[static_cast<std::size_t>(kind)];
  if (hooks_.start) hooks_.start();
  for (;;) {
    Work work;
    {
      std::unique_lock lock(q.mutex);
      q.ready.wait(lock, [&q] { return !q.jobs.empty() || q.stopping; });
      if (q.jobs.empty()) break;  // stopping, and nothing left
      work = std::move(q.jobs.front());
      q.jobs.pop_front();
      ++q.running;
    }
    try {
      work();
      log_info("{} performed", job_name(kind));
    } catch (const std::exception& e) {
      log_error("{} failed: {}", job_name(kind), e.what());
    } catch (...) {
      log_error("{} failed", job_name(kind));
    }
    work = nullptr;
    {
      const std::lock_guard lock(q.mutex);
      --q.running;
      if (q.jobs.empty() && q.running == 0) q.idle.notify_all();
    }
  }
  if (hooks_.finish) hooks_.finish();
}

}  // namespace campfire::jobs
