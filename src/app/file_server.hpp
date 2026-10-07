// `ActiveStorage::FileServer#serve_file` on `Rack::Files#serving` (rack 3.2): conditional GET on the mtime, byte ranges
// and the 416 page. Rust: crates/storage/src/file_server.rs.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/error.hpp"

namespace campfire::app {

// What the Rack file server answers. The body is in memory: a file is read when the response is made.
struct ServedFile {
  int status = 200;
  std::vector<std::pair<std::string, std::string>> headers;  // in Rack's order
  std::string body;
};

struct FileRequest {
  std::string_view method;
  std::optional<std::string_view> range;
  std::optional<std::string_view> if_modified_since;
};

// `serve_file(path, content_type:, disposition:)`. The error is a file that cannot be read: a missing file has
// `Errc::NotFound`.
[[nodiscard]] Result<ServedFile> serve_file(const FileRequest& request, const std::filesystem::path& path,
                                            std::optional<std::string_view> content_type,
                                            std::optional<std::string_view> disposition);

// The bytes `first` to `last` (inclusive) of a file.
[[nodiscard]] Result<std::string> read_file_range(const std::filesystem::path& path, std::uint64_t first,
                                                  std::uint64_t last);

}  // namespace campfire::app
