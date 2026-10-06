// Rails: Active Storage Attachment and Blob rows. Rust: crates/storage/src/blob.rs, presenters/attachments.rs.
#include "models/attachments.hpp"

#include "compat/json.hpp"
#include "core/time_format.hpp"
#include "models/account_admin.hpp"
#include "models/hooks.hpp"

namespace campfire::models::attachments {

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

struct AttachmentIds {
  int64_t id;
  int64_t blob_id;
  static AttachmentIds read(db::RowReader& r) { return {r.i64(0), r.i64(1)}; }
};
const db::Query<AttachmentIds(std::string_view, int64_t, std::string_view)> kFindAttachment{
    "SELECT id, blob_id FROM active_storage_attachments WHERE record_type = ? AND record_id = ? AND name = ? LIMIT 1"};
const db::Query<void(int64_t)> kDeleteAttachment{"DELETE FROM active_storage_attachments WHERE id = ?"};
const db::Query<int64_t(std::string_view, int64_t, std::string_view)> kAttachedCount{
    "SELECT 1 AS one FROM active_storage_attachments WHERE record_type = ? AND record_id = ? AND name = ? LIMIT 1"};

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

Timestamp from_compat(compat::Timestamp t) {
  return Timestamp::from_nanos(t.time_since_epoch().count());
}

const db::Query<BlobCols(int64_t)> kBlobById{"SELECT " CF_BLOB_COLS("") " FROM active_storage_blobs WHERE id = ?"};
const db::Query<int64_t(int64_t)> kBlobAttachments{
    "SELECT id FROM active_storage_attachments WHERE blob_id = ? LIMIT 1"};
const db::Query<int64_t(int64_t)> kVariantRecords{"SELECT id FROM active_storage_variant_records WHERE blob_id = ?"};
const db::Query<void(int64_t)> kDeleteVariantRecord{"DELETE FROM active_storage_variant_records WHERE id = ?"};
const db::Query<void(int64_t)> kDeleteBlob{"DELETE FROM active_storage_blobs WHERE id = ?"};

}  // namespace

Status touch(db::Tx& tx, Record record) {
  return record.type == "Account" ? accounts::touch_account(tx, record.id) : accounts::touch_user(tx, record.id);
}

Result<std::optional<storage::Blob>> find_blob(db::Connection& conn, std::int64_t id) {
  Arena arena(512);
  auto row = conn.first(kBlobById, arena, id);
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<storage::Blob>{};
  return std::optional<storage::Blob>(make_blob(**row));
}

Result<std::optional<storage::Blob>> purge_rows(db::Tx& tx, std::int64_t blob_id,
                                                std::vector<std::int64_t>& dependents) {
  auto blob = find_blob(tx.conn(), blob_id);
  if (!blob) return std::unexpected(blob.error());
  if (!*blob) return std::optional<storage::Blob>{};
  Arena arena(512);
  // `before_destroy(prepend: true) { raise ActiveRecord::InvalidForeignKey if attachments.exists? }`
  auto used = tx.conn().first(kBlobAttachments, arena, blob_id);
  if (!used) return std::unexpected(used.error());
  if (*used) return std::optional<storage::Blob>{};
  // `before_destroy { variant_records.destroy_all }`: the image of each record goes with it.
  auto records = tx.conn().all(kVariantRecords, arena, blob_id);
  if (!records) return std::unexpected(records.error());
  for (const std::int64_t record_id : *records) {
    auto found = tx.conn().first(kFindAttachment, arena, "ActiveStorage::VariantRecord", record_id, "image");
    if (!found) return std::unexpected(found.error());
    if (*found) {
      if (auto done = tx.conn().exec(kDeleteAttachment, (*found)->id); !done) return std::unexpected(done.error());
      dependents.push_back((*found)->blob_id);
    }
    if (auto done = tx.conn().exec(kDeleteVariantRecord, record_id); !done) return std::unexpected(done.error());
  }
  // `has_one_attached :preview_image`
  auto preview = tx.conn().first(kFindAttachment, arena, "ActiveStorage::Blob", blob_id, "preview_image");
  if (!preview) return std::unexpected(preview.error());
  if (*preview) {
    if (auto done = tx.conn().exec(kDeleteAttachment, (*preview)->id); !done) return std::unexpected(done.error());
    dependents.push_back((*preview)->blob_id);
  }
  if (auto done = tx.conn().exec(kDeleteBlob, blob_id); !done) return std::unexpected(done.error());
  tx.changed(db::schema::Table::ActiveStorageBlobs, blob_id);
  return blob;
}

