// Mime types and Accept negotiation; Rust: crates/kit/src/format.rs. Rails: action_dispatch/http/mime_type.rb.
#include "req/format.hpp"

#include <algorithm>
#include <cmath>

#include "compat/ruby.hpp"

namespace campfire::req {

namespace mime {
// action_dispatch/http/mime_types.rb, then turbo-rails' :turbo_stream.
const Mime HTML{"html", "text/html", {"application/xhtml+xml"}, {"xhtml"}};
const Mime TEXT{"text", "text/plain", {}, {"txt"}};
const Mime JS{"js", "text/javascript", {"application/javascript", "application/x-javascript"}, {}};
const Mime CSS{"css", "text/css", {}, {}};
const Mime ICS{"ics", "text/calendar", {}, {}};
const Mime CSV{"csv", "text/csv", {}, {}};
const Mime VCF{"vcf", "text/vcard", {}, {}};
const Mime VTT{"vtt", "text/vtt", {}, {"vtt"}};
const Mime MD{"md", "text/markdown", {}, {"md", "markdown"}};
const Mime PNG{"png", "image/png", {}, {"png"}};
const Mime JPEG{"jpeg", "image/jpeg", {}, {"jpg", "jpeg", "jpe", "pjpeg"}};
const Mime GIF{"gif", "image/gif", {}, {"gif"}};
const Mime BMP{"bmp", "image/bmp", {}, {"bmp"}};
const Mime TIFF{"tiff", "image/tiff", {}, {"tif", "tiff"}};
const Mime SVG{"svg", "image/svg+xml", {}, {}};
const Mime WEBP{"webp", "image/webp", {}, {"webp"}};
const Mime MPEG{"mpeg", "video/mpeg", {}, {"mpg", "mpeg", "mpe"}};
const Mime MP3{"mp3", "audio/mpeg", {}, {"mp1", "mp2", "mp3"}};
const Mime OGG{"ogg", "audio/ogg", {}, {"oga", "ogg", "spx", "opus"}};
const Mime M4A{"m4a", "audio/aac", {"audio/mp4"}, {"m4a", "mpg4", "aac"}};
const Mime WEBM{"webm", "video/webm", {}, {"webm"}};
const Mime MP4{"mp4", "video/mp4", {}, {"mp4", "m4v"}};
const Mime OTF{"otf", "font/otf", {}, {"otf"}};
const Mime TTF{"ttf", "font/ttf", {}, {"ttf"}};
const Mime WOFF{"woff", "font/woff", {}, {"woff"}};
const Mime WOFF2{"woff2", "font/woff2", {}, {"woff2"}};
const Mime XML{"xml", "application/xml", {"text/xml", "application/x-xml"}, {}};
const Mime RSS{"rss", "application/rss+xml", {}, {}};
const Mime ATOM{"atom", "application/atom+xml", {}, {}};
const Mime YAML{"yaml", "application/x-yaml", {"text/yaml"}, {"yml", "yaml"}};
const Mime MULTIPART_FORM{"multipart_form", "multipart/form-data", {}, {}};
const Mime URL_ENCODED_FORM{"url_encoded_form", "application/x-www-form-urlencoded", {}, {}};
const Mime JSON{"json", "application/json", {"text/x-json", "application/jsonrequest", "application/problem+json"}, {}};
const Mime PDF{"pdf", "application/pdf", {}, {"pdf"}};
const Mime ZIP{"zip", "application/zip", {}, {"zip"}};
const Mime GZIP{"gzip", "application/gzip", {"application/x-gzip"}, {"gz"}};
const Mime TURBO_STREAM{"turbo_stream", "text/vnd.turbo-stream.html", {}, {}};
const Mime ALL{"*/*", "*/*", {}, {}};

std::span<const Mime* const> registered() {
  static const Mime* const kAll[] = {&HTML, &TEXT, &JS, &CSS, &ICS, &CSV, &VCF, &VTT, &MD, &PNG, &JPEG, &GIF, &BMP,
                                     &TIFF, &SVG, &WEBP, &MPEG, &MP3, &OGG, &M4A, &WEBM, &MP4, &OTF, &TTF, &WOFF,
                                     &WOFF2, &XML, &RSS, &ATOM, &YAML, &MULTIPART_FORM, &URL_ENCODED_FORM, &JSON,
                                     &PDF, &ZIP, &GZIP, &TURBO_STREAM};
  return kAll;
}
}  // namespace mime

namespace {

bool contains(const std::vector<std::string_view>& v, std::string_view s) { return std::ranges::find(v, s) != v.end(); }

// Mime::Type#match?: the pattern appears in the type or in a synonym.
bool matches(const Mime& m, std::string_view pattern) {
  if (m.string.find(pattern) != std::string_view::npos) return true;
  return std::ranges::any_of(m.synonyms, [&](std::string_view s) { return s.find(pattern) != std::string_view::npos; });
}

Format lookup_exact(std::string_view s) {
  for (const Mime* m : mime::registered()) {
    if (m->string == s || contains(m->synonyms, s)) return m;
  }
  return nullptr;
}

std::string_view rtrim(std::string_view s) {
  while (!s.empty() && (s.back() == ' ' || (s.back() >= 9 && s.back() <= 13) || s.back() == '\0')) s.remove_suffix(1);
  return s;
}
std::string_view ltrim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || (s.front() >= 9 && s.front() <= 13) || s.front() == '\0')) s.remove_prefix(1);
  return s;
}
std::string_view trim(std::string_view s) { return rtrim(ltrim(s)); }

