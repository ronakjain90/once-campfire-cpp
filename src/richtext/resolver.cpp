// Rails: globalid locator.rb, signed_global_id.rb. Rust: crates/rails_compat/src/global_id.rs
#include "richtext/resolver.hpp"

#include <charconv>

namespace campfire::richtext {

namespace {

// The id of a GID as `id.parse::<i64>()` reads it: an optional sign and digits.
std::optional<std::int64_t> parse_id(std::string_view text) {
  if (text.starts_with('+')) {
    text.remove_prefix(1);
  }
  std::int64_t value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc() || end != text.data() + text.size() || text.empty()) {
    return std::nullopt;
  }
  return value;
}

}  // namespace

SignedLookup CompatResolver::locate_signed(std::string_view sgid) const {
  SignedLookup out;
  auto gid = compat::global_id::locate_signed(secrets_, sgid, compat::global_id::kAttachablePurpose, now_);
  if (!gid) {
    return out;  // Invalid
  }
  const auto id = parse_id(gid->id);
  if (gid->model_name == "User" && id) {
    if (auto user = records_.user(*id)) {
      out.kind = SignedLookup::Kind::User;
      out.user = std::move(*user);
      return out;
    }
  }
  out.kind = SignedLookup::Kind::MissingRecord;
  out.model_name = gid->model_name;
  return out;
}

GidLookup CompatResolver::find_gid(const compat::global_id::GlobalId& gid) const {
  GidLookup out;
  const auto id = parse_id(gid.id);
  if (!id) {
    return out;
  }
  if (gid.model_name == "User") {
    if (auto user = records_.user(*id)) {
      out.kind = GidLookup::Kind::User;
      out.user = std::move(*user);
    }
  } else if (records_.record_exists(gid.model_name, *id)) {
    out.kind = GidLookup::Kind::OtherModel;
  }
  return out;
}

}  // namespace campfire::richtext
