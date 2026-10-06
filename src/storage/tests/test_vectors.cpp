// Golden vectors of storage.json that need no media work: Marcel, filenames, disk URLs, route paths
// (Rust: crates/storage/tests/vectors.rs).
#include "storage/content_types.hpp"
#include "storage/disk.hpp"
#include "storage/disposition.hpp"
#include "storage/marcel.hpp"
#include "storage/paths.hpp"
#include "support.hpp"

using namespace testing_support;
using namespace storage_test;
namespace st = campfire::storage;
namespace compat = campfire::compat;

namespace {

st::Blob blob_from(const json::Value& row) {
  st::Blob b;
  b.id = *at(row, "id").to_int64();
  b.key = at(row, "key").as_string();
  b.filename = st::Filename(at(row, "filename").as_string());
  if (at(row, "content_type").is_string()) b.content_type = at(row, "content_type").as_string();
  b.metadata = *json::parse(at(row, "metadata").as_string());
  b.service_name = at(row, "service_name").as_string();
  b.byte_size = *at(row, "byte_size").to_int64();
  if (at(row, "checksum").is_string()) b.checksum = at(row, "checksum").as_string();
  return b;
}

std::string segment(const std::string& path, size_t index) {
  size_t start = 0;
  for (size_t i = 0; i < index; ++i) start = path.find('/', start) + 1;
  return path.substr(start, path.find('/', start) - start);
}

}  // namespace

TEST_CASE("storage marcel identification") {
  REQUIRE_FIXTURES();
  Group g("storage.json", "marcel");
  for (const auto& s : items(at(vectors(), "marcel"))) {
    std::string data;
    if (at(s, "fixture").is_string()) {
      const std::string& name = at(s, "fixture").as_string();
      data = name == ".keep" ? std::string() : read_file(fixture_path(name));
    } else if (at(s, "data_hex").is_string()) {
      data = unhex(at(s, "data_hex").as_string());
    }
    auto got = st::marcel::identify(data, opt_str(at(s, "name")), opt_str(at(s, "declared_type")));
    std::string label = at(s, "name").is_string() ? at(s, "name").as_string() : "nil";
    label += " declared ";
    label += at(s, "declared_type").is_string() ? at(s, "declared_type").as_string() : "nil";
    label += " got " + got;
    g.check(got == at(s, "content_type").as_string(), label);
  }
  g.finish();
}

TEST_CASE("storage filenames and path escaping") {
  Group g("storage.json", "filenames (storage)");
  for (const auto& f : items(at(vectors(), "filenames"))) {
    st::Filename filename = st::Filename::from_bytes(unhex(at(f, "input_hex").as_string()));
    std::string sanitized = filename.sanitized();
    g.check(sanitized == at(f, "sanitized").as_string(), sanitized + " sanitized");
    if (json::valid_utf8(unhex(at(f, "input_hex").as_string()))) {
      g.check(hex(filename.base()) == at(f, "base_hex").as_string(), sanitized + " base");
      g.check(hex(filename.extension()) == at(f, "extension_hex").as_string(), sanitized + " extension");
    }
    g.check(st::escape_path(sanitized) == at(f, "escaped_path").as_string(), sanitized + " escaped path");
  }
  g.finish();
}

TEST_CASE("storage disk urls and tokens") {
  Group g("storage.json", "disk service");
  const auto& v = at(vectors(), "verifier");
  st::DiskService service("/tmp/unused", "local");
  const std::string key = "abcdefghijklmnopqrstuvwxyz12";
  st::Filename weird("weird & <name> \xC3\xBCn\xC3\xAF.png");
  g.check(service.url_path(verifier(), key, std::nullopt, weird, "image/png", "inline") ==
              at(v, "disk_url_path").as_string(),
          "url path");
  g.check(service.url_path(verifier(), key, std::nullopt, weird, std::nullopt, "attachment") ==
              at(v, "disk_url_path_nil_type").as_string(),
          "url path, nil type");
  auto decoded = st::decode_verified_key(verifier(), segment(at(v, "disk_url_path").as_string(), 4), now());
  g.check(decoded && decoded->key == key && decoded->content_type == "image/png" && decoded->service_name == "local",
          "decode key");
  auto issued = *compat::parse_iso8601("2020-01-01T00:00:00Z");
  auto token = st::decode_verified_token(verifier(), segment(at(v, "direct_upload_path").as_string(), 4), issued);
  g.check(token && token->content_length == 42 && token->checksum == "abc==", "decode token");
  g.finish();
}

TEST_CASE("storage route paths") {
  Group g("storage.json", "route paths");
  st::DiskService service("/tmp/unused", "local");
  for (const auto& m : items(at(vectors(), "messages"))) {
    st::Blob blob = blob_from(at(m, "blob"));
    std::string name = at(m, "fixture").as_string();
    g.check(st::paths::blob_redirect_path(verifier(), blob) == at(m, "rails_blob_path").as_string(),
            name + " redirect");
    g.check(
        st::paths::blob_redirect_path(verifier(), blob, "attachment") == at(m, "rails_blob_download_path").as_string(),
        name + " download");
    g.check(st::paths::blob_proxy_path(verifier(), blob) == at(m, "rails_blob_proxy_path").as_string(),
            name + " proxy");
    g.check(st::paths::verify_signed_blob_id(verifier(), segment(at(m, "rails_blob_path").as_string(), 5), now()) ==
                blob.id,
            name + " verify");
    auto service_path = [&](std::string_view disposition) {
      std::string_view type = blob.type();
      auto forced = st::content_types::forced_disposition(type);
      return "http://campfire.test" + service.url_path(verifier(), blob.key, std::nullopt, blob.filename,
                                                       st::content_types::for_serving(type),
                                                       forced ? *forced : disposition);
    };
    g.check(service_path("inline") == at(m, "service_url").as_string(), name + " service url");
    g.check(service_path("attachment") == at(m, "service_url_attachment").as_string(),
            name + " service url attachment");
    if (at(m, "thumb_path").is_string()) {
      auto thumb = variation_of(at(items(at(m, "variants"))[0], "transformations_typed"));
      g.check(st::paths::representation_redirect_path(verifier(), blob, thumb) == at(m, "thumb_path").as_string(),
              name + " thumb");
      g.check(st::paths::representation_proxy_path(verifier(), blob, thumb) == at(m, "thumb_proxy_path").as_string(),
              name + " thumb proxy");
    }
    if (at(m, "poster_path").is_string()) {
      auto poster = compat::Variation(compat::Variation::Transformations{
          {"format", compat::marshal::Value::symbol("webp")},
          {"resize_to_limit", compat::marshal::Value::array(
                                  {compat::marshal::Value::integer(1200), compat::marshal::Value::integer(800)})}});
      g.check(st::paths::representation_redirect_path(verifier(), blob, poster) == at(m, "poster_path").as_string(),
              name + " poster");
      g.check(st::paths::representation_proxy_path(verifier(), blob, poster) == at(m, "poster_proxy_path").as_string(),
              name + " poster proxy");
    }
  }
  g.finish();
}
