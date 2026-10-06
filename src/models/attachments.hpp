// `has_one_attached :avatar` and `:logo`: the rows of an attachment and its blob. Rails: Active Storage
// (Attached::Changes::CreateOne, Attachment, Blob, VariantRecord), user/avatar.rb, account.rb.
// Rust: crates/campfire/src/controllers/presenters/attachments.rs, crates/storage/src/blob.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

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

// `record.<name> = uploaded_file`, in the save of the record: the old attachment goes first, then the blob and the
// attachment rows, then the record is touched. It gives the id of the new blob (to analyze after the commit). The
// file of `staged` is already in the service. The caller calls `staged.keep()` after the commit: a destroyed
// `Staged` deletes the file, so a write that rolls back leaves no file.
[[nodiscard]] Result<std::int64_t> attach(db::Tx& tx, Record record, std::string_view name, storage::Staged& staged);
// `record.<name>.destroy`: the attachment row, the touch, and the purge of the blob after the commit.
// It gives true if there was an attachment.
[[nodiscard]] Result<bool> destroy(db::Tx& tx, Record record, std::string_view name);

}  // namespace campfire::models::attachments
