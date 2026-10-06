// End to end tests of the Active Storage endpoints: blob and representation redirects and proxies, the disk service
// (ranges, conditional GET, Content-Disposition), direct uploads and their checks. Rails: activestorage controllers,
// config/initializers/active_storage*.rb. Rust: crates/campfire/src/active_storage.rs.
#include <doctest.h>

#include "app/controllers/accounts_common.hpp"
#include "app/tests/fixture.hpp"
#include "compat/base64.hpp"
#include "compat/json.hpp"
#include "models/attachments.hpp"
#include "storage/key.hpp"
#include "storage/paths.hpp"

namespace campfire::app::testing {

namespace {

std::string cookie_pair(const Reply& reply, const std::string& name) {
  for (const auto& [k, v] : reply.headers) {
    if (k == "set-cookie" && v.starts_with(name + "=")) return v.substr(0, v.find(';'));
  }
  return {};
}

std::string sign_in(Client& c) {
  const Reply r = c.request("POST", "/session", kSameOrigin + kForm,
                            std::string("email_address=david@example.com&password=") + kPassword);
  REQUIRE(r.status == 302);
  return "Cookie: " + cookie_pair(r, "session_token") + "\r\n";
}

storage::Blob make_blob(Fixture& f, const std::string& data, const std::string& name, const std::string& type) {
  std::optional<storage::Blob> blob;
  f.write([&](db::Tx& tx) -> Status {
    auto staged = f.state->storage->stage_bytes(data, storage::Filename(name), std::string_view(type));
    if (!staged) return std::unexpected(staged.error());
    models::attachments::AttachmentRecords records(tx.conn());
    auto saved = staged->insert(records, controllers::to_compat(tx.now()));
    if (!saved) return std::unexpected(saved.error());
    staged->keep();
    blob = std::move(*saved);
    return {};
  });
  REQUIRE(blob);
  return std::move(*blob);
}

std::string path_of(const std::string& url) {
  REQUIRE(url.starts_with("http://test.example"));
  return url.substr(std::string("http://test.example").size());
}

constexpr const char* kPngBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";

}  // namespace

TEST_CASE("active storage: a blob redirects to a signed disk URL that serves the file") {
  Fixture f;
  Client c(f.port());
  const auto blob = make_blob(f, "hello world", "hello.txt", "text/plain");
  const auto& verifier = f.state->storage->verifier();
  const Reply redirect = c.request("GET", storage::paths::blob_redirect_path(verifier, blob));
  CHECK(redirect.status == 302);
  CHECK(redirect.header("cache-control") == "max-age=300, private");
  const std::string disk = path_of(redirect.header("location"));
  CHECK(disk.starts_with("/rails/active_storage/disk/"));
  CHECK(disk.ends_with("/hello.txt"));

  const Reply file = c.request("GET", disk);
  CHECK(file.status == 200);
  CHECK(file.body == "hello world");
  CHECK(file.header("content-type") == "text/plain");
  CHECK(file.header("cache-control") == "max-age=3600, public");
  CHECK(file.header("content-disposition") == "attachment; filename=\"hello.txt\"; filename*=UTF-8''hello.txt");
  CHECK(file.header("content-length") == "11");

  const Reply range = c.request("GET", disk, "Range: bytes=0-4\r\n");
  CHECK(range.status == 206);
  CHECK(range.body == "hello");
  CHECK(range.header("content-range") == "bytes 0-4/11");
  const Reply multi = c.request("GET", disk, "Range: bytes=0-1,6-7\r\n");
  CHECK(multi.status == 206);
  // `serve_file` sets the content type of the key after Rack: the multipart type is lost (Rails does the same).
  CHECK(multi.header("content-type") == "text/plain");
  CHECK(multi.body.find("\r\n--AaB03x\r\ncontent-type: text/plain\r\ncontent-range: bytes 6-7/11\r\n\r\nwo") !=
        std::string::npos);
  const Reply unsatisfiable = c.request("GET", disk, "Range: bytes=100-200\r\n");
  CHECK(unsatisfiable.status == 416);
  CHECK(unsatisfiable.header("content-range") == "bytes */11");

  // A type that is served as binary is forced to download, whatever `disposition` says. A ranged request is not cached
  // by the front, so the conditional GET after it reaches the controller.
  const auto other = make_blob(f, "second file", "second.txt", "text/plain");
  const Reply forced = c.request("GET", storage::paths::blob_redirect_path(verifier, other, "inline"));
  CHECK(forced.status == 302);
  const std::string forced_disk = path_of(forced.header("location"));
  const Reply probe = c.request("GET", forced_disk, "Range: bytes=0-0\r\n");
  const Reply fresh = c.request("GET", forced_disk, "If-Modified-Since: " + probe.header("last-modified") + "\r\n");
  CHECK(fresh.status == 304);
  CHECK(c.request("GET", forced_disk).header("content-disposition").starts_with("attachment"));
}

TEST_CASE("active storage: signatures are checked") {
  Fixture f;
  Client c(f.port());
  const auto blob = make_blob(f, "hello", "hello.txt", "text/plain");
  CHECK(c.request("GET", "/rails/active_storage/blobs/redirect/nope/x.png").status == 404);
  CHECK(c.request("GET", "/rails/active_storage/blobs/proxy/nope/x.png").status == 404);
  CHECK(c.request("GET", "/rails/active_storage/disk/nope/x.png").status == 404);
  CHECK(c.request("GET", "/rails/active_storage/representations/redirect/nope/nope/x.png").status == 404);
  // A valid signature of a blob that is gone is a 404 too (`RecordNotFound`).
  storage::Blob missing = blob;
  missing.id = 9999;
  CHECK(c.request("GET", storage::paths::blob_redirect_path(f.state->storage->verifier(), missing)).status == 404);
}

TEST_CASE("active storage: the proxy sends the file, caches it for ever and answers ranges") {
  Fixture f;
  Client c(f.port());
  const auto blob = make_blob(f, "hello world", "hello.txt", "text/plain");
  const std::string path = storage::paths::blob_proxy_path(f.state->storage->verifier(), blob);
  const Reply full = c.request("GET", path);
  CHECK(full.status == 200);
  CHECK(full.body == "hello world");
  CHECK(full.header("cache-control") == "max-age=3155695200, public, immutable");
  CHECK(full.header("accept-ranges") == "bytes");
  CHECK(full.header("content-disposition").starts_with("attachment; filename=\"hello.txt\""));
  CHECK_FALSE(full.header("etag").empty());
  CHECK(full.header("last-modified") == "Sat, 01 Jan 2011 00:00:00 GMT");
  const Reply fresh = c.request("GET", path, "If-None-Match: " + full.header("etag") + "\r\n");
  CHECK(fresh.status == 304);
  const Reply range = c.request("GET", path, "Range: bytes=6-\r\n");
  CHECK(range.status == 206);
  CHECK(range.body == "world");
  CHECK(range.header("content-range") == "bytes 6-10/11");
  CHECK(c.request("GET", path, "Range: bytes=50-60\r\n").status == 416);
}

TEST_CASE("active storage: an image has a representation, made on the first request") {
  Fixture f;
  Client c(f.port());
  const auto png = *compat::base64::strict_decode(kPngBase64);
  const auto blob = make_blob(f, png, "dot.png", "image/png");
  const auto variation = storage::Variation::resize_to_limit(100, 100, "png");
  const auto& verifier = f.state->storage->verifier();
  const Reply redirect = c.request("GET", storage::paths::representation_redirect_path(verifier, blob, variation));
  REQUIRE(redirect.status == 302);
  const Reply image = c.request("GET", path_of(redirect.header("location")));
  CHECK(image.status == 200);
  CHECK(image.header("content-type") == "image/png");
  CHECK(image.body.starts_with("\x89PNG"));
  // The second request finds the variant: no new blob.
  const Reply again = c.request("GET", storage::paths::representation_redirect_path(verifier, blob, variation));
  CHECK(again.header("location") == redirect.header("location"));
  const Reply proxy = c.request("GET", storage::paths::representation_proxy_path(verifier, blob, variation));
  CHECK(proxy.status == 200);
  CHECK(proxy.body == image.body);
  CHECK(proxy.header("cache-control") == "max-age=3155695200, public, immutable");
  // A text blob has no representation: the error of Rails is a 500.
  const auto text = make_blob(f, "x", "x.txt", "text/plain");
  CHECK(c.request("GET", storage::paths::representation_redirect_path(verifier, text, variation)).status == 500);
}

TEST_CASE("active storage: direct uploads and the disk PUT need a session") {
  Fixture f;
  Client c(f.port());
  const std::string data = "uploaded bytes";
  const std::string checksum = storage::checksum(data);
  const std::string json_headers = kSameOrigin + "Content-Type: application/json\r\n";
  const std::string body =
      R"({"blob":{"filename":"up.txt","byte_size":14,"checksum":")" + checksum + R"(","content_type":"text/plain"}})";
  CHECK(c.request("POST", "/rails/active_storage/direct_uploads", json_headers, body).status == 401);
  const std::string cookie = sign_in(c);
  const Reply created = c.request("POST", "/rails/active_storage/direct_uploads", json_headers + cookie, body);
  REQUIRE(created.status == 200);
  const auto json = compat::json::parse(created.body.starts_with("\x1f") ? gunzip(created.body) : created.body);
  REQUIRE(json);
  CHECK(json->find("filename")->as_string() == "up.txt");
  CHECK(json->find("byte_size")->to_int64() == 14);
  CHECK(json->find("content_type")->as_string() == "text/plain");
  CHECK_FALSE(json->find("signed_id")->as_string().empty());
  const std::string put = path_of(json->find("direct_upload")->find("url")->as_string());
  CHECK(json->find("direct_upload")->find("headers")->find("Content-Type")->as_string() == "text/plain");

  const std::string put_headers = "Content-Type: text/plain\r\n";
  CHECK(c.request("PUT", put, put_headers, data).status == 401);
  CHECK(c.request("PUT", put, put_headers + cookie, "other bytes!!!").status == 422);  // another checksum
  CHECK(c.request("PUT", put, put_headers + cookie, "short").status == 422);           // another length
  CHECK(c.request("PUT", put, "Content-Type: image/png\r\n" + cookie, data).status == 422);
  CHECK(c.request("PUT", put, put_headers + cookie, data).status == 204);
  CHECK(c.request("PUT", "/rails/active_storage/disk/nope", put_headers + cookie, data).status == 404);

  // The file is there: the signed id of the new blob downloads it.
  const std::string signed_id = json->find("signed_id")->as_string();
  const Reply download = c.request("GET", "/rails/active_storage/blobs/proxy/" + signed_id + "/up.txt");
  CHECK(download.status == 200);
  CHECK(download.body == data);
  // `params.expect(blob: ...)`: missing fields are a 422 and a missing `blob` is a 400.
  CHECK(c.request("POST", "/rails/active_storage/direct_uploads", json_headers + cookie, R"({"blob":{"filename":"a"}})")
            .status == 422);
  CHECK(c.request("POST", "/rails/active_storage/direct_uploads", json_headers + cookie, "{}").status == 400);
}

}  // namespace campfire::app::testing