bool alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// Loosely Mime::Type::MIME_REGEXP: type/subtype of name characters with optional parameters.
bool valid_mime_type(std::string_view string) {
  std::string_view base = rtrim(string.substr(0, string.find(';')));
  if (base == "*/*") return true;
  auto name_ok = [](std::string_view s) {
    return !s.empty() && s.size() <= 127 && alnum(s[0]) &&
           std::ranges::all_of(s, [](char c) { return alnum(c) || std::string_view("!#$&-^_.+").find(c) != std::string_view::npos; });
  };
  std::size_t slash = base.find('/');
  if (slash == std::string_view::npos) return false;
  std::string_view kind = base.substr(0, slash);
  std::string_view sub = base.substr(slash + 1);
  return name_ok(kind) && (sub == "*" || name_ok(sub));
}

struct AcceptItem {
  std::size_t index;
  std::string name;
  double q;  // (q.to_f * 100).to_i kept in a double so that huge values still order as Integers do
};

// PARAMETER_SEPARATOR_REGEXP = /;\s*q="?/: where the first one starts and ends.
bool find_q_separator(std::string_view s, std::size_t& start, std::size_t& end) {
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] != ';') continue;
    std::size_t j = i + 1;
    while (j < s.size() && (s[j] == ' ' || (s[j] >= 9 && s[j] <= 13))) ++j;
    if (j + 1 >= s.size() || s[j] != 'q' || s[j + 1] != '=') continue;
    start = i;
    end = (j + 2 < s.size() && s[j + 2] == '"') ? j + 3 : j + 2;
    return true;
  }
  return false;
}

// String#split with the q separator. It drops trailing empty fields.
std::vector<std::string_view> split_q(std::string_view s) {
  std::vector<std::string_view> fields;
  std::string_view rest = s;
  std::size_t a = 0;
  std::size_t b = 0;
  while (find_q_separator(rest, a, b)) {
    fields.push_back(rest.substr(0, a));
    rest = rest.substr(b);
  }
  fields.push_back(rest);
  while (!fields.empty() && fields.back().empty()) fields.pop_back();
  return fields;
}

