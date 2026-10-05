// See filename.hpp.
#include "storage/filename.hpp"

#include "compat/ruby.hpp"

namespace campfire::storage {

namespace {

// Length of the valid UTF-8 sequence at the start of `s`, or the length of the invalid prefix
// (1 to 3) with `valid` false.
size_t utf8_step(std::string_view s, bool& valid) {
  auto b = [&](size_t i) { return static_cast<unsigned char>(s[i]); };
  valid = true;
  unsigned char c = b(0);
  if (c < 0x80) return 1;
  size_t need = 0;
  unsigned char lo = 0x80, hi = 0xBF;
  if (c >= 0xC2 && c <= 0xDF) need = 1;
  else if (c == 0xE0) { need = 2; lo = 0xA0; }
  else if (c >= 0xE1 && c <= 0xEC) need = 2;
  else if (c == 0xED) { need = 2; hi = 0x9F; }
  else if (c >= 0xEE && c <= 0xEF) need = 2;
  else if (c == 0xF0) { need = 3; lo = 0x90; }
  else if (c >= 0xF1 && c <= 0xF3) need = 3;
  else if (c == 0xF4) { need = 3; hi = 0x8F; }
  else { valid = false; return 1; }
  size_t i = 1;
  for (; i <= need; ++i) {
    if (i >= s.size()) { valid = false; return i; }
    unsigned char lower = i == 1 ? lo : 0x80, upper = i == 1 ? hi : 0xBF;
    if (b(i) < lower || b(i) > upper) { valid = false; return i; }
  }
  return need + 1;
}

}  // namespace

std::string utf8_lossy(std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size());
  while (!bytes.empty()) {
    bool valid = true;
    size_t n = utf8_step(bytes, valid);
    if (valid) out.append(bytes.substr(0, n));
    else out.append("\xEF\xBF\xBD");
    bytes.remove_prefix(n);
  }
  return out;
}

Filename Filename::from_bytes(std::string_view bytes) { return Filename(utf8_lossy(bytes)); }

std::string_view basename(std::string_view path) {
  size_t end = path.size();
  while (end > 0 && path[end - 1] == '/') --end;
  if (end == 0) return path.empty() ? std::string_view() : std::string_view("/");
  std::string_view trimmed = path.substr(0, end);
  size_t slash = trimmed.rfind('/');
  return slash == std::string_view::npos ? trimmed : trimmed.substr(slash + 1);
}

std::string_view extname(std::string_view path) {
  std::string_view base = basename(path);
  size_t lead = 0;
  while (lead < base.size() && base[lead] == '.') ++lead;
  std::string_view rest = base.substr(lead);
  size_t dot = rest.rfind('.');
  return dot == std::string_view::npos ? std::string_view() : rest.substr(dot);
}

std::string_view Filename::extension_with_delimiter() const { return extname(raw_); }

std::string_view Filename::extension() const {
  std::string_view ext = extension_with_delimiter();
  return ext.empty() ? ext : ext.substr(1);
}

std::string_view Filename::base() const {
  std::string_view base = basename(raw_);
  std::string_view ext = extension_with_delimiter();
  if (!ext.empty() && base.size() > ext.size() && base.ends_with(ext)) base.remove_suffix(ext.size());
  return base;
}

std::string Filename::sanitized() const {
  std::string text = utf8_lossy(raw_);
  std::string_view stripped = compat::strip(text);
  std::string out;
  out.reserve(stripped.size());
  for (size_t i = 0; i < stripped.size(); ++i) {
    char c = stripped[i];
    bool bad = std::string_view("%$|:;/<>?*\"\t\r\n\\").find(c) != std::string_view::npos && c != '\0';
    // U+202E, the right-to-left override.
    if (c == '\xE2' && stripped.substr(i, 3) == "\xE2\x80\xAE") {
      out.push_back('-');
      i += 2;
    } else {
      out.push_back(bad ? '-' : c);
    }
  }
  return out;
}

}  // namespace campfire::storage
