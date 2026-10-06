// The cookie jar (Rails: action_dispatch/middleware/cookies.rb, Rack::Utils cookie functions;
// Rust: crates/kit/src/cookies.rs).
#include "req/cookie.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_set>

#include "compat/cookies.hpp"
#include "compat/time.hpp"
#include "core/time_format.hpp"
#include "req/query.hpp"

namespace campfire::req {

namespace {

compat::Timestamp to_compat(Timestamp t) {
  return compat::Timestamp(std::chrono::nanoseconds(t.seconds * 1'000'000'000LL + t.nanos));
}

Timestamp from_compat(compat::Timestamp t) { return Timestamp::from_nanos(t.time_since_epoch().count()); }

std::string_view same_site_attribute(std::optional<SameSite> same_site) {
  if (!same_site) return "";
  switch (*same_site) {
    case SameSite::Lax: return "; samesite=lax";
    case SameSite::Strict: return "; samesite=strict";
    case SameSite::None: return "; samesite=none";
  }
  return "";
}

template <class T>
void upsert(std::vector<std::pair<std::string, T>>& list, std::string_view name, T value) {
  auto it = std::ranges::find_if(list, [&](const auto& e) { return e.first == name; });
  if (it != list.end()) {
    it->second = std::move(value);
  } else {
    list.emplace_back(std::string(name), std::move(value));
  }
}

Status check_for_overflow(std::string_view name, std::string_view value) {
  const std::size_t total = name.size() + value.size();
  if (total > kMaxCookieSize) {
    return fail(Errc::InvalidArgument,
                std::string(name) + " cookie overflowed with size " + std::to_string(total) + " bytes");
  }
  return {};
}

}  // namespace

std::vector<std::pair<std::string, std::string>> parse_cookie_header(std::string_view header) {
  std::vector<std::pair<std::string, std::string>> cookies;
  // A set of seen names keeps a header of tens of thousands of cookies linear.
  std::unordered_set<std::string_view> seen;
  bool first = true;
  std::size_t pos = 0;
  while (pos <= header.size()) {
    std::size_t end = header.find(';', pos);
    if (end == std::string_view::npos) end = header.size();
    std::string_view part = header.substr(pos, end - pos);
    pos = end + 1;
    if (!first) part.remove_prefix(std::min(part.find_first_not_of(' '), part.size()));
    first = false;
    if (part.empty()) continue;
    const std::size_t eq = part.find('=');
    const std::string_view key = part.substr(0, eq);
    const std::string_view raw = eq == std::string_view::npos ? std::string_view{} : part.substr(eq + 1);
    if (!seen.insert(key).second) continue;
    auto decoded = decode_www_form_component(raw);
    if (decoded && compat::json::valid_utf8(*decoded)) {
      cookies.emplace_back(std::string(key), std::move(*decoded));
    } else {
      cookies.emplace_back(std::string(key), std::string(raw));
    }
  }
  return cookies;
}

std::string set_cookie_header(std::string_view name, const Cookie& cookie) {
  std::string header(name);
  header += '=';
  header += compat::cookies::escape(cookie.value);
  if (cookie.domain) header += "; domain=" + *cookie.domain;
  header += "; path=" + cookie.path;
  if (cookie.expires) header += "; expires=" + format_httpdate(*cookie.expires);
  if (cookie.secure) header += "; secure";
  if (cookie.httponly) header += "; httponly";
  header += same_site_attribute(cookie.same_site);
  if (cookie.partitioned) header += "; partitioned";
  return header;
}

std::string delete_cookie_header(std::string_view name, const DeleteOptions& options) {
  std::string header(name);
  header += '=';
  if (options.domain) header += "; domain=" + *options.domain;
  header += "; path=" + options.path + "; max-age=0; expires=" + format_httpdate(Timestamp::from_seconds(0));
  header += same_site_attribute(options.same_site);
  return header;
}

CookieJar::CookieJar(const std::vector<std::string_view>& headers, const compat::Secrets& secrets, const Clock& clock)
    : secrets_(&secrets), clock_(&clock) {
  std::unordered_set<std::string> seen;
  for (const std::string_view header : headers) {
    for (auto& [name, value] : parse_cookie_header(header)) {
      if (seen.insert(name).second) cookies_.emplace_back(std::move(name), std::move(value));
    }
  }
}

std::optional<std::string_view> CookieJar::get(std::string_view name) const {
  const auto it = std::ranges::find_if(cookies_, [&](const auto& e) { return e.first == name; });
  if (it == cookies_.end()) return std::nullopt;
  return std::string_view(it->second);
}

std::optional<std::string> CookieJar::signed_value(std::string_view name) const {
  const auto raw = get(name);
  if (!raw) return std::nullopt;
  return compat::cookies::verify_signed(*secrets_, name, *raw, to_compat(clock_->now()));
}

std::optional<compat::json::Value> CookieJar::encrypted_value(std::string_view name) const {
  const auto raw = get(name);
  if (!raw) return std::nullopt;
  return compat::cookies::decrypt(*secrets_, name, *raw, to_compat(clock_->now()));
}

void CookieJar::resolve_expiry(Cookie& cookie) const {
  if (cookie.permanent) cookie.expires = from_compat(compat::permanent_expires_at(to_compat(clock_->now())));
}

void CookieJar::write_value(std::string_view name, Cookie cookie) {
  if (get(name) != std::string_view(cookie.value) || cookie.expires) {
    upsert(cookies_, name, cookie.value);
    upsert(set_cookies_, name, std::move(cookie));
    std::erase_if(delete_cookies_, [&](const auto& e) { return e.first == name; });
  }
}

void CookieJar::set(std::string_view name, Cookie cookie) {
  resolve_expiry(cookie);
  write_value(name, std::move(cookie));
}

Status CookieJar::set_signed(std::string_view name, Cookie cookie) {
  resolve_expiry(cookie);
  std::optional<compat::Timestamp> expires;
  if (cookie.expires) expires = to_compat(*cookie.expires);
  cookie.value = compat::cookies::sign(*secrets_, name, cookie.value, expires);
  if (auto status = check_for_overflow(name, cookie.value); !status) return status;
  write_value(name, std::move(cookie));
  return {};
}

Status CookieJar::set_encrypted(std::string_view name, const compat::json::Value& value, Cookie cookie) {
  resolve_expiry(cookie);
  std::optional<compat::Timestamp> expires;
  if (cookie.expires) expires = to_compat(*cookie.expires);
  cookie.value = compat::cookies::encrypt(*secrets_, name, value, expires);
  if (auto status = check_for_overflow(name, cookie.value); !status) return status;
  write_value(name, std::move(cookie));
  return {};
}

void CookieJar::remove(std::string_view name, const DeleteOptions& options) {
  const auto it = std::ranges::find_if(cookies_, [&](const auto& e) { return e.first == name; });
  if (it == cookies_.end()) return;
  cookies_.erase(it);
  upsert(delete_cookies_, name, options);
}

bool CookieJar::is_deleted(std::string_view name) const {
  return std::ranges::any_of(delete_cookies_, [&](const auto& e) { return e.first == name; });
}

std::vector<std::string> CookieJar::set_cookie_headers(bool ssl, std::string_view host) const {
  std::vector<std::string> headers;
  const bool onion = host.ends_with(".onion");
  for (const auto& [name, cookie] : set_cookies_) {
    if (ssl || !cookie.secure || onion) headers.push_back(set_cookie_header(name, cookie));
  }
  for (const auto& [name, options] : delete_cookies_) headers.push_back(delete_cookie_header(name, options));
  return headers;
}

}  // namespace campfire::req
