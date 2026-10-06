// Pieces that the controllers of the sign-in area share. Rails: ActionController::Parameters, ActiveRecord errors.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "app/flow.hpp"
#include "app/rq.hpp"

namespace campfire::app::controllers {

// `params.require(:user).permit(:name, :avatar, :email_address, :password)`: a missing `user` is
// `ActionController::ParameterMissing` (400). Anything that is not a hash permits nothing.
[[nodiscard]] Flow<req::ParamMap> user_params(Rq& rq);

// A permitted scalar as Ruby `to_s` prints it: nothing for a missing key, a file, an array or a hash.
[[nodiscard]] std::optional<std::string> param_text(const req::ParamMap& params, std::string_view key);

// `rescue ActiveRecord::RecordNotUnique`: a write that broke a unique index.
[[nodiscard]] bool is_record_not_unique(const Error& error);

// A response for the action that does not exist (`AbstractController::ActionNotFound`): 404.
[[nodiscard]] Task<Flow<net::Response>> action_not_found(Rq& rq);

}  // namespace campfire::app::controllers
