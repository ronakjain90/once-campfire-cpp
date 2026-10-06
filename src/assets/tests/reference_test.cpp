// Golden tests against the output of the reference app (fixtures in tests/data, from the Rust crate).
// The files in overrides/ differ from the reference on purpose, so only their digests and bytes may differ.
#include <doctest.h>
#include <openssl/evp.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "assets/assets.hpp"
#include "assets/static_files.hpp"
#include "compat/json.hpp"

namespace fs = std::filesystem;
namespace json = campfire::compat::json;
namespace assets = campfire::assets;

namespace {

std::string fixture(const std::string& name) {
  std::ifstream in(std::string(CAMPFIRE_ASSETS_FIXTURES) + "/" + name, std::ios::binary);
  REQUIRE(in.good());
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

json::Value json_fixture(const std::string& name) {
  auto value = json::parse(fixture(name));
  REQUIRE(value.has_value());
  return std::move(*value);
}

std::string sha256(std::string_view bytes) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  EVP_Digest(bytes.data(), bytes.size(), md, &len, EVP_sha256(), nullptr);
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  for (unsigned int i = 0; i < len; ++i) {
    out += kDigits[md[i] >> 4];
    out += kDigits[md[i] & 15];
  }
  return out;
}

// The logical paths in overrides/.
std::vector<std::string> override_files() {
  const fs::path dir = fs::path(CAMPFIRE_RAILS_ASSETS) / "overrides";
  std::vector<std::string> files;
  for (const auto& entry : fs::recursive_directory_iterator(dir)) {
    if (entry.is_regular_file()) {
      files.push_back(entry.path().lexically_relative(dir).string());
    }
  }
  return files;
}

// (reference digested path, our digested path) for each overridden logical path.
std::map<std::string, std::pair<std::string, std::string>> overridden() {
  const json::Value reference = json_fixture("manifest.json");
  std::map<std::string, std::pair<std::string, std::string>> out;
  for (const auto& logical : override_files()) {
    const json::Value* entry = reference.find(logical);
    if (entry == nullptr) {
      continue;
    }
    const std::string& theirs = entry->find("digested_path")->as_string();
    const auto ours = assets::digested_path(logical);
    REQUIRE(ours.has_value());
    CHECK_MESSAGE(theirs != *ours, logical << " is overridden but has the same digest");
    out[logical] = {theirs, std::string(*ours)};
  }
  return out;
}

// The logical paths that the overrides add and the reference does not have.
std::set<std::string> added() {
  const json::Value reference = json_fixture("manifest.json");
  std::set<std::string> out;
  for (const auto& logical : override_files()) {
    if (reference.find(logical) == nullptr) {
      out.insert(logical);
    }
  }
  return out;
}

// `text` with our digested paths of the overridden files replaced by the reference ones.
std::string as_reference(std::string text) {
  for (const auto& [logical, paths] : overridden()) {
    const auto& [theirs, ours] = paths;
    for (auto at = text.find(ours); at != std::string::npos; at = text.find(ours, at + theirs.size())) {
      text.replace(at, ours.size(), theirs);
    }
  }
  return text;
}

assets::StaticResponse get(const std::string& path) {
  auto response = assets::serve({.method = "GET", .path = path});
  REQUIRE_MESSAGE(response.has_value(), path << " is not served");
  return std::move(*response);
}

}  // namespace

TEST_CASE("the manifest equals the Propshaft precompile of the reference") {
  const json::Value reference = json_fixture("manifest.json");
  const auto skip = added();
  std::size_t total = 0;
  std::size_t equal = 0;
  std::vector<std::string> different;
  for (const auto& record : assets::manifest()) {
    if (skip.contains(std::string(record.logical))) {
      continue;
    }
    ++total;
    const json::Value* entry = reference.find(record.logical);
    if (entry != nullptr && entry->find("digested_path")->as_string() == as_reference(std::string(record.digested))) {
      ++equal;
    } else {
      different.emplace_back(record.logical);
    }
  }
  MESSAGE("assets: total=" << total << " equal=" << equal << " different=" << different.size()
                           << " overridden=" << overridden().size() << " added=" << skip.size());
  CHECK(different.empty());
  CHECK(total == reference.as_object().size());
}

