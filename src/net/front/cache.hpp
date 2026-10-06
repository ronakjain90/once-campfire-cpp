// Thruster's response cache. Rust: crates/kit/src/front/cache.rs (Thruster: cache_handler.go, memory_cache.go,
// variant.go).
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "net/http.hpp"
#include "net/response.hpp"

namespace campfire::net::front {

using CacheClock = std::chrono::steady_clock;

// What an entry costs beyond its key and response (Rust: ENTRY_OVERHEAD).
inline constexpr std::size_t kEntryOverhead = 256;
// The longest target (path and query) that a cacheable request may have (Rust README: 2 KB).
inline constexpr std::size_t kMaxCacheableUri = 2048;

// A response as the cache keeps it. Immutable after it is made. Many requests share it.
struct CachedResponse {
  int status = 200;
  std::vector<std::pair<std::string, std::string>> headers;  // the headers that the app sent, in order
  std::string body;
  // The values that the request had for the headers that `vary` names, when the entry was stored.
  std::vector<std::pair<std::string, std::string>> variant;

  // The size that the entry takes in the cache, stored under `key_size` bytes of key.
  [[nodiscard]] std::size_t cost(std::size_t key_size) const noexcept;
  [[nodiscard]] std::string_view header(std::string_view lower_name) const noexcept;

  // The compressed bodies of `body`, made on the first use. The jitter comes from the body, so
  // the result does not change. Not part of the size of the entry (the cache is bounded by
  // `CACHE_SIZE` for the entries as the app made them).
  mutable std::mutex memo_mutex;
  mutable std::shared_ptr<const std::string> gzip_memo;
  mutable std::shared_ptr<const std::string> zstd_memo;
};

// `MemoryCache`: a map bounded by its size that evicts by sampling 5 entries. The map has shards.
// Each shard has its own lock and `capacity / shards` bytes.
class MemoryCache {
 public:
  MemoryCache(std::int64_t capacity, std::int64_t max_item_size, std::size_t shards = 16);
  MemoryCache(const MemoryCache&) = delete;
  MemoryCache& operator=(const MemoryCache&) = delete;

  [[nodiscard]] std::shared_ptr<const CachedResponse> get(std::string_view key, CacheClock::time_point now);
  // Returns false if the entry is too large to store.
  bool set(std::string_view key, std::shared_ptr<const CachedResponse> value, CacheClock::time_point expires_at,
           CacheClock::time_point now);

  // The sum of the sizes of the entries (all shards).
  [[nodiscard]] std::int64_t size() const;
  [[nodiscard]] std::size_t count() const;

 private:
  struct Entry {
    CacheClock::time_point last_accessed_at;
    CacheClock::time_point expires_at;
    std::shared_ptr<const CachedResponse> value;
    std::int64_t size = 0;
    std::size_t key_index = 0;  // position in `keys`
  };
  struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
  };
  struct Shard {
    mutable std::mutex mutex;
    std::int64_t capacity = 0;
    std::int64_t size = 0;
    std::uint64_t random = 0x9E3779B97F4A7C15ULL;
    std::vector<std::string> keys;  // for sampling
    std::unordered_map<std::string, Entry, StringHash, std::equal_to<>> items;
  };

  Shard& shard_for(std::string_view key) noexcept;
  static void evict_one(Shard& shard, CacheClock::time_point now);
  static void erase_key_at(Shard& shard, std::size_t index);

  std::int64_t max_item_size_;
  std::vector<std::unique_ptr<Shard>> shards_;
};

// `shouldCacheRequest`: GET or HEAD, no upgrade, no range, and a short target.
[[nodiscard]] bool should_cache_request(const Request& request) noexcept;

// `CacheStatus`: how long a response may live in the cache. Nothing means: do not cache it.
// `vary` and `cache_control` are the values of the first header with that name.
[[nodiscard]] std::optional<std::chrono::seconds> cache_lifetime(int status, std::string_view vary,
                                                                 std::string_view cache_control) noexcept;

// `Variant`: the cache key of a request. The key has the raw path and query (Rust README:
// "keep raw paths and queries"), then the values of the headers that the response varies on.
class Variant {
 public:
  explicit Variant(const Request& request);

  // The names (lowercase, sorted) from the `vary` value of a response.
  void set_response_vary(std::string_view vary);
  // The key. The text stays valid until the next call.
  [[nodiscard]] std::string_view cache_key();
  [[nodiscard]] std::string_view base_key() const noexcept { return base_; }
  [[nodiscard]] bool matches(const std::vector<std::pair<std::string, std::string>>& stored) const;
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> variant_headers() const;

 private:
  [[nodiscard]] std::string_view request_value(const std::string& name) const noexcept;

  const Request* request_;
  std::string base_;
  std::string key_;
  std::vector<std::string> names_;
};

// `wasNotModified`: the request has an `If-None-Match` that lists the `etag` of the entry.
[[nodiscard]] bool was_not_modified(const CachedResponse& cached, const Request& request) noexcept;

}  // namespace campfire::net::front
