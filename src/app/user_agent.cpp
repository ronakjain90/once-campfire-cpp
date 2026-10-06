// The useragent gem 0.16.11. Rust: crates/campfire/src/concerns/user_agent.rs.
#include "app/user_agent.hpp"

#include <algorithm>
#include <array>
#include <cctype>

#include "compat/ruby.hpp"

namespace campfire::app::ua {

namespace {

constexpr std::string_view kDefaultUserAgent = "Mozilla/4.0 (compatible)";
constexpr std::string_view kKelvin = "\xE2\x84\xAA";  // U+212A: the only non-ASCII character that lowercases to ASCII

bool is_ruby_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
bool is_digit(char c) {
  return c >= '0' && c <= '9';
}
bool is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool is_version_char(char c) {
  return is_digit(c) || c == '.';
}

// Length of the run at the start of `s` of bytes that satisfy `f`.
template <class F>
std::size_t run(std::string_view s, F f) {
  std::size_t n = 0;
  while (n < s.size() && f(s[n])) ++n;
  return n;
}

bool starts_with(std::string_view s, std::string_view prefix) {
  return s.starts_with(prefix);
}
bool contains(std::string_view s, std::string_view needle) {
  return s.find(needle) != std::string_view::npos;
}

// `downcase` as far as it matters here: the needles are ASCII, so only A-Z and the Kelvin sign matter.
std::string fold(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c >= 'A' && c <= 'Z') {
      out.push_back(static_cast<char>(c - 'A' + 'a'));
    } else if (c == '\xE2' && s.substr(i, 3) == kKelvin) {
      out.push_back('k');
      i += 2;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

bool same_ignoring_case(std::string_view a, std::string_view b) {
  return fold(a) == fold(b);
}
// `haystack.downcase.include?(needle)` for a lowercase ASCII needle.
bool contains_ignoring_case(std::string_view haystack, std::string_view needle) {
  return contains(fold(haystack), needle);
}

// Byte length of the UTF-8 character at the start of `s`.
std::size_t char_length(std::string_view s) {
  const auto b = static_cast<unsigned char>(s[0]);
  std::size_t n = b < 0x80 ? 1 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
  return std::min(n, s.size());
}

// Is the character at the start of `s` Unicode white space? Sets `length`.
bool unicode_space_at(std::string_view s, std::size_t& length) {
  length = char_length(s);
  const auto c0 = static_cast<unsigned char>(s[0]);
  if (length == 1) return (c0 >= 9 && c0 <= 13) || c0 == ' ';
  const std::string_view ch = s.substr(0, length);
  static constexpr std::array<std::string_view, 17> kSpaces = {
      "\xC2\x85",     "\xC2\xA0",     "\xE1\x9A\x80", "\xE2\x80\xA8", "\xE2\x80\xA9", "\xE2\x80\xAF",
      "\xE2\x81\x9F", "\xE3\x80\x80", "\xE2\x80\x80", "\xE2\x80\x81", "\xE2\x80\x82", "\xE2\x80\x83",
      "\xE2\x80\x84", "\xE2\x80\x85", "\xE2\x80\x86", "\xE2\x80\x87", "\xE2\x80\x88"};
  if (std::ranges::find(kSpaces, ch) != kSpaces.end()) return true;
  return ch == "\xE2\x80\x89" || ch == "\xE2\x80\x8A";
}

std::vector<std::string> ruby_split(std::string_view s, std::string_view separator) {
  std::vector<std::string> parts;
  std::size_t at = 0;
  while (true) {
    const std::size_t found = s.find(separator, at);
    if (found == std::string_view::npos) {
      parts.emplace_back(s.substr(at));
      break;
    }
    parts.emplace_back(s.substr(at, found - at));
    at = found + separator.size();
  }
  while (!parts.empty() && parts.back().empty()) parts.pop_back();  // trailing empty fields go
  return parts;
}

// `str.scan(/\d+|[A-Za-z][0-9A-Za-z-]*$/)`
std::vector<Segment> scan_sequences(std::string_view s) {
  std::vector<Segment> out;
  std::size_t i = 0;
  while (i < s.size()) {
    if (is_digit(s[i])) {
      const std::size_t start = i;
      while (i < s.size() && is_digit(s[i])) ++i;
      std::string_view digits = s.substr(start, i - start);
      while (digits.size() > 1 && digits.front() == '0') digits.remove_prefix(1);
      out.push_back({true, std::string(digits)});
    } else if (is_alpha(s[i])) {
      std::size_t end = i + 1;
      while (end < s.size() && (is_alpha(s[end]) || is_digit(s[end]) || s[end] == '-')) ++end;
      if (end == s.size()) {
        out.push_back({false, std::string(s.substr(i))});
        i = end;
      } else {
        ++i;
      }
    } else {
      ++i;
    }
  }
  return out;
}

}  // namespace

std::string downcase(std::string_view text) {
  return fold(text);
}

bool is_present(std::string_view text) {
  std::size_t i = 0;
  while (i < text.size()) {
    std::size_t length = 0;
    if (!unicode_space_at(text.substr(i), length)) return true;
    i += length;
  }
  return false;
}

// ---- Version -----------------------------------------------------------------------------------

Version::Version(std::string_view text) : text_(text) {
  blank_ = std::ranges::all_of(text, is_ruby_space);
  const std::size_t digits = run(text, is_digit);
  comparable_ = !blank_ && digits > 0 && (digits == text.size() || text[digits] == '.');
}

bool Version::is_present() const {
  return ua::is_present(text_);
}

std::vector<Segment> Version::to_a() const {
  if (blank_) return {};
  if (comparable_) return scan_sequences(text_);
  return {Segment{false, text_}};
}

int Version::compare(const Version& other) const {
  if (comparable_) {
    const auto ours = to_a();
    const auto theirs = other.to_a();
    const Segment zero{true, "0"};
    for (std::size_t i = 0; i < 6; ++i) {
      const Segment& a = i < ours.size() ? ours[i] : zero;
      const Segment& b = i < theirs.size() ? theirs[i] : zero;
      if (!a.is_int && b.is_int) return -1;
      if (a.is_int && !b.is_int) return 1;
      if (a == b) continue;
      if (a.is_int) {
        if (a.text.size() != b.text.size()) return a.text.size() < b.text.size() ? -1 : 1;
        return a.text < b.text ? -1 : 1;
      }
      return a.text < b.text ? -1 : 1;  // bytes
    }
    return 0;
  }
  return text_ == other.text_ ? 0 : -1;
}

// ---- parse -------------------------------------------------------------------------------------

namespace {

// `UserAgent::MATCHER` at the start of `s`: `^['"]*([^/\s]+)/?([^\s,]*)(\s\(([^\)]*)\)|,gzip\(gfe\))?`.
std::optional<std::pair<std::size_t, Product>> match_product(std::string_view s) {
  const auto is_product_byte = [](char c) { return c != '/' && !is_ruby_space(c); };
  const std::size_t quotes = run(s, [](char c) { return c == '\'' || c == '"'; });
  std::size_t start = 0;
  if (quotes < s.size() && is_product_byte(s[quotes])) {
    start = quotes;
  } else if (quotes > 0) {
    start = quotes - 1;  // backtrack: the last quote is the product
  } else {
    return std::nullopt;
  }
  std::size_t i = start + 1;
  i += run(s.substr(i), is_product_byte);
  const std::string_view name = s.substr(start, i - start);
  if (i < s.size() && s[i] == '/') ++i;
  const std::size_t version_start = i;
  i += run(s.substr(i), [](char c) { return !is_ruby_space(c) && c != ','; });
  const std::string_view version = s.substr(version_start, i - version_start);

  std::optional<std::vector<std::string>> comment;
  if (i < s.size() && is_ruby_space(s[i]) && i + 1 < s.size() && s[i + 1] == '(') {
    const std::size_t close = s.find(')', i + 2);
    if (close != std::string_view::npos) {
      comment = ruby_split(s.substr(i + 2, close - (i + 2)), "; ");
      i = close + 1;
    }
  } else if (s.substr(i).starts_with(",gzip(gfe)")) {
    i += std::string_view(",gzip(gfe)").size();
  }
  return std::pair{i, Product{std::string(name), Version(version), std::move(comment)}};
}

std::optional<std::string> joined_comment(const Product& p) {
  if (!p.comment) return std::nullopt;
  std::string out;
  for (std::size_t i = 0; i < p.comment->size(); ++i) {
    if (i > 0) out += "; ";
    out += (*p.comment)[i];
  }
  return out;
}

const std::string* comment_at(const Product& p, std::size_t index) {
  if (!p.comment || index >= p.comment->size()) return nullptr;
  return &(*p.comment)[index];
}

// `/Windows NT [\d\.]+|Windows Phone (OS )?[\d\.]+/`: the matched text.
std::optional<std::string_view> windows_os(std::string_view s) {
  for (std::size_t i = 0; i < s.size(); ++i) {
    const std::string_view rest = s.substr(i);
    std::string_view tail;
    if (starts_with(rest, "Windows NT ")) {
      tail = rest.substr(11);
    } else if (starts_with(rest, "Windows Phone ")) {
      tail = rest.substr(14);
      if (starts_with(tail, "OS ") && run(tail.substr(3), is_version_char) > 0) tail.remove_prefix(3);
    } else {
      continue;
    }
    const std::size_t digits = run(tail, is_version_char);
    if (digits > 0) return rest.substr(0, rest.size() - tail.size() + digits);
  }
  return std::nullopt;
}

// `joined_comment =~ /Trident.+rv:/`: an "rv:" that is not at the start of the text after "Trident", on the same line.
bool trident_rv(std::string_view s) {
  std::size_t at = 0;
  while ((at = s.find("Trident", at)) != std::string_view::npos) {
    std::string_view line = s.substr(at + 7);
    line = line.substr(0, line.find('\n'));
    if (line.find("rv:", 1) != std::string_view::npos) return true;
    ++at;
  }
  return false;
}

// `joined_comment[/(MSIE\s|rv:)([\d\.]+)/, 2]`
std::optional<std::string_view> ie_version(std::string_view s) {
  for (std::size_t i = 0; i < s.size(); ++i) {
    const std::string_view rest = s.substr(i);
    std::string_view tail;
    if (starts_with(rest, "MSIE") && rest.size() > 4 && is_ruby_space(rest[4])) {
      tail = rest.substr(5);
    } else if (starts_with(rest, "rv:")) {
      tail = rest.substr(3);
    } else {
      continue;
    }
    const std::size_t digits = run(tail, is_version_char);
    if (digits > 0) return tail.substr(0, digits);
  }
  return std::nullopt;
}

// The capture of `/<prefix>([digits and dots]+)/` at its leftmost match.
std::optional<std::string_view> capture_after(std::string_view s, std::string_view prefix) {
  std::size_t at = 0;
  while ((at = s.find(prefix, at)) != std::string_view::npos) {
    const std::string_view tail = s.substr(at + prefix.size());
    const std::size_t len = run(tail, is_version_char);
    if (len > 0) return tail.substr(0, len);
    ++at;
  }
  return std::nullopt;
}

// `/(?:Intel|PPC) Mac OS X\s*([0-9_\.]+)?/`: nullopt for no match, "" for a match with no version.
std::optional<std::string_view> mac_os_x_version(std::string_view os) {
  for (std::size_t i = 0; i < os.size(); ++i) {
    std::string_view after = os.substr(i);
    if (starts_with(after, "Intel Mac OS X")) {
      after.remove_prefix(14);
    } else if (starts_with(after, "PPC Mac OS X")) {
      after.remove_prefix(12);
    } else {
      continue;
    }
    after.remove_prefix(run(after, is_ruby_space));
    const std::size_t digits = run(after, [](char c) { return is_digit(c) || c == '_' || c == '.'; });
    return after.substr(0, digits);
  }
  return std::nullopt;
}

// `IOS_VERSION_REGEX = /CPU (?:iPhone |iPod )?OS ([\d_]+) like Mac OS X/`
std::optional<std::string_view> ios_version(std::string_view os) {
  for (std::size_t i = 0; i < os.size(); ++i) {
    if (!starts_with(os.substr(i), "CPU ")) continue;
    const std::string_view rest = os.substr(i + 4);
    const std::array<std::string_view, 3> options = {
        starts_with(rest, "iPhone ") ? rest.substr(7) : std::string_view{"\x01"},
        starts_with(rest, "iPod ") ? rest.substr(5) : std::string_view{"\x01"}, rest};
    for (std::string_view candidate : options) {
      if (candidate == "\x01" || !starts_with(candidate, "OS ")) continue;
      candidate.remove_prefix(3);
      const std::size_t digits = run(candidate, [](char c) { return is_digit(c) || c == '_'; });
      if (digits > 0 && starts_with(candidate.substr(digits), " like Mac OS X")) return candidate.substr(0, digits);
    }
  }
  return std::nullopt;
}

// `/CrOS\s([^\s]+)\s(\d+(\.\d+)*)/`: the second capture.
std::optional<std::string_view> chrome_os_version(std::string_view os) {
  for (std::size_t i = 0; i + 4 <= os.size(); ++i) {
    if (!starts_with(os.substr(i), "CrOS")) continue;
    const std::string_view rest = os.substr(i + 4);
    if (rest.empty() || !is_ruby_space(rest[0])) continue;
    const std::size_t word = run(rest.substr(1), [](char c) { return !is_ruby_space(c); });
    if (word == 0) continue;
    const std::size_t at = 1 + word;
    if (at >= rest.size() || !is_ruby_space(rest[at])) continue;
    const std::size_t start = at + 1;
    std::size_t end = start + run(rest.substr(start), is_digit);
    if (end == start) continue;
    while (end + 1 < rest.size() && rest[end] == '.' && is_digit(rest[end + 1])) end += 1 + run(rest.substr(end + 1), is_digit);
    return rest.substr(start, end - start);
  }
  return std::nullopt;
}

// `WEBKIT_VERSION_REGEXP = /\A(?<webkit>AppleWebKit)\/(?<version>[\d\.]+)/i`: the version.
std::optional<std::string_view> webkit_comment_version(std::string_view comment) {
  std::size_t at = 0;
  for (int n = 0; n < 11; ++n) {  // eleven characters
    if (at >= comment.size()) return std::nullopt;
    at += char_length(comment.substr(at));
  }
  if (fold(comment.substr(0, at)) != "applewebkit") return std::nullopt;
  std::string_view tail = comment.substr(at);
  if (!tail.starts_with('/')) return std::nullopt;
  tail.remove_prefix(1);
  const std::size_t digits = run(tail, is_version_char);
  if (digits == 0) return std::nullopt;
  return tail.substr(0, digits);
}

// `Webkit::BuildVersions`: Safari before version 3 reported only the WebKit build.
std::optional<std::string_view> webkit_build_version(std::string_view build) {
  struct Entry {
    std::string_view build;
    std::string_view version;
  };
  static constexpr std::array<Entry, 28> kBuilds = {{
      {"85.7", "1.0"},       {"85.8.5", "1.0.3"},   {"85.8.2", "1.0.3"},   {"124", "1.2"},       {"125.2", "1.2.2"},
      {"125.4", "1.2.3"},    {"125.5.5", "1.2.4"},  {"125.5.6", "1.2.4"},  {"125.5.7", "1.2.4"}, {"312.1.1", "1.3"},
      {"312.1", "1.3"},      {"312.5", "1.3.1"},    {"312.5.1", "1.3.1"},  {"312.5.2", "1.3.1"}, {"312.8", "1.3.2"},
      {"312.8.1", "1.3.2"},  {"412", "2.0"},        {"412.6", "2.0"},      {"412.6.2", "2.0"},   {"412.7", "2.0.1"},
      {"416.11", "2.0.2"},   {"416.12", "2.0.2"},   {"417.9", "2.0.3"},    {"418", "2.0.3"},     {"418.8", "2.0.4"},
      {"418.9", "2.0.4"},    {"418.9.1", "2.0.4"},  {"419", "2.0.4"},
  }};
  for (const Entry& e : kBuilds) {
    if (e.build == build) return e.version;
  }
  if (build == "425.13") return "2.2";
  if (build == "534.52.7") return "5.1.2";
  return std::nullopt;
}

// `UserAgent::OperatingSystems.normalize_os`
std::string normalize_os(std::string_view os) {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 12> kWindows = {{
      {"Windows NT 10.0", "Windows 10"},
      {"Windows NT 6.3", "Windows 8.1"},
      {"Windows NT 6.2", "Windows 8"},
      {"Windows NT 6.1", "Windows 7"},
      {"Windows NT 6.0", "Windows Vista"},
      {"Windows NT 5.2", "Windows XP x64 Edition"},
      {"Windows NT 5.1", "Windows XP"},
      {"Windows NT 5.01", "Windows 2000, Service Pack 1 (SP1)"},
      {"Windows NT 5.0", "Windows 2000"},
      {"Windows NT 4.0", "Windows NT 4.0"},
      {"Windows 98", "Windows 98"},
      {"Windows 95", "Windows 95"},
  }};
  for (const auto& [from, to] : kWindows) {
    if (os == from) return std::string(to);
  }
  if (os == "Windows CE") return "Windows CE";
  const auto underscores_to_dots = [](std::string_view v) {
    std::string out(v);
    std::ranges::replace(out, '_', '.');
    return out;
  };
  if (const auto mac = mac_os_x_version(os)) return mac->empty() ? "OS X" : "OS X " + underscores_to_dots(*mac);
  if (const auto ios = ios_version(os)) return "iOS " + underscores_to_dots(*ios);
  if (const auto cros = chrome_os_version(os)) return "ChromeOS " + std::string(*cros);
  return std::string(os);
}

// `os` of Chrome, Vivaldi, WechatBrowser and AppleCoreMedia.
std::optional<std::string> chrome_os(const std::vector<std::string>& comment) {
  const auto at = [&](std::size_t i) -> const std::string* { return i < comment.size() ? &comment[i] : nullptr; };
  const std::string* pick = nullptr;
  if (at(0) && contains(*at(0), "Windows NT")) {
    pick = at(0);
  } else if (!at(2) || (at(1) && contains(*at(1), "Android"))) {
    pick = at(1);
  } else {
    pick = at(2);
  }
  if (pick == nullptr) return std::nullopt;
  return normalize_os(*pick);
}

}  // namespace

Agent parse(std::string_view user_agent) {
  std::string_view rest = compat::strip(user_agent).empty() ? kDefaultUserAgent : user_agent;
  std::vector<Product> products;
  while (auto matched = match_product(rest)) {
    products.push_back(std::move(matched->second));
    rest = compat::strip(rest.substr(matched->first));
  }
  const auto extends = [&](Kind kind) {
    const Product* first = products.empty() ? nullptr : &products.front();
    const Product* last = products.empty() ? nullptr : &products.back();
    const auto any = [&](std::string_view name) {
      return std::ranges::any_of(products, [&](const Product& p) { return p.name == name; });
    };
    switch (kind) {
      case Kind::Base: return true;
      case Kind::Edge: return last != nullptr && last->name == "Edge";
      case Kind::InternetExplorer: {
        if (first == nullptr || !first->comment) return false;
        const std::string* second = comment_at(*first, 1);
        if (second != nullptr && contains(*second, "MSIE")) return true;
        const auto joined = joined_comment(*first);
        return joined && trident_rv(*joined);
      }
      case Kind::Opera: return (first != nullptr && first->name == "Opera") || (last != nullptr && last->name == "OPR");
      case Kind::WechatBrowser:
        return std::ranges::any_of(products, [](const Product& p) { return contains_ignoring_case(p.name, "micromessenger"); });
      case Kind::Vivaldi: return any("Vivaldi");
      case Kind::Chrome: return any("Chrome") || any("CriOS");
      case Kind::ITunes: return any("iTunes");
      case Kind::PlayStation: {
        const std::string* c = first != nullptr ? comment_at(*first, 0) : nullptr;
        return c != nullptr && (contains(*c, "PLAYSTATION 3") || contains(*c, "PlayStation Vita") || contains(*c, "PlayStation 4"));
      }
      case Kind::PodcastAddict:
        return products.size() >= 3 && products[0].name == "Podcast" && products[1].name == "Addict" && products[2].name == "-";
      case Kind::Webkit:
        return std::ranges::any_of(products, [](const Product& p) {
          if (same_ignoring_case(p.name, "applewebkit")) return true;
          return p.comment && std::ranges::any_of(*p.comment, [](const std::string& c) { return webkit_comment_version(c).has_value(); });
        });
      case Kind::Gecko: return first != nullptr && first->name == "Mozilla";
      case Kind::WindowsMediaPlayer: {
        const std::string_view fv = first != nullptr ? std::string_view(first->version.str()) : std::string_view{};
        const bool listed = first != nullptr && (fv == "4.1.0.3856" || fv == "7.10.0.3059" || fv == "7.0.0.1956");
        return std::ranges::any_of(products, [&](const Product& p) {
          return (p.name == "NSPlayer" || p.name == "Windows-Media-Player" || p.name == "WMFSDK") && !listed;
        });
      }
      case Kind::AppleCoreMedia: return any("AppleCoreMedia");
      case Kind::Libavformat: {
        const bool nsplayer_old = first != nullptr && first->version.str() == "4.1.0.3856";
        return std::ranges::any_of(products, [&](const Product& p) { return p.name == "Lavf" || (p.name == "NSPlayer" && nsplayer_old); });
      }
    }
    return false;
  };
  static constexpr std::array<Kind, 14> kAll = {Kind::Edge,      Kind::InternetExplorer, Kind::Opera,         Kind::WechatBrowser,
                                                Kind::Vivaldi,   Kind::Chrome,           Kind::ITunes,        Kind::PlayStation,
                                                Kind::PodcastAddict, Kind::Webkit,       Kind::Gecko,         Kind::WindowsMediaPlayer,
                                                Kind::AppleCoreMedia, Kind::Libavformat};
  Kind kind = Kind::Base;
  for (Kind candidate : kAll) {
    if (extends(candidate)) {
      kind = candidate;
      break;
    }
  }
  return Agent(kind, std::move(products));
}

// ---- Agent -------------------------------------------------------------------------------------

namespace {
using OptString = std::optional<std::string>;

bool any_comment_contains(const std::vector<std::string>& comment, std::string_view needle) {
  return std::ranges::any_of(comment, [&](const std::string& c) { return contains(c, needle); });
}
}  // namespace

const Product* Agent::first() const {
  return products_.empty() ? nullptr : &products_.front();
}
const Product* Agent::last() const {
  return products_.empty() ? nullptr : &products_.back();
}

const Product* Agent::detect_product(std::string_view name) const {
  for (const Product& p : products_) {
    if (same_ignoring_case(p.name, name)) return &p;
  }
  return nullptr;
}

// Most classes use the first product; the WebKit based ones use the first product with a comment that is not empty.
const Product* Agent::application() const {
  switch (kind_) {
    case Kind::Chrome:
    case Kind::Vivaldi:
    case Kind::Webkit:
    case Kind::ITunes:
    case Kind::AppleCoreMedia:
      for (const Product& p : products_) {
        if (p.comment && !p.comment->empty()) return &p;
      }
      return nullptr;
    default: return first();
  }
}

const std::vector<std::string>* Agent::application_comment() const {
  const Product* a = application();
  return a != nullptr && a->comment ? &*a->comment : nullptr;
}

std::optional<Version> Agent::base_version() const {
  const Product* a = application();
  if (a == nullptr) return std::nullopt;
  return a->version;
}

bool Agent::is_bot() const {
  const Product* app = application();
  if (app == nullptr) return true;
  for (const Product& p : products_) {
    if (p.comment && std::ranges::any_of(*p.comment, [](const std::string& c) { return contains_ignoring_case(c, "bot"); })) return true;
  }
  return detect_product("Chrome-Lighthouse") != nullptr || contains(app->name, "bot");
}

// ---- browser

OptString Agent::playstation_browser() const {
  const auto* comment = application_comment();
  if (comment == nullptr || comment->empty()) return std::nullopt;
  const std::string& c = comment->front();
  if (contains(c, "PLAYSTATION 3")) return "PS3 Internet Browser";
  if (last() != nullptr && last()->name == "Silk") return "Silk";
  if (contains(c, "PlayStation 4")) return "PS4 Internet Browser";
  return std::nullopt;
}

std::string Agent::webkit_browser() const {
  const auto os = webkit_os();
  if (os && contains(*os, "Android")) return "Android";
  if (webkit_platform() == OptString("BlackBerry")) return "BlackBerry";
  return "Safari";
}

std::string Agent::gecko_browser() const {
  for (std::string_view name : {"PaleMoon", "Firefox", "Camino", "Iceweasel", "Seamonkey"}) {
    if (detect_product(name) != nullptr) return std::string(name);
  }
  return first() != nullptr ? first()->name : std::string{};
}

Rb<OptString> Agent::try_browser() const {
  switch (kind_) {
    case Kind::Base: {
      const Product* a = application();
      return a != nullptr ? OptString(a->name) : std::nullopt;
    }
    case Kind::Edge: return OptString("Edge");
    case Kind::InternetExplorer: return OptString("Internet Explorer");
    case Kind::Opera: return OptString("Opera");
    case Kind::WechatBrowser: return OptString("Wechat Browser");
    case Kind::Vivaldi: return OptString("Vivaldi");
    case Kind::Chrome: return OptString(detect_product("Iron") != nullptr ? "Iron" : "Chrome");
    case Kind::ITunes: return OptString("iTunes");
    case Kind::PlayStation: return playstation_browser();
    case Kind::PodcastAddict: return OptString("Podcast Addict");
    case Kind::Webkit: return OptString(webkit_browser());
    case Kind::Gecko: return OptString(gecko_browser());
    case Kind::WindowsMediaPlayer: return OptString("Windows Media Player");
    case Kind::AppleCoreMedia: return OptString("AppleCoreMedia");
    case Kind::Libavformat: return OptString("libavformat");
  }
  return std::nullopt;
}

std::string Agent::browser() const {
  auto b = try_browser();
  return b && *b ? **b : std::string{};
}

// ---- version

bool Agent::opera_mini() const {
  if (first() == nullptr) return false;
  const auto joined = joined_comment(*first());
  return joined && contains(*joined, "Opera Mini");
}

std::optional<Version> Agent::opera_version() const {
  if (opera_mini()) {
    // `rescue Version.new` covers a comment without "Opera Mini/<version>".
    std::optional<std::string_view> version;
    if (const auto* comment = application_comment()) {
      for (const std::string& c : *comment) {
        if (contains(c, "Opera Mini")) {
          version = capture_after(c, "Opera Mini/");
          break;
        }
      }
    }
    return Version(version.value_or(""));
  }
  if (const Product* p = detect_product("Version")) return p->version;
  if (const Product* p = detect_product("OPR")) return p->version;
  return base_version();
}

std::optional<Version> Agent::playstation_version() const {
  const auto os = playstation_os();
  if (!os) return std::nullopt;
  const auto after = [&](std::string_view marker) {
    const auto parts = ruby_split(*os, marker);
    return Version(parts.empty() ? std::string_view{} : std::string_view(parts.back()));
  };
  if (playstation_browser() == OptString("Silk")) return last() != nullptr ? std::optional<Version>(last()->version) : std::nullopt;
  const auto platform = playstation_platform();
  if (platform == OptString("PlayStation 3")) return after("PLAYSTATION 3 ");
  if (platform == OptString("PlayStation 4")) return after("PlayStation 4 ");
  if (platform == OptString("PlayStation Vita")) return after("PlayStation Vita ");
  return std::nullopt;
}

std::optional<Version> Agent::webkit() const {
  for (const Product& p : products_) {
    if (same_ignoring_case(p.name, "applewebkit")) return p.version;
  }
  for (const Product& p : products_) {
    if (!p.comment) continue;
    for (const std::string& c : *p.comment) {
      if (const auto v = webkit_comment_version(c)) return Version(*v);
    }
  }
  return std::nullopt;
}

Version Agent::webkit_version() const {
  if (const Product* p = detect_product("Version")) return p->version;
  if (const auto os = webkit_os()) {
    if (const auto ios = capture_after(*os, "iOS "); ios && webkit_browser() == "Safari") {
      std::string v(*ios);
      std::ranges::replace(v, '_', '.');
      return Version(v);
    }
  }
  const auto w = webkit();
  const std::string build = w ? w->str() : std::string{};
  return Version(webkit_build_version(build).value_or(""));
}

Rb<std::optional<Version>> Agent::try_version() const {
  switch (kind_) {
    case Kind::Base:
    case Kind::WindowsMediaPlayer:
    case Kind::AppleCoreMedia: return base_version();
    case Kind::Edge:
    case Kind::Vivaldi: return last() != nullptr ? std::optional<Version>(last()->version) : std::nullopt;
    case Kind::InternetExplorer: {
      const Product* a = application();
      const std::string joined = a != nullptr ? joined_comment(*a).value_or("") : std::string{};
      return std::optional<Version>(Version(ie_version(joined).value_or("")));
    }
    case Kind::Opera: return opera_version();
    case Kind::WechatBrowser: {
      const Product* p = detect_product("MicroMessenger");
      if (p == nullptr) return std::unexpected(Raised{});
      return std::optional<Version>(p->version);
    }
    case Kind::Chrome: {
      const Product* p = detect_product("CriOs");
      if (p == nullptr) p = detect_product("chrome");
      if (p == nullptr) return std::unexpected(Raised{});
      return std::optional<Version>(p->version);
    }
    case Kind::ITunes: {
      const Product* p = detect_product("iTunes");
      if (p == nullptr) return std::unexpected(Raised{});
      return std::optional<Version>(p->version);
    }
    case Kind::PlayStation: return playstation_version();
    case Kind::PodcastAddict: return std::optional<Version>{};
    case Kind::Webkit: return std::optional<Version>(webkit_version());
    case Kind::Gecko: {
      const Product* p = detect_product(gecko_browser());
      if (p == nullptr) return std::unexpected(Raised{});
      if (p->version.is_nil()) return base_version();
      return std::optional<Version>(p->version);
    }
    case Kind::Libavformat:
      if (detect_product("NSPlayer") != nullptr) return std::optional<Version>{};
      return base_version();
  }
  return std::optional<Version>{};
}

Version Agent::version() const {
  auto v = try_version();
  return v && *v ? **v : Version();
}

// ---- platform

OptString Agent::webkit_platform() const {
  const auto* comment = application_comment();
  if (comment == nullptr) return std::nullopt;
  const std::string* first_comment = comment->empty() ? nullptr : &comment->front();
  if (first_comment != nullptr && contains(*first_comment, "Windows")) return "Windows";
  if (first_comment != nullptr && *first_comment == "BB10") return "BlackBerry";
  if (any_comment_contains(*comment, "Android")) return "Android";
  return first_comment != nullptr ? OptString(*first_comment) : std::nullopt;
}

OptString Agent::playstation_os() const {
  const auto* comment = application_comment();
  if (comment == nullptr) return std::nullopt;
  std::string out;
  for (std::size_t i = 0; i < comment->size(); ++i) {
    if (i > 0) out += ' ';
    out += (*comment)[i];
  }
  return out;
}

OptString Agent::playstation_platform() const {
  const auto os = playstation_os();
  if (!os) return std::nullopt;
  if (contains(*os, "PLAYSTATION 3")) return "PlayStation 3";
  if (contains(*os, "PlayStation 4")) return "PlayStation 4";
  if (contains(*os, "PlayStation Vita")) return "PlayStation Vita";
  return std::nullopt;
}

// `PodcastAddict#os`; `Raised` is the gem raising on a Dalvik or Mozilla product that has no comment.
Rb<OptString> Agent::podcast_addict_os() const {
  if (products_.size() <= 3) return OptString{};
  const Product& device = products_[3];
  if (device.name != "Dalvik" && device.name != "Mozilla") return OptString{};
  if (!device.comment) return std::unexpected(Raised{});
  const auto& comment = *device.comment;
  if (comment.size() > 3) return OptString(comment[2]);
  if (comment.size() == 3) return OptString("Android");
  return OptString{};
}

Rb<OptString> Agent::try_platform() const {
  const auto* comment = application_comment();
  const std::string* first_comment = comment != nullptr && !comment->empty() ? &comment->front() : nullptr;
  const auto any = [&](std::string_view needle) { return comment != nullptr && any_comment_contains(*comment, needle); };
  const auto first_or_none = [&]() -> OptString { return first_comment != nullptr ? OptString(*first_comment) : std::nullopt; };

  switch (kind_) {
    case Kind::Base:
    case Kind::Libavformat: return OptString{};
    case Kind::Edge:
    case Kind::InternetExplorer:
    case Kind::WindowsMediaPlayer: return OptString("Windows");
    case Kind::Opera:
    case Kind::AppleCoreMedia:
      if (comment == nullptr) return OptString{};
      if (first_comment != nullptr && contains(*first_comment, "Windows")) return OptString("Windows");
      return first_or_none();
    case Kind::WechatBrowser:
      if (comment == nullptr) return OptString{};
      if (first_comment != nullptr && contains(*first_comment, "iPhone")) return OptString("iPhone");
      if (any("Android")) return OptString("Android");
      return first_or_none();
    case Kind::Chrome:
    case Kind::Vivaldi:
      if (comment == nullptr) return OptString{};
      if (first_comment != nullptr && contains(*first_comment, "Windows")) return OptString("Windows");
      if (any("CrOS")) return OptString("ChromeOS");
      if (any("Android")) return OptString("Android");
      return first_or_none();
    case Kind::Webkit:
    case Kind::ITunes: return webkit_platform();
    case Kind::PlayStation: return playstation_platform();
    case Kind::PodcastAddict: {
      auto os = podcast_addict_os();
      if (!os || !*os) return std::unexpected(Raised{});
      return contains(**os, "Android") ? OptString("Android") : OptString{};
    }
    case Kind::Gecko:
      if (comment == nullptr) return OptString{};
      if (first_comment != nullptr && (*first_comment == "compatible" || *first_comment == "Mobile")) return OptString{};
      if (first_comment != nullptr && first_comment->starts_with("Windows ")) return OptString("Windows");
      return first_or_none();
  }
  return OptString{};
}

// ---- os

OptString Agent::webkit_os() const {
  const auto* comment = application_comment();
  if (comment == nullptr) return std::nullopt;
  const auto at = [&](std::size_t i) -> const std::string* { return i < comment->size() ? &(*comment)[i] : nullptr; };
  if (at(0) != nullptr && contains(*at(0), "Windows NT")) return normalize_os(*at(0));
  if (at(2) == nullptr || (at(1) != nullptr && contains(*at(1), "Android"))) {
    return at(1) != nullptr ? OptString(normalize_os(*at(1))) : std::nullopt;
  }
  for (const std::string& c : *comment) {
    if (ios_version(c)) return normalize_os(c);
  }
  return normalize_os(*at(2));
}

// `ITunes#full_os`: the comment was cut at the first ")", so "(Build 7601" gets it back.
OptString Agent::itunes_full_os() const {
  const auto* comment = application_comment();
  if (comment == nullptr || comment->size() <= 1) return std::nullopt;
  const std::string& full_os = (*comment)[1];
  // The last 11 characters are "(Build " and 4 digits. The test text is ASCII there, so bytes are enough.
  const std::size_t n = full_os.size();
  const bool reopened = n >= 11 && std::string_view(full_os).substr(n - 11, 7) == "(Build " &&
                        std::ranges::all_of(std::string_view(full_os).substr(n - 4), is_digit);
  return reopened ? full_os + ")" : full_os;
}

OptString Agent::itunes_os() const {
  const auto* comment = application_comment();
  const bool windows = comment != nullptr && !comment->empty() && contains(comment->front(), "Windows");
  if (!windows) return webkit_os();
  const std::string full_os = itunes_full_os().value_or("");
  for (std::string_view name : {"Windows 8.1", "Windows 8", "Windows 7", "Windows Vista", "Windows XP"}) {
    if (contains(full_os, name)) return std::string(name);
  }
  return "Windows";
}

OptString Agent::gecko_os() const {
  const auto* comment = application_comment();
  if (comment == nullptr) return std::nullopt;
  const std::string* first_comment = comment->empty() ? nullptr : &comment->front();
  std::size_t index = 1;
  if (comment->size() > 1 && (*comment)[1] == "U") {
    index = 2;
  } else if (first_comment != nullptr && (first_comment->starts_with("Windows ") || first_comment->starts_with("Android"))) {
    index = 0;
  } else if (first_comment != nullptr && *first_comment == "Mobile") {
    return std::nullopt;
  }
  return index < comment->size() ? OptString(normalize_os((*comment)[index])) : std::nullopt;
}

// `version.to_a[0]` compared with an integer: nil raises NoMethodError, a string ArgumentError.
Rb<std::uint64_t> Agent::windows_media_player_major() const {
  const auto version = base_version();
  if (!version) return std::unexpected(Raised{});
  const auto segments = version->to_a();
  if (segments.empty() || !segments[0].is_int) return std::unexpected(Raised{});
  std::uint64_t n = 0;
  for (char c : segments[0].text) {
    const auto d = static_cast<std::uint64_t>(c - '0');
    if (n > (UINT64_MAX - d) / 10) return UINT64_MAX;
    n = n * 10 + d;
  }
  return n;
}

Rb<std::string_view> Agent::windows_media_player_os() const {
  const auto major = windows_media_player_major();
  if (!major) return std::unexpected(Raised{});
  const auto segments = base_version().value_or(Version()).to_a();
  const auto part = [&](std::size_t i) -> std::optional<std::uint64_t> {
    if (i >= segments.size() || !segments[i].is_int || segments[i].text.size() > 18) return std::nullopt;
    return std::stoull(segments[i].text);
  };
  const auto in = [](std::optional<std::uint64_t> v, std::initializer_list<std::uint64_t> list) {
    return v && std::ranges::find(list, *v) != list.end();
  };
  const std::uint64_t m = *major;
  if (m <= 4) {
    if (in(part(3), {3564, 3925})) return "Windows 98";
    if (in(part(3), {3857})) return "Windows 9x";
    if (in(part(3), {3936})) return "Windows XP";
    if (in(part(3), {3938})) return "Windows 2000";
    return "Windows";
  }
  if (m == 7) return in(part(3), {3055}) ? "Windows 98" : "Windows";
  if (m == 8) return "Windows XP";
  if (m == 9 || m == 10) {
    if (in(part(3), {2980})) return "Windows 98/2000";
    if (in(part(3), {3268, 3367, 3270})) return "Windows 2000";
    if (in(part(3), {3802, 4503})) return "Windows XP";
    return "Windows";
  }
  if (m == 11 || m == 12) {
    const auto p = part(2);
    if (in(p, {9841, 9858, 9860, 9879})) return "Windows 10";
    if (in(p, {9651})) return "Windows Phone 8.1";
    if (in(p, {9600})) return "Windows 8.1";
    if (in(p, {9200})) return "Windows 8";
    if (in(p, {7600, 7601})) return "Windows 7";
    if (p && *p >= 6000 && *p <= 6002) return "Windows Vista";
    if (in(p, {5721})) return "Windows XP";
    return "Windows";
  }
  return "Windows";
}

Rb<OptString> Agent::try_os() const {
  switch (kind_) {
    case Kind::Base:
    case Kind::Libavformat: return OptString{};
    case Kind::Edge: {
      const auto find_windows = [&]() -> std::string_view {
        for (const Product& p : products_) {
          if (!p.comment) continue;
          for (const std::string& c : *p.comment) {
            if (const auto w = windows_os(c)) return *w;
          }
        }
        return {};
      };
      return OptString(normalize_os(find_windows()));
    }
    case Kind::InternetExplorer: {
      const Product* a = application();
      const std::string joined = a != nullptr ? joined_comment(*a).value_or("") : std::string{};
      return OptString(normalize_os(windows_os(joined).value_or("")));
    }
    case Kind::Opera: {
      const auto* comment = application_comment();
      if (comment == nullptr) return OptString{};
      if (!comment->empty() && contains(comment->front(), "Windows")) return OptString(normalize_os(comment->front()));
      return comment->size() > 1 ? OptString((*comment)[1]) : OptString{};
    }
    case Kind::WechatBrowser:
    case Kind::Chrome:
    case Kind::Vivaldi:
    case Kind::AppleCoreMedia: {
      const auto* comment = application_comment();
      return comment != nullptr ? chrome_os(*comment) : OptString{};
    }
    case Kind::Webkit: return webkit_os();
    case Kind::ITunes: return itunes_os();
    case Kind::PlayStation: return playstation_os();
    case Kind::PodcastAddict: return podcast_addict_os();
    case Kind::Gecko: return gecko_os();
    case Kind::WindowsMediaPlayer: {
      auto os = windows_media_player_os();
      if (!os) return std::unexpected(Raised{});
      return OptString(std::string(*os));
    }
  }
  return OptString{};
}

Rb<bool> Agent::try_mobile() const {
  switch (kind_) {
    case Kind::Opera: return opera_mini();
    case Kind::PlayStation: return playstation_platform() == OptString("PlayStation Vita");
    case Kind::PodcastAddict: return true;
    case Kind::WindowsMediaPlayer: {
      auto os = windows_media_player_os();
      if (!os) return std::unexpected(Raised{});
      return *os == "Windows Phone 8" || *os == "Windows Phone 8.1";
    }
    default: break;
  }
  if (detect_product("Mobile") != nullptr) return true;
  for (const Product& p : products_) {
    if (p.comment && std::ranges::find(*p.comment, "Mobile") != p.comment->end()) return true;
  }
  const auto os = try_os();
  if (!os) return std::unexpected(Raised{});
  if (*os && contains(**os, "Android")) return true;
  const auto* comment = application_comment();
  return comment != nullptr && std::ranges::any_of(*comment, [](const std::string& c) { return c.starts_with("IEMobile"); });
}

}  // namespace campfire::app::ua
