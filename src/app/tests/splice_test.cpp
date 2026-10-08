// Tests of the gzip and ETag splice of page parts (src/app/splice.hpp). Rust: crates/kit/src/deflater/splice.rs tests.
#include "app/splice.hpp"

#include <doctest.h>
#include <zlib.h>

#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "app/page_cache.hpp"

namespace campfire::app {

namespace {

std::string gunzip(std::string_view gz) {
  z_stream z{};
  REQUIRE(inflateInit2(&z, 16 + 15) == Z_OK);
  std::string out;
  char buffer[65536];
  z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(gz.data()));
  z.avail_in = static_cast<uInt>(gz.size());
  int rc = Z_OK;
  while (rc != Z_STREAM_END) {
    z.next_out = reinterpret_cast<Bytef*>(buffer);
    z.avail_out = sizeof buffer;
    rc = inflate(&z, Z_NO_FLUSH);
    REQUIRE((rc == Z_OK || rc == Z_STREAM_END));
    out.append(buffer, sizeof buffer - z.avail_out);
  }
  CHECK(z.avail_in == 0);  // one gzip member, nothing after it
  inflateEnd(&z);
  return out;
}

// A message-like fragment: repetitive markup, with text that differs.
std::string fragment(std::mt19937& rng, int id) {
  std::string out = "<div class=\"message\" id=\"message_" + std::to_string(id) + "\"><span class=\"author\">";
  const int words = 150 + static_cast<int>(rng() % 200);
  for (int i = 0; i < words; ++i) {
    out += "w" + std::to_string(rng() % 500) + ' ';
    if (i % 17 == 0) out += "<svg viewBox=\"0 0 24 24\"><path d=\"M12 2L2 7l10 5 10-5-10-5z\"/></svg>";
  }
  out += "</span></div>\n";
  return out;
}

struct Page {
  std::string body;
  std::vector<FragmentSpan> spans;
};

// `before`, then the fragments with `glue` between them, then `after`.
Page page(const std::string& before, const std::vector<std::string>& fragments, const std::string& glue,
          const std::string& after) {
  Page p;
  p.body = before;
  for (std::size_t i = 0; i < fragments.size(); ++i) {
    if (i != 0) p.body += glue;
    p.spans.push_back({p.body.size(), fragments[i].size()});
    p.body += fragments[i];
  }
  p.body += after;
  return p;
}

}  // namespace

TEST_CASE("splice: the CRC of a concatenation is zlib's") {
  std::mt19937 rng(7);
  for (int round = 0; round < 50; ++round) {
    std::string a(rng() % 5000, '\0');
    std::string b(rng() % 70000, '\0');
    for (char& c : a) c = static_cast<char>(rng());
    for (char& c : b) c = static_cast<char>(rng());
    const auto crc_a =
        static_cast<std::uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(a.data()), static_cast<uInt>(a.size())));
    const auto crc_b =
        static_cast<std::uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(b.data()), static_cast<uInt>(b.size())));
    const std::string ab = a + b;
    const auto crc_ab =
        static_cast<std::uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(ab.data()), static_cast<uInt>(ab.size())));
    CHECK(splice::crc32_combine(crc_a, crc_b, b.size()) == crc_ab);
  }
}

TEST_CASE("splice: a page without a big fragment uses the whole-body path") {
  splice::clear();
  const Page p = page("<html>", {std::string(500, 'x'), std::string(1023, 'y')}, "", "</html>");
  CHECK_FALSE(splice::build(p.body, p.spans).has_value());
}

TEST_CASE("splice: the gzip decodes to the body and the ETag is parts_etag") {
  splice::clear();
  std::mt19937 rng(1);
  std::vector<std::string> fragments;
  for (int i = 0; i < 40; ++i) fragments.push_back(fragment(rng, i));
  const std::string layout_before = "<!doctype html><html><head>" + std::string(9000, 'h') + "</head><body>";
  const std::string layout_after = "</body>" + std::string(4000, 'f') + "</html>";
  const Page p = page(layout_before, fragments, "\n  ", layout_after);

  const auto first = splice::build(p.body, p.spans);
  REQUIRE(first.has_value());
  CHECK(gunzip(first->gzip) == p.body);
  CHECK(first->etag == parts_etag(p.body, p.spans));
  // Within a few percent of the whole body compressed at once.
  CHECK(first->gzip.size() < p.body.size() / 2);
  const splice::Stats after_first = splice::stats();
  CHECK(after_first.pieces == fragments.size() + 2);

  // The same page again: every piece is kept, and the bytes are the same.
  const auto second = splice::build(p.body, p.spans);
  REQUIRE(second.has_value());
  CHECK(second->gzip == first->gzip);
  CHECK(second->etag == first->etag);
  CHECK(splice::stats().pieces == after_first.pieces);

  // A new message at the end: only its piece and the pieces after it are new.
  fragments.push_back(fragment(rng, 40));
  const Page next = page(layout_before, fragments, "\n  ", layout_after);
  const auto third = splice::build(next.body, next.spans);
  REQUIRE(third.has_value());
  CHECK(gunzip(third->gzip) == next.body);
  CHECK(third->etag == parts_etag(next.body, next.spans));
  CHECK(splice::stats().pieces == after_first.pieces + 2);
}

TEST_CASE("splice: kept pieces stay correct when fragments change order, glue and neighbours") {
  splice::clear();
  std::mt19937 rng(3);
  std::vector<std::string> pool;
  for (int i = 0; i < 30; ++i) pool.push_back(fragment(rng, i));
  for (int round = 0; round < 60; ++round) {
    std::vector<std::string> chosen;
    const std::size_t count = 1 + rng() % 12;
    for (std::size_t i = 0; i < count; ++i) chosen.push_back(pool[rng() % pool.size()]);
    // Glue up to and above the limit, so some gaps are glue and some are text parts.
    const std::string glue(rng() % 2 == 0 ? rng() % 40 : 200 + rng() % 120, 'g');
    const std::string before(rng() % 3 == 0 ? 0 : rng() % 3000, 'b');
    const std::string after(rng() % 3 == 0 ? 0 : rng() % 3000, 'a');
    const Page p = page(before, chosen, glue, after);
    const auto out = splice::build(p.body, p.spans);
    REQUIRE(out.has_value());
    CHECK(gunzip(out->gzip) == p.body);
    CHECK(out->etag == parts_etag(p.body, p.spans));
  }
}

}  // namespace campfire::app
