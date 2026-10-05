// See key.hpp.
#include "storage/key.hpp"

#include <openssl/evp.h>

#include <array>
#include <fstream>

#include "compat/base64.hpp"
#include "compat/crypto.hpp"
#include "storage/errors.hpp"

namespace campfire::storage {

namespace {
constexpr std::string_view kBase36 = "0123456789abcdefghijklmnopqrstuvwxyz";
}

std::string generate_key() {
  std::string key;
  key.reserve(kKeyLength);
  std::string random;
  while (key.size() < kKeyLength) {
    compat::crypto::random_bytes(random, kKeyLength * 2);
    // 252 = 7 * 36: reject the top 4 byte values so each character is uniform.
    for (unsigned char byte : random) {
      if (byte >= 252) continue;
      key.push_back(kBase36[byte % 36]);
      if (key.size() == kKeyLength) break;
    }
  }
  return key;
}

struct ChecksumBuilder::Impl {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  ~Impl() { EVP_MD_CTX_free(ctx); }
};

ChecksumBuilder::ChecksumBuilder() : impl_(std::make_unique<Impl>()) {
  EVP_DigestInit_ex(impl_->ctx, EVP_md5(), nullptr);
}
ChecksumBuilder::~ChecksumBuilder() = default;

void ChecksumBuilder::update(std::string_view data) { EVP_DigestUpdate(impl_->ctx, data.data(), data.size()); }

std::string ChecksumBuilder::finish() {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned length = 0;
  EVP_DigestFinal_ex(impl_->ctx, digest.data(), &length);
  return compat::base64::strict_encode(std::string_view(reinterpret_cast<const char*>(digest.data()), length));
}

std::string checksum(std::string_view data) {
  ChecksumBuilder builder;
  builder.update(data);
  return builder.finish();
}

Result<std::string> checksum_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return file_not_found();
  ChecksumBuilder builder;
  std::string buffer(1 << 20, '\0');
  while (in) {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    builder.update(std::string_view(buffer.data(), static_cast<size_t>(in.gcount())));
  }
  if (in.bad()) return io_error("read failed: " + path.string());
  return builder.finish();
}

}  // namespace campfire::storage
