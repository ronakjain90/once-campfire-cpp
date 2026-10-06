// Ruby: uri/rfc3986_parser.rb, uri/generic.rb, uri/mailto.rb. Rust: crates/richtext/src/uri.rs
#include "richtext/uri.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

namespace campfire::richtext {

namespace {

using Bytes = std::string_view;

bool is_alpha(unsigned char b) { return std::isalpha(b) != 0 && b < 0x80; }
bool is_digit(unsigned char b) { return b >= '0' && b <= '9'; }
bool is_hex(unsigned char b) { return is_digit(b) || (b >= 'a' && b <= 'f') || (b >= 'A' && b <= 'F'); }
bool is_alnum(unsigned char b) { return is_alpha(b) || is_digit(b); }

std::string ascii_lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

std::string ascii_upper(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return out;
}

std::optional<std::uint64_t> default_port(const Uri& uri) {
  if (!uri.scheme) {
    return std::nullopt;
  }
  const std::string scheme = ascii_upper(*uri.scheme);
  if (scheme == "HTTP" || scheme == "WS") return 80;
  if (scheme == "HTTPS" || scheme == "WSS") return 443;
  if (scheme == "FTP") return 21;
  if (scheme == "LDAP") return 389;
  if (scheme == "LDAPS") return 636;
  return std::nullopt;
}

// [!$&-.0-9;=A-Z_a-z~]: `&-.` covers & ' ( ) * + , - .
bool is_unreserved_or_sub(unsigned char b) {
  return b == '!' || b == '$' || (b >= '&' && b <= '.') || is_digit(b) || b == ';' || b == '=' ||
         (b >= 'A' && b <= 'Z') || b == '_' || (b >= 'a' && b <= 'z') || b == '~';
}

bool pct_at(Bytes s, std::size_t i) {
  return i + 2 < s.size() && s[i] == '%' && is_hex(static_cast<unsigned char>(s[i + 1])) &&
         is_hex(static_cast<unsigned char>(s[i + 2]));
}

// Consumes `(?:%\h\h|[class])*` possessively and returns the end index.
template <class Pred>
std::size_t take_while_class(Bytes s, std::size_t i, Pred class_fn) {
  while (true) {
    if (pct_at(s, i)) {
      i += 3;
    } else if (i < s.size() && class_fn(static_cast<unsigned char>(s[i]))) {
      ++i;
    } else {
      return i;
    }
  }
}

bool seg_char(unsigned char b) { return is_unreserved_or_sub(b) || b == ':' || b == '@' || b == '/'; }
bool seg_nc_char(unsigned char b) { return is_unreserved_or_sub(b) || b == '@'; }
bool fragment_char(unsigned char b) { return is_unreserved_or_sub(b) || b == ':' || b == '@' || b == '/' || b == '?'; }
bool userinfo_char(unsigned char b) { return is_unreserved_or_sub(b) || b == ':'; }

// The IP-literal alternative of HOST (a bracketed address). Returns its end.
std::optional<std::size_t> ip_literal(Bytes s, std::size_t i) {
  if (i >= s.size() || s[i] != '[') {
    return std::nullopt;
  }
  const std::size_t close = s.find(']', i);
  if (close == Bytes::npos) {
    return std::nullopt;
  }
  const std::string inner(s.substr(i + 1, close - i - 1));
  bool valid = false;
  if (!inner.empty() && (inner[0] == 'v' || inner[0] == 'V')) {
    const std::size_t dot = inner.find('.');
    if (dot != std::string::npos) {
      const std::string_view hex = std::string_view(inner).substr(1, dot - 1);
      const std::string_view rest = std::string_view(inner).substr(dot + 1);
      valid = !hex.empty() && std::all_of(hex.begin(), hex.end(), [](char c) { return is_hex(static_cast<unsigned char>(c)); }) &&
              !rest.empty() &&
              std::all_of(rest.begin(), rest.end(), [](char c) {
                return is_unreserved_or_sub(static_cast<unsigned char>(c)) || c == ':';
              });
    }
  } else if (inner.find('%') == std::string::npos) {
    unsigned char buffer[16];
    valid = inet_pton(AF_INET6, inner.c_str(), buffer) == 1;
  }
  if (!valid) {
    return std::nullopt;
  }
  return close + 1;
}

struct Tail {
  std::size_t hier_end = 0;
  std::optional<std::pair<std::size_t, std::size_t>> query;
  std::optional<std::pair<std::size_t, std::size_t>> fragment;
};

// Splits `(?:\?(?<query>[^#]*+))?(?:\#(?<fragment>FRAGMENT))?\z` off the end of the hier-part.
std::optional<Tail> parse_query_fragment_positions(Bytes s, std::size_t start) {
  Tail tail;
  std::size_t hier_end = start;
  while (hier_end < s.size() && s[hier_end] != '?' && s[hier_end] != '#') {
    ++hier_end;
  }
  tail.hier_end = hier_end;
  std::size_t i = hier_end;
  if (i < s.size() && s[i] == '?') {
    std::size_t q_end = i + 1;
    while (q_end < s.size() && s[q_end] != '#') {
      ++q_end;
    }
    tail.query = std::pair{i + 1, q_end};
    i = q_end;
  }
  if (i < s.size() && s[i] == '#') {
    if (take_while_class(s, i + 1, fragment_char) != s.size()) {
      return std::nullopt;
    }
    tail.fragment = std::pair{i + 1, s.size()};
    i = s.size();
  }
  if (i != s.size()) {
    return std::nullopt;
  }
  return tail;
}

struct Authority {
  std::optional<std::string> userinfo;
  std::string host;
  std::optional<std::uint64_t> port;
  std::size_t end = 0;
};

std::optional<Authority> parse_authority(std::string_view value, std::size_t start, std::size_t limit) {
  const Bytes s = value.substr(0, limit);
  std::size_t i = start;
  Authority authority;
  const std::size_t ui_end = take_while_class(s, i, userinfo_char);
  if (ui_end < s.size() && s[ui_end] == '@') {
    authority.userinfo = std::string(value.substr(i, ui_end - i));
    i = ui_end + 1;
  }
  const std::size_t host_end = ip_literal(s, i).value_or(take_while_class(s, i, is_unreserved_or_sub));
  authority.host = std::string(value.substr(i, host_end - i));
  std::size_t end = host_end;
  if (end < s.size() && s[end] == ':') {
    std::size_t digits_end = end + 1;
    while (digits_end < s.size() && is_digit(static_cast<unsigned char>(s[digits_end]))) {
      ++digits_end;
    }
    if (digits_end > end + 1) {
      std::uint64_t port = 0;
      bool overflow = false;
      for (std::size_t k = end + 1; k < digits_end; ++k) {
        const unsigned d = static_cast<unsigned>(s[k] - '0');
        if (port > (UINT64_MAX - d) / 10) {
          overflow = true;
          break;
        }
        port = port * 10 + d;
      }
      authority.port = overflow ? UINT64_MAX : port;
    }
    end = digits_end;
  }
  if (end < s.size() && s[end] != '/') {
    return std::nullopt;
  }
  authority.end = end;
  return authority;
}

std::optional<Uri> split_absolute(std::string_view value) {
  const Bytes s = value;
  if (s.empty() || !is_alpha(static_cast<unsigned char>(s[0]))) {
    return std::nullopt;
  }
  std::size_t i = 1;
  while (i < s.size() && (is_alnum(static_cast<unsigned char>(s[i])) || s[i] == '+' || s[i] == '-' || s[i] == '.')) {
    ++i;
  }
  if (i >= s.size() || s[i] != ':') {
    return std::nullopt;
  }
  Uri uri;
  uri.scheme = ascii_lower(value.substr(0, i));  // URI::Generic#set_scheme downcases it
  const std::size_t rest_start = i + 1;
  auto tail = parse_query_fragment_positions(s, rest_start);
  if (!tail) {
    return std::nullopt;
  }
  const Bytes hier = s.substr(rest_start, tail->hier_end - rest_start);
  if (tail->query) uri.query = std::string(value.substr(tail->query->first, tail->query->second - tail->query->first));
  if (tail->fragment) {
    uri.fragment = std::string(value.substr(tail->fragment->first, tail->fragment->second - tail->fragment->first));
  }
  if (hier.starts_with("//")) {
    auto authority = parse_authority(value, rest_start + 2, tail->hier_end);
    if (!authority) {
      return std::nullopt;
    }
    uri.userinfo = authority->userinfo;
    uri.host = authority->host;
    uri.port = authority->port;
    uri.path = std::string(value.substr(authority->end, tail->hier_end - authority->end));
    // path-abempty: (?:/seg*)?
    if (authority->end < tail->hier_end &&
        (s[authority->end] != '/' || take_while_class(s, authority->end, seg_char) != tail->hier_end)) {
      return std::nullopt;
    }
  } else if (hier.starts_with("/")) {
    // path-absolute: /((?!/)seg++)?
    if (take_while_class(s, rest_start, seg_char) != tail->hier_end) {
      return std::nullopt;
    }
    uri.path = std::string(value.substr(rest_start, tail->hier_end - rest_start));
  } else if (!hier.empty()) {
    // path-rootless becomes the opaque part, with the query folded back in.
    if (take_while_class(s, rest_start, seg_char) != tail->hier_end) {
      return std::nullopt;
    }
    std::string opaque(value.substr(rest_start, tail->hier_end - rest_start));
    if (uri.query) {
      opaque.push_back('?');
      opaque += *uri.query;
      uri.query.reset();
    }
    uri.opaque = std::move(opaque);
  } else {
    uri.path = std::string();
  }
  return uri;
}

// RFC3986_relative_ref: only validity matters, since a relative reference is never HTTP.
std::optional<Uri> split_relative(std::string_view value) {
  const Bytes s = value;
  auto tail = parse_query_fragment_positions(s, 0);
  if (!tail) {
    return std::nullopt;
  }
  const Bytes hier = s.substr(0, tail->hier_end);
  bool valid = false;
  if (hier.starts_with("//")) {
    auto a = parse_authority(value, 2, tail->hier_end);
    valid = a && take_while_class(s, a->end, seg_char) == tail->hier_end;
  } else if (hier.starts_with("/") || hier.empty()) {
    valid = take_while_class(s, 0, seg_char) == tail->hier_end;
  } else {
    const std::size_t first = take_while_class(s, 0, seg_nc_char);
    valid = first > 0 && (first == tail->hier_end ||
                          (s[first] == '/' && take_while_class(s, first, seg_char) == tail->hier_end));
  }
  if (!valid) {
    return std::nullopt;
  }
  Uri uri;
  uri.path = std::string(value.substr(0, tail->hier_end));
  if (tail->query) uri.query = std::string(value.substr(tail->query->first, tail->query->second - tail->query->first));
  if (tail->fragment) {
    uri.fragment = std::string(value.substr(tail->fragment->first, tail->fragment->second - tail->fragment->first));
  }
  return uri;
}

// URI::Generic#query=: rejects `%` followed by two non-hex characters and escapes the rest.
std::expected<std::string, UriError> escape_query(std::string_view query) {
  std::string cleaned;
  for (char c : query) {
    if (c != '\t' && c != '\r' && c != '\n') {
      cleaned.push_back(c);
    }
  }
  for (std::size_t i = 0; i + 2 < cleaned.size() + 0 && i + 3 <= cleaned.size(); ++i) {
    if (cleaned[i] == '%' && !is_hex(static_cast<unsigned char>(cleaned[i + 1])) &&
        !is_hex(static_cast<unsigned char>(cleaned[i + 2]))) {
      return std::unexpected(UriError::InvalidUri);
    }
  }
  std::string out;
  for (std::size_t i = 0; i < cleaned.size(); ++i) {
    const auto b = static_cast<unsigned char>(cleaned[i]);
    const bool escape = i + 2 < cleaned.size() && b == '%' && is_hex(static_cast<unsigned char>(cleaned[i + 1])) &&
                        is_hex(static_cast<unsigned char>(cleaned[i + 2]));
    if (escape || b == '!' || (b >= '$' && b <= '&') || (b >= '(' && b <= ';') || b == '=' || (b >= '?' && b <= '_') ||
        (b >= 'a' && b <= '~')) {
      out.push_back(static_cast<char>(b));
    } else {
      char buffer[4];
      std::snprintf(buffer, sizeof buffer, "%%%02X", b);
      out += buffer;
    }
  }
  return out;
}

// /\A(?:[^@,;]+@[^@,;]+(?:\z|[,;]))*\z/
bool mailto_to_valid(std::string_view to) {
  const auto special = [](char c) { return c == '@' || c == ',' || c == ';'; };
  std::size_t i = 0;
  while (i < to.size()) {
    const std::size_t start = i;
    while (i < to.size() && !special(to[i])) ++i;
    if (i == start || i >= to.size() || to[i] != '@') return false;
    ++i;
    const std::size_t domain = i;
    while (i < to.size() && !special(to[i])) ++i;
    if (i == domain) return false;
    if (i < to.size()) {
      if (to[i] == '@') return false;
      ++i;
    }
  }
  return true;
}

// The initializers of the scheme classes that URI.for picks and that can raise.
std::expected<void, UriError> check_scheme_class(const Uri& uri) {
  if (!uri.scheme) {
    return {};
  }
  const std::string scheme = ascii_upper(*uri.scheme);
  if (scheme == "MAILTO") {
    std::optional<std::string> opaque = uri.opaque;
    if (!opaque && uri.query) {
      opaque = "?" + *uri.query;
    }
    if (!opaque) {
      return std::unexpected(UriError::InvalidComponent);
    }
    const std::string_view to = std::string_view(*opaque).substr(0, opaque->find('?'));
    return mailto_to_valid(to) ? std::expected<void, UriError>{} : std::unexpected(UriError::InvalidComponent);
  }
  if (scheme == "LDAP" || scheme == "LDAPS") {
    return (uri.fragment || !uri.path) ? std::unexpected(UriError::InvalidUri) : std::expected<void, UriError>{};
  }
  if (scheme == "FTP") {
    return !uri.path ? std::unexpected(UriError::InvalidUri) : std::expected<void, UriError>{};
  }
  return {};
}

bool is_ascii(std::string_view s) {
  return std::all_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

}  // namespace

bool Uri::is_http() const {
  if (!scheme) {
    return false;
  }
  const std::string s = ascii_lower(*scheme);
  return s == "http" || s == "https";
}

std::string Uri::to_s() const {
  std::string s;
  if (scheme) {
    s += *scheme;
    s.push_back(':');
  }
  if (opaque) {
    s += *opaque;
  } else {
    if (host || (scheme && (*scheme == "file" || *scheme == "postgres"))) {
      s += "//";
    }
    if (userinfo) {
      s += *userinfo;
      s.push_back('@');
    }
    if (host) {
      s += *host;
    }
    if (port && port != default_port(*this)) {
      s.push_back(':');
      s += std::to_string(*port);
    }
    s += path.value_or("");
    if (query) {
      s.push_back('?');
      s += *query;
    }
  }
  if (fragment) {
    s.push_back('#');
    s += *fragment;
  }
  return s;
}

std::expected<Uri, UriError> parse_uri(std::string_view value) {
  if (!is_ascii(value)) {
    return std::unexpected(UriError::InvalidUri);
  }
  std::optional<Uri> parsed = split_absolute(value);
  if (!parsed) {
    parsed = split_relative(value);
  }
  if (!parsed) {
    return std::unexpected(UriError::InvalidUri);
  }
  Uri uri = std::move(*parsed);
  // URI::Generic#initialize assigns the query through `query=`, which rejects bad escapes.
  if (uri.query) {
    auto escaped = escape_query(*uri.query);
    if (!escaped) {
      return std::unexpected(escaped.error());
    }
    uri.query = std::move(*escaped);
  }
  if (!uri.port) {
    uri.port = default_port(uri);
  }
  if (auto checked = check_scheme_class(uri); !checked) {
    return std::unexpected(checked.error());
  }
  return uri;
}

}  // namespace campfire::richtext
