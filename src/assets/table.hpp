// The data that the build-time generator writes (src/assets/gen/main.cpp). Runtime code reads it.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace campfire::assets {

// A range of bytes in the blob. `size` 0 means: no such body.
struct BlobRef {
  std::uint32_t offset = 0;
  std::uint32_t size = 0;
};

// One file that the server can send, with the URL path that it has.
struct FileRecord {
  std::string_view url;
  BlobRef identity;
  BlobRef gzip;
  BlobRef zstd;
};

struct ManifestRecord {
  std::string_view logical;
  std::string_view digested;
};

struct GeneratedData {
  const char* blob = nullptr;
  std::span<const FileRecord> files;              // sorted by URL path
  std::span<const ManifestRecord> manifest;       // sorted by logical path
  std::span<const std::string_view> stylesheets;  // logical paths, sorted
  BlobRef manifest_json;                          // /assets/.manifest.json
  BlobRef importmap_tags;                         // javascript_importmap_tags
  std::int64_t built_at = 0;                      // seconds since the epoch
};

// Defined in the generated file assets_data.cpp.
[[nodiscard]] const GeneratedData& generated_data() noexcept;

}  // namespace campfire::assets
