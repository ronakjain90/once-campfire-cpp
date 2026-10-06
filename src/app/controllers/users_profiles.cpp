// Users::ProfilesController. Rails: app/controllers/users/profiles_controller.rb. Rust: crates/campfire/src/
// controllers/users/profiles.rs.
#include "app/active_storage.hpp"
#include "app/concerns.hpp"
#include "app/controllers/accounts_common.hpp"
#include "app/controllers/common.hpp"
#include "app/dispatch.hpp"
#include "models/attachments.hpp"
#include "models/user_admin.hpp"
#include "routes/routes.hpp"
#include "views/accounts/types.hpp"
#include "views/helpers/forms.hpp"
#include "views/helpers/users.hpp"
#include "views/templates.gen.hpp"
#include "app/controllers/sidebars.hpp"

namespace campfire::app::controllers {

namespace {

views::RoomKind room_kind(std::string_view type) {
  if (type == models::kRoomDirect) return views::RoomKind::Direct;
  if (type == models::kRoomClosed) return views::RoomKind::Closed;
  return views::RoomKind::Open;
}

// `Current.user.memberships.with_ordered_room.partition { |m| m.room.direct? }`, with `room_display_name(room)`.
Flow<void> load_memberships(Rq& rq, const models::User& user, views::ProfileShowView& view) {
  auto memberships = models::users::profile_memberships(rq.db(), rq.arena(), user.id);
  if (!memberships) return fail_internal(memberships.error().message);
  for (const models::users::ProfileMembership& m : *memberships) {
    views::ProfileMembership item;
    item.room_id = m.room_id;
    item.kind = room_kind(m.room_type);
    item.involvement = m.involvement.value_or("");
    if (item.kind == views::RoomKind::Direct) {
      // `room.users.without(for_user).pluck(:name).to_sentence.presence || for_user&.name`
      auto members = models::users::of_room(rq.db(), rq.arena(), m.room_id);
      if (!members) return fail_internal(members.error().message);
      std::vector<std::string> others;
      for (const models::User& member : *members) {
        if (member.id != user.id) others.push_back(member.name);
      }
      const std::string sentence = views::helpers::to_sentence(others, " and ");
      item.room_display_name =
          sentence.find_first_not_of(" \t\n\v\f\r") == std::string::npos ? user.name : sentence;
      view.direct_memberships.push_back(std::move(item));
    } else {
      item.room_display_name = m.room_name.value_or("");
      view.shared_memberships.push_back(std::move(item));
    }
  }
  return {};
}

// `set_user` (`Current.user`).
Task<Flow<net::Response>> profiles_show(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  const req::Format offered[] = {&req::mime::HTML};
  if (auto format = rq.respond_to(offered); !format) co_return std::unexpected(std::move(format.error()));
  const models::User user = *rq.current_user();
  db::DependencyScope& deps = rq.track();
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));

  views::ProfileShowView view;
  view.user = user_summary(rq, user);
  view.email_address = user.email_address;
  view.bio = user.bio;
  auto attached = models::attachments::is_attached(rq.db(), rq.arena(), models::attachments::Record::user(user.id), "avatar");
  if (!attached) co_return fail_internal(attached.error().message);
  view.avatar_attached = *attached;
  if (auto loaded = load_memberships(rq, user, view); !loaded) co_return std::unexpected(std::move(loaded.error()));
  // The link carries its expiry: the page depends on the clock.
  view.transfer_url = rq.url_for(campfire::routes::session_transfer(transfer_id(rq, user.id)));
  deps.mark_uncacheable();

  // `profile_form_with @user, class: "txt-medium"` and `profile_form_with @user`.
  const std::string action = campfire::routes::user_profile();
  const views::helpers::FormWith first = views::helpers::form_with_url(action)
                                             .model("user")
                                             .method("patch")
                                             .cls("txt-medium")
                                             .data("controller", "form");
  const views::helpers::FormWith form =
      views::helpers::form_with_url(action).model("user").method("patch").data("controller", "form");
  const views::helpers::FormWith text_form =
      views::helpers::form_with_url(action).model("user").method("patch").data("controller", "form");

  PageSpec spec;
  spec.name = "users/profiles#show";
  spec.title = view.user.name;
  spec.nav = [](Out& out, const views::ViewContext& ctx) { views::users::profiles::show_nav(out, ctx); };
  spec.content = [&](Out& out, const views::ViewContext& ctx) {
    views::users::profiles::show(out, ctx, view, form, first, text_form);
  };
  spec.facets = [&](db::DependencyScope& d, const LayoutData&) {
    add_link_back_facets(rq, d);
    add_platform_facet(rq, d);
  };
  co_return render_tracked_page(rq, 200, deps, *layout, spec);
}

// `@user.update user_params`, then `redirect_to user_profile_url, notice: update_notice`.
Task<Flow<net::Response>> profiles_update(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  const std::int64_t user_id = rq.current_user()->id;

  // `params.require(:user).permit(:name, :avatar, :email_address, :password, :bio).compact`
  auto required = rq.params().require("user");
  if (!required) co_return fail_with(ErrorKind::ParameterMissing, required.error().message);
  const req::ParamMap* hash = (*required)->as_hash();
  req::ParamMap params = hash == nullptr ? req::ParamMap(rq.ctx.resource())
                                         : hash->permit({"name", "avatar", "email_address", "password", "bio"},
                                                        rq.ctx.resource());
  // A key with a nil value is dropped by `compact`; a value that is not text counts as nil.
  const auto present = [&](std::string_view key) -> std::optional<std::string> {
    const req::Param* value = params.get(key);
    return value == nullptr ? std::nullopt : value->to_s();
  };
  models::UserChanges changes;
  changes.name = present("name");
  if (const auto email = present("email_address")) changes.email_address = std::optional<std::string>(*email);
  auto digest = co_await concerns::password_digest(rq, present("password"));
  if (!digest) co_return std::unexpected(std::move(digest.error()));
  changes.password_digest = std::move(*digest);
  if (const auto bio = present("bio")) changes.bio = std::optional<std::string>(*bio);

  active_storage::Assignment avatar = active_storage::assignment_from(params, "avatar");
  // `.compact` drops a nil avatar before it is assigned.
  if (const req::Param* value = params.get("avatar"); value != nullptr && value->is_null()) {
    avatar.kind = active_storage::Assignment::Kind::Unchanged;
  }
  // `params[:user][:avatar] ? ... : "✓"`: any value that is not nil counts.
  const req::Param* original = (*required)->get("avatar");
  const bool avatar_given = original != nullptr && !original->is_null();
  const char* notice = avatar_given ? "It may take up to 30 minutes to change everywhere." : "\xE2\x9C\x93";

  if (auto staged = co_await active_storage::stage(rq, avatar); !staged) co_return std::unexpected(std::move(staged.error()));
  active_storage::Applied applied;
  const auto record = models::attachments::Record::user(user_id);
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
    applied = {};
    auto updated = models::users::update(tx, user_id, changes);
    if (!updated) return std::unexpected(updated.error());
    return active_storage::apply(tx, record, "avatar", avatar, applied);
  });
  if (!written) co_return fail_internal(written.error().message);
  co_await active_storage::after_write(rq, record, avatar, applied);
  co_return redirect_to_path(rq, campfire::routes::user_profile(), std::string(notice));
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::users_profiles {

Task<net::Response> show(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::profiles_show);
}
Task<net::Response> update(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::profiles_update);
}

}  // namespace campfire::routes::users_profiles
