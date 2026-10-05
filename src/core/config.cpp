// Matches crates/campfire/src/config.rs and crates/kit/src/front/config.rs of the Rust port.
#include "core/config.hpp"

#include <arpa/inet.h>
#include <sys/random.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <format>
#include <limits>
#include <system_error>

namespace campfire {
namespace {

constexpr std::string_view kSpace = " \t\n\v\f\r";

bool blank(std::string_view s) {
  return s.find_first_not_of(kSpace) == std::string_view::npos;
}

std::string_view trim(std::string_view s) {
  const std::size_t begin = s.find_first_not_of(kSpace);
  if (begin == std::string_view::npos) {
    return {};
  }
  return s.substr(begin, s.find_last_not_of(kSpace) + 1 - begin);
}

// Rust `{:?}` of a string: quoted, with escapes.
std::string debug_quote(std::string_view s) {
  std::string out = "\"";
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\0': out += "\\0"; break;
      default: out += c;
    }
  }
  return out + '"';
}

// Rust `str::parse::<integer>`: an optional `+` (and `-` for signed types), then digits.
template <class T>
std::optional<T> parse_integer(std::string_view s) {
  if (s.empty()) {
    return std::nullopt;
  }
  std::string_view digits = s;
  if (digits.front() == '+') {
    digits.remove_prefix(1);
  }
  if (digits.empty() || !(digits.front() == '-' || (digits.front() >= '0' && digits.front() <= '9'))) {
    return std::nullopt;
  }
  T value{};
  const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
  if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
    return std::nullopt;
  }
  return value;
}

// Go `strconv.ParseBool`.
std::optional<bool> parse_go_bool(std::string_view v) {
  if (v == "1" || v == "t" || v == "T" || v == "TRUE" || v == "true" || v == "True") {
    return true;
  }
  if (v == "0" || v == "f" || v == "F" || v == "FALSE" || v == "false" || v == "False") {
    return false;
  }
  return std::nullopt;
}

std::string random_hex_secret() {
  std::array<unsigned char, 64> bytes{};
  if (getentropy(bytes.data(), bytes.size()) != 0) {
    std::abort();  // no random bytes: the process cannot make a safe secret
  }
  std::string out;
  for (const unsigned char b : bytes) {
    out += std::format("{:02x}", b);
  }
  return out;
}

}  // namespace

EnvLookup process_env() {
  return [](std::string_view name) -> std::optional<std::string> {
    const char* value = std::getenv(std::string(name).c_str());
    if (value == nullptr) {
      return std::nullopt;
    }
    return std::string(value);
  };
}

StoragePaths StoragePaths::under(const std::filesystem::path& root, std::string_view environment) {
  return {root / "db" / (std::string(environment) + ".sqlite3"), root / "files", root / "backups"};
}

Status StoragePaths::create_dirs() const {
  std::error_code ec;
  if (database.has_parent_path()) {
    std::filesystem::create_directories(database.parent_path(), ec);
    if (ec) {
      return fail(Errc::Io, "cannot create " + database.parent_path().string() + ": " + ec.message());
    }
  }
  std::filesystem::create_directories(files, ec);
  if (ec) {
    return fail(Errc::Io, "cannot create " + files.string() + ": " + ec.message());
  }
  return {};
}

std::filesystem::path StoragePaths::backup_file() const {
  const std::filesystem::path name = database.filename();
  return backups / (name.empty() ? std::filesystem::path("production.sqlite3") : name);
}

Result<Config> Config::from_lookup(const EnvLookup& get) {
  const auto present = [&](std::string_view name) -> std::optional<std::string> {
    std::optional<std::string> value = get(name);
    if (value && blank(*value)) {
      return std::nullopt;
    }
    return value;
  };

  Config config;
  if (auto secret = present("SECRET_KEY_BASE")) {
    config.secret_key_base = std::move(*secret);
  } else if (present("SECRET_KEY_BASE_DUMMY")) {
    config.secret_key_base = random_hex_secret();
  } else {
    return fail(Errc::Config, "Missing `secret_key_base` for 'production' environment, set SECRET_KEY_BASE");
  }
  config.environment = present("RAILS_ENV").value_or("production");

  const std::filesystem::path root = present("CAMPFIRE_STORAGE_PATH").value_or("storage");
  config.storage = StoragePaths::under(root, config.environment);
  if (auto path = present("CAMPFIRE_DATABASE_PATH")) {
    config.storage.database = *path;
  }
  if (auto path = present("CAMPFIRE_FILES_PATH")) {
    config.storage.files = *path;
  }
  if (auto path = present("CAMPFIRE_BACKUPS_PATH")) {
    config.storage.backups = *path;
  }

  std::optional<Error> error;
  const auto number = [&](std::string_view name, std::size_t fallback) -> std::size_t {
    const std::optional<std::string> value = present(name);
    if (!value) {
      return fallback;
    }
    const std::optional<std::size_t> parsed = parse_integer<std::size_t>(trim(*value));
    if (!parsed) {
      if (!error) {
        error = Error{Errc::Config, std::string(name) + "=" + debug_quote(*value) + " is not a number"};
      }
      return fallback;
    }
    return *parsed;
  };

  config.vapid_public_key = present("VAPID_PUBLIC_KEY");
  config.vapid_private_key = present("VAPID_PRIVATE_KEY");
  if (auto subject = present("VAPID_SUBJECT")) {
    config.vapid_subject = std::move(*subject);
  } else {
    config.vapid_subject = std::string(kDefaultVapidSubject);
    if (const auto domains = present("TLS_DOMAIN")) {
      std::string_view rest = *domains;
      while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        const std::string_view domain = trim(rest.substr(0, comma));
        if (!domain.empty()) {
          config.vapid_subject = "https://" + std::string(domain);
          break;
        }
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
      }
    }
  }
  config.disable_ssl = present("DISABLE_SSL").has_value();
  if (auto version = present("APP_VERSION")) {
    config.app_version = std::move(*version);
  } else if (auto revision = present("GIT_REVISION")) {
    config.app_version = std::move(*revision);
  } else {
    config.app_version = "0";
  }
  config.git_revision = get("GIT_REVISION");
  config.db_readers = std::max<std::size_t>(number("RAILS_MAX_THREADS", 5), 1);
  config.job_concurrency = std::max<std::size_t>(number("JOB_CONCURRENCY", 2), 1);
  config.log_level = present("RAILS_LOG_LEVEL").value_or("info");
  const std::size_t megabytes = number("CAMPFIRE_FRAGMENT_CACHE_MB", 32);
  config.fragment_cache_bytes = megabytes > (std::numeric_limits<std::size_t>::max() >> 20)
                                    ? std::numeric_limits<std::size_t>::max()
                                    : megabytes << 20;
  if (error) {
    return std::unexpected(std::move(*error));
  }
  return config;
}

