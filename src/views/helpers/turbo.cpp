// turbo-rails helpers (Rust: crates/views/src/helpers/turbo.rs).
#include "views/helpers/turbo.hpp"

#include "core/time_format.hpp"

namespace campfire::views::helpers {

void turbo_stream_from(Out& out, std::string_view signed_stream_name) {
  builder_tag(out, "turbo-cable-stream-source",
              attrs().attr("channel", "Turbo::StreamsChannel").attr("signed-stream-name", signed_stream_name));
}

void turbo_page_requires_reload_tag(Out& out) {
  builder_tag(out, "meta", attrs().name("turbo-visit-control").attr("content", "reload"));
}

void turbo_stream_remove(Out& out, std::string_view target) {
  out.append_raw("<turbo-stream action=\"remove\" target=\"");
  html_escape(out, target);
  out.append_raw("\"></turbo-stream>");
}

std::string dom_id(std::string_view model, std::string_view id, std::string_view prefix) {
  std::string out;
  if (!prefix.empty()) {
    out.append(prefix).append("_");
  }
  out.append(model).append("_").append(id);
  return out;
}

void local_datetime_tag(Out& out, Timestamp datetime, std::string_view style, Attrs attributes) {
  attributes.set("datetime", Value(format_iso8601(datetime)));
  attributes.attr("data-local-time-target", Value(style));
  builder_tag(out, "time", attributes);
}

}  // namespace campfire::views::helpers
