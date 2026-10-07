// Rails: app/models/webhook.rb, Action Dispatch Mime::Type. Rust: crates/campfire/src/integrations/webhook.rs.
#include "jobs/webhook.hpp"

#include <array>

#include "storage/filename.hpp"

namespace campfire::jobs::webhook {

namespace {

struct Registered {
  std::string_view key;
  std::string_view symbol;
  std::string_view to_s;
};

// `Mime::LOOKUP` in the reference app: Action Dispatch's registrations plus the ones of turbo-rails.
constexpr std::array<Registered, 49> kLookup{{
    {"text/html", "html", "text/html"},
    {"application/xhtml+xml", "html", "text/html"},
    {"text/plain", "text", "text/plain"},
    {"text/javascript", "js", "text/javascript"},
    {"application/javascript", "js", "text/javascript"},
    {"application/x-javascript", "js", "text/javascript"},
    {"text/css", "css", "text/css"},
    {"text/calendar", "ics", "text/calendar"},
    {"text/csv", "csv", "text/csv"},
    {"text/vcard", "vcf", "text/vcard"},
    {"text/vtt", "vtt", "text/vtt"},
    {"vtt", "vtt", "text/vtt"},
    {"text/markdown", "md", "text/markdown"},
    {"image/png", "png", "image/png"},
    {"image/jpeg", "jpeg", "image/jpeg"},
    {"image/gif", "gif", "image/gif"},
    {"image/bmp", "bmp", "image/bmp"},
    {"image/tiff", "tiff", "image/tiff"},
    {"image/svg+xml", "svg", "image/svg+xml"},
    {"image/webp", "webp", "image/webp"},
    {"video/mpeg", "mpeg", "video/mpeg"},
    {"audio/mpeg", "mp3", "audio/mpeg"},
    {"audio/ogg", "ogg", "audio/ogg"},
    {"audio/aac", "m4a", "audio/aac"},
    {"audio/mp4", "m4a", "audio/aac"},
    {"video/webm", "webm", "video/webm"},
    {"video/mp4", "mp4", "video/mp4"},
    {"font/otf", "otf", "font/otf"},
    {"font/ttf", "ttf", "font/ttf"},
    {"font/woff", "woff", "font/woff"},
    {"font/woff2", "woff2", "font/woff2"},
    {"application/xml", "xml", "application/xml"},
    {"text/xml", "xml", "application/xml"},
    {"application/x-xml", "xml", "application/xml"},
    {"application/rss+xml", "rss", "application/rss+xml"},
    {"application/atom+xml", "atom", "application/atom+xml"},
    {"application/x-yaml", "yaml", "application/x-yaml"},
    {"text/yaml", "yaml", "application/x-yaml"},
    {"multipart/form-data", "multipart_form", "multipart/form-data"},
    {"application/x-www-form-urlencoded", "url_encoded_form", "application/x-www-form-urlencoded"},
    {"application/json", "json", "application/json"},
    {"text/x-json", "json", "application/json"},
    {"application/jsonrequest", "json", "application/json"},
    {"application/problem+json", "json", "application/json"},
    {"application/pdf", "pdf", "application/pdf"},
    {"application/zip", "zip", "application/zip"},
    {"application/gzip", "gzip", "application/gzip"},
    {"application/x-gzip", "gzip", "application/gzip"},
    {"text/vnd.turbo-stream.html", "turbo_stream", "text/vnd.turbo-stream.html"},
}};

const Registered* registered(std::string_view key) {
  for (const Registered& item : kLookup) {
    if (item.key == key) return &item;
  }
  return nullptr;
}

bool name_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}
bool name_char(char c) {
  return name_start(c) || std::string_view("!#$&-^_.+").find(c) != std::string_view::npos;
}

// `Mime::Type::MIME_REGEXP` for a string without ";": `*/*`, or a name, "/", and a name or "*".
// A name is a letter or digit and at most 126 more of the allowed characters.
bool valid_mime(std::string_view s) {
  if (s == "*/*") return true;
  const auto name = [&s](std::size_t& at) {
    if (at >= s.size() || !name_start(s[at])) return false;
    std::size_t length = 1;
    ++at;
    while (at < s.size() && name_char(s[at]) && length < 127) {
      ++at;
      ++length;
    }
    return true;
  };
  std::size_t at = 0;
  if (!name(at) || at >= s.size() || s[at] != '/') return false;
  ++at;
  if (at < s.size() && s[at] == '*') {
    ++at;
  } else if (!name(at)) {
    return false;
  }
  while (at < s.size() && std::string_view(" \t\n\x0B\x0C\r").find(s[at]) != std::string_view::npos) ++at;
  return at == s.size();
}

}  // namespace

std::expected<MimeLookup, InvalidMimeType> mime_lookup(std::string_view string) {
  if (const Registered* found = registered(string))
    return MimeLookup{std::string(found->symbol), std::string(found->to_s)};
  std::string_view base = string.substr(0, string.find(';'));
  while (!base.empty() && std::string_view(" \t\n\x0B\x0C\r").find(base.back()) != std::string_view::npos) {
    base.remove_suffix(1);
  }
  while (!base.empty() && base.back() == '\0') base.remove_suffix(1);
  if (const Registered* found = registered(base))
    return MimeLookup{std::string(found->symbol), std::string(found->to_s)};
  if (valid_mime(base)) return MimeLookup{{}, std::string(base)};
  return std::unexpected(InvalidMimeType{std::string(base)});
}

std::expected<Reply, InvalidMimeType> reply_from(int status, const std::optional<std::string>& content_type,
                                                 std::string_view body) {
  Reply reply;
  if (!content_type) return reply;
  if (status == 200 && (*content_type == "text/html" || *content_type == "text/plain")) {
    reply.kind = Reply::Kind::Text;
    reply.text = storage::utf8_lossy(body);
    return reply;
  }
  auto mime = mime_lookup(*content_type);
  if (!mime) return std::unexpected(std::move(mime.error()));
  reply.kind = Reply::Kind::Attachment;
  reply.attachment.data = std::string(body);
  reply.attachment.filename = "attachment." + mime->symbol;
  reply.attachment.content_type = std::move(mime->content_type);
  return reply;
}

Reply timed_out(std::chrono::seconds after) {
  Reply reply;
  reply.kind = Reply::Kind::Text;
  reply.text = "Failed to respond within " + std::to_string(after.count()) + " seconds";
  return reply;
}

}  // namespace campfire::jobs::webhook
