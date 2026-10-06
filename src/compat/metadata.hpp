// ActiveSupport::Messages::Metadata and SerializerWithFallback: how a value, its purpose and
// its expiry are packed into the bytes that get signed or encrypted.
// Rust: crates/rails_compat/src/metadata.rs.
//
// Two envelopes exist. For serializers Rails trusts with metadata (JSON, and the
// :json_allow_marshal fallback) it is {"_rails":{"data":<value>,"exp":..,"pur":..}}. With the
// cookie jars' NullSerializer it is the legacy "dual-serialized" one,
// {"_rails":{"message":"<base64 of the dumped value>","exp":..,"pur":..}}, which always
// carries exp and pur ("null" when unset).
#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "compat/errors.hpp"
#include "compat/json.hpp"
#include "compat/time.hpp"

namespace campfire::compat {

struct Serializer {
  enum class Kind {
    // MessageEncryptor::NullSerializer: the value is a string of bytes that are already serialized.
    Null,
    // The ::JSON module (JSON.dump / JSON.load), as signed ids and Turbo stream names use.
    Json,
    // SerializerWithFallback[:json] or [:json_allow_marshal] with ActiveSupport::JSON.
    JsonWithFallback,
  };
  Kind kind = Kind::Null;
  bool allow_marshal = false;

  static constexpr Serializer null() { return {Kind::Null, false}; }
  static constexpr Serializer json() { return {Kind::Json, false}; }
  static constexpr Serializer json_with_fallback(bool allow_marshal) { return {Kind::JsonWithFallback, allow_marshal}; }

  // The bytes to sign. The Null serializer takes only strings (a caller error otherwise: aborts).
  std::string dump(const json::Value& value) const;
  std::expected<json::Value, Error> load(std::string_view bytes) const;
  // The value as JSON in this serializer's escaping.
  std::string encode_json(const json::Value& value) const;
  bool uses_envelope() const { return kind != Kind::Null; }
};

std::string serialize_with_metadata(const Serializer& serializer, const json::Value& value,
                                    std::optional<std::string_view> purpose, std::optional<Timestamp> expires_at);

// For a value the caller already dumped with `serializer`, so the caller controls key order and escaping.
std::string serialize_dumped_with_metadata(const Serializer& serializer, std::string_view dumped,
                                           std::optional<std::string_view> purpose,
                                           std::optional<Timestamp> expires_at);

using LegacyDecoder = std::optional<std::string> (*)(std::string_view);

// The verifier accepts either Base64 alphabet inside a legacy envelope, the encryptor only strict.
std::expected<json::Value, Error> deserialize_with_metadata(const Serializer& serializer, std::string_view bytes,
                                                            std::optional<std::string_view> purpose, Timestamp now,
                                                            LegacyDecoder decode_legacy_message);

// Object#to_s for the JSON values a purpose could hold.
std::string ruby_to_s(const json::Value* value);

}  // namespace campfire::compat
