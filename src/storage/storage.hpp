// The Active Storage flows Campfire drives: uploads, analysis, tracked variants and video previews
// (Rails: ActiveStorage::Blob, VariantWithRecord, Preview; Rust: crates/storage/src/storage.rs).
//
// Each flow has two parts, so the slow part never holds the database writer. The file work
// (copy, checksum, libvips, ffmpeg, analysis) needs no database and blocks: call it on the media
// pool. It leaves a Staged blob with its file already in the service. The record step saves rows
// through `Records` and is quick.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "compat/message_verifier.hpp"
#include "core/error.hpp"
#include "storage/blob.hpp"
#include "storage/disk.hpp"
#include "storage/records.hpp"
#include "storage/tempfile.hpp"
#include "storage/transform.hpp"

namespace campfire::storage {

// A new blob whose file is in the service but whose row is not saved. A destroyed Staged deletes
// the file, so a write that rolls back leaves no orphan. Call keep() once the row is committed.
class Staged {
 public:
  Staged(NewBlob blob, DiskService service) : blob_(std::move(blob)), service_(std::move(service)) {}
  Staged(Staged&& o) noexcept : blob_(std::move(o.blob_)), service_(std::move(o.service_)), kept_(o.kept_) { o.kept_ = true; }
  Staged(const Staged&) = delete;
  Staged& operator=(const Staged&) = delete;
  Staged& operator=(Staged&&) = delete;
  ~Staged() {
    if (!kept_) (void)service_.remove(blob_.key);
  }

  const NewBlob& blob() const { return blob_; }
  NewBlob& blob() { return blob_; }
  Result<Blob> insert(Records& records, compat::Timestamp now) const { return records.insert_blob(blob_, now); }
  void keep() { kept_ = true; }

 private:
  NewBlob blob_;
  DiskService service_;
  bool kept_ = false;
};

class Storage {
 public:
  // `verifier` is ActiveStorage.verifier: secrets.active_storage_verifier(). It signs blob ids,
  // variation keys, disk URLs and direct upload tokens.
  Storage(DiskService service, compat::MessageVerifier verifier)
      : service_(std::move(service)), verifier_(std::move(verifier)) {}

  const DiskService& service() const { return service_; }
  const compat::MessageVerifier& verifier() const { return verifier_; }
  std::filesystem::path path_for(const Blob& blob) const { return service_.path_for(blob.key); }

  // --- File work: no database, blocking ---

  // The file half of Blob.create_and_upload!(io:, filename:, content_type:) with identify: true.
  Result<Staged> stage_file(const std::filesystem::path& source, Filename filename,
                            std::optional<std::string_view> declared_type) const;
  Result<Staged> stage_bytes(std::string_view data, Filename filename,
                             std::optional<std::string_view> declared_type) const;
  // blob.open: a temporary file named ActiveStorage-<id>-XXXXXX<ext> with a verified checksum.
  Result<TempFile> open(const Blob& blob) const;
  // What blob.analyze saves: metadata.merge(analyzer.metadata.merge(analyzed: true)).
  Result<compat::json::Value> analyzed_metadata(const Blob& blob) const;
  // The file half of VariantWithRecord#processed for a variation that variation_for gave.
  Result<Staged> transform_variant(const Blob& blob, const Variation& variation) const;
  // The file half of Preview#process: the ffmpeg frame, staged as <base>.jpg.
  Result<Staged> draw_preview_image(const Blob& blob) const;

  // --- Record work: quick ---

  // blob.variant(transformations): the variation with the format of the blob as the default.
  Result<Variation> variation_for(const Blob& blob, const Variation& transformations) const;
  Result<std::optional<Blob>> existing_variant(Records& records, const Blob& blob, const Variation& variation) const;
  // The record half of VariantWithRecord#processed. Nullopt when another request recorded the
  // variant first: read it with existing_variant and drop `image`.
  Result<std::optional<Blob>> record_variant(Records& records, const Blob& blob, const Variation& variation,
                                             const Staged& image, compat::Timestamp now) const;
  Result<std::optional<Blob>> existing_preview_image(Records& records, const Blob& blob) const;
  Result<std::optional<Blob>> record_preview_image(Records& records, const Blob& blob, const Staged& image,
                                                   compat::Timestamp now) const;

  // --- Both parts at once, for tests and tools ---

  Result<Blob> create_and_upload(Records& records, std::string_view data, Filename filename,
                                 std::optional<std::string_view> declared_type, compat::Timestamp now) const;
  // blob.analyze: saves the metadata and updates `blob`.
  Status analyze(Records& records, Blob& blob) const;
  Result<Blob> process_variant(Records& records, const Blob& blob, const Variation& variation,
                               compat::Timestamp now) const;
  Result<Blob> preview_image(Records& records, const Blob& blob, compat::Timestamp now) const;
  // blob.preview(transformations).processed: the preview image for empty transformations,
  // else its processed variant.
  Result<Blob> process_preview(Records& records, const Blob& blob, const Variation& transformations,
                               compat::Timestamp now) const;
  // blob.representation(transformations).processed.
  Result<Blob> process_representation(Records& records, const Blob& blob, const Variation& transformations,
                                      compat::Timestamp now) const;

  // Blob#delete: the file, and the legacy variants/<key>/ files of an image. Rows are the caller's.
  Status delete_files(const Blob& blob) const;

 private:
  Result<Staged> stage_analyzed(const std::filesystem::path& path, Filename filename, std::string_view content_type) const;

  DiskService service_;
  compat::MessageVerifier verifier_;
};

}  // namespace campfire::storage
