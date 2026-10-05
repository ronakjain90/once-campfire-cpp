// The row operations Active Storage needs from the database. T7 (src/db) or src/models
// implements this interface with SQL. All functions take and return plain structs.
// Rust: the `&Connection` functions of crates/storage/src/blob.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "compat/json.hpp"
#include "compat/time.hpp"
#include "core/error.hpp"
#include "storage/blob.hpp"

namespace campfire::storage {

class Records {
 public:
  virtual ~Records() = default;

  // The blob attached to a record under `name` (has_one_attached): ORDER BY attachments.id LIMIT 1.
  virtual Result<std::optional<Blob>> attached(std::string_view record_type, int64_t record_id,
                                               std::string_view name) = 0;
  // INSERT INTO active_storage_blobs. Returns the saved row, with its id.
  virtual Result<Blob> insert_blob(const NewBlob& blob, compat::Timestamp created_at) = 0;
  // INSERT INTO active_storage_attachments. Returns the id of the new row.
  virtual Result<int64_t> insert_attachment(std::string_view name, std::string_view record_type, int64_t record_id,
                                            int64_t blob_id, compat::Timestamp created_at) = 0;
  // UPDATE active_storage_blobs SET metadata.
  virtual Status update_metadata(int64_t blob_id, const json::Value& metadata) = 0;
  // blob.variant_records.find_by(variation_digest:).
  virtual Result<std::optional<int64_t>> find_variant_record(int64_t blob_id, std::string_view variation_digest) = 0;
  // The INSERT half of create_or_find_by!: nullopt when another writer made the row first
  // (a unique index violation).
  virtual Result<std::optional<int64_t>> insert_variant_record(int64_t blob_id, std::string_view variation_digest) = 0;
};

}  // namespace campfire::storage
