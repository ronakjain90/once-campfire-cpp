// Rails: rails-html-sanitizer 1.7.1 lib/rails/html/scrubbers.rb (PermitScrubber), loofah 2.25.2 lib/loofah/html5/scrub.rb. Rust: crates/richtext/src/sanitizer.rs
#include "richtext/sanitizer.hpp"

#include <array>
#include <cstdint>

namespace campfire::richtext {

namespace {

// --- UTF-8 ---------------------------------------------------------------------------------------

struct Decoded {
  char32_t code_point;
  std::size_t length;
  bool valid;
};

// Decodes one code point. An invalid byte decodes as itself with length 1 and valid false.
Decoded decode_utf8(std::string_view text, std::size_t pos) {
  const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
  const unsigned char b0 = byte(pos);
  if (b0 < 0x80) {
    return {b0, 1, true};
  }
  std::size_t length = 0;
  char32_t cp = 0;
  char32_t minimum = 0;
  if ((b0 & 0xE0) == 0xC0) {
    length = 2;
    cp = b0 & 0x1F;
    minimum = 0x80;
  } else if ((b0 & 0xF0) == 0xE0) {
    length = 3;
    cp = b0 & 0x0F;
    minimum = 0x800;
  } else if ((b0 & 0xF8) == 0xF0) {
    length = 4;
    cp = b0 & 0x07;
    minimum = 0x10000;
  } else {
    return {b0, 1, false};
  }
  if (pos + length > text.size()) {
    return {b0, 1, false};
  }
  for (std::size_t i = 1; i < length; ++i) {
    const unsigned char b = byte(pos + i);
    if ((b & 0xC0) != 0x80) {
      return {b0, 1, false};
    }
    cp = (cp << 6) | (b & 0x3F);
  }
  if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    return {b0, 1, false};
  }
  return {cp, length, true};
}

void append_utf8(char32_t cp, std::string& out) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

bool starts_with(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }
bool is_ascii_hex(char c) {
  return is_ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool is_ascii_lower(char c) { return c >= 'a' && c <= 'z'; }

std::string_view trim_unicode_space(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size()) {
    Decoded d = decode_utf8(text, begin);
    if (!is_unicode_space(d.code_point)) {
      break;
    }
    begin += d.length;
  }
  std::size_t end = text.size();
  while (end > begin) {
    std::size_t start = end - 1;
    while (start > begin && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
      --start;
    }
    Decoded d = decode_utf8(text, start);
    if (start + d.length != end || !is_unicode_space(d.code_point)) {
      break;
    }
    end = start;
  }
  return text.substr(begin, end - begin);
}

bool all_unicode_space(std::string_view text) {
  for (std::size_t i = 0; i < text.size();) {
    Decoded d = decode_utf8(text, i);
    if (!is_unicode_space(d.code_point)) {
      return false;
    }
    i += d.length;
  }
  return true;
}

// --- Loofah URI checks ---------------------------------------------------------------------------

// Loofah::HTML5::Scrub::CONTROL_CHARACTERS: /[`\u0000- \u007f\u0080-ā]/
bool is_control_character(char32_t c) {
  return c == '`' || c <= 0x20 || c == 0x7F || (c >= 0x80 && c <= 0x101);
}

std::string remove_control_characters(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    Decoded d = decode_utf8(text, i);
    if (!d.valid || !is_control_character(d.code_point)) {
      out.append(text.substr(i, d.length));
    }
    i += d.length;
  }
  return out;
}

// The code point of a numeric reference as CGI.unescapeHTML decodes it: below U+10FFFF, no
// surrogates, any number of leading zeros.
bool numeric_reference(std::string_view digits, int radix, char32_t& out) {
  if (digits.empty()) {
    return false;
  }
  for (char c : digits) {
    if (radix == 16 ? !is_ascii_hex(c) : !is_ascii_digit(c)) {
      return false;
    }
  }
  std::size_t zeros = 0;
  while (zeros < digits.size() && digits[zeros] == '0') {
    ++zeros;
  }
  std::string_view significant = digits.substr(zeros);
  if (significant.size() > (radix == 16 ? 6u : 7u)) {
    return false;
  }
  std::uint32_t value = 0;
  for (char c : significant) {
    std::uint32_t digit = is_ascii_digit(c) ? static_cast<std::uint32_t>(c - '0')
                          : (c >= 'a')      ? static_cast<std::uint32_t>(c - 'a' + 10)
                                            : static_cast<std::uint32_t>(c - 'A' + 10);
    value = value * static_cast<std::uint32_t>(radix) + digit;
  }
  if (value >= 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
    return false;
  }
  out = value;
  return true;
}

