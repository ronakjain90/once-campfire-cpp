// libFuzzer target for the full rich text pipeline: parse, sanitize, attachments, autolink, plain
// text, the editor value and the URL checks. The input is a stored message body.
// Build: see build.sh in this directory. Run: ./fuzz_richtext_sanitize -max_total_time=300
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "richtext/attachables.hpp"
#include "richtext/dom.hpp"
#include "richtext/filters.hpp"
#include "richtext/richtext.hpp"
#include "richtext/sanitizer.hpp"
#include "richtext/uri.hpp"

using namespace campfire::richtext;

namespace {

// The output of a sanitizer must hold only what the list allows, whatever the input was.
// `extra_attribute` is an attribute that a later step adds (autolink adds "target").
void check_node(const Node* node, const SafeList& list, std::string_view extra_attribute = {}) {
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
      if ((!list.attributes.contains(attr.name) && attr.name != extra_attribute) || attr.name.starts_with("on")) {
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
    check_node(child, list, extra_attribute);
  }
}

// What a browser must never get from the pipeline, whichever list a step used.
void check_safe(const Node* node) {
  static constexpr std::string_view kDangerous[] = {"script", "style",  "iframe", "object", "embed", "svg",
                                                    "math",   "form",   "input",  "base",   "meta",  "link"};
  for (const Node* child = node->first_child; child != nullptr; child = child->next) {
    if (child->type == NodeType::Comment || child->type == NodeType::CData) {
      std::abort();
    }
    if (child->is_element()) {
      for (std::string_view name : kDangerous) {
        if (child->name == name) {
          std::abort();
        }
      }
      for (const Attr& attr : child->attributes()) {
        if (attr.name.starts_with("on")) {
          std::abort();
        }
        if (attr.name == "href" || attr.name == "src") {
          std::string cleaned;
          for (char c : attr.value) {
            if (static_cast<unsigned char>(c) > 0x20 && c != 0x7f) {
              cleaned.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
            }
          }
          if (cleaned.starts_with("javascript:") || cleaned.starts_with("vbscript:")) {
            std::abort();
          }
        }
      }
    }
    check_safe(child);
  }
}

// Stands in for the database. The text of the SGID chooses the answer, so the fuzzer can reach each branch.
class FuzzResolver final : public AttachableResolver {
 public:
  SignedLookup locate_signed(std::string_view sgid) const override {
    SignedLookup out;
    if (sgid.find("u1") != std::string_view::npos) {
      out.kind = SignedLookup::Kind::User;
      out.user = user(1);
    } else if (sgid.find("u2") != std::string_view::npos) {
      out.kind = SignedLookup::Kind::User;
      out.user = user(2);
    } else if (sgid.find('m') != std::string_view::npos) {
      out.kind = SignedLookup::Kind::MissingRecord;
      out.model_name = "User";
    }
    return out;
  }

  GidLookup find_gid(const campfire::compat::global_id::GlobalId& gid) const override {
    GidLookup out;
    if (gid.model_name == "User") {
      if (gid.id == "1" || gid.id == "2") {
        out.kind = GidLookup::Kind::User;
        out.user = user(gid.id == "1" ? 1 : 2);
      } else if (gid.id == "3") {
        out.kind = GidLookup::Kind::Raises;
      }
    } else if (gid.model_name == "Room") {
      out.kind = GidLookup::Kind::OtherModel;
    }
    return out;
  }

 private:
  static MentionUser user(std::int64_t id) {
    MentionUser u;
    u.id = id;
    u.name = "User <" + std::to_string(id) + ">";
    u.title = u.name + " \"title\"";
    u.attachable_sgid = "sgid-u" + std::to_string(id);
    u.user_path = "/users/" + std::to_string(id);
    u.avatar_path = "/users/" + std::to_string(id) + "/avatar?v=1";
    return u;
  }
};

void run_pipeline(std::string_view body) {
  static const FuzzResolver resolver;
  const RenderContext ctx{resolver, "once.campfire.test"};
  (void)message_presentation(body, ctx);
  const Presentation presentation = present_message(body, ctx);
  if (presentation.kind == Presentation::Kind::Html && !presentation.html.empty()) {
    // What the pipeline writes is parsed again by a browser: it must hold only what the lists allow.
    auto reparsed = parse_fragment(presentation.html);
    if (reparsed) {
      check_safe(reparsed->root());
    }
  }
  (void)to_plain_text(body, ctx);
  (void)editable_value(body, ctx);
  (void)mentioned_users(body, ctx);
  (void)filtered_html(body, ctx);
  (void)parse_uri(body);
  (void)web_url(body, "once.campfire.test");
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
  run_pipeline(html);
  (void)allowed_uri(html);
  (void)cgi_unescape_html(html);
  return 0;
}
