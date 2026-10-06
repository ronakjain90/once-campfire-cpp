// OpenSSL helpers for certificates and keys. Rust: crates/kit/src/front/acme.rs.
#include "net/front/tls_keys.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>

namespace campfire::net::front {

namespace {

struct BioDeleter {
  void operator()(BIO* p) const noexcept { BIO_free_all(p); }
};
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

std::string ssl_error() {
  const unsigned long code = ERR_get_error();  // NOLINT(google-runtime-int)
  if (code == 0) return "unknown OpenSSL error";
  char buffer[256];
  ERR_error_string_n(code, buffer, sizeof buffer);
  ERR_clear_error();
  return buffer;
}

std::string bio_text(BIO* bio) {
  char* data = nullptr;
  const long size = BIO_get_mem_data(bio, &data);  // NOLINT(google-runtime-int)
  return std::string(data, static_cast<std::size_t>(size));
}

BioPtr memory_bio(std::string_view text) {
  return BioPtr(BIO_new_mem_buf(text.data(), static_cast<int>(text.size())));
}

}  // namespace

Result<PKeyPtr> generate_p256_key() {
  PKeyPtr key(EVP_EC_gen("P-256"));
  if (!key) return fail(Errc::Internal, "cannot make a P-256 key: " + ssl_error());
  return key;
}

Result<std::string> private_key_pem(EVP_PKEY* key) {
  BioPtr bio(BIO_new(BIO_s_mem()));
  if (PEM_write_bio_PrivateKey_traditional(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
    return fail(Errc::Internal, "cannot write the private key: " + ssl_error());
  }
  return bio_text(bio.get());
}

Result<PKeyPtr> parse_private_key(std::string_view pem) {
  BioPtr bio = memory_bio(pem);
  PKeyPtr key(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
  if (!key) return fail(Errc::Parse, "private key: " + ssl_error());
  return key;
}

std::string certificate_pem(X509* cert) {
  BioPtr bio(BIO_new(BIO_s_mem()));
  PEM_write_bio_X509(bio.get(), cert);
  return bio_text(bio.get());
}

Result<std::string> cache_entry(EVP_PKEY* key, std::string_view chain_pem) {
  auto pem = private_key_pem(key);
  if (!pem) return pem;
  BioPtr bio = memory_bio(chain_pem);
  int count = 0;
  while (X509Ptr cert{PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr)}) {
    *pem += certificate_pem(cert.get());
    ++count;
  }
  ERR_clear_error();
  if (count == 0) return fail(Errc::Parse, "the chain has no certificate");
  return pem;
}

Result<std::shared_ptr<CertifiedKey>> parse_cached(std::string_view pem, std::string_view domain, std::time_t now) {
  auto key = parse_private_key(pem);
  if (!key) return std::unexpected(key.error());
  auto result = std::make_shared<CertifiedKey>();
  result->key = std::move(*key);
  BioPtr bio = memory_bio(pem);
  while (X509Ptr cert{PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr)}) {
    if (!result->leaf) {
      result->leaf = std::move(cert);
    } else {
      result->chain.push_back(std::move(cert));
    }
  }
  ERR_clear_error();
  if (!result->leaf) return fail(Errc::Parse, "no certificate");
  X509* leaf = result->leaf.get();
  if (X509_cmp_time(X509_get0_notBefore(leaf), &now) > 0 || X509_cmp_time(X509_get0_notAfter(leaf), &now) < 0) {
    return fail(Errc::Parse, "certificate is expired or not yet valid");
  }
  if (X509_check_host(leaf, domain.data(), domain.size(),
                      X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS | X509_CHECK_FLAG_NEVER_CHECK_SUBJECT, nullptr) != 1) {
    return fail(Errc::Parse, "certificate is not valid for " + std::string(domain));
  }
  if (X509_check_private_key(leaf, result->key.get()) != 1) {
    ERR_clear_error();
    return fail(Errc::Parse, "the private key does not match the certificate");
  }
  tm end{};
  ASN1_TIME_to_tm(X509_get0_notAfter(leaf), &end);
  result->not_after = timegm(&end);
  return result;
}

Status write_cache_file(const std::filesystem::path& dir, std::string_view name, std::string_view data) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) return fail(Errc::Io, "cannot create " + dir.string() + ": " + ec.message());
  ::chmod(dir.c_str(), 0700);  // NOLINT(hicpp-signed-bitwise)
  const std::filesystem::path temporary = dir / (std::string(name) + ".tmp" + std::to_string(::getpid()));
  const int fd =
      ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);  // NOLINT(cppcoreguidelines-pro-type-vararg)
  if (fd < 0) return fail(Errc::Io, "cannot write " + temporary.string());
  std::size_t done = 0;
  while (done < data.size()) {
    const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
    if (n <= 0) {
      ::close(fd);
      return fail(Errc::Io, "cannot write " + temporary.string());
    }
    done += static_cast<std::size_t>(n);
  }
  ::fsync(fd);
  ::close(fd);
  std::filesystem::rename(temporary, dir / std::string(name), ec);
  if (ec) return fail(Errc::Io, "cannot rename " + temporary.string() + ": " + ec.message());
  return {};
}

