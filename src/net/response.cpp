// Response builder and wire format. Rust: crates/kit/src/response.rs, front/conn.rs.
#include "net/response.hpp"

#include <algorithm>
#include <cstring>

namespace campfire::net {

namespace {

std::string_view copy_into(std::pmr::memory_resource* resource, std::string_view text) {
  if (text.empty()) return {};
  char* data = static_cast<char*>(resource->allocate(text.size(), 1));
  std::memcpy(data, text.data(), text.size());
  return {data, text.size()};
}

}  // namespace

void Response::add_copy(std::string_view name, std::string_view value) {
  headers.push_back({copy_into(resource_, name), copy_into(resource_, value)});
}

const Header* Response::find(std::string_view name) const noexcept {
  for (const Header& h : headers) {
    if (iequals(h.name, name)) return &h;
  }
  return nullptr;
}

void Response::set(std::string_view name, std::string_view value) {
  for (Header& h : headers) {
    if (iequals(h.name, name)) {
      h.value = value;
      return;
    }
  }
  add(name, value);
}

void Response::erase(std::string_view name) {
  std::erase_if(headers, [&](const Header& h) { return iequals(h.name, name); });
}

std::string_view Response::get(std::string_view name) const noexcept {
  const Header* h = find(name);
  return h != nullptr ? h->value : std::string_view{};
}

void Response::body_view(std::string_view bytes) {
  view_ = bytes;
  out_.reset();
  owner_.reset();
}

void Response::body_out(Out&& out) {
  view_ = {};
  owner_.reset();
  out_.emplace(std::move(out));
}

void Response::body_shared(std::shared_ptr<const void> owner, std::string_view bytes) {
  view_ = bytes;
  out_.reset();
  owner_ = std::move(owner);
}

std::size_t Response::body_size() const noexcept {
  return out_ ? out_->size() : view_.size();
}

std::size_t Response::body_iovecs(std::span<iovec> out, std::size_t skip) const noexcept {
  if (out.empty()) return 0;
  if (out_) return out_->fill_iovecs(out, skip);
  if (skip >= view_.size()) return 0;
  out[0].iov_base = const_cast<char*>(view_.data() + skip);  // writev never writes through it
  out[0].iov_len = view_.size() - skip;
  return 1;
}

void Response::body_append_to(std::string& out) const {
  const std::size_t total = body_size();
  out.reserve(out.size() + total);
  std::size_t skip = 0;
  while (skip < total) {
    iovec iov[16];
    const std::size_t n = body_iovecs(iov, skip);
    if (n == 0) break;
    for (std::size_t i = 0; i < n; ++i) {
      out.append(static_cast<const char*>(iov[i].iov_base), iov[i].iov_len);
      skip += iov[i].iov_len;
    }
  }
}

std::string_view reason_phrase(int status) noexcept {
  switch (status) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 416: return "Range Not Satisfiable";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "";
  }
}

Wire::Wire(std::pmr::memory_resource* resource, const Response& response, const WireOptions& options)
    : head_(resource, 1024), response_(&response) {
  const int status = response.status;
  const bool bodiless_status = status == 204 || status == 304 || (status >= 100 && status < 200);
  send_body_ = !options.head_only && !bodiless_status;
  body_size_ = send_body_ ? response.body_size() : 0;

  head_.append_raw(options.http_minor == 0 ? "HTTP/1.0 " : "HTTP/1.1 ");
  head_.append_int(status);
  head_.append_char(' ');
  head_.append_raw(reason_phrase(status));
  head_.append_raw("\r\n");
  for (const Header& h : response.headers) {
    head_.append_raw(h.name);
    head_.append_raw(": ");
    head_.append_raw(h.value);
    head_.append_raw("\r\n");
  }
  if (options.close) {
    head_.append_raw("connection: close\r\n");
  } else if (options.keep_alive_header) {
    head_.append_raw("connection: keep-alive\r\n");
  }
  const bool chunked = (response.chunked || response.framed) && !bodiless_status;
  if (chunked) {
    head_.append_raw("transfer-encoding: chunked\r\n");
  } else if (!bodiless_status && !response.has("content-length") && !response.unsized) {
    // hyper writes the length of a body after the headers of the map (and after "connection").
    head_.append_raw("content-length: ");
    head_.append_uint(response.body_size());
    head_.append_raw("\r\n");
  }
  head_.append_raw("\r\n");
  if (chunked && send_body_ && !response.framed) {
    if (body_size_ != 0) {
      char hex[24];
      char* end = hex + sizeof hex;
      char* p = end;
      for (std::size_t n = body_size_; n != 0; n >>= 4) *--p = "0123456789ABCDEF"[n & 15];
      head_.append_raw({p, static_cast<std::size_t>(end - p)});
      head_.append_raw("\r\n");
      tail_ = "\r\n0\r\n\r\n";
    } else {
      tail_ = "0\r\n\r\n";
    }
  }
  total_ = head_.size() + body_size_ + tail_.size();
}

std::size_t Wire::fill_iovecs(std::span<iovec> out, std::size_t skip) const noexcept {
  std::size_t n = head_.fill_iovecs(out, skip);
  const std::size_t head_size = head_.size();
  if (n == out.size()) return n;
  if (body_size_ != 0) {
    const std::size_t body_skip = skip > head_size ? skip - head_size : 0;
    n += response_->body_iovecs(out.subspan(n), body_skip);
  }
  if (!tail_.empty() && n < out.size()) {
    const std::size_t before = head_size + body_size_;
    const std::size_t tail_skip = skip > before ? skip - before : 0;
    if (tail_skip < tail_.size()) {
      out[n].iov_base = const_cast<char*>(tail_.data() + tail_skip);
      out[n].iov_len = tail_.size() - tail_skip;
      ++n;
    }
  }
  return n;
}

}  // namespace campfire::net
