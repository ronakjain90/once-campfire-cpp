// libFuzzer target: the request inputs that the app reads before a controller runs: the route
// match on the real route table, the user agent, the Range header of the file server, and the
// private network check. Rails: config/routes.rb, useragent gem, Rack::Files.
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "app/file_server.hpp"
#include "app/network_guard.hpp"
#include "app/routes.hpp"
#include "app/user_agent.hpp"
#include "richtext/uri.hpp"

namespace {

const std::filesystem::path& sample_file() {
  static const std::filesystem::path path = [] {
    auto file = std::filesystem::temp_directory_path() / ("cf_range_fuzz_" + std::to_string(::getpid()));
    std::ofstream(file, std::ios::binary) << std::string(1000, 'x');
    return file;
  }();
  return path;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace campfire;
  if (size == 0) return 0;
  const std::string_view text(reinterpret_cast<const char*>(data) + 1, size - 1);
  const auto& table = app::routes();
  for (std::size_t m = 0; m < net::kMethodCount; ++m) {
    const auto match = net::match_route(table, static_cast<net::Method>(m), text);
    if (match) (void)match.params.size();
  }
  const auto agent = app::ua::parse(text);
  (void)agent.try_browser();
  (void)agent.try_version();
  (void)agent.try_platform();
  (void)agent.try_os();
  (void)agent.try_mobile();
  (void)agent.is_bot();
  (void)app::blocked_address(text.substr(0, data[0] % 2 ? 4 : 16));

  // A URL that the unfurl or a redirect gives goes into a request line and a Host header: no byte of
  // the host, the path or the query may be a space or a control byte (request splitting).
  if (const auto uri = richtext::parse_uri(text); uri && uri->is_http()) {
    for (const auto* part : {&uri->host, &uri->path, &uri->query}) {
      if (!part->has_value()) continue;
      for (const unsigned char c : **part) {
        if (c <= 0x20 || c == 0x7F) std::abort();
      }
    }
  }
  const app::FileRequest request{"GET", text, std::nullopt};
  const auto served = app::serve_file(request, sample_file(), std::nullopt, std::nullopt);
  // The ranges together never hold more than the file (else 416). A multipart answer adds at most 99
  // headings of about 200 bytes.
  if (served && served->body.size() > 1000 + 100 * 256) std::abort();
  const app::FileRequest conditional{"GET", std::nullopt, text};
  (void)app::serve_file(conditional, sample_file(), std::nullopt, std::nullopt);
  return 0;
}
