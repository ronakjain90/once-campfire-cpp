// Rust: crates/kit/src/deflater/splice.rs. Design: src/app/splice.hpp.
#include "app/splice.hpp"

#include <openssl/evp.h>
#include <zlib.h>

#include <array>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/xxh3.hpp"

namespace campfire::app::splice {

namespace {

// `MIN_FRAGMENT` and `MAX_GLUE` of the Rust port, and the same values as `parts_etag`.
constexpr std::size_t kMinFragment = 1024;
constexpr std::size_t kMaxGlue = 256;
// The deflate window: a piece can refer back this far into the part before it.
constexpr std::size_t kWindow = 32 * 1024;
// What the kept pieces may cost. A message fragment is ~10 KB and its pieces ~1 KB each.
constexpr std::size_t kPieceBudget = std::size_t{48} << 20;
// What the kept digests may cost (~64 K entries).
constexpr std::size_t kDigestBudget = std::size_t{4} << 20;
constexpr std::size_t kEntryOverhead = 128;

using Digest32 = std::array<unsigned char, 32>;

Digest32 sha256(std::string_view data) {
  Digest32 out{};
  unsigned int length = 0;
  EVP_Digest(data.data(), data.size(), out.data(), &length, EVP_sha256(), nullptr);
  return out;
}

// --- CRC-32 of a concatenation (zlib `crc32_combine_op`; Rust `Crc`) -------------------------------------------------

// The CRC-32 polynomial in the reflected bit order of gzip (bit 31 is x^0).
constexpr std::uint32_t kPolynomial = 0xedb88320U;

// `a` times `b` modulo the polynomial (zlib `multmodp`).
constexpr std::uint32_t multiply(std::uint32_t a, std::uint32_t b) noexcept {
  std::uint32_t product = 0;
  for (int bit = 31; bit >= 0; --bit) {
    product ^= b & (0U - ((a >> bit) & 1U));
    b = (b >> 1) ^ (kPolynomial & (0U - (b & 1U)));
  }
  return product;
}

// x^(2^k) modulo the polynomial, for k in 0..31 (zlib `x2n_table`).
constexpr std::array<std::uint32_t, 32> make_powers() noexcept {
  std::array<std::uint32_t, 32> table{};
  std::uint32_t power = 1U << 30;  // x^1
  for (std::size_t k = 0; k < 32; ++k) {
    table[k] = power;
    power = multiply(power, power);
  }
  return table;
}
constexpr std::array<std::uint32_t, 32> kPowers = make_powers();

// x^(8n) modulo the polynomial (zlib `x2nmodp(n, 3)`).
std::uint32_t x_to_the_8(std::uint64_t n) noexcept {
  std::uint32_t power = 1U << 31;  // x^0
  std::size_t k = 3;
  while (n != 0) {
    if ((n & 1U) != 0) power = multiply(kPowers[k % 32], power);
    n >>= 1;
    ++k;
  }
  return power;
}

// --- Compression ---------------------------------------------------------------------------------------------------

struct Piece {
  std::string deflated;  // raw deflate, ending on a sync flush, with no final block
  std::uint32_t crc = 0;
  std::uint32_t shift = 0;  // x^(8 · size of the part)
};

// Raw deflate of `text` at level 6, with the last 32 KB of `dictionary` preset, sync-flushed.
std::string compress(std::string_view dictionary, std::string_view text) {
  struct Stream {
    z_stream z{};
    Stream() {
      if (deflateInit2(&z, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) std::abort();
    }
    ~Stream() { deflateEnd(&z); }
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
  };
  // A stream is not thread-safe, so each thread keeps one. `deflateReset` keeps its buffers.
  thread_local Stream stream;
  z_stream& z = stream.z;
  if (deflateReset(&z) != Z_OK) std::abort();
  if (!dictionary.empty()) {
    const std::string_view window =
        dictionary.size() > kWindow ? dictionary.substr(dictionary.size() - kWindow) : dictionary;
    if (deflateSetDictionary(&z, reinterpret_cast<const Bytef*>(window.data()), static_cast<uInt>(window.size())) !=
        Z_OK) {
      std::abort();
    }
  }
  std::string out(deflateBound(&z, static_cast<uLong>(text.size())) + 64, '\0');
  std::size_t produced = 0;
  z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(text.data()));
  z.avail_in = static_cast<uInt>(text.size());
  while (true) {
    z.next_out = reinterpret_cast<Bytef*>(out.data() + produced);
    z.avail_out = static_cast<uInt>(out.size() - produced);
    if (deflate(&z, Z_SYNC_FLUSH) == Z_STREAM_ERROR) std::abort();
    produced = out.size() - z.avail_out;
    // Done when all input is in and the flush left room in the output.
    if (z.avail_in == 0 && z.avail_out != 0) break;
    out.resize(out.size() * 2);
  }
  out.resize(produced);
  out.shrink_to_fit();
  return out;
}

// --- What is kept ----------------------------------------------------------------------------------------------------

struct HashOf128 {
  std::size_t operator()(const Hash128& k) const noexcept { return static_cast<std::size_t>(k.low ^ (k.high * 31)); }
};

struct PieceKey {
  Hash128 part;    // the bytes of the part, glue included
  Hash128 before;  // the bytes of the part before it (its dictionary); zero for the first part
  [[nodiscard]] bool operator==(const PieceKey&) const = default;
};
struct HashOfPieceKey {
  std::size_t operator()(const PieceKey& k) const noexcept {
    return static_cast<std::size_t>(k.part.low ^ (k.before.low * 0x9e3779b97f4a7c15ULL) ^ (k.part.high >> 7));
  }
};

// A map in two generations, bounded by what its entries cost (Rust `Generations`). A read of an old entry moves it
// to the young generation. When the young generation costs more than half the budget, it becomes the old one.
template <class K, class V, class H>
class Generations {
 public:
  explicit Generations(std::size_t budget) : budget_(budget) {}

