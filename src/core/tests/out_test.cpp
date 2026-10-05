// Tests of core/out.hpp.
#include "core/out.hpp"

#include <doctest.h>

#include <limits>
#include <string>

using campfire::Arena;
using campfire::Out;
using campfire::SafeHtml;

TEST_CASE("Out appends and joins") {
  Out out(std::pmr::get_default_resource(), 16);
  out.append(SafeHtml::literal("<p>"));
  out.append_raw("text");
  out.append_char('!');
  out.append_int(-42);
  out.append_uint(std::numeric_limits<std::uint64_t>::max());
  out.append_int(std::numeric_limits<std::int64_t>::min());
  const std::string expected = "<p>text!-42" + std::to_string(std::numeric_limits<std::uint64_t>::max()) +
                               std::to_string(std::numeric_limits<std::int64_t>::min());
  CHECK(out.size() == expected.size());
  CHECK(out.to_string() == expected);
  Arena arena;
  CHECK(out.contiguous(arena) == expected);
}

TEST_CASE("Out grows in chunks and gives iovecs") {
  Arena arena;
  Out out(arena.resource(), 16);
  std::string expected;
  for (int i = 0; i < 500; ++i) {
    const std::string piece = "item-" + std::to_string(i) + ";";
    out.append_raw(piece);
    expected += piece;
  }
  CHECK(out.chunk_count() > 1);
  const std::vector<iovec> vecs = out.iovecs();
  std::string joined;
  for (const iovec& v : vecs) {
    joined.append(static_cast<const char*>(v.iov_base), v.iov_len);
  }
  CHECK(joined == expected);

  // A short write: skip the bytes that were sent.
  for (const std::size_t skip : {std::size_t{0}, std::size_t{1}, std::size_t{16}, expected.size() - 1, expected.size()}) {
    std::vector<iovec> rest(out.chunk_count());
    rest.resize(out.fill_iovecs(rest, skip));
    std::string tail;
    for (const iovec& v : rest) {
      tail.append(static_cast<const char*>(v.iov_base), v.iov_len);
    }
    CHECK(tail == expected.substr(skip));
  }
  // A limited span gives only the first entries.
  std::array<iovec, 1> one{};
  CHECK(out.fill_iovecs(one) == 1);
}

TEST_CASE("Out takes a big write without a gap") {
  Out out(std::pmr::get_default_resource(), 16);
  out.append_raw("ab");
  const std::string big(100000, 'z');
  out.append_raw(big);
  CHECK(out.to_string() == "ab" + big);
}

TEST_CASE("Out reserve and commit") {
  Out out;
  auto room = out.reserve(5000);
  CHECK(room.size() >= 5000);
  std::fill_n(room.begin(), 5000, 'q');
  out.commit(5000);
  CHECK(out.size() == 5000);
  CHECK(out.to_string() == std::string(5000, 'q'));
}

TEST_CASE("Out clear, empty and move") {
  Out out;
  CHECK(out.empty());
  CHECK(out.iovecs().empty());
  Arena arena;
  CHECK(out.contiguous(arena).empty());
  out.append_raw("x");
  Out moved(std::move(out));
  CHECK(moved.to_string() == "x");
  CHECK(out.size() == 0);  // NOLINT(bugprone-use-after-move): the moved-from state is specified
  moved.clear();
  CHECK(moved.empty());
  moved.append_raw("y");
  CHECK(moved.to_string() == "y");
}
