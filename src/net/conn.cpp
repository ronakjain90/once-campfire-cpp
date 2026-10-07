// Read buffer of a connection. Rust: crates/kit/src/front/conn.rs (hyper read buffer).
#include "net/conn.hpp"

#include <algorithm>
#include <cstring>

#include "net/h2.hpp"
#include "net/ws_state.hpp"

namespace campfire::net {

// The members that need a whole type (`H2Session`) are in the header, so the two ends live here.
Conn::Conn() = default;
Conn::~Conn() = default;

void ReadBuffer::reserve(std::size_t total) {
  if (capacity_ - begin_ >= total) return;
  if (capacity_ >= total && begin_ != 0) {
    // Enough room if the data moves to the front.
    std::memmove(data_.get(), data_.get() + begin_, end_ - begin_);
    end_ -= begin_;
    begin_ = 0;
    return;
  }
  std::size_t capacity = std::max<std::size_t>(kInitial, capacity_);
  while (capacity < total) capacity *= 2;
  auto fresh = std::make_unique_for_overwrite<char[]>(capacity);
  if (end_ != begin_) std::memcpy(fresh.get(), data_.get() + begin_, end_ - begin_);
  end_ -= begin_;
  begin_ = 0;
  data_ = std::move(fresh);
  capacity_ = capacity;
}

void ReadBuffer::consume(std::size_t n) noexcept {
  begin_ += n;
  if (begin_ >= end_) release();
}

void ReadBuffer::release() noexcept {
  data_.reset();
  capacity_ = begin_ = end_ = 0;
}

}  // namespace campfire::net
