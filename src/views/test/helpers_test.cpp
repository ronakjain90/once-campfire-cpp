// Helpers against the bytes that the Rails helpers write (test/helper_golden.json, made by
// gen_helper_golden.rb in campfire-reference). Rails: ActionView and turbo-rails helpers.
#include <doctest.h>

#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>

#include "compat/json.hpp"
#include "core/timestamp.hpp"
#include "views/helpers/helpers.hpp"

namespace {

using namespace campfire;  // NOLINT
using namespace campfire::views;  // NOLINT
using namespace campfire::views::helpers;  // NOLINT
namespace json = campfire::compat::json;

using Render = std::function<void(Out&, const ViewContext&, std::string_view signed_name)>;

SafeHtml lit(std::string_view s) { return SafeHtml::trusted(s); }

Attrs with_nil(std::string_view name) {
  Attrs a;
  a.set(name, std::nullopt);
  return a;
}

const std::map<std::string, Render>& cases() {
  static const std::map<std::string, Render> table = {
      {"tag_div", [](Out& o, const ViewContext&, auto) {
         content_tag_text(o, "div", attrs().cls("a&b").hidden().data("turbo_frame", "_top").data("x", "<").aria("hidden", "true").tabindex(-1), "x<");
       }},
      {"tag_br", [](Out& o, const ViewContext&, auto) { builder_tag(o, "br", attrs().cls("c")); }},
      {"tag_turbo_frame_name", [](Out& o, const ViewContext&, auto) { builder_tag(o, "turbo_frame", attrs().id("f")); }},
      {"content_tag_textarea", [](Out& o, const ViewContext&, auto) { content_tag_text(o, "textarea", attrs().id("t"), "x<"); }},
      {"attrs_boolean", [](Out& o, const ViewContext&, auto) {
         builder_tag(o, "input", attrs().type("checkbox").checked(false).disabled(true).required(false).attr("data-x", false).attr("aria-y", true));
       }},
      {"attrs_safe_value", [](Out& o, const ViewContext&, auto) {
         builder_tag(o, "div", attrs().attr("data-action", lit("a->\"b\"&c")));
       }},
      {"legacy_tag", [](Out& o, const ViewContext&, auto) { legacy_tag(o, "img", attrs().alt("a").attr("src", "/x.png")); }},
      {"link_to", [](Out& o, const ViewContext&, auto) {
         link_to_text(o, "a<b", "/x?y=1&z=2", attrs().cls("btn").data("turbo", false));
       }},
      {"link_to_block", [](Out& o, const ViewContext&, auto) {
         link_to(o, "/x", attrs().cls("btn").id("i"), lit("<b>c</b>"));
       }},
      {"link_to_if_false", [](Out& o, const ViewContext&, auto) { link_to_if(o, false, "a<", "/x", {}); }},
      {"link_to_if_true", [](Out& o, const ViewContext&, auto) { link_to_if(o, true, "a<", "/x", attrs().title("t")); }},
      {"mail_to", [](Out& o, const ViewContext&, auto) { mail_to(o, "a+b@x.com"); }},
      {"button_to_delete", [](Out& o, const ViewContext&, auto) {
         button_to(o, "/x?a=1&b=2", attrs().method("delete").cls("btn").attr("form_class", "f"), lit("go"));
       }},
      {"button_to_default", [](Out& o, const ViewContext&, auto) {
         button_to(o, "/x", attrs().cls("btn").data("turbo", false), lit("go<"));
       }},
      {"button_to_put_and_get", [](Out& o, const ViewContext&, auto) {
         button_to(o, "/x", attrs().method("put"), lit("a"));
         button_to(o, "/y", attrs().method("get"), lit("b"));
         button_to(o, "/z", attrs().method("patch").title("p"), lit("c"));
       }},
      {"image_tag_size", [](Out& o, const ViewContext& c, auto) { image_tag(o, c, "/img.svg", attrs().aria("hidden", true).size(20)); }},
      {"image_tag_size_xy", [](Out& o, const ViewContext& c, auto) {
         image_tag(o, c, "/img.svg", attrs().alt("A<").size("20x30").cls("c"));
       }},
      {"image_tag_url", [](Out& o, const ViewContext& c, auto) { image_tag(o, c, "https://example.com/a.png", attrs().cls("c")); }},
      {"turbo_frame_tag", [](Out& o, const ViewContext&, auto) {
         turbo_frame_tag(o, "f", attrs().cls("c").attr("src", "/x").attr("target", "_top"), [](Out& i) { i.append(lit("in")); });
       }},
      {"turbo_frame_tag_plain", [](Out& o, const ViewContext&, auto) {
         turbo_frame_tag(o, "room_1", attrs().cls("c"), [](Out& i) { i.append(lit("in")); });
       }},
      {"turbo_stream_append", [](Out& o, const ViewContext&, auto) {
         turbo_stream(o, "append", "messages", [](Out& i) { i.append(lit("<p>x</p>")); });
       }},
      {"turbo_stream_replace", [](Out& o, const ViewContext&, auto) {
         turbo_stream(o, "replace", "message_1", [](Out& i) { i.append(lit("<p>y</p>")); });
       }},
      {"turbo_stream_remove", [](Out& o, const ViewContext&, auto) { turbo_stream_remove(o, "message_1"); }},
      {"local_datetime_tag", [](Out& o, const ViewContext&, auto) {
         local_datetime_tag(o, Timestamp::from_seconds(1767270605), "date", attrs().cls("x"));
       }},
      {"local_datetime_tag_time", [](Out& o, const ViewContext&, auto) {
         local_datetime_tag(o, Timestamp::from_seconds(1780272000), "time");
       }},
      {"page_requires_reload", [](Out& o, const ViewContext&, auto) { turbo_page_requires_reload_tag(o); }},
      {"turbo_stream_from", [](Out& o, const ViewContext&, std::string_view signed_name) { turbo_stream_from(o, signed_name); }},
      {"form_with_fields", [](Out& o, const ViewContext&, auto) {
         const FormWith form = FormWith("/x?a=1").model("user").cls("c").id("i").method("patch").data("controller", "form").data("action", "a->b");
         form_with(o, form, [&](Out& i) {
           form.text_field(i, "name", std::nullopt, attrs().value("a<b").cls("input").maxlength(5));
           form.email_field(i, "email", std::nullopt, with_nil("value").merge(attrs().required(true)));
           form.password_field(i, "password", attrs().value("ignored").autocomplete("off"));
           form.hidden_field(i, "h", std::nullopt, attrs().value("1"));
           form.text_area(i, "bio", std::nullopt, attrs().rows(3).value("x\n<y"));
           form.check_box(i, "ok", attrs().cls("c").attr("checked", true), "1", "0", "");
           form.check_box(i, "off", {}, "1", "0", "");
           form.file_field(i, "avatar", attrs().attr("accept", "image/*"));
           form.url_field(i, "site", std::nullopt, attrs().value("http://x").name("custom").id("cid"));
           button_tag(i, attrs().cls("btn"), [](Out& b) { b.append(lit("Save")); });
         });
       }},
      {"form_with_plain", [](Out& o, const ViewContext&, auto) {
         const FormWith form = FormWith("/search").method("get");
         form_with(o, form, [&](Out& i) { form.text_field(i, "q", std::nullopt, attrs().value("v").cls("input")); });
       }},
      {"form_with_post_no_model", [](Out& o, const ViewContext&, auto) {
         const FormWith form = FormWith("/s").cls("k");
         form_with(o, form, [&](Out& i) {
           form.text_field(i, "q", std::nullopt, {});
           form.text_area(i, "body", std::nullopt, {});
         });
       }},
      {"form_with_nested", [](Out& o, const ViewContext&, auto) {
         const FormWith form = FormWith("/s").model("account");
         form_with(o, form, [&](Out& i) { form.fields_for("settings").text_field(i, "a", std::nullopt, attrs().value("1")); });
       }},
      {"hidden_field_tag", [](Out& o, const ViewContext&, auto) { hidden_field_tag(o, "a[b]", "v<", attrs().cls("c")); }},
      {"hidden_field_tag_nil", [](Out& o, const ViewContext&, auto) { hidden_field_tag(o, "q", std::nullopt, {}); }},
      {"button_tag", [](Out& o, const ViewContext&, auto) {
         button_tag(o, attrs().cls("b").data("a", 1), [](Out& b) { html_escape(b, "x<"); });
       }},
  };
  return table;
}

}  // namespace

