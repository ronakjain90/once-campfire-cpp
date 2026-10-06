// See storage.hpp.
#include "storage/storage.hpp"

#include <fstream>

#include "storage/analyze.hpp"
#include "storage/errors.hpp"
#include "storage/key.hpp"
#include "storage/process.hpp"

namespace campfire::storage {

namespace {

// metadata.merge(extracted.merge(analyzed: true))
compat::json::Value analyzed(const compat::json::Value& metadata, compat::json::Value extracted) {
  extracted.set("analyzed", compat::json::Value(true));
  compat::json::Value merged = metadata;
  for (const auto& [key, value] : extracted.as_object()) merged.set(key, value);
  return merged;
}

Result<TempFile> temp_with(std::string_view content, std::string_view suffix) {
  auto file = TempFile::create("ActiveStorage-", suffix);
  if (!file) return std::unexpected(file.error());
  std::ofstream out(file->path(), std::ios::binary | std::ios::trunc);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  out.flush();
  if (!out) return io_error("write failed: " + file->path().string());
  return file;
}

}  // namespace

// The Staged comes first, so a failed copy deletes what it wrote. Unlike DiskService#upload, the
// copy is not read back: the checksum came from the same local bytes, and open() checks the
// file before analysis, variants and posters.
Result<Staged> Storage::stage_file(const std::filesystem::path& source, Filename filename,
                                   std::optional<std::string_view> declared_type) const {
  auto blob = NewBlob::unfurl_file(source, std::move(filename), declared_type, service_.name(), true);
  if (!blob) return std::unexpected(blob.error());
  Staged staged(std::move(*blob), service_);
  if (auto s = service_.upload_file(staged.blob().key, source, std::nullopt); !s) return std::unexpected(s.error());
  return staged;
}

Result<Staged> Storage::stage_bytes(std::string_view data, Filename filename,
                                    std::optional<std::string_view> declared_type) const {
  Staged staged(NewBlob::unfurl(data, std::move(filename), declared_type, service_.name(), true), service_);
  if (auto s = service_.upload(staged.blob().key, data, std::nullopt); !s) return std::unexpected(s.error());
  return staged;
}

Result<TempFile> Storage::open(const Blob& blob) const {
  auto file = TempFile::create("ActiveStorage-" + std::to_string(blob.id) + "-",
                               std::string(blob.filename.extension_with_delimiter()));
  if (!file) return std::unexpected(file.error());
  std::error_code ec;
  std::filesystem::copy_file(path_for(blob), file->path(), std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    if (ec == std::errc::no_such_file_or_directory) return file_not_found();
    return io_error("copy failed: " + ec.message());
  }
  if (blob.checksum) {
    auto sum = checksum_file(file->path());
    if (!sum) return std::unexpected(sum.error());
    if (*sum != *blob.checksum) return integrity_error();
  }
  return file;
}

Result<compat::json::Value> Storage::analyzed_metadata(const Blob& blob) const {
  Analyzer analyzer = analyzer_for(blob.type());
  compat::json::Value extracted{compat::json::Value::Object{}};
  if (analyzer != Analyzer::Null) {
    auto file = open(blob);
    if (!file) return std::unexpected(file.error());
    auto metadata = analyze_metadata(analyzer, file->path());
    if (!metadata) return std::unexpected(metadata.error());
    extracted = std::move(*metadata);
  }
  return analyzed(blob.metadata, std::move(extracted));
}

Result<Staged> Storage::stage_analyzed(const std::filesystem::path& path, Filename filename,
                                       std::string_view content_type) const {
  auto staged = stage_file(path, std::move(filename), content_type);
  if (!staged) return staged;
  Analyzer analyzer = analyzer_for(staged->blob().content_type.value_or(""));
  auto extracted = analyze_metadata(analyzer, path);
  if (!extracted) return std::unexpected(extracted.error());
  staged->blob().metadata = analyzed(staged->blob().metadata, std::move(*extracted));
  return staged;
}

Result<Staged> Storage::transform_variant(const Blob& blob, const Variation& variation) const {
  auto format = variation_format(variation);
  if (!format) return std::unexpected(format.error());
  auto type = variation_content_type(variation);
  if (!type) return std::unexpected(type.error());
  auto output = [&]() -> Result<TempFile> {
    auto input = open(blob);
    if (!input) return std::unexpected(input.error());
    return transform(input->path(), variation);
  }();
  if (!output) return std::unexpected(output.error());
  std::string lower = *format;
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return stage_analyzed(output->path(), Filename(std::string(blob.filename.base()) + "." + lower), *type);
}

Result<Staged> Storage::draw_preview_image(const Blob& blob) const {
  if (!blob.is_previewable()) return prefixed(Errc::InvalidArgument, kUnpreviewable, blob.type());
  auto frame = [&]() -> Result<std::string> {
    auto input = open(blob);
    if (!input) return std::unexpected(input.error());
    return video_preview(input->path());
  }();
  if (!frame) return std::unexpected(frame.error());
  auto output = temp_with(*frame, ".jpg");
  if (!output) return std::unexpected(output.error());
  return stage_analyzed(output->path(), Filename(std::string(blob.filename.base()) + ".jpg"), "image/jpeg");
}

Result<Variation> Storage::variation_for(const Blob& blob, const Variation& transformations) const {
  if (!blob.is_variable()) return prefixed(Errc::InvalidArgument, kInvariable, blob.type());
  return default_to(transformations,
                    {{"format", compat::marshal::Value::string(blob.default_variant_format())}});
}

Result<std::optional<Blob>> Storage::existing_variant(Records& records, const Blob& blob,
                                                      const Variation& variation) const {
  auto record = records.find_variant_record(blob.id, variation.digest());
  if (!record) return std::unexpected(record.error());
  if (!*record) return std::optional<Blob>();
  return records.attached("ActiveStorage::VariantRecord", **record, "image");
}

Result<std::optional<Blob>> Storage::record_variant(Records& records, const Blob& blob, const Variation& variation,
                                                    const Staged& image, compat::Timestamp now) const {
  auto record = records.insert_variant_record(blob.id, variation.digest());
  if (!record) return std::unexpected(record.error());
  if (!*record) return std::optional<Blob>();
  auto saved = image.insert(records, now);
  if (!saved) return std::unexpected(saved.error());
  auto attachment = records.insert_attachment("image", "ActiveStorage::VariantRecord", **record, saved->id, now);
  if (!attachment) return std::unexpected(attachment.error());
  return std::optional<Blob>(std::move(*saved));
}

Result<std::optional<Blob>> Storage::existing_preview_image(Records& records, const Blob& blob) const {
  return records.attached("ActiveStorage::Blob", blob.id, "preview_image");
}

Result<std::optional<Blob>> Storage::record_preview_image(Records& records, const Blob& blob, const Staged& image,
                                                          compat::Timestamp now) const {
  auto existing = existing_preview_image(records, blob);
  if (!existing) return std::unexpected(existing.error());
  if (*existing) return std::optional<Blob>();
  auto saved = image.insert(records, now);
  if (!saved) return std::unexpected(saved.error());
  auto attachment = records.insert_attachment("preview_image", "ActiveStorage::Blob", blob.id, saved->id, now);
  if (!attachment) return std::unexpected(attachment.error());
  return std::optional<Blob>(std::move(*saved));
}

Result<Blob> Storage::create_and_upload(Records& records, std::string_view data, Filename filename,
                                        std::optional<std::string_view> declared_type, compat::Timestamp now) const {
  auto staged = stage_bytes(data, std::move(filename), declared_type);
  if (!staged) return std::unexpected(staged.error());
  auto blob = staged->insert(records, now);
  if (!blob) return std::unexpected(blob.error());
  staged->keep();
  return blob;
}

Status Storage::analyze(Records& records, Blob& blob) const {
  auto metadata = analyzed_metadata(blob);
  if (!metadata) return std::unexpected(metadata.error());
  if (auto s = records.update_metadata(blob.id, *metadata); !s) return s;
  blob.metadata = std::move(*metadata);
  return {};
}

Result<Blob> Storage::process_variant(Records& records, const Blob& blob, const Variation& variation,
                                      compat::Timestamp now) const {
  auto existing = existing_variant(records, blob, variation);
  if (!existing) return std::unexpected(existing.error());
  if (*existing) return std::move(**existing);
  auto image = transform_variant(blob, variation);
  if (!image) return std::unexpected(image.error());
  auto recorded = record_variant(records, blob, variation, *image, now);
  if (!recorded) return std::unexpected(recorded.error());
  if (*recorded) {
    image->keep();
    return std::move(**recorded);
  }
  auto winner = existing_variant(records, blob, variation);
  if (!winner) return std::unexpected(winner.error());
  if (!*winner) return file_not_found();
  return std::move(**winner);
}

Result<Blob> Storage::preview_image(Records& records, const Blob& blob, compat::Timestamp now) const {
  auto existing = existing_preview_image(records, blob);
  if (!existing) return std::unexpected(existing.error());
  if (*existing) return std::move(**existing);
  auto image = draw_preview_image(blob);
  if (!image) return std::unexpected(image.error());
  auto recorded = record_preview_image(records, blob, *image, now);
  if (!recorded) return std::unexpected(recorded.error());
  if (*recorded) {
    image->keep();
    return std::move(**recorded);
  }
  auto winner = existing_preview_image(records, blob);
  if (!winner) return std::unexpected(winner.error());
  if (!*winner) return file_not_found();
  return std::move(**winner);
}

Result<Blob> Storage::process_preview(Records& records, const Blob& blob, const Variation& transformations,
                                      compat::Timestamp now) const {
  auto image = preview_image(records, blob, now);
  if (!image) return image;
  if (transformations.transformations().empty()) return image;
  auto variation = variation_for(*image, transformations);
  if (!variation) return std::unexpected(variation.error());
  return process_variant(records, *image, *variation, now);
}

Result<Blob> Storage::process_representation(Records& records, const Blob& blob, const Variation& transformations,
                                             compat::Timestamp now) const {
  if (blob.is_previewable()) return process_preview(records, blob, transformations, now);
  if (blob.is_variable()) {
    auto variation = variation_for(blob, transformations);
    if (!variation) return std::unexpected(variation.error());
    return process_variant(records, blob, *variation, now);
  }
  return prefixed(Errc::InvalidArgument, kUnrepresentable, blob.type());
}

Status Storage::delete_files(const Blob& blob) const {
  if (auto s = service_.remove(blob.key); !s) return s;
  if (blob.is_image()) return service_.remove_prefixed("variants/" + blob.key + "/");
  return {};
}

}  // namespace campfire::storage
