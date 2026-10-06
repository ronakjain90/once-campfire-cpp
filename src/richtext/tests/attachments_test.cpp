// Differential test against Rails: tests/data/expected.json is the corpus of the Rust port
// (crates/richtext/tests/corpus/expected.json, made by reference-tools/richtext/generate.rb in the
// campfire-reference image). For each case and each output field, the C++ result must equal the
// recorded Rails result, except for the deliberate differences of the Rust README "Known differences".
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "compat/json.hpp"
#include "doctest.h"
#include "richtext/attachables.hpp"
#include "richtext/richtext.hpp"

namespace {

using campfire::compat::json::Value;
using namespace campfire::richtext;

std::string read_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

std::string str(const Value& v, std::string_view key) {
  const Value* m = v.find(key);
  return m != nullptr && m->get_string() != nullptr ? *m->get_string() : std::string();
}

std::optional<std::int64_t> parse_id(std::string_view text) {
  if (text.starts_with('+')) {
    text.remove_prefix(1);
  }
  if (text.empty()) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  std::size_t i = 0;
  const bool negative = text[0] == '-';
  if (negative) {
    i = 1;
  }
  if (i == text.size()) {
    return std::nullopt;
  }
  for (; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') {
      return std::nullopt;
    }
    value = value * 10 + (text[i] - '0');
  }
  return negative ? -value : value;
}

// Stands in for the app: the SGIDs that Rails made verify by exact match, and GIDs are found by model and id.
class TestResolver final : public AttachableResolver {
 public:
  explicit TestResolver(const Value& corpus) {
    for (const Value& u : corpus.find("users")->as_array()) {
      MentionUser user;
      user.id = *u.find("id")->to_int64();
      user.name = str(u, "name");
      user.title = str(u, "title");
      user.attachable_sgid = str(u, "attachable_sgid");
      user.user_path = str(u, "user_path");
      user.avatar_path = str(u, "avatar_path");
      users_.push_back(std::move(user));
    }
    for (const Value& r : corpus.find("rooms")->as_array()) {
      rooms_.push_back(*r.to_int64());
    }
    for (const Value& s : corpus.find("signed")->as_array()) {
      signed_.push_back({str(s, "sgid"), str(s, "model"), *s.find("id")->to_int64(), s.find("exists")->as_bool()});
    }
  }

  SignedLookup locate_signed(std::string_view sgid) const override {
    SignedLookup out;
    for (const auto& s : signed_) {
      if (s.sgid != sgid) {
        continue;
      }
      if (s.model == "User" && s.exists) {
        out.kind = SignedLookup::Kind::User;
        out.user = *find_user(s.id);
      } else {
        out.kind = SignedLookup::Kind::MissingRecord;
        out.model_name = s.model;
      }
      return out;
    }
    return out;
  }

  GidLookup find_gid(const campfire::compat::global_id::GlobalId& gid) const override {
    GidLookup out;
    auto id = parse_id(gid.id);
    if (!id) {
      return out;
    }
    if (gid.model_name == "User") {
      if (const MentionUser* user = find_user(*id)) {
        out.kind = GidLookup::Kind::User;
        out.user = *user;
      }
    } else if (gid.model_name == "Room") {
      for (auto room : rooms_) {
        if (room == *id) {
          out.kind = GidLookup::Kind::OtherModel;
        }
      }
    }
    return out;
  }

 private:
  struct Signed {
    std::string sgid;
    std::string model;
    std::int64_t id;
    bool exists;
  };
  const MentionUser* find_user(std::int64_t id) const {
    for (const auto& u : users_) {
      if (u.id == id) {
        return &u;
      }
    }
    return nullptr;
  }
  std::vector<MentionUser> users_;
  std::vector<std::int64_t> rooms_;
  std::vector<Signed> signed_;
};

// Rails' presentation as the port renders it on purpose (README "Known differences"): `<` and `>` are
// escaped in attribute values, the links that rails_autolink put inside an attribute value (its stored
// XSS) stay the text they replaced, and `name` attributes are dropped.
std::string with_port_divergences(const std::string& rails) {
  constexpr std::string_view kInsertedLink = "<a target=\"_blank\" href=\"";
  enum class State { Text, Tag, Value };
  std::string out;
  State state = State::Text;
  std::size_t i = 0;
  while (i < rails.size()) {
    const char c = rails[i];
    const std::string_view rest = std::string_view(rails).substr(i);
    if (state == State::Value && rest.starts_with(kInsertedLink)) {
      const std::size_t text_start = rest.find("\">") + 2;
      const std::size_t text_end = text_start + rest.substr(text_start).find("</a>");
      std::string text(rest.substr(text_start, text_end - text_start));
      for (std::size_t k = 0; k < text.size(); ++k) {
        if (text[k] == '>') {
          text.replace(k, 1, "&gt;");
          k += 3;
        }
      }
      out += text;
      i += text_end + 4;
      continue;
    }
    if (state == State::Tag && rest.starts_with(" name=\"")) {
      const std::size_t value_end = 7 + rest.substr(7).find('"');
      i += value_end + 1;
      continue;
    }
    if (state == State::Text && c == '<') {
      state = State::Tag;
    } else if (state == State::Tag && c == '>') {
      state = State::Text;
    } else if (state == State::Tag && c == '"') {
      state = State::Value;
    } else if (state == State::Value && c == '"') {
      state = State::Tag;
    }
    if (state == State::Value && c == '<') {
      out += "&lt;";
    } else if (state == State::Value && c == '>') {
      out += "&gt;";
    } else {
      out.push_back(c);
    }
    ++i;
  }
  return out;
}

