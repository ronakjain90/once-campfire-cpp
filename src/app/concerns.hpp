// The concerns of ApplicationController as before action functions. Rails: app/controllers/concerns/*
// (Authentication, BlockBannedRequests, SetCurrentRequest, VersionHeaders, AllowBrowser).
// Rust: crates/campfire/src/concerns.rs.
//
// Ruby includes the list right to left, so the chain that the reference runs is:
//   1. set_version_headers   2. set_current_request   3. reject_banned_ip (not GET or HEAD)
//   4. require_authentication   5. deny_bots   6. verify_authenticity_token   7. allow_browser
// `before_actions` runs 1 to 7 with the skips of a controller, then the controller runs its own.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "app/flow.hpp"
#include "app/rq.hpp"
#include "core/task.hpp"

namespace campfire::app::concerns {

enum class Authentication : std::uint8_t {
  Required,               // require_authentication
  Skipped,                // allow_unauthenticated_access
  RequireUnauthenticated  // require_unauthenticated_access: skip, then restore_authentication and redirect a signed in
                          // user
};

// How the declarations of a controller change the chain for one action.
struct Before {
  Authentication authentication = Authentication::Required;
  bool deny_bots = true;           // `allow_bot_access` turns it off
  bool forgery_protection = true;  // `skip_forgery_protection` turns it off

  [[nodiscard]] Before allow_unauthenticated_access() const {
    return {Authentication::Skipped, deny_bots, forgery_protection};
  }
  [[nodiscard]] Before require_unauthenticated_access() const {
    return {Authentication::RequireUnauthenticated, deny_bots, forgery_protection};
  }
  [[nodiscard]] Before allow_bot_access() const { return {authentication, false, forgery_protection}; }
  [[nodiscard]] Before skip_forgery_protection() const { return {authentication, deny_bots, false}; }
};

// The hook for `allow_browser`. A1 sets it from the user agent rules. The default passes every browser.
using AllowBrowserFn = Task<Flow<void>> (*)(Rq&);
void set_allow_browser(AllowBrowserFn fn) noexcept;

[[nodiscard]] Task<Flow<void>> before_actions(Rq& rq, Before before);

// Pieces that controllers call by themselves.
void set_version_headers(Rq& rq);
[[nodiscard]] Task<Flow<void>> reject_banned_ip(Rq& rq);
[[nodiscard]] Task<Flow<void>> require_authentication(Rq& rq);
// Resumes the session of the `session_token` cookie. Returns false if there is none.
[[nodiscard]] Task<Flow<bool>> restore_authentication(Rq& rq);
[[nodiscard]] Flow<void> verify_authenticity_token(Rq& rq);
[[nodiscard]] Flow<void> deny_bots(Rq& rq);
[[nodiscard]] Flow<void> ensure_can_administer(Rq& rq);
// `head status` from a before action: `text/html`, no charset (Rails sets the formats later).
[[nodiscard]] net::Response head_in_before_action(Rq& rq, int status);

// `User.active.authenticate_by(email_address:, password:)`: the lookup is inline, bcrypt runs on the job pool.
[[nodiscard]] Task<Flow<std::optional<models::User>>> authenticate_by(Rq& rq, std::string email_address,
                                                                      std::string password);
// `start_new_session_for(user)`: the session row through the writer, and the signed cookie.
[[nodiscard]] Task<Flow<void>> start_new_session_for(Rq& rq, models::User user);
// `terminate_current_session`: destroys the session, resets the Rails session, deletes the cookie.
[[nodiscard]] Task<Flow<void>> terminate_current_session(Rq& rq);
// `post_authenticating_url`: `session.delete(:return_to_after_authenticating) || root_url`.
[[nodiscard]] std::string post_authenticating_url(Rq& rq);

}  // namespace campfire::app::concerns
