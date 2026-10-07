// libFuzzer target: Marshal string loading and Base64 decoding (Rails 7 signed message payloads).
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "compat/base64.hpp"
#include "compat/content_disposition.hpp"
#include "compat/global_id.hpp"
#include "compat/marshal.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace campfire::compat;
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  (void)marshal::load_string(text);
  if (const auto decoded = base64::strict_decode(text)) {
    if (base64::strict_encode(*decoded).size() < text.size() / 2) std::abort();
  }
  (void)base64::urlsafe_decode(text);
  (void)global_id::GlobalId::parse(text);
  (void)global_id::GlobalId::from_param(text);
  (void)global_id::gid_from_unverified_sgid(text);
  const std::size_t cut = size / 2;
  (void)content_disposition("attachment", text.substr(0, cut));
  (void)content_disposition(text.substr(cut, 12), text.substr(0, cut));
  return 0;
}
