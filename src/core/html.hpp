// HTML escape. Matches ERB::Util.html_escape (reference: ActiveSupport core_ext/string/output_safety).
#pragma once

#include <cstddef>
#include <string_view>

namespace campfire {

class Out;

// Text that is already safe to put in an HTML document: escaped, or written by the program.
// `Out::append(SafeHtml)` writes it with no change. `Out::append(std::string_view)` does not
// exist, so a program cannot write unescaped text by mistake. A `SafeHtml` is a view: it does not
// own the bytes.
class SafeHtml {
 public:
  constexpr SafeHtml() noexcept = default;

  // A string literal is written by the program, so it is trusted. This is checked at compile time.
  template <std::size_t N>
  [[nodiscard]] static consteval SafeHtml literal(const char (&text)[N]) noexcept {
    return SafeHtml(std::string_view(text, N - 1));
  }

  // Marks text that the caller has escaped or has built from safe parts. The name is a warning:
  // each call needs a reason. Do not use it on text from a user.
  [[nodiscard]] static constexpr SafeHtml trusted(std::string_view text) noexcept { return SafeHtml(text); }

  [[nodiscard]] constexpr std::string_view view() const noexcept { return text_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return text_.empty(); }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return text_.size(); }

 private:
  explicit constexpr SafeHtml(std::string_view text) noexcept : text_(text) {}
  std::string_view text_;
};

// Writes `text` to `out` with these replacements, as ERB::Util.html_escape does:
//   &  ->  &amp;     <  ->  &lt;     >  ->  &gt;     "  ->  &quot;     '  ->  &#39;
// Other bytes, including bytes of UTF-8 sequences and NUL, are copied with no change.
void html_escape(Out& out, std::string_view text);

// Returns true if `html_escape` would change `text`.
[[nodiscard]] bool needs_html_escape(std::string_view text) noexcept;

}  // namespace campfire
