// Rails: Attached::Changes::CreateOne, ActiveStorage::AnalyzeJob, PurgeJob, Blob#variant. Rust: crates/campfire/src/
// controllers/presenters/attachments.rs, crates/campfire/src/active_storage.rs.
#include "app/active_storage.hpp"

#include <chrono>

#include "core/log.hpp"
#include "models/account_admin.hpp"

namespace campfire::app::active_storage {

namespace {

compat::Timestamp compat_now(Timestamp t) {
  return compat::Timestamp{std::chrono::nanoseconds(t.seconds * 1'000'000'000LL + t.nanos)};
}

}  // namespace

Assignment assignment_from(const req::ParamMap& params, std::string_view key) {
  Assignment assignment;
  if (!params.contains(key)) return assignment;
  const req::Param* param = params.get(key);
  if (param == nullptr || param->is_null() || param->as_str() == std::optional<std::string_view>("")) {
    assignment.kind = Assignment::Kind::Delete;
    return assignment;
  }
  if (const auto* file = param->as_file()) {
    assignment.kind = Assignment::Kind::Create;
    assignment.file = *file;
    return assignment;
  }
  assignment.kind = Assignment::Kind::Invalid;
  return assignment;
}

Task<Flow<void>> stage(Rq& rq, Assignment& assignment) {
  if (assignment.kind != Assignment::Kind::Create) co_return Flow<void>{};
  const storage::Storage& storage = *rq.app.storage;
  const std::shared_ptr<req::UploadedFile> file = assignment.file;
  auto staged = co_await rq.ctx.offload(rq.app.jobs, [&storage, file]() -> Result<storage::Staged> {
    std::optional<std::string_view> type;
    if (file->content_type) type = std::string_view(*file->content_type);
    return storage.stage_file(file->path, storage::Filename(file->original_filename), type);
  });
  if (!staged) co_return fail_internal(staged.error().message);
  assignment.staged.emplace(std::move(*staged));
  co_return Flow<void>{};
}

Status apply(db::Tx& tx, models::attachments::Record record, std::string_view name, Assignment& assignment,
             Applied& applied) {
  switch (assignment.kind) {
    case Assignment::Kind::Unchanged: return {};
    case Assignment::Kind::Delete: {
      auto removed = models::attachments::destroy(tx, record, name, applied.purge);
      if (!removed) return std::unexpected(removed.error());
      return {};
    }
    case Assignment::Kind::Create: {
      auto blob = models::attachments::attach(tx, record, name, *assignment.staged, applied.purge);
      if (!blob) return std::unexpected(blob.error());
      applied.blob = std::move(*blob);
      return {};
    }
    case Assignment::Kind::Invalid: return fail(Errc::Internal, "Could not find or build blob: expected attachable");
  }
  return {};
}

Task<void> purge(Rq& rq, std::vector<std::int64_t> blob_ids) {
  const storage::Storage& storage = *rq.app.storage;
  while (!blob_ids.empty()) {
    const std::int64_t blob_id = blob_ids.back();
    blob_ids.pop_back();
    std::vector<std::int64_t> dependents;
    std::optional<storage::Blob> destroyed;
    auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
      dependents.clear();
      auto purged = models::attachments::purge_rows(tx, blob_id, dependents);
      if (!purged) return std::unexpected(purged.error());
      destroyed = std::move(*purged);
      return {};
    });
    if (!written) {
      log_error("purge of blob {} failed: {}", blob_id, written.error().message);
      continue;
    }
    for (const std::int64_t dependent : dependents) blob_ids.push_back(dependent);
    if (destroyed) {
      const storage::Blob blob = std::move(*destroyed);
      auto deleted = co_await rq.ctx.offload(rq.app.jobs, [&storage, blob] { return storage.delete_files(blob); });
      if (!deleted) log_error("deleting the files of blob {} failed: {}", blob_id, deleted.error().message);
    }
  }
}

Task<void> after_write(Rq& rq, models::attachments::Record record, Assignment& assignment, Applied& applied) {
  if (assignment.staged) assignment.staged->keep();
  if (applied.blob) {
    const storage::Storage& storage = *rq.app.storage;
    const storage::Blob blob = *applied.blob;
    // `ActiveStorage::AnalyzeJob`: `blob.analyze`, then `touch_attachment_records`.
    auto metadata = co_await rq.ctx.offload(rq.app.jobs, [&storage, blob] { return storage.analyzed_metadata(blob); });
    if (metadata) {
      const compat::json::Value value = std::move(*metadata);
      auto saved = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
        models::attachments::AttachmentRecords records(tx.conn());
        if (auto updated = records.update_metadata(blob.id, value); !updated) return updated;
        return models::attachments::touch(tx, record);
      });
      if (!saved) log_error("analysis of blob {} failed: {}", blob.id, saved.error().message);
    } else {
      log_error("analysis of blob {} failed: {}", blob.id, metadata.error().message);
    }
  }
  if (!applied.purge.empty()) co_await purge(rq, std::move(applied.purge));
}

Task<Flow<std::optional<storage::Blob>>> processed_variant(Rq& rq, models::attachments::Record record,
                                                           std::string_view name,
                                                           const storage::Variation& transformations) {
  const storage::Storage& storage = *rq.app.storage;
  models::attachments::AttachmentRecords reader(rq.db());
  auto attached = reader.attached(record.type, record.id, name);
  if (!attached) co_return fail_internal(attached.error().message);
  if (!*attached || !(*attached)->is_variable()) co_return std::optional<storage::Blob>{};
  const storage::Blob blob = std::move(**attached);
  auto variation = storage.variation_for(blob, transformations);
  if (!variation) co_return fail_internal(variation.error().message);
  auto existing = storage.existing_variant(reader, blob, *variation);
  if (!existing) co_return fail_internal(existing.error().message);
  if (*existing) co_return std::optional<storage::Blob>(std::move(**existing));
  // `VariantWithRecord#process`: the file work off the writer, then the rows.
  const storage::Variation wanted = *variation;
  auto image = co_await rq.ctx.offload(rq.app.jobs, [&storage, blob, wanted] { return storage.transform_variant(blob, wanted); });
  if (!image) co_return fail_internal(image.error().message);
  std::optional<storage::Blob> recorded;
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
    models::attachments::AttachmentRecords records(tx.conn());
    auto saved = storage.record_variant(records, blob, wanted, *image, compat_now(tx.now()));
    if (!saved) return std::unexpected(saved.error());
    recorded = std::move(*saved);
    if (recorded) {
      tx.changed(db::schema::Table::ActiveStorageBlobs, recorded->id);
      tx.changed(db::schema::Table::ActiveStorageVariantRecords, blob.id);
    }
    return {};
  });
  if (!written) co_return fail_internal(written.error().message);
  if (recorded) {
    image->keep();
    co_return std::optional<storage::Blob>(std::move(*recorded));
  }
  // Another request recorded the variant first.
  auto winner = storage.existing_variant(reader, blob, wanted);
  if (!winner) co_return fail_internal(winner.error().message);
  if (!*winner) co_return fail_internal("variant record is gone");
  co_return std::optional<storage::Blob>(std::move(**winner));
}

}  // namespace campfire::app::active_storage
