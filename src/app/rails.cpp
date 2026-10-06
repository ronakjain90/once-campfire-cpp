// Headers that Rails middleware adds. Rails: ActionDispatch::RequestId, Rack::Runtime, Rack::Deflater.
#include "app/rails.hpp"

#include <sys/random.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "net/front/static_files.hpp"

namespace campfire::app {

namespace {

// ActionDispatch::RequestId#make_request_id: keep \w, "-" and "@" of the client value, at most 255.
std::string_view clean_request_id(net::Ctx& ctx, std::string_view given) {
  char* out = static_cast<char*>(ctx.arena().allocate(given.size() + 1, 1));
  std::size_t n = 0;
  for (const char c : given) {
    const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (word || c == '-' || c == '@') out[n++] = c;
    if (n == 255) break;
  }
  return {out, n};
}

std::string_view new_uuid(net::Ctx& ctx) {
  unsigned char b[16];
  if (getrandom(b, sizeof b, 0) != static_cast<ssize_t>(sizeof b)) std::abort();
  b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);
  b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);
  char* out = static_cast<char*>(ctx.arena().allocate(37, 1));
  static constexpr char kHex[] = "0123456789abcdef";
  std::size_t n = 0;
  for (std::size_t i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out[n++] = '-';
    out[n++] = kHex[b[i] >> 4];
    out[n++] = kHex[b[i] & 15];
  }
  return {out, n};
}

}  // namespace

void add_rails_tail(net::Ctx& ctx, net::Response& response) {
  std::string_view id = clean_request_id(ctx, ctx.request().header("x-request-id"));
  if (id.empty()) id = new_uuid(ctx);
  response.add("x-request-id", id);
  char* runtime = static_cast<char*>(ctx.arena().allocate(32, 1));
  const int n = std::snprintf(runtime, 32, "%.6f", ctx.elapsed_seconds());
  response.add("x-runtime", {runtime, static_cast<std::size_t>(n)});
}

bool wants_gzip(const net::Request& request) noexcept {
  return net::front::choose_deflater_encoding(request.header("accept-encoding")) == net::front::DeflaterChoice::Gzip;
}

bool refuses_every_encoding(const net::Request& request) {
  return net::front::choose_deflater_encoding(request.header("accept-encoding")) == net::front::DeflaterChoice::None;
}

net::Response not_acceptable(net::Ctx& ctx) {
  const net::Request& request = ctx.request();
  const std::string message = "An acceptable encoding for the requested resource " + std::string(request.target) +
                              " could not be found.";
  net::Response response = ctx.response(406);
  response.add("content-type", "text/plain");
  response.add_copy("content-length", std::to_string(message.size()));
  response.body_view(ctx.arena().copy(message));
  return response;
}

}  // namespace campfire::app
