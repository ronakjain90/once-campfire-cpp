// Unit tests for the URI parser, the SGID resolver and the attachment limits.
// Rust: crates/richtext/src/uri.rs and attachables.rs (tests).
#include <string>

#include "compat/global_id.hpp"
#include "compat/secrets.hpp"
#include "doctest.h"
#include "richtext/autolink.hpp"
#include "richtext/resolver.hpp"
#include "richtext/richtext.hpp"
#include "richtext/uri.hpp"

using namespace campfire::richtext;
namespace gid = campfire::compat::global_id;

namespace {

class Records final : public RecordLookup {
 public:
  [[nodiscard]] std::optional<MentionUser> user(std::int64_t id) const override {
    if (id != 1) {
      return std::nullopt;
    }
    return MentionUser{1, "David", "David - Founder", "sgid-1", "/users/1", "/users/1/avatar"};
  }
  [[nodiscard]] bool record_exists(std::string_view model, std::int64_t id) const override {
    return model == "Room" && id == 7;
  }
};

}  // namespace

TEST_CASE("URI.parse matches Ruby") {
  auto uri = parse_uri("https://x.com/dhh/status/1?s=20");
  REQUIRE(uri.has_value());
  CHECK(uri->host == "x.com");
  CHECK(uri->query == "s=20");
  CHECK(!parse_uri("http://exa mple.com/ ").has_value());
  CHECK(!parse_uri("https:/rooms/1")->host.has_value());
  CHECK(parse_uri("https:rooms/1")->opaque == "rooms/1");
  CHECK(parse_uri("https://")->host == "");
  CHECK(parse_uri("http://[::1]/x")->host == "[::1]");
  CHECK(parse_uri("mailto:foo").error() == UriError::InvalidComponent);
  CHECK(parse_uri("mailto:a@b.com").has_value());
  CHECK(parse_uri("https://x.com:443/a")->to_s() == "https://x.com/a");
  CHECK(parse_uri("HTTPS://x.com/a")->scheme == "https");
  CHECK(parse_uri("HTTP://X.com:80/a")->to_s() == "http://X.com/a");
}

TEST_CASE("the compat resolver verifies SGIDs and finds records") {
  const campfire::compat::Secrets secrets("test-secret");
  const Records records;
  const campfire::compat::Timestamp now =
      std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::sys_days{std::chrono::year{2024} / 1 / 2});
  const CompatResolver resolver(secrets, now, records);

  const std::string good = gid::attachable_sgid(secrets, gid::GlobalId::make("User", "1"));
  const SignedLookup found = resolver.locate_signed(good);
  CHECK(found.kind == SignedLookup::Kind::User);
  CHECK(found.user.name == "David");

  const std::string gone = gid::attachable_sgid(secrets, gid::GlobalId::make("User", "2"));
  CHECK(resolver.locate_signed(gone).kind == SignedLookup::Kind::MissingRecord);
  CHECK(resolver.locate_signed(good + "0").kind == SignedLookup::Kind::Invalid);
  CHECK(resolver.locate_signed("junk").kind == SignedLookup::Kind::Invalid);

  CHECK(resolver.find_gid(gid::GlobalId::make("Room", "7")).kind == GidLookup::Kind::OtherModel);
  CHECK(resolver.find_gid(gid::GlobalId::make("Room", "8")).kind == GidLookup::Kind::NotFound);
  CHECK(resolver.find_gid(gid::GlobalId::make("User", "1")).kind == GidLookup::Kind::User);

  // A mention renders with its partial, reads as plain text, and is listed once.
  const RenderContext ctx{resolver, "once.campfire.test"};
  const std::string tag = "<action-text-attachment sgid=\"" + good + "\" content-type=\"" +
                          std::string(kMentionContentType) + "\"></action-text-attachment>";
  const std::string body = "Hi " + tag + " and " + tag;
  auto html = message_presentation(body, ctx);
  REQUIRE(html.has_value());
  CHECK(html->find("<span class=\"mention\"") != std::string::npos);
  CHECK(*to_plain_text(body, ctx) == "Hi @David and @David");
  auto users = mentioned_users(body, ctx);
  REQUIRE(users.has_value());
  CHECK(users->size() == 1);
}

TEST_CASE("content attachments nest at most eight levels") {
  const campfire::compat::Secrets secrets("test-secret");
  const Records records;
  const CompatResolver resolver(secrets, {}, records);
  const RenderContext ctx{resolver, ""};
  std::string body = "deep";
  for (int i = 0; i < 12; ++i) {
    std::string escaped;
    for (char c : body) {
      escaped += c == '"' ? "&quot;" : std::string(1, c);
    }
    body = "<action-text-attachment content-type=\"text/html\" content=\"" + escaped + "\"></action-text-attachment>";
  }
  auto html = message_presentation(body, ctx);
  REQUIRE(html.has_value());
  CHECK(html->find("deep") == std::string::npos);
}

TEST_CASE("autolink escapes brackets in attributes and links bare URLs") {
  auto out = auto_link("<p title=\"a>b http://x.test/\">see http://y.test/a.</p>", SafeList::auto_link());
  REQUIRE(out.has_value());
  CHECK(
      *out ==
      "<p title=\"a&gt;b http://x.test/\">see <a target=\"_blank\" href=\"http://y.test/a\">http://y.test/a</a>.</p>");
  CHECK(*auto_link("mail me@x.test now", SafeList::auto_link()) ==
        "mail <a target=\"_blank\" href=\"mailto:me@x.test\">me@x.test</a> now");
}

TEST_CASE("without_recipient_mentions removes the bot name and trims white space") {
  CHECK(without_recipient_mentions("\xE2\x80\x83@Bot  hello @Bot\n", "Bot") == "hello");
}
