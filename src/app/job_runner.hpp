// The job runner of the app: the queues, the thread state of a job thread, and the jobs. Rails: reference/app/jobs/**
// (Room::PushMessageJob, Bot::WebhookJob, RemoveBannedContentJob, ActiveStorage::PurgeJob). Rust:
// crates/campfire/src/jobs.rs and crates/campfire/src/integrations/jobs.rs.
//
// A job runs on a thread of its own queue, with a reader connection and a scheduler of that thread. It writes through
// the writer of the database and waits for the result. Nothing retries a job. A job that fails is logged.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <semaphore>

#include "app/app.hpp"
#include "app/fragment_cache.hpp"
#include "core/scheduler.hpp"
#include "db/connection.hpp"
#include "jobs/queue.hpp"
#include "models/job_sink.hpp"

namespace campfire::app {

// What a job thread owns. `thread()` gives the state of the calling thread.
struct JobThread {
  JobThread(const App& app, db::Connection reader) : reader(std::move(reader)), fragments(*app.fragments) {}
  QueueScheduler scheduler;  // the owner is the thread that built this object
  db::Connection reader;
  WorkerFragmentCache fragments;
};
[[nodiscard]] JobThread& job_thread();

class JobRunner final : public models::JobSink {
 public:
  // Starts `app.config.job_concurrency` threads for each kind of job.
  explicit JobRunner(App& app);
  ~JobRunner() override;

  void push_message(std::int64_t room_id, std::int64_t message_id) override;
  void deliver_webhook(std::int64_t bot_id, std::int64_t message_id) override;
  void purge_blob(std::int64_t blob_id) override;
  // `RemoveBannedContentJob.perform_later(user)`
  void remove_banned_content(std::int64_t user_id);
  // Waits until no job is queued or running, or `timeout` passes. For the tests. True if the runner is idle.
  bool wait_idle(std::chrono::milliseconds timeout);

 private:
  App& app_;
  // `WebPush::Pool`: at most 50 deliveries at once.
  std::counting_semaphore<64> deliveries_{50};
  // The last member: the destructor joins the threads before the other members go.
  jobs::JobQueues queues_;
};

// The jobs. Each runs on a job thread and throws `std::runtime_error` if it fails.
namespace job {
void push_message(App& app, std::counting_semaphore<64>& deliveries, std::int64_t room_id, std::int64_t message_id);
void deliver_webhook(App& app, std::int64_t bot_id, std::int64_t message_id);
void purge_blob(App& app, std::int64_t blob_id);
void remove_banned_content(App& app, std::int64_t user_id);
}  // namespace job

// Parses the VAPID keys into `app.vapid`, starts the queues as `app.job_sink`, and sets the hooks of the models.
void start_jobs(App& app);

}  // namespace campfire::app
