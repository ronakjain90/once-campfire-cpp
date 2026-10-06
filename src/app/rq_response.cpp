// Response builders of Rq. Rails: ActionController::Rendering, Redirecting, DataStreaming, ConditionalGet.
// Rust: crates/kit/src/ctx.rs, crates/kit/src/response.rs.
#include <algorithm>
#include <fstream>
#include <sstream>

#include "app/page_cache.hpp"
#include "app/rq.hpp"
#include "compat/content_disposition.hpp"
#include "core/time_format.hpp"

namespace campfire::app {

bool CacheControl::empty() const noexcept {
  return !max_age && !is_public && !is_private && !must_revalidate && !no_cache && !no_store &&
         !stale_while_revalidate && !immutable;
}

std::string CacheControl::to_header() const {
  std::vector<std::string> parts;
  if (no_store) {
    if (is_private) parts.emplace_back("private");
    parts.emplace_back("no-store");
  } else if (no_cache) {
    if (is_public) parts.emplace_back("public");
    parts.emplace_back("no-cache");
  } else {
    if (max_age) parts.push_back("max-age=" + std::to_string(*max_age));
    parts.emplace_back(is_public ? "public" : "private");
    if (must_revalidate) parts.emplace_back("must-revalidate");
    if (stale_while_revalidate) parts.push_back("stale-while-revalidate=" + std::to_string(*stale_while_revalidate));
    if (immutable) parts.emplace_back("immutable");
  }
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) out += (i != 0 ? ", " : "") + parts[i];
  return out;
}

net::Response Rq::render_as(int status, std::string_view content_type, Out&& body) {
  net::Response response = ctx.response(status);
  response.add_copy("content-type", content_type);
  response.body_out(std::move(body));
  // `_set_vary_header`: `Vary: Accept` when the format came from the Accept header.
  if (!response.has("vary") && req::should_apply_vary_header(negotiation_input())) response.add("vary", "Accept");
  return response;
}

net::Response Rq::html(int status, Out&& body) {
  return render_as(status, "text/html; charset=utf-8", std::move(body));
}

net::Response Rq::respond_page(std::shared_ptr<const PageEntry> entry, int status) {
  net::Response response = ctx.response(status);
  response.add_copy("content-type", entry->content_type);
  if (req::should_apply_vary_header(negotiation_input())) response.add("vary", "Accept");
  response.body_shared(entry, entry->identity);
  page_entry_ = std::move(entry);
  return response;
}

net::Response Rq::turbo_stream(Out&& body) {
  return render_as(200, "text/vnd.turbo-stream.html; charset=utf-8", std::move(body));
}

net::Response Rq::json(int status, const compat::json::Value& value) {
  const std::string text = compat::json::encode(value);
  Out out(ctx.resource());
  out.append_raw(text);
  return render_as(status, "application/json; charset=utf-8", std::move(out));
}

net::Response Rq::head(int status) {
  net::Response response = ctx.response(status);
  if (!((status >= 100 && status < 200) || status == 204 || status == 205 || status == 304)) {
    response.add_copy("content-type", rendered_format()->string);
  }
  return response;
}

