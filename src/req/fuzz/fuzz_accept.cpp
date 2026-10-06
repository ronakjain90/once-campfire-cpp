// libFuzzer target: the Accept header and format negotiation (Rails: Mime::Type.parse).
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "req/format.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace campfire::req;
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  (void)parse_accept(input);
  (void)lookup(input);
  (void)content_mime_type(input);
  NegotiationInput in;
  in.accept = input;
  in.has_accept = true;
  in.path = "/rooms/1";
  (void)formats(in);
  NegotiationInput by_param;
  by_param.format_param = input;
  by_param.has_format_param = true;
  (void)formats(by_param);
  return 0;
}
