// Rails: rails_autolink 1.1.8 lib/rails_autolink/helpers.rb. Rust: crates/richtext/src/autolink.rs
#include "richtext/autolink.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <utility>
#include <vector>

#include "compat/ruby.hpp"
#include "richtext/error.hpp"
#include "richtext/text_util.hpp"

namespace campfire::richtext {

namespace {

struct Range {
  char32_t first;
  char32_t last;
};

constexpr Range kWordRanges[] = {
#include "richtext/word_ranges.inc"
};

constexpr std::array<std::string_view, 24> kSchemes = {
    "ed2k",   "ftp",  "http", "https", "irc", "mailto", "news", "gopher", "nntp", "telnet", "webcal", "xmpp",
    "callto", "feed", "svn",  "urn",   "aim", "rsync",  "tag",  "ssh",    "sftp", "rtsp",   "afs",    "file"};

char lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool starts_with_nocase(std::string_view text, std::size_t pos, std::string_view prefix) {
  if (pos + prefix.size() > text.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (lower(text[pos + i]) != prefix[i]) {
      return false;
    }
  }
  return true;
}

bool is_ascii_alnum(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Ruby's `\s` and `\w` are ASCII only.
bool is_url_stop(std::string_view text, std::size_t pos) {
  const char c = text[pos];
  if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f' || c == '<' || c == '"') {
    return true;
  }
  return static_cast<unsigned char>(c) == 0xC2 && pos + 1 < text.size() &&
         static_cast<unsigned char>(text[pos + 1]) == 0xA0;
}

struct UrlMatch {
  std::size_t start = 0;
  std::size_t end = 0;
  bool has_scheme = false;
};

// AUTO_LINK_RE: `(?:((?:ed2k|...|file):)// | www\.\w) [^\s<\u00A0"]+`, case-insensitive.
std::optional<UrlMatch> find_url(std::string_view text, std::size_t from) {
  for (std::size_t i = from; i < text.size(); ++i) {
    const char c = lower(text[i]);
    std::size_t prefix_end = 0;
    bool has_scheme = false;
    if (c == 'w') {
      if (starts_with_nocase(text, i, "www.") && i + 4 < text.size() &&
          (is_ascii_alnum(text[i + 4]) || text[i + 4] == '_')) {
        prefix_end = i + 5;
      }
    }
    if (prefix_end == 0) {
      for (std::string_view scheme : kSchemes) {
        if (scheme[0] == c && starts_with_nocase(text, i, scheme) &&
            text.substr(i + scheme.size()).starts_with("://")) {
          prefix_end = i + scheme.size() + 3;
          has_scheme = true;
          break;
        }
      }
    }
    if (prefix_end == 0) {
      continue;
    }
    std::size_t end = prefix_end;
    while (end < text.size() && !is_url_stop(text, end)) {
      ++end;
    }
    if (end > prefix_end) {
      return UrlMatch{i, end, has_scheme};
    }
  }
  return std::nullopt;
}

// `\p{Word}` of the last code point of `text`.
std::size_t last_code_point_start(std::string_view text) {
  std::size_t i = text.size() - 1;
  while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0u) == 0x80u) {
    --i;
  }
  return i;
}

// What `auto_linked?(left, right)` asks about the text around a match: `left` is all text before
// it and `right` all text after it. rails_autolink runs its regular expressions over all of `left`
// for each match, which is quadratic (cubic for `rindex`). This indexes the text once, so each
// question is a binary search. The answers are the same.
class TagIndex {
 public:
  explicit TagIndex(std::string_view text) {
    std::optional<std::size_t> unclosed_lt;  // The first `<` since the last `>`
    for (std::size_t i = 0; i < text.size(); ++i) {
      switch (text[i]) {
        case '<':
          lts_.push_back(i);
          if (!unclosed_lt) unclosed_lt = i;
          if (starts_with_nocase(text, i + 1, "/a>")) {
            close_anchors_.push_back(i);
          }
          if (open_anchors_.empty() || open_anchors_.back().second <= i) {
            if (auto end = open_anchor_end(text, i)) {
              open_anchors_.emplace_back(i, *end);
            }
          }
          break;
        case '>':
          gts_.push_back(i);
          unclosed_lt.reset();
          break;
        case '\n':
          if (!first_dangling_newline_ && unclosed_lt && *unclosed_lt + 2 <= i) {
            first_dangling_newline_ = i;
          }
          break;
        default: break;
      }
    }
  }

  // `auto_linked?(text[..start], text[end..])`: in a tag, or in an `<a>` that is not closed.
  [[nodiscard]] bool auto_linked(std::size_t start, std::size_t end) const {
    return (open_tag_at_line_end(start) && closes_tag(end)) || inside_anchor(start);
  }

