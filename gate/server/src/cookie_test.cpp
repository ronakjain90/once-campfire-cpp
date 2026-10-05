// Cookie vector test: ./cookie_test <campfire_sessions.json> <secret_key_base>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>

#include "crypto.hpp"
using namespace gate;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: %s  ", #c); printf(__VA_ARGS__); printf("\n"); } else printf("ok: %s\n", #c); } while (0)

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  std::ifstream f(argv[1]);
  std::stringstream ss; ss << f.rdbuf();
  std::string j = ss.str();
  Secrets sec(argv[2]);
  const int64_t now = *parse_iso_ms("2026-01-01T12:00:00Z");  // the vectors' frozen clock
  // sessions
  std::regex re(R"VV("token": "([^"]+)",\s*"cookie_value": "([^"]+)",\s*"cookie_header": "([^"]+)")VV");
  int n = 0;
  for (auto it = std::sregex_iterator(j.begin(), j.end(), re); it != std::sregex_iterator(); ++it, ++n) {
    std::string token = (*it)[1], cv = (*it)[2], ch = (*it)[3];
    auto got = verify_signed_cookie(sec, "session_token", cv, now);
    CHECK(got && *got == token, "session %d token=%s got=%s", n, token.c_str(), got ? got->c_str() : "(none)");
    std::string wire = ch.substr(strlen("session_token="));
    CHECK(www_unescape(wire) == cv, "unescape session %d", n);
    CHECK(www_escape(cv) == wire, "escape session %d", n);
    // re-sign with the same expiry: must reproduce Rails byte for byte
    std::string enc = *b64_urlsafe_decode(cv.substr(0, cv.size() - 42));
    std::smatch m;
    std::regex ex(R"VV("exp":"([^"]+)")VV");
    CHECK(std::regex_search(enc, m, ex), "exp present");
    int64_t exp = *parse_iso_ms(m[1].str());
    CHECK(sign_cookie(sec, "session_token", token, exp) == cv, "re-sign session %d reproduces Rails cookie", n);
    CHECK(!verify_signed_cookie(sec, "session_token", cv, exp + 1), "expired cookie rejected (session %d)", n);
    CHECK(!verify_signed_cookie(sec, "other", cv, now), "wrong purpose rejected (session %d)", n);
    std::string bad = cv; bad[3] = bad[3] == 'A' ? 'B' : 'A';
    CHECK(!verify_signed_cookie(sec, "session_token", bad, now), "tampered rejected (session %d)", n);
  }
  CHECK(n >= 1, "found %d sessions", n);
  std::regex fr(R"VV("forged": \{\s*"cookie_value": "([^"]+)")VV");
  std::smatch fm;
  CHECK(std::regex_search(j, fm, fr), "forged vector present");
  auto fg = verify_signed_cookie(sec, "session_token", fm[1].str(), now);
  CHECK(fg && *fg == "not-a-session-token", "forged cookie verifies to a token that is not in the sessions table");
  CHECK(!verify_signed_cookie(sec, "session_token", "tampered--0000", now), "tampered--0000 rejected");
  CHECK(!verify_signed_cookie(sec, "session_token", "forged", now), "garbage rejected");
  // signed id for avatar of David (labels.json avatar_tokens.david)
  CHECK(signed_id(sec, "User", 127326141, "avatar") == "eyJfcmFpbHMiOnsiZGF0YSI6MTI3MzI2MTQxLCJwdXIiOiJ1c2VyL2F2YXRhciJ9fQ--0ac8233a4786ee6c413408416bb81e7107534db5702d351533dc2ecd3876e97f", "avatar signed id");
  printf(fails ? "COOKIE VECTOR TEST FAILED (%d)\n" : "COOKIE VECTOR TEST PASSED\n", fails);
  return fails != 0;
}
