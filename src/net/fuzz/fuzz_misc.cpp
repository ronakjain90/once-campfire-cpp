// libFuzzer target of the small parsers of the front: Accept-Encoding, Cache-Control, Vary, the
// content type guess and the cache key. Rust: crates/kit/src/front/*.rs.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "net/front/cache.hpp"
#include "net/front/compress.hpp"
#include "net/front/static_files.hpp"
#include "net/http.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace campfire::net;
  using namespace campfire::net::front;
  if (size == 0) return 0;
  const std::string text(reinterpret_cast<const char*>(data) + 1, size - 1);
  const std::size_t cut = text.empty() ? 0 : data[0] % (text.size() + 1);
  const std::string_view a = std::string_view(text).substr(0, cut);
  const std::string_view b = std::string_view(text).substr(cut);

  (void)select_encoding(Method::Get, text);
  (void)select_encoding(Method::Head, text);
  (void)choose_deflater_encoding(text);
  (void)content_type_filter(text);
  (void)detect_content_type(text);
  (void)crc32c(text);
  for (int status : {200, 301, 404, 500}) (void)cache_lifetime(status, a, b);

  const Header headers[] = {{"vary", a}, {"cookie", b}, {"accept-encoding", a}};
  Request request;
  request.target = text;
  request.path = text;
  request.headers = headers;
  (void)should_cache_request(request);
  (void)has_user_specific_request_headers(request);
  Variant variant(request);
  variant.set_response_vary(a);
  const std::string key(variant.cache_key());
  (void)variant.variant_headers();
  (void)variant.matches({{"cookie", std::string(b)}});
  return 0;
}