TEST_CASE("helpers: bytes equal the Rails helpers") {
  const std::string path = std::string(CAMPFIRE_VIEWS_TEST_DIR) + "/helper_golden.json";
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "cannot open " << path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  const auto golden = json::parse(buffer.str());
  REQUIRE(golden.has_value());
  const std::string& signed_name = golden->find("signed_stream_name")->as_string();
  const auto& expected = golden->find("cases")->as_object();

  ViewContext ctx;
  ctx.asset_path = [](std::string_view logical) { return "/assets/" + std::string(logical); };

  int total = 0;
  int passed = 0;
  for (const auto& [name, html] : expected) {
    if (name == "asset_path") {
      continue;  // checked below
    }
    const auto it = cases().find(name);
    REQUIRE_MESSAGE(it != cases().end(), "no C++ case for " << name);
    Out out;
    it->second(out, ctx, signed_name);
    ++total;
    if (out.to_string() == html.as_string()) {
      ++passed;
    } else {
      FAIL_CHECK(name << "\n  expected: " << html.as_string() << "\n  actual:   " << out.to_string());
    }
  }
  std::printf("GOLDEN helper_golden.json comparisons=%d pass=%d\n", total, passed);
  CHECK_EQ(total, static_cast<int>(cases().size()));
  CHECK_EQ(passed, total);
}

TEST_CASE("helpers: asset_path passes URLs and resolves logical paths") {
  ViewContext ctx;
  ctx.asset_path = [](std::string_view logical) { return "/assets/" + std::string(logical) + "-d1"; };
  CHECK_EQ(asset_path(ctx, "arrow-left.svg"), "/assets/arrow-left.svg-d1");
  CHECK_EQ(asset_path(ctx, "/x.png"), "/x.png");
  CHECK_EQ(asset_path(ctx, "data:image/png;base64,AA"), "data:image/png;base64,AA");
  CHECK_EQ(asset_path(ctx, "https://x.com/a.png"), "https://x.com/a.png");
  CHECK_EQ(dom_id("room", "5", "messages"), "messages_room_5");
  CHECK_EQ(dom_id("room", "5"), "room_5");
}

TEST_CASE("helpers: default data goes where the data of the caller starts") {
  const Attrs given = attrs().id("x").data("b", "caller").cls("c").data("a", "override");
  const Attrs defaults = attrs().data("a", "default").data("z", "z");
  Out out;
  render_attrs(out, given.with_default_data(defaults));
  CHECK_EQ(out.to_string(), " id=\"x\" data-a=\"override\" data-z=\"z\" data-b=\"caller\" class=\"c\"");
}

TEST_CASE("helpers: assignment keeps the position of a key") {
  Attrs a = attrs().value("x").cls("c");
  a.set("value", Value("y"));
  Out out;
  render_attrs(out, a);
  CHECK_EQ(out.to_string(), " value=\"y\" class=\"c\"");
}
