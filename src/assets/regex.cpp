// A small PCRE2 wrapper for the Propshaft patterns. Build-time code.
#include "assets/regex.hpp"

#include <cstdint>

namespace campfire::assets::build {

namespace {
struct MatchDataDeleter {
  void operator()(pcre2_match_data* p) const noexcept { pcre2_match_data_free(p); }
};
struct ContextDeleter {
  void operator()(pcre2_compile_context* p) const noexcept { pcre2_compile_context_free(p); }
};
}  // namespace

Result<Regex> Regex::compile(const std::string& pattern, bool multiline) {
  std::unique_ptr<pcre2_compile_context, ContextDeleter> context(pcre2_compile_context_create(nullptr));
  pcre2_set_newline(context.get(), PCRE2_NEWLINE_LF);
  int error = 0;
  PCRE2_SIZE offset = 0;
  const std::uint32_t options = multiline ? PCRE2_MULTILINE : 0U;
  pcre2_code* code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()), pattern.size(), options, &error,
                                   &offset, context.get());
  if (code == nullptr) {
    return fail(Errc::Parse, "regex " + pattern + " failed at " + std::to_string(offset));
  }
  Regex regex;
  regex.code_.reset(code);
  return regex;
}

Result<std::optional<Match>> Regex::search(std::string_view subject, std::size_t from) const {
  std::unique_ptr<pcre2_match_data, MatchDataDeleter> data(pcre2_match_data_create_from_pattern(code_.get(), nullptr));
  const int rc = pcre2_match(code_.get(), reinterpret_cast<PCRE2_SPTR>(subject.data()), subject.size(), from, 0,
                             data.get(), nullptr);
  if (rc == PCRE2_ERROR_NOMATCH) {
    return std::optional<Match>{};
  }
  if (rc < 0) {
    return fail(Errc::Internal, "pcre2_match error " + std::to_string(rc));
  }
  const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(data.get());
  const std::uint32_t count = pcre2_get_ovector_count(data.get());
  Match match;
  for (std::size_t i = 0; i < count; ++i) {
    if (ovector[2 * i] == PCRE2_UNSET) {
      match.groups.emplace_back(std::string_view::npos, std::string_view::npos);
    } else {
      match.groups.emplace_back(ovector[2 * i], ovector[2 * i + 1]);
    }
  }
  return std::optional<Match>(std::move(match));
}

Result<std::string> Regex::gsub(std::string_view subject,
                                const std::function<std::string(const Match&)>& replace) const {
  std::string out;
  out.reserve(subject.size());
  std::size_t last = 0;
  std::size_t pos = 0;
  while (pos <= subject.size()) {
    auto found = search(subject, pos);
    if (!found) {
      return std::unexpected(found.error());
    }
    if (!*found) {
      break;
    }
    const Match& match = **found;
    const auto [start, end] = match.groups[0];
    out.append(subject.substr(last, start - last));
    out.append(replace(match));
    last = end;
    pos = end > start ? end : end + 1;
  }
  out.append(subject.substr(last));
  return out;
}

bool Regex::matches(std::string_view subject) const {
  auto found = search(subject);
  return found && found->has_value();
}

}  // namespace campfire::assets::build
