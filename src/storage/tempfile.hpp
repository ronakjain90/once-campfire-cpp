// A temporary file that deletes itself (Rust: the tempfile crate in crates/storage).
#pragma once

#include <filesystem>
#include <string_view>

#include "core/error.hpp"

namespace campfire::storage {

class TempFile {
 public:
  TempFile() = default;
  TempFile(TempFile&& other) noexcept : path_(std::move(other.path_)) { other.path_.clear(); }
  TempFile& operator=(TempFile&& other) noexcept {
    if (this != &other) {
      remove();
      path_ = std::move(other.path_);
      other.path_.clear();
    }
    return *this;
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  ~TempFile() { remove(); }

  // Creates an empty file named <prefix>XXXXXX<suffix> in the temporary directory.
  static Result<TempFile> create(std::string_view prefix, std::string_view suffix);

  const std::filesystem::path& path() const { return path_; }

 private:
  void remove() {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove(path_, ec);
    }
  }
  std::filesystem::path path_;
};

}  // namespace campfire::storage
