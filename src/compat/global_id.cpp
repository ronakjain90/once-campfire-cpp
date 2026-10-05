// GlobalID and SignedGlobalID (see global_id.hpp).
#include "compat/global_id.hpp"

#include "compat/base64.hpp"
#include "compat/ruby.hpp"

namespace campfire::compat::global_id {

GlobalId GlobalId::make(std::string_view model_name, std::string_view id) {
  return GlobalId{std::string(kApp), std::string(model_name), std::string(id)};
}

std::optional<GlobalId> GlobalId::parse(std::string_view gid) {
  if (!gid.starts_with("gid://")) return std::nullopt;
  gid.remove_prefix(6);
  gid = gid.substr(0, gid.find('?'));
  size_t slash = gid.find('/');
  if (slash == std::string_view::npos) return std::nullopt;
  std::string_view app = gid.substr(0, slash);
  std::string_view path = gid.substr(slash + 1);
  size_t slash2 = path.find('/');
  if (slash2 == std::string_view::npos) return std::nullopt;
  std::string_view model = path.substr(0, slash2);
  std::string_view id = path.substr(slash2 + 1);
  // URI::GID allows no "/" in the id.
  if (app.empty() || model.empty() || id.empty() || id.find('/') != std::string_view::npos) return std::nullopt;
  return GlobalId{std::string(app), std::string(model), std::string(id)};
}

std::string GlobalId::to_string() const { return "gid://" + app + "/" + model_name + "/" + id; }

std::string GlobalId::to_param() const { return base64::urlsafe_encode_unpadded(to_string()); }

std::optional<GlobalId> GlobalId::from_param(std::string_view param) {
  auto decoded = base64::urlsafe_decode(param);
  if (!decoded) return std::nullopt;
  return parse(*decoded);
}

std::string attachable_sgid(const Secrets& secrets, const GlobalId& gid) {
  return secrets.global_id_verifier().generate(json::Value(gid.to_string() + "?expires_in"), kAttachablePurpose,
                                               std::nullopt);
}

std::string sgid(const Secrets& secrets, const GlobalId& gid, std::string_view purpose,
                 std::optional<Timestamp> expires_at) {
  return secrets.global_id_verifier().generate(json::Value(gid.to_string()), purpose, expires_at);
}

namespace {

// globalid < 1.0 signed {"gid":..,"purpose":..,"expires_at":..} without a Rails envelope.
std::optional<json::Value> verify_legacy_self_validated(const MessageVerifier& verifier, std::string_view sgid,
                                                        std::string_view purpose, Timestamp now) {
  auto metadata = verifier.verify(sgid, std::nullopt, now);
  if (!metadata || !metadata->is_object()) return std::nullopt;
  if (const json::Value* expires = metadata->find("expires_at"); expires != nullptr && !expires->is_null()) {
    const std::string* text = expires->get_string();
    if (text == nullptr) return std::nullopt;
    auto at = parse_iso8601(*text);
    if (!at) return std::nullopt;
    if (now > *at) return std::nullopt;
  }
  if (ruby_to_s(metadata->find("purpose")) != purpose) return std::nullopt;
  const json::Value* gid = metadata->find("gid");
  return gid ? *gid : json::Value(nullptr);
}

}  // namespace

std::optional<GlobalId> locate_signed(const Secrets& secrets, std::string_view sgid, std::string_view purpose,
                                      Timestamp now) {
  const MessageVerifier& verifier = secrets.global_id_verifier();
  std::optional<json::Value> data;
  if (auto verified = verifier.verify(sgid, purpose, now)) data = std::move(*verified);
  else data = verify_legacy_self_validated(verifier, sgid, purpose, now);
  if (!data) return std::nullopt;
  const std::string* uri = data->get_string();
  if (uri == nullptr) return std::nullopt;
  if (auto gid = GlobalId::parse(*uri)) return gid;
  return GlobalId::from_param(*uri);
}

namespace {

// Ruby's parser reads invalid UTF-8 inside strings as it is. Replace the bad bytes so ours does too.
std::string lossy_utf8(std::string_view s) {
  if (json::valid_utf8(s)) return std::string(s);
  std::string out;
  for (size_t i = 0; i < s.size();) {
    size_t n = 1;
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0x80) {
      for (size_t len = 4; len >= 2; --len) {
        if (i + len <= s.size() && json::valid_utf8(s.substr(i, len))) {
          n = len;
          break;
        }
      }
      if (n == 1) {
        out += "\xEF\xBF\xBD";
        ++i;
        continue;
      }
    }
    out.append(s.data() + i, n);
    i += n;
  }
  return out;
}

std::optional<std::string> decode_base64(std::string_view message) {
  if (auto strict = base64::strict_decode(message)) return strict;
  return base64::urlsafe_decode(message);
}

// The regexp %r{(gid://campfire/[^/]+/\d+)} on bytes, first match.
std::optional<std::string> match_marshaled_gid(std::string_view bytes) {
  constexpr std::string_view kPrefix = "gid://campfire/";
  size_t from = 0;
  while (true) {
    size_t at = bytes.find(kPrefix, from);
    if (at == std::string_view::npos) return std::nullopt;
    size_t p = at + kPrefix.size();
    size_t model_end = p;
    while (model_end < bytes.size() && bytes[model_end] != '/') ++model_end;
    if (model_end > p && model_end < bytes.size()) {
      size_t d = model_end + 1;
      size_t digits_end = d;
      while (digits_end < bytes.size() && bytes[digits_end] >= '0' && bytes[digits_end] <= '9') ++digits_end;
      if (digits_end > d) return std::string(bytes.substr(at, digits_end - at));
    }
    from = at + 1;
  }
}

bool truthy(const json::Value* v) { return v != nullptr && !v->is_null() && !(v->is_bool() && !v->as_bool()); }

}  // namespace

