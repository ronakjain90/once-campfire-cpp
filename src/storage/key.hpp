// Blob keys and checksums (Rails: ActiveStorage::Blob has_secure_token, compute_checksum_in_chunks;
// Rust: crates/storage/src/key.rs).
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "core/error.hpp"

namespace campfire::storage {

// ActiveStorage::Blob::MINIMUM_TOKEN_LENGTH.
inline constexpr size_t kKeyLength = 28;

// SecureRandom.base36(28).
std::string generate_key();

// OpenSSL::Digest::MD5.base64digest of the content.
std::string checksum(std::string_view data);

// Streaming checksum of a file.
Result<std::string> checksum_file(const std::filesystem::path& path);

// Incremental checksum, for a copy that hashes while it writes.
class ChecksumBuilder {
 public:
  ChecksumBuilder();
  ~ChecksumBuilder();
  ChecksumBuilder(const ChecksumBuilder&) = delete;
  ChecksumBuilder& operator=(const ChecksumBuilder&) = delete;

  void update(std::string_view data);
  // Base64 of the digest. Call it once.
  std::string finish();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace campfire::storage
