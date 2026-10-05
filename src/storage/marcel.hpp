// Marcel 1.1.0 content type identification, Marcel::MimeType.for (Rust: crates/storage/src/marcel.rs).
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace campfire::storage::marcel {

inline constexpr std::string_view kBinary = "application/octet-stream";

// Marcel::MimeType.for(io, name:, declared_type:), as ActiveStorage::Blob#extract_content_type
// calls it on upload.
std::string identify(std::string_view data, std::optional<std::string_view> name,
                     std::optional<std::string_view> declared_type);
// Marcel::MimeType.for(extension:).
std::string for_extension(std::string_view extension);
// Marcel::Magic.by_extension: case-insensitive, with or without the leading dot.
std::optional<std::string_view> by_extension(std::string_view extension);
// Marcel::Magic.by_path: the extension per Ruby's File.extname.
std::optional<std::string_view> by_path(std::string_view path);
// Marcel::Magic.new(type).extensions.
std::span<const std::string_view> extensions(std::string_view content_type);
// Marcel::Magic.child?.
bool is_child(std::string_view child, std::string_view parent);
// How many leading bytes by_magic can look at.
size_t magic_prefix_len();
// Marcel::Magic.by_magic: the first table entry whose matches hit.
std::optional<std::string> by_magic(std::string_view data);

}  // namespace campfire::storage::marcel
