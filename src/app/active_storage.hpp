// What the controllers of the avatar and the logo need from Active Storage: an uploaded file as an assignment, the
// save in the write of the record, the analysis and the purge after it, and the processed variant.
// Rails: Attached::Changes::CreateOne, ActiveStorage::AnalyzeJob, PurgeJob, Blob#variant. Rust: crates/campfire/src/
// controllers/presenters/attachments.rs, crates/campfire/src/active_storage.rs.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "app/flow.hpp"
#include "app/rq.hpp"
#include "core/task.hpp"
#include "models/attachments.hpp"
#include "req/param.hpp"
#include "storage/storage.hpp"

namespace campfire::app::active_storage {

// What `record.avatar = value` does with a permitted param value.
struct Assignment {
  enum class Kind : std::uint8_t {
    Unchanged,  // the key was not given
    Delete,     // nil or "": the attachment is destroyed on save
    Create,     // an uploaded file
    Invalid     // anything else: Rails raises
  };
  Kind kind = Kind::Unchanged;
  std::shared_ptr<req::UploadedFile> file;
  std::optional<storage::Staged> staged;  // the file after `stage`
};

// The assignment for `params[key]`.
[[nodiscard]] Assignment assignment_from(const req::ParamMap& params, std::string_view key);
// Copies the uploaded file into the service (on the job pool), so that the write has rows to save only.
[[nodiscard]] Task<Flow<void>> stage(Rq& rq, Assignment& assignment);

// The outcome of `apply`, for `after_write`.
struct Applied {
  std::optional<storage::Blob> blob;  // the new blob: analyze it
  std::vector<std::int64_t> purge;    // the old blobs: purge them
};
// In the write of the record: `record.<name> = value`.
[[nodiscard]] Status apply(db::Tx& tx, models::attachments::Record record, std::string_view name,
                           Assignment& assignment, Applied& applied);
// After the commit: keeps the file, analyzes the new blob and purges the old ones. The Rails jobs run in the
// background. This runs in the request after the write. A failure is logged and does not change the response.
[[nodiscard]] Task<void> after_write(Rq& rq, models::attachments::Record record, Assignment& assignment,
                                     Applied& applied);

// `ActiveStorage::Blob#purge` for each blob (and the blobs of its variants).
[[nodiscard]] Task<void> purge(Rq& rq, std::vector<std::int64_t> blob_ids);

// `record.<name>.variant(transformations).processed if record.<name>.variable?`: the blob of the processed variant,
// or nothing if there is no attachment or it cannot be transformed.
[[nodiscard]] Task<Flow<std::optional<storage::Blob>>> processed_variant(Rq& rq, models::attachments::Record record,
                                                                         std::string_view name,
                                                                         const storage::Variation& transformations);

}  // namespace campfire::app::active_storage
