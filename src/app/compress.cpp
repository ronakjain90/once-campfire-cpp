// Gzip with libdeflate. Rails: Rack::Deflater. Rust: crates/kit/src/deflater.rs.
#include "app/compress.hpp"

#include <libdeflate.h>

#include <memory>

namespace campfire::app {

namespace {
struct Deleter {
  void operator()(libdeflate_compressor* c) const noexcept { libdeflate_free_compressor(c); }
};

std::string gzip_with(libdeflate_compressor* compressor, std::string_view bytes) {
  std::string out(libdeflate_gzip_compress_bound(compressor, bytes.size()), '\0');
  const std::size_t n = libdeflate_gzip_compress(compressor, bytes.data(), bytes.size(), out.data(), out.size());
  out.resize(n);
  return out;
}

}  // namespace

std::string gzip_compress(std::string_view bytes) {
  // A compressor is not thread-safe, so each thread keeps its own.
  thread_local std::unique_ptr<libdeflate_compressor, Deleter> compressor(libdeflate_alloc_compressor(6));
  return gzip_with(compressor.get(), bytes);
}

std::string gzip_compress_fast(std::string_view bytes) {
  thread_local std::unique_ptr<libdeflate_compressor, Deleter> compressor(libdeflate_alloc_compressor(1));
  return gzip_with(compressor.get(), bytes);
}

}  // namespace campfire::app
