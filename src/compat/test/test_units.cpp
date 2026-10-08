// Unit tests for edge cases the Rust crates test beside their code (not vector based).
#include <cmath>
#include <random>

#include "compat/base64.hpp"
#include "compat/cookies.hpp"
#include "compat/global_id.hpp"
#include "compat/marshal.hpp"
#include "compat/message_encryptor.hpp"
#include "compat/ruby.hpp"
#include "compat/signed_id.hpp"
#include "vectors.hpp"

namespace compat = campfire::compat;
namespace json = campfire::compat::json;

TEST_CASE("json: encode is generate and then escape_html_entities, in one pass") {
  std::mt19937 rng(11);
  const std::string alphabet = std::string("<>&\"\\\n\t\x01 az09/") + "\xc3\xa9" + "\xff" + "\xe2\x80\xa8";
  const auto text = [&] {
    std::string out;
    const std::size_t n = rng() % 40;
    for (std::size_t i = 0; i < n; ++i) out.push_back(alphabet[rng() % alphabet.size()]);
    return out;
  };
  for (int round = 0; round < 300; ++round) {
    json::Value::Object object;
    for (int k = 0; k < 4; ++k) object.emplace_back(text(), json::Value(text()));
    json::Value::Array array;
    array.emplace_back(json::Value(text()));
    array.emplace_back(json::Value(static_cast<std::int64_t>(rng() % 1000)));
    array.emplace_back(json::Value(nullptr));
    object.emplace_back("list", json::Value(std::move(array)));
    const json::Value value(std::move(object));
    CHECK(json::encode(value) == json::escape_html_entities(json::generate(value)));
  }
}

TEST_CASE("json: escapes and floats") {
  CHECK(json::encode(json::Value("<a href=\"x\">&'\xE2\x80\xA8\xC3\xA9\n\t\x01\x7f/</a>")) ==
        "\"\\u003ca href=\\\"x\\\"\\u003e\\u0026'\xE2\x80\xA8\xC3\xA9\\n\\t\\u0001\x7f/\\u003c/a\\u003e\"");
  CHECK(json::generate(json::Value("<&>")) == "\"<&>\"");
  struct {
    double f;
    const char* s;
  } floats[] = {{320.0, "320.0"},
                {65.84, "65.84"},
                {-2.5, "-2.5"},
                {0.0, "0.0"},
                {-0.0, "-0.0"},
                {0.1, "0.1"},
                {0.0001, "0.0001"},
                {0.00001, "0.00001"},
                {1.25e-5, "0.0000125"},
                {1.5e-7, "0.00000015"},
                {1e-7, "0.0000001"},
                {1.2e-9, "0.0000000012"},
                {1.23456789012e-8, "0.0000000123456789012"},
                {1e-10, "1e-10"},
                {5e-324, "5e-324"},
                {1e14, "100000000000000.0"},
                {123456789012345.6, "123456789012345.6"},
                {1e15, "1e+15"},
                {-1e15, "-1e+15"},
                {1.5e15, "1.5e+15"},
                {1234567890123456.0, "1.234567890123456e+15"},
                {9007199254740992.0, "9.007199254740992e+15"},
                {1e16, "1e+16"},
                {12345678901234567.0, "1.2345678901234568e+16"},
                {1e21, "1e+21"},
                {1e100, "1e+100"},
                {1.7976931348623157e308, "1.7976931348623157e+308"}};
  for (auto& [f, s] : floats) CHECK(json::generate(json::Value(f)) == s);
  CHECK(json::generate(json::Value(std::nan(""))) == "null");
  auto parsed = json::parse(R"({"z":1,"a":[true,null,2.0,1e16,1e-5],"s":"<a & b>"})");
  REQUIRE(parsed);
  CHECK(json::encode(*parsed) == R"({"z":1,"a":[true,null,2.0,1e+16,0.00001],"s":"\u003ca \u0026 b\u003e"})");
  CHECK_FALSE(json::parse("{\"a\":1,}").has_value());
  CHECK_FALSE(json::parse("{\"a\":1/*c*/}").has_value());
  CHECK(json::parse("{\"a\":1/*c*/}", {.allow_comments = true}).has_value());
  CHECK_FALSE(json::parse("\"\\ud800\"").has_value());
}

TEST_CASE("ruby: Float#to_s ties and to_f edge cases") {
  CHECK(compat::float_to_s(667020902720176.0 + 0.25) == "667020902720176.2");
  CHECK(compat::float_to_s(667020902720176.0 + 0.75) == "667020902720176.8");
  CHECK(compat::float_to_s(std::ldexp(1.0, -24)) == "5.960464477539063e-08");
  CHECK(compat::float_to_s(1e15) == "1.0e+15");
  CHECK(compat::float_to_s(1000000000000000.1) == "1000000000000000.1");
  CHECK(compat::to_f("1_000.5") == 1000.5);
  CHECK(compat::to_f("-0x1A") == -26.0);
  CHECK(compat::to_f("0x1A") == 0.0);
  CHECK(compat::to_f("1e400") == HUGE_VAL);
  CHECK(compat::to_f(std::string("1") + std::string(70, '0') + "x") == 1e59);
}

TEST_CASE("base64: strict and urlsafe decode") {
  using namespace compat::base64;
  CHECK(urlsafe_decode("QQ") == std::optional<std::string>("A"));
  CHECK(urlsafe_decode("QQ==") == std::optional<std::string>("A"));
  CHECK_FALSE(urlsafe_decode("QQ=").has_value());
  CHECK_FALSE(urlsafe_decode("QR").has_value());
  CHECK(urlsafe_decode("a+b/") == urlsafe_decode("a-b_"));
}

TEST_CASE("marshal: load string and dump") {
  CHECK(compat::marshal::load_string(std::string_view("\x04\x08I\"\x1agid://campfire/User/1\x06:\x06"
                                                      "ET",
                                                      31)) == std::optional<std::string>("gid://campfire/User/1"));
  CHECK_FALSE(compat::marshal::load_string("\x04\x08i\x06").has_value());
}

TEST_CASE("signed ids: purposes and expiry") {
  using compat::signed_id::combine_purposes;
  CHECK(combine_purposes("User", "avatar") == "user/avatar");
  CHECK(combine_purposes("User", std::nullopt) == "user");
  CHECK(combine_purposes("Rooms::Open", "") == "rooms/open");
  CHECK(combine_purposes("HTTPRequest", "x") == "http_request/x");
}

TEST_CASE("encryptor: round trip, wrong purpose, tampering") {
  compat::MessageEncryptor enc(std::string(32, '\7'), compat::Serializer::null());
  compat::Timestamp now{};
  std::string message = enc.encrypt_and_sign(json::Value("hi"), "p", std::nullopt);
  CHECK(enc.decrypt_and_verify(message, "p", now) == json::Value("hi"));
  CHECK(enc.decrypt_and_verify(message, "q", now).error() == compat::Error::PurposeMismatch);
  std::string tampered = message;
  tampered[0] = tampered[0] == 'A' ? 'B' : 'A';
  CHECK_FALSE(enc.decrypt_and_verify(tampered, "p", now).has_value());
  CHECK(enc.decrypt_and_verify("short", std::nullopt, now).error() == compat::Error::InvalidSignature);
}

TEST_CASE("permanent_expires_at clamps 29 February") {
  auto leap = *compat::parse_iso8601("2080-02-29T00:00:00Z");
  CHECK(compat::iso8601_millis(compat::permanent_expires_at(leap)) == "2100-02-28T00:00:00.000Z");
}
