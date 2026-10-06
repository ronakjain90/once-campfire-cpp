// Autocompletable::UsersController. Rails: app/controllers/autocompletable/users_controller.rb. Rust:
// crates/campfire/src/controllers/autocompletable.rs and presenters/pagination.rs (geared_pagination).
#include <algorithm>

#include "app/concerns.hpp"
#include "app/controllers/common.hpp"
#include "app/dispatch.hpp"
#include "app/message_actions.hpp"
#include "app/message_presenter.hpp"
#include "app/page.hpp"
#include "compat/global_id.hpp"
#include "compat/json.hpp"
#include "compat/ruby.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::controllers {

namespace {

using messages::db_failure;

constexpr std::int64_t kPerPage = 20;

// `param.to_i > 0 ? param.to_i : 1`, capped so that the page arithmetic cannot overflow.
std::int64_t page_number(std::optional<std::string_view> param) {
  const std::int64_t n = param ? compat::to_i(*param) : 0;
  return std::clamp<std::int64_t>(n, 1, 1'000'000'000);
}

std::string unencode(std::string_view value) {
  std::string out;
  const auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size() + 0 && hex(value[i + 1]) >= 0 && hex(value[i + 2]) >= 0) {
      out.push_back(static_cast<char>(hex(value[i + 1]) * 16 + hex(value[i + 2])));
      i += 2;
    } else {
      out.push_back(value[i]);
    }
  }
  return out;
}

// Addressable's `uri.query_values = (uri.query_values || {}).merge("page" => page)`: the query is encoded again from a
// hash, so the keys come out sorted, a repeated key keeps the last value, and "+" in a value is a space.
std::string with_page(std::string_view url, std::string_view page) {
  std::string_view fragment;
  bool has_fragment = false;
  if (const auto hash = url.find('#'); hash != std::string_view::npos) {
    fragment = url.substr(hash + 1);
    has_fragment = true;
    url = url.substr(0, hash);
  }
  std::string_view query;
  if (const auto mark = url.find('?'); mark != std::string_view::npos) {
    query = url.substr(mark + 1);
    url = url.substr(0, mark);
  }
  std::vector<std::pair<std::string, std::optional<std::string>>> values;
  std::size_t at = 0;
  while (at <= query.size()) {
    std::size_t end = query.find('&', at);
    if (end == std::string_view::npos) end = query.size();
    const std::string_view pair = query.substr(at, end - at);
    at = end + 1;
    if (pair.empty()) continue;
    std::string key;
    std::optional<std::string> value;
    if (const auto eq = pair.find('='); eq != std::string_view::npos) {
      key = unencode(pair.substr(0, eq));
      std::string raw(pair.substr(eq + 1));
      std::ranges::replace(raw, '+', ' ');
      value = unencode(raw);
    } else {
      key = unencode(pair);
    }
    const auto found = std::ranges::find_if(values, [&](const auto& entry) { return entry.first == key; });
    if (found != values.end()) {
      found->second = std::move(value);
    } else {
      values.emplace_back(std::move(key), std::move(value));
    }
  }
  const auto page_entry = std::ranges::find_if(values, [](const auto& entry) { return entry.first == "page"; });
  if (page_entry != values.end()) {
    page_entry->second = std::string(page);
  } else {
    values.emplace_back("page", std::string(page));
  }
  std::ranges::stable_sort(values, [](const auto& a, const auto& b) { return a.first < b.first; });
  std::string out(url);
  out += '?';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out += '&';
    out += compat::url_encode(values[i].first);
    if (values[i].second) out += "=" + compat::url_encode(*values[i].second);
  }
  if (has_fragment) out += "#" + std::string(fragment);
  return out;
}

Task<Flow<net::Response>> users_index(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  const std::int64_t user_id = rq.current_user()->id;

  // `params[:room_id].present? ? Current.user.rooms.find(params[:room_id]).users : User.all`
  std::optional<std::int64_t> room_id;
  if (const req::Param* param = rq.params().get("room_id"); param != nullptr && param->is_present()) {
    const auto text = param->as_str();
    const auto id = text ? compat::integer_cast(*text) : std::nullopt;
    if (!id) co_return fail_with(ErrorKind::NotFound, "Couldn't find Room");
    auto room = models::room_refs::find_for_user(rq.db(), rq.arena(), user_id, *id);
    if (!room) co_return db_failure(room.error());
    if (!*room) co_return fail_with(ErrorKind::NotFound, "Couldn't find Room");
    room_id = (*room)->id;
  }
  // The rich text editor's mentions prompt filters with `filter`, the autocomplete inputs with `query`.
  std::optional<std::string> query;
  for (const char* key : {"filter", "query"}) {
    const req::Param* param = rq.params().get(key);
    if (param != nullptr && param->is_present()) {
      query = param->to_s();
      break;
    }
  }
  std::optional<std::string_view> query_view;
  if (query) query_view = *query;
  auto users = models::room_refs::autocompletable_users(rq.db(), rq.arena(), room_id, query_view);
  if (!users) co_return db_failure(users.error());

  const std::int64_t number = page_number(rq.param_str("page"));
  const std::int64_t count = static_cast<std::int64_t>(users->size());
  const std::int64_t offset = (number - 1) * kPerPage;
  std::vector<models::User> records;
  for (std::int64_t i = offset; i < std::min(count, offset + kPerPage); ++i)
    records.push_back((*users)[static_cast<std::size_t>(i)]);
  std::int64_t page_count = 0;
  for (std::int64_t residual = count; residual > 0; residual -= kPerPage) ++page_count;
  page_count = std::max<std::int64_t>(page_count, 1);

  const req::Format offered[] = {&req::mime::HTML, &req::mime::JSON};
  auto format = rq.respond_to(offered);
  if (!format) co_return std::unexpected(std::move(format.error()));
  const bool json = *format == &req::mime::JSON;
  if (json) {
    // `set_paginated_headers` (after_action), for JSON requests.
    rq.set_header("x-total-count", std::to_string(count));
    if (number != page_count)
      rq.set_header("link", "<" + with_page(rq.info.url(), std::to_string(number + 1)) + ">; rel=\"next\"");
  }

  const std::string base_url = rq.url_for("");
  compat::json::Value::Array array;
  std::vector<views::autocompletable::MentionUserView> views;
  for (const models::User& user : records) {
    const std::string sgid = compat::global_id::attachable_sgid(
        rq.app.secrets, compat::global_id::GlobalId::make("User", std::to_string(user.id)));
    if (json) {
      Out escaped;
      html_escape(escaped, user.name);
      using compat::json::Value;
      array.push_back(Value(Value::Object{{"name", Value(escaped.to_string())},
                                          {"value", Value(user.id)},
                                          {"avatar_url", Value(base_url + avatar_path(rq.app, user))},
                                          {"sgid", Value(sgid)}}));
    } else {
      views.push_back({user.id, user.name, user_title(user), sgid, avatar_path(rq.app, user)});
    }
  }
  if (json) co_return rq.json(200, compat::json::Value(std::move(array)));

  // `render layout: false`: <lexxy-prompt-item> elements for the mentions prompt.
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));
  const views::ViewContext ctx = make_view_context(rq, *layout);
  Out out(rq.ctx.resource());
  views::autocompletable::users::index(out, ctx, views);
  co_return rq.html(200, std::move(out));
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::autocompletable_users_controller {

Task<net::Response> index(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::users_index);
}

}  // namespace campfire::routes::autocompletable_users_controller
