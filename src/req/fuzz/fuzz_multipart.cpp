// libFuzzer target: multipart bodies fed in chunks (Rack: Rack::Multipart::Parser).
// The input is: a boundary line, a chunk size byte, then the body.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <string>
#include <string_view>

#include "req/multipart.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace campfire::req;
  std::string_view input(reinterpret_cast<const char*>(data), size);
  const std::size_t eol = input.find('\n');
  if (eol == std::string_view::npos || eol == 0 || eol + 2 > input.size()) return 0;
  const std::string boundary(input.substr(0, eol));
  const std::size_t chunk = 1 + static_cast<std::uint8_t>(input[eol + 1]) % 64;
  input.remove_prefix(eol + 2);

  static const std::filesystem::path tmp = [] {
    auto dir = std::filesystem::temp_directory_path() / "cf_req_fuzz";
    std::filesystem::create_directories(dir);
    return dir;
  }();
  std::pmr::monotonic_buffer_resource arena;
  MultipartParser parser(boundary, tmp, &arena, 1U << 20);
  for (std::size_t i = 0; i < input.size(); i += chunk) {
    if (!parser.feed(input.substr(i, chunk))) return 0;
  }
  if (auto params = parser.finish()) (void)params->to_json();
  (void)parse_disposition(input);
  (void)parse_boundary(input);
  return 0;
}
