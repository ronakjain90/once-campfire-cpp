// `ApplicationPlatform` (Rails: app/models/application_platform.rb over platform_agent 1.0.1) and the
// `allow_browser` check (app/controllers/concerns/allow_browser.rb). Rust: crates/campfire/src/concerns/platform.rs.
#pragma once

#include <string>
#include <string_view>

#include "app/user_agent.hpp"
#include "views/context.hpp"

namespace campfire::app {

class ApplicationPlatform {
 public:
  // `ApplicationPlatform.new(request.user_agent)`: no header parses like "".
  explicit ApplicationPlatform(std::string_view user_agent);

  [[nodiscard]] bool ios() const;
  [[nodiscard]] bool android() const;
  [[nodiscard]] bool mac() const;
  [[nodiscard]] bool apple_messages() const;
  [[nodiscard]] bool mobile() const { return ios() || android(); }
  [[nodiscard]] bool desktop() const { return !mobile(); }
  [[nodiscard]] const ua::Agent& agent() const noexcept { return agent_; }
  [[nodiscard]] ua::Rb<std::optional<std::string>> try_operating_system() const;
  // The platform as the views see it. A predicate that raises in Ruby (a nil browser) answers false.
  [[nodiscard]] views::Platform to_view() const;
  // `AllowBrowser::VERSIONS` as `allow_browser` applies it. Rails raises (a 500) for a versioned agent with a nil
  // browser. This answers false then, as the Rust port does.
  [[nodiscard]] bool browser_blocked() const;

 private:
  [[nodiscard]] ua::Rb<bool> try_browser_blocked() const;
  std::string user_agent_;
  ua::Agent agent_;
};

}  // namespace campfire::app
