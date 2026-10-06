// Router with a build-time table. Rails: config/routes.rb; the table comes from src/app/routes/*.inc.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "net/ctx.hpp"
#include "net/http.hpp"

namespace campfire::net {

// One part of a route pattern between slashes.
enum class SegmentKind : std::uint8_t {
  Literal,  // "messages": text must be equal
  Param,    // ":room_id": one or more characters, no "." and no "/"
  Prefixed, // "@:message_id": the text "@", then a Param
  Glob,     // "*path": the rest of the path (last segment only)
};

struct Segment {
  SegmentKind kind;
  std::string_view text;  // Literal: the text. Prefixed: the prefix.
  std::string_view name;  // Param, Prefixed, Glob: the name of the parameter
};

struct RouteDefault {
  std::string_view name;
  std::string_view value;
};

struct Route {
  Method method;
  HandlerFn handler;
  std::string_view name;     // "messages::index", for logs
  std::string_view pattern;  // as in the .inc file
  std::span<const Segment> segments;
  bool format;               // the pattern ends with "(.:format)"
  std::span<const RouteDefault> defaults;
};

// The routes that can match a request whose first segment starts with `key` (up to the first ".").
struct Bucket {
  std::string_view key;
  std::span<const std::uint16_t> routes;  // indexes into `RouteTable::routes`, in route order
};

struct MethodIndex {
  std::span<const Bucket> buckets;            // sorted by key
  std::span<const std::uint16_t> fallback;    // for a first segment with no bucket
  std::span<const std::uint16_t> root;        // for the path "/"
};

struct RouteTable {
  std::span<const Route> routes;
  std::array<MethodIndex, kMethodCount> index;
};

struct Match {
  const Route* route = nullptr;
  PathParams params;
  explicit operator bool() const noexcept { return route != nullptr; }
};

// Finds the first route (in file order) that matches. HEAD uses the GET routes. A "." suffix on
// the last segment sets the parameter "format" when the route has "(.:format)". The function does
// not allocate memory.
[[nodiscard]] Match match_route(const RouteTable& table, Method method, std::string_view path) noexcept;

}  // namespace campfire::net
