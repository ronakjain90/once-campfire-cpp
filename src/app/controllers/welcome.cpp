// WelcomeController#show (reference/app/controllers/welcome_controller.rb). A0 proof route: it only runs the
// authentication chain, so that an unauthenticated request redirects to the sign-in page. A1 replaces it.
#include "app/concerns.hpp"
#include "app/dispatch.hpp"

namespace campfire::app::controllers {

namespace {
Task<Flow<net::Response>> welcome_show(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  // The redirect to `last_room_visited` needs rooms (A2). Until then a signed in request gets the 404 page.
  co_return fail_with(ErrorKind::NotFound, "welcome#show is not ported yet");
}
}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::welcome {

Task<net::Response> show(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::welcome_show);
}

}  // namespace campfire::routes::welcome
