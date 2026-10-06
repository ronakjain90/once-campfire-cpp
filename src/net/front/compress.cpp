// Thruster's compression. Rust: crates/kit/src/front/compression.rs.
#include "net/front/compress.hpp"

#include <libdeflate.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace campfire::net::front {

namespace {

constexpr int kGzipLevel = 6;
constexpr int kZstdLevel = 1;  // zstd.SpeedFastest
constexpr std::size_t kJitterBuffer = 64 << 10;

std::string_view trim(std::string_view text) noexcept {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

std::string lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// Rust `parse::<f64>()`, with 0 for text that is not a number, then clamped to 0..1.
double parse_quality(std::string_view text) noexcept {
  const std::string copy(text);
  if (copy.empty()) return 0.0;
  char* end = nullptr;
  const double value = std::strtod(copy.c_str(), &end);
  if (end != copy.c_str() + copy.size() || std::isnan(value)) return 0.0;
  return std::clamp(value, 0.0, 1.0);
}

// `parseEncodingQValue`: the quality of the first listed coding with this name. 1 without `q`.
double quality(std::string_view header, std::string_view encoding) {
  header = trim(header);
  std::size_t begin = 0;
  while (begin <= header.size()) {
    std::size_t end = header.find(',', begin);
    if (end == std::string_view::npos) end = header.size();
    std::string_view part = header.substr(begin, end - begin);
    begin = end + 1;
    const std::size_t semicolon = part.find(';');
    const std::string coding = lower(trim(part.substr(0, semicolon)));
    if (coding != encoding) continue;
    double q = 1.0;
    while (semicolon != std::string_view::npos) {
      std::string_view rest = part.substr(semicolon + 1);
      std::size_t at = 0;
      while (at <= rest.size()) {
        std::size_t next = rest.find(';', at);
        if (next == std::string_view::npos) next = rest.size();
        std::string_view piece = trim(rest.substr(at, next - at));
        if (piece.starts_with("q=")) q = parse_quality(piece.substr(2));
        at = next + 1;
      }
      break;
    }
    return q;
  }
  return 0.0;
}

void put_u32_le(std::string& out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

std::string gzip_encode(std::string_view body, std::string_view comment, std::uint32_t mtime = 0, char os = '\xff') {
  libdeflate_compressor* compressor = libdeflate_alloc_compressor(kGzipLevel);
  std::string out;
  // Header: ID1 ID2 CM FLG MTIME(4) XFL OS. FLG bit 4 is FCOMMENT. OS 255: unknown (flate2).
  const bool has_comment = !comment.empty();
  out.append({'\x1f', '\x8b', '\x08', static_cast<char>(has_comment ? 0x10 : 0x00), 0, 0, 0, 0, 0, os});
  for (int i = 0; i < 4; ++i) out[static_cast<std::size_t>(4 + i)] = static_cast<char>((mtime >> (8 * i)) & 0xFF);
  if (has_comment) {
    out.append(comment);
    out.push_back('\0');
  }
  const std::size_t header = out.size();
  const std::size_t bound = libdeflate_deflate_compress_bound(compressor, body.size());
  out.resize(header + bound);
  const std::size_t n = libdeflate_deflate_compress(compressor, body.data(), body.size(), out.data() + header, bound);
  libdeflate_free_compressor(compressor);
  out.resize(header + n);
  put_u32_le(out, libdeflate_crc32(0, body.data(), body.size()));
  put_u32_le(out, static_cast<std::uint32_t>(body.size()));
  return out;
}

std::string zstd_encode(std::string_view body, std::string_view jitter) {
  std::string out(ZSTD_compressBound(body.size()), '\0');
  const std::size_t n = ZSTD_compress(out.data(), out.size(), body.data(), body.size(), kZstdLevel);
  out.resize(ZSTD_isError(n) != 0U ? 0 : n);
  if (!jitter.empty()) {
    // The jitter goes in a skippable frame after the data (RFC 8878, 3.1.2).
    put_u32_le(out, 0x184D2A50U);
    put_u32_le(out, static_cast<std::uint32_t>(jitter.size()));
    out.append(jitter);
  }
  return out;
}

}  // namespace

std::string gzip_member(std::string_view body, std::uint32_t mtime) {
  return gzip_encode(body, {}, mtime, '\x03');
}

Encoding select_encoding(Method method, std::string_view accept_encoding) noexcept {
  if (method == Method::Head || accept_encoding.empty()) return Encoding::None;
  const double gzip = quality(accept_encoding, "gzip");
  const double zstd = quality(accept_encoding, "zstd");
  const bool has_gzip = gzip > 0.0;
  const bool has_zstd = zstd > 0.0;
  if (!has_gzip && !has_zstd) return Encoding::None;
  if (has_gzip && !has_zstd) return Encoding::Gzip;
  if (!has_gzip) return Encoding::Zstd;
  return gzip > zstd ? Encoding::Gzip : Encoding::Zstd;
}

bool content_type_filter(std::string_view content_type) {
  const std::string type = lower(trim(content_type));
  if (type.empty()) return true;
  static constexpr std::array<std::string_view, 8> kContains{"compress", "zip",  "snappy", "lzma",
                                                             "xz",       "zstd", "brotli", "stuffit"};
  static constexpr std::array<std::string_view, 13> kPrefix{
      "video/",     "audio/",    "image/jp",   "image/jpeg", "image/jpg",  "image/png", "image/apng",
      "image/webp", "image/gif", "image/avif", "image/heic", "image/heif", "image/jxl"};
  for (const std::string_view s : kContains) {
    if (type.find(s) != std::string::npos) return false;
  }
  for (const std::string_view p : kPrefix) {
    if (type.starts_with(p)) return false;
  }
  return true;
}

std::string_view detect_content_type(std::string_view data) noexcept {
  data = data.substr(0, 512);
  std::string_view trimmed = data;
  while (!trimmed.empty() && (trimmed.front() == '\t' || trimmed.front() == '\n' || trimmed.front() == '\f' ||
                              trimmed.front() == '\r' || trimmed.front() == ' ')) {
    trimmed.remove_prefix(1);
  }
  static constexpr std::array<std::string_view, 17> kHtml{
      "<!DOCTYPE HTML", "<HTML",  "<HEAD", "<SCRIPT", "<IFRAME", "<H1", "<DIV", "<FONT", "<TABLE", "<A",
      "<STYLE",         "<TITLE", "<B",    "<BODY",   "<BR",     "<P",  "<!--"};
  for (const std::string_view signature : kHtml) {
    if (trimmed.size() > signature.size() && iequals(trimmed.substr(0, signature.size()), signature) &&
        (trimmed[signature.size()] == ' ' || trimmed[signature.size()] == '>')) {
      return "text/html; charset=utf-8";
    }
  }
  if (trimmed.starts_with("<?xml")) return "text/xml; charset=utf-8";
  struct Exact {
    std::string_view signature;
    std::string_view type;
  };
  static constexpr std::array<Exact, 9> kExact{{{"%PDF-", "application/pdf"},
                                                {"%!PS-Adobe-", "application/postscript"},
                                                {"\xFE\xFF", "text/plain; charset=utf-16be"},
                                                {"\xFF\xFE", "text/plain; charset=utf-16le"},
                                                {"\xEF\xBB\xBF", "text/plain; charset=utf-8"},
                                                {"GIF87a", "image/gif"},
                                                {"GIF89a", "image/gif"},
                                                {"\x89PNG\x0D\x0A\x1A\x0A", "image/png"},
                                                {"\xFF\xD8\xFF", "image/jpeg"}}};
  for (const Exact& e : kExact) {
    if (data.starts_with(e.signature)) return e.type;
  }
  if (data.size() >= 14 && data.substr(0, 4) == "RIFF" && data.substr(8, 6) == "WEBPVP") return "image/webp";
  if (data.starts_with("\x1F\x8B\x08")) return "application/x-gzip";
  if (data.starts_with("PK\x03\x04")) return "application/zip";
  for (const char ch : data) {
    const auto b = static_cast<unsigned char>(ch);
    if (b <= 0x08 || b == 0x0B || (b >= 0x0E && b <= 0x1A) || (b >= 0x1C && b <= 0x1F))
      return "application/octet-stream";
  }
  return "text/plain; charset=utf-8";
}

std::uint32_t crc32c(std::string_view data) noexcept {
  static const auto table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t crc = i;
      for (int k = 0; k < 8; ++k) crc = (crc & 1U) != 0U ? (crc >> 1) ^ 0x82F63B78U : crc >> 1;
      t[i] = crc;
    }
    return t;
  }();
  std::uint32_t crc = ~0U;
  for (const char c : data) crc = table[(crc ^ static_cast<unsigned char>(c)) & 0xFFU] ^ (crc >> 8);
  return ~crc;
}

