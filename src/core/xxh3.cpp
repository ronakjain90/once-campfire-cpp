// XXH3 128-bit. The implementation is the vendored xxhash 0.8.3 (vendor/xxhash).
#include "core/xxh3.hpp"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <array>
#include <bit>
#include <cstdio>

namespace campfire {

struct Xxh3State::Impl {
  XXH3_state_t state;
};

std::string Hash128::hex() const {
  std::array<char, 33> text{};
  std::snprintf(text.data(), text.size(), "%016llx%016llx", static_cast<unsigned long long>(high),
                static_cast<unsigned long long>(low));
  return {text.data(), 32};
}

Hash128 xxh3_128(std::string_view data, std::uint64_t seed) noexcept {
  const XXH128_hash_t h = XXH3_128bits_withSeed(data.data(), data.size(), seed);
  return {h.low64, h.high64};
}

Xxh3State::Xxh3State(std::uint64_t seed) : impl_(std::make_unique<Impl>()) {
  reset(seed);
}
Xxh3State::Xxh3State(Xxh3State&&) noexcept = default;
Xxh3State& Xxh3State::operator=(Xxh3State&&) noexcept = default;
Xxh3State::~Xxh3State() = default;

void Xxh3State::reset(std::uint64_t seed) noexcept {
  XXH3_128bits_reset_withSeed(&impl_->state, seed);
}

void Xxh3State::update(std::string_view data) noexcept {
  XXH3_128bits_update(&impl_->state, data.data(), data.size());
}

void Xxh3State::update_u64(std::uint64_t value) noexcept {
  std::array<unsigned char, 8> bytes{};
  for (std::size_t i = 0; i < 8; ++i) {
    bytes[i] = static_cast<unsigned char>(value >> (8 * i));
  }
  XXH3_128bits_update(&impl_->state, bytes.data(), bytes.size());
}

Hash128 Xxh3State::digest() const noexcept {
  const XXH128_hash_t h = XXH3_128bits_digest(&impl_->state);
  return {h.low64, h.high64};
}

}  // namespace campfire
