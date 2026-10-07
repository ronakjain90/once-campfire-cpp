// libFuzzer target: the JSON parser and generator (Rails: JSON.parse, ActiveSupport::JSON.encode).
// A value that parses must generate and parse again to an equal text.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "compat/json.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace campfire::compat::json;
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  for (const bool comments : {false, true}) {
    ParseOptions options;
    options.allow_comments = comments;
    const auto value = parse(text, options);
    if (!value) continue;
    const std::string once = generate(*value);
    (void)encode(*value);
    const auto again = parse(once);
    if (!again) std::abort();
    if (generate(*again) != once) std::abort();
  }
  (void)valid_utf8(text);
  (void)escape_html_entities(text);
  return 0;
}
