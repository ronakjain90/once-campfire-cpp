// Gzip with libdeflate. Rails: Rack::Deflater. Rust: crates/kit/src/deflater.rs.
#include "app/compress.hpp"

#include <libdeflate.h>

#include <memory>

namespace campfire::app {

namespace {
struct Deleter {
  void operator()(libdeflate_compressor* c) const noexcept { libdeflate_free_compressor(c); }
};
}  // namespace

std::string gzip_compress(std::string_view bytes) {
  // A compressor is not thread-safe, so each thread keeps its own.
  thread_local std::unique_ptr<libdeflate_compressor, Deleter> compressor(libdeflate_alloc_compressor(6));
  std::string out(libdeflate_gzip_compress_bound(compressor.get(), bytes.size()), '\0');
  const std::size_t n = libdeflate_gzip_compress(compressor.get(), bytes.data(), bytes.size(), out.data(), out.size());
  out.resize(n);
  return out;
}

}  // namespace campfire::app
