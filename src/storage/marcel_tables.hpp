// Marcel 1.1.0 lookup tables (marcel/mime_type/definitions.rb; Rust: crates/storage/src/tables.rs).
// The data is in marcel_tables.cpp, which src/storage/tools/dump_marcel_tables.rb generates.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace campfire::storage::marcel_tables {

// One entry of Marcel::MAGIC: [offset, value, children]. `range_end` is -1 when the offset is
// not a Range.
struct Match {
  int64_t offset;
  int64_t range_end;
  bool has_value;
  std::string_view value;
  std::span<const Match> children;
};

// Marcel::EXTENSIONS: extension => type. Sorted by byte order.
extern const std::span<const std::pair<std::string_view, std::string_view>> kExtensions;
// Marcel::TYPE_EXTS: type => extensions, in table order. Sorted by type.
extern const std::span<const std::pair<std::string_view, std::span<const std::string_view>>> kTypeExts;
// Marcel::TYPE_PARENTS: type => parent types. Sorted by type.
extern const std::span<const std::pair<std::string_view, std::span<const std::string_view>>> kTypeParents;
// Marcel::MAGIC, in lookup order. The first matching entry wins.
extern const std::span<const std::pair<std::string_view, std::span<const Match>>> kMagic;

}  // namespace campfire::storage::marcel_tables
