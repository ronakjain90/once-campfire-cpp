// has_secure_password (Rails: ActiveModel::SecurePassword, bcrypt gem 3.1.22; Rust: crates/rails_compat bcrypt).
#include "req/bcrypt.hpp"

#include <array>
#include <cstring>

#include "compat/crypto.hpp"

extern "C" {
#include "crypt_blowfish.h"
}

namespace campfire::req::bcrypt {

namespace {

constexpr int kHashSize = 64;  // BCRYPT_HASHSIZE in the gem
constexpr std::size_t kDigestLength = 60;

// The gem passes the password as a C string: bytes after a NUL byte do not count.
std::string c_string(std::string_view s) { return std::string(s.substr(0, s.find('\0'))); }

}  // namespace

std::string hash_password(std::string_view password, int cost) {
  if (cost < kMinCost || cost > 31) return {};
  std::string salt_bytes;
  compat::crypto::random_bytes(salt_bytes, 16);
  std::array<char, kHashSize> setting{};
  if (_crypt_gensalt_blowfish_rn("$2a$", static_cast<unsigned long>(cost), salt_bytes.data(), 16, setting.data(),
                                 kHashSize) == nullptr) {
    return {};
  }
  std::array<char, kHashSize> out{};
  const std::string key = c_string(password);
  if (_crypt_blowfish_rn(key.c_str(), setting.data(), out.data(), kHashSize) == nullptr) return {};
  return std::string(out.data());
}

bool verify_password(std::string_view password, std::string_view digest) {
  // BCrypt::Password.valid_hash?: "$2a$", 2 digit cost, "$", 53 characters.
  if (digest.size() != kDigestLength || digest.find('\0') != std::string_view::npos) return false;
  const std::string setting(digest);
  std::array<char, kHashSize> out{};
  const std::string key = c_string(password);
  if (_crypt_blowfish_rn(key.c_str(), setting.c_str(), out.data(), kHashSize) == nullptr) return false;
  return compat::crypto::constant_time_equal(std::string_view(out.data()), digest);
}

}  // namespace campfire::req::bcrypt
