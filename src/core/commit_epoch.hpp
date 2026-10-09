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

// The epoch of the snapshot of the read transaction that runs on this thread, or 0 when none runs. The reader loads the
// epoch before its snapshot starts, so its snapshot holds every commit of that epoch.
inline void set_snapshot_epoch(std::uint64_t epoch) noexcept {
  detail::t_snapshot_epoch = epoch;
}
// True while a read transaction runs on this thread. Only then may a fragment go into a shared cache or come from it:
// rows that were read outside a snapshot can be older than the current epoch, and a key with that epoch would admit
// old HTML (the flaw of once-campfire-elixir #7).
[[nodiscard]] inline bool in_snapshot() noexcept {
  return detail::t_snapshot_epoch != 0;
}
// The epoch for a key: the epoch of the snapshot, else the current epoch.
[[nodiscard]] inline std::uint64_t fragment_epoch() noexcept {
  return detail::t_snapshot_epoch != 0 ? detail::t_snapshot_epoch : commit_epoch();
}

}  // namespace campfire
