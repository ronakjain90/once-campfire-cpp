// Tests of tools/ctc.py and the template runtime. ERB is the oracle: erb/<case>.<set>.expected is
// the output of the ERB twin with trim_mode "-" (test/gen_expected.rb, run in campfire-reference).
#include <doctest.h>

#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/html.hpp"
#include "core/out.hpp"
#include "views/fragment_cache.hpp"
#include "views/test_templates.gen.hpp"

namespace campfire::views {
namespace {

struct Data {
  std::string_view name;
  std::string_view markup;
  std::vector<std::string_view> items;
  int n;
  bool flag;
};

using CaseFn = void (*)(Out&, std::string_view, SafeHtml, const std::vector<std::string_view>&, int, bool,
                        std::optional<std::string_view>);

struct Case {
  const char* name;
  CaseFn fn;
};

const Case kCases[] = {
    {"text", &cases::text},       {"out_escape", &cases::out_escape}, {"safe", &cases::safe},
    {"if_else", &cases::if_else}, {"for_loop", &cases::for_loop},     {"trim", &cases::trim},
    {"nested", &cases::nested},   {"comment", &cases::comment},       {"braces", &cases::braces},
    {"partial", &cases::partial},
};

// The same data sets as test/gen_expected.rb.
const Data kSetA{"<b>&\"' \xc3\xa9", "<i>safe</i>", {"a", "b", "c"}, 2, true};
const Data kSetB{"", "", {}, 7, false};

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "cannot open " << path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string render(const Case& c, const Data& d) {
  Out out;
  c.fn(out, d.name, SafeHtml::trusted(d.markup), d.items, d.n, d.flag, std::nullopt);
  return out.to_string();
}

}  // namespace

TEST_CASE("ctc: output equals the ERB twin (trim_mode '-')") {
  int compared = 0;
  for (const Case& c : kCases) {
    for (const char* set : {"a", "b"}) {
      const std::string path = std::string(CAMPFIRE_VIEWS_TEST_DIR) + "/erb/" + c.name + "." + set + ".expected";
      CAPTURE(c.name);
      CAPTURE(set);
      CHECK_EQ(render(c, set[0] == 'a' ? kSetA : kSetB), read_file(path));
      ++compared;
    }
  }
  MESSAGE("ERB comparisons: " << compared);
  CHECK_EQ(compared, 20);
}

TEST_CASE("ctc: left trim of an expression and of a tag at the start of a line") {
  Out out;
  extra::trim_expr(out, 5);
  CHECK_EQ(out.to_string(), "a\n5\nb  5\n5  5  x\n");
}

TEST_CASE("ctc: call and wrap blocks") {
  Out out;
  extra::wrap(out, "n&");
  CHECK_EQ(out.to_string(),
           "<div class=\"n&amp;\"><p>n&amp;</p><div class=\"in\">x</div></div>\n"
           "<b>n&amp;</b><i>n&amp;</i>\n");
}

TEST_CASE("ctc: write rules of {{ }} and {{= }}") {
  Out out;
  extra::emit_types(out);
  CHECK_EQ(out.to_string(), "-5 18446744073709551615 true a&lt; |7\n<s> <s>\n");
}

namespace {

class MapCache final : public FragmentCache {
 public:
  [[nodiscard]] bool enabled() const noexcept override { return true; }
  bool read(std::string_view key, Out& out) override {
    reads.emplace_back(key);
    auto it = entries.find(std::string(key));
    if (it == entries.end()) {
      return false;
    }
    out.append(SafeHtml::trusted(it->second));
    return true;
  }
  void write(std::string_view key, std::string_view html) override { entries[std::string(key)] = std::string(html); }

  std::map<std::string, std::string> entries;
  std::vector<std::string> reads;
};

std::string render_cache(int n) {
  Out out;
  extra::cache(out, n);
  return out.to_string();
}

}  // namespace

TEST_CASE("ctc: cache block with the null cache renders the body each time") {
  CHECK_EQ(render_cache(5), "[c5]\n");
  CHECK_EQ(render_cache(6), "[c6]\n");
}

TEST_CASE("ctc: cache block with a real cache reads, then writes on a miss") {
  MapCache cache;
  cache.entries["k7"] = "HIT";
  set_fragment_cache(&cache);
  CHECK_EQ(render_cache(7), "[HIT]\n");
  CHECK_EQ(render_cache(5), "[c5]\n");
  CHECK_EQ(cache.entries.at("k5"), "c5");
  CHECK_EQ(render_cache(5), "[c5]\n");
  set_fragment_cache(nullptr);
  CHECK_EQ(cache.reads, std::vector<std::string>{"k7", "k5", "k5"});
  CHECK_EQ(render_cache(7), "[c7]\n");
}

}  // namespace campfire::views
