// Rails: ActiveStorage::Blob, ActiveStorage::Attachment, ActiveStorage::VariantRecord. Rust:
// crates/storage/src/blob.rs.
#include "models/storage_records.hpp"

#include "core/time_format.hpp"

namespace campfire::models {

namespace {

#define CF_BLOB_COLS(p)                                                                                    \
  p "id, " p "key, " p "filename, " p "content_type, " p "metadata, " p "service_name, " p "byte_size, " p \
    "checksum, " p "created_at"

struct BlobCols {
  int64_t id;
  std::string_view key;
  std::string_view filename;
  std::optional<std::string_view> content_type;
  std::optional<std::string_view> metadata;
  std::string_view service_name;
  int64_t byte_size;
  std::optional<std::string_view> checksum;
  std::string_view created_at;
  static BlobCols read(db::RowReader& r) {
    return {r.i64(0),  r.text(1), r.text(2),     r.text_opt(3), r.text_opt(4),
            r.text(5), r.i64(6),  r.text_opt(7), r.text(8)};
  }
};

const db::Query<BlobCols(std::string_view, int64_t, std::string_view)> kAttached{"SELECT " CF_BLOB_COLS(
    "b.") " FROM active_storage_blobs b JOIN active_storage_attachments a ON a.blob_id = b.id "
          "WHERE a.record_type = ? AND a.record_id = ? AND a.name = ? ORDER BY a.id LIMIT 1"};
const db::Query<BlobCols(int64_t)> kBlobById{"SELECT " CF_BLOB_COLS("") " FROM active_storage_blobs WHERE id = ?"};
const db::Query<int64_t(std::string_view, std::string_view, std::optional<std::string_view>, std::string_view,
                        std::string_view, int64_t, std::string_view, std::string_view)>
    kInsertBlob{
        "INSERT INTO active_storage_blobs (key, filename, content_type, metadata, service_name, byte_size, checksum, "
        "created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?) RETURNING id"};
const db::Query<int64_t(std::string_view, std::string_view, int64_t, int64_t, std::string_view)> kInsertAttachment{
    "INSERT INTO active_storage_attachments (name, record_type, record_id, blob_id, created_at) VALUES (?, ?, ?, ?, ?) "
    "RETURNING id"};
const db::Query<void(std::string_view, int64_t)> kUpdateMetadata{
    "UPDATE active_storage_blobs SET metadata = ? WHERE id = ?"};
const db::Query<int64_t(int64_t, std::string_view)> kFindVariant{
    "SELECT id FROM active_storage_variant_records WHERE blob_id = ? AND variation_digest = ?"};
const db::Query<int64_t(int64_t, std::string_view)> kInsertVariant{
    "INSERT INTO active_storage_variant_records (blob_id, variation_digest) VALUES (?, ?) RETURNING id"};

const db::Query<int64_t(int64_t)> kAnyAttachment{"SELECT 1 FROM active_storage_attachments WHERE blob_id = ? LIMIT 1"};
const db::Query<int64_t(int64_t)> kVariantIds{"SELECT id FROM active_storage_variant_records WHERE blob_id = ?"};
const db::Query<int64_t(std::string_view, int64_t, std::string_view)> kAttachmentBlobId{
    "SELECT blob_id FROM active_storage_attachments WHERE record_type = ? AND record_id = ? AND name = ? LIMIT 1"};
const db::Query<void(std::string_view, int64_t, std::string_view)> kDeleteAttachmentOf{
    "DELETE FROM active_storage_attachments WHERE record_type = ? AND record_id = ? AND name = ?"};
const db::Query<void(int64_t)> kDeleteVariant{"DELETE FROM active_storage_variant_records WHERE id = ?"};
const db::Query<void(int64_t)> kDeleteBlob{"DELETE FROM active_storage_blobs WHERE id = ?"};

storage::Blob make_blob(const BlobCols& row) {
  storage::Blob blob;
  blob.id = row.id;
  blob.key = std::string(row.key);
  blob.filename = storage::Filename(std::string(row.filename));
  if (row.content_type) blob.content_type = std::string(*row.content_type);
  if (row.metadata) {
    if (auto parsed = compat::json::parse(*row.metadata); parsed && parsed->is_object()) blob.metadata = *parsed;
  }
  blob.service_name = std::string(row.service_name);
  blob.byte_size = row.byte_size;
  if (row.checksum) blob.checksum = std::string(*row.checksum);
  blob.created_at = std::string(row.created_at);
  return blob;
}

}  // namespace

Result<std::optional<storage::Blob>> DbRecords::attached(std::string_view record_type, int64_t record_id,
                                                         std::string_view name) {
  Arena arena(512);
  auto row = conn_->first(kAttached, arena, record_type, record_id, name);
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<storage::Blob>{};
  return std::optional<storage::Blob>(make_blob(**row));
}

Result<storage::Blob> DbRecords::insert_blob(const storage::NewBlob& blob, compat::Timestamp created_at) {
  Arena arena(512);
  const std::string created = format_db(from_compat(created_at));
  const std::string metadata = compat::json::encode(blob.metadata);
  std::optional<std::string_view> content_type;
  if (blob.content_type) content_type = std::string_view(*blob.content_type);
  auto id = conn_->first(kInsertBlob, arena, std::string_view(blob.key), std::string_view(blob.filename.raw()),
                         content_type, std::string_view(metadata), std::string_view(blob.service_name), blob.byte_size,
                         std::string_view(blob.checksum), std::string_view(created));
  if (!id) return std::unexpected(id.error());
  storage::Blob saved;
  saved.id = **id;
  saved.key = blob.key;
  saved.filename = blob.filename;
  saved.content_type = blob.content_type;
  saved.metadata = blob.metadata;
  saved.service_name = blob.service_name;
  saved.byte_size = blob.byte_size;
  saved.checksum = blob.checksum;
  saved.created_at = created;
  return saved;
}

Result<int64_t> DbRecords::insert_attachment(std::string_view name, std::string_view record_type, int64_t record_id,
                                             int64_t blob_id, compat::Timestamp created_at) {
  Arena arena(256);
  const std::string created = format_db(from_compat(created_at));
  auto id = conn_->first(kInsertAttachment, arena, name, record_type, record_id, blob_id, std::string_view(created));
  if (!id) return std::unexpected(id.error());
  return **id;
}

Status DbRecords::update_metadata(int64_t blob_id, const compat::json::Value& metadata) {
  const std::string text = compat::json::encode(metadata);
  auto r = conn_->exec(kUpdateMetadata, std::string_view(text), blob_id);
  if (!r) return std::unexpected(r.error());
  return {};
}

Result<std::optional<int64_t>> DbRecords::find_variant_record(int64_t blob_id, std::string_view variation_digest) {
  Arena arena(256);
  return conn_->first(kFindVariant, arena, blob_id, variation_digest);
}

Result<std::optional<int64_t>> DbRecords::insert_variant_record(int64_t blob_id, std::string_view variation_digest) {
  Arena arena(256);
  auto id = conn_->first(kInsertVariant, arena, blob_id, variation_digest);
  if (id) return *id;
  // A unique index violation: another writer made the row first.
  if (id.error().message.find("UNIQUE") != std::string::npos) return std::optional<int64_t>{};
  return std::unexpected(id.error());
}

Result<std::optional<storage::Blob>> find_blob(db::Connection& conn, int64_t id) {
  Arena arena(512);
  auto row = conn.first(kBlobById, arena, id);
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<storage::Blob>{};
  return std::optional<storage::Blob>(make_blob(**row));
}

Result<PurgedBlob> purge_blob_rows(db::Tx& tx, int64_t blob_id) {
  Arena arena(512);
  PurgedBlob out;
  auto blob = find_blob(tx.conn(), blob_id);
  if (!blob) return std::unexpected(blob.error());
  if (!*blob) return out;
  // before_destroy(prepend: true) { raise ActiveRecord::InvalidForeignKey if attachments.exists? }
  auto attached = tx.conn().first(kAnyAttachment, arena, blob_id);
  if (!attached) return std::unexpected(attached.error());
  if (*attached) return out;
  const auto destroy_attachment = [&](std::string_view type, int64_t id, std::string_view name) -> Status {
    auto dependent = tx.conn().first(kAttachmentBlobId, arena, type, id, name);
    if (!dependent) return std::unexpected(dependent.error());
    if (!*dependent) return {};
    if (auto r = tx.conn().exec(kDeleteAttachmentOf, type, id, name); !r) return std::unexpected(r.error());
    out.dependent_ids.push_back(**dependent);
    return {};
  };
  // before_destroy { variant_records.destroy_all }
  auto variants = tx.conn().all(kVariantIds, arena, blob_id);
  if (!variants) return std::unexpected(variants.error());
  for (const int64_t variant_id : *variants) {
    if (auto s = destroy_attachment("ActiveStorage::VariantRecord", variant_id, "image"); !s) {
      return std::unexpected(s.error());
    }
    if (auto r = tx.conn().exec(kDeleteVariant, variant_id); !r) return std::unexpected(r.error());
  }
  // has_one_attached :preview_image
  if (auto s = destroy_attachment("ActiveStorage::Blob", blob_id, "preview_image"); !s)
    return std::unexpected(s.error());
  if (auto r = tx.conn().exec(kDeleteBlob, blob_id); !r) return std::unexpected(r.error());
  out.blob = std::move(**blob);
  return out;
}

}  // namespace campfire::models
