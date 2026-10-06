// Per-worker session cache and the change hub. Design: plans/architecture.md section 6.
#include "app/session_cache.hpp"

#include <algorithm>

namespace campfire::app {

namespace {
bool cached_table(db::schema::Table table) noexcept {
  return table == db::schema::Table::Sessions || table == db::schema::Table::Users;
}
}  // namespace

void ChangeInbox::post(std::span<const db::Change> changes) {
  const std::lock_guard lock(mutex_);
  changes_.insert(changes_.end(), changes.begin(), changes.end());
  pending_.store(true, std::memory_order_release);
}

std::vector<db::Change> ChangeInbox::take() {
  const std::lock_guard lock(mutex_);
  std::vector<db::Change> out;
  out.swap(changes_);
  pending_.store(false, std::memory_order_release);
  return out;
}

void ChangeHub::add(ChangeInbox* inbox) {
  const std::lock_guard lock(mutex_);
  inboxes_.push_back(inbox);
}

void ChangeHub::remove(ChangeInbox* inbox) {
  const std::lock_guard lock(mutex_);
  std::erase(inboxes_, inbox);
}

std::size_t ChangeHub::size() const {
  const std::lock_guard lock(mutex_);
  return inboxes_.size();
}

void ChangeHub::publish(std::span<const db::Change> changes) {
  std::vector<db::Change> relevant;
  bool users_changed = false;
  for (const db::Change& change : changes) {
    users_changed = users_changed || change.table == db::schema::Table::Users;
    if (cached_table(change.table)) relevant.push_back(change);
  }
  if (users_changed) users_generation_.fetch_add(1, std::memory_order_acq_rel);
  if (relevant.empty()) return;
  const std::lock_guard lock(mutex_);
  for (ChangeInbox* inbox : inboxes_) inbox->post(relevant);
}

std::shared_ptr<const AuthEntry> SessionCache::find(std::string_view cookie_value) const {
  const auto it = entries_.find(cookie_value);
  return it == entries_.end() ? nullptr : it->second;
}

void SessionCache::put(std::string cookie_value, std::shared_ptr<const AuthEntry> entry) {
  if (entries_.size() >= max_entries_) entries_.clear();
  entries_[std::move(cookie_value)] = std::move(entry);
}

void SessionCache::invalidate(std::span<const db::Change> changes) {
  for (auto it = entries_.begin(); it != entries_.end();) {
    bool drop = false;
    for (const db::Change& change : changes) {
      if (change.table == db::schema::Table::Sessions && change.id == it->second->session.id) drop = true;
      if (change.table == db::schema::Table::Users && change.id == it->second->user.id) drop = true;
    }
    it = drop ? entries_.erase(it) : std::next(it);
  }
}

}  // namespace campfire::app
