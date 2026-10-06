// OpenSSL helpers for certificates and keys: the cache files of Thruster (autocert's DirCache), challenge
// certificates, CSR. Rust: crates/kit/src/front/acme.rs (cache_entry, parse_cached, challenge_certificate).
#pragma once

#include <openssl/evp.h>
#include <openssl/x509.h>

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.hpp"

namespace campfire::net::front {

struct X509Deleter {
  void operator()(X509* p) const noexcept { X509_free(p); }
};
struct PKeyDeleter {
  void operator()(EVP_PKEY* p) const noexcept { EVP_PKEY_free(p); }
};
using X509Ptr = std::unique_ptr<X509, X509Deleter>;
using PKeyPtr = std::unique_ptr<EVP_PKEY, PKeyDeleter>;

// A private key with its certificate chain (leaf first). Immutable after it is made.
struct CertifiedKey {
  X509Ptr leaf;
  std::vector<X509Ptr> chain;  // the certificates behind the leaf
  PKeyPtr key;
  std::time_t not_after = 0;
};

// A P-256 key.
[[nodiscard]] Result<PKeyPtr> generate_p256_key();

// The PEM of a P-256 key as autocert writes it: SEC1 "EC PRIVATE KEY" with the curve named.
[[nodiscard]] Result<std::string> private_key_pem(EVP_PKEY* key);
// The first private key of a PEM text (SEC1 or PKCS#8).
[[nodiscard]] Result<PKeyPtr> parse_private_key(std::string_view pem);

// A cache file for a domain: the private key, then the certificates.
[[nodiscard]] Result<std::string> cache_entry(EVP_PKEY* key, std::string_view chain_pem);

// `validCert` of autocert: the leaf covers `domain`, it is current at `now`, and it matches the key.
[[nodiscard]] Result<std::shared_ptr<CertifiedKey>> parse_cached(std::string_view pem, std::string_view domain,
                                                                 std::time_t now);

// `DirCache.Put`: the directory 0700, the file 0600, written through a temporary file.
[[nodiscard]] Status write_cache_file(const std::filesystem::path& dir, std::string_view name, std::string_view data);

// A TLS-ALPN-01 challenge certificate (RFC 8737). `digest` is the SHA-256 of the key authorization.
[[nodiscard]] Result<std::shared_ptr<CertifiedKey>> challenge_certificate(std::string_view domain,
                                                                          std::string_view digest);

// A CSR (DER) for one DNS name, signed with `key`.
[[nodiscard]] Result<std::string> make_csr(std::string_view domain, EVP_PKEY* key);

// Self-signed certificate for tests and tools.
[[nodiscard]] Result<std::shared_ptr<CertifiedKey>> self_signed(std::string_view domain, std::time_t not_before,
                                                                std::time_t not_after);

// PEM encoding of a certificate.
[[nodiscard]] std::string certificate_pem(X509* cert);

// `idna.Lookup.ToASCII` for host names: lowercase, ASCII only (Rust: url::Host::parse). Returns an
// empty text for a name that is not a domain (an IP address, bad characters, non-ASCII).
[[nodiscard]] std::string normalize_domain(std::string_view host);

}  // namespace campfire::net::front
