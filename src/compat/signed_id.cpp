// Signed ids (see signed_id.hpp).
#include "compat/signed_id.hpp"

#include "compat/ruby.hpp"

namespace campfire::compat::signed_id {
namespace {

// String#underscore for class names: Rooms::Open -> rooms/open, WebPush -> web_push.
std::string underscore(std::string_view name) {
  std::string flat;
  for (size_t i = 0; i < name.size(); ++i) {
    if (name[i] == ':' && i + 1 < name.size() && name[i + 1] == ':') {
      flat += '/';
      ++i;
    } else {
      flat += name[i];
    }
  }
  auto upper = [](char c) { return c >= 'A' && c <= 'Z'; };
  auto lower = [](char c) { return c >= 'a' && c <= 'z'; };
  std::string out;
  for (size_t i = 0; i < flat.size(); ++i) {
    char c = flat[i];
    if (upper(c)) {
      bool prev_lower_or_digit = i > 0 && (lower(flat[i - 1]) || (flat[i - 1] >= '0' && flat[i - 1] <= '9'));
      bool acronym_end = i > 0 && upper(flat[i - 1]) && i + 1 < flat.size() && lower(flat[i + 1]);
      if (prev_lower_or_digit || acronym_end) out += '_';
      out += char(c + 32);
    } else {
      out += c == '-' ? '_' : c;
    }
  }
  return out;
}

bool blank(std::string_view s) {
  for (char c : s) {
    if (!(c == ' ' || (c >= '\t' && c <= '\r'))) return false;
  }
  return true;
}

}  // namespace

std::string combine_purposes(std::string_view model_name, std::optional<std::string_view> purpose) {
  std::string out;
  for (const std::string& part : {underscore(model_name), std::string(purpose.value_or(""))}) {
    if (blank(part)) continue;
    if (!out.empty()) out += '/';
    out += part;
  }
  return out;
}

std::string generate(const Secrets& secrets, std::string_view model_name, int64_t id,
                     std::optional<std::string_view> purpose, std::optional<Timestamp> expires_at) {
  return secrets.signed_id_verifier().generate(json::Value(id), combine_purposes(model_name, purpose), expires_at);
}

std::optional<int64_t> verify(const Secrets& secrets, std::string_view model_name, std::string_view signed_id,
                              std::optional<std::string_view> purpose, Timestamp now) {
  auto value = secrets.signed_id_verifier().verify(signed_id, combine_purposes(model_name, purpose), now);
  if (!value) return std::nullopt;
  if (auto n = value->to_int64()) return n;
  // find_by(id: "7") casts the string as Active Record does.
  if (const std::string* s = value->get_string()) return integer_cast(*s);
  return std::nullopt;
}

std::string blob_signed_id(const Secrets& secrets, int64_t blob_id, std::optional<Timestamp> expires_at) {
  return secrets.active_storage_verifier().generate_raw(std::to_string(blob_id), "blob_id", expires_at);
}

std::optional<int64_t> verify_blob_signed_id(const Secrets& secrets, std::string_view signed_id, Timestamp now) {
  auto json_text = secrets.active_storage_verifier().verify_raw(signed_id, "blob_id", now);
  if (!json_text) return std::nullopt;
  auto value = json::parse(*json_text);
  return value ? value->to_int64() : std::nullopt;
}

}  // namespace campfire::compat::signed_id