TEST_CASE("the manifest JSON equals the reference") {
  const auto served = json::parse(as_reference(std::string(assets::manifest_json())));
  REQUIRE(served.has_value());
  json::Value::Object members = served->as_object();
  std::erase_if(members, [&](const auto& m) { return added().contains(m.first); });
  json::Value::Object expected = json_fixture("manifest.json").as_object();
  // The integrity hash of an overridden file covers its own bytes.
  const auto over = overridden();
  for (auto* list : {&members, &expected}) {
    for (auto& [logical, value] : *list) {
      if (over.contains(logical)) {
        value.as_object().erase(std::remove_if(value.as_object().begin(), value.as_object().end(),
                                               [](const auto& m) { return m.first == "integrity"; }),
                                value.as_object().end());
      }
    }
  }
  // The reference lists the files in the order of readdir of its build host. Compare by key.
  for (auto* list : {&members, &expected}) {
    std::sort(list->begin(), list->end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  }
  CHECK(json::generate(json::Value(members)) == json::generate(json::Value(expected)));
}

TEST_CASE("compiled bodies equal the reference precompile byte for byte") {
  const json::Value reference = json_fixture("compiled_sha256.json");
  std::set<std::string> skip;
  for (const auto& [logical, paths] : overridden()) {
    skip.insert(paths.first);
  }
  std::vector<std::string> mismatched;
  for (const auto& [digested, hash] : reference.as_object()) {
    if (skip.contains(digested)) {
      continue;
    }
    const auto response = get("/assets/" + digested);
    if (sha256(response.body()) != hash.as_string()) {
      mismatched.push_back(digested);
    }
  }
  CHECK(mismatched.empty());
  CHECK(reference.as_object().size() + added().size() == assets::manifest().size());
}

TEST_CASE("the stylesheet tags and the link header equal the reference") {
  const auto tags = assets::stylesheet_link_tag_all(
      std::vector<std::pair<std::string_view, std::string_view>>{{"data-turbo-track", "reload"}});
  REQUIRE(tags.has_value());
  CHECK(tags->html == fixture("stylesheet_link_tag_all.html"));
  CHECK(assets::append_preload_links("", tags->preload_links) == fixture("link_header.txt"));
}

TEST_CASE("the import map tags equal the reference") {
  CHECK(as_reference(std::string(assets::javascript_importmap_tags())) == fixture("javascript_importmap_tags.html"));
}

TEST_CASE("public files are served like ActionDispatch::Static") {
  const auto over = overridden();
  const json::Value cases = json_fixture("static_responses.json");
  std::size_t checked = 0;
  for (const auto& c : cases.as_array()) {
    const json::Value& env = *c.find("env");
    std::string path = c.find("path")->as_string();
    const std::string method = c.find("method")->as_string();
    bool is_override = false;
    for (const auto& [logical, paths] : over) {
      if (path == "/assets/" + paths.first) {
        path = "/assets/" + paths.second;
        is_override = true;
      }
    }
    const auto env_string = [&](const char* key) -> std::optional<std::string_view> {
      const json::Value* v = env.find(key);
      if (v == nullptr || !v->is_string()) {
        return std::nullopt;
      }
      return v->as_string();
    };
    std::string label = method;
    label += " ";
    label += path;
    label += " ";
    label += json::generate(env);
    const int expected_status = static_cast<int>(*c.find("status")->to_int64());
    const json::Value& expected_headers = *c.find("headers");

    // The probe of the reference answers 404 with x-cascade: pass. Here the request falls through.
    if (expected_status == 404 && expected_headers.find("x-cascade") != nullptr) {
      CHECK_MESSAGE(!assets::serve({.method = method, .path = path, .range = env_string("HTTP_RANGE")}).has_value(),
                    label);
      continue;
    }
    const auto response = assets::serve({.method = method, .path = path, .range = env_string("HTTP_RANGE")});
    REQUIRE_MESSAGE(response.has_value(), label);
    CHECK_MESSAGE(response->status == expected_status, label);

    std::map<std::string, std::string> ours;
    for (const auto& [name, value] : response->headers) {
      if (name != "last-modified") {
        ours[std::string(name)] = value;
      }
    }
    std::map<std::string, std::string> theirs;
    for (const auto& [name, value] : expected_headers.as_object()) {
      theirs[name] = value.as_string();
    }
    // The manifest of the reference has the order of readdir of the build host, and the bytes of
    // an overridden file are its own. Only the type can match.
    if (path == "/assets/.manifest.json" || is_override) {
      CHECK_MESSAGE(ours["content-type"] == theirs["content-type"], label);
      continue;
    }
    CHECK_MESSAGE(ours == theirs, label);
    if (method == "GET") {
      CHECK_MESSAGE(sha256(response->body()) == c.find("body_sha256")->as_string(), label);
    }
    ++checked;
  }
  MESSAGE("static cases compared in full: " << checked << " of " << cases.as_array().size());
}

TEST_CASE("Last-Modified round-trips to a 304") {
  const auto response = get("/robots.txt");
  const std::string last_modified(*response.header("last-modified"));
  const auto not_modified = assets::serve({.method = "GET", .path = "/robots.txt", .if_modified_since = last_modified});
  REQUIRE(not_modified.has_value());
  CHECK(not_modified->status == 304);
  CHECK(not_modified->headers.empty());
  CHECK(not_modified->body().empty());
}

TEST_CASE("a HEAD request has no body but has the length") {
  const auto response = assets::serve({.method = "HEAD", .path = "/robots.txt"});
  REQUIRE(response.has_value());
  CHECK(response->status == 200);
  CHECK(response->body().empty());
  CHECK(response->header("content-length") == "99");
}

TEST_CASE("several ranges give a multipart body") {
  const auto path = assets::audio_path("56k.mp3");
  REQUIRE(path.has_value());
  const auto response = assets::serve({.method = "GET", .path = *path, .range = "bytes=0-1, 4-5"});
  REQUIRE(response.has_value());
  CHECK(response->status == 206);
  // Rack sets multipart/byteranges, then Static overwrites it with the type of the file.
  CHECK(response->header("content-type") == "audio/mpeg");
  const std::string body(response->body());
  CHECK(body.starts_with("\r\n--AaB03x\r\ncontent-type: audio/mpeg\r\ncontent-range: bytes 0-1/"));
  CHECK(body.ends_with("\r\n--AaB03x--\r\n"));
}

TEST_CASE("the helpers resolve logical paths") {
  CHECK(assets::asset_path("bot.svg?v=1#x").value() ==
        "/assets/" + std::string(*assets::digested_path("bot.svg")) + "?v=1#x");
  CHECK(assets::asset_path("https://example.com/a.png").value() == "https://example.com/a.png");
  CHECK(assets::asset_path("//cdn.example.com/a.png").value() == "//cdn.example.com/a.png");
  CHECK(assets::asset_path("data:image/png;base64,xx").value() == "data:image/png;base64,xx");
  CHECK(assets::asset_path("/rooms/1").value() == "/rooms/1");
  CHECK(assets::asset_path("").value().empty());
  CHECK(assets::stylesheet_path("base").value() == assets::stylesheet_path("base.css").value());
  CHECK(assets::javascript_path("application").value().starts_with("/assets/application-"));
  CHECK(assets::image_url("https://chat.example.com/", "add.svg").value() ==
        "https://chat.example.com" + assets::image_path("add.svg").value());
  const auto missing = assets::asset_path("nope.png");
  REQUIRE(!missing.has_value());
  CHECK(missing.error().message == "The asset 'nope.png' was not found in the load path.");
}

TEST_CASE("the table has compressed bodies that decode to the identity body") {
  const auto file = assets::find_file("/assets/" + assets::stylesheet_path("base").value().substr(8));
  REQUIRE(file.has_value());
  CHECK(file->content_type == "text/css");
  CHECK(!file->gzip.empty());
  CHECK(!file->zstd.empty());
  CHECK(file->gzip.size() < file->identity.size());
  CHECK(assets::choose_body(*file, "gzip, br").content_encoding == "gzip");
  CHECK(assets::choose_body(*file, "gzip, zstd").content_encoding == "zstd");
  CHECK(assets::choose_body(*file, "identity").body == file->identity);
  const auto image = assets::find_file("/assets/" + assets::image_path("add.svg").value().substr(8));
  REQUIRE(image.has_value());
  CHECK(assets::choose_body(*image, "gzip").body.size() > 0);
}
