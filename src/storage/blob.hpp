// Plain structs for active_storage_blobs rows, plus the content rules of ActiveStorage::Blob
// (Rust: crates/storage/src/blob.rs). SQL is not here: the `Records` interface in records.hpp
// is what src/db or src/models implements.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "compat/json.hpp"
#include "core/error.hpp"
#include "storage/filename.hpp"

namespace campfire::storage {

namespace json = compat::json;

// A row of active_storage_blobs.
struct Blob {
  int64_t id = 0;
  std::string key;
  Filename filename;
  std::optional<std::string> content_type;
  // The metadata column: an ordered JSON object (store :metadata, coder: JSON).
  json::Value metadata = json::Value(json::Value::Object{});
  std::string service_name;
  int64_t byte_size = 0;
  std::optional<std::string> checksum;
  std::string created_at;

  std::string_view type() const { return content_type ? std::string_view(*content_type) : std::string_view(); }
  bool is_image() const { return type().starts_with("image"); }
  bool is_video() const { return type().starts_with("video"); }
  bool is_audio() const { return type().starts_with("audio"); }
  // variable?
  bool is_variable() const;
  // previewable?: only the video previewer can accept in Campfire's image (no poppler or mupdf).
  bool is_previewable() const;
  // representable?
  bool is_representable() const { return is_variable() || is_previewable(); }
  bool is_analyzed() const;
  // metadata[:width] or metadata[:height], as the views read them.
  std::optional<double> dimension(std::string_view name) const;
  // default_variant_format: web images keep their format, everything else becomes PNG.
  std::string default_variant_format() const;

  friend bool operator==(const Blob&, const Blob&) = default;
};

// A blob built from uploaded bytes, not saved yet (Blob.build_after_unfurling).
struct NewBlob {
  std::string key;
  Filename filename;
  std::optional<std::string> content_type;
  json::Value metadata = json::Value(json::Value::Object{});
  std::string service_name;
  int64_t byte_size = 0;
  std::string checksum;

  // build_after_unfurling(io:, filename:, content_type:, identify:): makes the key, computes the
  // checksum, identifies the content type with Marcel (unless a declared type comes with
  // `identify: false`) and marks the blob `identified`.
  static NewBlob unfurl(std::string_view data, Filename filename, std::optional<std::string_view> declared_type,
                        std::string_view service_name, bool identify);
  // unfurl for a file: reads only the bytes that identification needs, and streams the file
  // through the checksum.
  static Result<NewBlob> unfurl_file(const std::filesystem::path& path, Filename filename,
                                     std::optional<std::string_view> declared_type, std::string_view service_name,
                                     bool identify);
};

// The row-level facts of a blob that the views read (Representable#format).
std::optional<std::string> blob_format(const Blob& blob);

}  // namespace campfire::storage
