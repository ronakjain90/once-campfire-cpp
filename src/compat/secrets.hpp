// Everything derived from SECRET_KEY_BASE, built once at boot and shared read only
// (Rust: rails_compat Secrets + KeyGenerator). Rails.application.key_generator is an
// ActiveSupport::CachingKeyGenerator over PBKDF2-HMAC-SHA256 with 1000 iterations
// (load_defaults 7.0+). The keys and the message objects Campfire needs are derived in the
// constructor, so the object never changes after that and many threads can read it.
#pragma once

#include <string>
#include <string_view>

#include "compat/message_encryptor.hpp"
#include "compat/message_verifier.hpp"

namespace campfire::compat {

class Secrets {
 public:
  static constexpr unsigned kIterations = 1000;
  static constexpr size_t kDefaultKeyLength = 64;

  explicit Secrets(std::string_view secret_key_base);

  // key_generator.generate_key(salt, length). It derives the key again on each call: use the
  // objects below for the keys Campfire uses.
  std::string generate_key(std::string_view salt, size_t length = kDefaultKeyLength) const;

  // cookies.signed: HMAC-SHA1 (signed_cookie_digest is unset), key "signed cookie".
  const MessageVerifier& signed_cookie_verifier() const { return signed_cookie_verifier_; }
  // cookies.encrypted: aes-256-gcm, key "authenticated encrypted cookie" (32 bytes).
  const MessageEncryptor& encrypted_cookie_encryptor() const { return encrypted_cookie_encryptor_; }
  // ActiveRecord::SignedId: generates with SHA256 / ::JSON / URL-safe Base64, and reads the app
  // default (SHA1, :json_allow_marshal, strict Base64) as a fall back.
  const MessageVerifier& signed_id_verifier() const { return signed_id_verifier_; }
  // GlobalID::Verifier, key "signed_global_ids".
  const MessageVerifier& global_id_verifier() const { return global_id_verifier_; }
  // Turbo.signed_stream_verifier_key: SHA256, ::JSON, strict Base64, no envelope.
  const MessageVerifier& turbo_stream_verifier() const { return turbo_stream_verifier_; }
  // Rails.application.message_verifier("ActiveStorage"): key generate_key(name, 64), SHA1,
  // strict Base64, :json_allow_marshal. Blob signed ids use purpose "blob_id", not signed_id's scheme.
  const MessageVerifier& active_storage_verifier() const { return active_storage_verifier_; }
  // Rails.application.message_verifier(name) for any other name.
  MessageVerifier app_verifier(std::string_view name) const;

 private:
  std::string secret_key_base_;
  MessageVerifier signed_cookie_verifier_;
  MessageEncryptor encrypted_cookie_encryptor_;
  MessageVerifier signed_id_verifier_;
  MessageVerifier global_id_verifier_;
  MessageVerifier turbo_stream_verifier_;
  MessageVerifier active_storage_verifier_;
};

}  // namespace campfire::compat
