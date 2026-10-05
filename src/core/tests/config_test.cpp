// Tests of core/config.hpp. Rust: the tests in crates/campfire/src/config.rs and
// crates/kit/src/front/config.rs.
#include "core/config.hpp"

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <unistd.h>

using namespace campfire;
namespace fs = std::filesystem;

namespace {
using Vars = std::map<std::string, std::string>;
EnvLookup lookup(Vars vars) {
  return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
    const auto it = vars.find(std::string(name));
    if (it == vars.end()) {
      return std::nullopt;
    }
    return it->second;
  };
}
Result<Config> config(Vars vars) { return Config::from_lookup(lookup(std::move(vars))); }
Config ok(Vars extra = {}) {
  extra.emplace("SECRET_KEY_BASE", "abc");
  Result<Config> c = config(std::move(extra));
  REQUIRE(c.has_value());
  return *c;
}
FrontConfig front(Vars vars) { return FrontConfig::from_lookup(lookup(std::move(vars))); }
}  // namespace

TEST_CASE("Config requires a secret key base") {
  const Result<Config> missing = config({});
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().code == Errc::Config);
  CHECK(missing.error().message == "Missing `secret_key_base` for 'production' environment, set SECRET_KEY_BASE");
  CHECK_FALSE(config({{"SECRET_KEY_BASE", "  "}}).has_value());
  const Result<Config> dummy = config({{"SECRET_KEY_BASE_DUMMY", "1"}});
  REQUIRE(dummy.has_value());
  CHECK(dummy->secret_key_base.size() == 128);
  CHECK(config({{"SECRET_KEY_BASE_DUMMY", "1"}})->secret_key_base != dummy->secret_key_base);
}

TEST_CASE("Config production defaults") {
  const Config c = ok();
  CHECK_FALSE(c.disable_ssl);
  CHECK(c.app_version == "0");
  CHECK_FALSE(c.git_revision.has_value());
  CHECK(c.environment == "production");
  CHECK(c.storage.database == fs::path("storage/db/production.sqlite3"));
  CHECK(c.storage.files == fs::path("storage/files"));
  CHECK(c.storage.backups == fs::path("storage/backups"));
  CHECK(c.storage.backup_file() == fs::path("storage/backups/production.sqlite3"));
  CHECK(c.fragment_cache_bytes == 32U * 1024 * 1024);
  CHECK(c.db_readers == 5);
  CHECK(c.job_concurrency == 2);
  CHECK(c.log_level == "info");
  CHECK(c.vapid_subject == Config::kDefaultVapidSubject);
  CHECK_FALSE(c.vapid_public_key.has_value());
}

TEST_CASE("Config numbers") {
  CHECK(ok({{"CAMPFIRE_FRAGMENT_CACHE_MB", "64"}}).fragment_cache_bytes == 64U * 1024 * 1024);
  CHECK(ok({{"RAILS_MAX_THREADS", "0"}}).db_readers == 1);
  CHECK(ok({{"RAILS_MAX_THREADS", " 8 "}}).db_readers == 8);
  CHECK(ok({{"RAILS_MAX_THREADS", "+3"}}).db_readers == 3);
  CHECK(ok({{"JOB_CONCURRENCY", "0"}}).job_concurrency == 1);
  CHECK(ok({{"JOB_CONCURRENCY", ""}}).job_concurrency == 2);
  CHECK(ok({{"CAMPFIRE_FRAGMENT_CACHE_MB", "18446744073709551615"}}).fragment_cache_bytes == SIZE_MAX);
  for (const char* bad : {"lots", "-1", "1.5", "99999999999999999999999", "+", "1 2"}) {
    INFO(bad);
    const Result<Config> c = config({{"SECRET_KEY_BASE", "abc"}, {"CAMPFIRE_FRAGMENT_CACHE_MB", bad}});
    REQUIRE_FALSE(c.has_value());
    CHECK(c.error().code == Errc::Config);
    CHECK(c.error().message == std::string("CAMPFIRE_FRAGMENT_CACHE_MB=\"") + bad + "\" is not a number");
  }
  CHECK_FALSE(config({{"SECRET_KEY_BASE", "abc"}, {"RAILS_MAX_THREADS", "x"}}).has_value());
  CHECK(config({{"SECRET_KEY_BASE", "abc"}, {"JOB_CONCURRENCY", "x"}}).error().message == "JOB_CONCURRENCY=\"x\" is not a number");
}

TEST_CASE("Config version falls back to the revision") {
  const Config c = ok({{"APP_VERSION", ""}, {"GIT_REVISION", "abc123"}});
  CHECK(c.app_version == "abc123");
  CHECK(c.git_revision == "abc123");
  CHECK(ok({{"APP_VERSION", "2.0"}, {"GIT_REVISION", "abc123"}}).app_version == "2.0");
  CHECK(ok({{"GIT_REVISION", ""}}).git_revision == "");  // the Rust port keeps a blank revision
}

TEST_CASE("DISABLE_SSL is any value that is not blank") {
  CHECK(ok({{"DISABLE_SSL", "false"}}).disable_ssl);
  CHECK_FALSE(ok({{"DISABLE_SSL", " "}}).disable_ssl);
}

