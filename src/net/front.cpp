// Headers of the front server. Rust: crates/kit/src/front/handler.rs, compression.rs (add_vary), conn.rs (Date).
#include "net/front.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "net/front/cache.hpp"
#include "net/front/headers.hpp"

namespace campfire::net {

namespace {

constexpr const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

void put2(char* p, int v) noexcept {
  p[0] = static_cast<char>('0' + v / 10);
  p[1] = static_cast<char>('0' + v % 10);
}

}  // namespace

std::string_view format_http_date(std::time_t when, char (&buffer)[32]) noexcept {
  std::tm tm{};
  gmtime_r(&when, &tm);
  char* p = buffer;
  for (const char* d = kDays[tm.tm_wday]; *d != '\0'; ++d) *p++ = *d;
  *p++ = ',';
  *p++ = ' ';
  put2(p, tm.tm_mday);
  p += 2;
  *p++ = ' ';
  for (const char* m = kMonths[tm.tm_mon]; *m != '\0'; ++m) *p++ = *m;
  *p++ = ' ';
  const int year = tm.tm_year + 1900;
  put2(p, year / 100);
  put2(p + 2, year % 100);
  p += 4;
  *p++ = ' ';
  put2(p, tm.tm_hour);
  p[2] = ':';
  put2(p + 3, tm.tm_min);
  p[5] = ':';
  put2(p + 6, tm.tm_sec);
  p += 8;
  for (const char* g = " GMT"; *g != '\0'; ++g) *p++ = *g;
  return {buffer, static_cast<std::size_t>(p - buffer)};
}

std::string_view http_date_now(char (&buffer)[32]) noexcept {
  const auto now = std::chrono::system_clock::now();
  return format_http_date(std::chrono::system_clock::to_time_t(now), buffer);
}

bool should_cache_request(const Request& request) noexcept {
  return front::should_cache_request(request);
}

// Rust: `suppress_bodiless_headers`. `HeaderMap::remove` moves the last header into the slot of the
// removed one, so e.g. a 304's "content-length" gives its slot to the "vary" that the compression
// appended after it.
void suppress_bodiless_headers(Response& response) {
  const int status = response.status;
  if (status == 304) {
    front::remove_header(response, "content-type");
    front::remove_header(response, "content-length");
    front::remove_header(response, "transfer-encoding");
  } else if ((status >= 100 && status < 200) || status == 204) {
    front::remove_header(response, "content-length");
    front::remove_header(response, "transfer-encoding");
  }
}

void apply_front_headers(const Request& request, Response& response) {
  if (response.status >= 100 && response.status < 200) return;
  if (!should_cache_request(request)) {
    // Bypass: "x-cache" first, then Accept-Encoding goes before the values that the app set.
    response.set("x-cache", "bypass");
    std::vector<std::string_view> existing;
    for (const Header& h : response.headers) {
      if (iequals(h.name, "vary")) existing.push_back(h.value);
    }
    // Rust: `headers.remove(VARY)` (http::HeaderMap) moves the last header into the slot of the first
    // "vary", so "x-cache" lands there when the app set a "vary" before it.
    {
      auto& headers = response.headers;
      const auto first =
          std::find_if(headers.begin(), headers.end(), [](const Header& h) { return iequals(h.name, "vary"); });
      if (first != headers.end()) {
        headers.erase(std::remove_if(first + 1, headers.end(), [](const Header& h) { return iequals(h.name, "vary"); }),
                      headers.end());
      }
    }
    for (std::size_t i = 0; i < response.headers.size(); ++i) {
      if (!iequals(response.headers[i].name, "vary")) continue;
      response.headers[i] = response.headers.back();
      response.headers.pop_back();
      break;
    }
    response.add("vary", "Accept-Encoding");
    for (const std::string_view value : existing) response.add("vary", value);
  } else {
    // Miss: `CacheHandler` adds "x-cache" first, then `compression.apply` appends "Accept-Encoding"
    // (unless the app already varies). The 304's "content-length" then gives its slot to it below.
    response.set("x-cache", "miss");
    if (!response.has("vary")) response.add("vary", "Accept-Encoding");
  }
  // The date goes on last, after the headers a bodiless status drops, as the Rust connection does.
  suppress_bodiless_headers(response);
  if (!response.has("date")) {
    char buffer[32];
    response.add_copy("date", http_date_now(buffer));
  }
}

}  // namespace campfire::net
