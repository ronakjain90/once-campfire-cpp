// ActiveSupport::MessageVerifier: <base64 payload>--<hex HMAC of the base64 payload>.
// Rust: crates/rails_compat/src/message_verifier.rs. Each use in Rails configures it
// differently, so the constructor takes every option.
#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "compat/crypto.hpp"
#include "compat/errors.hpp"
#include "compat/json.hpp"
#include "compat/metadata.hpp"
#include "compat/time.hpp"

namespace campfire::compat {

// How the payload is Base64-encoded when generating. Reading accepts either alphabet in
// every case (MessageVerifier#decode retries with the other one, and GlobalID::Verifier uses
// urlsafe_decode64).
enum class Base64Encoding {
  Strict,         // Base64.strict_encode64 (the default)
  UrlSafe,        // url_safe: true. URL-safe alphabet, no padding
  UrlSafePadded,  // GlobalID::Verifier. URL-safe alphabet with padding
};

class MessageVerifier {
 public:
  MessageVerifier(std::string secret, crypto::Digest digest, Base64Encoding encoding, Serializer serializer);

  // rotate / fall_back_to: tried in order when this verifier cannot read a message.
  MessageVerifier& fall_back_to(MessageVerifier rotation);

  std::string generate(const json::Value& value, std::optional<std::string_view> purpose = std::nullopt,
                       std::optional<Timestamp> expires_at = std::nullopt) const;

  // generate for data the caller already encoded as JSON, so the caller controls the key order:
  // {"_rails":{"data":<data_json>,"exp":..,"pur":..}}.
  std::string generate_raw(std::string_view data_json, std::optional<std::string_view> purpose = std::nullopt,
                           std::optional<Timestamp> expires_at = std::nullopt) const;

  // verify / verified: the value, or why the message cannot be read. The MAC comparison
  // takes constant time.
  std::expected<json::Value, Error> verify(std::string_view message, std::optional<std::string_view> purpose,
                                           Timestamp now) const;

  // verify, with the data encoded again as JSON in this verifier's serializer. Key order is
  // kept. Escapes and number formats are normalized.
  std::expected<std::string, Error> verify_raw(std::string_view message, std::optional<std::string_view> purpose,
                                               Timestamp now) const;

 private:
  std::string sign(std::string_view serialized) const;
  std::expected<json::Value, Error> read_message(std::string_view message, std::optional<std::string_view> purpose,
                                                 Timestamp now) const;
  std::optional<std::string_view> extract_encoded(std::string_view signed_message) const;
  size_t hex_length() const { return digest_ == crypto::Digest::Sha1 ? 40 : 64; }

  std::string secret_;
  crypto::Digest digest_;
  Base64Encoding encoding_;
  Serializer serializer_;
  std::vector<MessageVerifier> rotations_;
};

}  // namespace campfire::compat
