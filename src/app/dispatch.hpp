// The entry of every route handler. Rails: the middleware stack (ActionDispatch::SSL, RequestId, Runtime,
// Deflater) around ActionController. Rust: crates/kit/src/adapter.rs (dispatch, rails_middleware).
#pragma once

#include "app/flow.hpp"
#include "app/rq.hpp"
#include "core/task.hpp"
#include "net/ctx.hpp"

namespace campfire::app {

// An action: it gets the request context and returns a response, or a failure.
using Action = Task<Flow<net::Response>> (*)(Rq&);

// Builds the `Rq`, forces SSL if the app does, parses the request, runs the action, and finishes the
// response. An exception in the action gives the 500 response and a log line.
//
//   Task<net::Response> show(net::Ctx& c) { return app::dispatch(c, &show_action); }
[[nodiscard]] Task<net::Response> dispatch(net::Ctx& ctx, Action action);

}  // namespace campfire::app
