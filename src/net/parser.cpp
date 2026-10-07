// HTTP/1.1 request head and body parser. Rust: crates/kit/src/front/conn.rs (hyper); Rails: Puma.
#include "net/parser.hpp"

#include <new>

namespace campfire::net {

namespace {

std::string_view trim(std::string_view text) noexcept {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

// True if the comma list `value` has the token `token` (ASCII case-insensitive).
bool has_token(std::string_view value, std::string_view token) noexcept {
  while (!value.empty()) {
    const std::size_t comma = value.find(',');
    const std::string_view part = trim(value.substr(0, comma));
    if (iequals(part, token)) return true;
    if (comma == std::string_view::npos) break;
    value.remove_prefix(comma + 1);
  }
  return false;
}

// The last token of a comma list.
std::string_view last_token(std::string_view value) noexcept {
  const std::size_t comma = value.rfind(',');
  return trim(comma == std::string_view::npos ? value : value.substr(comma + 1));
}

// Digits only, at most 19 of them (so the value fits in 64 bits).
bool parse_length(std::string_view text, std::uint64_t& out) noexcept {
  if (text.empty() || text.size() > 19) return false;
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<std::uint64_t>(c - '0');
  }
  out = value;
  return true;
}

// Splits the request target into path and query. Returns false if the form is not allowed.
bool split_target(std::string_view target, Request& request) noexcept {
  std::string_view rest = target;
  if (rest.empty()) return false;
  if (rest.front() != '/' && rest != "*") {
    // Absolute form ("http://host/path"): the server uses the origin form.
    std::size_t scheme = 0;
    if (rest.starts_with("http://")) {
      scheme = 7;
    } else if (rest.starts_with("https://")) {
      scheme = 8;
    } else {
      return false;
    }
    rest.remove_prefix(scheme);
    const std::size_t slash = rest.find_first_of("/?");
    if (slash == std::string_view::npos) {
      rest = "/";
    } else if (rest[slash] == '?') {
      // "http://host?x": the path is "/". The query is a view of the target.
      request.path = "/";
      request.query = rest.substr(slash + 1);
      return true;
    } else {
      rest.remove_prefix(slash);
    }
  }
  const std::size_t question = rest.find('?');
  if (question == std::string_view::npos) {
    request.path = rest;
    request.query = {};
  } else {
    request.path = rest.substr(0, question);
    request.query = rest.substr(question + 1);
  }
  return true;
}

}  // namespace

HeadResult parse_head(std::string_view data, std::size_t scanned, Arena& arena, const ParserLimits& limits,
                      ParsedHead& out) {
  phr_header raw[kMaxHeaders];
  std::size_t count = kMaxHeaders;
  const char* method = nullptr;
  const char* path = nullptr;
  std::size_t method_size = 0;
  std::size_t path_size = 0;
  int minor = 1;
  const int consumed = phr_parse_request(data.data(), data.size(), &method, &method_size, &path, &path_size, &minor,
                                         raw, &count, scanned < data.size() ? scanned : 0);
  if (consumed == -2) {
    if (data.size() > limits.max_head_bytes) return {HeadStatus::Error, 431};
    return {HeadStatus::NeedMore, 0};
  }
  if (consumed < 0) return {HeadStatus::Error, 400};
  if (static_cast<std::size_t>(consumed) > limits.max_head_bytes) return {HeadStatus::Error, 431};
  if (path_size > limits.max_target_bytes) return {HeadStatus::Error, 414};

  out = ParsedHead{};
  Request& request = out.request;
  request.method_text = {method, method_size};
  request.method = parse_method(request.method_text);
  request.target = {path, path_size};
  request.minor_version = minor;
  if (!split_target(request.target, request)) return {HeadStatus::Error, 400};

  auto* headers = static_cast<Header*>(arena.allocate(sizeof(Header) * (count == 0 ? 1 : count), alignof(Header)));
  bool has_length = false;
  bool has_encoding = false;
  bool close = false;
  bool keep_alive_token = false;
  bool chunked = false;
  std::uint64_t length = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if (raw[i].name == nullptr) return {HeadStatus::Error, 400};  // obsolete line folding
    const std::string_view name{raw[i].name, raw[i].name_len};
    const std::string_view value = trim({raw[i].value, raw[i].value_len});
    ::new (&headers[i]) Header{name, value};
    if (iequals(name, "content-length")) {
      std::uint64_t parsed = 0;
      if (!parse_length(value, parsed)) return {HeadStatus::Error, 400};
      if (has_length && parsed != length) return {HeadStatus::Error, 400};
      has_length = true;
      length = parsed;
    } else if (iequals(name, "transfer-encoding")) {
      if (has_encoding || !iequals(last_token(value), "chunked")) return {HeadStatus::Error, 400};
      has_encoding = true;
      chunked = true;
    } else if (iequals(name, "connection")) {
      close = close || has_token(value, "close");
      keep_alive_token = keep_alive_token || has_token(value, "keep-alive");
    } else if (iequals(name, "expect")) {
      out.expect_continue = iequals(value, "100-continue");
    }
  }
  if (chunked && (has_length || minor == 0)) return {HeadStatus::Error, 400};
  request.headers = std::span<const Header>(headers, count);
  request.keep_alive = !close && (minor >= 1 || keep_alive_token);
  out.head_size = static_cast<std::size_t>(consumed);
  out.expect_continue = out.expect_continue && minor >= 1;
  if (chunked) {
    out.body_kind = BodyKind::Chunked;
  } else if (has_length && length != 0) {
    out.body_kind = BodyKind::Length;
    out.content_length = length;
  }
  return {HeadStatus::Ok, 0};
}

ChunkedBody::ChunkedBody(std::uint64_t max_decoded) : max_decoded_(max_decoded) {
  decoder_.consume_trailer = 1;
}

ChunkedBody::Status ChunkedBody::feed(char* region, std::size_t& have) {
  std::size_t size = have - decoded_;
  const ssize_t result = phr_decode_chunked(&decoder_, region + decoded_, &size);
  if (result == -1) return Status::Error;
  decoded_ += size;
  have = decoded_;
  if (max_decoded_ != 0 && decoded_ > max_decoded_) return Status::TooLarge;
  if (result == -2) return Status::NeedMore;
  leftover_ = static_cast<std::size_t>(result);
  have = decoded_ + leftover_;
  return Status::Done;
}

}  // namespace campfire::net
