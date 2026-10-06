// ApplicationPlatform and AllowBrowser. Rails: app/models/application_platform.rb, concerns/allow_browser.rb.
// Rust: crates/campfire/src/concerns/platform.rs.
#include "app/platform.hpp"

#include <algorithm>
#include <array>

namespace campfire::app {

namespace {

bool contains(std::string_view s, std::string_view needle) {
  return s.find(needle) != std::string_view::npos;
}

// `user_agent.browser.match?(/A|B/)`: raises for a nil browser.
ua::Rb<bool> browser_matches(const ua::Rb<std::optional<std::string>>& browser, std::initializer_list<std::string_view> names) {
  if (!browser || !*browser) return std::unexpected(ua::Raised{});
  return std::ranges::any_of(names, [&](std::string_view n) { return contains(**browser, n); });
}

}  // namespace

ApplicationPlatform::ApplicationPlatform(std::string_view user_agent) : user_agent_(user_agent), agent_(ua::parse(user_agent_)) {}

bool ApplicationPlatform::ios() const {
  return contains(user_agent_, "iPhone") || contains(user_agent_, "iPad");
}
bool ApplicationPlatform::android() const {
  return contains(user_agent_, "Android");
}
bool ApplicationPlatform::mac() const {
  return contains(user_agent_, "Macintosh");
}

// Apple Messages link previews claim to be the Facebook and the Twitter bot.
bool ApplicationPlatform::apple_messages() const {
  const std::string lower = ua::downcase(user_agent_);
  return contains(lower, "facebookexternalhit") && contains(lower, "twitterbot");
}

ua::Rb<std::optional<std::string>> ApplicationPlatform::try_operating_system() const {
  const auto platform = agent_.try_platform();
  if (!platform) return std::unexpected(ua::Raised{});
  const std::string name = platform->value_or("");
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 6> kNamed = {{
      {"Android", "Android"}, {"iPad", "iPad"}, {"iPhone", "iPhone"}, {"Macintosh", "macOS"}, {"Windows", "Windows"}, {"CrOS", "ChromeOS"}}};
  for (const auto& [needle, os_name] : kNamed) {
    if (contains(name, needle)) return std::optional<std::string>(std::string(os_name));
  }
  const auto os = agent_.try_os();
  if (!os) return std::unexpected(ua::Raised{});
  if (!*os) return std::optional<std::string>{};
  return std::optional<std::string>(contains(**os, "Linux") ? std::string("Linux") : **os);
}

views::Platform ApplicationPlatform::to_view() const {
  const auto browser = agent_.try_browser();
  const auto os = try_operating_system();
  const auto is = [&](std::initializer_list<std::string_view> names) { return browser_matches(browser, names).value_or(false); };
  views::Platform p;
  p.ios = ios();
  p.android = android();
  p.mac = mac();
  p.windows = os && *os && **os == "Windows";
  p.chrome = is({"Chrome"});
  p.firefox = is({"Firefox", "FxiOS"});
  p.safari = is({"Safari"});
  p.edge = is({"Edg"});
  p.mobile = mobile();
  p.desktop = desktop();
  p.apple_messages = apple_messages();
  if (browser && *browser) p.browser = **browser;
  if (os && *os) p.operating_system = **os;
  return p;
}

bool ApplicationPlatform::browser_blocked() const {
  return try_browser_blocked().value_or(false);
}

// `ActionController::AllowBrowser::BrowserBlocker#blocked?` with `{ safari: 17.2, chrome: 120, firefox: 121, opera: 104, ie: false }`.
ua::Rb<bool> ApplicationPlatform::try_browser_blocked() const {
  if (!ua::is_present(user_agent_)) return false;
  const auto version = agent_.try_version();
  if (!version) return std::unexpected(ua::Raised{});
  if (!*version || !(*version)->is_present()) return false;
  const auto name = agent_.try_browser();
  if (!name) return std::unexpected(ua::Raised{});
  if (!*name) return std::unexpected(ua::Raised{});
  const std::string browser = ua::downcase(**name);
  // A null minimum is `ie: false`: always blocked.
  std::optional<std::string_view> minimum;
  if (browser == "safari") {
    minimum = "17.2";
  } else if (browser == "chrome") {
    minimum = "120";
  } else if (browser == "firefox") {
    minimum = "121";
  } else if (browser == "opera") {
    minimum = "104";
  } else if (browser != "internet explorer") {
    return false;
  }
  const bool below = !minimum || (*version)->less(ua::Version(*minimum));
  return below && !agent_.is_bot();
}

}  // namespace campfire::app
