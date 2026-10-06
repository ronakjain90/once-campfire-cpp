// Block helpers and small helpers of the room templates. Rust: crates/views/src/rooms.rs.
#include "views/rooms/helpers.hpp"

#include <array>

#include "compat/base64.hpp"
#include "routes/query.hpp"
#include "routes/routes.hpp"
#include "views/helpers/assets.hpp"
#include "views/helpers/forms.hpp"
#include "views/helpers/tag.hpp"
#include "views/helpers/turbo.hpp"
#include "views/templates.gen.hpp"

namespace campfire::views::helpers {

void room_form(Out& out, const ViewContext& ctx, const FormRoom& room, bool can_administer, RoomKind kind,
               const std::function<void(Out&)>& body) {
  Out inner;
  body(inner);
  const std::string content = inner.to_string();
  views::rooms::layouts::form(out, ctx, room, can_administer, kind, content);
}

std::string_view humanize_involvement(std::string_view involvement) {
  if (involvement == "mentions") return "Notifying about @ mentions";
  if (involvement == "everything") return "Notifying about all messages";
  if (involvement == "nothing") return "Notifications are off";
  if (involvement == "invisible") return "Notifications are off and room invisible in sidebar";
  return {};
}

std::string_view next_involvement(bool direct, std::string_view involvement) {
  static constexpr std::array<std::string_view, 2> kDirect{"everything", "nothing"};
  static constexpr std::array<std::string_view, 4> kShared{"mentions", "everything", "nothing", "invisible"};
  const auto pick = [&](const auto& order) {
    for (std::size_t i = 0; i < order.size(); ++i) {
      if (order[i] == involvement) return i + 1 < order.size() ? order[i + 1] : order[0];
    }
    return order[0];
  };
  return direct ? pick(kDirect) : pick(kShared);
}

void button_to_change_involvement(Out& out, const ViewContext& ctx, const InvolvementView& view) {
  const bool direct = view.kind == RoomKind::Direct;
  const std::string label_id = dom_id(room_param_key(view.kind), std::to_string(view.room_id), "involvement_label");
  const std::string url = campfire::routes::with_query(
      campfire::routes::room_involvement(view.room_id),
      {{"involvement", campfire::routes::QueryValue(std::string(next_involvement(direct, view.involvement)))}});
  const Attrs options = attrs()
                            .method("put")
                            .role("checkbox")
                            .aria("checked", true)
                            .aria("labelledby", label_id)
                            .tabindex(0)
                            .cls("btn " + view.involvement);
  button_to(out, url, options, [&](Out& o) {
    image_tag(o, ctx, "notification-bell-" + view.involvement + ".svg", attrs().aria_hidden().size(20));
    content_tag_text(o, "span", attrs().cls("for-screen-reader").id(label_id), humanize_involvement(view.involvement));
  });
}

std::string lowercase(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

std::string zoom_qr_path(std::string_view url) {
  return campfire::routes::qr_code(compat::base64::urlsafe_encode_padded(url));
}

Attrs zoom_qr_options(std::string_view url) {
  return attrs()
      .cls("btn")
      .data("lightbox_target", "image")
      .data("action", "lightbox#open")
      .data("lightbox_url_value", zoom_qr_path(url));
}

Attrs copy_to_clipboard_options(std::string_view url) {
  return attrs()
      .cls("btn")
      .data("controller", "copy-to-clipboard")
      .data("action", "copy-to-clipboard#copy")
      .data("copy_to_clipboard_success_class", "btn--success")
      .data("copy_to_clipboard_content_value", url);
}

Attrs web_share_session_options(std::string_view url, std::string_view title, std::string_view text) {
  return attrs()
      .cls("btn")
      .hidden()
      .data("controller", "web-share")
      .data("action", "web-share#share")
      .data("web_share_url_value", url)
      .data("web_share_text_value", text)
      .data("web_share_title_value", title);
}

}  // namespace campfire::views::helpers
