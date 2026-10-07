// Tests of the Web Push encryption and VAPID. Rust: crates/campfire/src/integrations/web_push/{encryption,tests}.rs.
#include "jobs/web_push.hpp"

#include <doctest.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

#include "compat/base64.hpp"

using namespace campfire::jobs::web_push;
namespace b64 = campfire::compat::base64;

namespace {

std::string d(std::string_view text) {
  auto out = b64::urlsafe_decode(text);
  REQUIRE(out.has_value());
  return *out;
}

// RFC 8291, section 5.
constexpr std::string_view kPlaintext = "V2hlbiBJIGdyb3cgdXAsIEkgd2FudCB0byBiZSBhIHdhdGVybWVsb24";
constexpr std::string_view kAsPrivate = "yfWPiYE-n46HLnH0KqZOF1fJJU3MYrct3AELtAQ-oRw";
constexpr std::string_view kUaPublic =
    "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4";
constexpr std::string_view kUaPrivate = "q1dXpw3UpT5VOmu_cf_v6ih07Aems3njxI-JWgLcM94";
constexpr std::string_view kSalt = "DGv6ra1nlYgDCS1FRnbzlw";
constexpr std::string_view kAuth = "BTBZMqHH6r4Tts7J_aSIgg";
constexpr std::string_view kMessage =
    "DGv6ra1nlYgDCS1FRnbzlwAAEABBBP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7"
    "A_yl95bQpu6cVPTpK4Mqgkf1CXztLVBSt2Ks3oZwbuwXPXLWyouBWLVWGNWQexSgSxsj_Qulcy4a-fN";

// The keys of the gem's vectors (`reference/config` VAPID test keys).
constexpr std::string_view kVapidPublic =
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8cTriz_qYBVicY02_VxTQ=";
constexpr std::string_view kVapidPrivate = "qfXLHghuG1rSHZUVo9SscNRI-0EIHRbIrfeGCqbAwak=";

}  // namespace

TEST_CASE("encrypts the RFC 8291 test vector") {
  Layout layout;
  layout.record_size = 4096;
  layout.padding = std::string_view("\x02", 1);
  auto body = encrypt_with(d(kPlaintext), kUaPublic, kAuth, d(kAsPrivate), d(kSalt), layout);
  REQUIRE(body.has_value());
  CHECK(b64::urlsafe_encode_unpadded(*body) == kMessage);

  auto opened = decrypt(d(kMessage), d(kUaPrivate), d(kAuth));
  REQUIRE(opened.has_value());
  CHECK(opened->record_size == 4096);
  CHECK(opened->plaintext == d(kPlaintext) + std::string("\x02", 1));
}

TEST_CASE("a message made with the framing of the gem decrypts") {
  const KeyPair receiver = generate_key_pair();
  REQUIRE(receiver.public_key.size() == 65);
  const std::string auth = "0123456789abcdef";
  const std::string message = "{\"title\":\"hi\"}";
  auto body = encrypt(message, b64::urlsafe_encode_unpadded(receiver.public_key), b64::urlsafe_encode_unpadded(auth));
  REQUIRE(body.has_value());
  auto opened = decrypt(*body, receiver.private_key, auth);
  REQUIRE(opened.has_value());
  CHECK(opened->plaintext == message + std::string("\x02\x00", 2));
  CHECK(opened->record_size == body->size() - 86);
}

TEST_CASE("the public key comes from the scalar") {
  CHECK(public_key_of(d(kUaPrivate)) == d(kUaPublic));
}

TEST_CASE("rejects what the gem rejects") {
  const KeyPair key = generate_key_pair();
  const std::string ok = b64::urlsafe_encode_unpadded(key.public_key);
  using Kind = PushError::Kind;
  CHECK(encrypt("m", std::nullopt, "YXV0aA").error().kind == Kind::Argument);
  CHECK(encrypt("m", ok, "").error().kind == Kind::Argument);
  CHECK(encrypt("", ok, "YXV0aA").error().kind == Kind::Argument);
  CHECK(encrypt("m", "not base64!", "YXV0aA").error().kind == Kind::Argument);
  CHECK(encrypt("m", "dGVzdF9rZXk", "YXV0aA").error().kind == Kind::InvalidKey);
  CHECK(encrypt(std::string(4100, 'x'), ok, "YXV0aA").error().kind == Kind::Argument);
}

