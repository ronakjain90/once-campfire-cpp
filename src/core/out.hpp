// Chunked output buffer of a response body. Matches the "Out" object in docs/architecture.md
// section 7.1.
#pragma once

#include <sys/uio.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/arena.hpp"
#include "core/html.hpp"

namespace campfire {

// An append-only buffer made of chunks. A write never moves bytes that are already written, so
// the buffer has no big copy when it grows. `fill_iovecs` gives the chunks to `writev` with no
// copy. `contiguous` and `to_string` make one block when a caller needs it.
//
// Text goes in with `append(SafeHtml)` (already safe), `html_escape(out, text)` (escapes), or
// `append_raw` (bytes that are not HTML text: headers, JSON, numbers). There is no
// `append(std::string_view)`, so a program cannot write unescaped text into HTML by mistake.
//
// The chunks live in a `std::pmr::memory_resource`. Use the arena of the request:
// `Out out{ctx.arena().resource()}`. An `Out` is not thread-safe.
class Out {
 public:
  static constexpr std::size_t kFirstChunk = 1024;
  static constexpr std::size_t kMaxGrowChunk = std::size_t{64} * 1024;

  explicit Out(std::pmr::memory_resource* resource = std::pmr::get_default_resource(),
               std::size_t first_chunk = kFirstChunk);
  Out(const Out&) = delete;
  Out& operator=(const Out&) = delete;
  Out(Out&& other) noexcept;
  Out& operator=(Out&&) = delete;
  ~Out();

  // Writes already-safe HTML.
  void append(SafeHtml html) { append_raw(html.view()); }
  // Writes bytes with no change and no escape. Do not use it for text that goes in HTML.
  void append_raw(std::string_view bytes);
  void append_char(char c);
  // Writes the decimal text of a number, as Ruby `Integer#to_s` does.
  void append_int(std::int64_t value);
  void append_uint(std::uint64_t value);

  // Returns a writable block of at least `n` bytes at the end of the buffer. Write at most
  // `n` bytes into it, then call `commit(written)`. Do not call another member between the two.
  [[nodiscard]] std::span<char> reserve(std::size_t n);
  void commit(std::size_t written) noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] std::size_t chunk_count() const noexcept { return chunks_.size(); }

  // Gives up all bytes and chunks. The buffer is empty after this call.
  void clear();

  // Writes iovecs for the bytes from offset `skip` on, for `writev`. Writes at most
  // `out.size()` entries and returns the number written. After a short write of `n` bytes, call
  // again with `skip += n`. The iovecs are valid until the next write to this `Out`.
  std::size_t fill_iovecs(std::span<iovec> out, std::size_t skip = 0) const noexcept;
  // The same, for all bytes, in a new vector.
  [[nodiscard]] std::vector<iovec> iovecs() const;

  // Copies all bytes to `dst`, which must have room for `size()` bytes.
  void copy_to(char* dst) const noexcept;
  // Copies all bytes into one block in `arena`. The view is valid until the arena resets.
  [[nodiscard]] std::string_view contiguous(Arena& arena) const;
  // Copies all bytes into a new string.
  [[nodiscard]] std::string to_string() const;

 private:
  struct Chunk {
    char* data;
    std::size_t used;
    std::size_t capacity;
  };

  void add_chunk(std::size_t at_least);
  void release_chunks() noexcept;

  std::pmr::memory_resource* resource_;
  std::pmr::vector<Chunk> chunks_;
  std::size_t size_ = 0;
  std::size_t next_chunk_;
};

}  // namespace campfire