// Removes ` <name>="..."` from each tag (as the T3 corpus test does).
std::string without_attribute(const std::string& html, const std::string& name) {
  const std::string needle = " " + name + "=\"";
  std::string out;
  bool in_tag = false;
  for (std::size_t i = 0; i < html.size();) {
    if (!in_tag && html[i] == '<' && i + 1 < html.size() && html[i + 1] != '/' && html[i + 1] != '!') {
      in_tag = true;
    } else if (in_tag && html[i] == '>') {
      in_tag = false;
    } else if (in_tag && html.compare(i, needle.size(), needle) == 0) {
      std::size_t end = html.find('"', i + needle.size());
      if (end != std::string::npos) {
        i = end + 1;
        continue;
      }
    } else if (in_tag && html[i] == '"') {
      std::size_t end = html.find('"', i + 1);
      if (end != std::string::npos) {
        out.append(html, i, end + 1 - i);
        i = end + 1;
        continue;
      }
    }
    out.push_back(html[i++]);
  }
  return out;
}

struct Tally {
  std::size_t total = 0;
  std::size_t equal = 0;
  std::size_t deliberate = 0;  // Counted as equal because of a documented difference
  std::vector<std::string> failures;

  void pass() {
    ++total;
    ++equal;
  }
  void note(bool ok, const std::string& name, const std::string& expected, const std::string& actual) {
    ++total;
    if (ok) {
      ++equal;
    } else if (failures.size() < 20) {
      failures.push_back("  FAIL " + name + "\n    expected: " + expected.substr(0, 500) +
                         "\n    actual:   " + actual.substr(0, 500));
    }
  }
};

bool is_error(const Value& outcome) {
  return outcome.find("error") != nullptr;
}

std::string ok_string(const Value& outcome) {
  const Value* v = outcome.find("ok");
  return v != nullptr && v->get_string() != nullptr ? *v->get_string() : std::string();
}

}  // namespace

