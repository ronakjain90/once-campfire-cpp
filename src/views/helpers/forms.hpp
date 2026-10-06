// `form_with` and its builder, `button_to`, `hidden_field_tag`, `button_tag`. Rust:
// crates/views/src/helpers/forms.rs. Rails: form_helper.rb, form_tag_helper.rb, tags/*.rb,
// url_helper.rb#button_to. No `authenticity_token` field: forgery protection uses
// `Sec-Fetch-Site` (Rust "Known differences").
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "views/helpers/tag.hpp"

namespace campfire::views::helpers {

// The hidden `_method` field.
void method_tag(Out& out, std::string_view method);

// `form_with(url:, model:, method:, id:, class:, data:)` and the field methods of its builder.
// The `<form>` tag is written after the body, because a `file_field` in the body sets multipart.
class FormWith {
 public:
  explicit FormWith(std::string_view url) : action_(url) {}

  // `model:`: the param key that the field names go under ("user").
  FormWith& model(std::string_view param_key) &;
  FormWith&& model(std::string_view param_key) && { return std::move(model(param_key)); }
  // `method:`; a persisted model implies "patch", so pass it for those too.
  FormWith&& method(std::string_view m) && {
    method_ = m;
    return std::move(*this);
  }
  FormWith&& id(std::string_view v) && {
    id_ = v;
    return std::move(*this);
  }
  FormWith&& cls(std::string_view v) && {
    class_ = v;
    return std::move(*this);
  }
  FormWith&& data(std::string_view key, Value v) && {
    data_.attr("data-" + dasherize(key), std::move(v));
    return std::move(*this);
  }
  // `auto_submit_form_with`: puts the auto-submit Stimulus controller first.
  FormWith&& auto_submit() &&;
  FormWith&& multipart() && {
    multipart_ = true;
    return std::move(*this);
  }

  // The `<form>` tag and the `_method` field.
  void open(Out& out) const;
  // `fields_for(:settings)`.
  [[nodiscard]] FormWith fields_for(std::string_view name) const;

  void text_field(Out& out, std::string_view method, std::optional<std::string_view> value, Attrs options) const;
  void email_field(Out& out, std::string_view method, std::optional<std::string_view> value, Attrs options) const;
  void url_field(Out& out, std::string_view method, std::optional<std::string_view> value, Attrs options) const;
  // A password field never writes the value of the model.
  void password_field(Out& out, std::string_view method, Attrs options) const;
  void hidden_field(Out& out, std::string_view method, std::optional<std::string_view> value, Attrs options) const;
  void file_field(Out& out, std::string_view method, Attrs options) const;
  void text_area(Out& out, std::string_view method, std::optional<std::string_view> value, Attrs options) const;
  // A hidden field with the unchecked value, then the checkbox. `current` is compared with `checked_value`.
  void check_box(Out& out, std::string_view method, Attrs options, std::string_view checked_value,
                 std::string_view unchecked_value, std::string_view current) const;

  // Used by `form_with` below.
  [[nodiscard]] bool is_multipart() const { return multipart_; }

 private:
  [[nodiscard]] std::string tag_name(std::string_view method) const;
  [[nodiscard]] std::string tag_id(std::string_view method) const;
  void default_name_and_id(std::string_view method, Attrs& options) const;
  void input_field(Out& out, std::string_view type, std::string_view method, std::optional<Value> value,
                   Attrs options) const;

  std::string action_;
  std::string method_ = "post";
  std::string object_name_;
  std::optional<std::string> id_;
  std::optional<std::string> class_;
  Attrs data_;
  mutable bool multipart_ = false;
};

inline FormWith form_with_url(std::string_view url) {
  return FormWith(url);
}

// `form_with(...) do |form| ... end`.
template <BodyFn Body>
void form_with(Out& out, const FormWith& form, Body&& body) {
  Out inner;
  body(inner);  // first, so that a file field can set multipart
  form.open(out);
  append_out(out, inner);
  out.append_raw("</form>");
}

// `button_tag`'s attributes: `{ name: "button", type: "submit" }` merged with the options.
[[nodiscard]] Attrs button_options(const Attrs& options);

// `form.button(options) { ... }` / `button_tag`.
template <BodyFn Body>
void button_tag(Out& out, const Attrs& options, Body&& body) {
  content_tag(out, "button", button_options(options), std::forward<Body>(body));
}

// `hidden_field_tag(name, value, options)`.
void hidden_field_tag(Out& out, std::string_view name, std::optional<std::string_view> value, Attrs options);

// Opens the form of `button_to` and the button.
void open_button_to(Out& out, std::string_view url, Attrs options);
void close_button_to(Out& out);

// `button_to(url, options) do ... end`. `options` may have `method` ("delete", "put", "patch",
// "post" or "get"), `form_class`, and the attributes of the button.
template <BodyFn Body>
void button_to(Out& out, std::string_view url, Attrs options, Body&& body) {
  open_button_to(out, url, std::move(options));
  body(out);
  close_button_to(out);
}

void button_to(Out& out, std::string_view url, Attrs options, SafeHtml content);

}  // namespace campfire::views::helpers
