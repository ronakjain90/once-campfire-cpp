// `Webhook#deliver` for bots: the request, the 7-second timeouts and the reply. Rails: app/models/webhook.rb. Rust:
// crates/campfire/src/integrations/webhook.rs.
//
// The webhook is not guarded, unlike the link unfurl: only an administrator sets the URL, and it may point at an
// internal service. The name resolves in the normal way and nothing is pinned.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "app/unfurl_http.hpp"
#include "core/error.hpp"
#include "jobs/webhook.hpp"

namespace campfire::app::webhook {

struct Delivery {
  std::optional<int> status;  // nothing when the request timed out
  jobs::webhook::Reply reply;
};

// Posts `payload` to `url` and gives what the bot answered. A timeout is a delivery with a text reply. A bad URL, a
// failed connect, an invalid reply type or a reply that is too large is an error: the job fails, as in Rails.
// `deadline` is the longest that the whole delivery may take.
[[nodiscard]] Result<Delivery> deliver(const unfurl::Network& network, std::string_view url, std::string payload,
                                       std::chrono::seconds deadline = jobs::webhook::kDeliveryDeadline);

}  // namespace campfire::app::webhook
