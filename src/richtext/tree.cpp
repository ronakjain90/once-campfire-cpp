// Nokogiri::XML::Node operations used by Action Text. Rust: crates/richtext/src/dom.rs
#include "richtext/tree.hpp"

namespace campfire::richtext {

bool is_named(const Node* node, std::string_view name) noexcept {
  return node->type == NodeType::Element && node->name == name;
}

std::optional<std::string_view> attr_value(const Node* node, std::string_view name) noexcept {
  if (const Attr* attr = node->find_attr(name)) {
    return attr->value;
  }
  return std::nullopt;
}

std::optional<std::string> remove_attr(Dom& dom, Node* node, std::string_view name) {
  for (std::size_t i = 0; i < node->attr_count; ++i) {
    if (node->attrs[i].has_qualified_name(name)) {
      std::string value(node->attrs[i].value);
      dom.remove_attr_at(node, i);
      return value;
    }
  }
  return std::nullopt;
}

std::vector<Node*> descendants(Node* node) {
  std::vector<Node*> out;
  Node* current = node->first_child;
  while (current != nullptr) {
    out.push_back(current);
    if (current->first_child != nullptr) {
      current = current->first_child;
      continue;
    }
    while (current != node && current->next == nullptr) {
      current = current->parent;
    }
    current = current == node ? nullptr : current->next;
  }
  return out;
}

std::vector<Node*> ancestors(Node* node) {
  std::vector<Node*> out;
  for (Node* p = node->parent; p != nullptr; p = p->parent) {
    out.push_back(p);
  }
  return out;
}

std::vector<Node*> children(Node* node) {
  std::vector<Node*> out;
  for (Node* c = node->first_child; c != nullptr; c = c->next) {
    out.push_back(c);
  }
  return out;
}

std::vector<Node*> element_children(Node* node) {
  std::vector<Node*> out;
  for (Node* c = node->first_child; c != nullptr; c = c->next) {
    if (c->is_element()) {
      out.push_back(c);
    }
  }
  return out;
}

namespace {

void collect_text(const Node* node, std::string& out) {
  for (const Node* c = node->first_child; c != nullptr; c = c->next) {
    if (c->type == NodeType::Text || c->type == NodeType::CData) {
      out.append(c->text);
    }
    collect_text(c, out);
  }
}

}  // namespace

std::string text_content(const Node* node) {
  if (node->type == NodeType::Text || node->type == NodeType::Comment || node->type == NodeType::CData) {
    return std::string(node->text);
  }
  std::string out;
  collect_text(node, out);
  return out;
}

std::string_view node_name(const Node* node) noexcept {
  switch (node->type) {
    case NodeType::Element: return node->name;
    case NodeType::Text: return "text";
    case NodeType::Comment: return "comment";
    case NodeType::CData: return "#cdata-section";
    case NodeType::Fragment: return "#document-fragment";
  }
  return "";
}

Node* new_fragment(Dom& dom) {
  Node* node = dom.arena().make<Node>();
  node->type = NodeType::Fragment;
  return node;
}

FragmentContext context_for(const Node* node) noexcept {
  if (node->type == NodeType::Element) {
    return {node->name, node->ns};
  }
  // Gumbo falls back to a body context for anything that is not an element.
  return {};
}

std::expected<std::vector<Node*>, ParseError> parse_nodes(Dom& dom, std::string_view html,
                                                          const FragmentContext& context) {
  Node* holder = new_fragment(dom);
  if (auto parsed = parse_into(dom, holder, html, context); !parsed) {
    return std::unexpected(parsed.error());
  }
  std::vector<Node*> top = children(holder);
  for (Node* node : top) {
    dom.detach(node);
  }
  return top;
}

std::expected<Node*, ParseError> parse_fragment_node(Dom& dom, std::string_view html) {
  Node* fragment = new_fragment(dom);
  if (auto parsed = parse_into(dom, fragment, html); !parsed) {
    return std::unexpected(parsed.error());
  }
  return fragment;
}

std::expected<void, ParseError> set_inner_html(Dom& dom, Node* node, std::string_view html) {
  auto parsed = parse_nodes(dom, html, context_for(node));
  if (!parsed) {
    return std::unexpected(parsed.error());
  }
  while (node->first_child != nullptr) {
    dom.detach(node->first_child);
  }
  for (Node* child : *parsed) {
    dom.append_child(node, child);
  }
  return {};
}

std::expected<void, ParseError> replace_with_html(Dom& dom, Node* node, std::string_view html) {
  Node* parent = node->parent;
  if (parent == nullptr) {
    return {};
  }
  auto parsed = parse_nodes(dom, html, context_for(parent));
  if (!parsed) {
    return std::unexpected(parsed.error());
  }
  for (Node* n : *parsed) {
    dom.insert_before(node, n);
  }
  dom.detach(node);
  return {};
}

void replace_with_nodes(Dom& dom, Node* node, std::span<Node* const> replacements) {
  if (node->parent == nullptr) {
    return;
  }
  for (Node* n : replacements) {
    dom.insert_before(node, n);
  }
  dom.detach(node);
}

std::string inner_html(const Node* node) {
  std::string out;
  for (const Node* c = node->first_child; c != nullptr; c = c->next) {
    serialize(c, out);
  }
  return out;
}

Node* deep_clone(Dom& target, const Node* source) {
  Node* copy = target.arena().make<Node>();
  copy->type = source->type;
  copy->ns = source->ns;
  copy->name = target.arena().copy(source->name);
  copy->text = target.arena().copy(source->text);
  if (source->attr_count > 0) {
    copy->attrs = static_cast<Attr*>(target.arena().allocate(sizeof(Attr) * source->attr_count, alignof(Attr)));
    copy->attr_capacity = source->attr_count;
    for (std::uint32_t i = 0; i < source->attr_count; ++i) {
      const Attr& a = source->attrs[i];
      std::construct_at(copy->attrs + i, Attr{target.arena().copy(a.name), target.arena().copy(a.value), a.ns});
    }
    copy->attr_count = source->attr_count;
  }
  for (const Node* c = source->first_child; c != nullptr; c = c->next) {
    target.append_child(copy, deep_clone(target, c));
  }
  return copy;
}

Dom clone_dom(const Dom& source) {
  Dom copy;
  for (const Node* c = source.root()->first_child; c != nullptr; c = c->next) {
    copy.append_child(copy.root(), deep_clone(copy, c));
  }
  return copy;
}

}  // namespace campfire::richtext
