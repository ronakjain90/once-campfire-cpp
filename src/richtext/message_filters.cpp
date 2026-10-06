// Rails: app/helpers/content_filters/remove_solo_unfurled_link_text.rb, sanitize_tags.rb,
// sanitize_attributes.rb, text_message_presentation_filters.rb. Rust: crates/richtext/src/filters.rs
#include "richtext/message_filters.hpp"

#include <algorithm>
#include <optional>
#include <string>

#include "compat/ruby.hpp"
#include "richtext/filters.hpp"
#include "richtext/sanitizer.hpp"
#include "richtext/text_util.hpp"
#include "richtext/tree.hpp"
#include "richtext/uri.hpp"

namespace campfire::richtext {

namespace {

// `normalize_tweet_url`: x.com and twitter.com URLs lose the query, and x.com becomes twitter.com.
Result<std::optional<std::string>> normalize_tweet_url(const std::optional<std::string>& url) {
  if (!url) {
    return std::optional<std::string>();
  }
  const bool is_twitter_url = !is_blank(*url) && [&] {
    const std::string stripped(compat::strip(*url));
    return stripped.find("x.com") != std::string::npos || stripped.find("twitter.com") != std::string::npos;
  }();
  if (!is_twitter_url) {
    return url;
  }
  auto parsed = parse_uri(*url);
  if (!parsed) {
    if (parsed.error() == UriError::InvalidUri) {
      return url;
    }
    return std::unexpected(Error::raised("URI::InvalidComponentError"));
  }
  if (parsed->host) {
    std::string lowered = *parsed->host;
    for (char& c : lowered) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lowered == "x.com") {
      parsed->host = "twitter.com";
    }
  }
  parsed->query.reset();
  return std::optional<std::string>(parsed->to_s());
}

}  // namespace

Result<Content> remove_solo_unfurled_link_text(Content content, const RenderContext& ctx) {
  std::vector<Node*> unfurled_links;
  for (Node* n : descendants(content.root)) {
    if (is_named(n, kAttachmentTag) && attr_value(n, "content-type") == std::optional<std::string_view>(kOpengraphEmbedContentType)) {
      unfurled_links.push_back(n);
    }
  }
  std::optional<std::string> solo_unfurled_url;
  if (unfurled_links.size() == 1) {
    auto embed = opengraph_embed_from_node(unfurled_links[0], ctx);
    if (!embed) return std::unexpected(embed.error());
    if (embed->has_value()) {
      solo_unfurled_url = (*embed)->href;
    }
  }
  auto plain_text = content.to_plain_text(ctx);
  if (!plain_text) return std::unexpected(plain_text.error());
  auto left = normalize_tweet_url(solo_unfurled_url);
  if (!left) return std::unexpected(left.error());
  auto right = normalize_tweet_url(std::optional<std::string>(*plain_text));
  if (!right) return std::unexpected(right.error());
  if (*left != *right) {
    return content;
  }

  Dom& dom = content.dom;
  const std::vector<Node*> all = descendants(content.root);
  const bool is_trix_body = std::any_of(all.begin(), all.end(), [](Node* n) { return is_named(n, "div"); });
  if (is_trix_body) {
    // Each div gets the unfurl as its only content.
    const std::string unfurl = to_html(unfurled_links[0]);
    for (Node* div : all) {
      if (is_named(div, "div")) {
        if (auto r = set_inner_html(dom, div, unfurl); !r) return fail(r.error());
      }
    }
  } else {
    for (Node* p : all) {
      if (!is_named(p, "p")) {
        continue;
      }
      const std::vector<Node*> inside = descendants(p);
      const bool has_attachment = std::any_of(inside.begin(), inside.end(), [](Node* n) { return is_named(n, kAttachmentTag); });
      if (!has_attachment) {
        dom.detach(p);
      }
    }
  }
  return content;
}

Result<Content> apply_message_filters(Content content, const RenderContext& ctx) {
  auto solo = remove_solo_unfurled_link_text(std::move(content), ctx);
  if (!solo) return solo;
  sanitize_tags(solo->dom);
  // SanitizeAttributes: the safe list sanitizer over the tags of SanitizeTags, then a new fragment.
  auto sanitized = sanitize(solo->to_html(), SafeList::content_filter());
  if (!sanitized) {
    return fail(sanitized.error());
  }
  return Content::wrap(*sanitized);
}

}  // namespace campfire::richtext