FrontConfig FrontConfig::from_lookup(const EnvLookup& get) {
  const auto find = [&](std::string_view key) -> std::optional<std::string> {
    if (auto v = get("THRUSTER_" + std::string(key))) {
      return v;
    }
    return get(key);
  };
  const auto string = [&](std::string_view key, std::string_view fallback) {
    return find(key).value_or(std::string(fallback));
  };
  const auto integer = [&](std::string_view key, std::int64_t fallback) {
    const auto v = find(key);
    return (v ? parse_integer<std::int64_t>(*v) : std::nullopt).value_or(fallback);
  };
  const auto port = [&](std::string_view key, std::uint16_t fallback) {
    const std::int64_t v = integer(key, fallback);
    return v >= 0 && v <= 65535 ? static_cast<std::uint16_t>(v) : fallback;
  };
  const auto seconds = [&](std::string_view key, std::int64_t fallback) {
    return std::max<std::int64_t>(integer(key, fallback), 0);
  };
  const auto boolean = [&](std::string_view key, bool fallback) {
    const auto v = find(key);
    return (v ? parse_go_bool(*v) : std::nullopt).value_or(fallback);
  };

  FrontConfig c;
  if (const auto domains = find("TLS_DOMAIN")) {
    std::string_view rest = *domains;
    while (true) {
      const std::size_t comma = rest.find(',');
      const std::string_view domain = trim(rest.substr(0, comma));
      if (!domain.empty()) {
        c.tls_domains.emplace_back(domain);
      }
      if (comma == std::string_view::npos) {
        break;
      }
      rest = rest.substr(comma + 1);
    }
  }
  c.target_port = port("TARGET_PORT", 3000);
  c.target_bind = "127.0.0.1";
  if (const auto bind = find("TARGET_BIND")) {
    std::array<unsigned char, sizeof(in6_addr)> raw{};
    std::array<char, INET6_ADDRSTRLEN> text{};
    const std::string& value = *bind;
    if (inet_pton(AF_INET, value.c_str(), raw.data()) == 1) {
      inet_ntop(AF_INET, raw.data(), text.data(), text.size());
      c.target_bind = text.data();
    } else if (inet_pton(AF_INET6, value.c_str(), raw.data()) == 1) {
      inet_ntop(AF_INET6, raw.data(), text.data(), text.size());
      c.target_bind = text.data();
    }
  }
  c.cache_size = integer("CACHE_SIZE", 64 << 20);
  c.max_cache_item_size = integer("MAX_CACHE_ITEM_SIZE", 1 << 20);
  c.gzip_compression_enabled = boolean("GZIP_COMPRESSION_ENABLED", true);
  c.gzip_compression_disable_on_auth = boolean("GZIP_COMPRESSION_DISABLE_ON_AUTH", false);
  c.gzip_compression_jitter = integer("GZIP_COMPRESSION_JITTER", 32);
  c.max_request_body = integer("MAX_REQUEST_BODY", 0);
  c.acme_directory_url = string("ACME_DIRECTORY", kLetsEncryptUrl);
  c.eab_kid = string("EAB_KID", "");
  c.eab_hmac_key = string("EAB_HMAC_KEY", "");
  c.storage_path = string("STORAGE_PATH", "./storage/thruster");
  c.http_port = port("HTTP_PORT", 80);
  c.https_port = port("HTTPS_PORT", 443);
  c.http_idle_timeout_s = seconds("HTTP_IDLE_TIMEOUT", 60);
  c.http_read_timeout_s = seconds("HTTP_READ_TIMEOUT", 30);
  c.http_write_timeout_s = seconds("HTTP_WRITE_TIMEOUT", 30);
  c.h2c_enabled = boolean("H2C_ENABLED", false);
  c.debug = boolean("DEBUG", false);
  c.log_requests = boolean("LOG_REQUESTS", true);
  c.forward_headers = boolean("FORWARD_HEADERS", !c.has_tls());
  return c;
}

}  // namespace campfire
