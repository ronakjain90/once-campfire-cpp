// ActiveSupport::MessageVerifier (see message_verifier.hpp).
#include "compat/message_verifier.hpp"

#include "compat/base64.hpp"

namespace campfire::compat {

MessageVerifier::MessageVerifier(std::string secret, crypto::Digest digest, Base64Encoding encoding,
                                 Serializer serializer)
    : secret_(std::move(secret)), digest_(digest), encoding_(encoding), serializer_(serializer) {}

MessageVerifier& MessageVerifier::fall_back_to(MessageVerifier rotation) {
  rotations_.push_back(std::move(rotation));
  return *this;
}

std::string MessageVerifier::generate(const json::Value& value, std::optional<std::string_view> purpose,
                                      std::optional<Timestamp> expires_at) const {
  return sign(serialize_with_metadata(serializer_, value, purpose, expires_at));
}

std::string MessageVerifier::generate_raw(std::string_view data_json, std::optional<std::string_view> purpose,
                                          std::optional<Timestamp> expires_at) const {
  return sign(serialize_dumped_with_metadata(serializer_, data_json, purpose, expires_at));
}

std::string MessageVerifier::sign(std::string_view serialized) const {
  std::string encoded;
  switch (encoding_) {
    case Base64Encoding::Strict: encoded = base64::strict_encode(serialized); break;
    case Base64Encoding::UrlSafe: encoded = base64::urlsafe_encode_unpadded(serialized); break;
    case Base64Encoding::UrlSafePadded: encoded = base64::urlsafe_encode_padded(serialized); break;
  }
  std::string out = encoded;
  out += "--";
  out += crypto::hex_encode(crypto::hmac(digest_, secret_, encoded));
  return out;
}

std::expected<json::Value, Error> MessageVerifier::verify(std::string_view message,
                                                          std::optional<std::string_view> purpose,
                                                          Timestamp now) const {
  auto result = read_message(message, purpose, now);
  if (result || !rotates(result.error())) return result;
  Error first = result.error();
  for (const auto& rotation : rotations_) {
    auto r = rotation.read_message(message, purpose, now);
    if (!r && rotates(r.error())) continue;
    return r;
  }
  return std::unexpected(first);
}

std::expected<std::string, Error> MessageVerifier::verify_raw(std::string_view message,
                                                              std::optional<std::string_view> purpose,
                                                              Timestamp now) const {
  auto value = verify(message, purpose, now);
  if (!value) return std::unexpected(value.error());
  return serializer_.encode_json(*value);
}

std::expected<json::Value, Error> MessageVerifier::read_message(std::string_view message,
                                                                std::optional<std::string_view> purpose,
                                                                Timestamp now) const {
  auto encoded = extract_encoded(message);
  if (!encoded) return std::unexpected(Error::InvalidSignature);
  auto decoded = base64::urlsafe_decode(*encoded);
  if (!decoded) return std::unexpected(Error::InvalidSignature);
  return deserialize_with_metadata(serializer_, *decoded, purpose, now, base64::urlsafe_decode);
}

namespace {
bool blank(std::string_view s) {
  for (char c : s) {
    if (!(c == ' ' || (c >= '\t' && c <= '\r'))) return false;
  }
  return true;
}
}  // namespace

// extract_encoded: the digest is the last 2 * digest_length characters, preceded by "--".
std::optional<std::string_view> MessageVerifier::extract_encoded(std::string_view signed_message) const {
  size_t digest_length = hex_length();
  if (signed_message.size() < digest_length + 2) return std::nullopt;
  size_t index = signed_message.size() - (digest_length + 2);
  if (signed_message.substr(index, 2) != "--") return std::nullopt;
  std::string_view encoded = signed_message.substr(0, index);
  std::string_view digest = signed_message.substr(index + 2);
  if (blank(encoded) || blank(digest)) return std::nullopt;
  std::string expected = crypto::hex_encode(crypto::hmac(digest_, secret_, encoded));
  if (!crypto::constant_time_equal(digest, expected)) return std::nullopt;
  return encoded;
}

}  // namespace campfire::compat
