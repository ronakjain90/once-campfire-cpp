// Rails: the middleware stack before the router (Rack::MethodOverride); Rust: rails_middleware in
// crates/kit/src/adapter.rs.
#include <optional>
#include <string_view>

#include "app/not_found.hpp"
#include "app/routes.hpp"
#include "net/http.hpp"
#include "req/method_override.hpp"

namespace campfire::app {
namespace {

std::optional<std::string_view> header_of(const net::Request& request, std::string_view lower_name) {
  if (!request.has_header(lower_name)) return std::nullopt;
  return request.header(lower_name);
}

std::optional<std::string_view> method_override(const net::Request& request) {
  return req::method_override(header_of(request, "content-type"), request.body,
                              header_of(request, "x-http-method-override"));
}

}  // namespace

net::App server_app() {
  net::App app{&routes(), &not_found};
  app.method_override = &method_override;
  return app;
}

}  // namespace campfire::app
