// turbo-rails helpers: `turbo_frame_tag`, `turbo_stream_from`, Turbo Stream tags, `dom_id`, and
// the `time` tag of `local_datetime_tag` (reference/app/helpers/time_helper.rb).
// Rust: crates/views/src/helpers/turbo.rs.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/timestamp.hpp"
#include "views/helpers/tag.hpp"

namespace campfire::views::helpers {

// `turbo_frame_tag(id, src:, target:, **attributes) do ... end`: the given attributes first, then
// `id`, `src` and `target` (a nil one is left out). `src` and `target` may be in `attributes`.
template <BodyFn Body>
void turbo_frame_tag(Out& out, std::string_view id, Attrs attributes, Body&& body) {
  std::optional<Value> src = attributes.remove("src");
  std::optional<Value> target = attributes.remove("target");
  attributes.set("id", Value(id));
  attributes.set("src", std::move(src));
  attributes.set("target", std::move(target));
  content_tag(out, "turbo-frame", attributes, std::forward<Body>(body));
}

// `turbo_stream_from`: the signed stream name comes from the caller
// (`Turbo::StreamsChannel.signed_stream_name`).
void turbo_stream_from(Out& out, std::string_view signed_stream_name);

// `turbo_page_requires_reload_tag`.
void turbo_page_requires_reload_tag(Out& out);

// `turbo_stream.<action>(target) { content }`:
// `<turbo-stream action="append" target="x"><template>...</template></turbo-stream>`.
template <BodyFn Body>
void turbo_stream(Out& out, std::string_view action, std::string_view target, Body&& body) {
  out.append_raw("<turbo-stream action=\"");
  html_escape(out, action);
  out.append_raw("\" target=\"");
  html_escape(out, target);
  out.append_raw("\"><template>");
  body(out);
  out.append_raw("</template></turbo-stream>");
}

// `turbo_stream.remove(target)`: no template.
void turbo_stream_remove(Out& out, std::string_view target);

// `dom_id(record, prefix)`: "prefix_model_id", or "model_id".
[[nodiscard]] std::string dom_id(std::string_view model, std::string_view id, std::string_view prefix = {});

// `local_datetime_tag(datetime, style:, **attributes)`: `tag.time **attributes, datetime:, data:`.
void local_datetime_tag(Out& out, Timestamp datetime, std::string_view style, Attrs attributes = {});

}  // namespace campfire::views::helpers
