// The Active Storage controllers: Blobs::{Redirect,Proxy}, Representations::{Redirect,Proxy}, Disk, DirectUploads.
// Rails: activestorage/app/controllers/active_storage/*, config/initializers/active_storage{,_authentication}.rb.
// Rust: crates/campfire/src/active_storage.rs.
//
// These controllers inherit from `ActiveStorage::BaseController` (`protect_from_forgery with: :exception`), not from
// `ApplicationController`: none of the concerns of Campfire run. Downloads are public behind signed URLs. The disk
// PUT and the direct uploads need a Campfire session.
#include <chrono>
#include <random>

#include "app/active_storage.hpp"
#include "app/concerns.hpp"
#include "app/controllers/accounts_common.hpp"
#include "app/dispatch.hpp"
#include "app/file_server.hpp"
#include "compat/content_disposition.hpp"
#include "compat/ruby.hpp"
#include "core/time_format.hpp"
#include "models/attachments.hpp"
#include "models/session.hpp"
#include "req/body.hpp"
#include "storage/content_types.hpp"
#include "storage/key.hpp"
#include "storage/paths.hpp"

namespace campfire::app::controllers {

namespace {

namespace content_types = storage::content_types;

// `ActiveStorage.service_urls_expire_in`
constexpr std::int64_t kServiceUrlsExpireIn = 5 * 60;
// `http_cache_forever`: `expires_in 100.years`
constexpr std::uint64_t kHundredYears = 3'155'695'200;
// The most that the server keeps of a request body (net/options.hpp, `max_buffered_body`).
constexpr std::int64_t kMaxBufferedBody = std::int64_t{16} << 20;

// `ActiveStorage::SetBlob#set_blob`: `Blob.find_signed!(params[:signed_blob_id] || params[:signed_id])`. A bad
// signature is `head :not_found`. A valid one for a missing blob is `RecordNotFound`.
Flow<storage::Blob> set_blob(Rq& rq) {
  const std::string_view signed_id = rq.param_str("signed_blob_id").value_or(rq.param_str("signed_id").value_or(""));
  const auto blob_id = storage::paths::verify_signed_blob_id(rq.app.storage->verifier(), signed_id, to_compat(rq.now()));
  if (!blob_id) return halt(rq.head(404));
  auto blob = models::attachments::find_blob(rq.db(), *blob_id);
  if (!blob) return fail_internal(blob.error().message);
  if (!*blob) return fail_with(ErrorKind::NotFound, "Couldn't find ActiveStorage::Blob");
  return std::move(**blob);
}

// `set_representation`: `@blob.representation(params[:variation_key]).processed`. A bad variation key is `head
// :not_found`. It gives the blob that is the image of the variant or preview.
Task<Flow<storage::Blob>> set_representation(Rq& rq, const storage::Blob& blob) {
  const auto variation = storage::Variation::decode(rq.app.storage->verifier(),
                                                    rq.param_str("variation_key").value_or(""), to_compat(rq.now()));
  if (!variation) co_return halt(rq.head(404));
  co_return co_await active_storage::processed_representation(rq, blob, *variation);
}

// `blob.url(disposition:)` on the disk service: a signed `/rails/active_storage/disk/...` URL on the host of this
// request, which expires in `service_urls_expire_in`.
std::string blob_url(Rq& rq, const storage::Blob& blob, std::optional<std::string_view> disposition) {
  const storage::Storage& storage = *rq.app.storage;
  const std::string_view content_type = content_types::for_serving(blob.type());
  const std::string_view chosen =
      content_types::forced_disposition(blob.type()).value_or(disposition.value_or("inline"));
  const compat::Timestamp expires_at = to_compat(rq.now()) + std::chrono::seconds(kServiceUrlsExpireIn);
  return rq.url_for(
      storage.service().url_path(storage.verifier(), blob.key, expires_at, blob.filename, content_type, chosen));
}

Flow<net::Response> redirect_to_blob(Rq& rq, const storage::Blob& blob) {
  expires_in(rq, kServiceUrlsExpireIn, false);
  RedirectOptions options;
  options.allow_other_host = true;
  return rq.redirect_to(blob_url(rq, blob, rq.param_str("disposition")), std::move(options));
}

// `http_cache_forever(public: true)`: a hundred years, an ETag of the full path and a fixed `Last-Modified`. It gives
// the 304 response if the copy of the client is fresh.
std::optional<net::Response> http_cache_forever(Rq& rq) {
  CacheControl& cc = rq.cache_control;
  cc.no_store = false;
  cc.max_age = kHundredYears;
  cc.is_public = true;
  cc.immutable = true;
  if (rq.staged_header("date").empty()) rq.set_header("date", format_httpdate(rq.now()));
  Freshness freshness;
  freshness.etag = std::string(rq.request.target);
  freshness.last_modified = Timestamp::from_nanos(std::int64_t{1'293'840'000} * 1'000'000'000);  // 2011-01-01
  freshness.is_public = true;
  return rq.fresh_when(freshness);
}

// `send_data` and `send_stream`: the `Content-Disposition` for the sanitized file name.
void add_disposition(net::Response& response, std::string_view disposition, const storage::Blob& blob) {
  response.add_copy("content-disposition", compat::content_disposition(disposition, blob.filename.sanitized()));
}

// `send_blob_stream(blob, disposition:)`: the whole file, inline unless the type is forced to download.
Flow<net::Response> send_blob_stream(Rq& rq, const storage::Blob& blob, std::optional<std::string_view> disposition) {
  const std::filesystem::path path = rq.app.storage->path_for(blob);
  std::error_code ignored;
  if (!std::filesystem::is_regular_file(path, ignored)) {
    // `rescue ActiveStorage::FileNotFoundError`: `expires_now`, `head :not_found`.
    rq.cache_control = CacheControl{};
    rq.cache_control.no_cache = true;
    return rq.head(404);
  }
  const std::string_view chosen =
      content_types::forced_disposition(blob.type()).value_or(disposition.value_or("inline"));
  auto response = rq.send_file(path.string(), content_types::for_serving(blob.type()), std::nullopt, std::nullopt);
  if (!response) return response;
  add_disposition(*response, chosen, blob);
  return response;
}

std::string random_boundary() {
  std::random_device device;
  std::string out;
  constexpr std::string_view hex = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) out += hex[device() & 15U];
  return out;
}

// `send_blob_byte_range_data(blob, range_header)`
Flow<net::Response> send_blob_byte_range_data(Rq& rq, const storage::Blob& blob, std::string_view range) {
  const auto size = static_cast<std::uint64_t>(std::max<std::int64_t>(blob.byte_size, 0));
  const auto ranges = compat::byte_ranges(range, size);
  if (!ranges || ranges->empty()) return rq.head(416);
  const std::filesystem::path path = rq.app.storage->path_for(blob);
  std::error_code ignored;
  if (!std::filesystem::is_regular_file(path, ignored)) return fail_internal("ActiveStorage::FileNotFoundError");
  const std::string content_type_for_serving(content_types::for_serving(blob.type()));
  const std::string size_text = std::to_string(size);
  std::string content_type;
  std::string body;
  std::optional<std::string> content_range;
  const auto read = [&](const compat::ByteRange& r) -> Status {
    auto bytes = read_file_range(path, r.first, r.last);
    if (!bytes) return std::unexpected(bytes.error());
    body += *bytes;
    return {};
  };
  if (ranges->size() == 1) {
    const auto& only = ranges->front();
    content_type = content_type_for_serving;
    content_range = "bytes " + std::to_string(only.first) + "-" + std::to_string(only.last) + "/" + size_text;
    if (auto done = read(only); !done) return fail_internal(done.error().message);
  } else {
    const std::string boundary = random_boundary();
    content_type = "multipart/byteranges; boundary=" + boundary;
    for (const auto& r : *ranges) {
      body += "\r\n--" + boundary + "\r\nContent-Type: " + content_type_for_serving +
              "\r\nContent-Range: bytes " + std::to_string(r.first) + "-" + std::to_string(r.last) + "/" + size_text +
              "\r\n\r\n";
      if (auto done = read(r); !done) return fail_internal(done.error().message);
    }
    body += "\r\n--" + boundary + "--\r\n";
  }
  net::Response response = rq.send_data(body, content_type, std::nullopt, std::nullopt, 206);
  add_disposition(response, content_types::forced_disposition(blob.type()).value_or("inline"), blob);
  if (content_range) response.add_copy("content-range", *content_range);
  response.add("accept-ranges", "bytes");
  return response;
}

// `ActiveStorage::Blobs::RedirectController#show`
Task<Flow<net::Response>> blobs_redirect(Rq& rq) {
  if (auto verified = concerns::verify_authenticity_token(rq); !verified) co_return std::unexpected(std::move(verified.error()));
  auto blob = set_blob(rq);
  if (!blob) co_return std::unexpected(std::move(blob.error()));
  co_return redirect_to_blob(rq, *blob);
}

// `ActiveStorage::Blobs::ProxyController#show`
Task<Flow<net::Response>> blobs_proxy(Rq& rq) {
  if (auto verified = concerns::verify_authenticity_token(rq); !verified) co_return std::unexpected(std::move(verified.error()));
  auto blob = set_blob(rq);
  if (!blob) co_return std::unexpected(std::move(blob.error()));
  if (const std::string_view range = rq.request.header("range");
      compat::strip(range).size() != 0) {
    co_return send_blob_byte_range_data(rq, *blob, range);
  }
  if (auto fresh = http_cache_forever(rq)) co_return std::move(*fresh);
  auto response = send_blob_stream(rq, *blob, rq.param_str("disposition"));
  if (!response) co_return response;
  response->add("accept-ranges", "bytes");
  co_return response;
}

// `ActiveStorage::Representations::RedirectController#show`
Task<Flow<net::Response>> representations_redirect(Rq& rq) {
  if (auto verified = concerns::verify_authenticity_token(rq); !verified) co_return std::unexpected(std::move(verified.error()));
  auto blob = set_blob(rq);
  if (!blob) co_return std::unexpected(std::move(blob.error()));
  auto image = co_await set_representation(rq, *blob);
  if (!image) co_return std::unexpected(std::move(image.error()));
  co_return redirect_to_blob(rq, *image);
}

// `ActiveStorage::Representations::ProxyController#show`
Task<Flow<net::Response>> representations_proxy(Rq& rq) {
  if (auto verified = concerns::verify_authenticity_token(rq); !verified) co_return std::unexpected(std::move(verified.error()));
  auto blob = set_blob(rq);
  if (!blob) co_return std::unexpected(std::move(blob.error()));
  auto image = co_await set_representation(rq, *blob);
  if (!image) co_return std::unexpected(std::move(image.error()));
  if (auto fresh = http_cache_forever(rq)) co_return std::move(*fresh);
  co_return send_blob_stream(rq, *image, rq.param_str("disposition"));
}

std::optional<std::string_view> optional_header(const Rq& rq, std::string_view name) {
  if (!rq.request.has_header(name)) return std::nullopt;
  return rq.request.header(name);
}

// `ActiveStorage::DiskController#show`, and the `after_action` of the initializer: `Cache-Control`.
Task<Flow<net::Response>> disk_show(Rq& rq) {
  const storage::Storage& storage = *rq.app.storage;
  const auto key = storage::decode_verified_key(storage.verifier(), rq.param_str("encoded_key").value_or(""),
                                                to_compat(rq.now()));
  if (!key) co_return rq.head(404);
  const FileRequest file_request{rq.request.method_text, optional_header(rq, "range"),
                                 optional_header(rq, "if-modified-since")};
  std::optional<std::string_view> content_type;
  if (key->content_type) content_type = std::string_view(*key->content_type);
  auto served = serve_file(file_request, storage.service().path_for(key->key), content_type, key->disposition);
  if (!served) {
    if (served.error().code == Errc::NotFound) co_return rq.head(404);
    co_return fail_internal(served.error().message);
  }
  net::Response response = rq.ctx.response(served->status);
  for (const auto& [name, value] : served->headers) response.add_copy(name, value);
  if (!served->body.empty()) response.body_view(rq.arena().copy(served->body));
  response.add("cache-control", "max-age=3600, public");
  co_return response;
}

// `ActiveStorageAuthentication#require_active_storage_authentication`: 401 without a session.
Flow<void> require_active_storage_authentication(Rq& rq) {
  const auto token = rq.cookies().signed_value("session_token");
  if (token) {
    auto session = models::sessions::find_by_token(rq.db(), rq.arena(), *token);
    if (!session) return fail_internal(session.error().message);
    if (*session) return {};
  }
  return halt(rq.head(401));
}

// `token[:content_type] == request.content_mime_type && token[:content_length] == request.content_length`
bool acceptable_content(const Rq& rq, const storage::DiskToken& token) {
  const auto media = req::media_type(optional_header(rq, "content-type"));
  const auto lower = [](std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
  };
  std::optional<std::string> expected;
  if (token.content_type) expected = lower(*token.content_type);
  std::optional<std::string> actual;
  if (media) actual = lower(*media);
  const auto length = compat::integer_cast(compat::strip(rq.request.header("content-length")));
  return expected == actual && length && *length == token.content_length;
}

// `ActiveStorage::DiskController#update`: the direct upload PUT.
Task<Flow<net::Response>> disk_update(Rq& rq) {
  if (auto authed = require_active_storage_authentication(rq); !authed) co_return std::unexpected(std::move(authed.error()));
  const storage::Storage& storage = *rq.app.storage;
  const auto token = storage::decode_verified_token(storage.verifier(), rq.param_str("encoded_token").value_or(""),
                                                    to_compat(rq.now()));
  if (!token) co_return rq.head(404);
  if (!acceptable_content(rq, *token)) co_return rq.head(422);
  const std::string_view body = rq.request.body;
  auto uploaded = co_await rq.ctx.offload(
      rq.app.jobs, [&] { return storage.service().upload(token->key, body, std::string_view(token->checksum)); });
  if (!uploaded) {
    // `ActiveStorage::IntegrityError`
    if (uploaded.error().message == "checksum mismatch") co_return rq.head(422);
    co_return fail_internal(uploaded.error().message);
  }
  co_return rq.head(204);
}

// A string, or a number as its text: the JavaScript of Active Storage sends `byte_size` as a number.
std::optional<std::string> text_param(const req::ParamMap& params, std::string_view key) {
  const req::Param* param = params.get(key);
  if (param == nullptr) return std::nullopt;
  switch (param->kind()) {
    case req::ParamKind::String:
    case req::ParamKind::Int:
    case req::ParamKind::UInt:
    case req::ParamKind::Double: return param->to_s();
    default: return std::nullopt;
  }
}

// A stored `created_at` as ActiveSupport::JSON writes times: ISO 8601 with milliseconds.
std::string json_time(const std::string& db_time) {
  const auto parsed = parse_db(db_time);
  return parsed ? compat::iso8601_millis(to_compat(*parsed)) : db_time;
}

// `ActiveStorage::DirectUploadsController#create`
Task<Flow<net::Response>> direct_uploads_create(Rq& rq) {
  if (auto verified = concerns::verify_authenticity_token(rq); !verified) co_return std::unexpected(std::move(verified.error()));
  if (auto authed = require_active_storage_authentication(rq); !authed) co_return std::unexpected(std::move(authed.error()));
  // `params.expect(blob: [:filename, :byte_size, :checksum, :content_type, metadata: {}])`
  const auto required = rq.params().require("blob");
  if (!required) co_return fail_with(ErrorKind::ParameterMissing, required.error().message);
  const req::ParamMap* blob_params = (*required)->as_hash();
  if (blob_params == nullptr) co_return fail_with(ErrorKind::ParameterMissing, "param is missing or the value is empty: blob");
  const auto filename = text_param(*blob_params, "filename");
  const auto checksum = text_param(*blob_params, "checksum");
  if (!filename || filename->empty() || !checksum || checksum->empty()) co_return fail_status(422);
  // Stricter than Rails, whose cast makes a byte size that is not a number 0: it is refused.
  const auto size_text = text_param(*blob_params, "byte_size");
  const auto byte_size = size_text ? compat::integer_cast(*size_text) : std::nullopt;
  if (!byte_size) co_return fail_status(422);
  // The PUT body is kept in memory, so the cap is the one of other bodies.
  if (*byte_size < 0 || *byte_size > kMaxBufferedBody) co_return fail_status(413);
  const auto content_type = text_param(*blob_params, "content_type");
  compat::json::Value metadata{compat::json::Value::Object{}};
  if (const req::Param* meta = blob_params->get("metadata"); meta != nullptr && meta->as_hash() != nullptr) {
    metadata = meta->to_json();
  }
  const storage::Storage& storage = *rq.app.storage;
  storage::NewBlob fresh;
  fresh.key = storage::generate_key();
  fresh.filename = storage::Filename(*filename);
  fresh.content_type = content_type;
  fresh.metadata = std::move(metadata);
  fresh.service_name = storage.service().name();
  fresh.byte_size = *byte_size;
  fresh.checksum = *checksum;
  std::optional<storage::Blob> blob;
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
    models::attachments::AttachmentRecords records(tx.conn());
    auto saved = records.insert_blob(fresh, to_compat(tx.now()));
    if (!saved) return std::unexpected(saved.error());
    blob = std::move(*saved);
    return {};
  });
  if (!written) co_return fail_internal(written.error().message);
  const compat::Timestamp expires_at = to_compat(rq.now()) + std::chrono::seconds(kServiceUrlsExpireIn);
  std::optional<std::string_view> type;
  if (content_type) type = std::string_view(*content_type);
  const std::string url = rq.url_for(storage.service().url_path_for_direct_upload(
      storage.verifier(), blob->key, expires_at, type, *byte_size, *checksum));
  using compat::json::Value;
  const auto nullable = [](const std::optional<std::string>& text) { return text ? Value(*text) : Value(); };
  Value::Object headers;
  headers.emplace_back("Content-Type", nullable(content_type));
  Value::Object direct;
  direct.emplace_back("url", Value(url));
  direct.emplace_back("headers", Value(std::move(headers)));
  Value::Object out;
  out.emplace_back("id", Value(blob->id));
  out.emplace_back("key", Value(blob->key));
  out.emplace_back("filename", Value(blob->filename.raw()));
  out.emplace_back("content_type", nullable(blob->content_type));
  out.emplace_back("metadata", blob->metadata);
  out.emplace_back("service_name", Value(blob->service_name));
  out.emplace_back("byte_size", Value(blob->byte_size));
  out.emplace_back("checksum", nullable(blob->checksum));
  out.emplace_back("created_at", Value(json_time(blob->created_at)));
  out.emplace_back("signed_id", Value(storage::paths::signed_blob_id(storage.verifier(), blob->id)));
  out.emplace_back("direct_upload", Value(std::move(direct)));
  co_return rq.json(200, Value(std::move(out)));
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::active_storage {

#define CF_AS_ROUTE(name)                                       \
  Task<net::Response> name(net::Ctx& c) {                       \
    return app::dispatch(c, &app::controllers::name);           \
  }
CF_AS_ROUTE(blobs_redirect)
CF_AS_ROUTE(blobs_proxy)
CF_AS_ROUTE(representations_redirect)
CF_AS_ROUTE(representations_proxy)
CF_AS_ROUTE(disk_show)
CF_AS_ROUTE(disk_update)
CF_AS_ROUTE(direct_uploads_create)
#undef CF_AS_ROUTE

}  // namespace campfire::routes::active_storage
