// Web Push without the network: RFC 8291 message encryption (the web-push gem, `WebPush::Encryption.encrypt`), RFC 8292
// VAPID identification (`WebPush::Request#build_vapid_header`) and the bytes of a notification. Rails:
// reference/lib/web_push, config/initializers/web_push.rb. Rust: crates/campfire/src/integrations/web_push/*.rs.
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace campfire::jobs::web_push {

// What the gem raises, as a kind and the message of the exception.
struct PushError {
  enum class Kind : std::uint8_t {
    Argument,    // `ArgumentError`: a blank argument, bad Base64, or a payload over 4096 bytes
    InvalidKey,  // `OpenSSL::PKey::EC::Point::Error`: the key of the subscription is not a P-256 point
  };
  Kind kind = Kind::Argument;
  std::string message;
};

// `WebPush::Notification#deliver` limits and headers.
inline constexpr std::uint64_t kTtlSeconds = 60ULL * 60 * 24 * 7 * 4;  // `WebPush::Request` default: four weeks
inline constexpr std::string_view kUrgency = "high";

// The framing of the record. The gem writes the length of the ciphertext in the record size field and pads the
// plaintext with the bytes 0x02 0x00. RFC 8291 (section 5) uses 4096 and the byte 0x02: the test vector needs it.
struct Layout {
  std::optional<std::uint32_t> record_size;  // nothing: the length of the ciphertext
  std::string_view padding{"\x02\x00", 2};
};

// Encrypts `message` for the `p256dh` key and the `auth` secret of a subscription (both urlsafe Base64), with a new
// server key and salt. The result is the body of the request: salt, record size, key id length, server key, record.
[[nodiscard]] std::expected<std::string, PushError> encrypt(std::string_view message,
                                                            std::optional<std::string_view> p256dh,
                                                            std::optional<std::string_view> auth);
// The same with the server key (32 bytes of the private scalar) and the salt (16 bytes) given.
[[nodiscard]] std::expected<std::string, PushError> encrypt_with(std::string_view message,
                                                                 std::optional<std::string_view> p256dh,
                                                                 std::optional<std::string_view> auth,
                                                                 std::string_view server_private_key,
                                                                 std::string_view salt, const Layout& layout);

// What a user agent does with the body (RFC 8291, section 3.4). For tests and tools: `receiver_private_key` is the
// 32 bytes of the scalar and `auth` the raw secret. Gives the record size and the plaintext.
struct Decrypted {
  std::uint32_t record_size = 0;
  std::string plaintext;
};
[[nodiscard]] std::optional<Decrypted> decrypt(std::string_view body, std::string_view receiver_private_key,
                                               std::string_view auth);

// The public key (65 bytes, uncompressed) of a private scalar. Empty for a bad scalar. For tests and tools.
[[nodiscard]] std::string public_key_of(std::string_view private_key);
// A new key pair: the private scalar (32 bytes) and the public key (65 bytes).
struct KeyPair {
  std::string private_key;
  std::string public_key;
};
[[nodiscard]] KeyPair generate_key_pair();

enum class VapidErrc : std::uint8_t { InvalidPublicKey, InvalidPrivateKey, Mismatched };

// `VapidKey.from_keys(public_key, private_key)` and the subject: parsed once at boot, so that a bad key turns Web Push
// off instead of failing each delivery.
class Vapid {
 public:
  [[nodiscard]] static std::expected<Vapid, VapidErrc> create(std::string_view subject, std::string_view public_key,
                                                              std::string_view private_key);
  Vapid(Vapid&&) noexcept;
  Vapid& operator=(Vapid&&) noexcept;
  ~Vapid();

  // The `Authorization` header value for the push service at `audience` (`scheme://host`): `vapid t=<JWT>,k=<key>`.
  // The JWT has `aud`, `exp` (12 hours after `now`) and `sub`.
  [[nodiscard]] std::string authorization(std::string_view audience, std::int64_t now) const;

 private:
  struct Impl;
  explicit Vapid(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view to_string(VapidErrc code) noexcept;

// `Random.uuid`: a version 4 UUID, in lower case.
[[nodiscard]] std::string random_uuid();

// `Push::Subscription#notification(...).encoded_message`: `JSON.generate` of the title and the options, in this order.
[[nodiscard]] std::string encoded_message(std::string_view title, std::string_view body, std::string_view path,
                                          std::int64_t badge);
// `text`, cut short with an ellipsis if it takes more than `max_bytes` as the content of a JSON string.
[[nodiscard]] std::string truncate_json_string(std::string text, std::size_t max_bytes);

// `Room::MessagePusher#build_payload`, with the limits of the Rust port. The title is at most 256 bytes and the body
// at most 3072 bytes in the JSON message: a record holds 4096 bytes at most.
inline constexpr std::size_t kMaxPayloadTitleBytes = 256;
inline constexpr std::size_t kMaxPayloadBodyBytes = 3072;

}  // namespace campfire::jobs::web_push
