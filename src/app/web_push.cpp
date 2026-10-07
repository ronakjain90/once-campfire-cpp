// Rails: reference/lib/web_push (web-push 3.1.0 with WebPush::PersistentRequest), app/models/push/subscription.rb.
// Rust: crates/campfire/src/integrations/web_push.rs.
#include "app/web_push.hpp"

#include "app/network_guard.hpp"
#include "richtext/uri.hpp"

namespace campfire::app::web_push {

namespace {

std::string lower(std::string text) {
  for (char& c : text) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return text;
}

DeliveryError error_of(DeliveryError::Kind kind, std::string class_name, std::string message) {
  return DeliveryError{kind, std::move(class_name), std::move(message)};
}

}  // namespace

std::string Notification::encoded_message() const {
  return jobs::web_push::encoded_message(title, body, path, badge);
}

std::expected<int, DeliveryError> verify_response(int status, std::string_view reason, std::string_view host) {
  using Kind = DeliveryError::Kind;
  const auto fail = [&](Kind kind, const char* name) -> std::expected<int, DeliveryError> {
    return std::unexpected(error_of(
        kind, name, std::string(name) + ": host: " + std::string(host) + ", status: " + std::to_string(status)));
  };
  if (status == 410) return fail(Kind::SubscriptionGone, "WebPush::ExpiredSubscription");
  if (status == 404) return fail(Kind::SubscriptionGone, "WebPush::InvalidSubscription");
  if (status == 401 || status == 403) return fail(Kind::Response, "WebPush::Unauthorized");
  if (status == 400 && reason == "UnauthorizedRegistration") return fail(Kind::Response, "WebPush::Unauthorized");
  if (status == 413) return fail(Kind::Response, "WebPush::PayloadTooLarge");
  if (status == 429) return fail(Kind::Response, "WebPush::TooManyRequests");
  if (status >= 500 && status <= 599) return fail(Kind::Response, "WebPush::PushServiceError");
  if (status >= 200 && status <= 299) return status;
  return fail(Kind::Response, "WebPush::ResponseError");
}

std::expected<std::optional<int>, DeliveryError> deliver(const Notification& notification,
                                                         const jobs::web_push::Vapid& vapid,
                                                         const unfurl::Network& network, std::int64_t now) {
  using Kind = DeliveryError::Kind;
  const models::PushSubscription& subscription = notification.subscription;
  // `Push::Subscription#resolved_endpoint_ip`: only a permitted https endpoint on port 443, through the guard.
  const auto host = models::push_subscriptions::endpoint_host_to_resolve(subscription.endpoint);
  if (!host) return std::optional<int>{};
  const auto ip = network.lookup ? resolve_public_address(*host, network.lookup) : resolve_public_address(*host);
  if (!ip) return std::optional<int>{};

  const std::string& endpoint = *subscription.endpoint;
  const auto uri = richtext::parse_uri(endpoint);
  if (!uri) {
    return std::unexpected(error_of(Kind::Argument, "ArgumentError", "bad URI(is not URI?): \"" + endpoint + "\""));
  }
  const std::string uri_host = uri->host.value_or("");
  const std::string message = notification.encoded_message();
  const auto body = jobs::web_push::encrypt(message, subscription.p256dh_key, subscription.auth_key);
  if (!body) {
    const bool bad_key = body.error().kind == jobs::web_push::PushError::Kind::InvalidKey;
    return std::unexpected(error_of(bad_key ? Kind::InvalidSubscriptionKey : Kind::Argument,
                                    bad_key ? "OpenSSL::PKey::EC::Point::Error" : "ArgumentError",
                                    body.error().message));
  }

  // The headers in the order of `WebPush::PersistentRequest`, then those that `Net::HTTP` adds.
  const std::string audience = lower(uri->scheme.value_or("")) + "://" + uri_host;
  std::string target = uri->path && !uri->path->empty() ? *uri->path : "/";
  if (uri->query) target += "?" + *uri->query;
  unfurl::Request request;
  request.method = "POST";
  request.target = std::move(target);
  request.headers.emplace();
  auto& headers = *request.headers;
  headers.emplace_back("Content-Type", "application/octet-stream");
  headers.emplace_back("Ttl", std::to_string(jobs::web_push::kTtlSeconds));
  headers.emplace_back("Urgency", std::string(jobs::web_push::kUrgency));
  headers.emplace_back("Content-Encoding", "aes128gcm");
  headers.emplace_back("Content-Length", std::to_string(body->size()));
  headers.emplace_back("Authorization", vapid.authorization(audience, now));
  headers.emplace_back("Accept-Encoding", "gzip;q=1.0,deflate;q=0.6,identity;q=0.3");
  headers.emplace_back("Accept", "*/*");
  headers.emplace_back("User-Agent", "Ruby");
  headers.emplace_back("Connection", "close");
  headers.emplace_back("Host", uri_host);
  request.body = *body;
  request.decode_content = true;

  unfurl::Endpoint target_endpoint;
  target_endpoint.https = true;
  target_endpoint.host = uri_host;
  target_endpoint.port = static_cast<std::uint16_t>(uri->port.value_or(443));
  target_endpoint.pinned_ip = *ip;

  unfurl::Timeouts timeouts;
  timeouts.open = std::chrono::duration_cast<std::chrono::milliseconds>(kTimeout);
  timeouts.read = timeouts.open;
  const auto response =
      unfurl::exchange(network, target_endpoint, request, timeouts, unfurl::Clock::now() + kDeliveryDeadline);
  if (!response) {
    const Error& e = response.error();
    if (e.code == Errc::Timeout) {
      const bool open = e.message == "execution expired";
      return std::unexpected(error_of(Kind::Http, open ? "Net::OpenTimeout" : "Net::ReadTimeout", e.message));
    }
    if (e.message.starts_with("SSL error")) {
      return std::unexpected(error_of(Kind::Tls, "OpenSSL::SSL::SSLError", e.message));
    }
    return std::unexpected(error_of(Kind::Http, "SystemCallError", e.message));
  }
  auto verified = verify_response(response->status, response->reason, uri_host);
  if (!verified) return std::unexpected(std::move(verified.error()));
  return std::optional<int>(*verified);
}

std::optional<std::string> deliver_test_notification(const App& app, const models::PushSubscription& subscription,
                                                     std::int64_t badge, const std::string& path) {
  if (!app.vapid) return "Web Push is off (no valid VAPID keys)";
  Notification notification;
  notification.title = "Campfire Test";
  notification.body = jobs::web_push::random_uuid();
  notification.path = path;
  notification.badge = badge;
  notification.subscription = subscription;
  const auto delivered = deliver(notification, *app.vapid, app.push_network, app.now().seconds);
  if (!delivered) return delivered.error().class_name + ": " + delivered.error().message;
  return std::nullopt;
}

}  // namespace campfire::app::web_push
