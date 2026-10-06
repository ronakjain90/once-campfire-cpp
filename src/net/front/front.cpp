// The front server's request pipeline. Rust: crates/kit/src/front/handler.rs, cache.rs, compression.rs.
#include "net/front/front.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "net/front.hpp"
#include "net/front/headers.hpp"

namespace campfire::net::front {

namespace {

std::string_view first_value(const Response& response, std::string_view name) {
  return response.get(name);
}

bool token_in_list(std::string_view list, std::string_view token) {
  std::size_t begin = 0;
  while (begin <= list.size()) {
    std::size_t end = list.find(',', begin);
    if (end == std::string_view::npos) end = list.size();
    std::string_view part = list.substr(begin, end - begin);
    while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) part.remove_prefix(1);
    while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) part.remove_suffix(1);
    if (iequals(part, token)) return true;
    begin = end + 1;
  }
  return false;
}

// `hasUserSpecificResponseHeaders`
bool has_user_specific_response_headers(const Response& response) {
  if (!response.get("set-cookie").empty()) return true;
  std::string_view cache_control = response.get("cache-control");
  std::size_t begin = 0;
  while (begin <= cache_control.size()) {
    std::size_t end = cache_control.find(',', begin);
    if (end == std::string_view::npos) end = cache_control.size();
    std::string_view directive = cache_control.substr(begin, end - begin);
    while (!directive.empty() && directive.front() == ' ') directive.remove_prefix(1);
    directive = directive.substr(0, directive.find('='));
    if (iequals(directive, "private") || iequals(directive, "no-store")) return true;
    begin = end + 1;
  }
  return token_in_list(response.get("vary"), "cookie");
}

// The checks that gzhttp makes from the headers: not encoded, not a range, and a known length of
// at least 1 KB with a type that it compresses.
bool may_compress(const Response& response) {
  if (!response.get("content-encoding").empty() || !response.get("content-range").empty()) return false;
  const std::string_view length_text = response.get("content-length");
  std::size_t length = 0;
  for (const char c : length_text) {
    if (c < '0' || c > '9') {
      length = 0;
      break;
    }
    length = length * 10 + static_cast<std::size_t>(c - '0');
  }
  const std::string_view content_type = response.get("content-type");
  return length == 0 || (length >= kMinCompressSize && (content_type.empty() || content_type_filter(content_type)));
}

}  // namespace

Front::Front(const FrontConfig& config)
    : config_(config), cache_(std::make_unique<MemoryCache>(config.cache_size, config.max_cache_item_size)) {
  if (config.gzip_compression_enabled) {
    compression_.emplace(config.gzip_compression_jitter, config.gzip_compression_disable_on_auth);
  }
}

void Front::begin(FrontState& state, const Request& request) const {
  state = FrontState{};
  if (compression_) state.negotiation = compression_->negotiate(request);
  if (!front::should_cache_request(request)) return;
  state.status = CacheStatus::Miss;
  state.variant.emplace(request);
  Variant& variant = *state.variant;
  const auto now = CacheClock::now();
  state.key = std::string(variant.cache_key());
  std::shared_ptr<const CachedResponse> found = cache_->get(state.key, now);
  if (found) {
    variant.set_response_vary(found->header("vary"));
    if (!variant.matches(found->variant)) {
      state.key = std::string(variant.cache_key());
      found = cache_->get(state.key, now);
    }
  }
  if (found) {
    state.status = CacheStatus::Hit;
    state.hit = std::move(found);
  }
}

Response Front::hit_response(const FrontState& state, const Request& request,
                             std::pmr::memory_resource* resource) const {
  const CachedResponse& entry = *state.hit;
  const bool not_modified = was_not_modified(entry, request);
  Response response(resource, not_modified ? 304 : entry.status);
  response.headers.reserve(entry.headers.size() + 4);
  for (const auto& [name, value] : entry.headers) response.add(name, value);
  // The entry owns the strings of the headers. The response keeps it alive.
  response.body_shared(state.hit, not_modified ? std::string_view{} : std::string_view(entry.body));
  return response;
}

void Front::proxied(Request& request, Arena& arena, bool tls) const {
  const bool forward = config_.forward_headers;
  std::string prior_for;
  std::string_view incoming_host;
  std::string_view incoming_proto;
  bool have_request_start = false;
  for (const Header& h : request.headers) {
    if (forward && iequals(h.name, "x-forwarded-for")) {
      if (!prior_for.empty()) prior_for += ", ";
      prior_for += h.value;
    } else if (iequals(h.name, "x-forwarded-host") && incoming_host.empty()) {
      incoming_host = h.value;
    } else if (iequals(h.name, "x-forwarded-proto") && incoming_proto.empty()) {
      incoming_proto = h.value;
    } else if (iequals(h.name, "x-request-start") && !h.value.empty()) {
      have_request_start = true;
    }
  }
  const std::string_view host = request.header("host");
  std::string forwarded_for =
      prior_for.empty() ? std::string(request.remote_ip) : prior_for + ", " + std::string(request.remote_ip);

  auto* items = static_cast<Header*>(arena.allocate((request.headers.size() + 5) * sizeof(Header), alignof(Header)));
  std::size_t count = 0;
  for (const Header& h : request.headers) {
    if (iequals(h.name, "forwarded") || iequals(h.name, "x-forwarded-for") || iequals(h.name, "x-forwarded-host") ||
        iequals(h.name, "x-forwarded-proto") || (iequals(h.name, "x-request-start") && !have_request_start)) {
      continue;
    }
    items[count++] = h;
  }
  if (!have_request_start) {
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    items[count++] = {"x-request-start", arena.copy("t=" + std::to_string(millis.count()))};
  }
  items[count++] = {"x-forwarded-for", arena.copy(forwarded_for)};
  items[count++] = {"x-forwarded-host", forward && !incoming_host.empty() ? incoming_host : host};
  items[count++] = {"x-forwarded-proto",
                    forward && !incoming_proto.empty() ? incoming_proto : (tls ? "https" : "http")};
  request.headers = std::span<const Header>(items, count);
}

