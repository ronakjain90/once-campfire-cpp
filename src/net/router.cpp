// Router with a build-time table. Rails: config/routes.rb (ActionDispatch::Journey).
#include "net/router.hpp"

#include <algorithm>

namespace campfire::net {

namespace {

constexpr std::size_t kMaxSegments = 24;

struct Split {
  std::array<std::string_view, kMaxSegments> segments;
  std::size_t count = 0;
  const char* end = nullptr;  // end of the path without a trailing "/"
};

// Splits "/a/b/c" into views. Returns false for a path that no route can match.
bool split_path(std::string_view path, Split& out) noexcept {
  if (path.empty() || path.front() != '/') return false;
  if (path.size() > 1 && path.back() == '/') path.remove_suffix(1);  // Rails ignores one trailing "/"
  out.end = path.data() + path.size();
  path.remove_prefix(1);
  if (path.empty()) return true;  // the root
  while (true) {
    const std::size_t slash = path.find('/');
    const std::string_view part = path.substr(0, slash);
    // Journey squeezes repeated slashes ("//rooms" is "/rooms").
    if (!part.empty()) {
      if (out.count == kMaxSegments) return false;
      out.segments[out.count++] = part;
    }
    if (slash == std::string_view::npos) return true;
    path.remove_prefix(slash + 1);
  }
}

// A parameter value: not empty, no ".". With `allow_format`, a single "." splits off the format.
bool split_value(std::string_view text, bool allow_format, std::string_view& value, std::string_view& format) noexcept {
  const std::size_t dot = text.find('.');
  if (dot == std::string_view::npos) {
    value = text;
    return !text.empty();
  }
  if (!allow_format) return false;
  value = text.substr(0, dot);
  format = text.substr(dot + 1);
  return !value.empty() && !format.empty() && format.find('.') == std::string_view::npos;
}

bool try_route(const Route& route, const Split& split, Match& match) noexcept {
  const std::span<const Segment> pattern = route.segments;
  const bool glob = !pattern.empty() && pattern.back().kind == SegmentKind::Glob;
  if (glob ? split.count < pattern.size() : split.count != pattern.size()) return false;
  PathParams& params = match.params;
  params.clear();
  std::string_view format;
  for (std::size_t i = 0; i < pattern.size(); ++i) {
    const Segment& segment = pattern[i];
    const std::string_view part = split.segments[i];
    const bool allow_format = route.format && i + 1 == pattern.size();
    std::string_view value;
    switch (segment.kind) {
      case SegmentKind::Literal:
        if (part == segment.text) break;
        if (allow_format && part.size() > segment.text.size() + 1 && part.starts_with(segment.text) &&
            part[segment.text.size()] == '.') {
          format = part.substr(segment.text.size() + 1);
          if (format.find('.') == std::string_view::npos) break;
        }
        return false;
      case SegmentKind::Param:
        if (!split_value(part, allow_format, value, format)) return false;
        if (!params.add(segment.name, value)) return false;
        break;
      case SegmentKind::Prefixed:
        if (!part.starts_with(segment.text)) return false;
        if (!split_value(part.substr(segment.text.size()), allow_format, value, format)) return false;
        if (!params.add(segment.name, value)) return false;
        break;
      case SegmentKind::Glob:
        if (!params.add(segment.name, std::string_view(part.data(), static_cast<std::size_t>(split.end - part.data())))) {
          return false;
        }
        break;
    }
  }
  if (!format.empty() && !params.add("format", format)) return false;
  for (const RouteDefault& d : route.defaults) {
    if (!params.has(d.name) && !params.add(d.name, d.value)) return false;
  }
  match.route = &route;
  return true;
}

std::string_view bucket_key(std::string_view first) noexcept { return first.substr(0, first.find('.')); }

Match search(const RouteTable& table, const MethodIndex& index, const Split& split) noexcept {
  Match match;
  std::span<const std::uint16_t> candidates;
  if (split.count == 0) {
    candidates = index.root;
  } else {
    const std::string_view key = bucket_key(split.segments[0]);
    const auto it = std::lower_bound(index.buckets.begin(), index.buckets.end(), key,
                                     [](const Bucket& bucket, std::string_view k) { return bucket.key < k; });
    candidates = (it != index.buckets.end() && it->key == key) ? it->routes : index.fallback;
  }
  for (const std::uint16_t i : candidates) {
    if (try_route(table.routes[i], split, match)) return match;
  }
  return {};
}

}  // namespace

Match match_route(const RouteTable& table, Method method, std::string_view path) noexcept {
  if (method == Method::Other) return {};
  Split split;
  if (!split_path(path, split)) return {};
  Match match = search(table, table.index[static_cast<std::size_t>(method)], split);
  if (!match && method == Method::Head) {
    match = search(table, table.index[static_cast<std::size_t>(Method::Get)], split);
  }
  return match;
}

}  // namespace campfire::net
