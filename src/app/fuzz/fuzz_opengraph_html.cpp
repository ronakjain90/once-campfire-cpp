// libFuzzer target: the HTML of a page that a user asked to unfurl (the part of libxml2's parser that Opengraph reads,
// and the attributes taken from it). Rust: crates/campfire/src/integrations/opengraph/html.rs.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "app/opengraph/html.hpp"
#include "app/opengraph/opengraph.hpp"
#include "compat/json.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace campfire::app::opengraph;
  const std::string_view bytes(reinterpret_cast<const char*>(data), size);
  const std::string text = decode(bytes);
  // `decode` gives valid UTF-8, whatever the input was.
  if (!campfire::compat::json::valid_utf8(text)) std::abort();
  for (const Element& meta : meta_elements(text)) {
    if (meta.attributes.size() > 256) std::abort();
  }
  (void)meta_encoding(meta_elements(text));
  // At most the four attributes, each a value of valid UTF-8.
  const auto attributes = opengraph_attributes(bytes);
  if (attributes.size() > 4) std::abort();
  return 0;
}