  std::optional<V> get(const K& key) {
    if (const auto it = young_.find(key); it != young_.end()) return it->second.value;
    const auto it = old_.find(key);
    if (it == old_.end()) return std::nullopt;
    Entry entry = std::move(it->second);
    old_.erase(it);
    V value = entry.value;
    put(key, std::move(entry));
    return value;
  }

  void insert(const K& key, V value, std::size_t cost) { put(key, Entry{std::move(value), cost}); }

  void clear() {
    young_.clear();
    old_.clear();
    young_cost_ = 0;
  }
  [[nodiscard]] std::size_t size() const noexcept { return young_.size() + old_.size(); }
  [[nodiscard]] std::size_t cost() const noexcept {
    std::size_t total = young_cost_;
    for (const auto& [key, entry] : old_) total += entry.cost;
    return total;
  }

 private:
  struct Entry {
    V value;
    std::size_t cost;
  };
  void put(const K& key, Entry entry) {
    if (const auto it = young_.find(key); it != young_.end()) young_cost_ -= it->second.cost;
    young_cost_ += entry.cost;
    young_.insert_or_assign(key, std::move(entry));
    if (young_cost_ > budget_ / 2) {
      old_ = std::move(young_);
      young_ = {};
      young_cost_ = 0;
    }
  }

  std::unordered_map<K, Entry, H> young_;
  std::unordered_map<K, Entry, H> old_;
  std::size_t young_cost_ = 0;
  std::size_t budget_;
};

struct Stores {
  std::mutex digest_mutex;
  Generations<Hash128, Digest32, HashOf128> digests{kDigestBudget};
  std::mutex piece_mutex;
  Generations<PieceKey, std::shared_ptr<const Piece>, HashOfPieceKey> pieces{kPieceBudget};
};

Stores& stores() {
  static Stores* const instance = new Stores;  // never destroyed: worker threads may use it until the process ends
  return *instance;
}

// --- Parts -----------------------------------------------------------------------------------------------------------

struct Part {
  bool fragment = false;
  std::string_view glue;  // a fragment's text since the previous fragment (it goes out just before the fragment)
  std::string_view own;   // the text, or the fragment without its glue
  std::string_view full;  // glue and own: what the piece compresses (contiguous in the body)
  Hash128 own_id;
  Hash128 full_id;
  Digest32 sha{};  // of `own`
};

void put_u64_le(std::string& out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}
void put_u32_le(std::string& out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

}  // namespace

std::uint32_t crc32_combine(std::uint32_t crc_a, std::uint32_t crc_b, std::uint64_t length_b) noexcept {
  return multiply(crc_a, x_to_the_8(length_b)) ^ crc_b;
}

std::optional<Spliced> build(std::string_view body, std::span<const FragmentSpan> fragments) {
  // The parts, split as `parts_etag` splits them.
  std::vector<Part> parts;
  parts.reserve(fragments.size() * 2 + 1);
  bool follows_fragment = false;
  std::size_t position = 0;
  for (const FragmentSpan& span : fragments) {
    if (span.size < kMinFragment) continue;  // it stays in the text around it
    const std::string_view gap = body.substr(position, span.offset - position);
    Part part;
    part.fragment = true;
    part.own = body.substr(span.offset, span.size);
    if (follows_fragment && gap.size() <= kMaxGlue) {
      part.glue = gap;
    } else if (!gap.empty()) {
      Part text;
      text.own = gap;
      text.full = gap;
      parts.push_back(text);
    }
    part.full = body.substr(span.offset - part.glue.size(), part.glue.size() + span.size);
    parts.push_back(part);
    follows_fragment = true;
    position = span.offset + span.size;
  }
  if (parts.empty()) return std::nullopt;
  if (position < body.size()) {
    Part text;
    text.own = body.substr(position);
    text.full = text.own;
    parts.push_back(text);
  }
  for (Part& part : parts) {
    part.own_id = xxh3_128(part.own);
    if (part.glue.empty()) {
      part.full_id = part.own_id;
    } else {
      // The glue is short: hash it with the fragment's hash, not the fragment's bytes again.
      std::string id(part.glue);
      id.append(reinterpret_cast<const char*>(&part.own_id.low), sizeof part.own_id.low);
      id.append(reinterpret_cast<const char*>(&part.own_id.high), sizeof part.own_id.high);
      part.full_id = xxh3_128(id, part.glue.size() + 1);
    }
  }

  Stores& kept = stores();
  // The digests: kept ones under the lock, the others outside it.
  {
    std::vector<bool> known(parts.size(), false);
    {
      const std::lock_guard lock(kept.digest_mutex);
      for (std::size_t i = 0; i < parts.size(); ++i) {
        if (auto sha = kept.digests.get(parts[i].own_id)) {
          parts[i].sha = *sha;
          known[i] = true;
        }
      }
    }
    std::vector<std::size_t> fresh;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (!known[i]) {
        parts[i].sha = sha256(parts[i].own);
        fresh.push_back(i);
      }
    }
    if (!fresh.empty()) {
      const std::lock_guard lock(kept.digest_mutex);
      for (const std::size_t i : fresh) kept.digests.insert(parts[i].own_id, parts[i].sha, kEntryOverhead);
    }
  }

