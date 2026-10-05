// Golden vectors vectors/rails_compat.json and vectors/campfire_sessions.json.
#include "compat/content_disposition.hpp"
#include "compat/cookies.hpp"
#include "compat/global_id.hpp"
#include "compat/signed_id.hpp"
#include "compat/turbo.hpp"
#include "vectors.hpp"

using namespace testing_support;
namespace compat = campfire::compat;
namespace gid = campfire::compat::global_id;

namespace {

const json::Value& rails() { return load_vectors("rails_compat.json"); }
const json::Value& section(const char* a, const char* b = nullptr) {
  const json::Value& v = at(rails(), a);
  return b ? at(v, b) : v;
}
const compat::Secrets& secrets() {
  static const compat::Secrets s(at(rails(), "secret_key_base").as_string());
  return s;
}
compat::Timestamp now() { return time_of(at(rails(), "now")); }

// A vector's "expected" for a possibly missing result.
bool same(const std::optional<json::Value>& got, const json::Value& want) { return (got ? *got : json::Value()) == want; }

std::string label(const json::Value& c) { return at(c, "case").is_string() ? at(c, "case").as_string() : "?"; }

}  // namespace

TEST_CASE("rails_compat key_generator") {
  Group g("rails_compat.json", "key_generator");
  for (const auto& c : items(section("key_generator"))) {
    auto key = secrets().generate_key(at(c, "salt").as_string(), size_t(*at(c, "length").to_int64()));
    g.check(campfire::compat::crypto::hex_encode(key) == at(c, "key_hex").as_string(), at(c, "salt").as_string());
  }
  g.finish();
}

TEST_CASE("rails_compat cookie_escaping") {
  Group g("rails_compat.json", "cookie_escaping");
  for (const auto& c : items(section("cookie_escaping"))) {
    bool ok = compat::cookies::unescape(at(c, "wire").as_string()) == at(c, "parsed").as_string();
    if (at(c, "raw").is_string()) ok = ok && compat::cookies::escape(at(c, "raw").as_string()) == at(c, "wire").as_string();
    g.check(ok, at(c, "wire").as_string());
  }
  g.finish();
}

TEST_CASE("rails_compat signed_cookies.generate") {
  Group g("rails_compat.json", "signed_cookies.generate");
  for (const auto& c : items(section("signed_cookies", "generate"))) {
    std::string raw = compat::cookies::sign(secrets(), at(c, "name").as_string(), at(c, "value").as_string(),
                                            opt_time(at(c, "expires_at")));
    std::string set_cookie = at(c, "set_cookie").as_string();
    std::string wire = set_cookie.substr(set_cookie.find('=') + 1, set_cookie.find(';') - set_cookie.find('=') - 1);
    g.check(raw == at(c, "raw").as_string() && compat::cookies::escape(raw) == wire, at(c, "value").as_string());
  }
  g.finish();
}

TEST_CASE("rails_compat permanent cookies expire in twenty years") {
  const auto& first = items(section("signed_cookies", "generate"))[0];
  CHECK(compat::permanent_expires_at(now()) == time_of(at(first, "expires_at")));
}

TEST_CASE("rails_compat signed_cookies.verify") {
  Group g("rails_compat.json", "signed_cookies.verify");
  for (const auto& c : items(section("signed_cookies", "verify"))) {
    auto value = compat::cookies::verify_signed_value(secrets(), at(c, "name").as_string(), at(c, "raw").as_string(),
                                                      time_of(at(c, "now")));
    auto str = compat::cookies::verify_signed(secrets(), at(c, "name").as_string(), at(c, "raw").as_string(),
                                              time_of(at(c, "now")));
    bool ok = same(value, at(c, "expected")) &&
              (str ? json::Value(*str) : json::Value()) == (at(c, "expected").is_string() ? at(c, "expected") : json::Value());
    g.check(ok, label(c));
  }
  g.finish();
}

TEST_CASE("rails_compat encrypted_cookies.verify") {
  Group g("rails_compat.json", "encrypted_cookies.verify");
  for (const auto& c : items(section("encrypted_cookies", "verify"))) {
    auto value = compat::cookies::decrypt(secrets(), at(c, "name").as_string(), at(c, "raw").as_string(),
                                          time_of(at(c, "now")));
    g.check(same(value, at(c, "expected")), label(c));
  }
  g.finish();
}

