// The layouts against the golden pages of the reference app (test/golden/*.html, from the Rust
// repo, made by Rails). Each test cuts a golden page into the parts of a layout (head, nav,
// content, footer, sidebar, flash, user), renders the C++ layout from them, and compares the bytes
// with the page. Only the CSRF tags (and the empty csp line) are not in our output: the test
// removes them from the golden page first (Rust "Known differences").
#include <doctest.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>

#include "views/layout.hpp"
#include "views/templates.gen.hpp"

namespace {

using namespace campfire;         // NOLINT
using namespace campfire::views;  // NOLINT

std::string read_golden(const std::string& name) {
  const std::string path = std::string(CAMPFIRE_VIEWS_TEST_DIR) + "/golden/" + name + ".html";
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "cannot open " << path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// The text between `from` and `to`, searching from `*pos`; moves `*pos` after `to`.
std::string between(const std::string& s, const std::string& from, const std::string& to, std::size_t* pos) {
  const std::size_t a = s.find(from, *pos);
  REQUIRE_MESSAGE(a != std::string::npos, "missing " << from);
  const std::size_t start = a + from.size();
  const std::size_t b = s.find(to, start);
  REQUIRE_MESSAGE(b != std::string::npos, "missing " << to);
  *pos = b + to.size();
  return s.substr(start, b - start);
}

// Removes the CSRF meta lines, and the csp line that follows them in the application layout.
std::string without_csrf(const std::string& page, bool csp_line) {
  const std::size_t a = page.find("    <meta name=\"csrf-param\"");
  REQUIRE(a != std::string::npos);
  const std::size_t token = page.find("<meta name=\"csrf-token\"", a);
  std::size_t end = page.find('\n', token) + 1;
  if (csp_line) {
    REQUIRE_EQ(page.substr(end, 5), "    \n");
    end += 5;
  }
  return page.substr(0, a) + page.substr(end);
}

std::string str_match(const std::string& s, const std::string& re) {
  std::smatch m;
  REQUIRE_MESSAGE(std::regex_search(s, m, std::regex(re)), re);
  return m[1];
}

// Digested asset paths of the page: "remove.svg" -> "/assets/remove-0e7a045d.svg".
std::map<std::string, std::string> asset_map(const std::string& page) {
  std::map<std::string, std::string> map;
  const std::regex re(R"(/assets/([A-Za-z0-9_-]+?)-[0-9a-f]{8}\.(svg|png))");
  for (auto it = std::sregex_iterator(page.begin(), page.end(), re); it != std::sregex_iterator(); ++it) {
    map[(*it)[1].str() + "." + (*it)[2].str()] = (*it)[0].str();
  }
  return map;
}

struct Page {
  std::string title, nav, content, footer, sidebar, head, stylesheet, importmap;
  std::string body_class;
  ViewContext ctx;
  std::map<std::string, std::string> assets;
};

Page cut(const std::string& page) {
  Page p;
  std::size_t pos = 0;
  p.title = between(page, "<title>", "</title>", &pos);
  p.stylesheet = page.substr(page.find("<link rel=\"stylesheet\""));
  p.stylesheet.resize(p.stylesheet.find("\n    "));
  const std::size_t after_styles = page.find(p.stylesheet) + p.stylesheet.size();
  pos = after_styles;
  std::string rest = page.substr(pos);
  if (rest.starts_with("\n    <style data-turbo-track=\"reload\">")) {
    p.ctx.custom_styles = between(page, "<style data-turbo-track=\"reload\">", "</style>", &pos);
  }
  const std::size_t im = page.find("<script type=\"importmap\"");
  const std::string end_mark = "<script type=\"module\">import \"application\"</script>";
  const std::size_t im_end = page.find(end_mark) + end_mark.size();
  p.importmap = page.substr(im, im_end - im);
  pos = im_end;
  p.head = between(page, "\n\n    ", "\n  </head>", &pos);
  pos = 0;
  p.nav = between(page, "<nav id=\"nav\">\n      ", "\n    </nav>", &pos);
  p.content = between(page, "<main id=\"main-content\">\n      ", "\n\n      <footer id=\"footer\">", &pos);
  p.footer = between(page, "\n        ", "\n      </footer>", &pos);
  p.sidebar = between(page, "<aside id=\"sidebar\" data-controller=\"toggle-class\" data-toggle-class-toggle-class=\"open\">\n      ",
                      "\n    </aside>", &pos);
  const std::size_t flash = page.find("<span class=\"for-screen-reader\" role=\"alert\"");
  if (flash != std::string::npos) {
    const std::string text = str_match(page.substr(flash), R"re(aria-atomic="true">([^<]*)</span>)re");
    if (page.find("--flash-background: var(--color-negative)") != std::string::npos) {
      p.ctx.flash_alert = text;
    } else {
      p.ctx.flash_notice = text;
    }
  }
  std::smatch user;
  if (std::regex_search(page, user, std::regex(R"re(current-user-id" content="(\d+)" /><meta name="current-user-name" content="([^"]*)")re"))) {
    p.ctx.current_user = CurrentUser{std::stoll(user[1]), user[2], false, false, ""};
  }
  p.ctx.cable_url = str_match(page, R"re(action-cable-url" content="([^"]*)")re");
  p.ctx.vapid_public_key = str_match(page, R"re(vapid-public-key" content="([^"]*)")re");
  p.ctx.account.logo_url = str_match(page, R"re(rel="icon" href="([^"]*)")re");
  p.body_class = str_match(page, R"re(<body class="([^"]*)")re");
  for (const char* suffix : {" account-has-logo", "account-has-logo"}) {
    if (p.body_class.ends_with(suffix)) {
      p.ctx.account.has_logo = true;
      p.body_class.resize(p.body_class.size() - std::string(suffix).size());
    }
  }
  for (const char* suffix : {" admin", "admin"}) {
    if (p.body_class.ends_with(suffix)) {
      p.ctx.current_user->administrator = true;
      p.body_class.resize(p.body_class.size() - std::string(suffix).size());
    }
  }
  p.ctx.stylesheet_tags = p.stylesheet;
  p.ctx.importmap_tags = p.importmap;
  p.assets = asset_map(page);
  return p;
}

std::string render_application(Page& p) {
  p.ctx.asset_path = [&p](std::string_view logical) { return p.assets.at(std::string(logical)); };
  const auto text = [](const std::string& s) { return [&s](Out& o) { o.append(SafeHtml::trusted(s)); }; };
  LayoutParts parts;
  parts.page_title = p.title;
  if (!p.body_class.empty()) {
    parts.body_class = p.body_class;
  }
  parts.head = text(p.head);
  parts.nav = text(p.nav);
  parts.content = text(p.content);
  parts.footer = text(p.footer);
  parts.sidebar = text(p.sidebar);
  Out out;
  layouts::application(out, p.ctx, parts);
  return out.to_string();
}

}  // namespace

TEST_CASE("layouts: application layout equals the pages of Rails") {
  int total = 0;
  int passed = 0;
  for (const char* name : {"custom_styles_layout", "account_edit_notice", "account_edit_with_logo", "sessions_new_rejected",
                           "sessions_new", "welcome"}) {
    const std::string golden = read_golden(name);
    Page p = cut(golden);
    const std::string expected = without_csrf(golden, true);
    const std::string actual = render_application(p);
    ++total;
    if (actual == expected) {
      ++passed;
    } else {
      std::size_t i = 0;
      while (i < actual.size() && i < expected.size() && actual[i] == expected[i]) ++i;
      FAIL_CHECK(name << ": first difference at byte " << i << "\n  expected: "
                      << expected.substr(i > 40 ? i - 40 : 0, 120) << "\n  actual:   " << actual.substr(i > 40 ? i - 40 : 0, 120));
    }
  }
  std::printf("GOLDEN layouts application comparisons=%d pass=%d\n", total, passed);
  CHECK_EQ(passed, total);
}

TEST_CASE("layouts: turbo frame layout equals the frame page of Rails") {
  const std::string golden = read_golden("sidebar_frame");
  std::size_t pos = 0;
  const std::string content = between(golden, "<body>\n    ", "\n  </body>", &pos);
  LayoutParts parts;
  parts.content = [&](Out& o) { o.append(SafeHtml::trusted(content)); };
  Out out;
  layouts::turbo_rails::frame(out, parts);
  const bool same = out.to_string() == without_csrf(golden, false);
  std::printf("GOLDEN layouts frame comparisons=1 pass=%d\n", same ? 1 : 0);
  CHECK(same);
}

TEST_CASE("layouts: mailer layouts and the Action Text content layout") {
  const Region body = [](Out& o) { o.append(SafeHtml::literal("<p>hi</p>")); };
  Out text;
  layouts::mailer_text(text, body);
  CHECK_EQ(text.to_string(), "<p>hi</p>\n");
  Out content;
  layouts::action_text::contents::content(content, body);
  CHECK_EQ(content.to_string(), "<div class=\"lexxy-content\">\n  <p>hi</p></div>\n");
  Out html;
  layouts::mailer_html(html, body);
  CHECK(html.to_string().ends_with("  <body>\n    <p>hi</p>\n  </body>\n</html>\n"));
}
