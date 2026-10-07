// The search query. Rails: app/controllers/searches_controller.rb (`query`), app/models/message/searchable.rb.
// Rust: crates/campfire/src/integrations/search.rs, crates/db/src/models/message.rs (`match_terms`).
#pragma once

#include <string>
#include <string_view>

namespace campfire::models::search_query {

// Ruby `/[[:word:]]/` (Onigmo, Unicode 15.0.0).
[[nodiscard]] bool is_word(char32_t c) noexcept;

// `params[:q].gsub(/[^[:word:]]/, " ")`: one space for each character that is not a word character. A byte that is
// not valid UTF-8 becomes one space.
[[nodiscard]] std::string sanitize(std::string_view q);

// `query.present?`: the query has a character that is not white space.
[[nodiscard]] bool is_present(std::string_view query) noexcept;

// Each word as an FTS5 string, so that every word must be in the text and none is read as query syntax. The words
// are separated by white space or by NUL. Empty when the query has no word.
[[nodiscard]] std::string match_terms(std::string_view query);

}  // namespace campfire::models::search_query
