// libFuzzer target: every verifier and the encryptor of Secrets read a message from the network
// (cookies, signed ids, SGIDs, Turbo stream names, Active Storage ids).
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "compat/global_id.hpp"
#include "compat/secrets.hpp"
#include "compat/signed_id.hpp"
#include "compat/time.hpp"
#include "compat/turbo.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace campfire::compat;
  static const Secrets secrets("fuzz-secret-key-base");
  const Timestamp now{std::chrono::seconds(1'700'000'000)};
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  (void)secrets.signed_cookie_verifier().verify(text, "session", now);
  (void)secrets.signed_cookie_verifier().verify_raw(text, std::nullopt, now);
  (void)secrets.encrypted_cookie_encryptor().decrypt_and_verify(text, "cookie.session", now);
  (void)secrets.encrypted_cookie_encryptor().decrypt(text);
  (void)secrets.signed_id_verifier().verify(text, std::nullopt, now);
  (void)secrets.global_id_verifier().verify(text, "attachable", now);
  (void)secrets.turbo_stream_verifier().verify(text, std::nullopt, now);
  (void)secrets.active_storage_verifier().verify(text, "blob_id", now);
  (void)signed_id::verify_blob_signed_id(secrets, text, now);
  (void)global_id::locate_signed(secrets, text, "attachable", now);
  (void)turbo::verified_stream_name(secrets, text);
  // A message with a valid MAC or tag and the input as its content: this reaches the code that reads
  // the content (envelope, purpose, expiry, JSON).
  for (const MessageVerifier* v : {&secrets.signed_cookie_verifier(), &secrets.signed_id_verifier(),
                                   &secrets.global_id_verifier(), &secrets.active_storage_verifier()}) {
    const std::string message = v->generate_raw(text, "p", now + std::chrono::hours(1));
    (void)v->verify(message, "p", now);
    (void)v->verify_raw(message, "p", now);
    (void)v->verify(v->generate_raw(text), std::nullopt, now);
  }
  const std::string iv(12, 'i');
  (void)secrets.encrypted_cookie_encryptor().decrypt_and_verify(
      secrets.encrypted_cookie_encryptor().encrypt_with_iv(text, iv), "cookie.session", now);
  return 0;
}
