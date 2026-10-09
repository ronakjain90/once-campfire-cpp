// SearchesController: index, create, clear. Rails: app/controllers/searches_controller.rb. Rust: crates/campfire/src/
// controllers/searches.rs.
//
// The page cache key of the index comes from the rows that the handler reads in the tracked scope (the account, the
// last room, the messages that match, the recent searches) and from the facets: the text of `params[:q]` (it makes the
// query and the value of the search field). The rows that a message fragment needs are read with no tracking: the
// version of the message (its id and `updated_at`) and the commit epoch stand for them, as on the room page.
#include <string>
#include <vector>

#include "app/concerns.hpp"
#include "app/controllers/rooms.hpp"
#include "app/dispatch.hpp"
#include "app/message_actions.hpp"
#include "app/message_presenter.hpp"
#include "app/render_page.hpp"
#include "core/commit_epoch.hpp"
#include "models/search.hpp"
#include "models/search_query.hpp"
#include "routes/routes.hpp"
#include "views/messages/support.hpp"
#include "views/searches/index.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::controllers {

namespace {

using views::messages::MessageItem;

// Reads without folding the statements into the page key. The scope comes back when the object goes.
class UntrackedReads {
 public:
  explicit UntrackedReads(Rq& rq) : conn_(&rq.db()), scope_(conn_->scope()) { conn_->set_scope(nullptr); }
  UntrackedReads(const UntrackedReads&) = delete;
  UntrackedReads& operator=(const UntrackedReads&) = delete;
  ~UntrackedReads() { conn_->set_scope(scope_); }

 private:
  db::Connection* conn_;
  db::DependencyScope* scope_;
};

// `render @messages`: a message whose fragment the cache holds needs no view.
Flow<std::vector<MessageItem>> message_items(Rq& rq, const std::vector<models::Message>& messages) {
  const UntrackedReads untracked(rq);
  MessagePresenter presenter(rq.db(), rq.arena(), rq.app, std::string(rq.info.host()));
  views::FragmentCache& cache = views::fragment_cache();
  std::vector<MessageItem> items;
  items.reserve(messages.size());
  for (const models::Message& message : messages) {
    MessageItem item;
    item.id = message.id;
    item.room_id = message.room_id;
    item.client_message_id = message.client_message_id;
    item.updated_at = message.updated_at;
    if (cache.enabled()) {
      // Shared with the cache: the page copies the fragment one time, into its body.
      if (auto html = cache.get(views::messages::message_fragment_key(message.id, message.updated_at))) {
        item.html = std::move(html);
        items.push_back(std::move(item));
        continue;
      }
    }
    auto view = presenter.message(message);
    if (!view) return messages::db_failure(view.error());
    item.view = std::move(*view);
    items.push_back(std::move(item));
  }
  return items;
}

// `params[:q]`: a value that is not a string makes `gsub` raise.
Flow<std::optional<std::string>> raw_query(Rq& rq) {
  const req::Param* param = rq.params().get("q");
  if (param == nullptr || param->is_null()) return std::optional<std::string>{};
  const auto text = param->as_str();
  if (!text) return fail_internal("undefined method 'gsub'");
  return std::optional<std::string>(std::string(*text));
}

// `query`: `params[:q]&.gsub(/[^[:word:]]/, " ")`.
std::optional<std::string> query_of(const std::optional<std::string>& q) {
  if (!q) return std::nullopt;
  return models::search_query::sanitize(*q);
}

}  // namespace

// `before_action :set_messages` is part of `index`: the other actions need no result of it, and the match terms are
// quoted words, which FTS5 always accepts.
Task<Flow<net::Response>> searches_index(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto raw = raw_query(rq);
  if (!raw) co_return std::unexpected(std::move(raw.error()));
  db::DependencyScope& deps = rq.track();
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));
  const std::int64_t user_id = rq.current_user()->id;

  const std::optional<std::string> query = query_of(*raw);
  const bool present = query && models::search_query::is_present(*query);
  std::vector<models::Message> found;
  if (present) {
    auto result =
        models::messages::search_reachable(rq.db(), rq.arena(), user_id, models::search_query::match_terms(*query));
    if (!result) co_return messages::db_failure(result.error());
    found = std::move(*result);
  }
  auto recent = models::searches::recent_queries(rq.db(), rq.arena(), user_id);
  if (!recent) co_return fail_internal(recent.error().message);
  if (auto format = ensure_html(rq); !format) co_return std::unexpected(std::move(format.error()));

  views::searches::IndexView index;
  if (present) index.query = *query;
  index.q = *raw;
  index.recent_searches = std::move(*recent);
  index.return_to_room_id = layout->last_room_visited_id.value_or(0);

  PageSpec spec;
  spec.name = "searches#index";
  spec.title = "Search";
  spec.body_class = "sidebar searches";
  spec.parts_etag = true;
  spec.nav = [&](Out& out, const views::ViewContext& ctx) { views::searches::index_nav(out, ctx, index); };
  spec.sidebar = [&](Out& out, const views::ViewContext& ctx) { views::searches::index_sidebar(out, ctx, index); };
  spec.content = [&](Out& out, const views::ViewContext& ctx) { views::searches::index_content(out, ctx, index); };
  spec.footer = [&](Out& out, const views::ViewContext& ctx) { views::searches::index_footer(out, ctx, index); };
  // The message fragments are read on a miss only.
  spec.prepare = [&]() -> Flow<void> {
    auto items = message_items(rq, found);
    if (!items) return std::unexpected(std::move(items.error()));
    index.items = std::move(*items);
    return {};
  };
  spec.facets = [&](db::DependencyScope& scope, const LayoutData&) {
    scope.facet("q", raw->has_value() ? "1" + **raw : std::string("0"));
    // The message fragments are keyed by the commit epoch of the snapshot, not by their rows.
    scope.facet("fragment_epoch", fragment_epoch());
  };
  co_return render_page(rq, 200, spec, deps, *layout);
}

// `Current.user.searches.record(query)`, then `redirect_to searches_url(q: query)`.
Task<Flow<net::Response>> searches_create(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto raw = raw_query(rq);
  if (!raw) co_return std::unexpected(std::move(raw.error()));
  const std::optional<std::string> query = query_of(*raw);
  // A nil query violates the NOT NULL of the column.
  if (!query) co_return fail_internal("NOT NULL constraint failed: searches.query");
  const std::int64_t user_id = rq.current_user()->id;
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(),
                                           [&](db::Tx& tx) { return models::searches::record(tx, user_id, *query); });
  if (!written) co_return fail_internal(written.error().message);
  co_return rq.redirect_to(rq.url_for(views::searches::search_path(*query)));
}

// `Current.user.searches.destroy_all`, then `redirect_to searches_url`.
Task<Flow<net::Response>> searches_clear(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto raw = raw_query(rq);
  if (!raw) co_return std::unexpected(std::move(raw.error()));
  const std::int64_t user_id = rq.current_user()->id;
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(),
                                           [&](db::Tx& tx) { return models::searches::destroy_all(tx, user_id); });
  if (!written) co_return fail_internal(written.error().message);
  co_return rq.redirect_to(rq.url_for(campfire::routes::searches()));
}

}  // namespace campfire::app::controllers

namespace campfire::routes::searches {

Task<net::Response> index(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::searches_index);
}
Task<net::Response> create(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::searches_create);
}
Task<net::Response> clear(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::searches_clear);
}

}  // namespace campfire::routes::searches
