// Form helpers (Rust: crates/views/src/helpers/forms.rs).
#include "views/helpers/forms.hpp"

#include <algorithm>
#include <cctype>

namespace campfire::views::helpers {
namespace {

// `object_name.gsub(/\]\[|[^-a-zA-Z0-9:.]/, "_").delete_suffix("_")`.
std::string sanitize_object_name(std::string_view name) {
  std::string out;
  for (std::size_t i = 0; i < name.size(); ++i) {
    if (name.substr(i, 2) == "][") {
      out += '_';
      ++i;
      continue;
    }
    const char c = name[i];
    const bool keep = (std::isalnum(static_cast<unsigned char>(c)) != 0 && static_cast<unsigned char>(c) < 128) ||
                      c == '-' || c == ':' || c == '.';
    out += keep ? c : '_';
  }
  if (out.ends_with('_')) {
    out.pop_back();
  }
  return out;
}

// `sanitize_to_id`: `]` removed, other characters outside `[-a-zA-Z0-9_:.]` become "_".
std::string sanitize_to_id(std::string_view name) {
  std::string out;
  for (const char c : name) {
    if (c == ']') {
      continue;
    }
    const bool keep = (std::isalnum(static_cast<unsigned char>(c)) != 0 && static_cast<unsigned char>(c) < 128) ||
                      c == '-' || c == '_' || c == ':' || c == '.';
    out += keep ? c : '_';
  }
  return out;
}

}  // namespace

void method_tag(Out& out, std::string_view method) {
  legacy_tag(out, "input", attrs().type("hidden").name("_method").value(method));
}

FormWith& FormWith::model(std::string_view param_key) & {
  object_name_ = param_key;
  return *this;
}

FormWith&& FormWith::auto_submit() && {
  const Value* existing = data_.get("data-controller");
  std::string controller = "auto-submit " + (existing != nullptr ? existing->text : std::string());
  while (controller.ends_with(' ')) {
    controller.pop_back();
  }
  data_.set("data-controller", Value(controller));
  return std::move(*this);
}

FormWith FormWith::fields_for(std::string_view name) const {
  FormWith nested = *this;
  nested.object_name_ = object_name_ + "[" + std::string(name) + "]";
  return nested;
}

void FormWith::open(Out& out) const {
  Attrs html;
  if (id_) html.set("id", Value(*id_));
  if (class_) html.set("class", Value(*class_));
  html = html.merged(data_);
  if (multipart_) html.set("enctype", Value("multipart/form-data"));
  std::string method = method_;
  std::ranges::transform(method, method.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  html.set("action", Value(action_));
  html.set("accept-charset", Value("UTF-8"));
  html.set("method", Value(method == "get" ? "get" : "post"));
  open_tag(out, "form", html);
  out.append_char('>');
  if (method != "get" && method != "post" && !method.empty()) {
    method_tag(out, method);
  }
}

std::string FormWith::tag_name(std::string_view method) const {
  return object_name_.empty() ? std::string(method) : object_name_ + "[" + std::string(method) + "]";
}

std::string FormWith::tag_id(std::string_view method) const {
  if (object_name_.empty()) return std::string(method);
  return sanitize_object_name(object_name_) + "_" + std::string(method);
}

void FormWith::default_name_and_id(std::string_view method, Attrs& options) const {
  options.fetch_or_set("name", Value(tag_name(method)));
  options.fetch_or_set("id", Value(tag_id(method)));
}

void FormWith::input_field(Out& out, std::string_view type, std::string_view method, std::optional<Value> value,
                           Attrs options) const {
  if (type == "file") multipart_ = true;
  if (!options.has("size")) {
    const Value* maxlength = options.get("maxlength");
    options.set("size", maxlength != nullptr ? std::optional<Value>(*maxlength) : std::nullopt);
  }
  options.set_default("type", Value(type));
  if (type != "file") options.fetch_or_set("value", std::move(value));
  default_name_and_id(method, options);
  legacy_tag(out, "input", options);
}

namespace {
std::optional<Value> opt_value(std::optional<std::string_view> v) {
  return v ? std::optional<Value>(Value(*v)) : std::nullopt;
}
}  // namespace

void FormWith::text_field(Out& out, std::string_view method, std::optional<std::string_view> value,
                          Attrs options) const {
  input_field(out, "text", method, opt_value(value), std::move(options));
}
void FormWith::email_field(Out& out, std::string_view method, std::optional<std::string_view> value,
                           Attrs options) const {
  input_field(out, "email", method, opt_value(value), std::move(options));
}
void FormWith::url_field(Out& out, std::string_view method, std::optional<std::string_view> value,
                         Attrs options) const {
  input_field(out, "url", method, opt_value(value), std::move(options));
}
void FormWith::password_field(Out& out, std::string_view method, Attrs options) const {
  Attrs merged;
  merged.set("value", std::nullopt);
  input_field(out, "password", method, std::nullopt, std::move(merged).merge(std::move(options)));
}
void FormWith::hidden_field(Out& out, std::string_view method, std::optional<std::string_view> value,
                            Attrs options) const {
  input_field(out, "hidden", method, opt_value(value), std::move(options));
}
void FormWith::file_field(Out& out, std::string_view method, Attrs options) const {
  input_field(out, "file", method, std::nullopt, std::move(options));
}

void FormWith::text_area(Out& out, std::string_view method, std::optional<std::string_view> value,
                         Attrs options) const {
  default_name_and_id(method, options);
  std::optional<Value> own = options.remove("value");
  std::string content = own ? own->to_s() : std::string(value.value_or(""));
  content_tag(out, "textarea", options, [&](Out& o) { html_escape(o, content); });
}

void FormWith::check_box(Out& out, std::string_view method, Attrs options, std::string_view checked_value,
                         std::string_view unchecked_value, std::string_view current) const {
  // `input_checked?` deletes an own `checked` option. Rails writes `checked="checked"` after `value`.
  bool checked = current == checked_value;
  if (const std::optional<Value> own = options.remove("checked")) {
    checked = own->kind == Value::Kind::Bool ? own->flag : own->text == "checked";
  }
  options.set("type", Value("checkbox"));
  options.set("value", Value(checked_value));
  if (checked) options.set("checked", Value("checked"));
  default_name_and_id(method, options);
  Attrs hidden;
  for (const char* key : {"name", "disabled", "form"}) {
    if (options.has(key)) {
      const Value* v = options.get(key);
      hidden.set(key, v != nullptr ? std::optional<Value>(*v) : std::nullopt);
    }
  }
  legacy_tag(out, "input", std::move(hidden).type("hidden").value(unchecked_value));
  legacy_tag(out, "input", options);
}

Attrs button_options(const Attrs& options) {
  Attrs base = attrs().name("button").type("submit");
  return std::move(base).merge(options);
}

void hidden_field_tag(Out& out, std::string_view name, std::optional<std::string_view> value, Attrs options) {
  Attrs base = attrs().type("hidden").name(name).id(sanitize_to_id(name)).attr_opt("value", value);
  legacy_tag(out, "input", std::move(base).merge(std::move(options)));
}

void open_button_to(Out& out, std::string_view url, Attrs options) {
  const std::optional<Value> method_value = options.remove("method");
  const std::optional<Value> class_value = options.remove("form_class");
  const std::string method = method_value ? method_value->to_s() : "post";
  Attrs form = attrs().cls(class_value ? class_value->to_s() : "button_to");
  form.set("method", Value(method == "get" ? "get" : "post"));
  form.set("action", Value(url));
  open_tag(out, "form", form);
  out.append_char('>');
  if (method == "delete" || method == "patch" || method == "put") method_tag(out, method);
  options.set("type", Value("submit"));
  open_tag(out, "button", options);
  out.append_char('>');
}

void close_button_to(Out& out) {
  out.append_raw("</button></form>");
}

void button_to(Out& out, std::string_view url, Attrs options, SafeHtml content) {
  open_button_to(out, url, std::move(options));
  out.append(content);
  close_button_to(out);
}

}  // namespace campfire::views::helpers
