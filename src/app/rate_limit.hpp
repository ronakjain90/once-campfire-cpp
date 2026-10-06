// `rate_limit to:, within:` counters. Rails: ActionController::RateLimiting, which counts in Rails.cache
// with `increment(key, expires_in: within)`. Like Redis `EXPIRE ... NX`, the expiry is set only when the
// counter starts: the window is fixed, it does not slide. One process holds all counters.
// Rust: crates/campfire/src/controllers/sessions.rs (RATE_LIMITS).
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "core/timestamp.hpp"

namespace campfire::app {

class RateLimiter {
 public:
  // Adds one to the counter of `key` and returns the new count. A counter whose window ended starts again at 1.
  [[nodiscard]] std::uint64_t increment(std::string_view key, Timestamp now, std::int64_t within_seconds);
  [[nodiscard]] std::size_t size() const;

 private:
  struct Entry {
    std::uint64_t count = 0;
    Timestamp expires_at;
  };
  mutable std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  std::int64_t last_sweep_ = -1;
};

}  // namespace campfire::app
