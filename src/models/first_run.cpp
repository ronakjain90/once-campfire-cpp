// Rails: app/models/first_run.rb, rooms/open.rb (grant_access_to_all_users). Rust: crates/db/src/models/first_run.rs.
#include "models/first_run.hpp"

#include "models/account.hpp"

namespace campfire::models::first_run {

namespace {

const db::Query<std::int64_t(std::string_view, std::int64_t, std::string_view, std::string_view, std::string_view)>
    kInsertRoom{
        "INSERT INTO \"rooms\" (\"created_at\", \"creator_id\", \"name\", \"type\", \"updated_at\") VALUES (?, ?, ?, ?, ?) "
        "RETURNING \"id\""};

// `memberships.grant_to(User.active)` for the new room: `Rooms::Open`'s `after_save_commit`.
const db::Query<void(std::int64_t)> kGrantActiveUsers{
    "INSERT INTO \"memberships\" (\"created_at\",\"involvement\",\"room_id\",\"updated_at\",\"user_id\") "
    "SELECT STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW'), 'mentions', ?, STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW'), \"users\".\"id\" "
    "FROM \"users\" WHERE \"users\".\"status\" = 0 ON CONFLICT DO NOTHING"};

}  // namespace

Result<User> create(db::Tx& tx, std::string_view name, std::string_view email_address,
                    std::optional<std::string> password_digest) {
  if (auto account = accounts::create(tx, kAccountName); !account) return std::unexpected(account.error());
  NewUser attributes;
  attributes.name = std::string(name);
  attributes.email_address = std::string(email_address);
  attributes.password_digest = std::move(password_digest);
  attributes.role = Role::Administrator;
  auto administrator = users::create(tx, attributes);
  if (!administrator) return std::unexpected(administrator.error());
  Arena arena(256);
  const std::string now = tx.now_db();
  auto room = tx.conn().first(kInsertRoom, arena, now, administrator->id, kFirstRoomName, "Rooms::Open", now);
  if (!room) return std::unexpected(room.error());
  if (auto granted = tx.conn().exec(kGrantActiveUsers, **room); !granted) return std::unexpected(granted.error());
  tx.changed(db::schema::Table::Rooms, **room);
  tx.changed(db::schema::Table::Memberships, **room);
  return administrator;
}

}  // namespace campfire::models::first_run
