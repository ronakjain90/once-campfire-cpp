// Ruby's Base64 flavors as Rails uses them (Rust: crates/rails_compat/src/encoding.rs).
#include "compat/base64.hpp"

#include <array>
#include <cstdint>

namespace campfire::compat::base64 {
namespace {

constexpr std::string_view kStandard = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::string_view kUrlSafe = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string encode(std::string_view data, std::string_view alphabet, bool pad) {
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 3 <= data.size(); i += 3) {
    uint32_t n = (uint8_t(data[i]) << 16) | (uint8_t(data[i + 1]) << 8) | uint8_t(data[i + 2]);
    out += alphabet[n >> 18];
    out += alphabet[(n >> 12) & 63];
    out += alphabet[(n >> 6) & 63];
    out += alphabet[n & 63];
  }
  size_t rest = data.size() - i;
  if (rest == 1) {
    uint32_t n = uint8_t(data[i]) << 16;
    out += alphabet[n >> 18];
    out += alphabet[(n >> 12) & 63];
    if (pad) out += "==";
  } else if (rest == 2) {
    uint32_t n = (uint8_t(data[i]) << 16) | (uint8_t(data[i + 1]) << 8);
    out += alphabet[n >> 18];
    out += alphabet[(n >> 12) & 63];
    out += alphabet[(n >> 6) & 63];
    if (pad) out += '=';
  }
  return out;
}

constexpr std::array<int8_t, 256> make_decode_table() {
  std::array<int8_t, 256> table{};
  for (auto& v : table) v = -1;
  for (size_t i = 0; i < kStandard.size(); ++i) table[uint8_t(kStandard[i])] = int8_t(i);
  return table;
}
constexpr auto kDecode = make_decode_table();

}  // namespace

std::string strict_encode(std::string_view data) {
  return encode(data, kStandard, true);
}
std::string urlsafe_encode_unpadded(std::string_view data) {
  return encode(data, kUrlSafe, false);
}
std::string urlsafe_encode_padded(std::string_view data) {
  return encode(data, kUrlSafe, true);
}

std::optional<std::string> strict_decode(std::string_view encoded) {
  if (encoded.size() % 4 != 0) return std::nullopt;
  std::string out;
  out.reserve(encoded.size() / 4 * 3);
  for (size_t i = 0; i < encoded.size(); i += 4) {
    bool last = i + 4 == encoded.size();
    int pad = 0;
    if (last) {
      if (encoded[i + 3] == '=') pad = encoded[i + 2] == '=' ? 2 : 1;
    }
    uint32_t n = 0;
    for (size_t j = 0; j < 4; ++j) {
      char c = encoded[i + j];
      int v;
      if (j >= size_t(4 - pad)) {
        if (c != '=') return std::nullopt;
        v = 0;
      } else {
        v = kDecode[uint8_t(c)];
        if (v < 0) return std::nullopt;
      }
      n = (n << 6) | uint32_t(v);
    }
    // Trailing bits of the last group must be zero (canonical encoding).
    if (pad == 2 && (n & 0xffff) != 0) return std::nullopt;
    if (pad == 1 && (n & 0xff) != 0) return std::nullopt;
    out += char(n >> 16);
    if (pad < 2) out += char((n >> 8) & 0xff);
    if (pad < 1) out += char(n & 0xff);
  }
  return out;
}

std::optional<std::string> urlsafe_decode(std::string_view encoded) {
  std::string translated(encoded);
  for (auto& c : translated) {
    if (c == '-')
      c = '+';
    else if (c == '_')
      c = '/';
  }
  if (!encoded.empty() && encoded.back() != '=' && encoded.size() % 4 != 0) {
    while (translated.size() % 4 != 0) translated += '=';
  }
  return strict_decode(translated);
}

}  // namespace campfire::compat::base64