std::expected<std::optional<GlobalId>, UnverifiedSgidError> gid_from_unverified_sgid(
    std::optional<std::string_view> sgid) {
  if (!sgid) return std::nullopt;
  // sgid.split("--").first: Ruby drops trailing empty fields, so "" and "--" have no first.
  std::vector<std::string_view> fields;
  std::string_view rest = *sgid;
  while (true) {
    size_t at = rest.find("--");
    if (at == std::string_view::npos) break;
    fields.push_back(rest.substr(0, at));
    rest.remove_prefix(at + 2);
  }
  fields.push_back(rest);
  while (!fields.empty() && fields.back().empty()) fields.pop_back();
  if (fields.empty()) return std::nullopt;

  auto decoded = decode_base64(fields.front());
  if (!decoded) return std::unexpected(UnverifiedSgidError::ArgumentError);
  auto parsed = json::parse(lossy_utf8(*decoded), {.allow_comments = true});
  if (!parsed) return std::unexpected(UnverifiedSgidError::JsonParserError);
  if (parsed->is_array()) return std::unexpected(UnverifiedSgidError::TypeError);
  if (!parsed->is_object()) return std::unexpected(UnverifiedSgidError::NoMethodError);
  const json::Value* rails = parsed->find("_rails");
  if (rails != nullptr && !rails->is_null() && !rails->is_object()) {
    return std::unexpected(UnverifiedSgidError::TypeError);
  }
  const json::Value* data = rails && rails->is_object() ? rails->find("data") : nullptr;
  const json::Value* message = rails && rails->is_object() ? rails->find("message") : nullptr;
  std::optional<std::string> gid;
  if (truthy(data)) {
    // GlobalID.find of anything but a string finds nothing.
    if (const std::string* s = data->get_string()) gid = *s;
  } else if (truthy(message)) {
    // Rails 7 marshaled the GID. The signature is not checked, so the dump cannot be loaded
    // safely: the GID is matched out of its bytes.
    const std::string* s = message->get_string();
    if (s == nullptr) return std::unexpected(UnverifiedSgidError::NoMethodError);
    auto bytes = decode_base64(*s);
    if (!bytes) return std::unexpected(UnverifiedSgidError::ArgumentError);
    gid = match_marshaled_gid(*bytes);
  }
  if (!gid) return std::nullopt;
  return GlobalId::parse(*gid);
}

}  // namespace campfire::compat::global_id
