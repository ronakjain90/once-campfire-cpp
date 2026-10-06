// `has_one_attached :avatar` and `:logo`: the rows of an attachment and its blob. Rails: Active Storage
// (Attached::Changes::CreateOne, Attachment, Blob, VariantRecord), user/avatar.rb, account.rb.
// Rust: crates/campfire/src/controllers/presenters/attachments.rs, crates/storage/src/blob.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "compat/time.hpp"
#include "core/arena.hpp"
#include "core/error.hpp"
#include "db/connection.hpp"
#include "db/database.hpp"
#include "storage/records.hpp"
#include "storage/storage.hpp"

namespace campfire::models::attachments {

// The record that an attachment belongs to (polymorphic).
struct Record {
  std::string_view type;  // "User" or "Account"
  std::int64_t id = 0;
  [[nodiscard]] static Record user(std::int64_t id) { return {"User", id}; }
  [[nodiscard]] static Record account(std::int64_t id) { return {"Account", id}; }
};

// The SQL behind `storage::Records`, over one connection (a reader or the writer).
class AttachmentRecords final : public storage::Records {
 public:
  explicit AttachmentRecords(db::Connection& conn) : conn_(&conn) {}
  Result<std::optional<storage::Blob>> attached(std::string_view record_type, int64_t record_id,
                                                std::string_view name) override;
  Result<storage::Blob> insert_blob(const storage::NewBlob& blob, compat::Timestamp created_at) override;
  Result<int64_t> insert_attachment(std::string_view name, std::string_view record_type, int64_t record_id,
                                    int64_t blob_id, compat::Timestamp created_at) override;
  Status update_metadata(int64_t blob_id, const compat::json::Value& metadata) override;
  Result<std::optional<int64_t>> find_variant_record(int64_t blob_id, std::string_view variation_digest) override;
  Result<std::optional<int64_t>> insert_variant_record(int64_t blob_id, std::string_view variation_digest) override;

 private:
  db::Connection* conn_;
};

// `record.<name>.attached?`
[[nodiscard]] Result<bool> is_attached(db::Connection& conn, Arena& arena, Record record, std::string_view name);

// `record.<name> = uploaded_file`, in the save of the record: the old attachment goes first (its blob id goes in
// `purge`), then the blob and the attachment rows, then the record is touched. It gives the new blob. The file of
// `staged` is already in the service. The caller calls `staged.keep()` after the commit: a destroyed `Staged` deletes
// the file, so a write that rolls back leaves no file.
[[nodiscard]] Result<storage::Blob> attach(db::Tx& tx, Record record, std::string_view name, storage::Staged& staged,
                                           std::vector<std::int64_t>& purge);
// `record.<name>.destroy`: the attachment row and the touch. The id of the blob goes in `purge` (`dependent:
// :purge_later`: the caller purges it after the commit). It gives true if there was an attachment.
[[nodiscard]] Result<bool> destroy(db::Tx& tx, Record record, std::string_view name, std::vector<std::int64_t>& purge);

// `ActiveStorage::Blob#purge`, the row half: nothing happens while an attachment uses the blob. The variant records
// and the preview image attachment go with it, and the ids of their blobs go in `dependents` (purged later). It gives
// the blob that was destroyed, whose files the caller deletes.
[[nodiscard]] Result<std::optional<storage::Blob>> purge_rows(db::Tx& tx, std::int64_t blob_id,
                                                              std::vector<std::int64_t>& dependents);
// `ActiveStorage::Blob.find_by(id:)`
[[nodiscard]] Result<std::optional<storage::Blob>> find_blob(db::Connection& conn, std::int64_t id);
// `record.touch` for a record type of this file.
[[nodiscard]] Status touch(db::Tx& tx, Record record);

}  // namespace campfire::models::attachments
