// Rails: reference/lib/web_push (web-push gem 3.1.0). Rust:
// crates/campfire/src/integrations/web_push/{encryption,vapid}.rs.
#include "jobs/web_push.hpp"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>

#include <array>
#include <cstring>

#include "compat/base64.hpp"

namespace campfire::jobs::web_push {

namespace {

template <class T, void (*Free)(T*)>
struct Deleter {
  void operator()(T* p) const { Free(p); }
};
using Pkey = std::unique_ptr<EVP_PKEY, Deleter<EVP_PKEY, EVP_PKEY_free>>;
using PkeyCtx = std::unique_ptr<EVP_PKEY_CTX, Deleter<EVP_PKEY_CTX, EVP_PKEY_CTX_free>>;
using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, Deleter<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free>>;
using MdCtx = std::unique_ptr<EVP_MD_CTX, Deleter<EVP_MD_CTX, EVP_MD_CTX_free>>;
using Bn = std::unique_ptr<BIGNUM, Deleter<BIGNUM, BN_free>>;

const unsigned char* bytes(std::string_view s) {
  return reinterpret_cast<const unsigned char*>(s.data());
}

// The public point (uncompressed octets) of a private scalar on P-256. Empty if the scalar is not valid.
std::string point_of_scalar(std::string_view scalar) {
  std::unique_ptr<EC_GROUP, Deleter<EC_GROUP, EC_GROUP_free>> group(EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1));
  std::unique_ptr<EC_POINT, Deleter<EC_POINT, EC_POINT_free>> point(EC_POINT_new(group.get()));
  const Bn bn(BN_bin2bn(bytes(scalar), static_cast<int>(scalar.size()), nullptr));
  if (!group || !point || !bn || BN_is_zero(bn.get()) != 0) return {};
  if (EC_POINT_mul(group.get(), point.get(), bn.get(), nullptr, nullptr, nullptr) != 1) return {};
  std::array<unsigned char, 65> out{};
  const std::size_t n =
      EC_POINT_point2oct(group.get(), point.get(), POINT_CONVERSION_UNCOMPRESSED, out.data(), out.size(), nullptr);
  return n == out.size() ? std::string(reinterpret_cast<const char*>(out.data()), n) : std::string();
}

// An EC key on P-256 from its public point (octets) and, optionally, its private scalar. With a scalar and no point
// the point is computed.
Pkey make_key(std::string_view point_in, std::optional<std::string_view> scalar) {
  std::string derived;
  std::string_view point = point_in;
  if (scalar && point.empty()) {
    derived = point_of_scalar(*scalar);
    point = derived;
  }
  if (point.empty()) return nullptr;
  std::unique_ptr<OSSL_PARAM_BLD, Deleter<OSSL_PARAM_BLD, OSSL_PARAM_BLD_free>> builder(OSSL_PARAM_BLD_new());
  if (!builder) return nullptr;
  Bn priv;
  OSSL_PARAM_BLD_push_utf8_string(builder.get(), OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0);
  OSSL_PARAM_BLD_push_octet_string(builder.get(), OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size());
  if (scalar) {
    priv.reset(BN_bin2bn(bytes(*scalar), static_cast<int>(scalar->size()), nullptr));
    OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_PRIV_KEY, priv.get());
  }
  std::unique_ptr<OSSL_PARAM, Deleter<OSSL_PARAM, OSSL_PARAM_free>> params(OSSL_PARAM_BLD_to_param(builder.get()));
  PkeyCtx ctx(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr));
  if (!params || !ctx || EVP_PKEY_fromdata_init(ctx.get()) != 1) return nullptr;
  EVP_PKEY* raw = nullptr;
  if (EVP_PKEY_fromdata(ctx.get(), &raw, scalar ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, params.get()) != 1) {
    return nullptr;
  }
  return Pkey(raw);
}

std::string public_octets(const EVP_PKEY* key) {
  std::array<unsigned char, 133> buffer{};
  std::size_t length = 0;
  if (EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, buffer.data(), buffer.size(), &length) != 1) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(buffer.data()), length);
}

std::string private_scalar(const EVP_PKEY* key) {
  BIGNUM* raw = nullptr;
  if (EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_PRIV_KEY, &raw) != 1) return {};
  const Bn scalar(raw);
  std::string out(32, '\0');
  BN_bn2binpad(scalar.get(), reinterpret_cast<unsigned char*>(out.data()), 32);
  return out;
}