// Loofah::HTML5::Scrub.decode_numeric_character_references: /&#(x[0-9a-f]+|[0-9]+);?/i
std::string decode_numeric_character_references(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  std::size_t i = 0;
  std::size_t copied = 0;
  while (i < s.size()) {
    if (s[i] == '&' && i + 1 < s.size() && s[i + 1] == '#') {
      const std::size_t start = i + 2;
      const bool hex = start < s.size() && (s[start] == 'x' || s[start] == 'X');
      const std::size_t digits_start = hex ? start + 1 : start;
      std::size_t end = digits_start;
      while (end < s.size() && (hex ? is_ascii_hex(s[end]) : is_ascii_digit(s[end]))) {
        ++end;
      }
      if (end > digits_start) {
        const std::size_t full_end = (end < s.size() && s[end] == ';') ? end + 1 : end;
        std::string_view digits = s.substr(digits_start, end - digits_start);
        std::size_t zeros = 0;
        while (zeros < digits.size() && digits[zeros] == '0') {
          ++zeros;
        }
        std::string_view significant = digits.substr(zeros);
        bool decoded = false;
        char32_t cp = 0;
        if (significant.size() <= (hex ? 6u : 7u)) {
          std::uint64_t value = 0;
          for (char c : significant) {
            std::uint64_t digit = is_ascii_digit(c) ? static_cast<std::uint64_t>(c - '0')
                                  : (c >= 'a')      ? static_cast<std::uint64_t>(c - 'a' + 10)
                                                    : static_cast<std::uint64_t>(c - 'A' + 10);
            value = value * (hex ? 16 : 10) + digit;
          }
          // Ruby's Integer#chr(UTF_8) raises RangeError above U+10FFFF and for surrogates.
          if (value <= 0x10FFFF && !(value >= 0xD800 && value <= 0xDFFF)) {
            decoded = true;
            cp = static_cast<char32_t>(value);
          }
        }
        out.append(s.substr(copied, i - copied));
        if (decoded) {
          append_utf8(cp, out);
        } else {
          out.append(s.substr(i, full_end - i));
        }
        i = full_end;
        copied = i;
        continue;
      }
    }
    ++i;
  }
  out.append(s.substr(copied));
  return out;
}

// gsub of /&(Tab|NewLine);/ with "" in one pass over the original text.
std::string remove_whitespace_references(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    if (s[i] == '&') {
      if (starts_with(s.substr(i), "&Tab;")) {
        i += 5;
        continue;
      }
      if (starts_with(s.substr(i), "&NewLine;")) {
        i += 9;
        continue;
      }
    }
    out.push_back(s[i++]);
  }
  return out;
}

std::string replace_colon_references(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    if (s[i] == '&' && starts_with(s.substr(i), "&colon;")) {
      out.push_back(':');
      i += 7;
      continue;
    }
    out.push_back(s[i++]);
  }
  return out;
}

// String#downcase, as far as it matters to what follows: the result is matched against ASCII
// schemes. U+212A KELVIN SIGN folds to "k" and U+0130 to "i" plus U+0307.
std::string downcase(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    Decoded d = decode_utf8(s, i);
    if (d.valid && d.code_point == 0x212A) {
      out.push_back('k');
    } else if (d.valid && d.code_point == 0x130) {
      out.append("i\xCC\x87");
    } else if (d.length == 1 && s[i] >= 'A' && s[i] <= 'Z') {
      out.push_back(static_cast<char>(s[i] - 'A' + 'a'));
    } else {
      out.append(s.substr(i, d.length));
    }
    i += d.length;
  }
  return out;
}

// Length of Loofah's PROTOCOL_SEPARATOR /:|(&#0*58)|(&#x0*3a)|(%|&#37;)3A/i at the start of `s`
// (`s` is lower case), or 0.
std::size_t separator_length(std::string_view s) {
  if (s.empty()) {
    return 0;
  }
  if (s[0] == ':') {
    return 1;
  }
  if (starts_with(s, "&#x")) {
    std::size_t i = 3;
    while (i < s.size() && s[i] == '0') {
      ++i;
    }
    if (starts_with(s.substr(i), "3a")) {
      return i + 2;
    }
  }
  if (starts_with(s, "&#")) {
    std::size_t i = 2;
    while (i < s.size() && s[i] == '0') {
      ++i;
    }
    if (starts_with(s.substr(i), "58")) {
      return i + 2;
    }
  }
  if (starts_with(s, "%3a")) {
    return 3;
  }
  if (starts_with(s, "&#37;3a")) {
    return 7;
  }
  return 0;
}

