// Ruby's own string behavior, as Rails and Rack apply it (Rust: crates/ruby).
// ERB escape, String#to_i/#to_f/#strip, Float#to_s, CGI.escape, ERB::Util.url_encode,
// Active Record integer binding and Rack byte ranges. All functions work on bytes.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace campfire::compat {

// ERB::Util.html_escape: & < > " ' become &amp; &lt; &gt; &quot; &#39;.
std::string html_escape(std::string_view s);
void append_html_escaped(std::string& out, std::string_view s);

// String#strip: NUL and ASCII whitespace off both ends (not U+00A0).
std::string_view strip(std::string_view s);

// String#to_i, saturating at the int64 bounds where Ruby goes on to a Bignum.
int64_t to_i(std::string_view s);
// String#to_i, or nullopt where Ruby's answer does not fit in an int64.
std::optional<int64_t> to_i_checked(std::string_view s);
// A string as Active Record binds it for an integer column (find, find_by(id:), where):
// nullopt unless it starts like a number, and nullopt out of range (where Rails raises).
std::optional<int64_t> integer_cast(std::string_view s);

// String#to_f (reads up to the first NUL).
double to_f(std::string_view s);
// Float#to_s in Ruby 3.4.
std::string float_to_s(double value);

// CGI.escape: all but A-Za-z0-9_.-~ is percent-encoded and a space becomes "+".
std::string cgi_escape(std::string_view s);
// ERB::Util.url_encode, and Addressable's encode_component(s, UNRESERVED): the same set,
// with a space as "%20".
std::string url_encode(std::string_view s);

struct ByteRange {
  uint64_t first;
  uint64_t last;  // inclusive
  friend bool operator==(const ByteRange&, const ByteRange&) = default;
};

// Rack::Utils.get_byte_ranges(http_range, size). nullopt means "serve everything". An empty
// vector means 416. The ranges are inclusive and within `size`.
std::optional<std::vector<ByteRange>> byte_ranges(std::optional<std::string_view> header, uint64_t size);

}  // namespace campfire::compat