// HKDF (RFC 5869) with SHA-256: extract and expand.
std::string hkdf(std::string_view salt, std::string_view ikm, std::string_view info, std::size_t length) {
  std::unique_ptr<EVP_KDF, Deleter<EVP_KDF, EVP_KDF_free>> kdf(EVP_KDF_fetch(nullptr, "HKDF", nullptr));
  std::unique_ptr<EVP_KDF_CTX, Deleter<EVP_KDF_CTX, EVP_KDF_CTX_free>> ctx(EVP_KDF_CTX_new(kdf.get()));
  char digest[] = "SHA256";
  OSSL_PARAM params[] = {
      OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest, 0),
      OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, const_cast<char*>(ikm.data()), ikm.size()),
      OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, const_cast<char*>(salt.data()), salt.size()),
      OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, const_cast<char*>(info.data()), info.size()),
      OSSL_PARAM_construct_end()};
  std::string out(length, '\0');
  if (EVP_KDF_derive(ctx.get(), reinterpret_cast<unsigned char*>(out.data()), length, params) != 1) return {};
  return out;
}

std::string ecdh(EVP_PKEY* mine, EVP_PKEY* theirs) {
  PkeyCtx ctx(EVP_PKEY_CTX_new(mine, nullptr));
  if (!ctx || EVP_PKEY_derive_init(ctx.get()) != 1 || EVP_PKEY_derive_set_peer(ctx.get(), theirs) != 1) return {};
  std::size_t length = 0;
  if (EVP_PKEY_derive(ctx.get(), nullptr, &length) != 1) return {};
  std::string out(length, '\0');
  if (EVP_PKEY_derive(ctx.get(), reinterpret_cast<unsigned char*>(out.data()), &length) != 1) return {};
  out.resize(length);
  return out;
}

// `OpenSSL::BN.new(bytes, 2)` drops the leading zero bytes before the gem decodes the point.
std::string strip_leading_zeros(std::string value) {
  const std::size_t zeros = value.find_first_not_of('\0');
  value.erase(0, zeros == std::string::npos ? value.size() : zeros);
  return value;
}

bool blank(const std::optional<std::string_view>& value) {
  return !value || value->empty();
}

std::expected<std::string, PushError> argument_error(std::string message) {
  return std::unexpected(PushError{PushError::Kind::Argument, std::move(message)});
}

std::string be32(std::uint32_t value) {
  std::string out(4, '\0');
  for (int i = 0; i < 4; ++i) out[static_cast<std::size_t>(i)] = static_cast<char>((value >> (24 - 8 * i)) & 0xFF);
  return out;
}

}  // namespace

