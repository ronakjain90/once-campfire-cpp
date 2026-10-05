#include "crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <cstring>
#include <ctime>

namespace gate {

std::string pbkdf2_sha256(std::string_view secret, std::string_view salt, int iterations, size_t len) {
  std::string out(len, '\0');
  PKCS5_PBKDF2_HMAC(secret.data(), (int)secret.size(), (const unsigned char*)salt.data(), (int)salt.size(), iterations,
                    EVP_sha256(), (int)len, (unsigned char*)out.data());
  return out;
}
static std::string hmac(const EVP_MD* md, std::string_view key, std::string_view data) {
  unsigned char buf[EVP_MAX_MD_SIZE];
  unsigned len = 0;
  HMAC(md, key.data(), (int)key.size(), (const unsigned char*)data.data(), data.size(), buf, &len);
  return std::string((char*)buf, len);
}
std::string hmac_sha1(std::string_view k, std::string_view d) { return hmac(EVP_sha1(), k, d); }
std::string hmac_sha256(std::string_view k, std::string_view d) { return hmac(EVP_sha256(), k, d); }
std::string sha256(std::string_view d) {
  unsigned char buf[32];
  SHA256((const unsigned char*)d.data(), d.size(), buf);
  return std::string((char*)buf, 32);
}
std::string hex(std::string_view raw) {
  static const char* h = "0123456789abcdef";
  std::string o;
  o.reserve(raw.size() * 2);
  for (unsigned char c : raw) { o += h[c >> 4]; o += h[c & 15]; }
  return o;
}
std::string md5_hex(std::string_view data) {
  unsigned char buf[16];
  unsigned len = 0;
  EVP_Digest(data.data(), data.size(), buf, &len, EVP_md5(), nullptr);
  return hex(std::string_view((char*)buf, len));
}

static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string b64_strict_encode(std::string_view in) {
  std::string o;
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
    o += B64[v >> 18]; o += B64[(v >> 12) & 63]; o += B64[(v >> 6) & 63]; o += B64[v & 63];
  }
  if (i + 1 == in.size()) {
    uint32_t v = (uint8_t)in[i] << 16;
    o += B64[v >> 18]; o += B64[(v >> 12) & 63]; o += "==";
  } else if (i + 2 == in.size()) {
    uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8);
    o += B64[v >> 18]; o += B64[(v >> 12) & 63]; o += B64[(v >> 6) & 63]; o += '=';
  }
  return o;
}
std::string b64_url_encode_nopad(std::string_view in) {
  std::string o = b64_strict_encode(in);
  while (!o.empty() && o.back() == '=') o.pop_back();
  for (auto& c : o) { if (c == '+') c = '-'; else if (c == '/') c = '_'; }
  return o;
}
std::optional<std::string> b64_strict_decode(std::string_view in) {
  if (in.size() % 4) return std::nullopt;
  std::string o;
  o.reserve(in.size() / 4 * 3);
  for (size_t i = 0; i < in.size(); i += 4) {
    int v[4];
    int pad = 0;
    for (int j = 0; j < 4; j++) {
      char c = in[i + j];
      if (c == '=') {
        if (i + 4 != in.size() || j < 2) return std::nullopt;
        pad++; v[j] = 0;
      } else {
        if (pad) return std::nullopt;
        const char* p = c ? (const char*)memchr(B64, c, 64) : nullptr;
        if (!p) return std::nullopt;
        v[j] = (int)(p - B64);
      }
    }
    uint32_t x = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
    if (pad == 2 && (v[1] & 15)) return std::nullopt;
    if (pad == 1 && (v[2] & 3)) return std::nullopt;
    o += (char)(x >> 16);
    if (pad < 2) o += (char)((x >> 8) & 255);
    if (pad < 1) o += (char)(x & 255);
  }
  return o;
}
std::optional<std::string> b64_urlsafe_decode(std::string_view in) {
  std::string t(in);
  for (auto& c : t) { if (c == '-') c = '+'; else if (c == '_') c = '/'; }
  if (!in.empty() && in.back() != '=' && in.size() % 4) while (t.size() % 4) t += '=';
  return b64_strict_decode(t);
}

