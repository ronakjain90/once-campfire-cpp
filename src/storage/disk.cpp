// See disk.hpp.
#include "storage/disk.hpp"

#include <fstream>
#include <system_error>

#include "compat/json.hpp"
#include "storage/disposition.hpp"
#include "storage/errors.hpp"
#include "storage/key.hpp"

namespace campfire::storage {

namespace fs = std::filesystem;
namespace json = compat::json;

namespace {

// [key[0..1], key[2..3]].join("/")
std::string folder_for(std::string_view key) {
  std::string_view a = key.substr(0, 2);
  std::string_view b = key.size() > 2 ? key.substr(2, 2) : std::string_view();
  return std::string(a) + "/" + std::string(b);
}

json::Value optional_string(std::optional<std::string_view> s) {
  return s ? json::Value(*s) : json::Value();
}

const std::string* string_member(const json::Value& object, std::string_view name) {
  const json::Value* v = object.find(name);
  return v ? v->get_string() : nullptr;
}

}  // namespace

fs::path DiskService::path_for(std::string_view key) const {
  return root_ / folder_for(key) / std::string(key);
}

Result<fs::path> DiskService::make_path_for(std::string_view key) const {
  fs::path path = path_for(key);
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) return io_error("mkdir failed: " + ec.message());
  return path;
}

Status DiskService::ensure_integrity_of(std::string_view key, std::string_view expected) const {
  auto actual = checksum_file(path_for(key));
  if (!actual) return std::unexpected(actual.error());
  if (*actual != expected) {
    if (auto s = remove(key); !s) return s;
    return integrity_error();
  }
  return {};
}

Status DiskService::upload_file(std::string_view key, const fs::path& source,
                                std::optional<std::string_view> checksum) const {
  auto path = make_path_for(key);
  if (!path) return std::unexpected(path.error());
  std::error_code ec;
  fs::copy_file(source, *path, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    if (ec == std::errc::no_such_file_or_directory) return file_not_found();
    return io_error("copy failed: " + ec.message());
  }
  if (checksum) return ensure_integrity_of(key, *checksum);
  return {};
}

Status DiskService::upload(std::string_view key, std::string_view data,
                           std::optional<std::string_view> checksum) const {
  auto path = make_path_for(key);
  if (!path) return std::unexpected(path.error());
  {
    std::ofstream out(*path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush();
    if (!out) {
      std::error_code ec;
      fs::remove(*path, ec);
      return io_error("write failed: " + path->string());
    }
  }
  if (checksum) return ensure_integrity_of(key, *checksum);
  return {};
}

Result<std::string> DiskService::download(std::string_view key) const {
  std::ifstream in(path_for(key), std::ios::binary);
  if (!in) return file_not_found();
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

Status DiskService::remove(std::string_view key) const {
  std::error_code ec;
  fs::remove(path_for(key), ec);
  if (ec && ec != std::errc::no_such_file_or_directory) return io_error("delete failed: " + ec.message());
  return {};
}

Status DiskService::remove_prefixed(std::string_view prefix) const {
  fs::path pattern = path_for(prefix);
  fs::path dir = pattern.parent_path();
  std::string stem = pattern.filename().string();
  std::error_code ec;
  fs::directory_iterator it(dir, ec);
  if (ec) return {};
  for (const auto& entry : it) {
    if (!entry.path().filename().string().starts_with(stem)) continue;
    std::error_code rm;
    fs::remove_all(entry.path(), rm);
    if (rm) return io_error("delete failed: " + rm.message());
  }
  return {};
}

bool DiskService::exist(std::string_view key) const {
  std::error_code ec;
  return fs::exists(path_for(key), ec);
}

std::string DiskService::url_path(const compat::MessageVerifier& verifier, std::string_view key,
                                  std::optional<compat::Timestamp> expires_at, const Filename& filename,
                                  std::optional<std::string_view> content_type, std::string_view disposition) const {
  std::string sanitized = filename.sanitized();
  json::Value payload(json::Value::Object{
      {"key", json::Value(key)},
      {"disposition", json::Value(content_disposition_with(disposition, sanitized))},
      {"content_type", optional_string(content_type)},
      {"service_name", json::Value(name_)},
  });
  std::string encoded_key = verifier.generate_raw(json::encode(payload), "blob_key", expires_at);
  return "/rails/active_storage/disk/" + escape_segment(encoded_key) + "/" + escape_path(sanitized);
}

std::string DiskService::url_path_for_direct_upload(const compat::MessageVerifier& verifier, std::string_view key,
                                                    compat::Timestamp expires_at,
                                                    std::optional<std::string_view> content_type,
                                                    int64_t content_length, std::string_view checksum) const {
  json::Value payload(json::Value::Object{
      {"key", json::Value(key)},
      {"content_type", optional_string(content_type)},
      {"content_length", json::Value(content_length)},
      {"checksum", json::Value(checksum)},
      {"service_name", json::Value(name_)},
  });
  std::string token = verifier.generate_raw(json::encode(payload), "blob_token", expires_at);
  return "/rails/active_storage/disk/" + escape_segment(token);
}

std::optional<DiskKey> decode_verified_key(const compat::MessageVerifier& verifier, std::string_view encoded_key,
                                           compat::Timestamp now) {
  auto text = verifier.verify_raw(encoded_key, "blob_key", now);
  if (!text) return std::nullopt;
  auto data = json::parse(*text);
  if (!data || !data->is_object()) return std::nullopt;
  const std::string* key = string_member(*data, "key");
  const std::string* disposition = string_member(*data, "disposition");
  const std::string* service = string_member(*data, "service_name");
  if (!key || !disposition || !service) return std::nullopt;
  DiskKey out{*key, *disposition, std::nullopt, *service};
  if (const std::string* type = string_member(*data, "content_type")) out.content_type = *type;
  return out;
}

std::optional<DiskToken> decode_verified_token(const compat::MessageVerifier& verifier, std::string_view encoded_token,
                                               compat::Timestamp now) {
  auto text = verifier.verify_raw(encoded_token, "blob_token", now);
  if (!text) return std::nullopt;
  auto data = json::parse(*text);
  if (!data || !data->is_object()) return std::nullopt;
  const std::string* key = string_member(*data, "key");
  const std::string* checksum = string_member(*data, "checksum");
  const std::string* service = string_member(*data, "service_name");
  const json::Value* length = data->find("content_length");
  if (!key || !checksum || !service || !length || !length->to_int64()) return std::nullopt;
  DiskToken out{*key, std::nullopt, *length->to_int64(), *checksum, *service};
  if (const std::string* type = string_member(*data, "content_type")) out.content_type = *type;
  return out;
}

}  // namespace campfire::storage
