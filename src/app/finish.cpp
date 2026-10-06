// Finishing a response: what Rails does after the action. Rails: ActionController::Metal (commit of the
// flash and the session), ActionDispatch::Cookies, Rack::ETag, Rack::ConditionalGet, Rack::Deflater,
// ActionDispatch::RequestId, Rack::Runtime, ActionDispatch::SSL.
// Rust: crates/kit/src/ctx.rs (finish), adapter.rs (into_axum, rails_middleware), deflater.rs.
//
// The order of the headers is part of the bytes. The Rust port uses http::HeaderMap, which removes a
// header by moving the last one into its place. `swap_remove` below does the same.
#include <algorithm>

#include "app/compress.hpp"
#include "app/errors.hpp"
#include "app/page_cache.hpp"
#include "app/rails.hpp"
#include "app/rq.hpp"
#include "core/log.hpp"

namespace campfire::app {

namespace {

constexpr std::string_view kDefaultHeaders[][2] = {
    {"x-frame-options", "SAMEORIGIN"},
    {"x-xss-protection", "0"},
    {"x-content-type-options", "nosniff"},
    {"x-permitted-cross-domain-policies", "none"},
    {"referrer-policy", "strict-origin-when-cross-origin"},
};

bool bodiless(int status) {
  return (status >= 100 && status < 200) || status == 204 || status == 205 || status == 304;
}

// HeaderMap::remove: the last header takes the place of the removed one.
void swap_remove(net::Response& r, std::string_view name) {
  auto& h = r.headers;
  for (std::size_t i = 0; i < h.size(); ++i) {
    if (net::iequals(h[i].name, name)) {
      h[i] = h.back();
      h.pop_back();
      return;
    }
  }
}

// HeaderMap::insert: replaces the value in place, or appends.
void replace_or_add(net::Response& r, std::string_view name, std::string_view value) {
  for (net::Header& h : r.headers) {
    if (net::iequals(h.name, name)) {
      h.value = value;
      return;
    }
  }
  r.add(name, value);
}

bool has_word(std::string_view haystack, std::string_view word) {
  const auto is_word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
  for (std::size_t at = haystack.find(word); at != std::string_view::npos; at = haystack.find(word, at + 1)) {
    const bool before = at > 0 && is_word(haystack[at - 1]);
    const bool after = at + word.size() < haystack.size() && is_word(haystack[at + word.size()]);
    if (!before && !after) return true;
  }
  return false;
}

// `Rack::ETag`: the first 32 hex digits of the SHA-256 of the body, which can be in many chunks.
std::string digest_etag(const net::Response& response) {
  Sha256Etag sha;
  iovec iov[16];
  std::size_t skip = 0;
  while (true) {
    const std::size_t n = response.body_iovecs(iov, skip);
    if (n == 0) break;
    for (std::size_t i = 0; i < n; ++i) {
      sha.update(iov[i].iov_base, iov[i].iov_len);
      skip += iov[i].iov_len;
    }
  }
  return sha.etag();
}

std::string gather_body(const net::Response& response) {
  std::string bytes;
  bytes.reserve(response.body_size());
  iovec iov[16];
  std::size_t skip = 0;
  while (true) {
    const std::size_t n = response.body_iovecs(iov, skip);
    if (n == 0) break;
    for (std::size_t i = 0; i < n; ++i) {
      bytes.append(static_cast<const char*>(iov[i].iov_base), iov[i].iov_len);
      skip += iov[i].iov_len;
    }
  }
  return bytes;
}

}  // namespace

// Friend of Rq: the steps of `Rq::finish`.
class Finisher {
 public:
  static net::Response run(Rq& rq, Flow<net::Response> result);