std::string www_escape(std::string_view in) {
  static const char* h = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : in) {
    if (isalnum(c) || c == '*' || c == '-' || c == '.' || c == '_') o += (char)c;
    else if (c == ' ') o += '+';
    else { o += '%'; o += h[c >> 4]; o += h[c & 15]; }
  }
  return o;
}
static int hv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
std::string www_unescape(std::string_view in) {
  std::string o;
  for (size_t i = 0; i < in.size(); i++) {
    char c = in[i];
    if (c == '+') o += ' ';
    else if (c == '%') {
      if (i + 3 > in.size()) return std::string(in);
      int a = hv(in[i + 1]), b = hv(in[i + 2]);
      if (a < 0 || b < 0) return std::string(in);
      o += (char)(a * 16 + b);
      i += 2;
    } else o += c;
  }
  return o;
}

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}
std::optional<int64_t> parse_iso_ms(std::string_view s) {
  // YYYY-MM-DDTHH:MM:SS[.f+]Z
  if (s.size() < 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' || s.back() != 'Z') return std::nullopt;
  auto num = [&](size_t o, size_t n) -> int {
    int v = 0;
    for (size_t i = 0; i < n; i++) { if (!isdigit((unsigned char)s[o + i])) return -1; v = v * 10 + (s[o + i] - '0'); }
    return v;
  };
  int Y = num(0, 4), M = num(5, 2), D = num(8, 2), h = num(11, 2), mi = num(14, 2), se = num(17, 2);
  if (Y < 0 || M < 1 || M > 12 || D < 1 || D > 31 || h < 0 || mi < 0 || se < 0) return std::nullopt;
  int64_t ms = 0;
  if (s.size() > 20) {
    if (s[19] != '.') return std::nullopt;
    int digits = 0;
    for (size_t i = 20; i + 1 < s.size(); i++) {
      if (!isdigit((unsigned char)s[i])) return std::nullopt;
      if (digits < 3) { ms = ms * 10 + (s[i] - '0'); digits++; }
    }
    while (digits < 3) { ms *= 10; digits++; }
  }
  return ((days_from_civil(Y, M, D) * 24 + h) * 60 + mi) * 60000 + se * 1000 + ms;
}
std::string iso_ms(int64_t unix_ms) {
  time_t t = (time_t)(unix_ms / 1000);
  struct tm tm;
  gmtime_r(&t, &tm);
  char buf[40];
  snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
           tm.tm_sec, (int)(unix_ms % 1000));
  return buf;
}

Secrets::Secrets(std::string_view base)
    : signed_cookie_key(pbkdf2_sha256(base, "signed cookie", 1000, 64)), signed_id_key(pbkdf2_sha256(base, "active_record/signed_id", 1000, 64)) {}