std::expected<std::string, PushError> encrypt_with(std::string_view message, std::optional<std::string_view> p256dh,
                                                   std::optional<std::string_view> auth,
                                                   std::string_view server_private_key, std::string_view salt,
                                                   const Layout& layout) {
  if (message.empty()) return argument_error("message cannot be blank");
  if (blank(p256dh)) return argument_error("p256dh cannot be blank");
  if (blank(auth)) return argument_error("auth cannot be blank");
  const auto client_bytes = compat::base64::urlsafe_decode(*p256dh);
  if (!client_bytes) return argument_error("invalid base64");
  const std::string client_public = strip_leading_zeros(*client_bytes);
  const auto auth_bytes = compat::base64::urlsafe_decode(*auth);
  if (!auth_bytes) return argument_error("invalid base64");

  const Pkey client_key = make_key(client_public, std::nullopt);
  if (!client_key) return std::unexpected(PushError{PushError::Kind::InvalidKey, "invalid encoding"});
  // The public point of the server key comes from its scalar.
  const Pkey server_key = make_key({}, server_private_key);
  if (!server_key) return argument_error("invalid server key");
  const std::string server_public = public_octets(server_key.get());
  const std::string shared = ecdh(server_key.get(), client_key.get());
  if (server_public.empty() || shared.empty()) return argument_error("key agreement failed");

  const std::string info = std::string("WebPush: info\0", 14) + client_public + server_public;
  const std::string prk = hkdf(*auth_bytes, shared, info, 32);
  const std::string cek = hkdf(salt, prk, std::string("Content-Encoding: aes128gcm\0", 28), 16);
  const std::string nonce = hkdf(salt, prk, std::string("Content-Encoding: nonce\0", 24), 12);
  if (cek.empty() || nonce.empty()) return argument_error("key derivation failed");

  std::string plaintext(message);
  plaintext.append(layout.padding);
  CipherCtx cipher(EVP_CIPHER_CTX_new());
  std::string record(plaintext.size() + 16, '\0');
  int length = 0;
  int total = 0;
  if (EVP_EncryptInit_ex(cipher.get(), EVP_aes_128_gcm(), nullptr, bytes(cek), bytes(nonce)) != 1 ||
      EVP_EncryptUpdate(cipher.get(), reinterpret_cast<unsigned char*>(record.data()), &length, bytes(plaintext),
                        static_cast<int>(plaintext.size())) != 1) {
    return argument_error("encryption failed");
  }
  total = length;
  if (EVP_EncryptFinal_ex(cipher.get(), reinterpret_cast<unsigned char*>(record.data()) + total, &length) != 1 ||
      EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_GET_TAG, 16,
                          reinterpret_cast<unsigned char*>(record.data()) + plaintext.size()) != 1) {
    return argument_error("encryption failed");
  }
  if (record.size() > 4096) return argument_error("encrypted payload is too big");

  std::string out(salt);
  out += be32(layout.record_size.value_or(static_cast<std::uint32_t>(record.size())));
  out.push_back(static_cast<char>(server_public.size()));
  out += server_public;
  out += record;
  return out;
}

std::expected<std::string, PushError> encrypt(std::string_view message, std::optional<std::string_view> p256dh,
                                              std::optional<std::string_view> auth) {
  const KeyPair server = generate_key_pair();
  std::array<unsigned char, 16> salt{};
  RAND_bytes(salt.data(), static_cast<int>(salt.size()));
  return encrypt_with(message, p256dh, auth, server.private_key,
                      std::string_view(reinterpret_cast<const char*>(salt.data()), salt.size()), Layout{});
}

std::optional<Decrypted> decrypt(std::string_view body, std::string_view receiver_private_key, std::string_view auth) {
  if (body.size() < 21 + 16) return std::nullopt;
  const std::string_view salt = body.substr(0, 16);
  Decrypted out;
  for (std::size_t i = 16; i < 20; ++i) out.record_size = (out.record_size << 8) | static_cast<unsigned char>(body[i]);
  const std::size_t id_length = static_cast<unsigned char>(body[20]);
  if (body.size() < 21 + id_length + 16) return std::nullopt;
  const std::string_view server_public = body.substr(21, id_length);
  const std::string_view record = body.substr(21 + id_length);
  const Pkey receiver = make_key({}, receiver_private_key);
  const Pkey server = make_key(server_public, std::nullopt);
  if (!receiver || !server) return std::nullopt;
  const std::string shared = ecdh(receiver.get(), server.get());
  const std::string info =
      std::string("WebPush: info\0", 14) + public_octets(receiver.get()) + std::string(server_public);
  const std::string prk = hkdf(auth, shared, info, 32);
  const std::string cek = hkdf(salt, prk, std::string("Content-Encoding: aes128gcm\0", 28), 16);
  const std::string nonce = hkdf(salt, prk, std::string("Content-Encoding: nonce\0", 24), 12);
  const std::string_view cipher_text = record.substr(0, record.size() - 16);
  std::string tag(record.substr(record.size() - 16));
  CipherCtx cipher(EVP_CIPHER_CTX_new());
  out.plaintext.assign(cipher_text.size(), '\0');
  int length = 0;
  if (EVP_DecryptInit_ex(cipher.get(), EVP_aes_128_gcm(), nullptr, bytes(cek), bytes(nonce)) != 1 ||
      EVP_DecryptUpdate(cipher.get(), reinterpret_cast<unsigned char*>(out.plaintext.data()), &length,
                        bytes(cipher_text), static_cast<int>(cipher_text.size())) != 1 ||
      EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_TAG, 16, tag.data()) != 1 ||
      EVP_DecryptFinal_ex(cipher.get(), reinterpret_cast<unsigned char*>(out.plaintext.data()) + length, &length) !=
          1) {
    return std::nullopt;
  }
  return out;
}