namespace {

bool is_absolute_url(std::string_view location) {
  if (location.starts_with("//")) return true;
  if (location.empty() || !std::isalpha(static_cast<unsigned char>(location.front()))) return false;
  const std::size_t colon = location.find(':');
  if (colon == std::string_view::npos) return false;
  return std::all_of(location.begin(), location.begin() + static_cast<std::ptrdiff_t>(colon), [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.';
  });
}

// The host of an absolute or protocol-relative URL. Nothing for a path.
std::optional<std::string> url_host(std::string_view url) {
  std::string_view rest;
  if (url.starts_with("//")) {
    rest = url.substr(2);
  } else {
    const std::size_t sep = url.find("://");
    if (sep == std::string_view::npos || sep == 0) return std::nullopt;
    const std::string_view scheme = url.substr(0, sep);
    if (!std::all_of(scheme.begin(), scheme.end(), [](char c) {
          return std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.';
        })) {
      return std::nullopt;
    }
    rest = url.substr(sep + 3);
  }
  const std::string_view authority = rest.substr(0, rest.find_first_of("/?#"));
  const std::size_t at = authority.rfind('@');
  const std::string_view host_port = at == std::string_view::npos ? authority : authority.substr(at + 1);
  if (host_port.starts_with('[')) return std::string("[") + std::string(host_port.substr(1, host_port.find(']') - 1)) + "]";
  return std::string(host_port.substr(0, host_port.find(':')));
}

bool iequals_str(const std::string& a, const std::string& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

}  // namespace

Flow<net::Response> Rq::redirect_to(std::string_view location, RedirectOptions options) {
  if (options.notice) flash().set("notice", compat::json::Value(*options.notice));
  if (options.alert) flash().set("alert", compat::json::Value(*options.alert));
  std::string url;
  if (is_absolute_url(location)) {
    url = std::string(location);
  } else {
    if (!location.empty() && location.front() != '/' && location.front() != '?') {
      return fail_with(ErrorKind::UnsafeRedirect, "Path relative URL redirect detected: " + std::string(location));
    }
    url = std::string(info.protocol()) + info.host_with_port() + std::string(location);
  }
  std::erase_if(url, [](char c) { return c == '\0' || c == '\r' || c == '\n'; });
  if (std::any_of(url.begin(), url.end(), [](char c) {
        const auto b = static_cast<unsigned char>(c);
        return b <= 0x08 || (b >= 0x0A && b <= 0x1F);
      })) {
    return fail_with(ErrorKind::UnsafeRedirect, "The redirect URL contains illegal characters");
  }
  if (!options.allow_other_host) {
    const auto host = url_host(url);
    const bool allowed = host ? iequals_str(*host, info.host()) : (url.starts_with('/') && !url.starts_with("//"));
    if (!allowed) return fail_with(ErrorKind::UnsafeRedirect, "Unsafe redirect to " + url);
  }
  net::Response response = ctx.response(options.status);
  response.add("content-type", "text/html; charset=utf-8");
  response.add_copy("location", url);
  return response;
}

net::Response Rq::send_data(std::string_view bytes, std::string_view content_type,
                            std::optional<std::string_view> disposition, std::optional<std::string_view> filename,
                            int status) {
  net::Response response = ctx.response(status);
  response.add_copy("content-type", content_type);
  if (disposition) {
    response.add_copy("content-disposition",
                      filename ? compat::content_disposition(*disposition, *filename) : std::string(*disposition));
  }
  response.add("content-transfer-encoding", "binary");
  response.body_view(copy(bytes));
  return response;
}

Flow<net::Response> Rq::send_file(const std::string& path, std::string_view content_type,
                                  std::optional<std::string_view> disposition,
                                  std::optional<std::string_view> filename) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail_internal("Cannot read file " + path);
  std::ostringstream bytes;
  bytes << in.rdbuf();
  return send_data(bytes.str(), content_type, disposition, filename);
}

bool Rq::is_fresh(std::string_view etag, std::string_view last_modified) const {
  if (request.has_header("if-none-match")) {
    if (etag.empty()) return false;
    std::string_view list = request.header("if-none-match");
    while (!list.empty()) {
      const std::size_t comma = list.find(',');
      std::string_view part = list.substr(0, comma);
      list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
      while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) part.remove_prefix(1);
      while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) part.remove_suffix(1);
      if (part == etag || part == "*") return true;
    }
    return false;
  }
  if (const auto since = parse_httpdate(request.header("if-modified-since"))) {
    const auto modified = parse_httpdate(last_modified);
    return modified && *since >= *modified;
  }
  return false;
}

std::optional<net::Response> Rq::fresh_when(const Freshness& freshness) {
  cache_control.no_store = false;
  const bool etagged = freshness.strong_etag || freshness.etag || freshness.template_digest;
  if (etagged) {
    const bool weak = !freshness.strong_etag;
    std::vector<std::string> parts;
    if (freshness.strong_etag) parts.push_back(*freshness.strong_etag);
    else if (freshness.etag) parts.push_back(*freshness.etag);
    if (is_turbo_frame_request()) parts.emplace_back("frame");
    if (freshness.template_digest) parts.push_back(*freshness.template_digest);
    // ETagWithFlash: a flash changes the validator.
    std::string joined;
    for (std::size_t i = 0; i < parts.size(); ++i) joined += (i != 0 ? "/" : "") + parts[i];
    const std::string tag = body_etag(joined);  // `W/"<32 hex>"`
    const std::string hex = tag.substr(3, 32);
    set_header("etag", weak ? "W/\"" + hex + "\"" : "\"" + hex + "\"");
  }
  if (freshness.last_modified) set_header("last-modified", format_httpdate(*freshness.last_modified));
  if (freshness.is_public) cache_control.is_public = true;
  if (is_fresh(staged_header("etag"), staged_header("last-modified"))) return head(304);
  return std::nullopt;
}

}  // namespace campfire::app
