// Golden vectors vectors/storage.json: digests, keys, signatures and Content-Disposition
// (not image bytes).
#include "compat/content_disposition.hpp"
#include "compat/signed_id.hpp"
#include "compat/variation.hpp"
#include "vectors.hpp"

using namespace testing_support;
namespace compat = campfire::compat;
namespace marshal = campfire::compat::marshal;

namespace {

const json::Value& storage() {
  return load_vectors("storage.json");
}
const compat::Secrets& secrets() {
  static const compat::Secrets s(at(load_vectors("rails_compat.json"), "secret_key_base").as_string());
  return s;
}
compat::Timestamp now() {
  return *compat::parse_iso8601("2026-09-26T12:00:00Z");
}

marshal::Value typed(const json::Value& v) {
  if (v.is_null()) return marshal::Value::nil();
  if (v.is_bool()) return marshal::Value::boolean(v.as_bool());
  if (auto n = v.to_int64()) return marshal::Value::integer(*n);
  if (v.is_array()) {
    marshal::Value::Array items;
    for (const auto& i : v.as_array()) items.push_back(typed(i));
    return marshal::Value::array(std::move(items));
  }
  if (const json::Value* s = v.find("sym")) return marshal::Value::symbol(s->as_string());
  if (const json::Value* s = v.find("str")) return marshal::Value::string(s->as_string());
  marshal::Value::Hash entries;
  for (const auto& pair : at(v, "hash").as_array())
    entries.emplace_back(pair.as_array()[0].as_string(), typed(pair.as_array()[1]));
  return marshal::Value::hash(std::move(entries));
}

compat::Variation variation(const json::Value& v) {
  marshal::Value value = typed(v);
  const auto* hash = std::get_if<marshal::Value::Hash>(&value.variant());
  REQUIRE(hash != nullptr);
  return compat::Variation(*hash);
}

// The path segment at `index` after splitting on "/".
std::string segment(const std::string& path, size_t index) {
  size_t start = 0;
  for (size_t i = 0; i < index; ++i) start = path.find('/', start) + 1;
  return path.substr(start, path.find('/', start) - start);
}

}  // namespace

TEST_CASE("storage variations") {
  Group g("storage.json", "variations");
  const auto& verifier = secrets().active_storage_verifier();
  for (const auto& v : items(at(storage(), "variations"))) {
    std::string label = at(v, "inspect").as_string();
    auto var = variation(at(v, "typed"));
    g.check(compat::crypto::hex_encode(var.marshal()) == at(v, "marshal_hex").as_string(), label + " marshal");
    g.check(var.digest() == at(v, "digest").as_string(), label + " digest");
    g.check(var.key(verifier) == at(v, "key").as_string(), label + " key");
    auto decoded = compat::Variation::decode(verifier, at(v, "key").as_string(), now());
    bool ok = decoded && *decoded == variation(at(v, "decoded_typed")) &&
              compat::crypto::hex_encode(decoded->marshal()) == at(v, "decoded_marshal_hex").as_string() &&
              decoded->digest() == at(v, "decoded_digest").as_string();
    g.check(ok, label + " decode");
  }
  g.finish();
}

TEST_CASE("storage verifier messages and disk URLs") {
  Group g("storage.json", "verifier");
  const auto& v = at(storage(), "verifier");
  const auto& verifier = secrets().active_storage_verifier();
  auto expires = *compat::parse_iso8601("2030-01-02T03:04:05.678Z");
  const std::string& expiring = at(v, "expiring").as_string();
  g.check(verifier.generate_raw("\"x\"", "p", expires) == expiring, "expiring generate");
  g.check(verifier.verify_raw(expiring, "p", now()) == "\"x\"", "expiring verify");
  g.check(!verifier.verify_raw(expiring, "p", expires).has_value(), "expired at expiry");
  g.check(!verifier.verify_raw(expiring, "q", now()).has_value(), "wrong purpose");

  const std::string key = "abcdefghijklmnopqrstuvwxyz12";
  const std::string sanitized = "weird & -name- \xC3\xBCn\xC3\xAF.png";
  auto disk_key = [&](std::string_view disposition, json::Value content_type) {
    json::Value payload(json::Value::Object{});
    payload.set("key", key);
    payload.set("disposition", compat::content_disposition(disposition, sanitized));
    payload.set("content_type", std::move(content_type));
    payload.set("service_name", "local");
    return verifier.generate_raw(json::encode(payload), "blob_key");
  };
  g.check(disk_key("inline", "image/png") == segment(at(v, "disk_url_path").as_string(), 4), "disk url key");
  g.check(disk_key("attachment", nullptr) == segment(at(v, "disk_url_path_nil_type").as_string(), 4),
          "disk url key, nil type");

  // The direct upload token: Rails made it, so it must verify with the purpose and a time before expiry.
  std::string token = segment(at(v, "direct_upload_path").as_string(), 4);
  auto issued = *compat::parse_iso8601("2020-01-01T00:00:00Z");
  auto data = verifier.verify_raw(token, "blob_token", issued);
  g.check(
      data &&
          *data ==
              R"({"key":"abcdefghijklmnopqrstuvwxyz12","content_type":"image/png","content_length":42,"checksum":"abc==","service_name":"local"})",
      "direct upload token");
  g.finish();
}

TEST_CASE("storage filenames and dispositions") {
  Group g("storage.json", "filenames");
  for (const auto& f : items(at(storage(), "filenames"))) {
    const std::string& sanitized = at(f, "sanitized").as_string();
    g.check(compat::content_disposition("inline", sanitized) == at(f, "inline").as_string(), sanitized + " inline");
    g.check(compat::content_disposition("attachment", sanitized) == at(f, "attachment").as_string(),
            sanitized + " attachment");
  }
  CHECK(compat::content_disposition("inline", "\xC5\x81\xC3\xB3\x64\xC5\xBA \xC3\x97.pdf") ==
        "inline; filename=\"Lodz x.pdf\"; filename*=UTF-8''%C5%81%C3%B3d%C5%BA%20%C3%97.pdf");
  g.finish();
}

TEST_CASE("storage messages: blob signed ids and representation keys") {
  Group g("storage.json", "messages");
  for (const auto& m : items(at(storage(), "messages"))) {
    std::string name = at(m, "fixture").as_string();
    const std::string& path = at(m, "rails_blob_path").as_string();
    std::string signed_id = segment(path, 5);
    int64_t blob_id = *at(at(m, "blob"), "id").to_int64();
    g.check(compat::signed_id::blob_signed_id(secrets(), blob_id) == signed_id, name + " signed id");
    g.check(compat::signed_id::verify_blob_signed_id(secrets(), signed_id, now()) == blob_id, name + " verify");
    if (at(m, "thumb_path").is_string()) {
      auto thumb = variation(at(items(at(m, "variants"))[0], "transformations_typed"));
      g.check(thumb.key(secrets().active_storage_verifier()) == segment(at(m, "thumb_path").as_string(), 6),
              name + " thumb key");
    }
  }
  g.finish();
}
