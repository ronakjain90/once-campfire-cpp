// ActiveSupport::MessageEncryptor (see message_encryptor.hpp).
#include "compat/message_encryptor.hpp"

#include <cstdlib>

#include "compat/base64.hpp"
#include "compat/crypto.hpp"

namespace campfire::compat {
namespace {

constexpr size_t kEncodedIvLength = 16;   // strict Base64 of 12 bytes
constexpr size_t kEncodedTagLength = 24;  // strict Base64 of 16 bytes

struct Parts {
  std::string_view ciphertext, iv, tag;
};

// extract_parts: fixed-length IV and tag at the end, each preceded by "--".
std::optional<Parts> extract_parts(std::string_view m) {
  if (m.size() < kEncodedTagLength) return std::nullopt;
  size_t tag_start = m.size() - kEncodedTagLength;
  if (tag_start < 2 + kEncodedIvLength) return std::nullopt;
  size_t iv_start = tag_start - 2 - kEncodedIvLength;
  if (iv_start < 2) return std::nullopt;
  size_t ciphertext_end = iv_start - 2;
  if (m.substr(tag_start - 2, 2) != "--" || m.substr(ciphertext_end, 2) != "--") return std::nullopt;
  return Parts{m.substr(0, ciphertext_end), m.substr(iv_start, kEncodedIvLength), m.substr(tag_start)};
}

}  // namespace

MessageEncryptor::MessageEncryptor(std::string secret, Serializer serializer)
    : secret_(std::move(secret)), serializer_(serializer) {
  if (secret_.size() != 32) std::abort();  // caller error: aes-256-gcm needs a 32-byte key
}

std::string MessageEncryptor::encrypt_and_sign(const json::Value& value, std::optional<std::string_view> purpose,
                                               std::optional<Timestamp> expires_at) const {
  std::string plaintext = serialize_with_metadata(serializer_, value, purpose, expires_at);
  std::string iv;
  crypto::random_bytes(iv, crypto::kGcmIvLength);
  return encrypt_with_iv(plaintext, iv);
}

std::string MessageEncryptor::encrypt_with_iv(std::string_view plaintext, std::string_view iv) const {
  auto sealed = crypto::aes256_gcm_encrypt(secret_, iv, plaintext);
  if (!sealed) std::abort();
  return base64::strict_encode(sealed->ciphertext) + "--" + base64::strict_encode(iv) + "--" +
         base64::strict_encode(sealed->tag);
}

std::expected<json::Value, Error> MessageEncryptor::decrypt_and_verify(std::string_view message,
                                                                       std::optional<std::string_view> purpose,
                                                                       Timestamp now) const {
  auto plaintext = decrypt(message);
  if (!plaintext) return std::unexpected(Error::InvalidSignature);
  return deserialize_with_metadata(serializer_, *plaintext, purpose, now, base64::strict_decode);
}

std::optional<std::string> MessageEncryptor::decrypt(std::string_view message) const {
  auto parts = extract_parts(message);
  if (!parts) return std::nullopt;
  auto ciphertext = base64::strict_decode(parts->ciphertext);
  auto iv = base64::strict_decode(parts->iv);
  auto tag = base64::strict_decode(parts->tag);
  if (!ciphertext || !iv || !tag) return std::nullopt;
  if (iv->size() != crypto::kGcmIvLength || tag->size() != crypto::kGcmTagLength) return std::nullopt;
  return crypto::aes256_gcm_decrypt(secret_, *iv, *ciphertext, *tag);
}

}  // namespace campfire::compat
