// A DOM that behaves like the libxml2 tree that Nokogiri::HTML5 (Gumbo) builds. Rust: crates/richtext/src/dom.rs
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace campfire::richtext {

// Gumbo limits that Nokogiri::HTML5 applies (Nokogiri::Gumbo::DEFAULT_MAX_TREE_DEPTH and
// DEFAULT_MAX_ATTRIBUTES). Nokogiri raises ArgumentError when a parse goes over them.
inline constexpr int kMaxTreeDepth = 400;
inline constexpr int kMaxAttributes = 400;

enum class ParseError : std::uint8_t {
  TreeTooDeep,        // "Document tree depth limit exceeded"
  TooManyAttributes,  // "Attributes per element limit exceeded"
};

[[nodiscard]] std::string_view to_string(ParseError error) noexcept;

// A bump allocator. Nodes, names and values of one Dom live here until the Dom dies.
// The arena never runs destructors, so it holds trivially destructible types only.
class Arena {
 public:
  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&&) noexcept = default;
  Arena& operator=(Arena&&) noexcept = default;
  ~Arena() = default;

  [[nodiscard]] void* allocate(std::size_t bytes, std::size_t align);

  template <class T, class... Args>
  [[nodiscard]] T* make(Args&&... args) {
    static_assert(std::is_trivially_destructible_v<T>);
    return std::construct_at(static_cast<T*>(allocate(sizeof(T), alignof(T))), std::forward<Args>(args)...);
  }

  [[nodiscard]] std::string_view copy(std::string_view text);

  // Total bytes in all blocks. For tests and for limits.
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

 private:
  struct Block {
    std::unique_ptr<std::byte[]> data;
    std::size_t size = 0;
    std::size_t used = 0;
  };
  std::vector<Block> blocks_;
  std::size_t capacity_ = 0;
};

enum class NodeType : std::uint8_t { Fragment, Element, Text, Comment, CData };

// The namespace of an element. Gumbo gives HTML, SVG and MathML only.
enum class Ns : std::uint8_t { Html, Svg, MathMl };

// The namespace of an attribute. Only attributes of foreign elements have one.
enum class AttrNs : std::uint8_t { None, XLink, Xml, XmlNs };

struct Attr {
  std::string_view name;  // The local name: "href" for xlink:href
  std::string_view value;
  AttrNs ns = AttrNs::None;

  // The name as Nokogiri writes it: "xlink:href", "xml:lang", "xmlns", "xmlns:xlink", "href".
  [[nodiscard]] std::string qualified_name() const;
  [[nodiscard]] bool has_qualified_name(std::string_view qualified) const;
};

// A node. The tree uses intrusive links, so unwrap and detach take constant time.
struct Node {
  NodeType type = NodeType::Fragment;
  Ns ns = Ns::Html;
  std::string_view name;  // Elements: the local name
  std::string_view text;  // Text, comment, CDATA: the content
  Node* parent = nullptr;
  Node* first_child = nullptr;
  Node* last_child = nullptr;
  Node* prev = nullptr;
  Node* next = nullptr;
  Attr* attrs = nullptr;
  std::uint32_t attr_count = 0;
  std::uint32_t attr_capacity = 0;

  [[nodiscard]] bool is_element() const noexcept { return type == NodeType::Element; }
  [[nodiscard]] bool is_text() const noexcept { return type == NodeType::Text; }
  [[nodiscard]] bool is_html_element() const noexcept { return type == NodeType::Element && ns == Ns::Html; }
  [[nodiscard]] std::span<Attr> attributes() noexcept { return {attrs, attr_count}; }
  [[nodiscard]] std::span<const Attr> attributes() const noexcept { return {attrs, attr_count}; }
  // Nokogiri's `node[name]`, by qualified name. Null when the attribute is absent.
  [[nodiscard]] const Attr* find_attr(std::string_view qualified) const noexcept;
};

// A tree of nodes with a Fragment node as its root. A Dom owns its arena.
class Dom {
 public:
  Dom();
  Dom(Dom&&) noexcept = default;
  Dom& operator=(Dom&&) noexcept = default;
  Dom(const Dom&) = delete;
  Dom& operator=(const Dom&) = delete;
  ~Dom() = default;

  [[nodiscard]] Node* root() noexcept { return root_; }
  [[nodiscard]] const Node* root() const noexcept { return root_; }
  [[nodiscard]] Arena& arena() noexcept { return arena_; }

  // Creation. The node has no parent.
  [[nodiscard]] Node* create_element(std::string_view name, Ns ns = Ns::Html);
  [[nodiscard]] Node* create_text(std::string_view text);
  [[nodiscard]] Node* create_comment(std::string_view text);

  // Moves `child` (and its subtree) to the end of `parent`.
  void append_child(Node* parent, Node* child) noexcept;
  // Moves `node` to just before `reference`. `reference` must have a parent.
  void insert_before(Node* reference, Node* node) noexcept;
  // Removes `node` from its parent. The node keeps its subtree.
  void detach(Node* node) noexcept;
  // Moves the children of `node` to just before it, then detaches it
  // (Nokogiri: `node.before(node.children); node.remove`).
  void unwrap(Node* node) noexcept;

  // Nokogiri's `node[name] = value`: change in place, or append.
  void set_attr(Node* node, std::string_view name, std::string_view value);
  void remove_attr_at(Node* node, std::size_t index) noexcept;

 private:
  Arena arena_;
  Node* root_ = nullptr;
};

// `Nokogiri::HTML5::DocumentFragment.parse(html)`: a fragment parsed in a body context with
// Gumbo's limits. The caller strips the input where Rails does.
[[nodiscard]] std::expected<Dom, ParseError> parse_fragment(std::string_view html);

// `node.to_html` for an HTML5 document (Nokogiri's html_standard_serialize): the node itself,
// or the children of a fragment. Appends to `out`.
void serialize(const Node* node, std::string& out);
[[nodiscard]] std::string to_html(const Node* node);

}  // namespace campfire::richtext
