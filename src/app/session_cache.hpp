// The session cache of a worker (architecture section 6). Rails: Authentication#find_session_by_cookie.
// A cache belongs to one worker. The writer thread does not touch it: it posts changes to the
// inbox of each worker, and the worker applies them before its next lookup.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "db/database.hpp"
#include "models/session.hpp"
#include "models/user.hpp"

namespace campfire::app {

// A session and its user. An entry never changes after it is cached.
struct AuthEntry {
  models::Session session;
  models::User user;
};

// Changes that the writer posts to one worker. Any thread can post. Only the worker takes.
class ChangeInbox {
 public:
  void post(std::span<const db::Change> changes);
  [[nodiscard]] bool pending() const noexcept { return pending_.load(std::memory_order_acquire); }
  [[nodiscard]] std::vector<db::Change> take();

 private:
  std::mutex mutex_;
  std::vector<db::Change> changes_;
  std::atomic<bool> pending_{false};
};

// The list of inboxes, one for each worker. `publish` runs on the writer thread.
class ChangeHub {
 public:
  void add(ChangeInbox* inbox);
  void remove(ChangeInbox* inbox);
  // Posts the changes of the tables `sessions` and `users` to every inbox. Other tables are
  // not cached, so they are dropped here.
  void publish(std::span<const db::Change> changes);
  [[nodiscard]] std::size_t size() const;
  // How many commits changed a row of `users`. A cache of data from user rows (the avatar responses) stores the number
  // and drops its entry when the number changes.
  [[nodiscard]] std::uint64_t users_generation() const noexcept {
    return users_generation_.load(std::memory_order_acquire);
  }

 private:
  mutable std::mutex mutex_;
  std::vector<ChangeInbox*> inboxes_;
  std::atomic<std::uint64_t> users_generation_{0};
};

// From the raw `session_token` cookie value to the session row and the user row.
class SessionCache {
 public:
  static constexpr std::size_t kDefaultMaxEntries = 4096;
  explicit SessionCache(std::size_t max_entries = kDefaultMaxEntries) : max_entries_(max_entries) {}

  [[nodiscard]] std::shared_ptr<const AuthEntry> find(std::string_view cookie_value) const;
  // If the cache is full, it drops all entries first.
  void put(std::string cookie_value, std::shared_ptr<const AuthEntry> entry);
  // Drops each entry that a change touches: a `sessions` change by session id, a `users` change by user id.
  void invalidate(std::span<const db::Change> changes);
  // The commit epoch of the snapshot of the request. Another process can delete a session or change a user with
  // direct SQL, and only the epoch shows that commit: a new epoch drops all entries.
  void on_epoch(std::uint64_t epoch) {
    if (epoch == epoch_) return;
    entries_.clear();
    epoch_ = epoch;
  }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

 private:
  struct Hash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
  };
  std::size_t max_entries_;
  std::uint64_t epoch_ = 0;
  std::unordered_map<std::string, std::shared_ptr<const AuthEntry>, Hash, std::equal_to<>> entries_;
};

}  // namespace campfire::app
