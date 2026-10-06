// The front server's request pipeline: cache, compression, X-Forwarded-*, headers.
// Rust: crates/kit/src/front/handler.rs (Thruster: internal/handler.go).
#pragma once

#include <memory>
#include <memory_resource>
#include <optional>
#include <string_view>

#include "core/arena.hpp"
#include "core/config.hpp"
#include "net/front/cache.hpp"
#include "net/front/compress.hpp"
#include "net/response.hpp"

namespace campfire::net::front {

// How the cache treated a request.
enum class CacheStatus : std::uint8_t { Bypass, Miss, Hit };

// The state of one request in the pipeline. The connection (or the HTTP/2 stream) owns it.
struct FrontState {
  Negotiation negotiation;
  CacheStatus status = CacheStatus::Bypass;
  std::shared_ptr<const CachedResponse> hit;     // the entry that answers the request
  std::shared_ptr<const CachedResponse> stored;  // the entry that this request made
  std::optional<Variant> variant;
  // The key that `begin` looked up. A response that is stored goes under it (Rust: the `key` of
  // `cache_handler.go`, which a variant mismatch makes the longer one).
  std::string key;
};

class Front {
 public:
  explicit Front(const FrontConfig& config);
  Front(const Front&) = delete;
  Front& operator=(const Front&) = delete;

  // Chooses the encoding, then looks in the cache. After it, `state.status == Hit` means that
  // `hit_response` answers the request and no handler runs.
  void begin(FrontState& state, const Request& request) const;
  [[nodiscard]] Response hit_response(const FrontState& state, const Request& request,
                                      std::pmr::memory_resource* resource) const;

  // The request as the app sees it (`proxy`, `setXForwarded`, `NewRequestStartHandler`): adds
  // "x-request-start", and replaces "x-forwarded-for", "-host" and "-proto" and drops "forwarded".
  // The new header list is in the arena.
  void proxied(Request& request, Arena& arena, bool tls) const;

  // Completes the response: the cache, "x-cache", "vary", the compression and "date".
  void finish(FrontState& state, const Request& request, Response& response) const;

  [[nodiscard]] MemoryCache& cache() const noexcept { return *cache_; }
  [[nodiscard]] const FrontConfig& config() const noexcept { return config_; }

 private:
  void compress(FrontState& state, Response& response) const;

  FrontConfig config_;
  std::unique_ptr<MemoryCache> cache_;
  std::optional<Compression> compression_;
};

}  // namespace campfire::net::front
