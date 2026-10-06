// Headers of the front server. Rust: crates/kit/src/front/handler.rs, compression.rs (add_vary), conn.rs (Date).
#include "net/front.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace campfire::net {

namespace {

constexpr std::size_t kMaxCacheableUri = 2048;  // the Rust README: "skip URIs over 2 KB"

constexpr const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

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
  const bool allowed = request.method == Method::Get || request.method == Method::Head;
  const bool upgrade = request.header("connection") == "Upgrade" || request.header("upgrade") == "websocket";
  const bool range = !request.header("range").empty();
  return allowed && !upgrade && !range && request.target.size() <= kMaxCacheableUri;
}

void suppress_bodiless_headers(Response& response) {
  const int status = response.status;
  if (status == 304) {
    response.erase("content-type");
    response.erase("content-length");
    response.erase("transfer-encoding");
  } else if ((status >= 100 && status < 200) || status == 204) {
    response.erase("content-length");
    response.erase("transfer-encoding");
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
    response.erase("vary");
    response.add("vary", "Accept-Encoding");
    for (const std::string_view value : existing) response.add("vary", value);
  } else {
    // Miss: the front keeps a "vary" of the app, and adds one if there is none.
    if (!response.has("vary")) response.add("vary", "Accept-Encoding");
    response.set("x-cache", "miss");
  }
  if (!response.has("date")) {
    char buffer[32];
    response.add_copy("date", http_date_now(buffer));
  }
  suppress_bodiless_headers(response);
}

}  // namespace campfire::net
