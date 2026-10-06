// The sidebar against the golden frame page of the reference app (test/golden/sidebar_frame.html, made by Rails).
// The data of the page is read from the page itself. Only the CSRF tokens of the `button_to` forms are not in our
// output: the test removes them from the golden page first (Rust "Known differences").
#include "views/users/sidebar.hpp"

#include <doctest.h>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "views/templates.gen.hpp"

namespace {

using namespace campfire;         // NOLINT
using namespace campfire::views;  // NOLINT

std::string read_golden(const std::string& name) {
  std::ifstream in(std::string(CAMPFIRE_VIEWS_TEST_DIR) + "/golden/" + name + ".html", std::ios::binary);
  REQUIRE(in.good());
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

UserSummary user(std::int64_t id, std::string name, std::string avatar_path) {
  UserSummary u;
  u.id = id;
  u.name = std::move(name);
  u.avatar_path = std::move(avatar_path);
  return u;
}

}  // namespace

TEST_CASE("users/sidebars/show equals the page of Rails") {
  const std::string golden = read_golden("sidebar_frame");
  const std::size_t a = golden.find("<body>\n    ") + 11;
  const std::size_t b = golden.find("\n  </body>");
  REQUIRE(a != std::string::npos);
  REQUIRE(b != std::string::npos);
  std::string expected = golden.substr(a, b - a);
  expected = std::regex_replace(
      expected, std::regex(R"re(<input type="hidden" name="authenticity_token" value="[^"]*" />)re"), "");

  std::vector<std::string> avatars;
  const std::regex avatar_re(R"re(src="(/users/[^"]*)")re");
  for (auto it = std::sregex_iterator(golden.begin(), golden.end(), avatar_re); it != std::sregex_iterator(); ++it) {
    avatars.push_back((*it)[1].str());
  }
  REQUIRE(avatars.size() == 6);
  std::vector<std::string> streams;
  const std::regex stream_re(R"re(signed-stream-name="([^"]*)")re");
  for (auto it = std::sregex_iterator(golden.begin(), golden.end(), stream_re); it != std::sregex_iterator(); ++it) {
    streams.push_back((*it)[1].str());
  }
  REQUIRE(streams.size() == 2);

  SidebarShow sidebar;
  sidebar.rooms_stream = streams[0];
  sidebar.user_rooms_stream = streams[1];
  sidebar.current_user = user(127326141, "David", avatars[5]);
  sidebar.can_create_rooms = true;
  for (const auto& [id, name, avatar] : {std::tuple{699448325, "Kevin", 0}, std::tuple{186869642, "Jason", 1}}) {
    SidebarDirect d;
    d.room_id = id;
    d.updated_at_epoch = "1790427620300";
    d.members.push_back(user(id, name, avatars[static_cast<std::size_t>(avatar)]));
    sidebar.direct_memberships.push_back(std::move(d));
  }
  sidebar.direct_placeholder_users.push_back(user(394959859, "Bender", avatars[2]));
  sidebar.direct_placeholder_users.push_back(user(773523953, "JZ", avatars[3]));
  sidebar.direct_placeholder_users.push_back(user(773523956, "Anna", avatars[4]));
  sidebar.other_memberships.push_back({104393281, "rooms_open", "All Pets", false});
  sidebar.other_memberships.push_back({486777696, "rooms_closed", "All Talk", false});
  sidebar.other_memberships.push_back({654632876, "rooms_closed", "Designers", false});
  sidebar.other_memberships.push_back({201306877, "rooms_open", "HQ", false});

  ViewContext ctx;
  ctx.asset_path = [](std::string_view source) -> std::string {
    if (source == "messages-add.svg") return "/assets/messages-add-d229e6c2.svg";
    if (source == "add.svg") return "/assets/add-f232d8a6.svg";
    if (source == "menu.svg") return "/assets/menu-5462dfd3.svg";
    if (source == "settings.svg") return "/assets/settings-aee56972.svg";
    return std::string(source);
  };
  Out out;
  users::sidebars::show(out, ctx, sidebar);
  const std::string actual = out.to_string();
  if (actual != expected) {
    std::size_t i = 0;
    while (i < actual.size() && i < expected.size() && actual[i] == expected[i]) ++i;
    FAIL("first difference at byte " << i << "\n  expected: " << expected.substr(i > 40 ? i - 40 : 0, 160)
                                     << "\n  actual:   " << actual.substr(i > 40 ? i - 40 : 0, 160));
  }
}
