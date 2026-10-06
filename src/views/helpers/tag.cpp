// ActionView TagHelper (Rust: crates/views/src/helpers/tag.rs).
#include "views/helpers/tag.hpp"

#include <algorithm>
#include <array>

namespace campfire::views::helpers {
namespace {

// TagHelper::BOOLEAN_ATTRIBUTES
constexpr std::array<std::string_view, 44> kBooleanAttributes = {"allowfullscreen",
                                                                 "allowpaymentrequest",
                                                                 "async",
                                                                 "autofocus",
                                                                 "autoplay",
                                                                 "checked",
                                                                 "compact",
                                                                 "controls",
                                                                 "declare",
                                                                 "default",
                                                                 "defaultchecked",
                                                                 "defaultmuted",
                                                                 "defaultselected",
                                                                 "defer",
                                                                 "disabled",
                                                                 "enabled",
                                                                 "formnovalidate",
                                                                 "hidden",
                                                                 "indeterminate",
                                                                 "inert",
                                                                 "ismap",
                                                                 "itemscope",
                                                                 "loop",
                                                                 "multiple",
                                                                 "muted",
                                                                 "nohref",
                                                                 "nomodule",
                                                                 "noresize",
                                                                 "noshade",
                                                                 "novalidate",
                                                                 "nowrap",
                                                                 "open",
                                                                 "pauseonexit",
                                                                 "playsinline",
                                                                 "readonly",
                                                                 "required",
                                                                 "reversed",
                                                                 "scoped",
                                                                 "seamless",
                                                                 "selected",
                                                                 "sortable",
                                                                 "truespeed",
                                                                 "typemustmatch",
                                                                 "visible"};

// HTML void elements.
constexpr std::array<std::string_view, 14> kVoidElements = {
    "area", "base", "br", "col", "embed", "hr", "img", "input", "keygen", "link", "meta", "source", "track", "wbr"};

template <class Array>
bool contains(const Array& list, std::string_view name) {
  return std::ranges::find(list, name) != list.end();
}

void write_attr_start(Out& out, std::string_view name) {
  out.append_char(' ');
  out.append_raw(name);
  out.append_raw("=\"");
}

void render_one(Out& out, std::string_view name, const Value& value) {
  switch (value.kind) {
    case Value::Kind::Bool:
      if (value.flag && contains(kBooleanAttributes, name)) {
        write_attr_start(out, name);
        out.append_raw(name);
        out.append_char('"');
      } else if (!contains(kBooleanAttributes, name)) {
        write_attr_start(out, name);
        out.append_raw(value.flag ? "true" : "false");
        out.append_char('"');
      }
      break;
    case Value::Kind::Text:
      write_attr_start(out, name);
      html_escape(out, value.text);
      out.append_char('"');
      break;
    case Value::Kind::Safe: {
      write_attr_start(out, name);
      std::string_view rest = value.text;
      for (std::size_t q = rest.find('"'); q != std::string_view::npos; q = rest.find('"')) {
        out.append_raw(rest.substr(0, q));
        out.append_raw("&quot;");
        rest.remove_prefix(q + 1);
      }
      out.append_raw(rest);
      out.append_char('"');
      break;
    }
  }
}

}  // namespace

Attrs& Attrs::set(std::string_view name, std::optional<Value> value) {
  for (auto& [key, slot] : entries_) {
    if (key == name) {
      slot = std::move(value);
      return *this;
    }
  }
  entries_.emplace_back(std::string(name), std::move(value));
  return *this;
}

Attrs&& Attrs::attr_opt(std::string_view name, std::optional<std::string_view> value) && {
  set(name, value ? std::optional<Value>(Value(*value)) : std::nullopt);
  return std::move(*this);
}

std::string dasherize(std::string_view key) {
  std::string out(key);
  std::ranges::replace(out, '_', '-');
  return out;
}

Attrs&& Attrs::data(std::string_view key, Value value) && {
  return std::move(*this).attr("data-" + dasherize(key), std::move(value));
}

Attrs&& Attrs::aria(std::string_view key, Value value) && {
  return std::move(*this).attr("aria-" + dasherize(key), std::move(value));
}

void Attrs::set_default(std::string_view name, std::optional<Value> value) {
  if (get(name) == nullptr) {
    set(name, std::move(value));
  }
}

void Attrs::fetch_or_set(std::string_view name, std::optional<Value> value) {
  if (!has(name)) {
    set(name, std::move(value));
  }
}

const Value* Attrs::get(std::string_view name) const {
  for (const auto& [key, slot] : entries_) {
    if (key == name) {
      return slot ? &*slot : nullptr;
    }
  }
  return nullptr;
}

bool Attrs::has(std::string_view name) const {
  return std::ranges::any_of(entries_, [&](const Entry& e) { return e.first == name; });
}

std::optional<Value> Attrs::remove(std::string_view name) {
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (it->first == name) {
      std::optional<Value> value = std::move(it->second);
      entries_.erase(it);
      return value;
    }
  }
  return std::nullopt;
}

Attrs Attrs::merged(const Attrs& other) const& {
  Attrs result = *this;
  for (const auto& [key, slot] : other.entries_) {
    result.set(key, slot);
  }
  return result;
}

Attrs&& Attrs::merge(Attrs other) && {
  for (auto& [key, slot] : other.entries_) {
    set(key, std::move(slot));
  }
  return std::move(*this);
}

Attrs Attrs::with_default_data(const Attrs& defaults) const {
  const auto is_data = [](const Entry& e) { return e.first.starts_with("data-"); };
  const auto first = std::ranges::find_if(entries_, is_data);
  Attrs before;
  Attrs data = defaults;
  Attrs after;
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (it < first) {
      before.set(it->first, it->second);
    } else if (is_data(*it)) {
      data.set(it->first, it->second);
    } else {
      after.set(it->first, it->second);
    }
  }
  for (const auto& e : data.entries_) {
    before.set(e.first, e.second);
  }
  for (const auto& e : after.entries_) {
    before.set(e.first, e.second);
  }
  return before;
}

void render_attrs(Out& out, const Attrs& attrs) {
  for (const auto& [name, value] : attrs.entries()) {
    if (value) {
      render_one(out, name, *value);
    }
  }
}

void open_tag(Out& out, std::string_view name, const Attrs& attrs) {
  out.append_char('<');
  out.append_raw(name);
  render_attrs(out, attrs);
}

void close_tag(Out& out, std::string_view name) {
  out.append_raw("</");
  out.append_raw(name);
  out.append_char('>');
}

void content_tag(Out& out, std::string_view name, const Attrs& attrs, SafeHtml content) {
  content_tag(out, name, attrs, [&](Out& o) { o.append(content); });
}

void content_tag_text(Out& out, std::string_view name, const Attrs& attrs, std::string_view text) {
  content_tag(out, name, attrs, [&](Out& o) { html_escape(o, text); });
}

void builder_tag(Out& out, std::string_view name, const Attrs& attrs) {
  const std::string tag = name.contains('_') ? dasherize(name) : std::string(name);
  open_tag(out, tag, attrs);
  out.append_char('>');
  if (!contains(kVoidElements, tag)) {
    close_tag(out, tag);
  }
}

void legacy_tag(Out& out, std::string_view name, const Attrs& attrs) {
  open_tag(out, name, attrs);
  out.append_raw(" />");
}

}  // namespace campfire::views::helpers
