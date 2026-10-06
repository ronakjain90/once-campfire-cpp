// Typed SQL statements. Design: plans/architecture.md section 5.
// Rust: crates/db/src/sql.rs (statements with a fixed text).
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace campfire::db {

// Bytes bound as a blob.
struct Blob {
  std::string_view bytes;
};

namespace detail {
inline std::uint32_t next_statement_index() noexcept {
  static std::atomic<std::uint32_t> counter{0};
  return counter.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace detail

// The part of a statement that does not depend on the types: the SQL text and the index in the
// per-connection array of prepared statements.
class QueryBase {
 public:
  [[nodiscard]] std::string_view sql() const noexcept { return sql_; }
  [[nodiscard]] std::uint32_t index() const noexcept { return index_; }

 protected:
  explicit QueryBase(std::string_view sql) noexcept : sql_(sql), index_(detail::next_statement_index()) {}

 private:
  std::string_view sql_;
  std::uint32_t index_;
};

template <class Sig>
class Query;

// `inline const Query<RoomRow(std::int64_t, std::int64_t)> RoomForUser{"SELECT ..."};`
// `Row` is the type of one result row (`void` for a statement with no rows). `Args` are the types
// of the `?` parameters, in order. The index is taken once, when the program starts. It is not
// `constexpr` because the number comes from a counter at run time.
template <class Row, class... Args>
class Query<Row(Args...)> : public QueryBase {
 public:
  using row_type = Row;
  explicit Query(std::string_view sql) noexcept : QueryBase(sql) {}
};

}  // namespace campfire::db