// Matches \A[a-z][a-z0-9+\-.]* followed by the separator, and returns the scheme.
bool protocol_before_separator(std::string_view s, std::string_view& protocol) {
  if (s.empty() || !is_ascii_lower(s[0])) {
    return false;
  }
  std::size_t end = 1;
  while (end < s.size() && (is_ascii_lower(s[end]) || is_ascii_digit(s[end]) || s[end] == '+' ||
                            s[end] == '-' || s[end] == '.')) {
    ++end;
  }
  if (separator_length(s.substr(end)) == 0) {
    return false;
  }
  protocol = s.substr(0, end);
  return true;
}

bool is_tchar(char c) {
  return is_ascii_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

// Loofah::HTML5::Scrub.data_uri_mediatype
std::string data_uri_mediatype(std::string_view s) {
  if (starts_with(s, "data:")) {
    s.remove_prefix(5);
  }
  std::size_t comma = s.find(',');
  if (comma == std::string_view::npos) {
    return {};  // nil: never an allowed type
  }
  std::string_view metadata = s.substr(0, comma);
  if (metadata.size() >= 7 && metadata.substr(metadata.size() - 7) == ";base64") {
    metadata.remove_suffix(7);
  }
  std::string_view mediatype = metadata.substr(0, metadata.find(';'));
  const auto is_space = [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r' || c == '\0';
  };
  while (!mediatype.empty() && is_space(mediatype.front())) {
    mediatype.remove_prefix(1);
  }
  while (!mediatype.empty() && is_space(mediatype.back())) {
    mediatype.remove_suffix(1);
  }
  std::size_t slash = mediatype.find('/');
  bool valid = slash != std::string_view::npos && slash > 0 && slash + 1 < mediatype.size();
  if (valid) {
    for (std::size_t i = 0; i < mediatype.size(); ++i) {
      if (i != slash && !is_tchar(mediatype[i])) {
        valid = false;
        break;
      }
    }
  }
  return valid ? std::string(mediatype) : "text/plain";
}

// --- Style ---------------------------------------------------------------------------------------

// Folds the code points that Rust's (?i)[a-z] also matches: U+212A is k, U+017F is s.
char32_t fold(char32_t c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A' + 'a';
  }
  if (c == 0x212A) {
    return 'k';
  }
  if (c == 0x17F) {
    return 's';
  }
  return c;
}

bool is_letter(char32_t c) { return c >= 'a' && c <= 'z'; }
bool is_digit(char32_t c) { return c >= '0' && c <= '9'; }

// \A(?:[a-z]+|#[0-9a-f]{3,8}|var\(\s*--[a-z0-9_-]+\s*\)|(?:rgb|rgba|hsl|hsla)\([0-9a-z.,%\s/+-]*\))\z
bool is_plain_color(std::string_view value) {
  std::u32string cps;
  for (std::size_t i = 0; i < value.size();) {
    Decoded d = decode_utf8(value, i);
    cps.push_back(d.valid ? fold(d.code_point) : 0xFFFFFFFF);
    i += d.length;
  }
  const std::size_t n = cps.size();
  if (n == 0) {
    return false;
  }
  const auto all = [&](std::size_t from, std::size_t to, auto pred) {
    for (std::size_t i = from; i < to; ++i) {
      if (!pred(cps[i])) {
        return false;
      }
    }
    return true;
  };
  if (all(0, n, is_letter)) {
    return true;
  }
  if (cps[0] == '#') {
    return n >= 4 && n <= 9 &&
           all(1, n, [](char32_t c) { return is_digit(c) || (c >= 'a' && c <= 'f'); });
  }
  const auto is_space = [](char32_t c) { return c < 0x110000 && is_unicode_space(c); };
  const auto matches = [&](std::size_t at, std::u32string_view literal) {
    return cps.compare(at, literal.size(), literal) == 0 && at + literal.size() <= n;
  };
  if (matches(0, U"var(")) {
    std::size_t i = 4;
    while (i < n && is_space(cps[i])) {
      ++i;
    }
    if (!matches(i, U"--")) {
      return false;
    }
    i += 2;
    const std::size_t name_start = i;
    while (i < n && (is_letter(cps[i]) || is_digit(cps[i]) || cps[i] == '_' || cps[i] == '-')) {
      ++i;
    }
    if (i == name_start) {
      return false;
    }
    while (i < n && is_space(cps[i])) {
      ++i;
    }
    return i + 1 == n && cps[i] == ')';
  }
  for (std::u32string_view fn : {U"rgba(", U"rgb(", U"hsla(", U"hsl("}) {
    if (matches(0, fn)) {
      if (cps[n - 1] != ')' || n < fn.size() + 1) {
        return false;
      }
      return all(fn.size(), n - 1, [&](char32_t c) {
        return is_digit(c) || is_letter(c) || c == '.' || c == ',' || c == '%' || c == '/' ||
               c == '+' || c == '-' || is_space(c);
      });
    }
  }
  return false;
}

