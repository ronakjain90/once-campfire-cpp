// Golden vectors vectors/ruby_core.json against the Ruby string functions.
#include <bit>
#include <cmath>
#include <cstdlib>

#include "compat/cookies.hpp"
#include "compat/ruby.hpp"
#include "vectors.hpp"

using namespace testing_support;
namespace compat = campfire::compat;

namespace {

// Group of the "strings" cases: `fn` maps the input to what Ruby gave for `field`.
template <typename Fn>
void string_group(const char* field, Fn fn) {
  const auto& vectors = load_vectors("ruby_core.json");
  Group group("ruby_core.json", std::string("strings.") + field);
  for (const auto& c : items(at(vectors, "strings"))) {
    const std::string& input = at(c, "input").as_string();
    group.check(fn(input, at(c, field)), field + std::string(" of ") + json::generate(json::Value(input)));
  }
  group.finish();
}

std::optional<int64_t> parse_i64(const std::string& s) {
  char* end = nullptr;
  errno = 0;
  long long v = std::strtoll(s.c_str(), &end, 10);
  if (errno != 0 || *end != '\0' || s.empty()) return std::nullopt;
  return v;
}

}  // namespace

TEST_CASE("ruby_core to_i") {
  string_group("to_i", [](const std::string& in, const json::Value& ruby) {
    const std::string& text = ruby.as_string();
    auto checked = parse_i64(text);
    int64_t saturated = checked ? *checked : (text.starts_with('-') ? INT64_MIN : INT64_MAX);
    return compat::to_i(in) == saturated && compat::to_i_checked(in) == checked;
  });
}

TEST_CASE("ruby_core integer_cast") {
  string_group("integer_cast", [](const std::string& in, const json::Value& ruby) {
    std::optional<int64_t> expected;
    if (ruby.is_string()) expected = parse_i64(ruby.as_string());  // "ActiveModel::RangeError" gives none
    return compat::integer_cast(in) == expected;
  });
}

TEST_CASE("ruby_core to_f") {
  string_group("to_f", [](const std::string& in, const json::Value& ruby) {
    double expected = std::strtod(ruby.as_string().c_str(), nullptr);
    return std::bit_cast<uint64_t>(compat::to_f(in)) == std::bit_cast<uint64_t>(expected);
  });
}

TEST_CASE("ruby_core strip") {
  string_group("strip", [](const std::string& in, const json::Value& r) { return compat::strip(in) == r.as_string(); });
}
TEST_CASE("ruby_core html_escape") {
  string_group("html_escape",
               [](const std::string& in, const json::Value& r) { return compat::html_escape(in) == r.as_string(); });
}
TEST_CASE("ruby_core cgi_escape") {
  string_group("cgi_escape",
               [](const std::string& in, const json::Value& r) { return compat::cgi_escape(in) == r.as_string(); });
}
TEST_CASE("ruby_core url_encode") {
  string_group("url_encode",
               [](const std::string& in, const json::Value& r) { return compat::url_encode(in) == r.as_string(); });
}
TEST_CASE("ruby_core addressable_unreserved") {
  string_group("addressable_unreserved",
               [](const std::string& in, const json::Value& r) { return compat::url_encode(in) == r.as_string(); });
}
TEST_CASE("ruby_core rack_escape (cookie escape)") {
  string_group("rack_escape", [](const std::string& in, const json::Value& r) {
    return compat::cookies::escape(in) == r.as_string();
  });
}

TEST_CASE("ruby_core floats") {
  const auto& vectors = load_vectors("ruby_core.json");
  Group group("ruby_core.json", "floats");
  for (const auto& c : items(at(vectors, "floats"))) {
    const std::string& bits = at(c, "bits").as_string();
    double f = std::bit_cast<double>(std::strtoull(bits.c_str(), nullptr, 16));
    std::string got = compat::float_to_s(f);
    group.check(got == at(c, "to_s").as_string(), bits + " got " + got + " want " + at(c, "to_s").as_string());
  }
  group.finish();
}

TEST_CASE("ruby_core byte_ranges") {
  const auto& vectors = load_vectors("ruby_core.json");
  Group group("ruby_core.json", "byte_ranges");
  for (const auto& c : items(at(vectors, "byte_ranges"))) {
    uint64_t size = uint64_t(*at(c, "size").to_int64());
    auto got = compat::byte_ranges(opt_str(at(c, "header")), size);
    const auto& want = at(c, "ranges");
    bool ok;
    if (want.is_null()) {
      ok = !got.has_value();
    } else {
      ok = got.has_value() && got->size() == want.as_array().size();
      for (size_t i = 0; ok && i < got->size(); ++i) {
        const auto& r = want.as_array()[i].as_array();
        ok = (*got)[i].first == uint64_t(*r[0].to_int64()) && (*got)[i].last == uint64_t(*r[1].to_int64());
      }
    }
    group.check(ok, json::generate(at(c, "header")) + " of " + std::to_string(size));
  }
  group.finish();
}