TEST_CASE("the message bytes are the ones of the Rust port") {
  const std::string title = "Designers <&> \"quotes\" \xC3\xA9 \xF0\x9F\x98\x80";
  const std::string body = "Kevin: line\nbreak\ttab \xE2\x80\xA8 \x1f / \\ ";
  CHECK(encoded_message(title, body, "/rooms/1", 3) ==
        "{\"title\":\"Designers <&> \\\"quotes\\\" \xC3\xA9 \xF0\x9F\x98\x80\",\"options\":{\"body\":\"Kevin: "
        "line\\nbreak\\ttab \xE2\x80\xA8 \\u001f / \\\\ \",\"icon\":\"/account/logo\",\"data\":{\"path\":\"/rooms/1\","
        "\"badge\":3}}}");
}

TEST_CASE("the longest payload fits a record") {
  const KeyPair receiver = generate_key_pair();
  const std::string auth = "0123456789abcdef";
  const std::string title(kMaxPayloadTitleBytes / 2, '"');
  std::string body;
  for (std::size_t i = 0; i < kMaxPayloadBodyBytes / 6; ++i) body.push_back('\x01');
  const std::string message = encoded_message(title, body, "/rooms/-9223372036854775808", INT64_MIN);
  auto sent = encrypt(message, b64::urlsafe_encode_unpadded(receiver.public_key), b64::urlsafe_encode_unpadded(auth));
  REQUIRE(sent.has_value());
  CHECK(decrypt(*sent, receiver.private_key, auth)->plaintext == message + std::string("\x02\x00", 2));
}

TEST_CASE("truncates by the length in JSON") {
  CHECK(truncate_json_string("short", 5) == "short");
  CHECK(truncate_json_string("longer", 5) == "lo\xE2\x80\xA6");
  CHECK(truncate_json_string(std::string(10, '\x01'), 16) == std::string(2, '\x01') + "\xE2\x80\xA6");
  CHECK(truncate_json_string("\"\"\"", 5) == "\"\xE2\x80\xA6");
  CHECK(truncate_json_string("\xF0\x9F\x98\x80\xF0\x9F\x98\x80", 7) == "\xF0\x9F\x98\x80\xE2\x80\xA6");
}

TEST_CASE("signs the VAPID header like the gem") {
  auto vapid = Vapid::create("mailto:support@37signals.com", kVapidPublic, kVapidPrivate);
  REQUIRE(vapid.has_value());
  const std::string header = vapid->authorization("https://fcm.googleapis.com", 1700000000);
  constexpr std::string_view kPrefix = "vapid t=";
  REQUIRE(header.starts_with(kPrefix));
  const std::size_t comma = header.find(",k=");
  const std::string token = header.substr(kPrefix.size(), comma - kPrefix.size());
  CHECK(header.substr(comma + 3) ==
        "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8cTriz_qYBVicY02_VxTQ");
  const std::size_t first = token.find('.');
  const std::size_t second = token.find('.', first + 1);
  CHECK(token.substr(0, first) == "eyJ0eXAiOiJKV1QiLCJhbGciOiJFUzI1NiJ9");
  CHECK(token.substr(first + 1, second - first - 1) ==
        "eyJhdWQiOiJodHRwczovL2ZjbS5nb29nbGVhcGlzLmNvbSIsImV4cCI6MTcwMDA0MzIwMCwic3ViIjoibWFpbHRvOnN1cHBvcnRAMzdzaWduYW"
        "xzLmNvbSJ9");

  // The signature is r and s, 32 bytes each, and verifies with the public key.
  const std::string raw = d(token.substr(second + 1));
  REQUIRE(raw.size() == 64);
  ECDSA_SIG* sig = ECDSA_SIG_new();
  ECDSA_SIG_set0(sig, BN_bin2bn(reinterpret_cast<const unsigned char*>(raw.data()), 32, nullptr),
                 BN_bin2bn(reinterpret_cast<const unsigned char*>(raw.data()) + 32, 32, nullptr));
  unsigned char* der = nullptr;
  const int der_size = i2d_ECDSA_SIG(sig, &der);
  ECDSA_SIG_free(sig);
  REQUIRE(der_size > 0);
  const std::string public_key = d(kVapidPublic);
  // Verify through a key built from the public point.
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
  OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
  OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0);
  OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, public_key.data(), public_key.size());
  OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
  EVP_PKEY* key = nullptr;
  EVP_PKEY_fromdata_init(ctx);
  REQUIRE(EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params) == 1);
  EVP_MD_CTX* md = EVP_MD_CTX_new();
  REQUIRE(EVP_DigestVerifyInit(md, nullptr, EVP_sha256(), nullptr, key) == 1);
  const std::string signed_text = token.substr(0, second);
  CHECK(EVP_DigestVerify(md, der, static_cast<std::size_t>(der_size),
                         reinterpret_cast<const unsigned char*>(signed_text.data()), signed_text.size()) == 1);
  EVP_MD_CTX_free(md);
  EVP_PKEY_free(key);
  OSSL_PARAM_free(params);
  OSSL_PARAM_BLD_free(bld);
  EVP_PKEY_CTX_free(ctx);
  OPENSSL_free(der);
}

