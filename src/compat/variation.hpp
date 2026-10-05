// ActiveStorage::Variation: an ordered transformations hash, its Marshal-based digest (the
// variation_digest column) and its signed URL key (Rust: crates/storage/src/variation.rs).
#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compat/marshal.hpp"
#include "compat/message_verifier.hpp"

namespace campfire::compat {

class Variation {
 public:
  using Transformations = std::vector<std::pair<std::string, marshal::Value>>;

  explicit Variation(Transformations transformations) : transformations_(std::move(transformations)) {}

  // resize_to_limit: [width, height] plus an optional format: symbol.
  static Variation resize_to_limit(int64_t width, int64_t height, std::optional<std::string_view> format);

  const Transformations& transformations() const { return transformations_; }

  // Marshal.dump(transformations).
  std::string marshal() const;
  // OpenSSL::Digest::SHA1.base64digest Marshal.dump(transformations).
  std::string digest() const;
  // Variation#key: ActiveStorage.verifier.generate(transformations, purpose: :variation).
  // Symbols become strings in the JSON.
  std::string key(const MessageVerifier& verifier) const;
  // Variation.decode: keys are symbols, values stay strings. Nullopt for a bad key, a wrong
  // purpose or a float argument.
  static std::optional<Variation> decode(const MessageVerifier& verifier, std::string_view key, Timestamp now);

  friend bool operator==(const Variation&, const Variation&) = default;

 private:
  Transformations transformations_;
};

}  // namespace campfire::compat
