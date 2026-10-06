// Shared pieces of the sign-in controllers. Rust: crates/kit/src/params.rs (permit_keys), crates/db/src/error.rs.
#include "app/controllers/common.hpp"

namespace campfire::app::controllers {

Flow<req::ParamMap> user_params(Rq& rq) {
  auto required = rq.params().require("user");
  if (!required) return fail_with(ErrorKind::ParameterMissing, required.error().message);
  const req::ParamMap* hash = (*required)->as_hash();
  if (hash == nullptr) return req::ParamMap(rq.ctx.resource());
  return hash->permit({"name", "avatar", "email_address", "password"}, rq.ctx.resource());
}

std::optional<std::string> param_text(const req::ParamMap& params, std::string_view key) {
  const req::Param* value = params.get(key);
  if (value == nullptr) return std::nullopt;
  return value->to_s();
}

bool is_record_not_unique(const Error& error) {
  return error.message.find("UNIQUE constraint failed") != std::string::npos;
}

Task<Flow<net::Response>> action_not_found(Rq&) {
  co_return fail_with(ErrorKind::NotFound, "action not found");
}

}  // namespace campfire::app::controllers
