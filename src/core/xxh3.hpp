// XXH3 128-bit hash (vendored xxhash 0.8.3). Used for the page cache keys (plans/architecture.md section 6.1).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace campfire {

// A 128-bit hash value.
struct Hash128 {
  std::uint64_t low = 0;
  std::uint64_t high = 0;

  // 32 lowercase hex digits, the high half first (the canonical form of xxhsum).
  [[nodiscard]] std::string hex() const;
  [[nodiscard]] bool operator==(const Hash128&) const = default;
};

// XXH3 128-bit of `data`, with seed 0 unless given.
[[nodiscard]] Hash128 xxh3_128(std::string_view data, std::uint64_t seed = 0) noexcept;

// A hash that takes its input in parts. The result is the same as `xxh3_128` of the joined parts.
class Xxh3State {
 public:
  explicit Xxh3State(std::uint64_t seed = 0);
  Xxh3State(Xxh3State&&) noexcept;
  Xxh3State& operator=(Xxh3State&&) noexcept;
  ~Xxh3State();

  void update(std::string_view data) noexcept;
  void update_u64(std::uint64_t value) noexcept;  // the 8 bytes of `value`, little endian
  [[nodiscard]] Hash128 digest() const noexcept;
  void reset(std::uint64_t seed = 0) noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace campfire
