// PwaController. Rails: app/controllers/pwa_controller.rb, app/views/pwa/*. Rust: crates/campfire/src/controllers/
// pwa.rs, crates/views/src/pwa.rs.
#include "app/concerns.hpp"
#include "app/dispatch.hpp"
#include "assets/assets.hpp"
#include "compat/json.hpp"
#include "core/time_format.hpp"
#include "models/account.hpp"
#include "routes/routes.hpp"

namespace campfire::app::controllers {

namespace {

// `pwa/service_worker.js`, served as it is.
constexpr std::string_view kServiceWorker = R"JS(
self.addEventListener("push", async (event) => {
  const data = await event.data.json()
  event.waitUntil(Promise.all([ showNotification(data), updateBadgeCount(data.options) ]))
})

async function showNotification({ title, options }) {
  return self.registration.showNotification(title, options)
}

async function updateBadgeCount({ data: { badge } }) {
  return self.navigator.setAppBadge?.(badge || 0)
}

self.addEventListener("notificationclick", (event) => {
  event.notification.close()

  const url = new URL(event.notification.data.path, self.location.origin).href
  event.waitUntil(openURL(url))
})

async function openURL(url) {
  const clients = await self.clients.matchAll({ type: "window" })
  const focused = clients.find((client) => client.focused)

  if (focused) {
    await focused.navigate(url)
  } else {
    await self.clients.openWindow(url)
  }
}
)JS";

// `skip_forgery_protection`, `allow_unauthenticated_access`
concerns::Before pwa_before() {
  return concerns::Before{}.allow_unauthenticated_access().skip_forgery_protection();
}

Task<Flow<net::Response>> pwa_service_worker(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, pwa_before());
  if (!before) co_return std::unexpected(std::move(before.error()));
  const req::Format offered[] = {&req::mime::JS};
  if (auto format = rq.respond_to(offered); !format) co_return std::unexpected(std::move(format.error()));
  Out out(rq.ctx.resource());
  out.append_raw(kServiceWorker.substr(1));  // the raw string starts after a newline
  co_return rq.render_as(200, "text/javascript; charset=utf-8", std::move(out));
}

// A string as a JSON value, quotes included. ERB escaped HTML in the Rails manifest: the Rust port does not.
std::string json_string(std::string_view text) {
  return compat::json::generate(compat::json::Value(std::string(text)));
}

// `image_url(source)`
std::string image_url(const Rq& rq, std::string_view logical) {
  const auto path = assets::asset_path(logical);
  return rq.url_for(path ? *path : "/assets/" + std::string(logical));
}

Task<Flow<net::Response>> pwa_manifest(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, pwa_before());
  if (!before) co_return std::unexpected(std::move(before.error()));
  const req::Format offered[] = {&req::mime::JSON};
  if (auto format = rq.respond_to(offered); !format) co_return std::unexpected(std::move(format.error()));
  auto account = models::accounts::first(rq.db(), rq.arena());
  if (!account) co_return fail_internal(account.error().message);
  std::string version;
  std::string name = "Campfire";
  if (*account) {
    name = (*account)->name;
    if (const auto updated = parse_db((*account)->updated_at)) version = format_to_fs_number(*updated);
  }
  const std::optional<std::string_view> v = version.empty() ? std::nullopt : std::optional<std::string_view>(version);
  const std::string small = json_string(campfire::routes::fresh_account_logo(v, "small"));
  const std::string large = json_string(campfire::routes::fresh_account_logo(v));
  const auto icon = [&](std::string_view logical) { return json_string(image_url(rq, logical)); };
  std::string body;
  body += "{\n  \"name\": " + json_string(name) + ",\n";
  body += "  \"icons\": [\n    {\n      \"src\": " + small +
          ",\n      \"type\": \"image/png\",\n      \"sizes\": \"192x192\"\n    },\n";
  body +=
      "    {\n      \"src\": " + large + ",\n      \"type\": \"image/png\",\n      \"sizes\": \"512x512\"\n    },\n";
  body += "    {\n      \"src\": " + large +
          ",\n      \"type\": \"image/png\",\n      \"sizes\": \"512x512\",\n      \"purpose\": \"maskable\"\n    }\n  "
          "],\n";
  body += R"J(  "start_url": "/",
  "display": "standalone",
  "scope": "/",
  "description": "A chat app from the makers of Basecamp and HEY.",
  "categories": ["social", "business", "productivity"],
  "theme_color": "#ffffff",
  "background_color": "#ffffff",
  "shortcuts": [
    {
      "name": "New chat room",
      "description": "Open Campfire and start a new chat room",
      "url": "rooms/opens/new",
      "icons": [{ "src": )J" +
          icon("add.svg") + R"J(, "sizes": "any" }]
    },
    {
      "name": "My profile",
      "description": "Open Campfire and view your profile",
      "url": "/users/me/profile",
      "icons": [{ "src": )J" +
          icon("person.svg") + R"J(, "sizes": "any" }]
    }
  ],
  "screenshots": [
    {
      "src": )J" +
          icon("screenshots/android-chat.png") + R"J(,
      "sizes": "1080x2400",
      "form_factor": "narrow",
      "label": "Campfire is an installable, self-hosted group chat system."
    },
    {
      "src": )J" +
          icon("screenshots/android-sidebar.png") + R"J(,
      "sizes": "1080x2400",
      "form_factor": "narrow",
      "label": "Easily invite people. Make rooms. @mentions, DMs, and mobile support."
    },
    {
      "src": )J" +
          icon("screenshots/android-dark-mode.png") + R"J(,
      "sizes": "1080x2400",
      "form_factor": "narrow",
      "label": "Full support for dark mode, customizable to your brand."
    }
  ]
}
)J";
  Out out(rq.ctx.resource());
  out.append_raw(body);
  co_return rq.render_as(200, "application/json; charset=utf-8", std::move(out));
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::pwa {

Task<net::Response> manifest(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::pwa_manifest);
}

Task<net::Response> service_worker(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::pwa_service_worker);
}

}  // namespace campfire::routes::pwa
