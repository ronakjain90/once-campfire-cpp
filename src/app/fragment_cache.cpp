// Shared fragment cache. Design: docs/architecture.md section 6.
#include "app/fragment_cache.hpp"

#include <functional>

#include "core/out.hpp"

namespace campfire::app {

SharedFragmentCache::SharedFragmentCache(std::size_t max_bytes, std::size_t shard_count)
    : max_bytes_(max_bytes), shard_bytes_(max_bytes / (shard_count == 0 ? 1 : shard_count)) {
  if (shard_count == 0) shard_count = 1;
  for (std::size_t i = 0; i < shard_count; ++i) shards_.push_back(std::make_unique<Shard>());
}

SharedFragmentCache::Shard& SharedFragmentCache::shard_for(std::string_view key) const {
  return *shards_[std::hash<std::string_view>{}(key) % shards_.size()];
}

std::shared_ptr<const std::string> SharedFragmentCache::get(std::string_view key) {
  Shard& shard = shard_for(key);
  const std::lock_guard lock(shard.mutex);
  const auto it = shard.index.find(key);
  if (it == shard.index.end()) return nullptr;
  shard.lru.splice(shard.lru.begin(), shard.lru, it->second);
  return it->second->html;
}

void SharedFragmentCache::put(std::string_view key, std::string html) {
  const std::size_t cost = key.size() + html.size() + kEntryOverhead;
  if (cost > shard_bytes_) return;
  auto value = std::make_shared<const std::string>(std::move(html));
  Shard& shard = shard_for(key);
  const std::lock_guard lock(shard.mutex);
  if (const auto it = shard.index.find(key); it != shard.index.end()) {
    const auto node = it->second;
    shard.bytes -= node->cost;
    shard.index.erase(it);
    shard.lru.erase(node);
  }
  while (!shard.lru.empty() && shard.bytes + cost > shard_bytes_) {
    shard.index.erase(shard.lru.back().key);
    shard.bytes -= shard.lru.back().cost;
    shard.lru.pop_back();
  }
  shard.lru.push_front(Entry{std::string(key), std::move(value), cost});
  shard.index.emplace(std::string_view(shard.lru.front().key), shard.lru.begin());
  shard.bytes += cost;
}

void SharedFragmentCache::clear() {
  for (auto& shard : shards_) {
    const std::lock_guard lock(shard->mutex);
    shard->index.clear();
    shard->lru.clear();
    shard->bytes = 0;
  }
}

std::size_t SharedFragmentCache::bytes() const {
  std::size_t total = 0;
  for (auto& shard : shards_) {
    const std::lock_guard lock(shard->mutex);
    total += shard->bytes;
  }
  return total;
}

std::size_t SharedFragmentCache::entries() const {
  std::size_t total = 0;
  for (auto& shard : shards_) {
    const std::lock_guard lock(shard->mutex);
    total += shard->lru.size();
  }
  return total;
}

bool WorkerFragmentCache::read(std::string_view key, Out& out) {
  const auto html = shared_->get(key);
  if (!html) return false;
  out.append(SafeHtml::trusted(*html));
  return true;
}

void WorkerFragmentCache::write(std::string_view key, std::string_view html) {
  shared_->put(key, std::string(html));
}

}  // namespace campfire::app