bool has_user_specific_request_headers(const Request& request) noexcept {
  for (const std::string_view name : {"cookie", "authorization", "x-csrf-token"}) {
    if (!request.header(name).empty()) return true;
  }
  return false;
}

Compression::Compression(std::int64_t jitter, bool disable_on_auth) : disable_on_auth_(disable_on_auth) {
  if (jitter > 0) {
    const auto n = static_cast<std::size_t>(jitter);
    std::string padding;
    for (std::size_t i = 0; i < 1 + n / 8; ++i) padding += "Padding-";
    jitter_ = padding.substr(0, n + 1);
  }
}

Negotiation Compression::negotiate(const Request& request) const noexcept {
  Negotiation n;
  n.encoding = select_encoding(request.method, request.header("accept-encoding"));
  n.user_specific_request = disable_on_auth_ && has_user_specific_request_headers(request);
  return n;
}

std::string_view Compression::jitter_for(std::string_view body) const noexcept {
  if (jitter_.empty()) return {};
  const std::uint32_t crc = crc32c(body.substr(0, kJitterBuffer));
  const std::uint32_t rng = ((crc << 19) | (crc >> 13)) ^ 0xab0755deU;
  const std::size_t len = 1 + (rng % (static_cast<std::uint32_t>(jitter_.size()) - 1));
  return std::string_view(jitter_).substr(0, len);
}

std::string Compression::encode(Encoding encoding, std::string_view body) const {
  const std::string_view jitter = jitter_for(body);
  return encoding == Encoding::Zstd ? zstd_encode(body, jitter) : gzip_encode(body, jitter);
}

}  // namespace campfire::net::front
