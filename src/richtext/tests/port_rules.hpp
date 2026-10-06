// Rules for the documented differences between the Rails corpus and the port. Only the EXPECTED Rails value
// is transformed. The actual output is compared as it is (except for the style format, see canonical_styles).
#pragma once

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace port_rules {

// Rewrites or removes ` <name>="..."` in each tag. `fn` gets the raw value and returns the new value,
// or std::nullopt to drop the attribute.
inline std::string rewrite_attribute(const std::string& html, const std::string& name,
                                     const std::function<std::optional<std::string>(const std::string&)>& fn) {
  const std::string needle = " " + name + "=\"";
  std::string out;
  bool in_tag = false;
  for (std::size_t i = 0; i < html.size();) {
    if (!in_tag && html[i] == '<' && i + 1 < html.size() && html[i + 1] != '/' && html[i + 1] != '!') {
      in_tag = true;
    } else if (in_tag && html[i] == '>') {
      in_tag = false;
    } else if (in_tag && html.compare(i, needle.size(), needle) == 0) {
      std::size_t end = html.find('"', i + needle.size());
      if (end != std::string::npos) {
        auto value = fn(html.substr(i + needle.size(), end - i - needle.size()));
        if (value) out += needle + *value + "\"";
        i = end + 1;
        continue;
      }
    } else if (in_tag && html[i] == '"') {
      std::size_t end = html.find('"', i + 1);
      if (end != std::string::npos) {
        out.append(html, i, end + 1 - i);
        i = end + 1;
        continue;
      }
    }
    out.push_back(html[i++]);
  }
  return out;
}

inline std::string trim_ascii(const std::string& s) {
  std::size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

// The Rust port's `scrub_style` (crates/richtext/src/sanitizer.rs), applied to a Rails style value:
// keep `color` and `background-color` declarations with a plain color value, written as `prop: value;`.
inline std::optional<std::string> port_style(const std::string& style) {
  static const std::regex plain(
      R"(^(?:[a-z]+|#[0-9a-f]{3,8}|var\(\s*--[a-z0-9_-]+\s*\)|(?:rgb|rgba|hsl|hsla)\([0-9a-z.,%\s/+-]*\))$)",
      std::regex::icase);
  std::vector<std::pair<std::string, std::string>> decls;
  std::size_t pos = 0;
  while (pos <= style.size()) {
    std::size_t semi = style.find(';', pos);
    if (semi == std::string::npos) semi = style.size();
    const std::string d = style.substr(pos, semi - pos);
    pos = semi + 1;
    if (trim_ascii(d).empty()) continue;
    const std::size_t colon = d.find(':');
    std::string prop = trim_ascii(colon == std::string::npos ? d : d.substr(0, colon));
    for (char& ch : prop) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    decls.emplace_back(prop, colon == std::string::npos ? "" : trim_ascii(d.substr(colon + 1)));
  }
  auto allowed = [&](const auto& d) {
    return (d.first == "color" || d.first == "background-color") && std::regex_match(d.second, plain);
  };
  // The port keeps a style unchanged when all its declarations are allowed, and writes `prop: value;`
  // otherwise. Rails writes its own CSS form, so the declarations are compared in the canonical form
  // `prop:value;` (see canonical_style) and the format is not compared.
  std::string out;
  for (const auto& d : decls) {
    if (allowed(d)) out += d.first + ":" + d.second + ";";
  }
  if (out.empty()) return std::nullopt;
  return out;
}

// Rewrites every style attribute of `html` to the canonical form `prop:value;` (no value is dropped).
inline std::string canonical_styles(const std::string& html) {
  return rewrite_attribute(html, "style", [](const std::string& v) -> std::optional<std::string> {
    std::string out;
    std::size_t pos = 0;
    while (pos <= v.size()) {
      std::size_t semi = v.find(';', pos);
      if (semi == std::string::npos) semi = v.size();
      const std::string d = v.substr(pos, semi - pos);
      pos = semi + 1;
      if (trim_ascii(d).empty()) continue;
      const std::size_t colon = d.find(':');
      std::string prop = trim_ascii(colon == std::string::npos ? d : d.substr(0, colon));
      for (char& ch : prop) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      out += prop + ":" + (colon == std::string::npos ? "" : trim_ascii(d.substr(colon + 1))) + ";";
    }
    return out;
  });
}

// Rule 1: the port drops `name` attributes.
inline std::string without_names(const std::string& html) {
  return rewrite_attribute(html, "name", [](const std::string&) { return std::nullopt; });
}

}  // namespace port_rules
