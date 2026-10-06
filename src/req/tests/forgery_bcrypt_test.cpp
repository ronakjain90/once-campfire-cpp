// Tests of forgery protection and bcrypt. The vectors are the csrf and passwords groups of
// spec/vectors/rails_compat.json, made by the Rails reference.
#include <doctest.h>

#include <string>

#include "req/bcrypt.hpp"
#include "req/forgery.hpp"
#include "vectors.hpp"

using namespace testing_support;
using namespace campfire::req;

namespace {
const json::Value& csrf() { return at(load_vectors("rails_compat.json"), "csrf"); }
const json::Value& passwords() { return at(load_vectors("rails_compat.json"), "passwords"); }

std::optional<std::string_view> opt(const json::Value& v) { return opt_str(v); }
}  // namespace

TEST_CASE("csrf vectors: masked, per-form and unmasked tokens") {
  const std::string session(at(csrf(), "session_token").as_string());
  std::size_t count = 0;
  for (const auto& c : items(at(csrf(), "validity"))) {
    const bool expected = at(c, "expected").as_bool();
    CAPTURE(at(c, "case").as_string());
    CAPTURE(at(c, "token").as_string());
    CAPTURE(at(c, "path").as_string());
    CHECK(is_valid_authenticity_token(session, at(c, "token").as_string(), at(c, "path").as_string(),
                                      at(c, "method").as_string()) == expected);
    ++count;
  }
  CHECK(count == 189);
}

TEST_CASE("csrf vectors: the Origin check") {
  std::size_t count = 0;
  for (const auto& c : items(at(csrf(), "origin"))) {
    ForgeryInput in;
    in.method = "POST";
    in.origin = opt(at(c, "origin"));
    in.sec_fetch_site = "same-origin";
    in.base_url = at(c, "base_url").as_string();
    const auto result = verify_authenticity_token(in);
    const auto& expected = at(c, "expected");
    CAPTURE(in.origin.value_or("(none)"));
    if (expected.is_bool()) {
      CHECK(result.has_value() != expected.as_bool());
    } else {
      // "raises": the null origin is an error with its own message.
      REQUIRE(result.has_value());
      CHECK(result->message == "The browser returned a 'null' origin");
    }
    ++count;
  }
  CHECK(count == 8);
}

TEST_CASE("forgery: Sec-Fetch-Site rules") {
  ForgeryInput in;
  in.method = "POST";
  in.base_url = "https://campfire.test";
  in.ssl = true;
  for (const char* ok : {"same-origin", "same-site"}) {
    in.sec_fetch_site = ok;
    CHECK_FALSE(verify_authenticity_token(in).has_value());
  }
  in.sec_fetch_site = "cross-site";
  CHECK(verify_authenticity_token(in)->message == "Sec-Fetch-Site header (cross-site) indicates a cross-site request");
  in.sec_fetch_site = "none";
  CHECK(verify_authenticity_token(in).has_value());
  in.sec_fetch_site = std::nullopt;
  CHECK(verify_authenticity_token(in).has_value());   // missing over HTTPS
  in.ssl = false;
  CHECK_FALSE(verify_authenticity_token(in).has_value());  // missing over plain HTTP
  in.force_ssl = true;
  CHECK(verify_authenticity_token(in).has_value());
  in.force_ssl = false;
  in.method = "GET";
  in.sec_fetch_site = "cross-site";
  CHECK_FALSE(verify_authenticity_token(in).has_value());
  in.method = "POST";
  in.exempt = true;
  CHECK_FALSE(verify_authenticity_token(in).has_value());  // the bot key exemption
}

TEST_CASE("forgery: a cross-origin script embed") {
  CHECK(is_cross_origin_javascript(true, true, false));
  CHECK_FALSE(is_cross_origin_javascript(true, true, true));
  CHECK_FALSE(is_cross_origin_javascript(false, true, false));
}

TEST_CASE("password vectors: digests, checks") {
  const auto& p = passwords();
  std::size_t checks = 0;
  for (const auto& d : items(at(p, "digests"))) {
    CHECK(bcrypt::verify_password(at(d, "password").as_string(), at(d, "digest").as_string()));
    ++checks;
  }
  for (const auto& c : items(at(p, "checks"))) {
    CAPTURE(at(c, "password").as_string());
    CHECK(bcrypt::verify_password(at(c, "password").as_string(), at(c, "digest").as_string()) ==
          at(c, "expected").as_bool());
    ++checks;
  }
  MESSAGE("password checks: " << checks);
  CHECK(checks > 20);
}

TEST_CASE("bcrypt: create and verify at the minimum cost") {
  const auto digest = bcrypt::hash_password("secret123456", bcrypt::kMinCost);
  REQUIRE(digest.size() == 60);
  CHECK(digest.starts_with("$2a$04$"));
  CHECK(bcrypt::verify_password("secret123456", digest));
  CHECK_FALSE(bcrypt::verify_password("secret12345", digest));
  CHECK(bcrypt::hash_password("secret123456", bcrypt::kMinCost) != digest);  // a random salt
  CHECK(bcrypt::hash_password("x", 3).empty());
  CHECK_FALSE(bcrypt::verify_password("x", "not a digest"));
  // Only 72 bytes count.
  const std::string long_password(72, 'a');
  const auto long_digest = bcrypt::hash_password(long_password + "tail", bcrypt::kMinCost);
  CHECK(bcrypt::verify_password(long_password, long_digest));
}

TEST_CASE("bcrypt: create at the default cost matches the vector cost") {
  const auto digest = bcrypt::hash_password("secret123456");
  REQUIRE(digest.size() == 60);
  CHECK(digest.starts_with("$2a$12$"));
  CHECK(bcrypt::verify_password("secret123456", digest));
}
