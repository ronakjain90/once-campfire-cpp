// MIME types, Accept parsing and format negotiation (Rails: Mime::Type,
// ActionDispatch::Http::MimeNegotiation, respond_to; Rust: crates/kit/src/format.rs).
#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace campfire::req {

struct Mime {
  std::string_view symbol;
  std::string_view string;
  std::vector<std::string_view> synonyms;
  std::vector<std::string_view> extensions;
  [[nodiscard]] bool is(std::string_view sym) const { return symbol == sym; }
};

using Format = const Mime*;  // nullptr is "no format"

namespace mime {
extern const Mime HTML, TEXT, JS, CSS, ICS, CSV, VCF, VTT, MD, PNG, JPEG, GIF, BMP, TIFF, SVG, WEBP, MPEG, MP3, OGG,
    M4A, WEBM, MP4, OTF, TTF, WOFF, WOFF2, XML, RSS, ATOM, YAML, MULTIPART_FORM, URL_ENCODED_FORM, JSON, PDF, ZIP, GZIP,
    TURBO_STREAM, ALL;
// Every registered type, in registration order (the order matters for text/* expansion).
[[nodiscard]] std::span<const Mime* const> registered();
}  // namespace mime

struct InvalidMimeType {
  std::string value;
};

// Mime[ext]: Mime::Type.lookup_by_extension.
[[nodiscard]] Format lookup_by_extension(std::string_view extension);
// Mime::Type.lookup. The value is null for a valid but unregistered type. An invalid type is an error.
[[nodiscard]] std::expected<Format, InvalidMimeType> lookup(std::string_view string);
// Mime::Type.parse(header) keeping registered types and */*, in preference order. An infinite
// q-value sorts first (or last), where Rails raises FloatDomainError and answers 500.
[[nodiscard]] std::expected<std::vector<Format>, InvalidMimeType> parse_accept(std::string_view header);

// What MimeNegotiation#formats reads.
struct NegotiationInput {
  std::string_view format_param;  // params[:format]; empty when absent
  bool has_format_param = false;
  std::string_view accept;
  bool has_accept = false;
  std::string_view content_type;
  std::string_view path;
  bool xhr = false;
};

// request.formats.
[[nodiscard]] std::expected<std::vector<Format>, InvalidMimeType> formats(const NegotiationInput& input);
// request.content_mime_type.
[[nodiscard]] std::expected<Format, InvalidMimeType> content_mime_type(std::string_view content_type);
// request.should_apply_vary_header?: the format came from the Accept header.
[[nodiscard]] bool should_apply_vary_header(const NegotiationInput& input);
// request.negotiate_mime(order): the first acceptable format that `order` offers, or null.
[[nodiscard]] Format negotiate(std::span<const Format> formats, std::span<const Format> order);

}  // namespace campfire::req