  // The ETag: the stream of `parts_etag`, from the kept digests.
  Spliced out;
  {
    std::string stream;
    stream.reserve(parts.size() * 48);
    for (const Part& part : parts) {
      if (part.fragment) {
        stream.push_back('F');
        put_u64_le(stream, part.glue.size());
        stream.append(part.glue);
      } else {
        stream.push_back('T');
      }
      put_u64_le(stream, part.own.size());
      stream.append(reinterpret_cast<const char*>(part.sha.data()), part.sha.size());
    }
    const Digest32 digest = sha256(stream);
    static constexpr char kHex[] = "0123456789abcdef";
    out.etag = "W/\"";
    for (std::size_t i = 0; i < 16; ++i) {
      out.etag.push_back(kHex[digest[i] >> 4]);
      out.etag.push_back(kHex[digest[i] & 15]);
    }
    out.etag.push_back('"');
  }

  // The pieces: kept ones under the lock, the others compressed outside it.
  std::vector<std::shared_ptr<const Piece>> pieces(parts.size());
  std::vector<PieceKey> keys(parts.size());
  for (std::size_t i = 0; i < parts.size(); ++i) keys[i] = {parts[i].full_id, i == 0 ? Hash128{} : parts[i - 1].own_id};
  {
    const std::lock_guard lock(kept.piece_mutex);
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (auto piece = kept.pieces.get(keys[i])) pieces[i] = std::move(*piece);
    }
  }
  std::vector<std::size_t> fresh;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (pieces[i]) continue;
    auto piece = std::make_shared<Piece>();
    piece->deflated = compress(i == 0 ? std::string_view{} : parts[i - 1].own, parts[i].full);
    piece->crc = static_cast<std::uint32_t>(
        crc32(0, reinterpret_cast<const Bytef*>(parts[i].full.data()), static_cast<uInt>(parts[i].full.size())));
    piece->shift = x_to_the_8(parts[i].full.size());
    pieces[i] = std::move(piece);
    fresh.push_back(i);
  }
  if (!fresh.empty()) {
    const std::lock_guard lock(kept.piece_mutex);
    for (const std::size_t i : fresh)
      kept.pieces.insert(keys[i], pieces[i], pieces[i]->deflated.size() + kEntryOverhead);
  }

  // The gzip member: header (mtime 0, OS Unix), the pieces, an empty final block, the CRC and the size.
  std::size_t size = 18;
  for (const auto& piece : pieces) size += piece->deflated.size();
  out.gzip.reserve(size);
  out.gzip.append({'\x1f', '\x8b', '\x08', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x03'});
  std::uint32_t crc = 0;
  for (const auto& piece : pieces) {
    out.gzip.append(piece->deflated);
    crc = multiply(crc, piece->shift) ^ piece->crc;
  }
  out.gzip.append({'\x03', '\x00'});  // an empty final block with fixed codes, after the sync flushes
  put_u32_le(out.gzip, crc);
  put_u32_le(out.gzip, static_cast<std::uint32_t>(body.size()));
  return out;
}

Stats stats() {
  Stores& kept = stores();
  Stats out;
  {
    const std::lock_guard lock(kept.piece_mutex);
    out.pieces = kept.pieces.size();
    out.piece_bytes = kept.pieces.cost();
  }
  {
    const std::lock_guard lock(kept.digest_mutex);
    out.digests = kept.digests.size();
  }
  return out;
}

void clear() {
  Stores& kept = stores();
  {
    const std::lock_guard lock(kept.piece_mutex);
    kept.pieces.clear();
  }
  const std::lock_guard lock(kept.digest_mutex);
  kept.digests.clear();
}

}  // namespace campfire::app::splice
