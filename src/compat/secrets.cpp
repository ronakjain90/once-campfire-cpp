// Derived secrets (see secrets.hpp).
#include "compat/secrets.hpp"

namespace campfire::compat {
namespace {

MessageVerifier make_app_verifier(std::string key) {
  return MessageVerifier(std::move(key), crypto::Digest::Sha1, Base64Encoding::Strict,
                         Serializer::json_with_fallback(true));
}

MessageVerifier make_signed_id_verifier(const std::string& key) {
  MessageVerifier verifier(key, crypto::Digest::Sha256, Base64Encoding::UrlSafe, Serializer::json());
  verifier.fall_back_to(make_app_verifier(key));
  return verifier;
}

}  // namespace

Secrets::Secrets(std::string_view secret_key_base)
    : secret_key_base_(secret_key_base),
      signed_cookie_verifier_(generate_key("signed cookie", 64), crypto::Digest::Sha1, Base64Encoding::Strict,
                              Serializer::null()),
      encrypted_cookie_encryptor_(generate_key("authenticated encrypted cookie", 32), Serializer::null()),
      signed_id_verifier_(make_signed_id_verifier(generate_key("active_record/signed_id", 64))),
      global_id_verifier_(generate_key("signed_global_ids", 64), crypto::Digest::Sha1, Base64Encoding::UrlSafePadded,
                          Serializer::json_with_fallback(true)),
      turbo_stream_verifier_(generate_key("turbo/signed_stream_verifier_key", 64), crypto::Digest::Sha256,
                             Base64Encoding::Strict, Serializer::json()),
      active_storage_verifier_(make_app_verifier(generate_key("ActiveStorage", 64))) {}

std::string Secrets::generate_key(std::string_view salt, size_t length) const {
  return crypto::pbkdf2_sha256(secret_key_base_, salt, kIterations, length);
}

MessageVerifier Secrets::app_verifier(std::string_view name) const {
  return make_app_verifier(generate_key(name, 64));
}

}  // namespace campfire::compat
