// Nokogiri node operations on the Dom of dom.hpp. Rust: crates/richtext/src/dom.rs
#pragma once

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "richtext/dom.hpp"

namespace campfire::richtext {

// An element with this local name (any namespace), as `dom.local_name(node) == Some(name)` in Rust.
[[nodiscard]] bool is_named(const Node* node, std::string_view name) noexcept;
// The value of an attribute by qualified name, or nullopt.
[[nodiscard]] std::optional<std::string_view> attr_value(const Node* node, std::string_view name) noexcept;
// Nokogiri's `node.remove_attribute(name)`: the removed value.
[[nodiscard]] std::optional<std::string> remove_attr(Dom& dom, Node* node, std::string_view name);

// All descendants in document order, not the node itself.
[[nodiscard]] std::vector<Node*> descendants(Node* node);
// The parents from the nearest up to the root.
[[nodiscard]] std::vector<Node*> ancestors(Node* node);
[[nodiscard]] std::vector<Node*> children(Node* node);
[[nodiscard]] std::vector<Node*> element_children(Node* node);

// libxml2's xmlNodeGetContent: the text of a text or comment node, or the text of all descendants.
[[nodiscard]] std::string text_content(const Node* node);
// Nokogiri's `node.name`: the local name for elements, "text" for text, "comment" for comments.
[[nodiscard]] std::string_view node_name(const Node* node) noexcept;

// A node that has no parent, for use as the root of a fragment.
[[nodiscard]] Node* new_fragment(Dom& dom);
// The parse context Nokogiri uses for markup in a node (`Node#fragment`).
[[nodiscard]] FragmentContext context_for(const Node* node) noexcept;
// Parses `html` in `context` and returns the top nodes, detached.
[[nodiscard]] std::expected<std::vector<Node*>, ParseError> parse_nodes(Dom& dom, std::string_view html,
                                                                        const FragmentContext& context);
// `Nokogiri::HTML5::Document#fragment(html)` in `dom`: a new fragment node in a body context.
[[nodiscard]] std::expected<Node*, ParseError> parse_fragment_node(Dom& dom, std::string_view html);
// `node.inner_html = html`, parsed in the context of the node.
[[nodiscard]] std::expected<void, ParseError> set_inner_html(Dom& dom, Node* node, std::string_view html);
// `node.replace(html)`, parsed in the context of the parent of the node.
[[nodiscard]] std::expected<void, ParseError> replace_with_html(Dom& dom, Node* node, std::string_view html);
void replace_with_nodes(Dom& dom, Node* node, std::span<Node* const> replacements);
[[nodiscard]] std::string inner_html(const Node* node);

// A copy of a subtree in the arena of `target`. The copy has no parent.
[[nodiscard]] Node* deep_clone(Dom& target, const Node* source);
// A copy of the whole tree, in a new arena.
[[nodiscard]] Dom clone_dom(const Dom& source);

}  // namespace campfire::richtext
