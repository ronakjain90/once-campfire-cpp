// Files that tools/embed.py puts in the program. Rails: public/404.html; Rust: crates/campfire (public pages).
#pragma once

#include <string_view>

namespace campfire::app::data {

extern const std::string_view f_404_html;
extern const std::string_view f_404_html_gz_chunked;  // gzip of 404.html with the chunk framing of the Rust port
extern const std::string_view f_up_html;
extern const std::string_view f_up_html_gz;
extern const std::string_view f_422_html;
extern const std::string_view f_500_html;
extern const std::string_view f_502_html;
extern const std::string_view f_schema_sql;  // data/schema.sql: the database schema of a new install
}  // namespace campfire::app::data
