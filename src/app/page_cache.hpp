// The page cache (architecture section 6.1). The key is the hash of the data that the page read, so
// a page cannot miss a dependency. An entry has the identity body, the gzip body and the ETag.
// Entries are immutable. Rails: Rack::ETag and Rack::Deflater work for each request; this cache does
// them once for each distinct page.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/xxh3.hpp"

namespace campfire::app {

struct PageEntry {
  std::string identity;
  std::string gzip;
  std::string etag;  // `W/"<32 hex>"`
  std::string content_type;
};

// 1 in 16 in the asan and tsan builds, never in the others.
[[nodiscard]] unsigned default_audit_every() noexcept;

// `Rack::ETag`: `W/"` and the first 32 hex digits of the SHA-256 of the body.
[[nodiscard]] std::string body_etag(std::string_view body);
// The same digest, for a body in many chunks: `update` each chunk, then `etag`.
class Sha256Etag {
 public:
  Sha256Etag();
  ~Sha256Etag();
  Sha256Etag(const Sha256Etag&) = delete;
  Sha256Etag& operator=(const Sha256Etag&) = delete;
  void update(const void* data, std::size_t size) noexcept;
  [[nodiscard]] std::string etag();  // `W/"<32 hex>"`

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// A cached fragment in the body of a page: its offset and its size.
struct FragmentSpan {
  std::size_t offset = 0;
  std::size_t size = 0;
};
// The ETag of a page from its parts, as the Rust port makes it (crates/kit/src/deflater/splice.rs, `PageParts::etag`):
// a SHA-256 over the digests of the fragments and of the text between them. A page with no fragment of 1 KB or more
// gets `body_etag`. `fragments` are in order, with no overlap.
[[nodiscard]] std::string parts_etag(std::string_view body, std::span<const FragmentSpan> fragments);
// The same format from a page key. A page whose Rust ETag hashes page parts uses this one.
[[nodiscard]] std::string key_etag(const Hash128& key);

class PageCache {
 public:
  struct Options {
    std::size_t max_bytes = std::size_t{32} << 20;
    std::size_t shards = 16;
    unsigned audit_every = 0;  // 0: no audit
  };
  explicit PageCache(Options options);
  PageCache(const PageCache&) = delete;
  PageCache& operator=(const PageCache&) = delete;

  [[nodiscard]] std::shared_ptr<const PageEntry> get(const Hash128& key);
  // Builds the gzip body and (if `etag` is empty) the body ETag, then stores the entry. A page
  // that is bigger than one shard is returned but not stored.
  std::shared_ptr<const PageEntry> put(const Hash128& key, std::string identity, std::string content_type,
                                       std::string etag = {});
  // The same, with the gzip body and the ETag already made (src/app/splice.hpp).
  std::shared_ptr<const PageEntry> put_built(const Hash128& key, std::string identity, std::string gzip,
                                             std::string content_type, std::string etag);
  void clear();

  [[nodiscard]] std::size_t bytes() const;
  [[nodiscard]] std::size_t entries() const;
  [[nodiscard]] std::uint64_t hits() const noexcept { return hits_.load(); }
  [[nodiscard]] std::uint64_t misses() const noexcept { return misses_.load(); }

  // The audit. `audit_due` is true on 1 hit in `audit_every`. The handler gets the text of a
  // difference. The default handler logs an error and aborts.
  [[nodiscard]] bool audit_due() noexcept;
  using FailureHandler = std::function<void(std::string_view)>;
  void set_failure_handler(FailureHandler handler);
  // Compares a fresh render with the cached body. Calls the handler on a difference.
  // Returns true if the bodies are equal.
  bool audit_compare(const Hash128& key, const PageEntry& cached, std::string_view fresh);
  [[nodiscard]] std::uint64_t audits() const noexcept { return audits_.load(); }

 private:
  struct Slot {
    Hash128 key;
    std::shared_ptr<const PageEntry> entry;
    std::size_t cost;
  };
  struct KeyHash {
    std::size_t operator()(const Hash128& k) const noexcept { return static_cast<std::size_t>(k.low ^ (k.high << 1)); }
  };
  struct Shard {
    std::mutex mutex;
    std::list<Slot> lru;
    std::unordered_map<Hash128, std::list<Slot>::iterator, KeyHash> index;
    std::size_t bytes = 0;
  };
  [[nodiscard]] Shard& shard_for(const Hash128& key) const { return *shards_[key.low % shards_.size()]; }

  Options options_;
  std::size_t shard_bytes_;
  mutable std::vector<std::unique_ptr<Shard>> shards_;
  std::atomic<std::uint64_t> hits_{0};
  std::atomic<std::uint64_t> misses_{0};
  std::atomic<std::uint64_t> audit_counter_{0};
  std::atomic<std::uint64_t> audits_{0};
  std::mutex handler_mutex_;
  FailureHandler handler_;
};

}  // namespace campfire::app
