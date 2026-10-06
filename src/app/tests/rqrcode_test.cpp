// Tests of the QR code port against the vectors that the Rust port made with the rqrcode gem (spec/vectors/rqrcode.json).
#include <doctest.h>

#include <fstream>
#include <sstream>

#include "app/rqrcode.hpp"
#include "compat/base64.hpp"
#include "compat/json.hpp"

namespace campfire::app {

TEST_CASE("rqrcode: modules and SVG equal the gem") {
  std::ifstream file(std::string(CAMPFIRE_SPEC_DIR) + "/vectors/rqrcode.json");
  REQUIRE(file.good());
  std::stringstream text;
  text << file.rdbuf();
  const auto parsed = compat::json::parse(text.str());
  REQUIRE(parsed);
  REQUIRE(parsed->is_array());
  CHECK(parsed->as_array().size() >= 30);
  for (const compat::json::Value& vector : parsed->as_array()) {
    const std::string input = *compat::base64::strict_decode(*vector.find("input_base64")->get_string());
    const std::int64_t version = *vector.find("version")->to_int64();
    CHECK(rqrcode::version_for(input) == static_cast<std::size_t>(version));
    const auto modules = rqrcode::modules(input);
    REQUIRE(modules);
    std::string joined;
    for (std::size_t r = 0; r < modules->size(); ++r) {
      if (r != 0) joined += "\n";
      for (const bool dark : (*modules)[r]) joined += dark ? '1' : '0';
    }
    CHECK(joined == *vector.find("modules")->get_string());
    if (const auto* svg = vector.find("svg"); svg != nullptr && svg->get_string() != nullptr) {
      CHECK(rqrcode::svg(input) == *svg->get_string());
    }
  }
}

TEST_CASE("rqrcode: data that does not fit version 40 gives nothing") {
  CHECK_FALSE(rqrcode::svg(std::string(3000, 'a')).has_value());
  CHECK(rqrcode::svg("http://campfire.test").has_value());
}

}  // namespace campfire::app
