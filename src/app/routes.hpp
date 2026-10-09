// The route table of the app, generated from src/app/routes/*.inc by tools/routes_gen.py.
#pragma once

#include "net/options.hpp"
#include "net/router.hpp"

namespace campfire::app {

[[nodiscard]] const net::RouteTable& routes();

// What the server serves: the routes, the handler of an unknown route, and Rack::MethodOverride (server_app.cpp).
[[nodiscard]] net::App server_app();

}  // namespace campfire::app