TEST_CASE("rejects bad VAPID keys up front") {
  const auto keys = [](std::string_view pub, std::string_view priv) {
    return Vapid::create("mailto:a@b.c", pub, priv);
  };
  CHECK(keys("", kVapidPrivate).error() == VapidErrc::InvalidPublicKey);
  CHECK(keys("dGVzdF9rZXk", kVapidPrivate).error() == VapidErrc::InvalidPublicKey);
  CHECK(keys(kVapidPublic, "not base64!").error() == VapidErrc::InvalidPrivateKey);
  CHECK(keys(kVapidPublic, b64::urlsafe_encode_unpadded(std::string(32, '\xff'))).error() ==
        VapidErrc::InvalidPrivateKey);
  const KeyPair other = generate_key_pair();
  CHECK(keys(b64::urlsafe_encode_unpadded(other.public_key), kVapidPrivate).error() == VapidErrc::Mismatched);
  CHECK(keys("BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8cTriz_qYBVicY02_VxTQ",
             "qfXLHghuG1rSHZUVo9SscNRI-0EIHRbIrfeGCqbAwak")
            .has_value());
}

// A message that the gem itself encrypted (the capture of the Rust port tests).
TEST_CASE("decrypts what the gem encrypted") {
  constexpr std::string_view kCiphertext =
      "aDE1uz32M2tQ2YYPv8YaFgAAALhBBFV0pWPjdGKwrY1oz2-NmLXT5SR0UqYQ03iA6_A8Y31sv1ypadCShWimwnH_k6x4pD3AEDyG"
      "2_dw_9St5Q6lcifhHkDdS_zMiD9jb3UQ2Fhzj8ruG1JMPaT6hhcbqokvY9mqIgQLqcTIWnDqjzYHt8Cw9l-pQzTsWan6M6njekee"
      "DAdf09ietcPM2AU4kSJh59AD3sCJfxW8s_fSQYGUbbw7TitE1FfklTZ9fGxwME3KBDqqhIoCJr4W9F22sk22eMtmQwMN90xo2nP0"
      "Sb59Yurptd3_erpzfKknE5iAYpQeID4IF1qXYQ6O-G0jg8scfCtzxtaXqlT4";
  const std::string body = d(kCiphertext);
  auto opened = decrypt(body, d("Y1H_BKvKi4Ht2cE4HZ8AgD0nn8chE3zhgBtEODTn4I4="), d("BCuqPYqblNIL9NEJ4WT1Yw=="));
  REQUIRE(opened.has_value());
  CHECK(opened->record_size == body.size() - 86);
  const std::string text = "Designers <&> \"quotes\" \xC3\xA9 \xF0\x9F\x98\x80";
  CHECK(opened->plaintext == encoded_message(text, "Kevin: line\nbreak\ttab \xE2\x80\xA8 \x1f / \\ ", "/rooms/1", 3) +
                                 std::string("\x02\x00", 2));
}