std::string public_key_of(std::string_view private_key) {
  // The point comes from the scalar: OpenSSL computes it when the key has no public part.
  const Pkey key = make_key({}, private_key);
  return key ? public_octets(key.get()) : std::string();
}

KeyPair generate_key_pair() {
  const Pkey key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
  if (!key) return {};
  return {private_scalar(key.get()), public_octets(key.get())};
}

}  // namespace campfire::jobs::web_push

namespace campfire::jobs::web_push {

namespace {

// `JSON.generate` for a string: quotes, backslash and control characters only (no HTML or "/" escapes).
void json_string(std::string& out, std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  out.push_back('"');
  for (const char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          out += "\\u00";
          out.push_back(kHex[(static_cast<unsigned char>(ch) >> 4) & 0xF]);
          out.push_back(kHex[static_cast<unsigned char>(ch) & 0xF]);
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back('"');
}

// The bytes that `c` takes inside a JSON string: two for the short escapes, six for the other control characters.
std::size_t json_length(char32_t c) {
  switch (c) {
    case U'"':
    case U'\\':
    case U'\n':
    case U'\r':
    case U'\t':
    case U'\b':
    case U'\f': return 2;
    default: break;
  }
  if (c < 0x20) return 6;
  if (c < 0x80) return 1;
  if (c < 0x800) return 2;
  if (c < 0x10000) return 3;
  return 4;
}

// The length of the UTF-8 sequence that starts with `lead`. A stray byte counts as one.
std::size_t sequence_length(unsigned char lead) {
  if (lead < 0x80) return 1;
  if (lead >= 0xF0) return 4;
  if (lead >= 0xE0) return 3;
  if (lead >= 0xC0) return 2;
  return 1;
}

char32_t decode_at(std::string_view text, std::size_t at, std::size_t length) {
  if (length == 1 || at + length > text.size()) return static_cast<unsigned char>(text[at]);
  char32_t c = static_cast<unsigned char>(text[at]) & (0xFF >> (length + 1));
  for (std::size_t i = 1; i < length; ++i) c = (c << 6) | (static_cast<unsigned char>(text[at + i]) & 0x3F);
  return c;
}

std::string base64_nopad(std::string_view data) {
  return compat::base64::urlsafe_encode_unpadded(data);
}

}  // namespace

std::string truncate_json_string(std::string text, std::size_t max_bytes) {
  constexpr std::string_view kEllipsis = "\xE2\x80\xA6";  // U+2026
  std::size_t total = 0;
  for (std::size_t i = 0; i < text.size();) {
    const std::size_t n = sequence_length(static_cast<unsigned char>(text[i]));
    total += json_length(decode_at(text, i, n));
    i += n;
  }
  if (total <= max_bytes) return text;
  std::size_t used = kEllipsis.size();
  std::size_t cut = text.size();
  for (std::size_t i = 0; i < text.size();) {
    const std::size_t n = sequence_length(static_cast<unsigned char>(text[i]));
    used += json_length(decode_at(text, i, n));
    if (used > max_bytes) {
      cut = i;
      break;
    }
    i += n;
  }
  text.resize(cut);
  text += kEllipsis;
  return text;
}

std::string encoded_message(std::string_view title, std::string_view body, std::string_view path, std::int64_t badge) {
  std::string out = "{\"title\":";
  json_string(out, title);
  out += ",\"options\":{\"body\":";
  json_string(out, body);
  out += ",\"icon\":\"/account/logo\",\"data\":{\"path\":";
  json_string(out, path);
  out += ",\"badge\":" + std::to_string(badge) + "}}}";
  return out;
}

std::string random_uuid() {
  std::array<unsigned char, 16> b{};
  RAND_bytes(b.data(), static_cast<int>(b.size()));
  b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);
  b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < b.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
    out.push_back(kHex[b[i] >> 4]);
    out.push_back(kHex[b[i] & 0xF]);
  }
  return out;
}

struct Vapid::Impl {
  std::string subject;
  Pkey key;
  std::string public_key;
};