// Rust scrub_style: keep only `color` and `background-color` with plain color values.
void scrub_style(Dom& dom, Node* node) {
  const Attr* style_attr = node->find_attr("style");
  if (style_attr == nullptr) {
    return;
  }
  std::string_view style = style_attr->value;
  struct Declaration {
    std::string property;
    std::string_view value;
  };
  std::vector<Declaration> declarations;
  std::size_t pos = 0;
  while (pos <= style.size()) {
    std::size_t semi = style.find(';', pos);
    std::string_view piece =
        style.substr(pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
    if (!trim_unicode_space(piece).empty()) {
      std::size_t colon = piece.find(':');
      std::string_view property = colon == std::string_view::npos ? piece : piece.substr(0, colon);
      std::string_view value =
          colon == std::string_view::npos ? std::string_view() : piece.substr(colon + 1);
      std::string lowered(trim_unicode_space(property));
      for (char& c : lowered) {
        if (c >= 'A' && c <= 'Z') {
          c = static_cast<char>(c - 'A' + 'a');
        }
      }
      declarations.push_back({std::move(lowered), trim_unicode_space(value)});
    }
    if (semi == std::string_view::npos) {
      break;
    }
    pos = semi + 1;
  }
  const auto allowed = [](const Declaration& d) {
    return (d.property == "color" || d.property == "background-color") && is_plain_color(d.value);
  };
  if (!declarations.empty() && std::all_of(declarations.begin(), declarations.end(), allowed)) {
    return;
  }
  std::string scrubbed;
  for (const Declaration& d : declarations) {
    if (allowed(d)) {
      scrubbed.append(d.property).append(": ").append(d.value).append(";");
    }
  }
  if (scrubbed.empty()) {
    for (std::size_t i = 0; i < node->attr_count; ++i) {
      if (node->attrs[i].ns == AttrNs::None && node->attrs[i].name == "style") {
        dom.remove_attr_at(node, i);
        break;
      }
    }
  } else {
    dom.set_attr(node, "style", scrubbed);
  }
}

// --- Attribute scrubbing -------------------------------------------------------------------------

// Loofah::HTML5::SafeList::ATTR_VAL_IS_URI
bool is_uri_attribute(const Attr& attr) {
  static const NameSet kUri{"action", "cite", "href",       "longdesc", "poster",
                            "preload", "src",  "xlink:href", "xml:base"};
  if (attr.ns == AttrNs::None) {
    return kUri.contains(attr.name);
  }
  return kUri.contains(attr.qualified_name());
}

bool is_escaped(char c) {
  return c == ' ' || c == '"' || (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n' && c != '\r');
}

// Loofah's force_correct_attribute_escaping! on libxml2: spaces and double quotes in href,
// action, src and an a's name become %20 and %22. The value goes back through
// Nokogiri::XML::Attr#value=, where libxml2 drops the C0 controls.
void force_correct_attribute_escaping(Dom& dom, Node* node) {
  const bool is_a = node->name == "a";
  for (Attr& attr : node->attributes()) {
    const bool qualifies = attr.ns == AttrNs::None && (attr.name == "href" || attr.name == "action" ||
                                                       attr.name == "src" || (is_a && attr.name == "name"));
    if (!qualifies || !std::any_of(attr.value.begin(), attr.value.end(), is_escaped)) {
      continue;
    }
    std::string escaped;
    escaped.reserve(attr.value.size() + 8);
    for (char c : attr.value) {
      if (c == ' ') {
        escaped.append("%20");
      } else if (c == '"') {
        escaped.append("%22");
      } else if (is_escaped(c)) {
        continue;
      } else {
        escaped.push_back(c);
      }
    }
    attr.value = dom.arena().copy(escaped);
  }
}

// PermitScrubber#scrub_attributes with an attribute allowlist. Each allowed attribute re-escapes
// every URL attribute of the node as it goes, so a later URL is checked in its re-escaped form.
void scrub_attributes(Dom& dom, Node* node, const SafeList& list) {
  std::size_t i = 0;
  while (i < node->attr_count) {
    const Attr& attr = node->attrs[i];
    bool scrubbed = !list.attributes.contains(attr.name);
    if (!scrubbed && is_uri_attribute(attr) && !allowed_uri(attr.value)) {
      scrubbed = true;
    }
    if (scrubbed) {
      dom.remove_attr_at(node, i);
      continue;
    }
    // A blank src goes, but the escaping still runs for the attributes after it.
    if (attr.ns == AttrNs::None && attr.name == "src" && all_unicode_space(attr.value)) {
      dom.remove_attr_at(node, i);
    } else {
      ++i;
    }
    force_correct_attribute_escaping(dom, node);
  }
  scrub_style(dom, node);
}

void scrub_node(Dom& dom, Node* node, const SafeList& list) {
  switch (node->type) {
    case NodeType::Text:
    case NodeType::CData:  // Only in foreign content, which goes with its root element.
    case NodeType::Fragment:
      return;
    case NodeType::Comment:
      dom.detach(node);
      return;
    case NodeType::Element:
      break;
  }
  if (!list.tags.contains(node->name)) {
    // An HTML element is unwrapped. A foreign (SVG, MathML) element goes with its contents.
    if (node->ns == Ns::Html) {
      dom.unwrap(node);
    } else {
      dom.detach(node);
    }
    return;
  }
  scrub_attributes(dom, node, list);
}

// Loofah::Scrubber#traverse_conditionally_bottom_up: children (as they were before any of them
// was scrubbed) first, then the node itself.
void scrub_bottom_up(Dom& dom, Node* node, const SafeList& list) {
  for (Node* child = node->first_child; child != nullptr;) {
    Node* next = child->next;
    scrub_bottom_up(dom, child, list);
    child = next;
  }
  scrub_node(dom, node, list);
}

constexpr auto kDefaultTags = std::to_array<std::string_view>({
    "a",   "abbr", "acronym", "address", "b",   "big", "blockquote", "br",  "cite", "code",
    "dd",  "del",  "dfn",     "div",     "dl",  "dt",  "em",         "h1",  "h2",   "h3",
    "h4",  "h5",   "h6",      "hr",      "i",   "img", "ins",        "kbd", "li",   "mark",
    "ol",  "p",    "pre",     "samp",    "small", "span", "strong",  "sub", "sup",  "time",
    "tt",  "ul",   "var"});

// DEFAULT_ALLOWED_ATTRIBUTES without `name`, which lets a message shadow the page's DOM globals
// (<img name="body"> shadows document.body). The Rust port drops it on purpose.
constexpr auto kDefaultAttributes = std::to_array<std::string_view>({
    "abbr", "alt", "cite", "class", "datetime", "height", "href", "lang", "src", "title", "width",
    "xml:lang"});

// ContentFilters::EDITOR_FORMATTING_TAGS and _ATTRIBUTES
constexpr auto kEditorTags = std::to_array<std::string_view>({"s",     "u",  "mark", "table", "thead",
                                                          "tbody", "tfoot", "tr", "th",    "td"});

// ActionText::Attachment::ATTRIBUTES
constexpr auto kAttachmentAttributes = std::to_array<std::string_view>({
    "sgid",     "content-type", "url",          "href",    "filename", "filesize",
    "width",    "height",       "previewable",  "presentation", "caption", "content"});

NameSet make_set(std::initializer_list<std::span<const std::string_view>> parts,
                 std::initializer_list<std::string_view> extra = {}) {
  NameSet set;
  for (auto part : parts) {
    NameSet piece;
    for (std::string_view name : part) {
      piece.add({name});
    }
    set.add(piece);
  }
  set.add(extra);
  return set;
}

}  // namespace

// --- Public --------------------------------------------------------------------------------------

bool is_unicode_space(char32_t c) noexcept {
  return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F ||
         c == 0x205F || c == 0x3000;
}

std::string cgi_unescape_html(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  std::size_t pos = 0;
  while (pos < s.size()) {
    std::size_t amp = s.find('&', pos);
    if (amp == std::string_view::npos) {
      break;
    }
    out.append(s.substr(pos, amp - pos));
    std::string_view rest = s.substr(amp);
    struct Named {
      std::string_view name;
      char value;
    };
    static constexpr Named kNamed[] = {
        {"&apos;", '\''}, {"&amp;", '&'}, {"&quot;", '"'}, {"&gt;", '>'}, {"&lt;", '<'}};
    bool done = false;
    for (const Named& named : kNamed) {
      if (starts_with(rest, named.name)) {
        out.push_back(named.value);
        pos = amp + named.name.size();
        done = true;
        break;
      }
    }
    if (done) {
      continue;
    }
    std::size_t semi = rest.find(';');
    char32_t cp = 0;
    bool ok = false;
    if (semi != std::string_view::npos) {
      std::string_view body = rest.substr(1, semi - 1);
      if (starts_with(body, "#x") || starts_with(body, "#X")) {
        ok = numeric_reference(body.substr(2), 16, cp);
      } else if (starts_with(body, "#")) {
        ok = numeric_reference(body.substr(1), 10, cp);
      }
    }
    if (ok) {
      append_utf8(cp, out);
      pos = amp + semi + 1;
    } else {
      out.push_back('&');
      pos = amp + 1;
    }
  }
  out.append(s.substr(pos));
  return out;
}

bool allowed_uri(std::string_view uri) {
  std::string s = remove_control_characters(
      decode_numeric_character_references(cgi_unescape_html(remove_control_characters(uri))));
  s = remove_whitespace_references(s);
  s = replace_colon_references(s);
  s = downcase(s);
  std::string_view protocol;
  if (!protocol_before_separator(s, protocol)) {
    return true;
  }
  static const NameSet kProtocols{
      "afs",  "aim",  "callto", "data",   "ed2k",  "fax",    "ftp",  "gopher", "http",
      "https", "irc", "line",   "mailto", "modem", "news",   "nntp", "rsync",  "rtsp",
      "sftp", "sms",  "ssh",    "tag",    "tel",   "telnet", "urn",  "webcal", "xmpp"};
  if (!kProtocols.contains(protocol)) {
    return false;
  }
  if (protocol == "data") {
    std::string mediatype = data_uri_mediatype(s);
    return mediatype == "image/gif" || mediatype == "image/jpeg" || mediatype == "image/png" ||
           mediatype == "text/css" || mediatype == "text/plain";
  }
  return true;
}

const SafeList& SafeList::defaults() {
  static const SafeList list{make_set({kDefaultTags}), make_set({kDefaultAttributes})};
  return list;
}

const SafeList& SafeList::action_text() {
  static const SafeList list{
      make_set({kDefaultTags, kEditorTags},
               {"action-text-attachment", "figure", "figcaption", "video", "audio", "source",
                "embed", "table", "tbody", "tr", "th", "td"}),
      make_set({kDefaultAttributes, kAttachmentAttributes},
               {"controls", "poster", "data-language", "style", "value", "start"})};
  return list;
}

const NameSet& sanitize_tags_allowed_tags() {
  static const NameSet tags = [] {
    // SanitizeTags' own list: the default tags without img and mark, then the editor tags.
    NameSet filtered;
    for (std::string_view name : kDefaultTags) {
      if (name != "img" && name != "mark") {
        filtered.add({name});
      }
    }
    filtered.add(
        {"s", "u", "mark", "table", "thead", "tbody", "tfoot", "tr", "th", "td",
         "action-text-attachment", "figure", "figcaption"});
    return filtered;
  }();
  return tags;
}

const SafeList& SafeList::content_filter() {
  static const SafeList list{sanitize_tags_allowed_tags(), action_text().attributes};
  return list;
}

const SafeList& SafeList::auto_link() {
  static const SafeList list{make_set({kDefaultTags, kEditorTags}),
                             make_set({kDefaultAttributes}, {"data-language"})};
  return list;
}

void scrub(Dom& dom, const SafeList& list) {
  for (Node* child = dom.root()->first_child; child != nullptr;) {
    Node* next = child->next;
    scrub_bottom_up(dom, child, list);
    child = next;
  }
}

std::expected<std::string, ParseError> sanitize(std::string_view html, const SafeList& list) {
  if (html.empty()) {
    return std::string();
  }
  auto dom = parse_fragment(html);
  if (!dom) {
    return std::unexpected(dom.error());
  }
  scrub(*dom, list);
  return to_html(dom->root());
}

}  // namespace campfire::richtext