Result<std::optional<storage::Blob>> AttachmentRecords::attached(std::string_view record_type, int64_t record_id,
                                                                 std::string_view name) {
  Arena arena(512);
  auto row = conn_->first(kAttached, arena, record_type, record_id, name);
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<storage::Blob>{};
  return std::optional<storage::Blob>(make_blob(**row));
}

Result<storage::Blob> AttachmentRecords::insert_blob(const storage::NewBlob& blob, compat::Timestamp created_at) {
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

Result<int64_t> AttachmentRecords::insert_attachment(std::string_view name, std::string_view record_type,
                                                     int64_t record_id, int64_t blob_id, compat::Timestamp created_at) {
  Arena arena(256);
  const std::string created = format_db(from_compat(created_at));
  auto id = conn_->first(kInsertAttachment, arena, name, record_type, record_id, blob_id, std::string_view(created));
  if (!id) return std::unexpected(id.error());
  return **id;
}

Status AttachmentRecords::update_metadata(int64_t blob_id, const compat::json::Value& metadata) {
  const std::string text = compat::json::encode(metadata);
  auto done = conn_->exec(kUpdateMetadata, std::string_view(text), blob_id);
  if (!done) return std::unexpected(done.error());
  return {};
}

Result<std::optional<int64_t>> AttachmentRecords::find_variant_record(int64_t blob_id,
                                                                      std::string_view variation_digest) {
  Arena arena(256);
  return conn_->first(kFindVariant, arena, blob_id, variation_digest);
}

Result<std::optional<int64_t>> AttachmentRecords::insert_variant_record(int64_t blob_id,
                                                                        std::string_view variation_digest) {
  Arena arena(256);
  auto id = conn_->first(kInsertVariant, arena, blob_id, variation_digest);
  if (id) return *id;
  // A unique index violation: another writer made the row first.
  if (id.error().message.find("UNIQUE") != std::string::npos) return std::optional<int64_t>{};
  return std::unexpected(id.error());
}

Result<bool> is_attached(db::Connection& conn, Arena& arena, Record record, std::string_view name) {
  auto row = conn.first(kAttachedCount, arena, record.type, record.id, name);
  if (!row) return std::unexpected(row.error());
  return row->has_value();
}

Result<bool> destroy(db::Tx& tx, Record record, std::string_view name, std::vector<std::int64_t>& purge) {
  Arena arena(256);
  auto found = tx.conn().first(kFindAttachment, arena, record.type, record.id, name);
  if (!found) return std::unexpected(found.error());
  if (!*found) return false;
  const AttachmentIds ids = **found;
  if (auto done = tx.conn().exec(kDeleteAttachment, ids.id); !done) return std::unexpected(done.error());
  tx.changed(db::schema::Table::ActiveStorageAttachments, ids.id);
  if (auto touched = touch(tx, record); !touched) return std::unexpected(touched.error());
  purge.push_back(ids.blob_id);
  return true;
}

Result<storage::Blob> attach(db::Tx& tx, Record record, std::string_view name, storage::Staged& staged,
                             std::vector<std::int64_t>& purge) {
  if (auto removed = destroy(tx, record, name, purge); !removed) return std::unexpected(removed.error());
  AttachmentRecords records(tx.conn());
  const Timestamp at = tx.now();
  const compat::Timestamp now{std::chrono::nanoseconds(at.seconds * 1'000'000'000LL + at.nanos)};
  auto blob = staged.insert(records, now);
  if (!blob) return std::unexpected(blob.error());
  auto attachment = records.insert_attachment(name, record.type, record.id, blob->id, now);
  if (!attachment) return std::unexpected(attachment.error());
  tx.changed(db::schema::Table::ActiveStorageBlobs, blob->id);
  tx.changed(db::schema::Table::ActiveStorageAttachments, *attachment);
  if (auto touched = touch(tx, record); !touched) return std::unexpected(touched.error());
  return blob;
}

}  // namespace campfire::models::attachments
