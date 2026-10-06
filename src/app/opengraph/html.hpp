// The slice of `Nokogiri::HTML(html)` (libxml2's legacy HTML parser) that `Opengraph::Document` reads: every `<meta>`
// element's attributes. Rails: reference/app/models/opengraph/document.rb. Rust: crates/campfire/src/integrations/
// opengraph/html.rs.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <optional>

namespace campfire::app::opengraph {

struct Element {
  std::vector<std::pair<std::string, std::string>> attributes;
  [[nodiscard]] const std::string* attr(std::string_view name) const;
  [[nodiscard]] bool has_attr(std::string_view name) const { return attr(name) != nullptr; }
};

// libxml2 reading a UTF-8 buffer: valid sequences decode, any other byte is taken as Latin-1. The result is UTF-8.
[[nodiscard]] std::string decode(std::string_view bytes);

// The `<meta>` elements of the document, in document order.
[[nodiscard]] std::vector<Element> meta_elements(std::string_view html);

// `Nokogiri::HTML4::Document#meta_encoding`.
[[nodiscard]] std::optional<std::string> meta_encoding(const std::vector<Element>& metas);

}  // namespace campfire::app::opengraph