Vapid::Vapid(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Vapid::Vapid(Vapid&&) noexcept = default;
Vapid& Vapid::operator=(Vapid&&) noexcept = default;
Vapid::~Vapid() = default;

std::string_view to_string(VapidErrc code) noexcept {
  switch (code) {
    case VapidErrc::InvalidPublicKey: return "VAPID_PUBLIC_KEY isn't a Base64 P-256 public key";
    case VapidErrc::InvalidPrivateKey: return "VAPID_PRIVATE_KEY isn't a Base64 P-256 private key";
    case VapidErrc::Mismatched: return "VAPID_PUBLIC_KEY isn't the public key of VAPID_PRIVATE_KEY";
  }
  return "VAPID key";
}

std::expected<Vapid, VapidErrc> Vapid::create(std::string_view subject, std::string_view public_key,
                                              std::string_view private_key) {
  const auto point = compat::base64::urlsafe_decode(public_key);
  if (!point || !make_key(*point, std::nullopt)) return std::unexpected(VapidErrc::InvalidPublicKey);
  auto scalar = compat::base64::urlsafe_decode(private_key);
  if (!scalar || scalar->size() > 32) return std::unexpected(VapidErrc::InvalidPrivateKey);
  scalar->insert(0, 32 - scalar->size(), '\0');
  // The scalar must be at least 1 and less than the order of the curve.
  static constexpr std::string_view kOrder(
      "\xFF\xFF\xFF\xFF\x00\x00\x00\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xBC\xE6\xFA\xAD\xA7\x17\x9E\x84\xF3\xB9\xCA\xC2"
      "\xFC\x63\x25\x51",
      32);
  if (*scalar >= kOrder || scalar->find_first_not_of('\0') == std::string::npos) {
    return std::unexpected(VapidErrc::InvalidPrivateKey);
  }
  Pkey signing = make_key({}, *scalar);
  if (!signing) return std::unexpected(VapidErrc::InvalidPrivateKey);
  const std::string derived = public_octets(signing.get());
  const Pkey given = make_key(*point, std::nullopt);
  const std::string given_octets = public_octets(given.get());
  if (derived.empty() || derived != given_octets) return std::unexpected(VapidErrc::Mismatched);
  auto impl = std::make_unique<Impl>();
  impl->subject = std::string(subject);
  impl->key = std::move(signing);
  impl->public_key = *point;
  return Vapid(std::move(impl));
}

std::string Vapid::authorization(std::string_view audience, std::int64_t now) const {
  constexpr std::int64_t kExpirationSeconds = std::int64_t{12} * 60 * 60;
  std::string claims = "{\"aud\":";
  json_string(claims, audience);
  claims += ",\"exp\":" + std::to_string(now + kExpirationSeconds) + ",\"sub\":";
  json_string(claims, impl_->subject);
  claims += "}";
  const std::string signing_input = base64_nopad(R"({"typ":"JWT","alg":"ES256"})") + "." + base64_nopad(claims);
  // ES256: the signature is r and s as two 32-byte numbers, not DER.
  MdCtx md(EVP_MD_CTX_new());
  std::size_t der_length = 0;
  std::string der;
  if (EVP_DigestSignInit(md.get(), nullptr, EVP_sha256(), nullptr, impl_->key.get()) != 1 ||
      EVP_DigestSign(md.get(), nullptr, &der_length, bytes(signing_input), signing_input.size()) != 1) {
    return {};
  }
  der.assign(der_length, '\0');
  if (EVP_DigestSign(md.get(), reinterpret_cast<unsigned char*>(der.data()), &der_length, bytes(signing_input),
                     signing_input.size()) != 1) {
    return {};
  }
  const unsigned char* cursor = bytes(der);
  std::unique_ptr<ECDSA_SIG, Deleter<ECDSA_SIG, ECDSA_SIG_free>> sig(
      d2i_ECDSA_SIG(nullptr, &cursor, static_cast<long>(der_length)));
  if (!sig) return {};
  const BIGNUM* r = nullptr;
  const BIGNUM* s = nullptr;
  ECDSA_SIG_get0(sig.get(), &r, &s);
  std::string raw(64, '\0');
  BN_bn2binpad(r, reinterpret_cast<unsigned char*>(raw.data()), 32);
  BN_bn2binpad(s, reinterpret_cast<unsigned char*>(raw.data()) + 32, 32);
  return "vapid t=" + signing_input + "." + base64_nopad(raw) + ",k=" + base64_nopad(impl_->public_key);
}

}  // namespace campfire::jobs::web_push
