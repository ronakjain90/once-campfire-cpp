// Content-Disposition of a blob and Journey path escaping for the *filename glob of Active
// Storage routes (Rails: ActiveStorage::Service#content_disposition_with; Rust: disposition.rs).
#pragma once

#include <string>
#include <string_view>

namespace campfire::storage {

// Anything but "attachment" is "inline".
std::string content_disposition_with(std::string_view disposition, std::string_view sanitized_filename);
// Journey::Router::Utils.escape_path: keeps unreserved, sub-delims, ":", "@" and "/".
std::string escape_path(std::string_view s);
// Journey::Router::Utils.escape_segment: like escape_path, but "/" is escaped too.
std::string escape_segment(std::string_view s);

}  // namespace campfire::storage
