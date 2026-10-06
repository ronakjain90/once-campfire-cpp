// Rails: app/models/boost.rb (belongs_to :message, touch: true). Rust: crates/db/src/models/boost.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/arena.hpp"
#include "core/error.hpp"
#include "db/connection.hpp"
#include "db/database.hpp"

namespace campfire::models {

struct Boost {
  std::int64_t id = 0;
  std::int64_t message_id = 0;
  std::int64_t booster_id = 0;
  std::string content;
  std::string created_at;
  std::string updated_at;

  [[nodiscard]] static Boost from_row(const db::schema::BoostRow& row);
};

namespace boosts {

// `message.boosts.find_by(id:, booster:)`
[[nodiscard]] Result<std::optional<Boost>> find_by_message_and_booster(db::Connection& conn, Arena& arena,
                                                                       std::int64_t message_id, std::int64_t id,
                                                                       std::int64_t booster_id);
// `message.boosts.ordered`
[[nodiscard]] Result<std::vector<Boost>> for_message_ordered(db::Connection& conn, Arena& arena,
                                                             std::int64_t message_id);

// `message.boosts.create!(content:, booster:)`: the boost, then the touch of the message (and so of its room).
// `plain_text` is the `plain_text_body` of the message, for the search index that the touch updates.
[[nodiscard]] Result<Boost> create(db::Tx& tx, std::int64_t message_id, std::int64_t booster_id,
                                   std::string_view content, std::string_view plain_text);
// `boost.destroy!`: the same touch.
[[nodiscard]] Status destroy(db::Tx& tx, const Boost& boost, std::string_view plain_text);

}  // namespace boosts
}  // namespace campfire::models
