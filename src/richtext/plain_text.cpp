// Rails: actiontext/lib/action_text/plain_text_conversion.rb. Rust: crates/richtext/src/plain_text.rs
#include "richtext/plain_text.hpp"

#include <vector>

#include "richtext/text_util.hpp"
#include "richtext/tree.hpp"

namespace campfire::richtext {

namespace {

std::string plain_text_for(const Node* node);

bool is_list(std::string_view name) {
  return name == "ul" || name == "ol";
}

std::string concat_children(const Node* node) {
  std::string out;
  for (const Node* c = node->first_child; c != nullptr; c = c->next) {
    out += plain_text_for(c);
  }
  return out;
}

std::string block(const Node* node) {
  std::string joined = concat_children(node);
  return std::string(chomp_newlines(joined)) + "\n\n";
}

std::size_t list_depth(const Node* node) {
  std::size_t depth = 0;
  for (const Node* a = node->parent; a != nullptr; a = a->parent) {
    if (a->type == NodeType::Element && is_list(a->name)) {
      ++depth;
    }
  }
  return depth;
}

std::string bullet_for_li(const Node* node) {
  const Node* list = nullptr;
  for (const Node* a = node->parent; a != nullptr; a = a->parent) {
    if (a->type == NodeType::Element && is_list(a->name)) {
      list = a;
      break;
    }
  }
  if (list != nullptr && list->name == "ol") {
    std::size_t index = 0;
    if (node->parent != nullptr) {
      for (const Node* c = node->parent->first_child; c != nullptr && c != node; c = c->next) {
        if (c->is_element()) {
          ++index;
        }
      }
    }
    return std::to_string(index + 1) + ".";
  }
  return "•";
}

bool is_ascii_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

std::string blockquote(const Node* node) {
  std::string text = block(node);
  if (is_blank(text)) {
    return "“”";
  }
  // `text.insert(text.rindex(/\S/) + 1, "”")`, then `text.index(/\S/)` for "“"
  std::size_t end = text.size();
  while (end > 0 && is_ascii_space(text[end - 1])) {
    --end;
  }
  text.insert(end, "”");
  std::size_t start = 0;
  while (start < text.size() && is_ascii_space(text[start])) {
    ++start;
  }
  text.insert(start, "“");
  return text;
}

std::string plain_text_for(const Node* node) {
  const std::string_view name = node_name(node);
  if (name == "script" || name == "style" || name == "unsupported") {
    return "";
  }
  if (name == "h1" || name == "p") {
    return block(node);
  }
  if (name == "ul" || name == "ol") {
    std::string text = block(node);
    return list_depth(node) > 0 ? "\n" + text : text;
  }
  if (name == "br") {
    return "\n";
  }
  // Text nodes, and elements that happen to be named "text" (SVG), use `node.text`.
  if (name == "text") {
    return std::string(chomp_newlines(text_content(node)));
  }
  if (name == "div") {
    std::string joined = concat_children(node);
    return std::string(chomp_newlines(joined)) + "\n";
  }
  if (name == "figcaption") {
    std::string joined = concat_children(node);
    return "[" + std::string(chomp_newlines(joined)) + "]";
  }
  if (name == "blockquote") {
    return blockquote(node);
  }
  if (name == "li") {
    const std::string bullet = bullet_for_li(node);
    std::string joined = concat_children(node);
    const std::string text(chomp_newlines(joined));
    const std::size_t depth = list_depth(node);
    const std::string indentation = depth > 1 ? std::string(2 * (depth - 1), ' ') : std::string();
    return indentation + bullet + " " + text + "\n";
  }
  return concat_children(node);
}

}  // namespace

std::string node_to_plain_text(const Node* node) {
  std::string text = plain_text_for(node);
  return std::string(chomp_newlines(text));
}

}  // namespace campfire::richtext
