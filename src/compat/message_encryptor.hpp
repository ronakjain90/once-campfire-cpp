// ActiveSupport::MessageEncryptor with aes-256-gcm, as the encrypted cookie jar builds it:
// <base64 ciphertext>--<base64 12-byte IV>--<base64 16-byte tag>, strict Base64, empty
// authenticated data, no separate signature. Rust: crates/rails_compat/src/message_encryptor.rs.
#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "compat/errors.hpp"
#include "compat/json.hpp"
#include "compat/metadata.hpp"
#include "compat/time.hpp"

namespace campfire::compat {

class MessageEncryptor {
 public:
  // `secret` must have 32 bytes (key_generator.generate_key(salt, 32)).
  MessageEncryptor(std::string secret, Serializer serializer);

  std::string encrypt_and_sign(const json::Value& value, std::optional<std::string_view> purpose = std::nullopt,
                               std::optional<Timestamp> expires_at = std::nullopt) const;

  // For tests: encrypt with a fixed 12-byte IV. Never reuse an IV with the same key in production.
  std::string encrypt_with_iv(std::string_view plaintext, std::string_view iv) const;

  std::expected<json::Value, Error> decrypt_and_verify(std::string_view message,
                                                       std::optional<std::string_view> purpose, Timestamp now) const;

  // The decrypted bytes, before any envelope handling.
  std::optional<std::string> decrypt(std::string_view message) const;

 private:
  std::string secret_;
  Serializer serializer_;
};

}  // namespace campfire::compat
