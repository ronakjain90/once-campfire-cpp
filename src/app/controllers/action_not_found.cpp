// A route whose action the controller does not define. Rails: AbstractController::ActionNotFound (404), which Rails
// raises before the callbacks. Rust: crates/campfire/src/controllers.rs (action_not_found).
#include "app/controllers/common.hpp"
#include "app/dispatch.hpp"

namespace campfire::routes::misc {

Task<net::Response> action_not_found(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::action_not_found);
}

}  // namespace campfire::routes::misc
