// Differential test against the Rails pipeline: tests/data/corpus.json holds, for each case of the
// Rust rich text corpus (crates/richtext/tests/corpus/expected.json), the stored body and what
// TextMessagePresentationFilters.apply(...).to_html returned in Rails ("filtered").
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "port_rules.hpp"
#include "richtext/filters.hpp"

namespace {

// A reader for the one JSON shape of the corpus: an array of objects with string, null values.
struct Case {
  std::string name;
  std::string body;
  bool has_filtered = false;
  std::string filtered;
  bool has_error = false;
};

class Reader {
 public:
  explicit Reader(const std::string& text) : text_(text) {}

  std::vector<Case> cases() {
    std::vector<Case> out;
    expect('[');
    skip();
    if (peek() == ']') {
      return out;
    }
    while (true) {
      out.push_back(object());
      skip();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      expect(']');
      return out;
    }
  }

 private:
  char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
  void skip() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n')) {
      ++pos_;
    }
  }
  void expect(char c) {
    skip();
    REQUIRE(peek() == c);
    ++pos_;
  }

  Case object() {
    Case c;
    expect('{');
    while (true) {
      std::string key = string();
      expect(':');
      skip();
      if (peek() == 'n') {
        pos_ += 4;  // null
        if (key == "error") {
          c.has_error = false;
        }
      } else {
        std::string value = string();
        if (key == "name") {
          c.name = value;
        } else if (key == "body") {
          c.body = value;
        } else if (key == "filtered") {
          c.has_filtered = true;
          c.filtered = value;
        } else if (key == "error") {
          c.has_error = true;
        }
      }
      skip();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      expect('}');
      return c;
    }
  }

  static void append_utf8(unsigned cp, std::string& out) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  unsigned hex4() {
    unsigned value = static_cast<unsigned>(std::stoul(text_.substr(pos_, 4), nullptr, 16));
    pos_ += 4;
    return value;
  }

  std::string string() {
    expect('"');
    std::string out;
    while (true) {
      REQUIRE(pos_ < text_.size());
      char c = text_[pos_++];
      if (c == '"') {
        return out;
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      char e = text_[pos_++];
      switch (e) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'u': {
          unsigned cp = hex4();
          if (cp >= 0xD800 && cp < 0xDC00 && text_.compare(pos_, 2, "\\u") == 0) {
            pos_ += 2;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (hex4() - 0xDC00);
          }
          append_utf8(cp, out);
          break;
        }
        default: out.push_back(e);
      }
    }
  }

  const std::string& text_;
  std::size_t pos_ = 0;
};

bool in_scope(const Case& c) {
  // Bodies with Action Text attachments (mentions, opengraph embeds, Trix attachments and
  // galleries) belong to task T10: canonicalization and the attachment filters change the result.
  std::string lower = c.body;
  for (char& ch : lower) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return lower.find("attachment") == std::string::npos;
}

}  // namespace

TEST_CASE("the filtered output matches Rails for the corpus cases without attachments") {
  std::ifstream file(std::string(RICHTEXT_TEST_DATA_DIR) + "/corpus.json", std::ios::binary);
  REQUIRE(file.good());
  std::stringstream buffer;
  buffer << file.rdbuf();
  const std::string text = buffer.str();
  Reader reader(text);
  const std::vector<Case> cases = reader.cases();

  std::size_t scoped = 0;
  std::size_t passed = 0;
  std::size_t name_cases = 0;
  std::size_t style_cases = 0;
  for (const Case& c : cases) {
    if (!in_scope(c)) {
      continue;
    }
    ++scoped;
    auto result = campfire::richtext::filter_message_html(c.body);
    bool ok = false;
    if (c.has_error) {
      ok = !result.has_value();
    } else if (result.has_value()) {
      // Deliberate differences from Rails (README "Rich text attributes"), applied to the expected value only:
      // `name` attributes are dropped, and style keeps only plain color and background-color values.
      // The style format is compared in the canonical form (Rails writes "color:red;", the port keeps the input).
      const bool has_name = c.filtered.find(" name=\"") != std::string::npos;
      const bool has_style = c.filtered.find(" style=\"") != std::string::npos;
      if (has_name) {
        ++name_cases;
      }
      if (has_style) {
        ++style_cases;
      }
      std::string expected = port_rules::without_names(c.filtered);
      expected = port_rules::rewrite_attribute(expected, "style", port_rules::port_style);
      const std::string actual = port_rules::canonical_styles(*result);
      ok = expected == actual;
    }
    if (ok) {
      ++passed;
    } else {
      std::string actual = result ? *result : std::string("<error>");
      INFO("case: ", c.name);
      INFO("body:     ", c.body.substr(0, 400));
      INFO("expected: ", c.has_error ? "<error>" : c.filtered.substr(0, 400));
      INFO("actual:   ", actual.substr(0, 400));
      CHECK(ok);
    }
  }
  std::printf(
      "corpus: %zu cases, %zu in scope, %zu passing (%zu with the name difference, %zu with the style difference)\n",
      cases.size(), scoped, passed, name_cases, style_cases);
  CHECK(cases.size() == 658);
  CHECK(passed == scoped);
}