TEST_CASE("rails_compat encrypted_cookies.generate") {
  Group g("rails_compat.json", "encrypted_cookies.generate");
  const auto& encryptor = secrets().encrypted_cookie_encryptor();
  for (const auto& c : items(section("encrypted_cookies", "generate"))) {
    auto plaintext = encryptor.decrypt(at(c, "raw").as_string());
    // Our plaintext is byte-identical to Rails'.
    std::string ours = compat::serialize_with_metadata(
        compat::Serializer::null(), json::Value(json::encode(at(c, "value"))),
        "cookie." + at(c, "name").as_string(), opt_time(at(c, "expires_at")));
    std::string raw = compat::cookies::encrypt(secrets(), at(c, "name").as_string(), at(c, "value"),
                                               opt_time(at(c, "expires_at")));
    auto back = compat::cookies::decrypt(secrets(), at(c, "name").as_string(), raw, now());
    g.check(plaintext && *plaintext == at(c, "plaintext").as_string() && ours == at(c, "plaintext").as_string() &&
                back && *back == at(c, "value") && raw != at(c, "raw").as_string(),
            at(c, "name").as_string());
  }
  g.finish();
}

TEST_CASE("rails_compat session") {
  Group g("rails_compat.json", "session");
  const auto& s = section("session");
  g.check(*at(s, "post_with_form_token_status").to_int64() == 302, "form token status");
  auto hash = compat::cookies::decrypt(secrets(), "_campfire_session", at(s, "session_cookie_raw").as_string(), now());
  g.check(hash && *hash == at(s, "session"), "session cookie decrypts");
  const std::string& token = at(s, "session_token_value").as_string();
  g.check(compat::cookies::verify_signed(secrets(), "session_token", at(s, "session_token_raw").as_string(), now()) == token,
          "session_token verifies");
  g.check(compat::cookies::sign(secrets(), "session_token", token, compat::permanent_expires_at(now())) ==
              at(s, "session_token_raw").as_string(),
          "session_token signs");
  auto after = compat::cookies::decrypt(secrets(), "_campfire_session", at(s, "session_after_login_raw").as_string(), now());
  g.check(after && *after == at(s, "session_after_login"), "session after login");
  g.finish();
}

TEST_CASE("rails_compat signed_ids.generate") {
  Group g("rails_compat.json", "signed_ids.generate");
  for (const auto& c : items(section("signed_ids", "generate"))) {
    std::string got = compat::signed_id::generate(secrets(), at(c, "model").as_string(), *at(c, "id").to_int64(),
                                                  opt_str(at(c, "purpose")), opt_time(at(c, "expires_at")));
    g.check(got == at(c, "signed_id").as_string(), at(c, "model").as_string());
  }
  g.finish();
}

TEST_CASE("rails_compat signed_ids.verify") {
  Group g("rails_compat.json", "signed_ids.verify");
  for (const auto& c : items(section("signed_ids", "verify"))) {
    auto id = compat::signed_id::verify(secrets(), at(c, "model").as_string(), at(c, "signed_id").as_string(),
                                        opt_str(at(c, "purpose")), time_of(at(c, "now")));
    std::optional<int64_t> want;
    if (at(c, "expected").is_string()) want = std::stoll(at(c, "expected").as_string());
    else want = at(c, "expected").to_int64();
    g.check(id == want, label(c));
  }
  g.finish();
}

TEST_CASE("rails_compat global_ids") {
  Group g("rails_compat.json", "global_ids");
  for (const auto& c : items(section("global_ids"))) {
    auto parsed = gid::GlobalId::parse(at(c, "gid").as_string());
    bool ok = parsed && *parsed == gid::GlobalId::make(at(c, "model_name").as_string(), at(c, "id").as_string()) &&
              parsed->to_string() == at(c, "gid").as_string() && parsed->to_param() == at(c, "param").as_string() &&
              gid::GlobalId::from_param(at(c, "param").as_string()) == parsed;
    g.check(ok, at(c, "gid").as_string());
  }
  g.finish();
}

TEST_CASE("rails_compat sgids.generate") {
  Group g("rails_compat.json", "sgids.generate");
  CHECK(at(section("sgids"), "app").as_string() == gid::kApp);
  for (const auto& c : items(section("sgids", "generate"))) {
    auto parsed = gid::GlobalId::parse(at(c, "gid").as_string());
    const std::string& data = at(c, "data").as_string();
    std::string got = data.ends_with("?expires_in")
                          ? gid::attachable_sgid(secrets(), *parsed)
                          : gid::sgid(secrets(), *parsed, at(c, "purpose").as_string(), opt_time(at(c, "expires_at")));
    g.check(got == at(c, "sgid").as_string(), data);
  }
  g.finish();
}

TEST_CASE("rails_compat sgids.verify") {
  Group g("rails_compat.json", "sgids.verify");
  for (const auto& c : items(section("sgids", "verify"))) {
    auto got = gid::locate_signed(secrets(), at(c, "sgid").as_string(), at(c, "purpose").as_string(), time_of(at(c, "now")));
    std::optional<gid::GlobalId> want;
    if (at(c, "expected").is_string()) want = gid::GlobalId::parse(at(c, "expected").as_string());
    g.check(got == want, label(c));
  }
  g.finish();
}

