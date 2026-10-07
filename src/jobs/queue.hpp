// The in-process job queues: one bounded queue for each kind of job, with its own threads. Rails: ActiveJob with the
// Resque adapter (reference/app/jobs/**). Rust: crates/campfire/src/jobs.rs.
//
// A queue that is full drops the new job and logs it, so the writer thread never waits. Nothing retries a job. A job
// that throws is logged. Queued jobs are lost if the process crashes.
#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace campfire::jobs {

enum class JobKind : std::uint8_t { PushMessage, DeliverWebhook, RemoveBannedContent, PurgeBlob };
inline constexpr std::size_t kJobKinds = 4;

// The Rails job class, for the log.
[[nodiscard]] std::string_view job_name(JobKind kind) noexcept;

// How many jobs of one kind may wait before new jobs of that kind are dropped.
inline constexpr std::size_t kQueueCapacity = 1024;

class JobQueues {
 public:
  using Work = std::move_only_function<void()>;
  // Runs on each worker thread, before its first job and after its last one.
  struct ThreadHooks {
    std::function<void()> start;
    std::function<void()> finish;
  };

  // `concurrency` threads for each kind of job.
  explicit JobQueues(std::size_t concurrency, std::size_t capacity = kQueueCapacity, ThreadHooks hooks = {});
  JobQueues(const JobQueues&) = delete;
  JobQueues& operator=(const JobQueues&) = delete;
  // `shutdown` with a deadline of 10 seconds.
  ~JobQueues();

  // True if the job is queued. False if the queue is full or the queues stopped.
  bool enqueue(JobKind kind, Work work);

  // Stops taking jobs. Waits until `deadline` for the queued jobs to run. Drops the jobs that are still queued then,
  // and joins the threads. A job that is running always finishes.
  void shutdown(std::chrono::milliseconds deadline);

  // The jobs that wait or run, for the tests.
  [[nodiscard]] std::size_t pending(JobKind kind) const;

 private:
  struct Queue {
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable idle;
    std::deque<Work> jobs;
    std::size_t running = 0;
    bool stopping = false;
  };
  void run(JobKind kind);

  std::size_t capacity_;
  ThreadHooks hooks_;
  std::array<Queue, kJobKinds> queues_;
  std::vector<std::thread> threads_;
  bool joined_ = false;
};

}  // namespace campfire::jobs
