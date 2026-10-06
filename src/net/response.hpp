// Response builder and wire format. Rust: crates/kit/src/response.rs, front/conn.rs (hyper encoder).
#pragma once

#include <sys/uio.h>

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/out.hpp"
#include "net/http.hpp"

namespace campfire::net {

// A response. Headers keep the order of insertion, because the order is part of the bytes that
// the Rust port sends. Names and values are views: they must outlive the write (use static text
// or the arena of the request; `add_copy` copies into the arena).
class Response {
 public:
  explicit Response(std::pmr::memory_resource* resource, int status_code = 200)
      : status(status_code), headers(resource), resource_(resource) {}
  Response(Response&&) noexcept = default;
  Response& operator=(Response&&) = delete;

  int status;
  std::pmr::vector<Header> headers;
  // Send the body with chunked transfer coding (the Rust app does this for gzip bodies).
  bool chunked = false;
  // The body already has chunk framing (sizes, CRLF, the last chunk). Implies a chunked reply. Use it
  // to keep the exact chunk boundaries of the Rust port.
  bool framed = false;

  [[nodiscard]] std::pmr::memory_resource* resource() const noexcept { return resource_; }

  void add(std::string_view name, std::string_view value) { headers.push_back({name, value}); }
  void add_copy(std::string_view name, std::string_view value);
  // Replaces the value of the first header with this name, or appends one.
  void set(std::string_view name, std::string_view value);
  void erase(std::string_view name);
  [[nodiscard]] std::string_view get(std::string_view name) const noexcept;
  [[nodiscard]] bool has(std::string_view name) const noexcept { return find(name) != nullptr; }

  // Body choices. Only the last call counts.
  void body_view(std::string_view bytes);  // static text or arena bytes
  void body_out(Out&& out);                // buffer in the arena
  // A slice of an immutable entry. `owner` keeps the entry alive until the write ends.
  void body_shared(std::shared_ptr<const void> owner, std::string_view bytes);

  [[nodiscard]] std::size_t body_size() const noexcept;
  [[nodiscard]] bool has_body() const noexcept { return body_size() != 0; }
  // Appends the body iovecs, from byte `skip` on. Returns the number written.
  std::size_t body_iovecs(std::span<iovec> out, std::size_t skip) const noexcept;
  // The body as one view, or nothing if the body is a chain of buffers (A8: the front cache reads it).
  [[nodiscard]] std::optional<std::string_view> body_contiguous() const noexcept {
    if (out_) return std::nullopt;
    return view_;
  }
  // Appends the whole body to `out`.
  void body_append_to(std::string& out) const;

 private:
  friend class Wire;
  [[nodiscard]] const Header* find(std::string_view name) const noexcept;

  std::pmr::memory_resource* resource_;
  std::string_view view_;
  std::optional<Out> out_;
  std::shared_ptr<const void> owner_;
};

[[nodiscard]] std::string_view reason_phrase(int status) noexcept;

struct WireOptions {
  bool head_only = false;  // answer to HEAD: send the headers and no body
  bool close = false;      // add "connection: close"
  bool keep_alive_header = false;  // HTTP/1.0 keep-alive: add "connection: keep-alive"
  int http_minor = 1;              // the status line has the version of the request, as hyper does
};

// The bytes of one response: the head (status line, headers, blank line) and the body. The Wire
// borrows the Response, which must stay alive and unchanged while the wire is in use.
class Wire {
 public:
  Wire(std::pmr::memory_resource* resource, const Response& response, const WireOptions& options);

  [[nodiscard]] std::size_t total() const noexcept { return total_; }
  // Writes iovecs for the bytes from offset `skip` on, for `writev`. Returns the number written.
  std::size_t fill_iovecs(std::span<iovec> out, std::size_t skip) const noexcept;

 private:
  Out head_;
  const Response* response_;
  bool send_body_;
  std::string_view tail_;
  std::size_t body_size_ = 0;
  std::size_t total_ = 0;
};

}  // namespace campfire::net
