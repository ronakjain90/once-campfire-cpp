// ActiveStorage::Variation (see variation.hpp).
#include "compat/variation.hpp"

#include "compat/base64.hpp"
#include "compat/crypto.hpp"

namespace campfire::compat {
namespace {

json::Value to_json(const marshal::Value& value) {
  return std::visit(
      [](const auto& x) -> json::Value {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, marshal::Value::Nil>) {
          return nullptr;
        } else if constexpr (std::is_same_v<T, bool>) {
          return x;
        } else if constexpr (std::is_same_v<T, int64_t>) {
          return x;
        } else if constexpr (std::is_same_v<T, marshal::Value::Symbol>) {
          return x.name;
        } else if constexpr (std::is_same_v<T, marshal::Value::Str>) {
          return x.text;
        } else if constexpr (std::is_same_v<T, marshal::Value::Array>) {
          json::Value::Array items;
          for (const auto& item : x) items.push_back(to_json(item));
          return items;
        } else {
          json::Value::Object members;
          for (const auto& [k, v] : x) members.emplace_back(k, to_json(v));
          return members;
        }
      },
      value.variant());
}

std::optional<marshal::Value> from_json(const json::Value& json) {
  if (json.is_null()) return marshal::Value::nil();
  if (json.is_bool()) return marshal::Value::boolean(json.as_bool());
  if (auto n = json.to_int64()) return marshal::Value::integer(*n);
  if (json.is_number()) return std::nullopt;  // floats and huge integers are unsupported
  if (json.is_string()) return marshal::Value::string(json.as_string());
  if (json.is_array()) {
    marshal::Value::Array items;
    for (const auto& item : json.as_array()) {
      auto v = from_json(item);
      if (!v) return std::nullopt;
      items.push_back(std::move(*v));
    }
    return marshal::Value::array(std::move(items));
  }
  marshal::Value::Hash entries;
  for (const auto& [k, v] : json.as_object()) {
    auto item = from_json(v);
    if (!item) return std::nullopt;
    entries.emplace_back(k, std::move(*item));
  }
  return marshal::Value::hash(std::move(entries));
}

marshal::Value as_hash(const Variation::Transformations& t) {
  return marshal::Value::hash(t);
}

}  // namespace

Variation Variation::resize_to_limit(int64_t width, int64_t height, std::optional<std::string_view> format) {
  Transformations t;
  t.emplace_back("resize_to_limit",
                 marshal::Value::array({marshal::Value::integer(width), marshal::Value::integer(height)}));
  if (format) t.emplace_back("format", marshal::Value::symbol(std::string(*format)));
  return Variation(std::move(t));
}

std::string Variation::marshal() const {
  return marshal::dump(as_hash(transformations_));
}

std::string Variation::digest() const {
  return base64::strict_encode(crypto::sha1(marshal()));
}

std::string Variation::key(const MessageVerifier& verifier) const {
  return verifier.generate_raw(json::encode(to_json(as_hash(transformations_))), "variation");
}

std::optional<Variation> Variation::decode(const MessageVerifier& verifier, std::string_view key, Timestamp now) {
  auto text = verifier.verify_raw(key, "variation", now);
  if (!text) return std::nullopt;
  auto parsed = json::parse(*text);
  if (!parsed || !parsed->is_object()) return std::nullopt;
  auto value = from_json(*parsed);
  if (!value) return std::nullopt;
  const auto* hash = std::get_if<marshal::Value::Hash>(&value->variant());
  if (hash == nullptr) return std::nullopt;
  return Variation(*hash);
}

}  // namespace campfire::compat