TEST_CASE("VAPID settings") {
  const Config blank = ok({{"VAPID_PUBLIC_KEY", ""}, {"VAPID_PRIVATE_KEY", " "}});
  CHECK_FALSE(blank.vapid_public_key.has_value());
  CHECK_FALSE(blank.vapid_private_key.has_value());
  CHECK(ok({{"VAPID_PUBLIC_KEY", "pub"}}).vapid_public_key == "pub");
  CHECK(ok({{"VAPID_SUBJECT", "mailto:ops@example.com"}, {"TLS_DOMAIN", "chat.example.com"}}).vapid_subject ==
        "mailto:ops@example.com");
  CHECK(ok({{"TLS_DOMAIN", " , chat.example.com,other.example.com"}}).vapid_subject == "https://chat.example.com");
  CHECK(ok({{"VAPID_SUBJECT", " "}}).vapid_subject == Config::kDefaultVapidSubject);
}

TEST_CASE("Storage overrides") {
  const Config c = ok({{"CAMPFIRE_STORAGE_PATH", "/rails/storage"}, {"CAMPFIRE_FILES_PATH", "/seed/storage"}, {"RAILS_ENV", "test"}});
  CHECK(c.environment == "test");
  CHECK(c.storage.database == fs::path("/rails/storage/db/test.sqlite3"));
  CHECK(c.storage.files == fs::path("/seed/storage"));
  CHECK(c.storage.backups == fs::path("/rails/storage/backups"));
  const Config d = ok({{"CAMPFIRE_DATABASE_PATH", "/d/x.db"}, {"CAMPFIRE_BACKUPS_PATH", "/b"}});
  CHECK(d.storage.database == fs::path("/d/x.db"));
  CHECK(d.storage.backup_file() == fs::path("/b/x.db"));
}

TEST_CASE("StoragePaths::create_dirs") {
  const fs::path root = fs::temp_directory_path() / ("campfire_cfg_test_" + std::to_string(::getpid()));
  const StoragePaths paths = StoragePaths::under(root, "production");
  REQUIRE(paths.create_dirs().has_value());
  CHECK(fs::is_directory(root / "db"));
  CHECK(fs::is_directory(root / "files"));
  fs::remove_all(root);
  // A file in the way gives an error.
  fs::create_directories(root);
  { std::ofstream(root / "db") << "x"; }
  const Status s = paths.create_dirs();
  CHECK_FALSE(s.has_value());
  CHECK(s.error().code == Errc::Io);
  fs::remove_all(root);
}

TEST_CASE("FrontConfig defaults") {
  const FrontConfig c = front({});
  CHECK(c.http_port == 80);
  CHECK(c.https_port == 443);
  CHECK(c.target_port == 3000);
  CHECK(c.target_bind == "127.0.0.1");
  CHECK(c.cache_size == 64 * 1024 * 1024);
  CHECK(c.max_cache_item_size == 1024 * 1024);
  CHECK(c.http_idle_timeout_s == 60);
  CHECK(c.http_read_timeout_s == 30);
  CHECK(c.http_write_timeout_s == 30);
  CHECK(c.storage_path == fs::path("./storage/thruster"));
  CHECK(c.acme_directory_url == FrontConfig::kLetsEncryptUrl);
  CHECK(c.gzip_compression_jitter == 32);
  CHECK(c.gzip_compression_enabled);
  CHECK_FALSE(c.gzip_compression_disable_on_auth);
  CHECK_FALSE(c.h2c_enabled);
  CHECK_FALSE(c.has_tls());
  CHECK(c.forward_headers);
  CHECK(c.log_requests);
  CHECK_FALSE(c.debug);
  CHECK(c.max_request_body == 0);
}

TEST_CASE("FrontConfig prefixed variables win") {
  const FrontConfig c = front({{"HTTP_PORT", "8080"}, {"THRUSTER_HTTP_PORT", "9090"}, {"HTTPS_PORT", "8443"}});
  CHECK(c.http_port == 9090);
  CHECK(c.https_port == 8443);
}

TEST_CASE("FrontConfig TARGET_BIND") {
  CHECK(front({{"TARGET_BIND", "0.0.0.0"}}).target_bind == "0.0.0.0");
  CHECK(front({{"TARGET_BIND", "::"}}).target_bind == "::");
  CHECK(front({{"TARGET_BIND", "everywhere"}}).target_bind == "127.0.0.1");
}

TEST_CASE("FrontConfig values that do not parse give the default") {
  const FrontConfig c = front({{"HTTP_PORT", "eighty"}, {"HTTP_READ_TIMEOUT", "5s"}, {"LOG_REQUESTS", "yes"}, {"H2C_ENABLED", "1"},
                               {"HTTPS_PORT", "70000"}, {"HTTP_IDLE_TIMEOUT", "-5"}});
  CHECK(c.http_port == 80);
  CHECK(c.https_port == 443);
  CHECK(c.http_read_timeout_s == 30);
  CHECK(c.http_idle_timeout_s == 0);  // Go clamps a negative duration the same way in the Rust port
  CHECK(c.log_requests);
  CHECK(c.h2c_enabled);
  CHECK(front({{"THRUSTER_HTTP_PORT", ""}, {"HTTP_PORT", "81"}}).http_port == 80);  // set but blank wins
}

TEST_CASE("FrontConfig TLS domains turn off forwarded headers") {
  const FrontConfig c = front({{"TLS_DOMAIN", " chat.example.com, ,other.example.com "}});
  CHECK(c.tls_domains == std::vector<std::string>{"chat.example.com", "other.example.com"});
  CHECK(c.has_tls());
  CHECK_FALSE(c.forward_headers);
  CHECK(front({{"TLS_DOMAIN", "a.example.com"}, {"FORWARD_HEADERS", "true"}}).forward_headers);
  CHECK_FALSE(front({{"SSL_DOMAIN", "a.example.com"}}).has_tls());
  CHECK_FALSE(front({{"TLS_DOMAIN", " , "}}).has_tls());
}
