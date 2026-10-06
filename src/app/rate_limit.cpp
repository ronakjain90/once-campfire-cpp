// Rate limit counters. Rails: ActionController::RateLimiting. Rust: crates/campfire/src/controllers/sessions.rs.
#include "app/rate_limit.hpp"

namespace campfire::app {

std::uint64_t RateLimiter::increment(std::string_view key, Timestamp now, std::int64_t within_seconds) {
  const std::lock_guard lock(mutex_);
  if (now.seconds != last_sweep_) {  // other counters that ended are dropped at most once a second
    last_sweep_ = now.seconds;
    for (auto it = entries_.begin(); it != entries_.end();) {
      it = it->second.expires_at > now ? std::next(it) : entries_.erase(it);
    }
  } else if (const auto old = entries_.find(std::string(key)); old != entries_.end() && old->second.expires_at <= now) {
    entries_.erase(old);
  }
  auto [it, inserted] = entries_.try_emplace(std::string(key));
  if (inserted) it->second.expires_at = now.plus_seconds(within_seconds);
  return ++it->second.count;
}

std::size_t RateLimiter::size() const {
  const std::lock_guard lock(mutex_);
  return entries_.size();
}

}  // namespace campfire::app
