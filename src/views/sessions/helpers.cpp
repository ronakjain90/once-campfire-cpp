// Translation popups and the account logo. Rust: crates/views/src/helpers/translations.rs, users.rs.
#include "views/sessions/helpers.hpp"

#include <stdexcept>
#include <string>

#include "views/helpers/assets.hpp"
#include "views/sessions/translations_table.hpp"

namespace campfire::views::helpers {

void translations_for(Out& out, std::string_view key) {
  for (const TranslationSet& set : kTranslations) {
    if (set.key != key) continue;
    content_tag(out, "dl", attrs().cls("language-list"), [&](Out& body) {
      for (const Translation& entry : set.entries) {
        content_tag_text(body, "dt", attrs(), entry.flag);
        content_tag_text(body, "dd", attrs().cls("margin-none"), entry.text);
      }
    });
    return;
  }
  throw std::invalid_argument("unknown translation key " + std::string(key));
}

void translation_button(Out& out, const ViewContext& ctx, std::string_view key) {
  Attrs details =
      attrs()
          .cls("position-relative")
          .data("controller", "popup")
          .data("action", "keydown.esc->popup#close toggle->popup#toggle click@document->popup#closeOnClickOutside")
          .data("popup_orientation_top_class", "popup-orientation-top");
  content_tag(out, "details", details, [&](Out& body) {
    content_tag(body, "summary", attrs().cls("btn").tabindex(-1), [&](Out& summary) {
      image_tag(summary, ctx, "globe.svg", attrs().size(20).aria_hidden().cls("color-icon"));
      content_tag_text(summary, "span", attrs().cls("for-screen-reader"), "Translate");
    });
    content_tag(body, "div", attrs().cls("language-list-menu shadow").data("popup_target", "menu"),
                [&](Out& menu) { translations_for(menu, key); });
  });
}

void account_logo_tag(Out& out, const ViewContext& ctx, std::optional<std::string_view> style) {
  const std::string klass = "account-logo avatar " + std::string(style.value_or(""));
  content_tag(out, "figure", attrs().cls(klass),
              [&](Out& body) { image_tag(body, ctx, ctx.account.logo_url, attrs().alt("Account logo").size(300)); });
}

}  // namespace campfire::views::helpers
