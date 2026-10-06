// Rails: actiontext attachment.rb, lexxy attachables, reference/lib/rails_ext/action_text_attachables.rb,
// reference/lib/rails_ext/actiontext_opengraph_embeds.rb. Rust: crates/richtext/src/attachables.rs
#include "richtext/attachables.hpp"

#include <algorithm>
#include <cctype>

#include "compat/base64.hpp"
#include "compat/json.hpp"
#include "compat/ruby.hpp"
#include "richtext/filters.hpp"
#include "richtext/text_util.hpp"
#include "richtext/tree.hpp"
#include "richtext/uri.hpp"

namespace campfire::richtext {

namespace {

constexpr std::string_view kTwitterAvatarUrlPrefix = "https://pbs.twimg.com/profile_images";

std::string lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

// `/application/vnd.actiontext.opengraph-embed/` where each "." matches one character but "\n".
bool matches_opengraph_type(std::string_view text) {
  constexpr std::string_view pattern = kOpengraphEmbedContentType;
  for (std::size_t start = 0; start < text.size(); ++start) {
    std::size_t pos = start;
    bool ok = true;
    for (char p : pattern) {
      if (pos >= text.size()) {
        ok = false;
        break;
      }
      if (p == '.') {
        if (text[pos] == '\n') {
          ok = false;
          break;
        }
        (void)next_code_point(text, pos);
      } else if (text[pos] == p) {
        ++pos;
      } else {
        ok = false;
        break;
      }
    }
    if (ok) {
      return true;
    }
  }
  return false;
}

// `/^<kind>(\/.+|$)/m` over the lines of a content type.
bool matches_media_type(std::string_view text, std::string_view kind) {
  for (std::size_t line = 0; line <= text.size();) {
    std::string_view rest = text.substr(line);
    if (rest.starts_with(kind)) {
      rest.remove_prefix(kind.size());
      if (rest.empty() || rest.front() == '\n') {
        return true;
      }
      if (rest.front() == '/' && rest.size() > 1 && rest[1] != '\n') {
        return true;
      }
    }
    const std::size_t newline = text.find('\n', line);
    if (newline == std::string_view::npos) {
      break;
    }
    line = newline + 1;
  }
  return false;
}

std::optional<std::string> to_owned(std::optional<std::string_view> v) {
  return v ? std::optional<std::string>(std::string(*v)) : std::nullopt;
}

std::optional<std::string_view> presence(std::optional<std::string_view> v) {
  return v && !is_blank(*v) ? v : std::nullopt;
}

// `Base64.strict_decode64(message) rescue Base64.urlsafe_decode64(message)`
std::optional<std::string> decode_base64(std::string_view message) {
  if (auto strict = compat::base64::strict_decode(message)) {
    return strict;
  }
  return compat::base64::urlsafe_decode(message);
}

// The error that `attachable_from_possibly_expired_sgid` raised. A JSON parse error quotes the
// invalid bytes of the decoded SGID, and the log line of the rescue raises in turn.
Error sgid_error(compat::global_id::UnverifiedSgidError error, std::string_view sgid) {
  using compat::global_id::UnverifiedSgidError;
  switch (error) {
    case UnverifiedSgidError::JsonParserError: {
      // sgid.split("--").first, as the compat code takes it.
      std::string_view first = sgid.substr(0, sgid.find("--"));
      auto decoded = decode_base64(first);
      if (decoded && !compat::json::valid_utf8(*decoded)) {
        return Error::unrenderable("JSON::ParserError");
      }
      return Error::raised("JSON::ParserError");
    }
    case UnverifiedSgidError::ArgumentError: return Error::raised("ArgumentError: invalid base64");
    case UnverifiedSgidError::TypeError: return Error::raised("TypeError: dig");
    case UnverifiedSgidError::NoMethodError: return Error::raised("NoMethodError");
  }
  return Error::raised("unknown");
}

// `attachable_from_possibly_expired_sgid`: reads the GlobalID out of an SGID without a signature
// check. It only ever gives a User.
Result<std::optional<MentionUser>> attachable_from_possibly_expired_sgid(std::optional<std::string_view> sgid,
                                                                         const RenderContext& ctx) {
  auto gid = compat::global_id::gid_from_unverified_sgid(sgid);
  if (!gid) {
    return std::unexpected(sgid_error(gid.error(), sgid.value_or("")));
  }
  if (!gid->has_value()) {
    return std::optional<MentionUser>();
  }
  GidLookup found = ctx.resolver.find_gid(**gid);
  switch (found.kind) {
    case GidLookup::Kind::User: return std::optional<MentionUser>(std::move(found.user));
    case GidLookup::Kind::OtherModel:
    case GidLookup::Kind::NotFound: return std::optional<MentionUser>();
    case GidLookup::Kind::Raises: return std::unexpected(Error::raised("GlobalID.find"));
  }
  return std::optional<MentionUser>();
}

bool has_class(const Node* node, std::string_view cls) {
  auto value = attr_value(node, "class");
  if (!value) {
    return false;
  }
  std::size_t pos = 0;
  while (pos <= value->size()) {
    std::size_t end = value->find_first_of(" \t\n\r", pos);
    if (end == std::string_view::npos) {
      end = value->size();
    }
    if (value->substr(pos, end - pos) == cls) {
      return true;
    }
    pos = end + 1;
  }
  return false;
}

bool named_host(std::string_view host, bool& raised) {
  raised = false;
  if (is_blank(host) || host.find('%') != std::string_view::npos || host.find('.') == std::string_view::npos) {
    return false;
  }
  // `host.split(".").last`: Ruby drops trailing empty labels, and nil.match? raises.
  while (host.ends_with('.')) {
    host.remove_suffix(1);
  }
  if (host.empty()) {
    raised = true;
    return false;
  }
  const std::string_view label = host.substr(host.rfind('.') == std::string_view::npos ? 0 : host.rfind('.') + 1);
  const bool has_alpha = std::any_of(label.begin(), label.end(), [](char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 && static_cast<unsigned char>(c) < 0x80;
  });
  return has_alpha && !lower_ascii(label).starts_with("0x");
}

std::string canonical_host(std::string_view host) {
  std::string lower = lower_ascii(host);
  if (lower.ends_with('.')) {
    lower.pop_back();
  }
  return lower;
}

Result<bool> elsewhere(const std::optional<std::string>& host, std::string_view request_host) {
  if (!host) {
    return false;
  }
  bool raised = false;
  const bool named = named_host(*host, raised);
  if (raised) {
    return std::unexpected(Error::raised("NoMethodError: match?"));
  }
  if (!named) {
    return false;
  }
  return canonical_host(*host) != canonical_host(request_host);
}

// `attributes_from_content`: the details that Lexxy writes as the markup of the content of an embed.
Result<OpengraphEmbed> embed_from_content(std::string_view content, std::string_view host) {
  Dom dom;
  auto fragment = parse_fragment_node(dom, content);
  if (!fragment) {
    return fail(fragment.error());
  }
  const std::vector<Node*> all = descendants(*fragment);
  const auto with_class = [&](std::string_view cls) -> Node* {
    for (Node* n : all) {
      if (n->is_element() && has_class(n, cls)) {
        return n;
      }
    }
    return nullptr;
  };
  Node* title = with_class("og-embed__title");
  Node* link = nullptr;
  if (title != nullptr) {
    for (Node* n : descendants(title)) {
      if (is_named(n, "a")) {
        link = n;
        break;
      }
    }
  }
  Node* image = nullptr;
  for (Node* n : all) {
    if (is_named(n, "img")) {
      const auto up = ancestors(n);
      if (std::any_of(up.begin(), up.end(),
                      [](Node* a) { return a->is_element() && has_class(a, "og-embed__image"); })) {
        image = n;
        break;
      }
    }
  }
  Node* description = with_class("og-embed__description");
  OpengraphEmbed embed;
  auto href = web_url(link != nullptr ? attr_value(link, "href") : std::nullopt, host);
  if (!href) return std::unexpected(href.error());
  auto url = web_url(image != nullptr ? attr_value(image, "src") : std::nullopt, host);
  if (!url) return std::unexpected(url.error());
  embed.href = std::move(*href);
  embed.url = std::move(*url);
  if (Node* named = link != nullptr ? link : title) {
    embed.filename = std::string(compat::strip(text_content(named)));
  }
  if (description != nullptr) {
    embed.description = std::string(compat::strip(text_content(description)));
  }
  return embed;
}

}  // namespace

bool OpengraphEmbed::twitter_avatar() const {
  return url.value_or("").starts_with(kTwitterAvatarUrlPrefix);
}

Result<std::optional<std::string>> web_url(std::optional<std::string_view> value, std::string_view request_host) {
  if (!value || is_blank(*value)) {
    return std::optional<std::string>();
  }
  auto parsed = parse_uri(*value);
  if (!parsed) {
    if (parsed.error() == UriError::InvalidUri) {
      return std::optional<std::string>();
    }
    return std::unexpected(Error::raised("URI::InvalidComponentError"));
  }
  if (parsed->is_http()) {
    auto other = elsewhere(parsed->host, request_host);
    if (!other) return std::unexpected(other.error());
    if (*other) {
      return std::optional<std::string>(std::string(*value));
    }
  }
  return std::optional<std::string>();
}

Result<std::optional<OpengraphEmbed>> opengraph_embed_from_node(const Node* node, const RenderContext& ctx) {
  auto content_type = attr_value(node, "content-type");
  if (!content_type || !matches_opengraph_type(*content_type)) {
    return std::optional<OpengraphEmbed>();
  }
  const std::string_view host = ctx.request_host;
  if (presence(attr_value(node, "filename"))) {
    OpengraphEmbed embed;
    auto href = web_url(attr_value(node, "href"), host);
    if (!href) return std::unexpected(href.error());
    auto url = web_url(attr_value(node, "url"), host);
    if (!url) return std::unexpected(url.error());
    embed.href = std::move(*href);
    embed.url = std::move(*url);
    embed.filename = to_owned(attr_value(node, "filename"));
    embed.description = to_owned(attr_value(node, "caption"));
    return std::optional<OpengraphEmbed>(std::move(embed));
  }
  auto embed = embed_from_content(attr_value(node, "content").value_or(""), host);
  if (!embed) return std::unexpected(embed.error());
  return std::optional<OpengraphEmbed>(std::move(*embed));
}

Attachable action_text_attachable_from_node(const Node* node, const RenderContext& ctx) {
  Attachable out;
  SignedLookup signed_lookup;
  if (auto sgid = attr_value(node, "sgid")) {
    signed_lookup = ctx.resolver.locate_signed(*sgid);
  }
  if (signed_lookup.kind == SignedLookup::Kind::User) {
    out.kind = Attachable::Kind::User;
    out.user = std::move(signed_lookup.user);
    return out;
  }
  const auto content_type = attr_value(node, "content-type");
  if (auto content = attr_value(node, "content");
      content && content_type && content_type->find("html") != std::string_view::npos && !is_blank(*content)) {
    out.kind = Attachable::Kind::Content;
    out.content = std::string(*content);
    return out;
  }
  if (auto url = attr_value(node, "url")) {
    if (matches_media_type(content_type.value_or(""), "image")) {
      out.kind = Attachable::Kind::RemoteImage;
      out.url = std::string(*url);
      out.width = to_owned(attr_value(node, "width"));
      out.height = to_owned(attr_value(node, "height"));
      return out;
    }
    if (matches_media_type(content_type.value_or(""), "video")) {
      out.kind = Attachable::Kind::RemoteVideo;
      out.url = std::string(*url);
      out.content_type = std::string(content_type.value_or(""));
      out.width = to_owned(attr_value(node, "width"));
      out.height = to_owned(attr_value(node, "height"));
      out.filename = to_owned(attr_value(node, "filename"));
      return out;
    }
  }
  out.kind = Attachable::Kind::Missing;
  return out;
}

Result<Attachment> attachment_from_node(const Node* node, const RenderContext& ctx) {
  Attachment attachment;
  auto embed = opengraph_embed_from_node(node, ctx);
  if (!embed) return std::unexpected(embed.error());
  if (embed->has_value()) {
    attachment.attachable.kind = Attachable::Kind::OpengraphEmbed;
    attachment.attachable.embed = std::move(**embed);
  } else {
    auto user = attachable_from_possibly_expired_sgid(attr_value(node, "sgid"), ctx);
    if (!user) return std::unexpected(user.error());
    if (user->has_value()) {
      attachment.attachable.kind = Attachable::Kind::User;
      attachment.attachable.user = std::move(**user);
    } else {
      attachment.attachable = action_text_attachable_from_node(node, ctx);
    }
  }
  attachment.caption = to_owned(presence(attr_value(node, "caption")));
  return attachment;
}

Result<std::string_view> attachable_content_type(const Attachable& attachable) {
  switch (attachable.kind) {
    case Attachable::Kind::User: return kMentionContentType;
    case Attachable::Kind::OpengraphEmbed: return kOpengraphEmbedContentType;
    default: return std::unexpected(Error::raised("NoMethodError: attachable_content_type"));
  }
}

// --- Partials ------------------------------------------------------------------------------------

namespace {

void append_caption(std::string& html, const Attachment& attachment) {
  if (attachment.caption) {
    html += "    <figcaption class=\"attachment__caption\">\n      " + compat::html_escape(*attachment.caption) +
            "\n    </figcaption>\n";
  }
}

// ASSET_URI_RE `(?mi)^[-a-z]+://|^(?:cid|data):|^//`, tried at the start of each line.
bool is_asset_uri(std::string_view url) {
  for (std::size_t line = 0; line <= url.size();) {
    const std::string_view rest = url.substr(line);
    std::size_t n = 0;
    while (n < rest.size() && (rest[n] == '-' || (std::isalpha(static_cast<unsigned char>(rest[n])) != 0 &&
                                                  static_cast<unsigned char>(rest[n]) < 0x80))) {
      ++n;
    }
    if (n > 0 && rest.substr(n).starts_with("://")) {
      return true;
    }
    const std::string head = lower_ascii(rest.substr(0, 5));
    if (head.starts_with("cid:") || head.starts_with("data:") || rest.starts_with("//")) {
      return true;
    }
    const std::size_t newline = url.find('\n', line);
    if (newline == std::string_view::npos) {
      break;
    }
    line = newline + 1;
  }
  return false;
}

// `image_tag(url, width:, height:)` for a remote image. A source that is not a URL goes through the
// asset pipeline, which raises for what it does not know. A rooted path passes.
Result<std::string> image_tag(std::string_view url, const std::optional<std::string>& width,
                              const std::optional<std::string>& height) {
  std::string src;
  if (is_blank(url)) {
    src.clear();
  } else if (is_asset_uri(url) || url.starts_with('/')) {
    src = std::string(url);
  } else {
    return std::unexpected(Error::raised("Propshaft::MissingAssetError"));
  }
  std::string html = "<img";
  if (width) html += " width=\"" + compat::html_escape(*width) + "\"";
  if (height) html += " height=\"" + compat::html_escape(*height) + "\"";
  html += " src=\"" + compat::html_escape(src) + "\" />";
  return html;
}

}  // namespace

std::string render_mention(const MentionUser& user) {
  using compat::html_escape;
  return "<span class=\"mention\" sgid=\"" + html_escape(user.attachable_sgid) + "\"><a title=\"" +
         html_escape(user.title) + "\" class=\"btn avatar\" data-turbo-frame=\"_top\" href=\"" +
         html_escape(user.user_path) + "\"><img aria-hidden=\"true\" src=\"" + html_escape(user.avatar_path) +
         "\" width=\"48\" height=\"48\" /></a> " + html_escape(user.name) + "</span>\n";
}

std::string render_opengraph_embed(const OpengraphEmbed& embed) {
  using compat::html_escape;
  std::string title;
  if (embed.href) {
    const std::string text =
        embed.filename ? html_escape(truncate(*embed.filename, 280, "…")) : html_escape(*embed.href);
    title = "<a rel=\"noreferrer\" target=\"_blank\" href=\"" + html_escape(*embed.href) + "\">" + text + "</a>";
  } else if (embed.filename) {
    title = html_escape(truncate(*embed.filename, 280, "…"));
  }
  std::string html =
      "<figure class=\"attachment attachment--content attachment--og\">\n  <actiontext-opengraph-embed>\n    "
      "<div class=\"og-embed gap ";
  html += embed.twitter_avatar() ? "og-embed--twitter-avatar" : "";
  html += "\">\n      <div class=\"og-embed__content\">\n        <div class=\"og-embed__title\">\n          ";
  html += title;
  html += "\n        </div>\n        <div class=\"og-embed__description\">";
  html += html_escape(truncate(embed.description.value_or(""), 560, "…"));
  html += "</div>\n      </div>\n";
  if (embed.url) {
    html += "        <div class=\"og-embed__image\">\n          <img src=\"" + html_escape(*embed.url) +
            "\" class=\"image center\" alt=\"\">\n        </div>\n";
  }
  html += "    </div>\n  </actiontext-opengraph-embed>\n</figure>\n";
  return html;
}

Result<std::string> render_attachment(const Attachment& attachment, const RenderContentFn& render_content) {
  using compat::html_escape;
  const Attachable& a = attachment.attachable;
  std::string html;
  switch (a.kind) {
    case Attachable::Kind::User: html = render_mention(a.user); break;
    case Attachable::Kind::OpengraphEmbed: html = render_opengraph_embed(a.embed); break;
    // Rails asks the model of the SGID for its missing partial, which only models with the
    // ActionText::Attachable concern have. User has none, so Rails raised and blanked the message
    // for a mention of a deleted user. Campfire in Rust renders the ☒ of Action Text for each
    // missing attachable (README "Known differences"). Copy that.
    case Attachable::Kind::Missing: html = "☒"; break;
    case Attachable::Kind::Content: {
      auto content = render_content(a.content);
      if (!content) return std::unexpected(content.error());
      html = "<figure class=\"attachment attachment--content\">\n  " + *content + "\n</figure>\n";
      break;
    }
    case Attachable::Kind::RemoteImage: {
      auto tag = image_tag(a.url, a.width, a.height);
      if (!tag) return std::unexpected(tag.error());
      html = "<figure class=\"attachment attachment--preview\">\n  " + *tag + "\n";
      append_caption(html, attachment);
      html += "</figure>\n";
      break;
    }
    case Attachable::Kind::RemoteVideo: {
      html = "<figure class=\"attachment attachment--preview attachment--video\">\n  <video controls=\"controls\"";
      if (a.width) html += " width=\"" + html_escape(*a.width) + "\"";
      if (a.height) html += " height=\"" + html_escape(*a.height) + "\"";
      html +=
          ">\n    <source src=\"" + html_escape(a.url) + "\" type=\"" + html_escape(a.content_type) + "\">\n</video>";
      append_caption(html, attachment);
      html += "</figure>\n";
      break;
    }
  }
  return std::string(chomp(html));
}

PlainTextRepresentation attachment_plain_text(const Attachment& attachment) {
  const Attachable& a = attachment.attachable;
  const std::optional<std::string>& caption = attachment.caption;
  switch (a.kind) {
    case Attachable::Kind::User: return {false, "@" + a.user.name};
    case Attachable::Kind::OpengraphEmbed: return {false, ""};
    case Attachable::Kind::Content: return {true, a.content};
    case Attachable::Kind::RemoteImage: return {false, "[" + caption.value_or("Image") + "]"};
    case Attachable::Kind::RemoteVideo: return {false, "[" + (caption ? *caption : a.filename.value_or("Video")) + "]"};
    case Attachable::Kind::Missing: return {false, caption.value_or("")};
  }
  return {};
}

}  // namespace campfire::richtext
