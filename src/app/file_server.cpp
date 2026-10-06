// ActiveStorage::FileServer, Rack::Files#serving. Rust: crates/storage/src/file_server.rs.
#include "app/file_server.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "compat/ruby.hpp"
#include "core/time_format.hpp"

namespace campfire::app {

namespace {

constexpr std::string_view kBoundary = "AaB03x";

void set_header(ServedFile& served, std::string_view name, std::string value) {
  for (auto& [key, existing] : served.headers) {
    if (key == name) {
      existing = std::move(value);
      return;
    }
  }
  served.headers.emplace_back(std::string(name), std::move(value));
}

Error io_error(int code, const std::filesystem::path& path) {
  return Error{code == ENOENT || code == ENOTDIR ? Errc::NotFound : Errc::Io,
               path.string() + ": " + std::strerror(code)};
}

Result<ServedFile> serving(const FileRequest& request, const std::filesystem::path& path) {
  ServedFile served;
  if (request.method == "OPTIONS") {
    served.headers = {{"allow", "GET, HEAD, OPTIONS"}, {"content-length", "0"}};
    return served;
  }
  struct stat info{};
  if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return std::unexpected(io_error(errno, path));
  const std::string last_modified = format_httpdate(Timestamp::from_nanos(std::int64_t{info.st_mtime} * 1'000'000'000));
  if (request.if_modified_since == std::optional<std::string_view>(last_modified)) {
    served.status = 304;
    return served;
  }
  // Disk keys have no extension, so Rack's mime lookup falls back to its default.
  constexpr std::string_view mime_type = "text/plain";
  served.headers = {{"last-modified", last_modified}, {"content-type", std::string(mime_type)}};
  const auto size = static_cast<std::uint64_t>(info.st_size);
  std::string body;
  const auto ranges = compat::byte_ranges(request.range, size);
  if (!ranges) {
    served.status = 200;
    if (size > 0 && request.method != "HEAD") {
      auto bytes = read_file_range(path, 0, size - 1);
      if (!bytes) return std::unexpected(bytes.error());
      body = std::move(*bytes);
    }
    served.headers.emplace_back("content-length", std::to_string(size));
    served.body = std::move(body);
    return served;
  }
  if (ranges->empty()) {
    constexpr std::string_view text = "Byte range unsatisfiable\n";
    served.status = 416;
    served.headers = {{"content-type", "text/plain"},
                      {"content-length", std::to_string(text.size())},
                      {"x-cascade", "pass"},
                      {"content-range", "bytes */" + std::to_string(size)}};
    served.body = std::string(text);
    return served;
  }
  served.status = 206;
  std::uint64_t length = 0;
  const bool head = request.method == "HEAD" || size == 0;
  const auto add_range = [&](const compat::ByteRange& range) -> Status {
    length += range.last - range.first + 1;
    if (head) return {};
    auto bytes = read_file_range(path, range.first, range.last);
    if (!bytes) return std::unexpected(bytes.error());
    body += *bytes;
    return {};
  };
  const auto size_text = std::to_string(size);
  if (ranges->size() == 1) {
    const auto range = ranges->front();
    served.headers.emplace_back("content-range",
                                "bytes " + std::to_string(range.first) + "-" + std::to_string(range.last) + "/" +
                                    size_text);
    if (auto added = add_range(range); !added) return std::unexpected(added.error());
  } else {
    set_header(served, "content-type", "multipart/byteranges; boundary=" + std::string(kBoundary));
    for (const auto& range : *ranges) {
      const std::string heading = "\r\n--" + std::string(kBoundary) + "\r\ncontent-type: " + std::string(mime_type) +
                                  "\r\ncontent-range: bytes " + std::to_string(range.first) + "-" +
                                  std::to_string(range.last) + "/" + size_text + "\r\n\r\n";
      length += heading.size();
      if (!head) body += heading;
      if (auto added = add_range(range); !added) return std::unexpected(added.error());
    }
    const std::string closing = "\r\n--" + std::string(kBoundary) + "--\r\n";
    length += closing.size();
    if (!head) body += closing;
  }
  served.headers.emplace_back("content-length", std::to_string(length));
  served.body = std::move(body);
  return served;
}

}  // namespace

Result<std::string> read_file_range(const std::filesystem::path& path, std::uint64_t first, std::uint64_t last) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::unexpected(io_error(errno, path));
  std::string bytes(last - first + 1, '\0');
  std::size_t done = 0;
  while (done < bytes.size()) {
    const ssize_t got = ::pread(fd, bytes.data() + done, bytes.size() - done, static_cast<off_t>(first + done));
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) break;
    done += static_cast<std::size_t>(got);
  }
  ::close(fd);
  bytes.resize(done);
  return bytes;
}

Result<ServedFile> serve_file(const FileRequest& request, const std::filesystem::path& path,
                              std::optional<std::string_view> content_type,
                              std::optional<std::string_view> disposition) {
  auto served = serving(request, path);
  if (!served) return served;
  if (served->status == 416) {
    std::erase_if(served->headers, [](const auto& header) { return header.first == "x-cascade"; });
  }
  set_header(*served, "content-type", std::string(content_type.value_or("application/octet-stream")));
  set_header(*served, "content-disposition", std::string(disposition.value_or("attachment")));
  return served;
}

}  // namespace campfire::app
