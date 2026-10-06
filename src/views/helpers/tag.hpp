// ActionView TagHelper: attribute rendering (`tag_options`) and tag builders.
// Rust: crates/views/src/helpers/tag.rs. Rails: action_view/helpers/tag_helper.rb.
#pragma once

#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/html.hpp"
#include "core/out.hpp"

namespace campfire::views::helpers {

// Copies the bytes of `src` to the end of `dst`.
inline void append_out(Out& dst, const Out& src) {
  for (const iovec& v : src.iovecs()) {
    dst.append_raw(std::string_view(static_cast<const char*>(v.iov_base), v.iov_len));
  }
}

// A body of a block helper: writes its content to the output.
template <class F>
concept BodyFn = std::invocable<F, Out&>;

// An attribute value. Text is escaped. Safe text only has `"` replaced. A bool is a boolean
// attribute (`hidden="hidden"` or nothing) or, for other names, "true" or "false".
struct Value {
  enum class Kind : std::uint8_t { Text, Safe, Bool };
  Kind kind = Kind::Text;
  bool flag = false;
  std::string text;

  Value() = default;
  Value(std::string_view s) : text(s) {}                          // NOLINT(google-explicit-constructor)
  Value(const std::string& s) : text(s) {}                        // NOLINT(google-explicit-constructor)
  Value(const char* s) : text(s) {}                               // NOLINT(google-explicit-constructor)
  Value(bool b) : kind(Kind::Bool), flag(b) {}                    // NOLINT(google-explicit-constructor)
  Value(SafeHtml html) : kind(Kind::Safe), text(html.view()) {}   // NOLINT(google-explicit-constructor)
  template <std::integral T>
    requires(!std::same_as<T, bool>)
  Value(T n) : text(std::to_string(n)) {}                         // NOLINT(google-explicit-constructor)

  // The value as Ruby `to_s` prints it.
  [[nodiscard]] std::string to_s() const { return kind == Kind::Bool ? (flag ? "true" : "false") : text; }
};

// An ordered options hash. A nullopt value keeps its place (Ruby `options[k] = nil`) but is never
// written.
class Attrs {
 public:
  using Entry = std::pair<std::string, std::optional<Value>>;

  // Hash assignment: replaces the value in place, or appends.
  Attrs& set(std::string_view name, std::optional<Value> value);

  Attrs& attr(std::string_view name, Value value) & { return set(name, std::move(value)); }
  Attrs&& attr(std::string_view name, Value value) && { return std::move(set(name, std::move(value))); }
  Attrs&& attr_opt(std::string_view name, std::optional<std::string_view> value) &&;

  // `data: { turbo_frame: ... }` is `data-turbo-frame`.
  Attrs&& data(std::string_view key, Value value) &&;
  Attrs&& aria(std::string_view key, Value value) &&;
  Attrs&& aria_hidden() && { return std::move(*this).aria("hidden", "true"); }
  Attrs&& cls(Value v) && { return std::move(*this).attr("class", std::move(v)); }
  Attrs&& id(Value v) && { return std::move(*this).attr("id", std::move(v)); }
  Attrs&& style(Value v) && { return std::move(*this).attr("style", std::move(v)); }
  Attrs&& title(Value v) && { return std::move(*this).attr("title", std::move(v)); }
  Attrs&& alt(Value v) && { return std::move(*this).attr("alt", std::move(v)); }
  Attrs&& role(Value v) && { return std::move(*this).attr("role", std::move(v)); }
  Attrs&& name(Value v) && { return std::move(*this).attr("name", std::move(v)); }
  Attrs&& type(Value v) && { return std::move(*this).attr("type", std::move(v)); }
  Attrs&& value(Value v) && { return std::move(*this).attr("value", std::move(v)); }
  Attrs&& target(Value v) && { return std::move(*this).attr("target", std::move(v)); }
  Attrs&& placeholder(Value v) && { return std::move(*this).attr("placeholder", std::move(v)); }
  Attrs&& autocomplete(Value v) && { return std::move(*this).attr("autocomplete", std::move(v)); }
  Attrs&& tabindex(Value v) && { return std::move(*this).attr("tabindex", std::move(v)); }
  Attrs&& rows(Value v) && { return std::move(*this).attr("rows", std::move(v)); }
  Attrs&& maxlength(Value v) && { return std::move(*this).attr("maxlength", std::move(v)); }
  // `image_tag`'s `size:` option ("20" or "20x30").
  Attrs&& size(Value v) && { return std::move(*this).attr("size", std::move(v)); }
  // `button_to`'s `method:` option.
  Attrs&& method(Value v) && { return std::move(*this).attr("method", std::move(v)); }
  Attrs&& hidden() && { return std::move(*this).attr("hidden", true); }
  Attrs&& required(bool v) && { return std::move(*this).attr("required", v); }
  Attrs&& autofocus() && { return std::move(*this).attr("autofocus", true); }
  Attrs&& disabled(bool v) && { return std::move(*this).attr("disabled", v); }
  Attrs&& checked(bool v) && { return std::move(*this).attr("checked", v); }

  // `options[name] ||= value`.
  void set_default(std::string_view name, std::optional<Value> value);
  // `options.fetch(name) { value }`: sets only when the key is absent.
  void fetch_or_set(std::string_view name, std::optional<Value> value);
  [[nodiscard]] const Value* get(std::string_view name) const;
  [[nodiscard]] bool has(std::string_view name) const;
  std::optional<Value> remove(std::string_view name);
  // `Hash#merge`: other's entries override in place.
  [[nodiscard]] Attrs merged(const Attrs& other) const&;
  Attrs&& merge(Attrs other) &&;

  // `link_to(url, **attributes, data: defaults.merge(attributes.delete(:data)))`: the default
  // `data-*` entries go where the first `data-*` entry of the caller is (or at the end), before
  // the caller's own, which override them.
  [[nodiscard]] Attrs with_default_data(const Attrs& defaults) const;

  [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }
  [[nodiscard]] bool empty() const { return entries_.empty(); }

 private:
  std::vector<Entry> entries_;
};

inline Attrs attrs() { return {}; }

// `tag_options`: each attribute with a leading space.
void render_attrs(Out& out, const Attrs& attrs);

// `String#dasherize`.
[[nodiscard]] std::string dasherize(std::string_view key);

// `<name attrs` (left open).
void open_tag(Out& out, std::string_view name, const Attrs& attrs);
void close_tag(Out& out, std::string_view name);

// `content_tag(name, content, options)` with already safe content. A textarea's content starts on
// a new line.
void content_tag(Out& out, std::string_view name, const Attrs& attrs, SafeHtml content);
// The same, with plain text that is escaped.
void content_tag_text(Out& out, std::string_view name, const Attrs& attrs, std::string_view text);
// `content_tag(name, options) do ... end`.
template <BodyFn Body>
void content_tag(Out& out, std::string_view name, const Attrs& attrs, Body&& body) {
  open_tag(out, name, attrs);
  out.append_char('>');
  if (name == "textarea") {
    out.append_char('\n');
  }
  body(out);
  close_tag(out, name);
}

// `tag.name(**options)`: no closing tag for a void element, empty otherwise. `_` in the name is `-`.
void builder_tag(Out& out, std::string_view name, const Attrs& attrs);
// Legacy `tag(:name, options)`: always `" />"`. Used by `image_tag` and the form fields.
void legacy_tag(Out& out, std::string_view name, const Attrs& attrs);

}  // namespace campfire::views::helpers