namespace {

X509Ptr new_certificate(EVP_PKEY* key, std::string_view domain, std::time_t not_before, std::time_t not_after) {
  X509Ptr cert(X509_new());
  X509_set_version(cert.get(), 2);
  unsigned char serial[16];
  RAND_bytes(serial, sizeof serial);
  serial[0] &= 0x7F;
  BIGNUM* bn = BN_bin2bn(serial, sizeof serial, nullptr);
  BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(cert.get()));
  BN_free(bn);
  X509_time_adj_ex(X509_getm_notBefore(cert.get()), 0, 0, &not_before);
  X509_time_adj_ex(X509_getm_notAfter(cert.get()), 0, 0, &not_after);
  X509_set_pubkey(cert.get(), key);
  X509_NAME* name = X509_get_subject_name(cert.get());
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>(domain.data()),
                             static_cast<int>(domain.size()), -1, 0);
  X509_set_issuer_name(cert.get(), name);
  return cert;
}

bool add_extension(X509* cert, int nid, const std::string& value, bool critical = false) {
  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
  const std::string text = (critical ? "critical," : "") + value;
  X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, text.c_str());
  if (ext == nullptr) return false;
  const int ok = X509_add_ext(cert, ext, -1);
  X509_EXTENSION_free(ext);
  return ok == 1;
}

}  // namespace

Result<std::shared_ptr<CertifiedKey>> challenge_certificate(std::string_view domain, std::string_view digest) {
  auto key = generate_p256_key();
  if (!key) return std::unexpected(key.error());
  const std::time_t now = std::time(nullptr);
  X509Ptr cert = new_certificate(key->get(), domain, now - 3600, now + 7 * 24 * 3600);
  if (!add_extension(cert.get(), NID_subject_alt_name, "DNS:" + std::string(domain))) {
    return fail(Errc::Internal, "cannot add the name: " + ssl_error());
  }
  // The acmeIdentifier extension (RFC 8737): an OCTET STRING of the digest, critical.
  ASN1_OCTET_STRING* inner = ASN1_OCTET_STRING_new();
  ASN1_OCTET_STRING_set(inner, reinterpret_cast<const unsigned char*>(digest.data()), static_cast<int>(digest.size()));
  unsigned char* der = nullptr;
  const int der_size = i2d_ASN1_OCTET_STRING(inner, &der);
  ASN1_OCTET_STRING_free(inner);
  ASN1_OBJECT* oid = OBJ_txt2obj("1.3.6.1.5.5.7.1.31", 1);
  ASN1_OCTET_STRING* value = ASN1_OCTET_STRING_new();
  ASN1_OCTET_STRING_set(value, der, der_size);
  OPENSSL_free(der);
  X509_EXTENSION* ext = X509_EXTENSION_create_by_OBJ(nullptr, oid, 1, value);
  ASN1_OBJECT_free(oid);
  ASN1_OCTET_STRING_free(value);
  const int added = X509_add_ext(cert.get(), ext, -1);
  X509_EXTENSION_free(ext);
  if (added != 1) return fail(Errc::Internal, "cannot add the acmeIdentifier extension: " + ssl_error());
  if (X509_sign(cert.get(), key->get(), EVP_sha256()) == 0) return fail(Errc::Internal, "cannot sign: " + ssl_error());
  auto result = std::make_shared<CertifiedKey>();
  result->leaf = std::move(cert);
  result->key = std::move(*key);
  result->not_after = now + 7 * 24 * 3600;
  return result;
}

Result<std::shared_ptr<CertifiedKey>> self_signed(std::string_view domain, std::time_t not_before,
                                                  std::time_t not_after) {
  auto key = generate_p256_key();
  if (!key) return std::unexpected(key.error());
  X509Ptr cert = new_certificate(key->get(), domain, not_before, not_after);
  if (!add_extension(cert.get(), NID_subject_alt_name, "DNS:" + std::string(domain))) {
    return fail(Errc::Internal, "cannot add the name: " + ssl_error());
  }
  if (X509_sign(cert.get(), key->get(), EVP_sha256()) == 0) return fail(Errc::Internal, "cannot sign: " + ssl_error());
  auto result = std::make_shared<CertifiedKey>();
  result->leaf = std::move(cert);
  result->key = std::move(*key);
  result->not_after = not_after;
  return result;
}

Result<std::string> make_csr(std::string_view domain, EVP_PKEY* key) {
  std::unique_ptr<X509_REQ, decltype(&X509_REQ_free)> request(X509_REQ_new(), &X509_REQ_free);
  X509_REQ_set_version(request.get(), 0);
  X509_NAME* name = X509_REQ_get_subject_name(request.get());
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>(domain.data()),
                             static_cast<int>(domain.size()), -1, 0);
  X509_REQ_set_pubkey(request.get(), key);
  STACK_OF(X509_EXTENSION)* extensions = sk_X509_EXTENSION_new_null();
  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, nullptr, nullptr, request.get(), nullptr, 0);
  const std::string san = "DNS:" + std::string(domain);
  X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name, san.c_str());
  sk_X509_EXTENSION_push(extensions, ext);
  X509_REQ_add_extensions(request.get(), extensions);
  sk_X509_EXTENSION_pop_free(extensions, X509_EXTENSION_free);
  if (X509_REQ_sign(request.get(), key, EVP_sha256()) == 0)
    return fail(Errc::Internal, "cannot sign the CSR: " + ssl_error());
  unsigned char* der = nullptr;
  const int size = i2d_X509_REQ(request.get(), &der);
  if (size <= 0) return fail(Errc::Internal, "cannot encode the CSR");
  std::string out(reinterpret_cast<char*>(der), static_cast<std::size_t>(size));
  OPENSSL_free(der);
  return out;
}