 private:
  // AUTO_LINK_CRE[2] `/<a\b.*?>/i`, anchored: it is tried only at a `<`.
  static std::optional<std::size_t> open_anchor_end(std::string_view text, std::size_t i) {
    if (!starts_with_nocase(text, i, "<a")) {
      return std::nullopt;
    }
    std::size_t pos = i + 2;
    // `\b` after the "a": the next character must not be a word character.
    if (pos < text.size()) {
      std::size_t next = pos;
      if (is_word_char(next_code_point(text, next))) {
        return std::nullopt;
      }
    }
    // `.*?>`: the first `>` with no newline before it.
    for (; pos < text.size(); ++pos) {
      if (text[pos] == '>') {
        return pos + 1;
      }
      if (text[pos] == '\n') {
        return std::nullopt;
      }
    }
    return std::nullopt;
  }

  // `left =~ /<[^>]+$/`
  [[nodiscard]] bool open_tag_at_line_end(std::size_t start) const {
    if (first_dangling_newline_ && *first_dangling_newline_ < start) {
      return true;
    }
    // At the end of `left`: a `<` after the last `>`, with at least one character after it.
    const auto gt_it = std::lower_bound(gts_.begin(), gts_.end(), start);
    const std::optional<std::size_t> last_gt =
        gt_it == gts_.begin() ? std::nullopt : std::optional<std::size_t>(*(gt_it - 1));
    const auto lt_it = last_gt ? std::upper_bound(lts_.begin(), lts_.end(), *last_gt) : lts_.begin();
    return lt_it != lts_.end() && *lt_it + 2 <= start;
  }

  // `right =~ /^[^>]*>/`, which matches when `right` has any `>`.
  [[nodiscard]] bool closes_tag(std::size_t end) const { return !gts_.empty() && gts_.back() >= end; }

  // `(i = left.rindex(/<a\b.*?>/i)) && left[i..] !~ /<\/a>/i`
  [[nodiscard]] bool inside_anchor(std::size_t start) const {
    const auto it = std::partition_point(open_anchors_.begin(), open_anchors_.end(),
                                         [&](const auto& a) { return a.second <= start; });
    if (it == open_anchors_.begin()) {
      return false;
    }
    const std::size_t anchor_end = (it - 1)->second;
    const auto close = std::lower_bound(close_anchors_.begin(), close_anchors_.end(), anchor_end);
    return !(close != close_anchors_.end() && *close + 4 <= start);
  }

  std::vector<std::size_t> lts_;
  std::vector<std::size_t> gts_;
  std::optional<std::size_t> first_dangling_newline_;
  std::vector<std::pair<std::size_t, std::size_t>> open_anchors_;
  std::vector<std::size_t> close_anchors_;
};

// How many of each bracket a URL has. It stays current while trailing punctuation is stripped.
class BracketCounts {
 public:
  explicit BracketCounts(std::string_view s) {
    for (char c : s) adjust(c, +1);
  }
  [[nodiscard]] long count(char bracket) const {
    const int i = index(bracket);
    return i < 0 ? 0 : counts_[static_cast<std::size_t>(i)];
  }
  void remove(char c) { adjust(c, -1); }

 private:
  static int index(char c) {
    constexpr std::string_view kBrackets = "[](){}";
    const auto pos = kBrackets.find(c);
    return pos == std::string_view::npos ? -1 : static_cast<int>(pos);
  }
  void adjust(char c, long delta) {
    if (const int i = index(c); i >= 0) counts_[static_cast<std::size_t>(i)] += delta;
  }
  std::array<long, 6> counts_{};
};

char opening_bracket(char closing) {
  switch (closing) {
    case ']': return '[';
    case ')': return '(';
    case '}': return '{';
    default: return '\0';
  }
}

std::string trim_last_code_point(std::string& s) {
  const std::size_t start = last_code_point_start(s);
  std::string removed = s.substr(start);
  s.resize(start);
  return removed;
}

char32_t decode_last(const std::string& s) {
  std::size_t pos = last_code_point_start(s);
  return next_code_point(s, pos);
}

Result<std::string> sanitize_text(std::string_view text) {
  auto out = sanitize(text, SafeList::defaults());
  if (!out) {
    return fail(out.error());
  }
  return std::move(*out);
}

