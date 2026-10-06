// Rails: app/helpers/messages_helper.rb (message_presentation), app/helpers/messages/attachment_presentation.rb and the
// paths of the message partials. Rust: crates/views/src/messages/presentation.rs, crates/views/src/messages.rs.
#include <cmath>

#include "compat/ruby.hpp"
#include "core/time_format.hpp"
#include "routes/routes.hpp"
#include "views/fragment_cache.hpp"
#include "views/messages/support.hpp"
#include "views/templates.gen.hpp"

namespace campfire::views::messages {

namespace {

// `Message::THUMBNAIL_MAX_WIDTH` and `THUMBNAIL_MAX_HEIGHT`.
constexpr double kThumbnailMaxWidth = 1200;
constexpr double kThumbnailMaxHeight = 800;

double to_f(const RubyNumber& n) {
  return n.value;
}

std::string ruby_number(const RubyNumber& n) {
  if (n.is_float) return compat::float_to_s(n.value);
  return std::to_string(static_cast<std::int64_t>(n.value));
}

// `number / 2`: an integer divides as an integer.
RubyNumber half(const RubyNumber& n) {
  if (n.is_float) return RubyNumber{true, n.value / 2.0};
  const auto value = static_cast<std::int64_t>(n.value);
  std::int64_t quotient = value / 2;
  if (value % 2 != 0 && value < 0) --quotient;
  return RubyNumber{false, static_cast<double>(quotient)};
}

struct Dimensions {
  RubyNumber width;
  RubyNumber height;
};

// `preview_dimensions`: the size in the metadata, scaled down to fit the thumbnail limits.
std::optional<Dimensions> preview_dimensions(const AttachmentView& attachment) {
  if (!attachment.width || !attachment.height) return std::nullopt;
  const RubyNumber& width = *attachment.width;
  const RubyNumber& height = *attachment.height;
  if (to_f(width) <= kThumbnailMaxWidth && to_f(height) <= kThumbnailMaxHeight) return Dimensions{width, height};
  const double scale = std::fmin(kThumbnailMaxWidth / to_f(width), kThumbnailMaxHeight / to_f(height));
  return Dimensions{RubyNumber{true, to_f(width) * scale}, RubyNumber{true, to_f(height) * scale}};
}

template <class Body>
void inline_media_dimension_constraints(Out& out, const std::optional<Dimensions>& dimensions, Body&& body) {
  if (dimensions) {
    out.append_raw("<div class=\"max-inline-size center flex overflow-clip\" style=\"width: ");
    out.append_raw(ruby_number(half(dimensions->width)));
    out.append_raw("px; aspect-ratio: ");
    out.append_raw(compat::float_to_s(to_f(dimensions->width) / to_f(dimensions->height)));
    out.append_raw(";\">");
  } else {
    out.append_raw("<div class=\"max-inline-size center overflow-clip\">");
  }
  body();
  out.append_raw("</div>");
}

void video_preview(Out& out, const AttachmentView& attachment) {
  inline_media_dimension_constraints(out, preview_dimensions(attachment), [&] {
    out.append_raw("<video src=\"");
    html_escape(out, attachment.blob_path);
    out.append_raw("\" poster=\"");
    html_escape(out, attachment.preview.url);
    out.append_raw("\" controls=\"controls\" preload=\"none\" width=\"100%\" height=\"100%\" "
                   "class=\"message__attachment\"></video>");
  });
}

void lightboxed_image_preview(Out& out, const AttachmentView& attachment) {
  const std::optional<Dimensions> dimensions = preview_dimensions(attachment);
  inline_media_dimension_constraints(out, dimensions, [&] {
    out.append_raw("<a class=\"flex\" data-lightbox-target=\"image\" data-action=\"lightbox#open\" "
                   "data-lightbox-url-value=\"");
    html_escape(out, attachment.download_path);
    out.append_raw("\" href=\"");
    html_escape(out, attachment.blob_path);
    out.append_raw("\"><img");
    if (dimensions) {
      out.append_raw(" width=\"");
      out.append_raw(ruby_number(dimensions->width));
      out.append_raw("\" height=\"");
      out.append_raw(ruby_number(dimensions->height));
      out.append_char('"');
    }
    out.append_raw(" class=\"message__attachment\" loading=\"lazy\" src=\"");
    html_escape(out, attachment.preview.url);
    out.append_raw("\" /></a>");
  });
}

void image_icon(Out& out, const ViewContext& ctx, std::string_view name, std::string_view class_attr, int size) {
  out.append_raw("<img ");
  out.append_raw(class_attr);
  out.append_raw("aria-hidden=\"true\" src=\"");
  html_escape(out, ctx.asset(name));
  out.append_raw("\" width=\"");
  out.append_int(size);
  out.append_raw("\" height=\"");
  out.append_int(size);
  out.append_raw("\" />");
}

// `render_link`: the file icon, the name, the download link and the share button, with no space between.
void file_link(Out& out, const ViewContext& ctx, const AttachmentView& attachment) {
  out.append_raw("<div class=\"flex-inline align-center gap-half\">");
  image_icon(out, ctx, "common-file-text.svg", "class=\"colorize--black\" ", 22);
  out.append_raw("<span>");
  html_escape(out, attachment.filename);
  out.append_raw("</span><a class=\"btn message__action-btn hide-in-ios-pwa\" style=\"--width: auto;\" href=\"");
  html_escape(out, attachment.download_path);
  out.append_raw("\">");
  image_icon(out, ctx, "download.svg", "", 20);
  out.append_raw("<span class=\"for-screen-reader\">Download ");
  html_escape(out, attachment.filename);
  out.append_raw("</span></a><button class=\"btn message__action-btn\" style=\"--width: auto;\" "
                 "data-controller=\"web-share\" data-action=\"web-share#share\" data-web-share-files-value=\"");
  html_escape(out, attachment.download_path);
  out.append_raw("\">");
  image_icon(out, ctx, "share.svg", "", 20);
  out.append_raw("<span class=\"for-screen-reader\">Share ");
  html_escape(out, attachment.filename);
  out.append_raw("</span></button></div>");
}

void sound_presentation(Out& out, const SoundView& sound) {
  out.append_raw("<div class=\"sound\" data-controller=\"sound\" data-action=\"messages:play-&gt;sound#play\" "
                 "data-sound-url-value=\"");
  html_escape(out, sound.url);
  out.append_raw("\"><button class=\"btn btn--plain\" data-action=\"sound#play\">\xF0\x9F\x94\x8A</button>");
  if (sound.image) {
    out.append_raw("<img width=\"");
    out.append_uint(sound.image->width);
    out.append_raw("\" height=\"");
    out.append_uint(sound.image->height);
    out.append_raw("\" class=\"align--middle\" src=\"");
    html_escape(out, sound.image->src);
    out.append_raw("\" />");
  } else if (sound.text) {
    html_escape(out, *sound.text);
  }
  out.append_raw("</div>");
}

std::int64_t epoch_of(const std::string& db_text) {
  const auto t = parse_db(db_text);
  return t ? epoch_ms(*t) : 0;
}

}  // namespace

void attachment_presentation(Out& out, const ViewContext& ctx, const AttachmentView& attachment) {
  switch (attachment.preview.kind) {
    case AttachmentPreview::Kind::Video: video_preview(out, attachment); break;
    case AttachmentPreview::Kind::Image: lightboxed_image_preview(out, attachment); break;
    case AttachmentPreview::Kind::File: file_link(out, ctx, attachment); break;
  }
}

void message_presentation(Out& out, const ViewContext& ctx, const MessageView& message) {
  if (const auto* attachment = std::get_if<AttachmentView>(&message.content)) {
    attachment_presentation(out, ctx, *attachment);
  } else if (const auto* sound = std::get_if<SoundView>(&message.content)) {
    sound_presentation(out, *sound);
  } else if (const auto* text = std::get_if<TextContent>(&message.content)) {
    out.append(SafeHtml::trusted(text->html));
  }
}

void render_message_item(Out& out, const ViewContext& ctx, const MessageItem& item) {
  const std::size_t start = out.size();
  if (item.view) {
    message(out, ctx, *item.view);
  } else {
    out.append(SafeHtml::trusted(item.html));
  }
  if (FragmentRecorder* recorder = fragment_recorder()) recorder->record(start, out.size() - start);
}

std::string message_fragment_key(std::int64_t id, std::string_view updated_at) {
  std::string key = "views/messages/_message/messages/";
  key += std::to_string(id);
  key.push_back('-');
  key += updated_at;
  key += "/presentation-v3";
  return key;
}

std::string boost_fragment_key(std::int64_t id, std::string_view updated_at) {
  std::string key = "views/messages/boosts/_boost/boosts/";
  key += std::to_string(id);
  key.push_back('-');
  key += updated_at;
  return key;
}

std::string created_at_iso(const MessageView& message) {
  const auto t = parse_db(message.created_at);
  return t ? format_iso8601(*t) : std::string();
}

std::int64_t created_at_epoch(const MessageView& message) {
  return epoch_of(message.created_at);
}

std::int64_t updated_at_epoch(const MessageView& message) {
  return epoch_of(message.updated_at);
}

std::string at_path(const MessageView& message) {
  return campfire::routes::room_at_message(message.room_id, message.id);
}
std::string edit_path(const MessageView& message) {
  return campfire::routes::edit_room_message(message.room_id, message.id);
}
std::string boosts_path(const MessageView& message) {
  return campfire::routes::message_boosts(message.id);
}
std::string new_boost_path(const MessageView& message) {
  return campfire::routes::new_message_boost(message.id);
}
std::string boost_path(const BoostView& boost) {
  return campfire::routes::message_boost(boost.message_id, boost.id);
}

}  // namespace campfire::views::messages