// ACCEPT_HEADER_REGEXP = /[^,\s"](?:[^,"]|"[^"]*")*/
std::vector<std::string_view> scan_accept_items(std::string_view h) {
  std::vector<std::string_view> items;
  std::size_t i = 0;
  while (i < h.size()) {
    char c = h[i];
    if (c == ',' || c == '"' || c == ' ' || (c >= 9 && c <= 13)) {
      ++i;
      continue;
    }
    std::size_t start = i++;
    while (i < h.size() && h[i] != ',') {
      if (h[i] == '"') {
        std::size_t close = h.find('"', i + 1);
        if (close == std::string_view::npos) break;
        i = close + 1;
      } else {
        ++i;
      }
    }
    items.push_back(h.substr(start, i - start));
  }
  return items;
}

// TRAILING_STAR_REGEXP = /^(text|application)\/\*/
bool trailing_star(std::string_view accept, std::vector<Format>& out) {
  std::string_view prefix;
  if (accept.starts_with("text/*")) prefix = "text/";
  else if (accept.starts_with("application/*")) prefix = "application/";
  else return false;
  for (const Mime* m : mime::registered()) {
    if (matches(*m, prefix)) out.push_back(m);
  }
  return true;
}

void sort_xml(std::vector<AcceptItem>& list) {
  auto find = [&](std::string_view name) -> std::ptrdiff_t {
    for (std::size_t i = 0; i < list.size(); ++i) {
      if (list[i].name == name) return static_cast<std::ptrdiff_t>(i);
    }
    return -1;
  };
  std::ptrdiff_t text_xml = find("text/xml");
  std::ptrdiff_t app_xml = find("application/xml");
  if (text_xml >= 0 && app_xml >= 0) {
    auto t = static_cast<std::size_t>(text_xml);
    auto a = static_cast<std::size_t>(app_xml);
    list[a].q = std::max(list[a].q, list[t].q);
    if (a > t) {
      std::swap(list[a], list[t]);
      std::swap(a, t);
    }
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(t));
    app_xml = static_cast<std::ptrdiff_t>(a);
  } else if (text_xml >= 0) {
    list[static_cast<std::size_t>(text_xml)].name = "application/xml";
  }
  if (app_xml >= 0) {
    auto app_idx = static_cast<std::size_t>(app_xml);
    double app_q = list[app_idx].q;
    for (std::size_t idx = app_idx; idx < list.size(); ++idx) {
      if (list[idx].q < app_q) break;
      if (list[idx].name.ends_with("+xml")) {
        std::swap(list[app_idx], list[idx]);
        app_idx = idx;
      }
    }
  }
}

bool browser_like(std::string_view accept) {
  std::string compact;
  for (char c : accept) {
    if (!(c == ' ' || (c >= 9 && c <= 13))) compact.push_back(c);
  }
  return compact.find(",*/*") != std::string::npos || compact.find("*/*,") != std::string::npos;
}

bool valid_accept_header(const NegotiationInput& in) {
  bool present = in.has_accept && !trim(in.accept).empty();
  return (in.xhr && (present || !in.content_type.empty())) || (present && !browser_like(in.accept));
}

Format format_from_path_extension(std::string_view path) {
  std::size_t dot = path.rfind('.');
  if (dot == std::string_view::npos) return nullptr;
  std::string_view ext = path.substr(dot + 1);
  if (ext.empty() || !std::ranges::all_of(ext, [](char c) { return alnum(c) || c == '_'; })) return nullptr;
  return lookup_by_extension(ext);
}

}  // namespace

Format lookup_by_extension(std::string_view extension) {
  for (const Mime* m : mime::registered()) {
    if (m->symbol == extension || contains(m->extensions, extension)) return m;
  }
  return nullptr;
}

std::expected<Format, InvalidMimeType> lookup(std::string_view string) {
  if (Format m = lookup_exact(string)) return m;
  std::string_view base = rtrim(string.substr(0, string.find(';')));
  if (Format m = lookup_exact(base)) return m;
  if (base == "*/*") return &mime::ALL;
  if (valid_mime_type(string)) return nullptr;
  return std::unexpected(InvalidMimeType{std::string(string)});
}