std::string replace_quotes(std::string_view s) {
  std::string out;
  for (char c : s) {
    if (c == '"') {
      out += "&quot;";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

Result<std::string> auto_link_urls(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t last = 0;
  const TagIndex tags(text);
  std::size_t from = 0;
  while (auto m = find_url(text, from)) {
    out.append(text.substr(last, m->start - last));
    last = m->end;
    from = m->end;
    std::string href(text.substr(m->start, m->end - m->start));
    if (tags.auto_linked(m->start, m->end)) {
      out += href;
      continue;
    }
    // The punctuation at the end of the URL is not part of it.
    std::vector<std::string> punctuation;
    BracketCounts brackets(href);
    while (!href.empty()) {
      const char32_t cp = decode_last(href);
      if (is_word_char(cp) || cp == '/' || cp == '-' || cp == '=' || cp == ';') {
        break;
      }
      std::string removed = trim_last_code_point(href);
      punctuation.push_back(removed);
      if (removed.size() == 1) {
        brackets.remove(removed[0]);
        const char opening = opening_bracket(removed[0]);
        if (opening != '\0' && brackets.count(opening) > brackets.count(removed[0])) {
          href += punctuation.back();
          punctuation.pop_back();
          break;
        }
      }
    }
    std::string trailing_gt;
    if (href.ends_with("&gt;")) {
      href.resize(href.size() - 4);
      trailing_gt = "&gt;";
    }
    std::string link_text = href;
    if (!m->has_scheme) {
      href.insert(0, "http://");
    }
    auto safe_text = sanitize_text(link_text);
    if (!safe_text) return std::unexpected(safe_text.error());
    auto safe_href = sanitize_text(href);
    if (!safe_href) return std::unexpected(safe_href.error());
    // content_tag(:a, link_text, attrs, false): only double quotes in attributes are escaped.
    out += "<a target=\"_blank\" href=\"" + replace_quotes(*safe_href) + "\">" + *safe_text + "</a>";
    // SafeBuffer#+ escapes the punctuation, which is not marked safe.
    std::string trailing;
    for (auto it = punctuation.rbegin(); it != punctuation.rend(); ++it) {
      trailing += *it;
    }
    out += compat::html_escape(trailing);
    out += trailing_gt;
  }
  out.append(text.substr(last));
  return out;
}

bool is_email_local_char(char c) {
  return is_ascii_alnum(c) || std::string_view("_.!#$%&'*/=?^`{|}~+-").find(c) != std::string_view::npos;
}

bool is_email_first_char(char c) {
  return is_ascii_alnum(c) || std::string_view("_.!#$%+-").find(c) != std::string_view::npos;
}

bool is_label_char(char c) {
  return is_ascii_alnum(c) || c == '_' || c == '-';
}

// AUTO_EMAIL_RE without its lookbehind, matched at `pos`: the end of the address.
std::optional<std::size_t> match_email(std::string_view text, std::size_t pos) {
  if (pos >= text.size() || !is_email_first_char(text[pos])) {
    return std::nullopt;
  }
  std::size_t i = pos + 1;
  while (i < text.size() && is_email_local_char(text[i])) {
    ++i;
  }
  if (i >= text.size() || text[i] != '@') {
    return std::nullopt;
  }
  ++i;
  const auto label = [&](std::size_t at) {
    std::size_t e = at;
    while (e < text.size() && is_label_char(text[e])) ++e;
    return e;
  };
  std::size_t end = label(i);
  if (end == i) {
    return std::nullopt;
  }
  std::size_t groups = 0;
  while (end < text.size() && text[end] == '.') {
    const std::size_t next = label(end + 1);
    if (next == end + 1) {
      break;
    }
    end = next;
    ++groups;
  }
  if (groups == 0) {
    return std::nullopt;
  }
  return end;
}

Result<std::string> auto_link_email_addresses(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t copied = 0;
  std::size_t position = 0;
  const TagIndex tags(text);
  while (position < text.size()) {
    const bool after_local = position > 0 && is_email_local_char(text[position - 1]);
    std::optional<std::size_t> found;
    if (!after_local) {
      found = match_email(text, position);
    }
    if (!found) {
      ++position;
      continue;
    }
    const std::size_t start = position;
    const std::size_t end = *found;
    const std::string email(text.substr(start, end - start));
    out.append(text.substr(copied, start - copied));
    if (tags.auto_linked(start, end)) {
      out += email;
    } else {
      auto sanitized = sanitize_text(email);
      if (!sanitized) return std::unexpected(sanitized.error());
      // display_text is sanitized (and so marked safe) only when sanitizing changed the address.
      const std::string display = *sanitized == email ? compat::html_escape(email) : *sanitized;
      std::string encoded = compat::url_encode(*sanitized);
      std::string href = "mailto:";
      for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (encoded.compare(i, 3, "%40") == 0) {
          href.push_back('@');
          i += 2;
        } else {
          href.push_back(encoded[i]);
        }
      }
      out += "<a target=\"_blank\" href=\"" + compat::html_escape(href) + "\">" + display + "</a>";
    }
    copied = end;
    position = std::max(end, position + 1);
  }
  out.append(text.substr(copied));
  return out;
}

}  // namespace

bool is_word_char(char32_t cp) noexcept {
  const auto it = std::upper_bound(std::begin(kWordRanges), std::end(kWordRanges), cp,
                                   [](char32_t value, const Range& r) { return value < r.first; });
  return it != std::begin(kWordRanges) && cp <= (it - 1)->last;
}

std::expected<std::string, ParseError> auto_link(std::string_view text, const SafeList& sanitize_options) {
  if (is_blank(text)) {
    return std::string();
  }
  auto sanitized = sanitize_with_escaped_attribute_brackets(text, sanitize_options);
  if (!sanitized) {
    return std::unexpected(sanitized.error());
  }
  auto linked = auto_link_urls(*sanitized);
  if (!linked) {
    return std::unexpected(linked.error().parse);
  }
  auto emailed = auto_link_email_addresses(*linked);
  if (!emailed) {
    return std::unexpected(emailed.error().parse);
  }
  return std::move(*emailed);
}

}  // namespace campfire::richtext
