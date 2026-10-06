// The room pages against the pages that the Rust port renders (test/rust/*.html, made by running the Rust templates
// on the inputs in test/rust/*.json). The bytes must be equal. Rust: crates/views/tests/rooms_views.rs.
#include <doctest.h>

#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "compat/json.hpp"
#include "views/rooms/pages.hpp"
#include "views/templates.gen.hpp"

namespace {

using namespace campfire;         // NOLINT
using namespace campfire::views;  // NOLINT
using compat::json::Value;

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "cannot open " << path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string str(const Value& object, std::string_view key) {
  const Value* v = object.find(key);
  REQUIRE_MESSAGE(v != nullptr, "missing " << key);
  const std::string* s = v->get_string();
  REQUIRE_MESSAGE(s != nullptr, "not a string: " << key);
  return *s;
}

std::int64_t integer(const Value& object, std::string_view key) {
  const Value* v = object.find(key);
  REQUIRE_MESSAGE(v != nullptr, "missing " << key);
  const auto n = v->to_int64();
  REQUIRE_MESSAGE(n.has_value(), "not an integer: " << key);
  return *n;
}

struct Fixture {
  Value json;
  std::string expected;
  std::map<std::string, std::string> assets;
  ViewContext ctx;

  explicit Fixture(const std::string& name) {
    const std::string dir = std::string(CAMPFIRE_VIEWS_TEST_DIR) + "/rust/";
    auto parsed = compat::json::parse(read_file(dir + name + ".json"));
    REQUIRE(parsed.has_value());
    json = std::move(*parsed);
    expected = read_file(dir + name + ".html");
    const Value& context = *json.find("context");
    for (const auto& [key, value] : context.find("assets")->as_object()) assets[key] = value.as_string();
    ctx.asset_path = [this](std::string_view source) { return assets.at(std::string(source)); };
    if (const Value* user = context.find("current_user"); user != nullptr && user->is_object()) {
      CurrentUser current;
      current.id = integer(*user, "id");
      current.name = str(*user, "name");
      current.administrator = user->find("administrator")->as_bool();
      current.bot = user->find("bot")->as_bool();
      current.avatar_url = str(*user, "avatar_url");
      ctx.current_user = std::move(current);
    }
    const Value& account = *context.find("account");
    ctx.account.name = str(account, "name");
    ctx.account.logo_url = str(account, "logo_url");
    ctx.account.has_logo = account.find("has_logo")->as_bool();
    ctx.base_url = str(context, "base_url");
    if (const auto room = context.find("last_room_visited_id")->to_int64()) ctx.last_room_visited_id = *room;
    ctx.importmap_tags = "<script type=\"module\">import \"application\"</script>";
  }

  [[nodiscard]] const Value& input() const { return *json.find("input"); }

  void check(const LayoutParts& parts) const {
    Out out;
    layouts::application(out, ctx, parts);
    const std::string actual = out.to_string();
    if (actual == expected) return;
    std::size_t i = 0;
    while (i < actual.size() && i < expected.size() && actual[i] == expected[i]) ++i;
    FAIL("first difference at byte " << i << " (actual " << actual.size() << ", expected " << expected.size()
                                     << ")\n  expected: " << expected.substr(i > 60 ? i - 60 : 0, 200)
                                     << "\n  actual:   " << actual.substr(i > 60 ? i - 60 : 0, 200));
  }
};

UserSummary user_of(const Value& v) {
  UserSummary u;
  u.id = integer(v, "id");
  u.name = str(v, "name");
  u.title = str(v, "title");
  u.avatar_path = str(v, "avatar_url");
  return u;
}

std::vector<UserSummary> users_of(const Value& array) {
  std::vector<UserSummary> out;
  for (const Value& v : array.as_array()) out.push_back(user_of(v));
  return out;
}

FormRoom form_room_of(const Value& v) {
  FormRoom room;
  if (const auto id = v.find("id")->to_int64()) room.id = *id;
  if (const std::string* name = v.find("name")->get_string()) room.name = *name;
  return room;
}

OpenFormView open_form(const Fixture& f) {
  OpenFormView form;
  form.room = form_room_of(*f.input().find("room"));
  form.can_administer = f.input().find("can_administer")->as_bool();
  form.users = users_of(*f.input().find("users"));
  return form;
}

ClosedFormView closed_form(const Fixture& f) {
  ClosedFormView form;
  form.room = form_room_of(*f.input().find("room"));
  form.can_administer = f.input().find("can_administer")->as_bool();
  form.current_user_id = integer(f.input(), "current_user_id");
  form.selected_users = users_of(*f.input().find("selected_users"));
  form.unselected_users = users_of(*f.input().find("unselected_users"));
  return form;
}

}  // namespace

TEST_CASE("rust pages: open room forms") {
  for (const char* name : {"rooms_opens_new", "rooms_opens_edit", "rooms_opens_edit_member"}) {
    CAPTURE(name);
    const Fixture f(name);
    const OpenFormView form = open_form(f);
    LayoutParts parts;
    std::string title;
    if (std::string_view(name) == "rooms_opens_new") {
      rooms::opens_new(parts, title, f.ctx, form);
    } else {
      rooms::opens_edit(parts, title, f.ctx, form);
    }
    f.check(parts);
  }
}

TEST_CASE("rust pages: closed room forms") {
  for (const char* name : {"rooms_closeds_new", "rooms_closeds_edit", "rooms_closeds_edit_member"}) {
    CAPTURE(name);
    const Fixture f(name);
    const ClosedFormView form = closed_form(f);
    LayoutParts parts;
    std::string title;
    if (std::string_view(name) == "rooms_closeds_new") {
      rooms::closeds_new(parts, title, f.ctx, form);
    } else {
      rooms::closeds_edit(parts, title, f.ctx, form);
    }
    f.check(parts);
  }
}

TEST_CASE("rust pages: direct room pages") {
  {
    const Fixture f("rooms_directs_new");
    LayoutParts parts;
    rooms::directs_new(parts, f.ctx);
    f.check(parts);
  }
  {
    const Fixture f("rooms_directs_edit");
    DirectEditView edit;
    edit.room_id = integer(f.input(), "room_id");
    edit.display_name = str(f.input(), "display_name");
    edit.users = users_of(*f.input().find("users"));
    LayoutParts parts;
    std::string title;
    rooms::directs_edit(parts, title, f.ctx, edit);
    f.check(parts);
  }
}

TEST_CASE("rust pages: involvement frame") {
  for (const char* name : {"rooms_involvements_show", "rooms_involvements_show_direct"}) {
    CAPTURE(name);
    const Fixture f(name);
    InvolvementView view;
    view.room_id = integer(f.input(), "room_id");
    const std::string kind = str(f.input(), "kind");
    view.kind = kind == "open" ? RoomKind::Open : kind == "closed" ? RoomKind::Closed : RoomKind::Direct;
    view.involvement = str(f.input(), "involvement");
    Out out;
    rooms::involvements::show(out, f.ctx, view);
    CHECK(out.to_string() == f.expected);
  }
}