std::expected<std::vector<Format>, InvalidMimeType> parse_accept(std::string_view header) {
  std::vector<Format> formats;
  if (header.find(',') == std::string_view::npos) {
    std::size_t a = 0;
    std::size_t b = 0;
    if (find_q_separator(header, a, b)) header = trim(header.substr(0, a));
    if (trim(header).empty()) return formats;
    if (trailing_star(header, formats)) return formats;
    auto m = lookup(header);
    if (!m) return std::unexpected(m.error());
    if (*m != nullptr) formats.push_back(*m);
    return formats;
  }

  std::vector<AcceptItem> list;
  std::size_t index = 0;
  for (std::string_view item : scan_accept_items(header)) {
    std::vector<std::string_view> fields = split_q(item);
    if (fields.empty()) continue;
    std::string_view params = trim(fields[0]);
    if (params.empty()) continue;
    bool has_q = fields.size() > 1;
    std::vector<std::string> names;
    std::vector<Format> expanded;
    if (trailing_star(params, expanded)) {
      for (Format m : expanded) names.emplace_back(m->string);
    } else {
      names.emplace_back(params);
    }
    for (std::string& name : names) {
      double q = has_q ? compat::to_f(fields[1]) : (name == "*/*" ? 0.0 : 1.0);
      list.push_back(AcceptItem{index++, std::move(name), std::trunc(q * 100.0)});
    }
  }
  // -0.0 ties with 0.0: both are Ruby's 0.
  std::ranges::sort(list, [](const AcceptItem& a, const AcceptItem& b) {
    if (a.q != b.q) return a.q > b.q;
    return a.index < b.index;
  });
  sort_xml(list);
  for (const AcceptItem& item : list) {
    auto m = lookup(item.name);
    if (!m) return std::unexpected(m.error());
    if (*m != nullptr && std::ranges::find(formats, *m) == formats.end()) formats.push_back(*m);
  }
  return formats;
}

std::expected<Format, InvalidMimeType> content_mime_type(std::string_view content_type) {
  if (content_type.empty()) return nullptr;
  std::size_t cut = content_type.find_first_of(",;");
  std::string base(trim(content_type.substr(0, cut)));
  std::ranges::transform(base, base.begin(), [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
  if (base.empty()) return nullptr;
  return lookup(base);
}

std::expected<std::vector<Format>, InvalidMimeType> formats(const NegotiationInput& input) {
  if (input.has_format_param) {
    std::vector<Format> out;
    if (Format m = lookup_by_extension(input.format_param)) out.push_back(m);
    return out;
  }
  if (valid_accept_header(input)) {
    std::string_view accept = trim(input.accept);
    if (accept.empty()) {
      auto m = content_mime_type(input.content_type);
      if (!m) return std::unexpected(m.error());
      std::vector<Format> out;
      if (*m != nullptr) out.push_back(*m);
      return out;
    }
    return parse_accept(accept);
  }
  if (Format m = format_from_path_extension(input.path)) return std::vector<Format>{m};
  return std::vector<Format>{input.xhr ? &mime::JS : &mime::HTML};
}

bool should_apply_vary_header(const NegotiationInput& input) { return !input.has_format_param && valid_accept_header(input); }

Format negotiate(std::span<const Format> formats, std::span<const Format> order) {
  auto offers = [&](Format f) { return std::ranges::find(order, f) != order.end(); };
  for (Format priority : formats) {
    if (priority == &mime::ALL) return order.empty() ? nullptr : order.front();
    if (offers(priority)) return priority;
  }
  if (offers(&mime::ALL)) return formats.empty() ? nullptr : formats.front();
  return nullptr;
}

}  // namespace campfire::req
