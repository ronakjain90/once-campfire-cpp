// Rails: reference/app/jobs/application_job.rb and the jobs. Rust: crates/campfire/src/jobs.rs,
// crates/campfire/src/integrations/jobs.rs (register_jobs, web_push_pool).
#include "app/job_runner.hpp"

#include <thread>

#include "app/job_support.hpp"
#include "app/web_push.hpp"
#include "core/log.hpp"
#include "jobs/web_push.hpp"
#include "models/hooks.hpp"
#include "views/fragment_cache.hpp"

namespace campfire::app {

namespace {

thread_local std::unique_ptr<JobThread> t_thread;

}  // namespace

JobThread& job_thread() {
  if (!t_thread) {
    log_error("a job runs on a thread that has no job state");
    std::abort();
  }
  return *t_thread;
}

JobRunner::JobRunner(App& app)
    : app_(app),
      queues_(app.config.job_concurrency, jobs::kQueueCapacity,
              {[&app] {
                 auto reader = app.db->open_reader();
                 if (!reader) {
                   log_error("job thread cannot open the database: {}", reader.error().message);
                   std::abort();
                 }
                 t_thread = std::make_unique<JobThread>(app, std::move(*reader));
                 views::set_fragment_cache(&t_thread->fragments);
               },
               [] {
                 views::set_fragment_cache(nullptr);
                 t_thread.reset();
               }}) {}

JobRunner::~JobRunner() {
  queues_.shutdown(std::chrono::seconds(10));
}

void JobRunner::push_message(std::int64_t room_id, std::int64_t message_id) {
  queues_.enqueue(jobs::JobKind::PushMessage,
                  [this, room_id, message_id] { job::push_message(app_, deliveries_, room_id, message_id); });
}

void JobRunner::deliver_webhook(std::int64_t bot_id, std::int64_t message_id) {
  queues_.enqueue(jobs::JobKind::DeliverWebhook,
                  [this, bot_id, message_id] { job::deliver_webhook(app_, bot_id, message_id); });
}

void JobRunner::purge_blob(std::int64_t blob_id) {
  queues_.enqueue(jobs::JobKind::PurgeBlob, [this, blob_id] { job::purge_blob(app_, blob_id); });
}

void JobRunner::remove_banned_content(std::int64_t user_id) {
  queues_.enqueue(jobs::JobKind::RemoveBannedContent, [this, user_id] { job::remove_banned_content(app_, user_id); });
}

bool JobRunner::wait_idle(std::chrono::milliseconds timeout) {
  const auto until = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    bool idle = true;
    for (std::size_t kind = 0; kind < jobs::kJobKinds; ++kind) {
      if (queues_.pending(static_cast<jobs::JobKind>(kind)) != 0) idle = false;
    }
    if (idle) return true;
    if (std::chrono::steady_clock::now() >= until) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void start_jobs(App& app) {
  // config/initializers/web_push.rb: keys that are missing or invalid turn Web Push off.
  if (!app.config.vapid_public_key || !app.config.vapid_private_key) {
    log_warn("Web Push is off: VAPID_PUBLIC_KEY and VAPID_PRIVATE_KEY aren't set");
  } else {
    auto vapid = jobs::web_push::Vapid::create(app.config.vapid_subject, *app.config.vapid_public_key,
                                               *app.config.vapid_private_key);
    if (vapid) {
      app.vapid = std::make_shared<const jobs::web_push::Vapid>(std::move(*vapid));
    } else {
      log_error("Web Push is off: {}", jobs::web_push::to_string(vapid.error()));
    }
  }
  auto runner = std::make_shared<JobRunner>(app);
  app.job_sink = runner;
  // The models call these after a commit. The hooks are global: the newest app wins. A hook that outlives its app does
  // nothing.
  models::hooks::set_remove_banned_content([weak = std::weak_ptr<JobRunner>(runner)](std::int64_t user_id) {
    if (auto locked = weak.lock()) locked->remove_banned_content(user_id);
  });
}

}  // namespace campfire::app