TEST_CASE("the attachment pipeline matches Rails for all corpus cases") {
  const std::string text = read_file(std::string(RICHTEXT_TEST_DATA_DIR) + "/expected.json");
  auto parsed = campfire::compat::json::parse(text);
  REQUIRE(parsed.has_value());
  const Value& corpus = *parsed;
  TestResolver resolver(corpus);
  const auto& cases = corpus.find("cases")->as_array();
  CHECK(cases.size() == 658);

  std::map<std::string, Tally> tallies;
  for (const Value& c : cases) {
    const std::string name = str(c, "name");
    const std::string body = str(c, "body");
    const RenderContext ctx{resolver, str(c, "host")};
    const Value& presentation = *c.find("presentation");
    const std::string raised_message = str(c, "presentation_raised_message");
    const bool raised =
        c.find("presentation_raised") != nullptr && c.find("presentation_raised")->get_string() != nullptr;
    const bool missing_partial = raised_message.find("to_missing_attachable_partial_path") != std::string::npos;

    // presentation and presentation_raised
    {
      auto result = message_presentation(body, ctx);
      const Presentation actual = present_message(body, ctx);
      Tally& t = tallies["presentation"];
      if (is_error(presentation)) {
        t.note(actual.kind == Presentation::Kind::Unrenderable, name, "<unrenderable>", actual.html);
      } else if (missing_partial) {
        // Deliberate: a missing attachable that Rails has no partial for renders ☒ and does not blank the message.
        const bool ok = actual.kind == Presentation::Kind::Html && actual.html.find("☒") != std::string::npos;
        t.note(ok, name, "<contains ☒>", actual.html);
        if (ok) ++t.deliberate;
      } else {
        t.note(actual.kind == Presentation::Kind::Html && actual.html == with_port_divergences(ok_string(presentation)),
               name, with_port_divergences(ok_string(presentation)), actual.html);
      }
      Tally& r = tallies["presentation_raised"];
      if (missing_partial) {
        r.note(result.has_value(), name, "<deliberately no error>", result ? "ok" : result.error().message);
        if (result) ++r.deliberate;
      } else {
        r.note(raised == !result.has_value(), name, raised ? "raised " + str(c, "presentation_raised") : "no error",
               result ? "no error" : "raised " + result.error().message);
      }
    }

    // plain_text
    {
      const Value& expected = *c.find("plain_text");
      auto actual = to_plain_text(body, ctx);
      Tally& t = tallies["plain_text"];
      if (is_error(expected)) {
        t.note(!actual.has_value(), name, "<error>", actual ? *actual : "");
      } else {
        t.note(actual.has_value() && *actual == ok_string(expected), name, ok_string(expected),
               actual ? *actual : "<error " + actual.error().message + ">");
      }
    }

    // editable
    {
      const Value& expected = *c.find("editable");
      auto actual = editable_value(body, ctx);
      Tally& t = tallies["editable"];
      if (is_error(expected)) {
        // Deliberate: a missing attachable leaves the editor, where Rails raises.
        const bool missing = str(expected, "message").find("MissingAttachable") != std::string::npos;
        const bool ok = !actual.has_value() || missing;
        t.note(ok, name, "<error>", actual && actual->has_value() ? **actual : "<none>");
        if (actual.has_value() && missing) ++t.deliberate;
      } else {
        const Value* v = expected.find("ok");
        const bool has = v != nullptr && v->get_string() != nullptr;
        const bool ok = actual.has_value() && actual->has_value() == has && (!has || **actual == *v->get_string());
        t.note(ok, name, has ? *v->get_string() : "<none>",
               !actual ? "<error " + actual.error().message + ">" : (actual->has_value() ? **actual : "<none>"));
      }
    }

    // mentioned
    {
      const Value& expected = *c.find("mentioned");
      auto actual = mentioned_users(body, ctx);
      Tally& t = tallies["mentioned"];
      if (is_error(expected)) {
        t.note(!actual.has_value(), name, "<error>", "<ok>");
      } else {
        std::string want;
        for (const Value& id : expected.find("ok")->as_array()) want += std::to_string(*id.to_int64()) + ",";
        std::string got;
        if (actual) {
          for (const auto& u : *actual) got += std::to_string(u.id) + ",";
        }
        t.note(actual.has_value() && want == got, name, want, actual ? got : "<error " + actual.error().message + ">");
      }
    }

    // filtered
    {
      const Value& expected = *c.find("filtered");
      auto actual = filtered_html(body, ctx);
      Tally& t = tallies["filtered"];
      if (is_error(expected)) {
        t.note(!actual.has_value(), name, "<error>", actual ? *actual : "");
      } else if (!actual) {
        t.note(false, name, ok_string(expected), "<error " + actual.error().message + ">");
      } else {
        // Deliberate (README "Rich text attributes"): `name` attributes are dropped, and a style keeps
        // only plain color values. The two are compared with the style attribute removed.
        const std::string want = ok_string(expected);
        const bool has_style =
            want.find(" style=\"") != std::string::npos || actual->find(" style=\"") != std::string::npos;
        std::string a = without_attribute(*actual, "name");
        std::string w = without_attribute(want, "name");
        if (has_style) {
          a = without_attribute(a, "style");
          w = without_attribute(w, "style");
        }
        t.note(a == w, name, w, a);
        if (a == w && (a != *actual || w != want)) ++t.deliberate;
      }
    }
  }

  // The checks of the opengraph URL, as the Rust test does.
  for (const Value& u : corpus.find("web_urls")->as_array()) {
    const std::string value = str(u, "value");
    const std::string host = str(u, "host");
    const Value& expected = *u.find("result");
    auto actual = web_url(value, host);
    Tally& t = tallies["web_url"];
    if (is_error(expected)) {
      t.note(!actual.has_value(), value, "<error>", "<ok>");
    } else {
      const Value* v = expected.find("ok");
      const bool has = v != nullptr && v->get_string() != nullptr;
      const bool ok = actual.has_value() && actual->has_value() == has && (!has || **actual == *v->get_string());
      t.note(ok, value, has ? *v->get_string() : "<none>",
             !actual ? "<error>" : (actual->has_value() ? **actual : "<none>"));
    }
  }

  std::size_t failed = 0;
  for (const auto& [field, t] : tallies) {
    std::printf("corpus %-20s %zu of %zu equal (%zu by a documented difference)\n", field.c_str(), t.equal, t.total,
                t.deliberate);
    for (const auto& f : t.failures) {
      std::printf("%s\n", f.c_str());
    }
    failed += t.total - t.equal;
    CHECK(t.equal == t.total);
  }
  CHECK(failed == 0);
}
