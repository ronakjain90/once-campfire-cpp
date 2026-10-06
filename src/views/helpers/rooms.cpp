// The parts of RoomsHelper that the sidebar and the profile use. Rust: crates/views/src/helpers/rooms.rs.
#include "views/helpers/rooms.hpp"

#include <array>

#include "routes/query.hpp"

namespace campfire::views::helpers {

std::string rooms_directs_with_user(std::int64_t user_id) {
  const std::array<std::int64_t, 1> ids{user_id};
  return campfire::routes::rooms_directs_with_users(ids);
}

}  // namespace campfire::views::helpers
