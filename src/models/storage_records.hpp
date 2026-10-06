// The SQL behind storage::Records. Rails: ActiveStorage::Blob, ActiveStorage::Attachment, ActiveStorage::VariantRecord.
// Rust: the `&Connection` functions of crates/storage/src/blob.rs.
// A reader connection serves `attached` and the `find_*` calls. The writer connection (`tx.conn()`) serves them all.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "compat/time.hpp"
#include "core/arena.hpp"
#include "core/timestamp.hpp"
#include "db/connection.hpp"
#include "db/database.hpp"
#include "storage/records.hpp"

namespace campfire::models {

// `core/timestamp.hpp` and `compat/time.hpp` each have a time type.
[[nodiscard]] inline Timestamp from_compat(compat::Timestamp t) {
  return Timestamp::from_nanos(t.time_since_epoch().count());
}
[[nodiscard]] inline compat::Timestamp to_compat(Timestamp t) {
  return compat::Timestamp{std::chrono::nanoseconds(t.seconds * 1'000'000'000LL + t.nanos)};
}

class DbRecords final : public storage::Records {
 public:
  explicit DbRecords(db::Connection& conn) : conn_(&conn) {}

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

// The rows that `ActiveStorage::Blob#purge` removes in its transaction.
struct PurgedBlob {
  std::optional<storage::Blob> blob;        // nothing when the blob is gone or still attached
  std::vector<std::int64_t> dependent_ids;  // blobs of the variant images and the preview image: purge them next
};
// `blob.purge`: `destroy` is refused while an attachment points at the blob. It destroys the variant records (and the
// attachments of their images) and the `preview_image` attachment. The caller deletes the files and purges the
// dependents (`after_destroy_commit :purge_dependent_blob_later`).
[[nodiscard]] Result<PurgedBlob> purge_blob_rows(db::Tx& tx, std::int64_t blob_id);

// `ActiveStorage::Blob.find_by(id:)`
[[nodiscard]] Result<std::optional<storage::Blob>> find_blob(db::Connection& conn, int64_t id);

}  // namespace campfire::models
