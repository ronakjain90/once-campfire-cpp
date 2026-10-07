// GlobalID and SignedGlobalID: gid://campfire/User/1 and user.attachable_sgid
// (Rust: crates/rails_compat/src/global_id.rs; Rails: reference/lib/rails_ext/action_text_attachables.rb).
//
// SGIDs use GlobalID::Verifier: key "signed_global_ids", HMAC-SHA1, URL-safe Base64 with
// padding, the :json_allow_marshal serializer and the {"_rails":{"data":..}} envelope.
#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "compat/secrets.hpp"
#include "compat/time.hpp"

namespace campfire::compat::global_id {

inline constexpr std::string_view kApp = "campfire";                  // GlobalID.app
inline constexpr std::string_view kAttachablePurpose = "attachable";  // ActionText::Attachable::LOCATOR_NAME
inline constexpr std::string_view kDefaultPurpose = "default";        // SignedGlobalID::DEFAULT_PURPOSE

// A parsed gid://<app>/<Model>/<id>. Query parameters (Rails writes "?expires_in" into
// attachable SGIDs) are dropped: the locator ignores them.
struct GlobalId {
  std::string app;
  std::string model_name;
  std::string id;

  static GlobalId make(std::string_view model_name, std::string_view id);
  static std::optional<GlobalId> parse(std::string_view gid);
  std::string to_string() const;
  // GlobalID#to_param: URL-safe Base64 without padding (used in Turbo stream names).
  std::string to_param() const;
  static std::optional<GlobalId> from_param(std::string_view param);
  friend bool operator==(const GlobalId&, const GlobalId&) = default;
};

// record.attachable_sgid: to_sgid(expires_in: nil, for: "attachable"). GlobalID turns the
// leftover expires_in: nil into a query parameter, so the signed data is
// "gid://campfire/User/1?expires_in", with no expiry in the envelope.
std::string attachable_sgid(const Secrets& secrets, const GlobalId& gid);
// SignedGlobalID.new(gid_uri, for: purpose, expires_at:) for a bare GID URI.
std::string sgid(const Secrets& secrets, const GlobalId& gid, std::string_view purpose,
                 std::optional<Timestamp> expires_at);
// SignedGlobalID.parse(sgid, for: purpose): the GID if signature, purpose and expiry check out.
// Looking up the record is the caller's job.
std::optional<GlobalId> locate_signed(const Secrets& secrets, std::string_view sgid, std::string_view purpose,
                                      Timestamp now);

// The errors Ruby raises while attachable_from_possibly_expired_sgid reads a bad SGID.
enum class UnverifiedSgidError { JsonParserError, ArgumentError, TypeError, NoMethodError };

// The first half of attachable_from_possibly_expired_sgid. It reads the GID out of an SGID
// WITHOUT checking the signature. Use it only for User attachments: the caller must look up
// the GID and accept the record only when its model is "User". Never use it for another
// model. nullopt means "no GID", where Rails finds nothing. An error is where Rails raises.
std::expected<std::optional<GlobalId>, UnverifiedSgidError> gid_from_unverified_sgid(
    std::optional<std::string_view> sgid);

}  // namespace campfire::compat::global_id
