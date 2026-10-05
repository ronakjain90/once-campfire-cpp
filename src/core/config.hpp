// Environment configuration. Rust: crates/campfire/src/config.rs (Config) and
// crates/kit/src/front/config.rs (FrontConfig). Rails: config/environments/production.rb, config/puma.rb.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.hpp"

namespace campfire {

// A lookup of an environment variable. Returns nothing if the variable is not set.
using EnvLookup = std::function<std::optional<std::string>(std::string_view)>;

// The lookup of the real environment.
[[nodiscard]] EnvLookup process_env();

// Storage paths. They mirror Rails.root.join("storage"): the database under db/, blobs under
// files/ (config/storage.yml), backups under backups/ (script/admin/prepare-backup).
struct StoragePaths {
  std::filesystem::path database;  // storage/db/<env>.sqlite3
  std::filesystem::path files;     // storage/files
  std::filesystem::path backups;   // storage/backups

  [[nodiscard]] static StoragePaths under(const std::filesystem::path& root, std::string_view environment);
  // storage/{db,files} exist after boot (config/initializers/storage_paths.rb).
  [[nodiscard]] Status create_dirs() const;
  // Where prepare-backup writes the snapshot: backups/<file name of the database>.
  [[nodiscard]] std::filesystem::path backup_file() const;
};

// The settings of the app. All variables here, and their defaults:
//   SECRET_KEY_BASE            required (SECRET_KEY_BASE_DUMMY makes a random one)
//   VAPID_PUBLIC_KEY, VAPID_PRIVATE_KEY   none
//   VAPID_SUBJECT              "https://<first TLS_DOMAIN>", or the project URL
//   DISABLE_SSL                false (any value that is not blank turns it on, even "false")
//   APP_VERSION, GIT_REVISION  version: APP_VERSION, else GIT_REVISION, else "0"
//   RAILS_ENV                  "production"
//   CAMPFIRE_STORAGE_PATH      "storage"
//   CAMPFIRE_DATABASE_PATH, CAMPFIRE_FILES_PATH, CAMPFIRE_BACKUPS_PATH   from the storage path
//   RAILS_MAX_THREADS          5 (at least 1) -> db_readers
//   JOB_CONCURRENCY            2 (at least 1)
//   RAILS_LOG_LEVEL            "info"
//   CAMPFIRE_FRAGMENT_CACHE_MB 32
// A variable that is empty or has only spaces counts as not set (except GIT_REVISION, which the
// Rust port keeps as it is for `git_revision`).
struct Config {
  std::string secret_key_base;
  std::optional<std::string> vapid_public_key;
  std::optional<std::string> vapid_private_key;
  std::string vapid_subject;
  bool disable_ssl = false;
  std::string app_version;
  std::optional<std::string> git_revision;
  std::string environment;
  StoragePaths storage;
  std::size_t db_readers = 5;
  std::size_t job_concurrency = 2;
  std::string log_level;
  std::size_t fragment_cache_bytes = 32U << 20;

  static constexpr std::string_view kDefaultVapidSubject = "https://github.com/basecamp/once-campfire-rust";

  // Fails with `Errc::Config` if SECRET_KEY_BASE is missing or a number is not a number.
  [[nodiscard]] static Result<Config> from_lookup(const EnvLookup& get);
  [[nodiscard]] static Result<Config> from_env() { return from_lookup(process_env()); }
};

// Thruster's settings, which the front server of this process reads. Each setting is read from
// THRUSTER_<NAME>, then <NAME>. A value that does not parse gives the default (as Go does).
//   TARGET_PORT 3000, TARGET_BIND 127.0.0.1, CACHE_SIZE 64 MiB, MAX_CACHE_ITEM_SIZE 1 MiB,
//   GZIP_COMPRESSION_ENABLED true, GZIP_COMPRESSION_DISABLE_ON_AUTH false,
//   GZIP_COMPRESSION_JITTER 32, MAX_REQUEST_BODY 0, TLS_DOMAIN (comma list), ACME_DIRECTORY,
//   EAB_KID, EAB_HMAC_KEY, STORAGE_PATH ./storage/thruster, HTTP_PORT 80, HTTPS_PORT 443,
//   HTTP_IDLE_TIMEOUT 60, HTTP_READ_TIMEOUT 30, HTTP_WRITE_TIMEOUT 30 (seconds),
//   H2C_ENABLED false, FORWARD_HEADERS (true unless TLS), DEBUG false, LOG_REQUESTS true.
struct FrontConfig {
  static constexpr std::string_view kLetsEncryptUrl = "https://acme-v02.api.letsencrypt.org/directory";

  std::uint16_t target_port = 3000;
  std::string target_bind = "127.0.0.1";
  std::int64_t cache_size = 64 << 20;
  std::int64_t max_cache_item_size = 1 << 20;
  bool gzip_compression_enabled = true;
  bool gzip_compression_disable_on_auth = false;
  std::int64_t gzip_compression_jitter = 32;
  std::int64_t max_request_body = 0;  // 0 means no limit
  std::vector<std::string> tls_domains;
  std::string acme_directory_url{kLetsEncryptUrl};
  std::string eab_kid;
  std::string eab_hmac_key;
  std::filesystem::path storage_path = "./storage/thruster";
  std::uint16_t http_port = 80;
  std::uint16_t https_port = 443;
  std::int64_t http_idle_timeout_s = 60;
  std::int64_t http_read_timeout_s = 30;
  std::int64_t http_write_timeout_s = 30;
  bool h2c_enabled = false;
  bool forward_headers = true;
  bool debug = false;
  bool log_requests = true;

  [[nodiscard]] static FrontConfig from_lookup(const EnvLookup& get);
  [[nodiscard]] static FrontConfig from_env() { return from_lookup(process_env()); }
  [[nodiscard]] bool has_tls() const noexcept { return !tls_domains.empty(); }
};

}  // namespace campfire
