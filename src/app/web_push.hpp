// Web Push delivery: `WebPush::Notification#deliver` over the pinned address of a permitted push service. Rails:
// reference/lib/web_push, config/initializers/web_push.rb, app/models/push/subscription.rb. Rust:
// crates/campfire/src/integrations/web_push.rs.
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include "app/app.hpp"
#include "app/unfurl_http.hpp"
#include "jobs/web_push.hpp"
#include "models/push_subscription.hpp"

namespace campfire::app::web_push {

// `Push::Subscription#notification(title:, body:, path:)`: the badge is counted when the job runs.
struct Notification {
  std::string title;
  std::string body;
  std::string path;
  std::int64_t badge = 0;
  models::PushSubscription subscription;

  // `encoded_message`
  [[nodiscard]] std::string encoded_message() const;
};

// What `WebPush.payload_send` raised.
struct DeliveryError {
  enum class Kind : std::uint8_t {
    SubscriptionGone,        // 404 and 410: the push service does not know the subscription
    Response,                // the other statuses that the gem raises on
    InvalidSubscriptionKey,  // the key of the subscription is not a P-256 point
    Argument,                // blank or malformed keys, a payload that is too big
    Tls,                     // a failed TLS session
    Http,                    // a failed connect, a timeout
  };
  Kind kind = Kind::Http;
  std::string class_name;  // the Ruby class of the exception, for the log
  std::string message;

  // The subscription cannot be delivered to, so the pool destroys it. A 404 or a 410 say so. So does a key that is not
  // a point. A TLS failure says nothing about the subscription.
  [[nodiscard]] bool invalidates_subscription() const noexcept {
    return kind == Kind::SubscriptionGone || kind == Kind::InvalidSubscriptionKey;
  }
};

// Per connect and per read. The gem leaves the 60 seconds of `Net::HTTP`, which let a slow push service hold a worker.
inline constexpr std::chrono::seconds kTimeout{10};
// For a whole delivery, however slowly the push service answers.
inline constexpr std::chrono::seconds kDeliveryDeadline{30};

// Delivers a notification. The result is the status of the push service, or nothing if the endpoint is not a permitted
// push service or does not resolve to a public address. `network` gives the DNS answers and the dialer: the tests
// change them.
[[nodiscard]] std::expected<std::optional<int>, DeliveryError> deliver(const Notification& notification,
                                                                       const jobs::web_push::Vapid& vapid,
                                                                       const unfurl::Network& network,
                                                                       std::int64_t now);

// `@push_subscription.notification(title: "Campfire Test", body: Random.uuid, path:).deliver`, in the request. Blocks:
// call it off the worker. Gives the text of the error, or nothing. Web Push that is off is an error, as in Rust.
[[nodiscard]] std::optional<std::string> deliver_test_notification(const App& app,
                                                                   const models::PushSubscription& subscription,
                                                                   std::int64_t badge, const std::string& path);

// `WebPush::Request#verify_response`: the status, or the error that the gem raises.
[[nodiscard]] std::expected<int, DeliveryError> verify_response(int status, std::string_view reason,
                                                                std::string_view host);

}  // namespace campfire::app::web_push
