// Thin OpenSSL 3 wrappers (see crypto.hpp).
#include "compat/crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <cstdlib>
#include <memory>

namespace campfire::compat::crypto {
namespace {

struct CtxDeleter {
  void operator()(EVP_CIPHER_CTX* ctx) const { EVP_CIPHER_CTX_free(ctx); }
};
using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, CtxDeleter>;

const unsigned char* bytes(std::string_view s) {
  return reinterpret_cast<const unsigned char*>(s.data());
}
unsigned char* bytes(std::string& s) {
  return reinterpret_cast<unsigned char*>(s.data());
}

}  // namespace

std::string pbkdf2_sha256(std::string_view password, std::string_view salt, unsigned iterations, size_t length) {
  std::string key(length, '\0');
  int ok = PKCS5_PBKDF2_HMAC(password.data(), int(password.size()), bytes(salt), int(salt.size()), int(iterations),
                             EVP_sha256(), int(length), bytes(key));
  if (ok != 1) std::abort();
  return key;
}

std::string hmac(Digest digest, std::string_view key, std::string_view data) {
  const EVP_MD* md = digest == Digest::Sha1 ? EVP_sha1() : EVP_sha256();
  unsigned char out[EVP_MAX_MD_SIZE];
  unsigned len = 0;
  // HMAC() with an empty key needs a non-null pointer.
  const void* key_ptr = key.empty() ? static_cast<const void*>("") : key.data();
  if (HMAC(md, key_ptr, int(key.size()), bytes(data), data.size(), out, &len) == nullptr) std::abort();
  return std::string(reinterpret_cast<char*>(out), len);
}

std::string sha1(std::string_view data) {
  unsigned char out[SHA_DIGEST_LENGTH];
  SHA1(bytes(data), data.size(), out);
  return std::string(reinterpret_cast<char*>(out), SHA_DIGEST_LENGTH);
}

bool constant_time_equal(std::string_view a, std::string_view b) {
  return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string hex_encode(std::string_view in) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(in.size() * 2);
  for (unsigned char c : in) {
    out += kHex[c >> 4];
    out += kHex[c & 15];
  }
  return out;
}

std::optional<std::string> hex_decode(std::string_view hex) {
  if (hex.size() % 2 != 0) return std::nullopt;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    int hi = nibble(hex[i]);
    int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out += char(hi << 4 | lo);
  }
  return out;
}

std::optional<GcmSealed> aes256_gcm_encrypt(std::string_view key, std::string_view iv, std::string_view plaintext) {
  if (key.size() != 32 || iv.size() != kGcmIvLength) return std::nullopt;
  CipherCtx ctx(EVP_CIPHER_CTX_new());
  if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, int(kGcmIvLength), nullptr) != 1 ||
      EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, bytes(key), bytes(iv)) != 1) {
    return std::nullopt;
  }
  GcmSealed sealed;
  sealed.ciphertext.resize(plaintext.size());
  int len = 0;
  if (!plaintext.empty() &&
      EVP_EncryptUpdate(ctx.get(), bytes(sealed.ciphertext), &len, bytes(plaintext), int(plaintext.size())) != 1) {
    return std::nullopt;
  }
  int total = len;
  unsigned char final_block[16];
  if (EVP_EncryptFinal_ex(ctx.get(), final_block, &len) != 1) return std::nullopt;
  sealed.ciphertext.resize(size_t(total));
  sealed.tag.resize(kGcmTagLength);
  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, int(kGcmTagLength), sealed.tag.data()) != 1) {
    return std::nullopt;
  }
  return sealed;
}

std::optional<std::string> aes256_gcm_decrypt(std::string_view key, std::string_view iv, std::string_view ciphertext,
                                              std::string_view tag) {
  if (key.size() != 32 || iv.size() != kGcmIvLength || tag.size() != kGcmTagLength) return std::nullopt;
  CipherCtx ctx(EVP_CIPHER_CTX_new());
  if (!ctx || EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, int(kGcmIvLength), nullptr) != 1 ||
      EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, bytes(key), bytes(iv)) != 1) {
    return std::nullopt;
  }
  std::string plaintext(ciphertext.size(), '\0');
  int len = 0;
  if (!ciphertext.empty() &&
      EVP_DecryptUpdate(ctx.get(), bytes(plaintext), &len, bytes(ciphertext), int(ciphertext.size())) != 1) {
    return std::nullopt;
  }
  int total = len;
  std::string tag_copy(tag);
  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, int(kGcmTagLength), tag_copy.data()) != 1)
    return std::nullopt;
  unsigned char final_block[16];
  if (EVP_DecryptFinal_ex(ctx.get(), final_block, &len) != 1) return std::nullopt;
  plaintext.resize(size_t(total));
  return plaintext;
}

void random_bytes(std::string& out, size_t length) {
  out.assign(length, '\0');
  if (RAND_bytes(bytes(out), int(length)) != 1) std::abort();
}

}  // namespace campfire::compat::crypto
