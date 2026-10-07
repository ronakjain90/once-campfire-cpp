// libFuzzer target of the JSON reader for the answers of an ACME server. Rust: instant_acme (serde_json).
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "net/front/acme_http.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using campfire::net::front::Json;
  const auto parsed = Json::parse(std::string_view(reinterpret_cast<const char*>(data), size));
  if (parsed) {
    (void)parsed->str("url");
    (void)parsed->find("status");
    (void)parsed->array().size();
  }
  return 0;
}