// --- tiny JSON string helpers ---
static std::optional<std::string> parse_json_string(std::string_view s, size_t& i) {
  if (i >= s.size() || s[i] != '"') return std::nullopt;
  i++;
  std::string o;
  while (i < s.size()) {
    char c = s[i++];
    if (c == '"') return o;
    if (c == '\\') {
      if (i >= s.size()) return std::nullopt;
      char e = s[i++];
      switch (e) {
        case '"': o += '"'; break; case '\\': o += '\\'; break; case '/': o += '/'; break;
        case 'b': o += '\b'; break; case 'f': o += '\f'; break; case 'n': o += '\n'; break;
        case 'r': o += '\r'; break; case 't': o += '\t'; break;
        case 'u': {
          if (i + 4 > s.size()) return std::nullopt;
          unsigned cp = 0;
          for (int k = 0; k < 4; k++) { int h = hv(s[i + k]); if (h < 0) return std::nullopt; cp = cp * 16 + h; }
          i += 4;
          if (cp < 0x80) o += (char)cp;
          else if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 63)); }
          else { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
          break;
        }
        default: return std::nullopt;
      }
    } else o += c;
  }
  return std::nullopt;
}
static void skip_ws(std::string_view s, size_t& i) { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r')) i++; }

// Parses {"_rails":{"message":"..","exp":"..."|null,"pur":"..."|null}}.
struct Envelope { std::string message; std::optional<std::string> exp, pur; bool exp_bad = false; };
static std::optional<Envelope> parse_envelope(std::string_view s) {
  size_t i = 0;
  auto lit = [&](std::string_view t) { skip_ws(s, i); if (s.substr(i, t.size()) != t) return false; i += t.size(); return true; };
  if (!lit("{") || !lit("\"_rails\"") || !lit(":") || !lit("{")) return std::nullopt;
  Envelope e;
  bool have_msg = false;
  for (;;) {
    skip_ws(s, i);
    auto key = parse_json_string(s, i);
    if (!key || !lit(":")) return std::nullopt;
    skip_ws(s, i);
    std::optional<std::string> val;
    if (s.substr(i, 4) == "null") { i += 4; }
    else { val = parse_json_string(s, i); if (!val) { if (*key == "exp") e.exp_bad = true; else return std::nullopt; } }
    if (*key == "message") { if (!val) return std::nullopt; e.message = *val; have_msg = true; }
    else if (*key == "exp") e.exp = val;
    else if (*key == "pur") e.pur = val;
    skip_ws(s, i);
    if (i < s.size() && s[i] == ',') { i++; continue; }
    break;
  }
  if (!lit("}") || !lit("}") || !have_msg) return std::nullopt;
  skip_ws(s, i);
  if (i != s.size()) return std::nullopt;
  return e;
}

std::optional<std::string> verify_signed_cookie(const Secrets& sec, std::string_view name, std::string_view raw, int64_t now_ms) {
  if (raw.size() < 42) return std::nullopt;
  size_t idx = raw.size() - 42;
  if (raw.substr(idx, 2) != "--") return std::nullopt;
  std::string_view enc = raw.substr(0, idx), dig = raw.substr(idx + 2);
  auto blank = [](std::string_view v) { for (char c : v) if (!isspace((unsigned char)c)) return false; return true; };
  if (blank(enc) || blank(dig)) return std::nullopt;
  std::string want = hex(hmac_sha1(sec.signed_cookie_key, enc));
  if (CRYPTO_memcmp(want.data(), dig.data(), 40) != 0) return std::nullopt;
  auto decoded = b64_urlsafe_decode(enc);
  if (!decoded) return std::nullopt;
  std::string purpose = "cookie." + std::string(name);
  std::string dumped;
  static const std::string_view kPrefix = R"({"_rails":{"message":")";
  if (std::string_view(*decoded).substr(0, kPrefix.size()) == kPrefix) {
    auto env = parse_envelope(*decoded);
    if (!env) return std::nullopt;
    if (env->exp_bad) return std::nullopt;
    if (env->exp) {
      auto e = parse_iso_ms(*env->exp);
      if (!e) return std::nullopt;
      if (now_ms >= *e) return std::nullopt;
    }
    if (env->pur.value_or("") != purpose) return std::nullopt;  // the no-purpose retry cannot succeed for an envelope
    auto inner = b64_urlsafe_decode(env->message);
    if (!inner) return std::nullopt;
    dumped = std::move(*inner);
  } else {
    dumped = std::move(*decoded);  // signed without metadata: accepted under no purpose
  }
  if (dumped.size() >= 2 && dumped[0] == 4 && dumped[1] == 8) return std::nullopt;  // Marshal not allowed
  size_t i = 0;
  skip_ws(dumped, i);
  auto v = parse_json_string(dumped, i);
  if (!v) return std::nullopt;
  skip_ws(dumped, i);
  if (i != dumped.size()) return std::nullopt;
  return v;
}

static std::string json_quote(std::string_view s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\";
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  return o + "\"";
}

std::string sign_cookie(const Secrets& sec, std::string_view name, std::string_view value, int64_t expires_ms) {
  std::string dumped = json_quote(value);
  std::string env = R"({"_rails":{"message":)" + json_quote(b64_strict_encode(dumped)) + R"(,"exp":)" +
                    (expires_ms >= 0 ? json_quote(iso_ms(expires_ms)) : "null") + R"(,"pur":)" + json_quote("cookie." + std::string(name)) + "}}";
  std::string enc = b64_strict_encode(env);
  return enc + "--" + hex(hmac_sha1(sec.signed_cookie_key, enc));
}

std::string signed_id(const Secrets& sec, std::string_view model, int64_t id, std::string_view purpose) {
  std::string lower;
  for (size_t i = 0; i < model.size(); i++) { char c = model[i]; if (isupper((unsigned char)c)) { if (i) lower += '_'; lower += (char)tolower(c); } else lower += c; }
  std::string pur = lower + "/" + std::string(purpose);
  std::string payload = R"({"_rails":{"data":)" + std::to_string(id) + R"(,"pur":)" + json_quote(pur) + "}}";
  std::string enc = b64_url_encode_nopad(payload);
  return enc + "--" + hex(hmac_sha256(sec.signed_id_key, enc));
}

}  // namespace gate
