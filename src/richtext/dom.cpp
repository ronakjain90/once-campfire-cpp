// Gumbo to DOM conversion and HTML5 serialization. Rails: nokogiri ext/nokogiri/gumbo.c (build_tree),
// ext/nokogiri/xml_node.c (html_standard_serialize). Rust: crates/richtext/src/dom.rs
#include "richtext/dom.hpp"

#include <algorithm>
#include <cstring>

extern "C" {
#include "vendor/gumbo/nokogiri_gumbo.h"
}

namespace campfire::richtext {

std::string_view to_string(ParseError error) noexcept {
  switch (error) {
    case ParseError::TreeTooDeep: return "Document tree depth limit exceeded";
    case ParseError::TooManyAttributes: return "Attributes per element limit exceeded";
  }
  return "";
}

// --- Arena ---------------------------------------------------------------------------------------

void* Arena::allocate(std::size_t bytes, std::size_t align) {
  if (bytes == 0) {
    bytes = 1;
  }
  if (!blocks_.empty()) {
    Block& block = blocks_.back();
    std::size_t offset = (block.used + align - 1) & ~(align - 1);
    if (offset + bytes <= block.size) {
      block.used = offset + bytes;
      return block.data.get() + offset;
    }
  }
  constexpr std::size_t kBlock = 64 * 1024;
  std::size_t size = std::max(kBlock, bytes + align);
  Block block;
  block.data = std::make_unique_for_overwrite<std::byte[]>(size);
  block.size = size;
  auto base = reinterpret_cast<std::uintptr_t>(block.data.get());
  std::size_t offset = ((base + align - 1) & ~(align - 1)) - base;
  block.used = offset + bytes;
  void* result = block.data.get() + offset;
  capacity_ += size;
  // A large block must not replace the partly used tail block as the current one.
  if (size > kBlock && !blocks_.empty()) {
    blocks_.insert(blocks_.end() - 1, std::move(block));
  } else {
    blocks_.push_back(std::move(block));
  }
  return result;
}

std::string_view Arena::copy(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  char* data = static_cast<char*>(allocate(text.size(), 1));
  std::memcpy(data, text.data(), text.size());
  return {data, text.size()};
}

// --- Attr and Node -------------------------------------------------------------------------------

std::string Attr::qualified_name() const {
  switch (ns) {
    case AttrNs::XLink: return "xlink:" + std::string(name);
    case AttrNs::Xml: return "xml:" + std::string(name);
    case AttrNs::XmlNs: return name == "xmlns" ? std::string(name) : "xmlns:" + std::string(name);
    case AttrNs::None: break;
  }
  return std::string(name);
}

bool Attr::has_qualified_name(std::string_view qualified) const {
  if (ns == AttrNs::None) {
    return name == qualified;
  }
  return qualified_name() == qualified;
}

const Attr* Node::find_attr(std::string_view qualified) const noexcept {
  for (const Attr& attr : attributes()) {
    if (attr.has_qualified_name(qualified)) {
      return &attr;
    }
  }
  return nullptr;
}

// --- Dom -----------------------------------------------------------------------------------------

Dom::Dom() {
  root_ = arena_.make<Node>();
  root_->type = NodeType::Fragment;
}

Node* Dom::create_element(std::string_view name, Ns ns) {
  Node* node = arena_.make<Node>();
  node->type = NodeType::Element;
  node->ns = ns;
  node->name = arena_.copy(name);
  return node;
}

Node* Dom::create_text(std::string_view text) {
  Node* node = arena_.make<Node>();
  node->type = NodeType::Text;
  node->text = arena_.copy(text);
  return node;
}

Node* Dom::create_comment(std::string_view text) {
  Node* node = arena_.make<Node>();
  node->type = NodeType::Comment;
  node->text = arena_.copy(text);
  return node;
}

void Dom::detach(Node* node) noexcept {
  if (node->parent == nullptr) {
    return;
  }
  Node* parent = node->parent;
  if (node->prev != nullptr) {
    node->prev->next = node->next;
  } else {
    parent->first_child = node->next;
  }
  if (node->next != nullptr) {
    node->next->prev = node->prev;
  } else {
    parent->last_child = node->prev;
  }
  node->parent = node->prev = node->next = nullptr;
}

void Dom::append_child(Node* parent, Node* child) noexcept {
  detach(child);
  child->parent = parent;
  child->prev = parent->last_child;
  if (parent->last_child != nullptr) {
    parent->last_child->next = child;
  } else {
    parent->first_child = child;
  }
  parent->last_child = child;
}

void Dom::insert_before(Node* reference, Node* node) noexcept {
  detach(node);
  Node* parent = reference->parent;
  node->parent = parent;
  node->next = reference;
  node->prev = reference->prev;
  if (reference->prev != nullptr) {
    reference->prev->next = node;
  } else {
    parent->first_child = node;
  }
  reference->prev = node;
}

void Dom::unwrap(Node* node) noexcept {
  while (node->first_child != nullptr) {
    insert_before(node, node->first_child);
  }
  detach(node);
}

void Dom::set_attr(Node* node, std::string_view name, std::string_view value) {
  for (Attr& attr : node->attributes()) {
    if (attr.has_qualified_name(name)) {
      attr.value = arena_.copy(value);
      return;
    }
  }
  if (node->attr_count == node->attr_capacity) {
    std::uint32_t capacity = node->attr_capacity == 0 ? 4 : node->attr_capacity * 2;
    Attr* grown = static_cast<Attr*>(arena_.allocate(sizeof(Attr) * capacity, alignof(Attr)));
    for (std::uint32_t i = 0; i < node->attr_count; ++i) {
      std::construct_at(grown + i, node->attrs[i]);
    }
    node->attrs = grown;
    node->attr_capacity = capacity;
  }
  std::construct_at(node->attrs + node->attr_count, Attr{arena_.copy(name), arena_.copy(value), AttrNs::None});
  ++node->attr_count;
}

void Dom::remove_attr_at(Node* node, std::size_t index) noexcept {
  for (std::size_t i = index + 1; i < node->attr_count; ++i) {
    node->attrs[i - 1] = node->attrs[i];
  }
  --node->attr_count;
}

// --- Gumbo conversion ----------------------------------------------------------------------------

namespace {

struct GumboOutputDeleter {
  void operator()(GumboOutput* output) const noexcept { gumbo_destroy_output(output); }
};

// libxml2's xmlAddChild merges a text node into a preceding text node.
void add_text(Dom& dom, Node* parent, std::string_view text) {
  Node* last = parent->last_child;
  if (last != nullptr && last->type == NodeType::Text) {
    std::string merged;
    merged.reserve(last->text.size() + text.size());
    merged.append(last->text).append(text);
    last->text = dom.arena().copy(merged);
    return;
  }
  dom.append_child(parent, dom.create_text(text));
}

void convert(Dom& dom, Node* parent, const GumboNode* gumbo) {
  const GumboVector& children =
      gumbo->type == GUMBO_NODE_DOCUMENT ? gumbo->v.document.children : gumbo->v.element.children;
  for (unsigned i = 0; i < children.length; ++i) {
    const auto* child = static_cast<const GumboNode*>(children.data[i]);
    switch (child->type) {
      case GUMBO_NODE_DOCUMENT: break;
      case GUMBO_NODE_TEXT:
      case GUMBO_NODE_WHITESPACE: add_text(dom, parent, child->v.text.text); break;
      case GUMBO_NODE_CDATA: {
        Node* node = dom.create_text(child->v.text.text);
        node->type = NodeType::CData;
        dom.append_child(parent, node);
        break;
      }
      case GUMBO_NODE_COMMENT: dom.append_child(parent, dom.create_comment(child->v.text.text)); break;
      case GUMBO_NODE_TEMPLATE:  // Nokogiri keeps template contents as ordinary children
      case GUMBO_NODE_ELEMENT: {
        const GumboElement& element = child->v.element;
        Ns ns = Ns::Html;
        if (element.tag_namespace == GUMBO_NAMESPACE_SVG) {
          ns = Ns::Svg;
        } else if (element.tag_namespace == GUMBO_NAMESPACE_MATHML) {
          ns = Ns::MathMl;
        }
        Node* node = dom.create_element(element.name, ns);
        if (element.attributes.length > 0) {
          node->attrs =
              static_cast<Attr*>(dom.arena().allocate(sizeof(Attr) * element.attributes.length, alignof(Attr)));
          node->attr_capacity = element.attributes.length;
          for (unsigned a = 0; a < element.attributes.length; ++a) {
            const auto* attr = static_cast<const GumboAttribute*>(element.attributes.data[a]);
            AttrNs attr_ns = AttrNs::None;
            switch (attr->attr_namespace) {
              case GUMBO_ATTR_NAMESPACE_XLINK: attr_ns = AttrNs::XLink; break;
              case GUMBO_ATTR_NAMESPACE_XML: attr_ns = AttrNs::Xml; break;
              case GUMBO_ATTR_NAMESPACE_XMLNS: attr_ns = AttrNs::XmlNs; break;
              case GUMBO_ATTR_NAMESPACE_NONE: break;
            }
            std::construct_at(node->attrs + a,
                              Attr{dom.arena().copy(attr->name), dom.arena().copy(attr->value), attr_ns});
          }
          node->attr_count = element.attributes.length;
        }
        dom.append_child(parent, node);
        convert(dom, node, child);
        break;
      }
    }
  }
}

}  // namespace

std::expected<void, ParseError> parse_into(Dom& dom, Node* parent, std::string_view html,
                                           const FragmentContext& context) {
  // The options Nokogiri::HTML5::DocumentFragment passes: max_attributes 400, max_errors 0,
  // max_tree_depth 400 plus one for the html element of a fragment.
  const std::string context_name(context.name);
  GumboOptions options = kGumboDefaultOptions;
  options.max_attributes = kMaxAttributes;
  options.max_errors = 0;
  options.max_tree_depth = kMaxTreeDepth + 1;
  options.fragment_context = context_name.c_str();
  switch (context.ns) {
    case Ns::Html: options.fragment_namespace = GUMBO_NAMESPACE_HTML; break;
    case Ns::Svg: options.fragment_namespace = GUMBO_NAMESPACE_SVG; break;
    case Ns::MathMl: options.fragment_namespace = GUMBO_NAMESPACE_MATHML; break;
  }
  options.fragment_encoding = nullptr;
  options.quirks_mode = GUMBO_DOCTYPE_NO_QUIRKS;
  options.fragment_context_has_form_ancestor = false;
  options.parse_noscript_content_as_text = false;

  std::unique_ptr<GumboOutput, GumboOutputDeleter> output(gumbo_parse_with_options(&options, html.data(), html.size()));
  switch (output->status) {
    case GUMBO_STATUS_OK: break;
    case GUMBO_STATUS_TOO_MANY_ATTRIBUTES: return std::unexpected(ParseError::TooManyAttributes);
    case GUMBO_STATUS_TREE_TOO_DEEP:
    case GUMBO_STATUS_OUT_OF_MEMORY: return std::unexpected(ParseError::TreeTooDeep);
  }
  convert(dom, parent, output->root);
  return {};
}

std::expected<Dom, ParseError> parse_fragment(std::string_view html) {
  Dom dom;
  auto parsed = parse_into(dom, dom.root(), html);
  if (!parsed) {
    return std::unexpected(parsed.error());
  }
  return dom;
}

// --- Serialization -------------------------------------------------------------------------------

namespace {

bool is_void_element(std::string_view name) {
  static constexpr std::string_view kVoid[] = {"area",  "base",  "basefont", "bgsound", "br",    "col",
                                               "embed", "frame", "hr",       "img",     "input", "keygen",
                                               "link",  "meta",  "param",    "source",  "track", "wbr"};
  return std::find(std::begin(kVoid), std::end(kVoid), name) != std::end(kVoid);
}

bool is_unescaped_text_element(std::string_view name) {
  static constexpr std::string_view kRaw[] = {"style",   "script",   "xmp",       "iframe",
                                              "noembed", "noframes", "plaintext", "noscript"};
  return std::find(std::begin(kRaw), std::end(kRaw), name) != std::end(kRaw);
}

// Nokogiri's output_escaped_string. U+00A0 (C2 A0 in UTF-8) becomes &nbsp;.
void escape(std::string_view text, bool attribute, std::string& out, bool brackets = false) {
  std::size_t start = 0;
  const std::size_t size = text.size();
  for (std::size_t i = 0; i < size; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    std::string_view replacement;
    std::size_t skip = 1;
    if (c == '&') {
      replacement = "&amp;";
    } else if (c == 0xC2 && i + 1 < size && static_cast<unsigned char>(text[i + 1]) == 0xA0) {
      replacement = "&nbsp;";
      skip = 2;
    } else if (attribute && c == '"') {
      replacement = "&quot;";
    } else if (attribute && brackets && c == '<') {
      replacement = "&lt;";
    } else if (attribute && brackets && c == '>') {
      replacement = "&gt;";
    } else if (!attribute && c == '<') {
      replacement = "&lt;";
    } else if (!attribute && c == '>') {
      replacement = "&gt;";
    } else {
      continue;
    }
    out.append(text.substr(start, i - start));
    out.append(replacement);
    i += skip - 1;
    start = i + 1;
  }
  out.append(text.substr(start));
}

void serialize_node(const Node* node, std::string& out, bool brackets) {
  switch (node->type) {
    case NodeType::Element: {
      out.push_back('<');
      out.append(node->name);
      for (const Attr& attr : node->attributes()) {
        out.push_back(' ');
        switch (attr.ns) {
          case AttrNs::XLink: out.append("xlink:"); break;
          case AttrNs::Xml: out.append("xml:"); break;
          case AttrNs::XmlNs:
            if (attr.name != "xmlns") {
              out.append("xmlns:");
            }
            break;
          case AttrNs::None: break;
        }
        out.append(attr.name);
        out.append("=\"");
        escape(attr.value, true, out, brackets);
        out.push_back('"');
      }
      out.push_back('>');
      if (node->ns == Ns::Html && is_void_element(node->name)) {
        return;
      }
      for (const Node* child = node->first_child; child != nullptr; child = child->next) {
        serialize_node(child, out, brackets);
      }
      out.append("</");
      out.append(node->name);
      out.push_back('>');
      return;
    }
    case NodeType::Text:
      if (node->parent != nullptr && node->parent->type == NodeType::Element && node->parent->ns == Ns::Html &&
          is_unescaped_text_element(node->parent->name)) {
        out.append(node->text);
      } else {
        escape(node->text, false, out);
      }
      return;
    case NodeType::CData:
      out.append("<![CDATA[");
      out.append(node->text);
      out.append("]]>");
      return;
    case NodeType::Comment:
      out.append("<!--");
      out.append(node->text);
      out.append("-->");
      return;
    case NodeType::Fragment:
      for (const Node* child = node->first_child; child != nullptr; child = child->next) {
        serialize_node(child, out, brackets);
      }
      return;
  }
}

}  // namespace

void serialize(const Node* node, std::string& out) {
  serialize_node(node, out, false);
}

void serialize(const Node* node, std::string& out, AttrBrackets brackets) {
  serialize_node(node, out, brackets == AttrBrackets::Escaped);
}

std::string to_html(const Node* node) {
  std::string out;
  serialize_node(node, out, false);
  return out;
}

std::string to_html(const Node* node, AttrBrackets brackets) {
  std::string out;
  serialize_node(node, out, brackets == AttrBrackets::Escaped);
  return out;
}

}  // namespace campfire::richtext
