// The calls that a model makes to other areas after a commit: the sockets (A7), the jobs (A9). The default does
// nothing. The area that owns the call sets it once at boot. Rails: ActionCable remote_connections, ActiveJob.
// Rust: crates/db/src/events.rs (Event::DisconnectUser, RemoveBannedContent).
#pragma once

#include <cstdint>
#include <functional>

namespace campfire::models::hooks {

using DisconnectUser = std::function<void(std::int64_t user_id, bool reconnect)>;
using RemoveBannedContent = std::function<void(std::int64_t user_id)>;

// `ActionCable.server.remote_connections.where(current_user: user).disconnect(reconnect:)`
void set_disconnect_user(DisconnectUser fn);
void disconnect_user(std::int64_t user_id, bool reconnect);
// `RemoveBannedContentJob.perform_later(user)`
void set_remove_banned_content(RemoveBannedContent fn);
void remove_banned_content(std::int64_t user_id);

using PurgeBlob = std::function<void(std::int64_t blob_id)>;
using AnalyzeBlob = std::function<void(std::int64_t blob_id)>;

// `ActiveStorage::PurgeJob`: the blob row and its files, when no attachment uses the blob.
void set_purge_blob(PurgeBlob fn);
void purge_blob(std::int64_t blob_id);
// `ActiveStorage::AnalyzeJob`
void set_analyze_blob(AnalyzeBlob fn);
void analyze_blob(std::int64_t blob_id);

}  // namespace campfire::models::hooks
