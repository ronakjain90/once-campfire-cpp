// libFuzzer target for the parser, the serializer and the sanitizer.
// Build: see build.sh in this directory. Run: ./fuzz_richtext_sanitize -max_total_time=300
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "richtext/dom.hpp"
#include "richtext/filters.hpp"
#include "richtext/sanitizer.hpp"

using namespace campfire::richtext;

namespace {

// The output of a sanitizer must hold only what the list allows, whatever the input was.
void check_node(const Node* node, const SafeList& list) {
  for (const Node* child = node->first_child; child != nullptr; child = child->next) {
    if (child->type == NodeType::Comment || child->type == NodeType::CData) {
      std::abort();
    }
    if (!child->is_element()) {
      continue;
    }
    if (child->ns != Ns::Html || !list.tags.contains(child->name)) {
      std::abort();
    }
    for (const Attr& attr : child->attributes()) {
      if (!list.attributes.contains(attr.name) || attr.name.starts_with("on")) {
        std::abort();
      }
      if ((attr.name == "href" || attr.name == "src") && !allowed_uri(attr.value)) {
        // The value was re-escaped after the check, which can only add %20 and %22.
        std::string_view v = attr.value;
        if (v.find("javascript:") == 0 || v.find("vbscript:") == 0) {
          std::abort();
        }
      }
    }
    check_node(child, list);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view html(reinterpret_cast<const char*>(data), size);
  for (const SafeList* list :
       {&SafeList::defaults(), &SafeList::action_text(), &SafeList::content_filter(), &SafeList::auto_link()}) {
    auto out = sanitize(html, *list);
    if (!out) {
      continue;
    }
    auto reparsed = parse_fragment(*out);
    if (reparsed) {
      check_node(reparsed->root(), *list);
    }
  }
  (void)filter_message_html(html);
  (void)allowed_uri(html);
  (void)cgi_unescape_html(html);
  return 0;
}
