// The place where model callbacks enqueue jobs. Rails: Room::PushMessageJob.perform_later,
// Bot::WebhookJob.perform_later, ActiveStorage::PurgeJob.perform_later (app/models/room.rb, app/models/user/bot.rb).
// Rust: crates/db/src/events.rs (Event). The app uses the real queues (src/app/job_runner.hpp). The tests use a sink
// that records or drops the jobs.
#pragma once

#include <cstdint>

namespace campfire::models {

class JobSink {
 public:
  JobSink() = default;
  JobSink(const JobSink&) = delete;
  JobSink& operator=(const JobSink&) = delete;
  virtual ~JobSink() = default;

  // `Room::PushMessageJob.perform_later(room, message)`
  virtual void push_message(std::int64_t room_id, std::int64_t message_id) = 0;
  // `bot.deliver_webhook_later(message)`: `Bot::WebhookJob.perform_later(bot, message)`
  virtual void deliver_webhook(std::int64_t bot_id, std::int64_t message_id) = 0;
  // `ActiveStorage::PurgeJob.perform_later(blob)` (`purge_later` of an attachment): `blob.purge`, then the blobs that
  // depended on it. A sink that does not override this drops the job.
  virtual void purge_blob(std::int64_t blob_id) { (void)blob_id; }
};

// The sink that drops each job.
class NullJobSink final : public JobSink {
 public:
  void push_message(std::int64_t, std::int64_t) override {}
  void deliver_webhook(std::int64_t, std::int64_t) override {}
};

}  // namespace campfire::models
