// ActiveStorage::PurgeJob and RemoveBannedContentJob. Rails: app/jobs/remove_banned_content_job.rb,
// app/models/user/bannable.rb, active_storage/blob#purge. Rust: crates/campfire/src/jobs.rs.
#include "app/broadcasts.hpp"
#include "app/job_support.hpp"
#include "core/log.hpp"
#include "models/message.hpp"
#include "models/room_ref.hpp"
#include "models/storage_records.hpp"
#include "models/user_admin.hpp"
#include "storage/storage.hpp"

namespace campfire::app::job {

// `blob.purge`, then the blobs that depended on it.
void purge_blob(App& app, std::int64_t blob_id) {
  std::vector<std::int64_t> pending{blob_id};
  const storage::Storage& store = *app.storage;
  while (!pending.empty()) {
    const std::int64_t id = pending.back();
    pending.pop_back();
    const auto purged = write(app, [id](db::Tx& tx) { return models::purge_blob_rows(tx, id); });
    if (!purged) raise(purged.error().message);
    if (!purged->blob) continue;
    if (auto deleted = store.delete_files(*purged->blob); !deleted) {
      log_error("purge of blob {} left files: {}", id, deleted.error().message);
    }
    pending.insert(pending.end(), purged->dependent_ids.begin(), purged->dependent_ids.end());
  }
}

// `user.remove_banned_content`: `messages.each { |message| message.destroy; message.broadcast_remove }`. Each message
// is destroyed in its own transaction.
void remove_banned_content(App& app, std::int64_t user_id) {
  JobThread& thread = job_thread();
  db::Connection& conn = thread.reader;
  Arena arena(4096);
  const std::vector<std::int64_t> ids = must(models::users::message_ids(conn, arena, user_id));
  for (const std::int64_t id : ids) {
    Arena each(2048);
    auto message = must(models::messages::find_by_id(conn, each, id));
    if (!message) continue;
    const auto destroyed = write(app, [&](db::Tx& tx) { return models::messages::destroy(tx, *message); });
    if (!destroyed) raise(destroyed.error().message);
    if (destroyed->purged_blob_id) app.job_sink->purge_blob(*destroyed->purged_blob_id);
    auto room = must(models::room_refs::find(conn, each, message->room_id));
    if (room) broadcasts::message_remove(app, *room, message->client_message_id);
  }
}

}  // namespace campfire::app::job