 private:
  static net::Response error_response(Rq& rq, const HttpError& error);
  static Status commit(Rq& rq, net::Response& response);
  static void cache_headers(Rq& rq, net::Response& response);
  static void rack_etag(Rq& rq, net::Response& response);
  static void conditional_get(Rq& rq, net::Response& response);
  static net::Response outer(Rq& rq, net::Response response);
};

net::Response Finisher::error_response(Rq& rq, const HttpError& error) {
  if (error.status() >= 500) {
    log_error("request failed: {} {} -> {}: {}", rq.request.method_text, rq.request.path, error.status(),
              error.message);
  } else {
    log_info("request rejected: {} {} -> {}: {}", rq.request.method_text, rq.request.path, error.status(),
             error.message);
  }
  if (!rq.formats_) rq.formats_ = req::formats(rq.negotiation_input());
  req::Format format = nullptr;
  if (*rq.formats_ && !(*rq.formats_)->empty()) format = (*rq.formats_)->front();
  return render_error(rq.ctx, error.status(), format, rq.is_head());
}

Status Finisher::commit(Rq& rq, net::Response& response) {
  if (rq.flash_) {
    const bool has_flash_key = rq.session().contains_key("flash");
    if (!rq.flash_->empty() || has_flash_key) {
      if (auto value = rq.flash_->to_session_value()) {
        rq.session().insert("flash", std::move(*value));
      } else {
        rq.session().insert("flash", compat::json::Value(nullptr));
      }
    }
    rq.flash_.reset();
  }
  if (rq.session_.is_loaded() && rq.session_.contains_key("flash") && rq.session_.get("flash") == nullptr) {
    rq.session_.remove("flash");
  }
  if (auto saved = rq.session_.commit(*rq.cookies_, rq.now()); !saved) return saved;
  for (const std::string& cookie : rq.cookies_->set_cookie_headers(rq.info.ssl(), rq.info.host())) {
    response.add_copy("set-cookie", cookie);
  }
  return {};
}

void Finisher::cache_headers(Rq& rq, net::Response& response) {
  if (response.has("cache-control")) return;
  CacheControl cc = rq.cache_control;
  if (cc.empty() && (response.has("etag") || response.has("last-modified"))) {
    cc.max_age = 0;
    cc.must_revalidate = true;
  }
  if (!cc.empty()) response.add_copy("cache-control", cc.to_header());
}

void Finisher::rack_etag(Rq& rq, net::Response& response) {
  bool digested = false;
  const bool skip = rq.live_response || response.has("etag") || response.has("last-modified");
  if ((response.status == 200 || response.status == 201) && !skip && response.body_size() != 0) {
    const std::string etag = rq.page_entry_ ? rq.page_entry_->etag : digest_etag(response);
    response.add_copy("etag", etag);
    digested = true;
  }
  if (!response.has("cache-control")) {
    response.add("cache-control", digested ? "max-age=0, private, must-revalidate" : "no-cache");
  }
}

void Finisher::conditional_get(Rq& rq, net::Response& response) {
  if (!(rq.is_get() || rq.is_head()) || response.status != 200) return;
  if (rq.is_fresh(response.get("etag"), response.get("last-modified"))) {
    response.status = 304;
    swap_remove(response, "content-type");
    swap_remove(response, "content-length");
    response.body_view({});
  }
}

// `into_axum`, then `Rack::Deflater`, then the request id and the runtime (Rust: adapter.rs, deflater.rs).
net::Response Finisher::outer(Rq& rq, net::Response response) {
  const bool app_set_length = response.has("content-length");
  if (response.body_size() != 0) {
    char* digits = static_cast<char*>(rq.arena().allocate(24, 1));
    const auto text = std::to_string(response.body_size());
    std::copy(text.begin(), text.end(), digits);
    replace_or_add(response, "content-length", {digits, text.size()});
  }
  // `should_deflate?`
  bool deflate = !(response.status >= 100 && response.status < 200) && response.status != 204 && response.status != 304;
  if (deflate && has_word(response.get("cache-control"), "no-transform")) deflate = false;
  if (deflate && response.has("content-encoding") && !has_word(response.get("content-encoding"), "identity"))
    deflate = false;
  if (deflate && app_set_length && response.get("content-length") == "0") deflate = false;
  // Rack::Deflater: a client that refuses gzip and identity gets a 406 in place of the response.
  if (deflate && refuses_every_encoding(rq.request)) return not_acceptable(rq.ctx);
  bool vary_missing = false;
  if (deflate) {
    // `Vary: Accept-Encoding`: added to an existing header in place. Without one, it goes after the tail.
    if (response.has("vary")) {
      std::string_view value = response.get("vary");
      std::vector<std::string> tokens;
      bool has_encoding = false;
      while (!value.empty()) {
        const std::size_t comma = value.find(',');
        std::string_view token = value.substr(0, comma);
        value = comma == std::string_view::npos ? std::string_view{} : value.substr(comma + 1);
        while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
        while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
        if (token == "*" || net::iequals(token, "accept-encoding")) has_encoding = true;
        tokens.emplace_back(token);
      }
      if (!has_encoding) {
        tokens.emplace_back("Accept-Encoding");
        std::string joined;
        for (std::size_t i = 0; i < tokens.size(); ++i) joined += (i != 0 ? "," : "") + tokens[i];
        replace_or_add(response, "vary", rq.arena().copy(joined));
      }
    } else {
      vary_missing = true;
    }
    if (wants_gzip(rq.request)) {
      std::shared_ptr<const std::string> gz;
      if (rq.page_entry_ && response.body_size() == rq.page_entry_->identity.size()) {
        gz = std::shared_ptr<const std::string>(rq.page_entry_, &rq.page_entry_->gzip);
      } else if (rq.is_head()) {
        gz = std::make_shared<const std::string>();
      } else {
        gz = std::make_shared<const std::string>(gzip_compress(gather_body(response)));
      }
      // The new header goes where `content-length` was (HeaderMap::insert, then remove). With no
      // `content-length`, it goes before the tail, as the Rust port does.
      bool placed = false;
      for (net::Header& h : response.headers) {
        if (net::iequals(h.name, "content-length")) {
          h = {"content-encoding", "gzip"};
          placed = true;
          break;
        }
      }
      if (!placed) response.add("content-encoding", "gzip");
      response.chunked = true;
      response.body_shared(gz, *gz);
    }
  }
  // `into_axum` gives the Rust 304 an empty body with a `content-length: 0` before the request id
  // and the runtime. `apply_front_headers` then removes it (`HeaderMap::remove`), which moves the
  // last header ("vary") into its slot. Add it here so the front can do the same. The wire drops
  // it for a bodiless status (see `net::Wire`).
  if (response.status == 304 && !response.has("content-length")) response.add("content-length", "0");
  add_rails_tail(rq.ctx, response);
  // ActionDispatch::SSL is inside Rack::Deflater: its HSTS header comes before a "vary" that the
  // deflater adds at the end.
  if (rq.app.proxy.force_ssl && rq.info.ssl()) {
    response.add_copy("strict-transport-security", rq.app.proxy.hsts);
    for (net::Header& h : response.headers) {
      if (!net::iequals(h.name, "set-cookie")) continue;
      std::string_view rest = h.value;
      bool secure = false;
      bool first = true;
      while (!rest.empty()) {
        const std::size_t semi = rest.find(';');
        std::string_view attr = rest.substr(0, semi);
        rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
        if (first) {
          first = false;
          continue;
        }
        while (!attr.empty() && attr.front() == ' ') attr.remove_prefix(1);
        if (net::iequals(attr, "secure")) secure = true;
      }
      if (!secure) h.value = rq.arena().copy(std::string(h.value) + "; secure");
    }
  }
  if (vary_missing) response.add("vary", "Accept-Encoding");
  return response;
}

net::Response Finisher::run(Rq& rq, Flow<net::Response> result) {
  std::optional<net::Response> out;
  bool failed = false;
  if (result) {
    out.emplace(std::move(*result));
  } else if (auto* halted = std::get_if<Halt>(&result.error())) {
    out.emplace(std::move(halted->response));
  } else {
    out.emplace(error_response(rq, std::get<HttpError>(result.error())));
    failed = true;
  }
  if (!failed) {
    for (const net::Header& h : rq.staged_) {
      if (!out->has(h.name)) out->add(h.name, h.value);
    }
    // `verify_same_origin_request`: a GET that renders JavaScript for a request that is not XHR.
    const std::string_view type = out->get("content-type");
    const std::string_view with = rq.request.header("x-requested-with");
    const bool xhr = with.find("XMLHttpRequest") != std::string_view::npos;
    if (rq.marked_for_same_origin_verification && !xhr &&
        (type.starts_with("text/javascript") || type.starts_with("application/javascript"))) {
      const HttpError error{ErrorKind::InvalidCrossOriginRequest, 0, "invalid cross-origin request"};
      out.reset();
      out.emplace(error_response(rq, error));
      failed = true;
    }
  }
  if (!failed) {
    if (auto saved = commit(rq, *out); !saved) {
      const HttpError error{ErrorKind::CookieOverflow, 0, saved.error().message};
      out.reset();
      out.emplace(error_response(rq, error));
      failed = true;
    }
  }
  if (!failed) {
    cache_headers(rq, *out);
    if (!rq.live_response) {
      for (const auto& [name, value] : kDefaultHeaders) {
        if (!out->has(name)) out->add(name, value);
      }
    }
    if (!out->has("content-type") && !bodiless(out->status)) out->add("content-type", "text/html; charset=utf-8");
    rack_etag(rq, *out);
    conditional_get(rq, *out);
  }
  return outer(rq, std::move(*out));
}

net::Response Rq::finish(Flow<net::Response> result) {
  return Finisher::run(*this, std::move(result));
}

}  // namespace campfire::app
