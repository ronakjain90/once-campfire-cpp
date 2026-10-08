// AllowBrowser. Rails: app/controllers/concerns/allow_browser.rb (`allow_browser versions:, block:`),
// views/sessions/incompatible_browser. Rust: crates/campfire/src/concerns.rs (allow_browser,
// render_incompatible_browser).
#include "app/concerns.hpp"
#include "app/platform.hpp"
#include "app/render_page.hpp"
#include "app/user_agent.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::concerns {

// Rendered from a before action: the page is HTML whatever the request format, and no format is looked up.
// A controller with its own layout (MessagesController) always uses the application layout: the messages area (A3)
// must call `set_allow_browser` for that case.
Task<Flow<void>> allow_browser(Rq& rq) {
  const std::string_view header = rq.user_agent();
  if (!ua::is_present(header)) co_return Flow<void>{};
  // A pure function of the header: the worker keeps the answer for headers of a usual size.
  const auto blocked = [&] { return ApplicationPlatform(header).browser_blocked(); };
  if (header.size() <= 512) {
    const std::string key = "ua-blocked:" + std::string(header);
    if (rq.worker.memo(key, [&] { return std::string(blocked() ? "1" : "0"); }) == "0") co_return Flow<void>{};
  } else if (!blocked()) {
    co_return Flow<void>{};
  }
  const ApplicationPlatform platform(header);
  PageSpec spec;
  spec.name = "sessions#incompatible_browser";
  spec.title = platform.apple_messages() ? "Campfire" : "Unsupported browser";
  spec.content = [](Out& out, const views::ViewContext& ctx) { views::sessions::incompatible_browser(out, ctx); };
  auto response = render_page(rq, 200, spec);
  if (!response) co_return std::unexpected(std::move(response.error()));
  co_return halt(std::move(*response));
}

}  // namespace campfire::app::concerns
