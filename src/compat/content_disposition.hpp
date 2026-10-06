// ActionDispatch::Http::ContentDisposition.format (Rust: content_disposition.rs). An ASCII
// filename= for old clients, transliterated with I18n's default approximations, and the full
// name in RFC 5987's filename*=.
#pragma once

#include <string>
#include <string_view>

namespace campfire::compat {

std::string content_disposition(std::string_view disposition, std::string_view filename);

}  // namespace campfire::compat
