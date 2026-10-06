// Thruster's response cache. Rust: crates/kit/src/front/cache.rs (Thruster: cache_handler.go, memory_cache.go,
// variant.go).
#include "net/front/cache.hpp"

#include <algorithm>
#include <cctype>

namespace campfire::net::front {

namespace {

bool is_word(char c) noexcept {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Regex `\b<token>` at a position: no word character before it.
bool word_start(std::string_view text, std::size_t at) noexcept {
  return at == 0 || !is_word(text[at - 1]);
}

// Regex `\b<word>\b`.
bool has_word(std::string_view text, std::string_view word) noexcept {
  for (std::size_t at = text.find(word); at != std::string_view::npos; at = text.find(word, at + 1)) {
    const std::size_t end = at + word.size();
    if (word_start(text, at) && (end == text.size() || !is_word(text[end]))) return true;
  }
  return false;
}

// Regex `\b<prefix>(\d+)\b`. Returns false if no match. `overflow` is set for a number that does
// not fit in 63 bits (the Rust port gives up on the response then).
bool find_number(std::string_view text, std::string_view prefix, std::int64_t& value, bool& overflow) noexcept {
  for (std::size_t at = text.find(prefix); at != std::string_view::npos; at = text.find(prefix, at + 1)) {
    if (!word_start(text, at)) continue;
    std::size_t i = at + prefix.size();
    const std::size_t digits_begin = i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
    if (i == digits_begin) continue;
    if (i != text.size() && is_word(text[i])) continue;  // `\b` fails after the digits
    std::int64_t number = 0;
    overflow = false;
    for (std::size_t k = digits_begin; k < i; ++k) {
      const int digit = text[k] - '0';
      if (number > (INT64_MAX - digit) / 10) {
        overflow = true;
        return true;
      }
      number = number * 10 + digit;
    }
    value = number;
    return true;
  }
  return false;
}

std::string_view trim(std::string_view text) noexcept {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

}  // namespace

std::size_t CachedResponse::cost(std::size_t key_size) const noexcept {
  std::size_t total = body.size() + key_size + kEntryOverhead;
  for (const auto& [name, value] : headers) total += name.size() + value.size();
  for (const auto& [name, value] : variant) total += name.size() + value.size();
  return total;
}

std::string_view CachedResponse::header(std::string_view lower_name) const noexcept {
  for (const auto& [name, value] : headers) {
    if (iequals(name, lower_name)) return value;
  }
  return {};
}

MemoryCache::MemoryCache(std::int64_t capacity, std::int64_t max_item_size, std::size_t shards)
    : max_item_size_(max_item_size) {
  shards = std::max<std::size_t>(shards, 1);
  for (std::size_t i = 0; i < shards; ++i) {
    auto shard = std::make_unique<Shard>();
    shard->capacity = capacity / static_cast<std::int64_t>(shards);
    shard->random += i * 0x1000193ULL;
    shards_.push_back(std::move(shard));
  }
}

MemoryCache::Shard& MemoryCache::shard_for(std::string_view key) noexcept {
  return *shards_[std::hash<std::string_view>{}(key) % shards_.size()];
}

std::shared_ptr<const CachedResponse> MemoryCache::get(std::string_view key, CacheClock::time_point now) {
  Shard& shard = shard_for(key);
  const std::lock_guard lock(shard.mutex);
  const auto found = shard.items.find(key);
  if (found == shard.items.end()) return nullptr;
  Entry& item = found->second;
  if (item.expires_at < now) return nullptr;
  item.last_accessed_at = now;
  return item.value;
}

void MemoryCache::erase_key_at(Shard& shard, std::size_t index) {
  // swap_remove of the key list. The moved key keeps its entry in sync.
  if (index + 1 != shard.keys.size()) {
    shard.keys[index] = std::move(shard.keys.back());
    shard.items.find(std::string_view(shard.keys[index]))->second.key_index = index;
  }
  shard.keys.pop_back();
}

void MemoryCache::evict_one(Shard& shard, CacheClock::time_point now) {
  // Samples 5 random entries: the first expired one, or else the least recently used.
  std::size_t chosen = 0;
  bool have = false;
  CacheClock::time_point oldest{};
  for (int i = 0; i < 5; ++i) {
    shard.random ^= shard.random << 13;
    shard.random ^= shard.random >> 7;
    shard.random ^= shard.random << 17;
    const std::size_t index = static_cast<std::size_t>(shard.random % shard.keys.size());
    const Entry& item = shard.items.find(std::string_view(shard.keys[index]))->second;
    if (item.expires_at < now) {
      chosen = index;
      have = true;
      break;
    }
    if (!have || item.last_accessed_at < oldest) {
      chosen = index;
      oldest = item.last_accessed_at;
      have = true;
    }
  }
  const auto found = shard.items.find(std::string_view(shard.keys[chosen]));
  shard.size -= found->second.size;
  const std::string key = shard.keys[chosen];
  erase_key_at(shard, chosen);
  shard.items.erase(key);
}

bool MemoryCache::set(std::string_view key, std::shared_ptr<const CachedResponse> value,
                      CacheClock::time_point expires_at, CacheClock::time_point now) {
  Shard& shard = shard_for(key);
  const auto item_size = static_cast<std::int64_t>(value->cost(key.size()));
  const std::lock_guard lock(shard.mutex);
  if (item_size > max_item_size_ || item_size > shard.capacity) return false;
  const std::int64_t limit = shard.capacity - item_size;
  while (shard.size > limit && !shard.keys.empty()) evict_one(shard, now);
  auto entry_value = std::move(value);
  const auto found = shard.items.find(key);
  if (found != shard.items.end()) {
    shard.size -= found->second.size;
    found->second.last_accessed_at = now;
    found->second.expires_at = expires_at;
    found->second.value = std::move(entry_value);
    found->second.size = item_size;
  } else {
    Entry entry;
    entry.last_accessed_at = now;
    entry.expires_at = expires_at;
    entry.value = std::move(entry_value);
    entry.size = item_size;
    entry.key_index = shard.keys.size();
    shard.keys.emplace_back(key);
    shard.items.emplace(std::string(key), std::move(entry));
  }
  shard.size += item_size;
  return true;
}

std::int64_t MemoryCache::size() const {
  std::int64_t total = 0;
  for (const auto& shard : shards_) {
    const std::lock_guard lock(shard->mutex);
    total += shard->size;
  }
  return total;
}

std::size_t MemoryCache::count() const {
  std::size_t total = 0;
  for (const auto& shard : shards_) {
    const std::lock_guard lock(shard->mutex);
    total += shard->items.size();
  }
  return total;
}

bool should_cache_request(const Request& request) noexcept {
  const bool allowed = request.method == Method::Get || request.method == Method::Head;
  // Thruster compares "Upgrade" exactly: "connection: upgrade" does not count (Rust test).
  const bool upgrade = request.header("connection") == "Upgrade" || request.header("upgrade") == "websocket";
  const bool range = !request.header("range").empty();
  return allowed && !upgrade && !range && request.target.size() <= kMaxCacheableUri;
}

std::optional<std::chrono::seconds> cache_lifetime(int status, std::string_view vary,
                                                   std::string_view cache_control) noexcept {
  if (status < 200 || status > 399 || status == 304) return std::nullopt;
  if (vary.find('*') != std::string_view::npos) return std::nullopt;
  if (!has_word(cache_control, "public") || has_word(cache_control, "no-cache")) return std::nullopt;
  std::int64_t seconds = 0;
  bool overflow = false;
  // `s-max-age` (sic) first: it is the name that Thruster reads.
  bool found = find_number(cache_control, "s-max-age=", seconds, overflow);
  if (!found) found = find_number(cache_control, "max-age=", seconds, overflow);
  if (!found || overflow || seconds <= 0) return std::nullopt;
  return std::chrono::seconds(seconds);
}

Variant::Variant(const Request& request) : request_(&request) {
  std::string_view host = request.header("host");
  base_.reserve(request.target.size() + host.size() + 16);
  base_ += request.method_text;
  base_ += '\n';
  base_ += request.path;
  base_ += '\n';
  base_ += request.query;
  base_ += '\n';
  base_ += host;
}

void Variant::set_response_vary(std::string_view vary) {
  names_.clear();
  if (vary.empty()) return;
  std::size_t begin = 0;
  while (begin <= vary.size()) {
    std::size_t end = vary.find(',', begin);
    if (end == std::string_view::npos) end = vary.size();
    std::string name(trim(vary.substr(begin, end - begin)));
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    names_.push_back(std::move(name));
    begin = end + 1;
  }
  std::sort(names_.begin(), names_.end());
}

std::string_view Variant::request_value(const std::string& name) const noexcept {
  return request_->header(name);
}

std::string_view Variant::cache_key() {
  key_ = base_;
  for (const std::string& name : names_) {
    key_ += '\n';
    key_ += name;
    key_ += '=';
    key_ += request_value(name);
  }
  return key_;
}

bool Variant::matches(const std::vector<std::pair<std::string, std::string>>& stored) const {
  return std::all_of(names_.begin(), names_.end(), [&](const std::string& name) {
    std::string_view value;
    for (const auto& [n, v] : stored) {
      if (n == name) {
        value = v;
        break;
      }
    }
    return value == request_value(name);
  });
}

std::vector<std::pair<std::string, std::string>> Variant::variant_headers() const {
  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(names_.size());
  for (const std::string& name : names_) out.emplace_back(name, std::string(request_value(name)));
  return out;
}

bool was_not_modified(const CachedResponse& cached, const Request& request) noexcept {
  const std::string_view etag = cached.header("etag");
  if (etag.empty()) return false;
  std::string_view list = request.header("if-none-match");
  std::size_t begin = 0;
  while (begin <= list.size()) {
    std::size_t end = list.find(',', begin);
    if (end == std::string_view::npos) end = list.size();
    if (trim(list.substr(begin, end - begin)) == etag) return true;
    begin = end + 1;
  }
  return false;
}

}  // namespace campfire::net::front
