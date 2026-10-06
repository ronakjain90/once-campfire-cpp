// Tests of format negotiation. The cases come from the tests of crates/kit/src/format.rs.
#include <doctest.h>

#include <string>
#include <vector>

#include "req/format.hpp"

using namespace campfire::req;

namespace {

std::vector<std::string> symbols(const std::vector<Format>& formats) {
  std::vector<std::string> out;
  for (Format f : formats) out.emplace_back(f->symbol);
  return out;
}

using Names = std::vector<std::string>;

Names fmts(const char* accept, const char* path, bool xhr = false) {
  NegotiationInput in;
  if (accept != nullptr) {
    in.accept = accept;
    in.has_accept = true;
  }
  in.path = path;
  in.xhr = xhr;
  auto r = formats(in);
  REQUIRE(r.has_value());
  return symbols(*r);
}

Names accept_order(const char* accept) {
  auto r = parse_accept(accept);
  REQUIRE(r.has_value());
  return symbols(*r);
}

}  // namespace

TEST_CASE("format: a browser Accept header gives html") {
  const char* chrome = "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8";
  CHECK(fmts(chrome, "/rooms/1") == Names{"html"});
  CHECK(fmts(nullptr, "/rooms/1") == Names{"html"});
  CHECK(fmts("*/*", "/rooms/1") == Names{"*/*"});
  CHECK(fmts("text/vnd.turbo-stream.html, text/html, application/xhtml+xml", "/m") == Names{"turbo_stream", "html"});
}

TEST_CASE("format: quality ordering") {
  CHECK(fmts("text/html;q=0.5, application/json", "/") == Names{"json", "html"});
  CHECK(fmts("application/json, */*", "/") == Names{"html"});
  CHECK(fmts("*/*, application/json;q=0.1", "/") == Names{"html"});
  CHECK(fmts("application/json;q=0.1,text/html;q=0.1", "/") == Names{"json", "html"});
}

TEST_CASE("format: q-values read like Rails") {
  struct Row {
    const char* accept;
    Names order;
  };
  const std::vector<Row> rows = {
      {"text/html;q=, application/json", {"html", "json"}},
      {"text/html; q=, application/json;q=0.5", {"html", "json"}},
      {"text/html;q=;q=, application/json;q=0.5", {"html", "json"}},
      {"text/html;q=0.5, application/json;q=", {"json", "html"}},
      {"text/html;q=;x=1, application/json", {"json", "html"}},
      {"text/html;q=;q=0.9, application/json;q=0.5", {"json", "html"}},
      {"text/html;q=0.4;q=0.9, application/json;q=0.5", {"json", "html"}},
      {"text/html;q=0.5.1, application/json;q=0.6", {"json", "html"}},
      {"text/html;q=0.5.1.2, application/json;q=0.49", {"html", "json"}},
      {"text/html;q=\"0.9\", application/json;q=0.5", {"html", "json"}},
      {"text/html;q=\"\"0.9, application/json;q=0.5", {"json", "html"}},
      {"text/html;q=1e-1, application/json;q=0.5", {"json", "html"}},
      {"text/html;q=1_0, application/json;q=5", {"html", "json"}},
      {"text/html;q=abc, application/json;q=0.1", {"json", "html"}},
      {"text/html;q=+0.3, application/json;q=0.2", {"html", "json"}},
      {"text/html;q= 0.3, application/json;q=0.2", {"html", "json"}},
      {"text/html;q=1e17, application/json;q=1e18", {"json", "html"}},
      {"text/html;q=1e19, application/json;q=1e20", {"json", "html"}},
      {"text/html;q=-0.001, application/json;q=0", {"html", "json"}},
      {"application/json;q=0, text/html;q=-0.001", {"json", "html"}},
      {"text/html;q=0.5, application/json;q=+0x1", {"json", "html"}},
      {"text/html;q=0.5, application/json;q=0x1", {"html", "json"}},
  };
  for (const auto& row : rows) {
    CAPTURE(row.accept);
    CHECK(accept_order(row.accept) == row.order);
  }
  CHECK(accept_order("*/*;q=, application/json;q=0.5") == Names{"json", "*/*"});
}

TEST_CASE("format: an infinite q-value sorts first or last") {
  CHECK(accept_order("text/html;q=1e400, application/json") == Names{"html", "json"});
  CHECK(accept_order("text/html;q=-1e400, application/json") == Names{"json", "html"});
}

TEST_CASE("format: single types, wildcards and XML folding") {
  CHECK(fmts("application/json", "/") == Names{"json"});
  CHECK(fmts("application/json; charset=utf-8", "/") == Names{"json"});
  CHECK(fmts("image/svg+xml", "/") == Names{"svg"});
  CHECK(fmts("application/x-unknown", "/").empty());
  NegotiationInput bad;
  bad.accept = "garbage";
  bad.has_accept = true;
  bad.path = "/";
  CHECK_FALSE(formats(bad).has_value());

  const auto text = fmts("text/*", "/");
  REQUIRE(text.size() > 3);
  CHECK(Names(text.begin(), text.begin() + 3) == Names{"html", "text", "js"});

  CHECK(fmts("text/xml, application/rss+xml", "/") == Names{"xml", "rss"});
  CHECK(fmts("application/xml, application/rss+xml", "/") == Names{"rss", "xml"});
  CHECK(fmts("text/xml;q=0.9, application/xml;q=0.5, text/html", "/") == Names{"html", "xml"});
}

TEST_CASE("format: path extension, format parameter and XHR") {
  CHECK(fmts(nullptr, "/users/1/avatar.svg") == Names{"svg"});
  CHECK(fmts(nullptr, "/messages.json") == Names{"json"});
  CHECK(fmts(nullptr, "/x.unknownext") == Names{"html"});
  NegotiationInput in;
  in.format_param = "json";
  in.has_format_param = true;
  in.accept = "text/html";
  in.has_accept = true;
  in.path = "/";
  CHECK(symbols(*formats(in)) == Names{"json"});
  NegotiationInput nope;
  nope.format_param = "nope";
  nope.has_format_param = true;
  nope.path = "/";
  CHECK(formats(nope)->empty());

  CHECK(fmts(nullptr, "/", true) == Names{"js"});
  NegotiationInput xhr;
  xhr.xhr = true;
  xhr.content_type = "application/json";
  xhr.path = "/";
  CHECK(symbols(*formats(xhr)) == Names{"json"});
}

TEST_CASE("format: negotiate picks the first acceptable format") {
  const Format turbo_html[] = {&mime::TURBO_STREAM, &mime::HTML};
  const Format html_json[] = {&mime::HTML, &mime::JSON};
  const Format all[] = {&mime::ALL};
  const Format png[] = {&mime::PNG};
  const Format html_all[] = {&mime::HTML, &mime::ALL};
  const Format html_only[] = {&mime::HTML};
  CHECK(negotiate(turbo_html, html_json) == &mime::HTML);
  CHECK(negotiate(all, html_json) == &mime::HTML);
  CHECK(negotiate(png, html_json) == nullptr);
  CHECK(negotiate(png, html_all) == &mime::PNG);
  CHECK(negotiate({}, html_only) == nullptr);
}
