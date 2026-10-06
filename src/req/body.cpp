// Request body to params (Rails: ActionDispatch::Request#POST; Rust: crates/kit/src/body.rs).
#include "req/body.hpp"

#include <algorithm>
#include <cctype>

#include "req/format.hpp"
#include "req/query.hpp"

namespace campfire::req {

std::optional<std::string> media_type(std::optional<std::string_view> content_type) {
  if (!content_type) return std::nullopt;
  std::string_view base = content_type->substr(0, content_type->find_first_of(";,"));
  while (!base.empty() && std::isspace(static_cast<unsigned char>(base.front())) != 0) base.remove_prefix(1);
  while (!base.empty() && std::isspace(static_cast<unsigned char>(base.back())) != 0) base.remove_suffix(1);
  if (base.empty()) return std::nullopt;
  std::string out(base);
  std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::expected<ParsedBody, BodyTooLarge> parse_body(std::string_view original_method,
                                                   std::optional<std::string_view> content_type,
                                                   std::string_view body, const std::filesystem::path& tmp_dir,
                                                   std::pmr::memory_resource* mr, std::optional<std::size_t> limit) {
  if (content_type && content_type->empty()) content_type = std::nullopt;
  const auto media = media_type(content_type);
  const bool multipart_media =
      media && (*media == "multipart/form-data" || *media == "multipart/related" || *media == "multipart/mixed");

  if (multipart_media) {
    if (auto boundary = parse_boundary(*content_type)) {
      std::uint64_t cap = kMultipartBytesizeLimit;
      if (limit) cap = std::min<std::uint64_t>(cap, *limit);
      MultipartParser parser(std::move(*boundary), tmp_dir, mr, cap);
      auto fed = parser.feed(body);
      if (!fed) {
        if (fed.error().code == ParamErrc::TooLarge) return std::unexpected(BodyTooLarge{});
        return ParsedBody{{}, std::unexpected(fed.error())};
      }
      auto params = parser.finish();
      if (!params && params.error().code == ParamErrc::TooLarge) return std::unexpected(BodyTooLarge{});
      return ParsedBody{{}, std::move(params)};
    }
  }

  if (body.size() > std::min(limit.value_or(SIZE_MAX), kMaxBufferedBody)) return std::unexpected(BodyTooLarge{});

  bool is_json = false;
  if (content_type) {
    auto parsed = content_mime_type(*content_type);
    is_json = parsed && *parsed == &mime::JSON;
  }
  ParamResult<ParamMap> params = ParamMap(mr);
  if (is_json && !body.empty()) {
    params = from_json_body(body, mr);
  } else if ((media && *media == "application/x-www-form-urlencoded") ||
             (!content_type && original_method == "POST") || multipart_media) {
    params = from_form_body(body, mr);
  }
  return ParsedBody{std::string(body), std::move(params)};
}

}  // namespace campfire::req
