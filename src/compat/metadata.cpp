// Message metadata envelopes (see metadata.hpp).
#include "compat/metadata.hpp"

#include <cstdlib>

#include "compat/base64.hpp"
#include "compat/marshal.hpp"

namespace campfire::compat {

std::string Serializer::dump(const json::Value& value) const {
  switch (kind) {
    case Kind::Null:
      if (const std::string* s = value.get_string()) return *s;
      std::abort();  // the null serializer only signs strings
    case Kind::Json: return json::generate(value);
    case Kind::JsonWithFallback: return json::encode(value);
  }
  std::abort();
}

std::expected<json::Value, Error> Serializer::load(std::string_view bytes) const {
  switch (kind) {
    case Kind::Null:
      if (!json::valid_utf8(bytes)) return std::unexpected(Error::InvalidMessage);
      return json::Value(std::string(bytes));
    case Kind::Json: {
      if (bytes.empty()) return json::Value(nullptr);  // JSON.load("") is nil
      auto v = json::parse(bytes);
      if (!v) return std::unexpected(Error::InvalidMessage);
      return std::move(*v);
    }
    case Kind::JsonWithFallback: {
      if (bytes.starts_with(marshal::kSignature)) {
        if (!allow_marshal) return std::unexpected(Error::InvalidMessage);
        auto s = marshal::load_string(bytes);
        if (!s) return std::unexpected(Error::InvalidMessage);
        return json::Value(std::move(*s));
      }
      auto v = json::parse(bytes);
      if (!v) return std::unexpected(Error::InvalidMessage);
      return std::move(*v);
    }
  }
  std::abort();
}

std::string Serializer::encode_json(const json::Value& value) const {
  return kind == Kind::Json ? json::generate(value) : json::encode(value);
}

std::string serialize_with_metadata(const Serializer& serializer, const json::Value& value,
                                    std::optional<std::string_view> purpose, std::optional<Timestamp> expires_at) {
  return serialize_dumped_with_metadata(serializer, serializer.dump(value), purpose, expires_at);
}

std::string serialize_dumped_with_metadata(const Serializer& serializer, std::string_view dumped,
                                           std::optional<std::string_view> purpose,
                                           std::optional<Timestamp> expires_at) {
  if (!purpose && !expires_at) return std::string(dumped);

  json::Value expiry = expires_at ? json::Value(iso8601_millis(*expires_at)) : json::Value(nullptr);
  json::Value pur = purpose ? json::Value(*purpose) : json::Value(nullptr);

  if (serializer.uses_envelope()) {
    std::string out = R"({"_rails":{"data":)";
    out += dumped;
    if (expires_at) out += R"(,"exp":)" + serializer.encode_json(expiry);
    if (purpose) out += R"(,"pur":)" + serializer.encode_json(pur);
    out += "}}";
    return out;
  }
  json::Value message(base64::strict_encode(dumped));
  return R"({"_rails":{"message":)" + json::encode(message) + R"(,"exp":)" + json::encode(expiry) + R"(,"pur":)" +
         json::encode(pur) + "}}";
}

std::string ruby_to_s(const json::Value* value) {
  if (value == nullptr || value->is_null()) return "";
  if (const std::string* s = value->get_string()) return *s;
  if (value->is_bool()) return value->as_bool() ? "true" : "false";
  if (value->is_number()) return json::generate(*value);
  // Arrays and hashes to_s as their #inspect, which no purpose we compare against looks like.
  return std::string("\0", 1) + json::generate(*value);
}

namespace {

// extract_from_metadata_envelope: expired when now >= exp. Purposes compare with to_s, so a
// missing "pur" matches no purpose.
std::expected<const json::Value::Object*, Error> extract(const json::Value& envelope,
                                                         std::optional<std::string_view> purpose, Timestamp now) {
  const json::Value* rails = envelope.find("_rails");
  if (rails == nullptr || !rails->is_object()) return std::unexpected(Error::InvalidMessage);

  const json::Value* exp = rails->find("exp");
  if (exp != nullptr && !exp->is_null()) {
    const std::string* text = exp->get_string();
    if (text == nullptr) return std::unexpected(Error::InvalidMessage);
    auto at = parse_iso8601(*text);
    if (!at) return std::unexpected(Error::InvalidMessage);
    if (now >= *at) return std::unexpected(Error::Expired);
  }
  if (ruby_to_s(rails->find("pur")) != purpose.value_or("")) return std::unexpected(Error::PurposeMismatch);
  return &rails->as_object();
}

const json::Value* member(const json::Value::Object& object, std::string_view key) {
  for (const auto& [k, v] : object) {
    if (k == key) return &v;
  }
  return nullptr;
}

}  // namespace

std::expected<json::Value, Error> deserialize_with_metadata(const Serializer& serializer, std::string_view bytes,
                                                            std::optional<std::string_view> purpose, Timestamp now,
                                                            LegacyDecoder decode_legacy_message) {
  if (bytes.starts_with(R"({"_rails":{"message":)")) {
    auto envelope = json::parse(bytes);
    if (!envelope) return std::unexpected(Error::InvalidSignature);
    auto rails = extract(*envelope, purpose, now);
    if (!rails) return std::unexpected(rails.error());
    const json::Value* message = member(**rails, "message");
    if (message == nullptr || !message->is_string()) return std::unexpected(Error::InvalidSignature);
    auto dumped = decode_legacy_message(message->as_string());
    if (!dumped) return std::unexpected(Error::InvalidSignature);
    return serializer.load(*dumped);
  }
  auto value = serializer.load(bytes);
  if (!value) return value;
  if (value->is_object() && value->find("_rails") != nullptr) {
    auto rails = extract(*value, purpose, now);
    if (!rails) return std::unexpected(rails.error());
    const json::Value* data = member(**rails, "data");
    return data ? *data : json::Value(nullptr);
  }
  if (!purpose) return value;
  return std::unexpected(Error::PurposeMismatch);
}

}  // namespace campfire::compat
