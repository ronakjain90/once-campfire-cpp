// The route table of the app, generated from src/app/routes/*.inc by tools/routes_gen.py.
#pragma once

#include "net/router.hpp"

namespace campfire::app {

[[nodiscard]] const net::RouteTable& routes();

}  // namespace campfire::app
