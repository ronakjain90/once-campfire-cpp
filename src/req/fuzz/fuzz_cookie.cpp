// libFuzzer target: the Cookie header and signed and encrypted cookie values (Rack: parse_cookies_header).
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "compat/secrets.hpp"
#include "core/clock.hpp"
#include "req/cookie.hpp"
#include "req/session.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace campfire::req;
  static const campfire::compat::Secrets secrets("fuzz-secret");
  static const campfire::SystemClock clock;
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  (void)parse_cookie_header(input);
  CookieJar jar(std::vector<std::string_view>{input}, secrets, clock);
  (void)jar.signed_value("session_token");
  (void)jar.encrypted_value(kSessionKey);
  Session session;
  session.load(jar);
  (void)session.id();
  return 0;
}
