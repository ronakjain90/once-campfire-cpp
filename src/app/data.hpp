// Files that tools/embed.py puts in the program. Rails: public/404.html; Rust: crates/campfire (public pages).
#pragma once

#include <string_view>

namespace campfire::app::data {

extern const std::string_view f_404_html;
extern const std::string_view f_404_html_gz_chunked;  // gzip of 404.html with the chunk framing of the Rust port
extern const std::string_view f_up_html;
extern const std::string_view f_up_html_gz;
}  // namespace campfire::app::data
