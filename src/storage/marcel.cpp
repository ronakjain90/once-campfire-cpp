// See marcel.hpp. Tables: marcel_tables.cpp.
#include "storage/marcel.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

#include "storage/filename.hpp"
#include "storage/marcel_tables.hpp"

namespace campfire::storage::marcel {

namespace tables = marcel_tables;

namespace {

template <class Span>
const typename Span::value_type* find_sorted(const Span& table, std::string_view key) {
  auto it = std::lower_bound(table.begin(), table.end(), key,
                             [](const auto& entry, std::string_view k) { return entry.first < k; });
  return it != table.end() && it->first == key ? &*it : nullptr;
}

std::string lowercase(std::string_view s) {
  std::string out(s);
  // Ruby's String#downcase is Unicode aware. Type and extension names are ASCII.
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::span<const std::string_view> parents(std::string_view type) {
  const auto* entry = find_sorted(tables::kTypeParents, type);
  return entry ? entry->second : std::span<const std::string_view>();
}

// IO#read(length) after skipping `offset` bytes: nullopt at EOF, otherwise up to `length` bytes.
std::optional<std::string_view> read(std::string_view data, int64_t offset, size_t length) {
  if (length == 0) return std::string_view();
  auto off = static_cast<size_t>(offset);
  if (off >= data.size()) return std::nullopt;
  return data.substr(off, length);
}

bool matches_any(std::string_view data, std::span<const tables::Match> matches) {
  for (const auto& m : matches) {
    if (!m.has_value) continue;
    bool hit;
    if (m.range_end >= 0) {
      // io.read(offset.begin); io.read(offset.end - offset.begin + value.bytesize).include?(value)
      auto window = read(data, m.offset, static_cast<size_t>(m.range_end - m.offset) + m.value.size());
      hit = window && (m.value.empty() || window->find(m.value) != std::string_view::npos);
    } else {
      auto bytes = read(data, m.offset, m.value.size());
      hit = bytes && *bytes == m.value;
    }
    if (hit && (m.children.empty() || matches_any(data, m.children))) return true;
  }
  return false;
}

size_t reach(std::span<const tables::Match> matches) {
  size_t best = 0;
  for (const auto& m : matches) {
    size_t value = m.has_value ? m.value.size() : 0;
    size_t own = m.range_end >= 0 ? static_cast<size_t>(m.range_end) + value : static_cast<size_t>(m.offset) + value;
    best = std::max({best, own, reach(m.children)});
  }
  return best;
}

// Declared types are downcased, stripped of parameters, and ignored when binary.
std::optional<std::string> for_declared_type(std::optional<std::string_view> declared) {
  if (!declared) return std::nullopt;
  std::string lower = lowercase(*declared);
  size_t end = lower.find_first_of(";, \t\n\r\v\f");
  std::string media = lower.substr(0, end);
  if (media.find('/') == std::string::npos || media == kBinary) return std::nullopt;
  return media;
}

// most_specific_type(*candidates, BINARY): later candidates win only when they are children of
// the current pick.
std::string most_specific_type(const std::vector<std::optional<std::string>>& candidates) {
  std::vector<std::string> unique;
  auto add = [&](const std::string& c) {
    if (std::find(unique.begin(), unique.end(), c) == unique.end()) unique.push_back(c);
  };
  for (const auto& c : candidates) {
    if (c) add(*c);
  }
  add(std::string(kBinary));
  std::string pick = unique[0];
  for (size_t i = 1; i < unique.size(); ++i) {
    if (is_child(unique[i], pick)) pick = unique[i];
  }
  return pick;
}

}  // namespace

std::optional<std::string_view> by_extension(std::string_view extension) {
  std::string ext = lowercase(extension);
  std::string_view view = ext;
  if (view.starts_with('.')) view.remove_prefix(1);
  const auto* entry = find_sorted(tables::kExtensions, view);
  return entry ? std::optional<std::string_view>(entry->second) : std::nullopt;
}

std::optional<std::string_view> by_path(std::string_view path) { return by_extension(extname(path)); }

std::span<const std::string_view> extensions(std::string_view content_type) {
  const auto* entry = find_sorted(tables::kTypeExts, content_type);
  return entry ? entry->second : std::span<const std::string_view>();
}

bool is_child(std::string_view child, std::string_view parent) {
  if (child == parent) return true;
  for (std::string_view p : parents(child)) {
    if (is_child(p, parent)) return true;
  }
  return false;
}

size_t magic_prefix_len() {
  static const size_t len = [] {
    size_t best = 0;
    for (const auto& [type, matches] : tables::kMagic) best = std::max(best, reach(matches));
    return best;
  }();
  return len;
}

std::optional<std::string> by_magic(std::string_view data) {
  for (const auto& [type, matches] : tables::kMagic) {
    if (matches_any(data, matches)) return lowercase(type);
  }
  return std::nullopt;
}

std::string for_extension(std::string_view extension) {
  std::optional<std::string> type;
  if (auto t = by_extension(extension)) type = std::string(*t);
  return most_specific_type({type});
}

std::string identify(std::string_view data, std::optional<std::string_view> name,
                     std::optional<std::string_view> declared_type) {
  std::optional<std::string> by_name;
  if (name) {
    if (auto t = by_path(*name)) by_name = std::string(*t);
  }
  return most_specific_type({by_magic(data), for_declared_type(declared_type), by_name});
}

}  // namespace campfire::storage::marcel
