// The page cache. Design: plans/architecture.md section 6.1.
#include "app/page_cache.hpp"

#include <openssl/evp.h>

#include <cstdlib>

#include "app/compress.hpp"
#include "core/log.hpp"

namespace campfire::app {

unsigned default_audit_every() noexcept {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
  return 16;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
  return 16;
#else
  return 0;
#endif
#else
  return 0;
#endif
}

struct Sha256Etag::Impl {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  Impl() { EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr); }
  ~Impl() { EVP_MD_CTX_free(ctx); }
};

Sha256Etag::Sha256Etag() : impl_(std::make_unique<Impl>()) {}
Sha256Etag::~Sha256Etag() = default;

void Sha256Etag::update(const void* data, std::size_t size) noexcept {
  EVP_DigestUpdate(impl_->ctx, data, size);
}

std::string Sha256Etag::etag() {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  EVP_DigestFinal_ex(impl_->ctx, digest, &length);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "W/\"";
  for (int i = 0; i < 16; ++i) {
    out.push_back(kHex[digest[i] >> 4]);
    out.push_back(kHex[digest[i] & 15]);
  }
  out.push_back('"');
  return out;
}

std::string body_etag(std::string_view body) {
  Sha256Etag sha;
  sha.update(body.data(), body.size());
  return sha.etag();
}

std::string key_etag(const Hash128& key) {
  return "W/\"" + key.hex() + "\"";
}

PageCache::PageCache(Options options) : options_(options) {
  const std::size_t count = options_.shards == 0 ? 1 : options_.shards;
  shard_bytes_ = options_.max_bytes / count;
  for (std::size_t i = 0; i < count; ++i) shards_.push_back(std::make_unique<Shard>());
}

std::shared_ptr<const PageEntry> PageCache::get(const Hash128& key) {
  Shard& shard = shard_for(key);
  const std::lock_guard lock(shard.mutex);
  const auto it = shard.index.find(key);
  if (it == shard.index.end()) {
    misses_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  hits_.fetch_add(1, std::memory_order_relaxed);
  shard.lru.splice(shard.lru.begin(), shard.lru, it->second);
  return it->second->entry;
}

std::shared_ptr<const PageEntry> PageCache::put(const Hash128& key, std::string identity, std::string content_type,
                                                std::string etag) {
  auto entry = std::make_shared<PageEntry>();
  entry->etag = etag.empty() ? body_etag(identity) : std::move(etag);
  entry->gzip = gzip_compress(identity);
  entry->identity = std::move(identity);
  entry->content_type = std::move(content_type);
  const std::size_t cost =
      entry->identity.size() + entry->gzip.size() + entry->etag.size() + entry->content_type.size() + 128;
  if (cost > shard_bytes_) return entry;
  Shard& shard = shard_for(key);
  const std::lock_guard lock(shard.mutex);
  if (const auto it = shard.index.find(key); it != shard.index.end()) {
    shard.bytes -= it->second->cost;
    shard.lru.erase(it->second);
    shard.index.erase(it);
  }
  while (!shard.lru.empty() && shard.bytes + cost > shard_bytes_) {
    shard.index.erase(shard.lru.back().key);
    shard.bytes -= shard.lru.back().cost;
    shard.lru.pop_back();
  }
  shard.lru.push_front(Slot{key, entry, cost});
  shard.index.emplace(key, shard.lru.begin());
  shard.bytes += cost;
  return entry;
}

void PageCache::clear() {
  for (auto& shard : shards_) {
    const std::lock_guard lock(shard->mutex);
    shard->index.clear();
    shard->lru.clear();
    shard->bytes = 0;
  }
}

std::size_t PageCache::bytes() const {
  std::size_t total = 0;
  for (auto& shard : shards_) {
    const std::lock_guard lock(shard->mutex);
    total += shard->bytes;
  }
  return total;
}

std::size_t PageCache::entries() const {
  std::size_t total = 0;
  for (auto& shard : shards_) {
    const std::lock_guard lock(shard->mutex);
    total += shard->lru.size();
  }
  return total;
}

bool PageCache::audit_due() noexcept {
  if (options_.audit_every == 0) return false;
  return audit_counter_.fetch_add(1, std::memory_order_relaxed) % options_.audit_every == 0;
}

void PageCache::set_failure_handler(FailureHandler handler) {
  const std::lock_guard lock(handler_mutex_);
  handler_ = std::move(handler);
}

bool PageCache::audit_compare(const Hash128& key, const PageEntry& cached, std::string_view fresh) {
  audits_.fetch_add(1, std::memory_order_relaxed);
  if (cached.identity == fresh) return true;
  std::size_t at = 0;
  while (at < fresh.size() && at < cached.identity.size() && fresh[at] == cached.identity[at]) ++at;
  const std::string message = "page cache audit: key " + key.hex() + " cached " +
                              std::to_string(cached.identity.size()) + " bytes, fresh " + std::to_string(fresh.size()) +
                              " bytes, first difference at byte " + std::to_string(at) + ". A facet is missing.";
  FailureHandler handler;
  {
    const std::lock_guard lock(handler_mutex_);
    handler = handler_;
  }
  if (handler) {
    handler(message);
  } else {
    log_error("{}", message);
    std::abort();
  }
  return false;
}

}  // namespace campfire::app
