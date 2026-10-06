// The SQL behind storage::Records. Rails: ActiveStorage::Blob, ActiveStorage::Attachment, ActiveStorage::VariantRecord.
// Rust: the `&Connection` functions of crates/storage/src/blob.rs.
// A reader connection serves `attached` and the `find_*` calls. The writer connection (`tx.conn()`) serves them all.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "compat/time.hpp"
#include "core/arena.hpp"
#include "core/timestamp.hpp"
#include "db/connection.hpp"
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

// `ActiveStorage::Blob.find_by(id:)`
[[nodiscard]] Result<std::optional<storage::Blob>> find_blob(db::Connection& conn, int64_t id);

}  // namespace campfire::models
