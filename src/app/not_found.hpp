// Unknown route. Rails: ActionDispatch::PublicExceptions; Rust: crates/kit/src/exceptions.rs.
#pragma once

#include "net/ctx.hpp"

namespace campfire::app {

// The handler for a request that no route matches: the public/404.html page, or JSON.
campfire::Task<net::Response> not_found(net::Ctx& ctx);

}  // namespace campfire::app
