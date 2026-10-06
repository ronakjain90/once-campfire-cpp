// ActiveStorage::Service::DiskService: files at <root>/ab/cd/<key>, signed disk URLs and upload
// tokens (Rails: activestorage/lib/active_storage/service/disk_service.rb; Rust: crates/storage/src/disk.rs).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "compat/message_verifier.hpp"
#include "compat/time.hpp"
#include "core/error.hpp"
#include "storage/filename.hpp"

namespace campfire::storage {

// The payload of the encoded_key of a disk URL (purpose "blob_key").
struct DiskKey {
  std::string key;
  std::string disposition;
  std::optional<std::string> content_type;
  std::string service_name;
};

// The payload of the encoded_token of a direct upload (purpose "blob_token").
struct DiskToken {
  std::string key;
  std::optional<std::string> content_type;
  int64_t content_length = 0;
  std::string checksum;
  std::string service_name;
};

class DiskService {
 public:
  // Campfire's "local" service: root storage/files (config/storage.yml).
  DiskService(std::filesystem::path root, std::string name) : root_(std::move(root)), name_(std::move(name)) {}

  const std::string& name() const { return name_; }
  const std::filesystem::path& root() const { return root_; }

  // <root>/<key[0,2]>/<key[2,2]>/<key>.
  std::filesystem::path path_for(std::string_view key) const;

  // upload(key, io, checksum:): copy the file at `source`, then check the MD5 and delete the
  // copy when it differs (Errc::Internal "checksum mismatch").
  Status upload_file(std::string_view key, const std::filesystem::path& source,
                     std::optional<std::string_view> checksum) const;
  // upload for bytes in memory.
  Status upload(std::string_view key, std::string_view data, std::optional<std::string_view> checksum) const;
  Result<std::string> download(std::string_view key) const;
  // Succeeds when the file is not there.
  Status remove(std::string_view key) const;
  // delete_prefixed: removes each entry whose path starts with path_for(prefix).
  Status remove_prefixed(std::string_view prefix) const;
  bool exist(std::string_view key) const;

  // The path of service.url(key, expires_in:, filename:, content_type:, disposition:). The caller
  // adds the protocol and host.
  std::string url_path(const compat::MessageVerifier& verifier, std::string_view key,
                       std::optional<compat::Timestamp> expires_at, const Filename& filename,
                       std::optional<std::string_view> content_type, std::string_view disposition) const;
  // The path of url_for_direct_upload.
  std::string url_path_for_direct_upload(const compat::MessageVerifier& verifier, std::string_view key,
                                         compat::Timestamp expires_at, std::optional<std::string_view> content_type,
                                         int64_t content_length, std::string_view checksum) const;

 private:
  Result<std::filesystem::path> make_path_for(std::string_view key) const;
  Status ensure_integrity_of(std::string_view key, std::string_view checksum) const;

  std::filesystem::path root_;
  std::string name_;
};

// DiskController#decode_verified_key. Nullopt for a bad or expired value.
std::optional<DiskKey> decode_verified_key(const compat::MessageVerifier& verifier, std::string_view encoded_key,
                                           compat::Timestamp now);
// DiskController#decode_verified_token.
std::optional<DiskToken> decode_verified_token(const compat::MessageVerifier& verifier,
                                               std::string_view encoded_token, compat::Timestamp now);

}  // namespace campfire::storage
