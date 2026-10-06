// STUB. Temporary implementation of the message partials of the rooms area (A2). It is not the Rails output.
// Rails: app/views/messages/_message.html.erb. Replace this file with the partials of A2 when A2 merges.
#include "app/message_partial.hpp"

#include "views/helpers/tag.hpp"
#include "views/helpers/turbo.hpp"

namespace campfire::views::messages {

void message(Out& out, const ViewContext&, const MessageView& message) {
  out.append_raw("<div id=\"");
  html_escape(out, message.dom_id());
  out.append_raw("\" class=\"message\" data-stub=\"true\">");
  if (const auto* text = std::get_if<TextContent>(&message.content)) {
    out.append(SafeHtml::trusted(text->html));
  } else if (const AttachmentView* attachment = message.attachment()) {
    html_escape(out, attachment->filename);
  }
  out.append_raw("</div>\n");
}

void presentation(Out& out, const ViewContext&, const MessageView& message) {
  out.append_raw("<div id=\"");
  html_escape(out, message.dom_id("presentation"));
  out.append_raw("\" data-stub=\"true\">");
  if (const auto* text = std::get_if<TextContent>(&message.content)) out.append(SafeHtml::trusted(text->html));
  out.append_raw("</div>\n");
}

void boosts(Out& out, const ViewContext&, const MessageView& message) {
  out.append_raw("<turbo-frame id=\"");
  html_escape(out, message.dom_id("boosting"));
  out.append_raw("\" data-stub=\"true\"></turbo-frame>");
}

void attachment_presentation(Out& out, const ViewContext&, const AttachmentView& attachment) {
  out.append_raw("<div data-stub=\"true\">");
  html_escape(out, attachment.filename);
  out.append_raw("</div>");
}

}  // namespace campfire::views::messages
