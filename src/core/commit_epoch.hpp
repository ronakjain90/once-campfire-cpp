// The commit epoch of the database, for the keys of fragments that do not hash their rows. Rails:
// FragmentCache with `@response_cache_version` (app/models/fragment_cache.rb); Rust: the database generation of
// crates/views fragment keys.
#pragma once

#include <atomic>
#include <cstdint>

namespace campfire {

namespace detail {
inline std::atomic<std::uint64_t> g_commit_epoch{1};
inline thread_local std::uint64_t t_snapshot_epoch = 0;
}  // namespace detail

// The epoch grows when a reader connection sees a commit of another connection (PRAGMA data_version), also a commit
// of another process. A key with the epoch cannot give HTML of an older database after a direct SQL edit.
[[nodiscard]] inline std::uint64_t commit_epoch() noexcept {
  return detail::g_commit_epoch.load(std::memory_order_acquire);
}
// A reader saw a new commit: the epoch that its snapshot may use.
[[nodiscard]] inline std::uint64_t observe_commit() noexcept {
  return detail::g_commit_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
}

// The epoch of the snapshot of the current read transaction of this thread. The reader loads the epoch before its
// snapshot starts, so its snapshot holds every commit of that epoch. Before the first read transaction of the
// thread, the current epoch.
inline void set_snapshot_epoch(std::uint64_t epoch) noexcept {
  detail::t_snapshot_epoch = epoch;
}
[[nodiscard]] inline std::uint64_t fragment_epoch() noexcept {
  return detail::t_snapshot_epoch != 0 ? detail::t_snapshot_epoch : commit_epoch();
}

}  // namespace campfire
