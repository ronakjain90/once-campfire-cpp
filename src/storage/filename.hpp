// ActiveStorage::Filename, with Ruby's File.extname and File.basename rules
// (Rust: crates/storage/src/filename.rs).
#pragma once

#include <string>
#include <string_view>

namespace campfire::storage {

class Filename {
 public:
  Filename() = default;
  explicit Filename(std::string raw) : raw_(std::move(raw)) {}

  // Bytes as they arrived. Invalid UTF-8 becomes U+FFFD, as String#encode does in `sanitized`.
  static Filename from_bytes(std::string_view bytes);

  // The stored value, as written to active_storage_blobs.filename.
  const std::string& raw() const { return raw_; }
  // File.basename(filename, extension_with_delimiter).
  std::string_view base() const;
  std::string_view extension_with_delimiter() const;
  // extension_without_delimiter.
  std::string_view extension() const;
  // strip, then "-" for the RTL override, path separators and shell or HTML metacharacters.
  std::string sanitized() const;

  friend bool operator==(const Filename&, const Filename&) = default;

 private:
  std::string raw_;
};

// File.basename(path): the last component, ignoring trailing slashes.
std::string_view basename(std::string_view path);
// File.extname(path) on Unix.
std::string_view extname(std::string_view path);

// Replaces each invalid UTF-8 sequence with U+FFFD (the maximal-subpart rule of Rust's
// String::from_utf8_lossy).
std::string utf8_lossy(std::string_view bytes);

}  // namespace campfire::storage