void Front::compress(FrontState& state, Response& response) const {
  if (!compression_) return;
  if (response.status >= 100 && response.status < 200) return;
  const Negotiation& negotiation = state.negotiation;
  const bool guarded = negotiation.user_specific_request ||
                       (compression_->disable_on_auth() && has_user_specific_response_headers(response));
  const bool vetoed = guarded || response.has("no-gzip-compression");
  remove_header(response, "no-gzip-compression");
  if (negotiation.encoding == Encoding::None || vetoed || !may_compress(response)) return;

  std::string copy;
  std::string_view body;
  if (state.hit && !response.body_contiguous()) {
    body = state.hit->body;
  } else if (const auto view = response.body_contiguous()) {
    body = *view;
  } else {
    response.body_append_to(copy);
    body = copy;
  }
  const bool body_allowed = response.status != 204 && response.status != 304;
  std::string_view content_type = response.get("content-type");
  if (content_type.empty() && body_allowed && !body.empty()) {
    content_type = detect_content_type(body);
    if (!response.has("content-type")) response.add("content-type", content_type);
  }
  if (!(body.size() >= kMinCompressSize && content_type_filter(content_type))) return;

  const Encoding encoding = negotiation.encoding;
  response.add("content-encoding", encoding == Encoding::Gzip ? "gzip" : "zstd");
  remove_header(response, "content-length");
  remove_header(response, "accept-ranges");

  // The result depends on the body alone, so an entry keeps it for the next hit.
  const CachedResponse* owner = state.hit ? state.hit.get() : state.stored.get();
  std::shared_ptr<const std::string> encoded;
  if (owner != nullptr && owner->body.size() == body.size() && owner->body.data() == body.data()) {
    const std::lock_guard lock(owner->memo_mutex);
    std::shared_ptr<const std::string>& memo = encoding == Encoding::Gzip ? owner->gzip_memo : owner->zstd_memo;
    if (!memo) memo = std::make_shared<const std::string>(compression_->encode(encoding, body));
    encoded = memo;
  } else {
    encoded = std::make_shared<const std::string>(compression_->encode(encoding, body));
  }
  response.chunked = encoded->size() > kGoChunkingBuffer;
  const std::string_view view(*encoded);
  response.body_shared(std::move(encoded), view);
}

void Front::finish(FrontState& state, const Request& request, Response& response) const {
  (void)request;
  if (response.status >= 100 && response.status < 200) {
    suppress_bodiless_headers(response);
    return;
  }
  switch (state.status) {
    case CacheStatus::Hit: {
      insert_header(response, "x-cache", "hit");
      if (!response.has("vary")) response.add("vary", "Accept-Encoding");
      break;
    }
    case CacheStatus::Miss: {
      const auto lifetime =
          cache_lifetime(response.status, first_value(response, "vary"), first_value(response, "cache-control"));
      if (lifetime) remove_header(response, "set-cookie");
      const bool head = request.method == Method::Head;
      const bool fits = head || response.body_size() <=
                                    static_cast<std::size_t>(std::max<std::int64_t>(config_.max_cache_item_size, 0));
      // A response with chunk framing in its body is not stored: no cacheable route sends one.
      if (lifetime && fits && !response.framed) {
        auto entry = std::make_shared<CachedResponse>();
        entry->status = response.status;
        for (const Header& h : response.headers) {
          if (!iequals(h.name, "x-cache")) entry->headers.emplace_back(h.name, h.value);
        }
        // A HEAD response has no body to record.
        if (!head) response.body_append_to(entry->body);
        state.variant->set_response_vary(first_value(response, "vary"));
        entry->variant = state.variant->variant_headers();
        // The entry goes under the key that the lookup used (Rust: the `key` of `cache_handler.go`,
        // which a variant mismatch made longer before anything was stored).
        const auto now = CacheClock::now();
        const std::shared_ptr<const CachedResponse> shared = entry;
        if (cache_->set(state.key, shared, now + *lifetime, now)) state.stored = shared;
      }
      insert_header(response, "x-cache", "miss");
      if (!response.has("vary")) response.add("vary", "Accept-Encoding");
      break;
    }
    case CacheStatus::Bypass: {
      std::vector<std::string_view> existing;
      for (const Header& h : response.headers) {
        if (iequals(h.name, "vary")) existing.push_back(h.value);
      }
      insert_header(response, "x-cache", "bypass");
      remove_header(response, "vary");
      response.add("vary", "Accept-Encoding");
      for (const std::string_view value : existing) response.add("vary", value);
      break;
    }
  }
  compress(state, response);
  suppress_bodiless_headers(response);
  if (!response.has("date")) {
    char buffer[32];
    response.add_copy("date", http_date_now(buffer));
  }
}

}  // namespace campfire::net::front
