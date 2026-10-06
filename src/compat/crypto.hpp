// Thin OpenSSL 3 wrappers for the primitives Rails uses: PBKDF2-HMAC-SHA256, HMAC, SHA-1,
// AES-256-GCM, constant-time comparison and hex. Byte strings are std::string.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace campfire::compat::crypto {

enum class Digest { Sha1, Sha256 };

// OpenSSL PKCS5_PBKDF2_HMAC with SHA-256 (ActiveSupport::KeyGenerator).
std::string pbkdf2_sha256(std::string_view password, std::string_view salt, unsigned iterations, size_t length);
std::string hmac(Digest digest, std::string_view key, std::string_view data);
std::string sha1(std::string_view data);

// Compares two byte strings. The length leaks. The content does not.
bool constant_time_equal(std::string_view a, std::string_view b);

std::string hex_encode(std::string_view bytes);
std::optional<std::string> hex_decode(std::string_view hex);

inline constexpr size_t kGcmIvLength = 12;
inline constexpr size_t kGcmTagLength = 16;

struct GcmSealed {
  std::string ciphertext;
  std::string tag;
};

// AES-256-GCM with no associated data. The key must have 32 bytes and the IV 12 bytes.
std::optional<GcmSealed> aes256_gcm_encrypt(std::string_view key, std::string_view iv, std::string_view plaintext);
std::optional<std::string> aes256_gcm_decrypt(std::string_view key, std::string_view iv, std::string_view ciphertext,
                                              std::string_view tag);

// Fills `out` from the OpenSSL random generator. Aborts if the generator fails.
void random_bytes(std::string& out, size_t length);

}  // namespace campfire::compat::crypto
