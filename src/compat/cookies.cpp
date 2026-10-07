// Cookie values (see cookies.hpp).
#include "compat/cookies.hpp"

namespace campfire::compat::cookies {
namespace {

std::string purpose(std::string_view name) {
  return "cookie." + std::string(name);
}

// SerializerWithFallback[:json].load: Marshal payloads are not allowed for cookies.
std::optional<json::Value> load(const json::Value& dumped) {
  const std::string* text = dumped.get_string();
  if (text == nullptr) return std::nullopt;
  auto value = Serializer::json_with_fallback(false).load(*text);
  if (!value) return std::nullopt;
  return std::move(*value);
}

}  // namespace

std::string sign(const Secrets& secrets, std::string_view name, std::string_view value,
                 std::optional<Timestamp> expires_at) {
  std::string dumped = json::encode(json::Value(value));
  return secrets.signed_cookie_verifier().generate(json::Value(std::move(dumped)), purpose(name), expires_at);
}

std::optional<json::Value> verify_signed_value(const Secrets& secrets, std::string_view name, std::string_view raw,
                                               Timestamp now) {
  const MessageVerifier& verifier = secrets.signed_cookie_verifier();
  auto dumped = verifier.verify(raw, purpose(name), now);
  if (!dumped) dumped = verifier.verify(raw, std::nullopt, now);
  if (!dumped) return std::nullopt;
  return load(*dumped);
}

std::optional<std::string> verify_signed(const Secrets& secrets, std::string_view name, std::string_view raw,
                                         Timestamp now) {
  auto value = verify_signed_value(secrets, name, raw, now);
  if (!value || !value->is_string()) return std::nullopt;
  return value->as_string();
}

std::string encrypt(const Secrets& secrets, std::string_view name, const json::Value& value,
                    std::optional<Timestamp> expires_at) {
  return secrets.encrypted_cookie_encryptor().encrypt_and_sign(json::Value(json::encode(value)), purpose(name),
                                                               expires_at);
}

std::optional<json::Value> decrypt(const Secrets& secrets, std::string_view name, std::string_view raw, Timestamp now) {
  const MessageEncryptor& encryptor = secrets.encrypted_cookie_encryptor();
  auto dumped = encryptor.decrypt_and_verify(raw, purpose(name), now);
  if (!dumped) dumped = encryptor.decrypt_and_verify(raw, std::nullopt, now);
  if (!dumped) return std::nullopt;
  return load(*dumped);
}

std::string escape(std::string_view raw) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(raw.size() + raw.size() / 2);
  for (unsigned char c : raw) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '*' || c == '-' ||
        c == '.' || c == '_') {
      out += char(c);
    } else if (c == ' ') {
      out += '+';
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

std::string unescape(std::string_view wire) {
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(wire.size());
  for (size_t i = 0; i < wire.size(); ++i) {
    char c = wire[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%') {
      if (i + 2 >= wire.size()) return std::string(wire);
      int hi = nibble(wire[i + 1]);
      int lo = nibble(wire[i + 2]);
      if (hi < 0 || lo < 0) return std::string(wire);
      out += char(hi << 4 | lo);
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace campfire::compat::cookies
