// Matches the "Out" object in docs/architecture.md section 7.1.
#include "core/out.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>

namespace campfire {

Out::Out(std::pmr::memory_resource* resource, std::size_t first_chunk)
    : resource_(resource), chunks_(resource), next_chunk_(std::max<std::size_t>(first_chunk, 16)) {}

Out::Out(Out&& other) noexcept
    : resource_(other.resource_),
      chunks_(std::move(other.chunks_)),
      size_(std::exchange(other.size_, 0)),
      next_chunk_(other.next_chunk_) {
  other.chunks_.clear();
}

Out::~Out() {
  release_chunks();
}

void Out::release_chunks() noexcept {
  for (const Chunk& chunk : chunks_) {
    resource_->deallocate(chunk.data, chunk.capacity, 1);
  }
  chunks_.clear();
}

void Out::clear() {
  release_chunks();
  size_ = 0;
}

void Out::add_chunk(std::size_t at_least) {
  const std::size_t capacity = std::max(at_least, next_chunk_);
  char* data = static_cast<char*>(resource_->allocate(capacity, 1));
  try {
    chunks_.push_back(Chunk{data, 0, capacity});
  } catch (...) {
    resource_->deallocate(data, capacity, 1);
    throw;
  }
  next_chunk_ = std::min(std::max(next_chunk_, capacity) * 2, kMaxGrowChunk);
}

std::span<char> Out::reserve(std::size_t n) {
  if (chunks_.empty() || chunks_.back().capacity - chunks_.back().used < n) {
    add_chunk(n);
  }
  Chunk& last = chunks_.back();
  return {last.data + last.used, last.capacity - last.used};
}

void Out::commit(std::size_t written) noexcept {
  chunks_.back().used += written;
  size_ += written;
}

void Out::append_raw(std::string_view bytes) {
  if (bytes.empty()) {
    return;
  }
  // Fill the free space of the last chunk first, so a big write does not leave a gap.
  if (!chunks_.empty()) {
    Chunk& last = chunks_.back();
    const std::size_t room = std::min(last.capacity - last.used, bytes.size());
    std::memcpy(last.data + last.used, bytes.data(), room);
    last.used += room;
    size_ += room;
    bytes.remove_prefix(room);
    if (bytes.empty()) {
      return;
    }
  }
  add_chunk(bytes.size());
  Chunk& last = chunks_.back();
  std::memcpy(last.data, bytes.data(), bytes.size());
  last.used = bytes.size();
  size_ += bytes.size();
}

void Out::append_char(char c) {
  const std::span<char> room = reserve(1);
  room[0] = c;
  commit(1);
}

void Out::append_int(std::int64_t value) {
  std::array<char, 24> text{};
  const auto result = std::to_chars(text.data(), text.data() + text.size(), value);
  append_raw({text.data(), static_cast<std::size_t>(result.ptr - text.data())});
}

void Out::append_uint(std::uint64_t value) {
  std::array<char, 24> text{};
  const auto result = std::to_chars(text.data(), text.data() + text.size(), value);
  append_raw({text.data(), static_cast<std::size_t>(result.ptr - text.data())});
}

std::size_t Out::fill_iovecs(std::span<iovec> out, std::size_t skip) const noexcept {
  std::size_t count = 0;
  for (const Chunk& chunk : chunks_) {
    if (count == out.size()) {
      break;
    }
    std::size_t offset = 0;
    if (skip > 0) {
      offset = std::min(skip, chunk.used);
      skip -= offset;
    }
    if (offset == chunk.used) {
      continue;
    }
    out[count].iov_base = chunk.data + offset;
    out[count].iov_len = chunk.used - offset;
    ++count;
  }
  return count;
}

std::vector<iovec> Out::iovecs() const {
  std::vector<iovec> result(chunks_.size());
  result.resize(fill_iovecs(result));
  return result;
}

void Out::copy_to(char* dst) const noexcept {
  for (const Chunk& chunk : chunks_) {
    if (chunk.used > 0) {
      std::memcpy(dst, chunk.data, chunk.used);
      dst += chunk.used;
    }
  }
}

std::string_view Out::contiguous(Arena& arena) const {
  if (size_ == 0) {
    return {};
  }
  char* data = static_cast<char*>(arena.allocate(size_, 1));
  copy_to(data);
  return {data, size_};
}

std::string Out::to_string() const {
  std::string result(size_, '\0');
  copy_to(result.data());
  return result;
}

}  // namespace campfire