TEST_CASE("rails_compat unverified_sgids (User-only fallback)") {
  Group g("rails_compat.json", "unverified_sgids");
  for (const auto& c : items(section("unverified_sgids"))) {
    auto got = gid::gid_from_unverified_sgid(opt_str(at(c, "sgid")));
    const auto& want = at(c, "expected");
    bool ok;
    if (want.is_object()) {
      ok = !got.has_value();  // Rails raises
    } else {
      // The reference's records: users 1 and 2 exist. The caller accepts only the model User.
      std::optional<std::string> found;
      if (got && *got && (*got)->model_name == "User" && ((*got)->id == "1" || (*got)->id == "2")) {
        found = "gid://campfire/User/" + (*got)->id;
      }
      ok = got.has_value() && (want.is_null() ? !found : found == want.as_string());
    }
    g.check(ok, label(c));
  }
  g.finish();
}

TEST_CASE("forged SGIDs are rejected for every model") {
  compat::Secrets attacker("attacker");
  for (const char* model : {"User", "Rooms::Open", "Account", "Message", "Rooms::Direct", "Session"}) {
    auto g = gid::GlobalId::make(model, "1");
    std::string forged = gid::sgid(attacker, g, "attachable", std::nullopt);
    CHECK_FALSE(gid::locate_signed(secrets(), forged, "attachable", now()).has_value());
  }
}

TEST_CASE("rails_compat turbo_stream_names") {
  Group g("rails_compat.json", "turbo_stream_names");
  for (const auto& c : items(section("turbo_stream_names", "generate"))) {
    std::vector<std::string> parts;
    for (const auto& p : items(at(c, "parts"))) parts.push_back(p.as_string());
    std::vector<std::string_view> views(parts.begin(), parts.end());
    g.check(compat::turbo::signed_stream_name(secrets(), views) == at(c, "signed").as_string(), at(c, "stream_name").as_string());
  }
  for (const auto& c : items(section("turbo_stream_names", "verify"))) {
    auto got = compat::turbo::verified_stream_name(secrets(), at(c, "signed").as_string());
    const auto& want = at(c, "expected");
    std::optional<std::string> expected;
    if (want.is_string()) expected = want.as_string();
    else if (want.is_number()) expected = json::generate(want);
    g.check(got == expected, label(c));
  }
  g.finish();
}

TEST_CASE("rails_compat app_verifiers") {
  Group g("rails_compat.json", "app_verifiers");
  for (const auto& c : items(section("app_verifiers", "generate"))) {
    auto verifier = secrets().app_verifier(at(c, "name").as_string());
    g.check(verifier.generate_raw(at(c, "data_json").as_string(), opt_str(at(c, "purpose")), opt_time(at(c, "expires_at"))) ==
                at(c, "message").as_string(),
            at(c, "data_json").as_string());
  }
  for (const auto& c : items(section("app_verifiers", "verify"))) {
    auto verifier = secrets().app_verifier(at(c, "name").as_string());
    auto got = verifier.verify_raw(at(c, "message").as_string(), opt_str(at(c, "purpose")), time_of(at(c, "now")));
    g.check((got ? std::optional<std::string>(*got) : std::nullopt) ==
                (at(c, "expected_json").is_string() ? std::optional<std::string>(at(c, "expected_json").as_string()) : std::nullopt),
            label(c));
  }
  g.finish();
}

TEST_CASE("campfire_sessions") {
  const auto& v = load_vectors("campfire_sessions.json");
  compat::Timestamp when = *compat::parse_iso8601("2026-09-26T12:00:00Z");
  Group g("campfire_sessions.json", "sessions, blobs, forged");
  for (const auto& s : items(at(v, "sessions"))) {
    g.check(compat::cookies::verify_signed(secrets(), "session_token", at(s, "cookie_value").as_string(), when) ==
                at(s, "token").as_string(),
            "session cookie");
    std::string header = at(s, "cookie_header").as_string();
    g.check(compat::cookies::unescape(header.substr(header.find('=') + 1)) == at(s, "cookie_value").as_string(), "cookie header");
  }
  for (const auto& b : items(at(v, "blobs"))) {
    int64_t id = *at(b, "blob_id").to_int64();
    g.check(compat::signed_id::blob_signed_id(secrets(), id) == at(b, "signed_id").as_string(), "blob signed id");
    g.check(compat::signed_id::verify_blob_signed_id(secrets(), at(b, "signed_id").as_string(), when) == id, "blob verify");
  }
  const auto& forged = at(v, "forged");
  // The forged cookie carries a valid signature over a token that is in no session row, so
  // the cookie reads as that token and the database lookup rejects it.
  g.check(compat::cookies::verify_signed(secrets(), "session_token", at(forged, "cookie_value").as_string(), when) ==
              "not-a-session-token",
          "forged cookie reads as unknown token");
  g.check(!compat::cookies::verify_signed(compat::Secrets(at(rails(), "rotated_secret_key_base").as_string()), "session_token",
                                          at(forged, "cookie_value").as_string(), when),
          "forged cookie rejected under another secret");
  g.finish();
}
