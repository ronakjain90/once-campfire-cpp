// SessionsController (reference/app/controllers/sessions_controller.rb): sign in and out.
// Rust: crates/campfire/src/controllers/sessions.rs.
#pragma once

#include "app/dispatch.hpp"

namespace campfire::app::controllers {

[[nodiscard]] Task<Flow<net::Response>> sessions_new(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> sessions_create(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> sessions_destroy(Rq& rq);

}  // namespace campfire::app::controllers
