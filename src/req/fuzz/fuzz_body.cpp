// libFuzzer target: a whole request body to params (Rack form, JSON and multipart parsing).
// The first byte chooses the content type.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <string>
#include <string_view>

#include "req/body.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace campfire::req;
  if (size == 0) return 0;
  static const std::filesystem::path tmp = [] {
    auto dir = std::filesystem::temp_directory_path() / "cf_req_fuzz_body";
    std::filesystem::create_directories(dir);
    return dir;
  }();
  static constexpr std::string_view kTypes[] = {
      "application/x-www-form-urlencoded", "application/json", "text/json",
      "multipart/form-data; boundary=xyz", "multipart/form-data; boundary=\"a b\"", "text/plain", ""};
  const std::string_view type = kTypes[data[0] % std::size(kTypes)];
  const std::string_view body(reinterpret_cast<const char*>(data) + 1, size - 1);
  std::pmr::monotonic_buffer_resource arena;
  const auto parsed = parse_body("POST", type.empty() ? std::nullopt : std::optional<std::string_view>(type), body,
                                 tmp, &arena, 1U << 20);
  if (parsed && parsed->params) (void)parsed->params->to_json();
  (void)media_type(body);
  return 0;
}