namespace {

// RFC 3492 encoding of one label with non-ASCII code points (as UTF-32), no "xn--" prefix.
std::string punycode(const std::vector<char32_t>& input) {
  constexpr std::uint32_t kBase = 36;
  constexpr std::uint32_t kTMin = 1;
  constexpr std::uint32_t kTMax = 26;
  constexpr std::uint32_t kSkew = 38;
  constexpr std::uint32_t kDamp = 700;
  const auto digit = [](std::uint32_t d) { return static_cast<char>(d < 26 ? 'a' + d : '0' + (d - 26)); };
  const auto adapt = [&](std::uint32_t delta, std::uint32_t points, bool first) {
    delta = first ? delta / kDamp : delta / 2;
    delta += delta / points;
    std::uint32_t k = 0;
    while (delta > ((kBase - kTMin) * kTMax) / 2) {
      delta /= kBase - kTMin;
      k += kBase;
    }
    return k + (kBase - kTMin + 1) * delta / (delta + kSkew);
  };
  std::string out;
  for (const char32_t c : input) {
    if (c < 0x80) out += static_cast<char>(c);
  }
  const std::uint32_t basic = static_cast<std::uint32_t>(out.size());
  std::uint32_t handled = basic;
  if (basic > 0) out += '-';
  std::uint32_t n = 128;
  std::uint32_t delta = 0;
  std::uint32_t bias = 72;
  while (handled < input.size()) {
    std::uint32_t m = 0xFFFFFFFFU;
    for (const char32_t c : input) {
      if (c >= n && c < m) m = c;
    }
    delta += (m - n) * (handled + 1);
    n = m;
    for (const char32_t c : input) {
      if (c < n) ++delta;
      if (c == n) {
        std::uint32_t q = delta;
        for (std::uint32_t k = kBase;; k += kBase) {
          const std::uint32_t t = k <= bias ? kTMin : (k >= bias + kTMax ? kTMax : k - bias);
          if (q < t) break;
          out += digit(t + (q - t) % (kBase - t));
          q = (q - t) / (kBase - t);
        }
        out += digit(q);
        bias = adapt(delta, handled + 1, handled == basic);
        delta = 0;
        ++handled;
      }
    }
    ++delta;
    ++n;
  }
  return out;
}

bool decode_utf8(std::string_view text, std::vector<char32_t>& out) {
  for (std::size_t i = 0; i < text.size();) {
    const auto b = static_cast<unsigned char>(text[i]);
    int extra = 0;
    char32_t cp = 0;
    if (b < 0x80) {
      cp = b;
    } else if ((b & 0xE0) == 0xC0) {
      cp = b & 0x1F;
      extra = 1;
    } else if ((b & 0xF0) == 0xE0) {
      cp = b & 0x0F;
      extra = 2;
    } else if ((b & 0xF8) == 0xF0) {
      cp = b & 0x07;
      extra = 3;
    } else {
      return false;
    }
    if (extra > 0 && i + static_cast<std::size_t>(extra) >= text.size()) return false;
    for (int k = 1; k <= extra; ++k) {
      const auto c = static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
      if ((c & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (c & 0x3F);
    }
    out.push_back(cp);
    i += static_cast<std::size_t>(extra) + 1;
  }
  return true;
}

}  // namespace

std::string normalize_domain(std::string_view host) {
  if (host.empty() || host.size() > 253) return {};
  std::string out;
  bool first_label = true;
  std::size_t begin = 0;
  while (begin <= host.size()) {
    std::size_t end = host.find('.', begin);
    if (end == std::string_view::npos) end = host.size();
    const std::string_view label = host.substr(begin, end - begin);
    begin = end + 1;
    std::vector<char32_t> points;
    if (!decode_utf8(label, points)) return {};
    const bool ascii = std::all_of(points.begin(), points.end(), [](char32_t c) { return c < 0x80; });
    if (!first_label) out += '.';
    first_label = false;
    if (ascii) {
      for (const char32_t c : points) {
        const auto ch = static_cast<char>(c);
        if (!(std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '-' || ch == '_')) return {};
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      }
    } else {
      out += "xn--" + punycode(points);
    }
  }
  // An IP address is not a domain name.
  in_addr v4{};
  in6_addr v6{};
  if (inet_pton(AF_INET, out.c_str(), &v4) == 1 || inet_pton(AF_INET6, out.c_str(), &v6) == 1) return {};
  return out;
}

}  // namespace campfire::net::front
