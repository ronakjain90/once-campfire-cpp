// The shared fragment cache for `{% cache key %}` (architecture section 6). Rails: ActionView cache helper
// with a memory store. Rust: crates/views fragment_cache. Entries are immutable. The cache is
// split in shards, and each shard is bounded by bytes with least-recently-used eviction.
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "views/fragment_cache.hpp"

namespace campfire::app {

class SharedFragmentCache {
 public:
  static constexpr std::size_t kEntryOverhead = 96;  // the cost of an entry besides its text
  explicit SharedFragmentCache(std::size_t max_bytes, std::size_t shard_count = 16);
  SharedFragmentCache(const SharedFragmentCache&) = delete;
  SharedFragmentCache& operator=(const SharedFragmentCache&) = delete;

  [[nodiscard]] std::shared_ptr<const std::string> get(std::string_view key);
  // An entry that is bigger than one shard is not stored.
  void put(std::string_view key, std::string html);
  void clear();

  [[nodiscard]] std::size_t bytes() const;
  [[nodiscard]] std::size_t entries() const;
  [[nodiscard]] std::size_t max_bytes() const noexcept { return max_bytes_; }

 private:
  struct Entry {
    std::string key;
    std::shared_ptr<const std::string> html;
    std::size_t cost;
  };
  struct Shard {
    std::mutex mutex;
    std::list<Entry> lru;  // the front is the most recent
    std::unordered_map<std::string_view, std::list<Entry>::iterator> index;
    std::size_t bytes = 0;
  };
  [[nodiscard]] Shard& shard_for(std::string_view key) const;

  std::size_t max_bytes_;
  std::size_t shard_bytes_;
  mutable std::vector<std::unique_ptr<Shard>> shards_;
};

// The `views::FragmentCache` that a worker installs with `views::set_fragment_cache`.
class WorkerFragmentCache final : public views::FragmentCache {
 public:
  explicit WorkerFragmentCache(SharedFragmentCache& shared) noexcept : shared_(&shared) {}
  [[nodiscard]] bool enabled() const noexcept override { return true; }
  bool read(std::string_view key, Out& out) override;
  void write(std::string_view key, std::string_view html) override;

 private:
  SharedFragmentCache* shared_;
};

}  // namespace campfire::app
