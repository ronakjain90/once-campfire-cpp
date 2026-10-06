// A small PCRE2 wrapper for the Propshaft patterns. Build-time code. Works on bytes.
#pragma once

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.hpp"

namespace campfire::assets::build {

struct Match {
  // Byte ranges (start, end) of the whole match and of each group. npos start: group did not match.
  std::vector<std::pair<std::size_t, std::size_t>> groups;
  [[nodiscard]] bool has(std::size_t i) const { return i < groups.size() && groups[i].first != std::string_view::npos; }
};

class Regex {
 public:
  [[nodiscard]] static Result<Regex> compile(const std::string& pattern, bool multiline = false);
  // The first match at or after `from`.
  [[nodiscard]] Result<std::optional<Match>> search(std::string_view subject, std::size_t from = 0) const;
  // Calls `replace` for each match and returns the new text (String#gsub with a block).
  [[nodiscard]] Result<std::string> gsub(std::string_view subject,
                                         const std::function<std::string(const Match&)>& replace) const;
  [[nodiscard]] bool matches(std::string_view subject) const;

 private:
  struct CodeDeleter {
    void operator()(pcre2_code* p) const noexcept { pcre2_code_free(p); }
  };
  std::unique_ptr<pcre2_code, CodeDeleter> code_;
};

}  // namespace campfire::assets::build
