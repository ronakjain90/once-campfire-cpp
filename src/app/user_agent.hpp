// The useragent gem 0.16.11 (what Rails `allow_browser` and platform_agent read). Rust:
// crates/campfire/src/concerns/user_agent.rs. `parse` splits the header into products. The first browser class of
// `UserAgent::Browsers::ALL` that accepts them decides how `browser`, `version`, `platform`, `os`, `bot?` and `mobile?`
// answer. Where the gem raises (a NoMethodError on nil), the `try_*` calls return `Raised`.
#pragma once

#include <compare>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace campfire::app::ua {

// The gem raised instead of an answer.
struct Raised {};
template <class T>
using Rb = std::expected<T, Raised>;

// One piece of a version: a run of digits (no leading zeros) or a word.
struct Segment {
  bool is_int = false;
  std::string text;
  friend bool operator==(const Segment&, const Segment&) = default;
};

// `UserAgent::Version`. The order is the gem's `<=>`: not a total order, so `compare` is a plain function.
class Version {
 public:
  Version() : Version(std::string_view{}) {}
  explicit Version(std::string_view text);

  [[nodiscard]] bool is_nil() const noexcept { return blank_; }  // empty or only whitespace
  [[nodiscard]] bool is_present() const;                         // `to_s.present?`
  [[nodiscard]] const std::string& str() const noexcept { return text_; }
  [[nodiscard]] std::vector<Segment> to_a() const;
  // `<=>`: -1, 0 or 1.
  [[nodiscard]] int compare(const Version& other) const;
  [[nodiscard]] bool less(const Version& other) const { return compare(other) < 0; }
  friend bool operator==(const Version& a, const Version& b) { return a.text_ == b.text_; }

 private:
  std::string text_;
  bool blank_ = true;
  bool comparable_ = false;
};

struct Product {
  std::string name;
  Version version;
  std::optional<std::vector<std::string>> comment;
};

enum class Kind : std::uint8_t {
  Base,
  Edge,
  InternetExplorer,
  Opera,
  WechatBrowser,
  Vivaldi,
  Chrome,
  ITunes,
  PlayStation,
  PodcastAddict,
  Webkit,
  Gecko,
  WindowsMediaPlayer,
  AppleCoreMedia,
  Libavformat
};

class Agent {
 public:
  Agent(Kind kind, std::vector<Product> products) : kind_(kind), products_(std::move(products)) {}

  [[nodiscard]] Rb<std::optional<std::string>> try_browser() const;
  [[nodiscard]] Rb<std::optional<Version>> try_version() const;
  [[nodiscard]] Rb<std::optional<std::string>> try_platform() const;
  [[nodiscard]] Rb<std::optional<std::string>> try_os() const;
  [[nodiscard]] Rb<bool> try_mobile() const;
  [[nodiscard]] bool is_bot() const;
  // Nil answers as "" (or the empty version).
  [[nodiscard]] std::string browser() const;
  [[nodiscard]] Version version() const;
  [[nodiscard]] const std::vector<Product>& products() const noexcept { return products_; }

 private:
  [[nodiscard]] const Product* first() const;
  [[nodiscard]] const Product* last() const;
  [[nodiscard]] const Product* detect_product(std::string_view name) const;
  [[nodiscard]] const Product* application() const;
  [[nodiscard]] const std::vector<std::string>* application_comment() const;
  [[nodiscard]] std::optional<Version> base_version() const;
  [[nodiscard]] std::optional<std::string> playstation_browser() const;
  [[nodiscard]] std::string webkit_browser() const;
  [[nodiscard]] std::string gecko_browser() const;
  [[nodiscard]] bool opera_mini() const;
  [[nodiscard]] std::optional<Version> opera_version() const;
  [[nodiscard]] std::optional<Version> playstation_version() const;
  [[nodiscard]] Version webkit_version() const;
  [[nodiscard]] std::optional<Version> webkit() const;
  [[nodiscard]] std::optional<std::string> webkit_platform() const;
  [[nodiscard]] std::optional<std::string> playstation_platform() const;
  [[nodiscard]] std::optional<std::string> webkit_os() const;
  [[nodiscard]] std::optional<std::string> itunes_os() const;
  [[nodiscard]] std::optional<std::string> itunes_full_os() const;
  [[nodiscard]] std::optional<std::string> playstation_os() const;
  [[nodiscard]] Rb<std::optional<std::string>> podcast_addict_os() const;
  [[nodiscard]] std::optional<std::string> gecko_os() const;
  [[nodiscard]] Rb<std::uint64_t> windows_media_player_major() const;
  [[nodiscard]] Rb<std::string_view> windows_media_player_os() const;

  Kind kind_;
  std::vector<Product> products_;
};

// `UserAgent.parse`: a blank string parses as "Mozilla/4.0 (compatible)".
[[nodiscard]] Agent parse(std::string_view user_agent);
// `String#downcase` for the needles of this code (ASCII, and the Kelvin sign that lowercases to "k").
[[nodiscard]] std::string downcase(std::string_view text);
// ActiveSupport `present?` for a string (Unicode whitespace).
[[nodiscard]] bool is_present(std::string_view text);

}  // namespace campfire::app::ua
