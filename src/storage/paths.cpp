// See paths.hpp.
#include "storage/paths.hpp"

#include "compat/json.hpp"
#include "compat/ruby.hpp"
#include "storage/disposition.hpp"

namespace campfire::storage::paths {

namespace {

std::string blob_path(std::string_view kind, const compat::MessageVerifier& verifier, const Blob& blob,
                      std::optional<std::string_view> disposition) {
  std::string path = std::string(kPrefix) + "/blobs/" + std::string(kind) + "/" +
                     escape_segment(signed_blob_id(verifier, blob.id)) + "/" + escape_path(blob.filename.sanitized());
  // Hash#to_query escapes with CGI.escape.
  if (disposition) path += "?disposition=" + compat::cgi_escape(*disposition);
  return path;
}

std::string representation_path(std::string_view kind, const compat::MessageVerifier& verifier, const Blob& blob,
                                const compat::Variation& variation) {
  return std::string(kPrefix) + "/representations/" + std::string(kind) + "/" +
         escape_segment(signed_blob_id(verifier, blob.id)) + "/" + escape_segment(variation.key(verifier)) + "/" +
         escape_path(blob.filename.sanitized());
}

}  // namespace

std::string signed_blob_id(const compat::MessageVerifier& verifier, int64_t blob_id,
                           std::optional<compat::Timestamp> expires_at) {
  return verifier.generate_raw(std::to_string(blob_id), "blob_id", expires_at);
}

std::optional<int64_t> verify_signed_blob_id(const compat::MessageVerifier& verifier, std::string_view signed_id,
                                             compat::Timestamp now) {
  auto text = verifier.verify_raw(signed_id, "blob_id", now);
  if (!text) return std::nullopt;
  auto value = compat::json::parse(*text);
  if (!value) return std::nullopt;
  return value->to_int64();
}

std::string blob_redirect_path(const compat::MessageVerifier& verifier, const Blob& blob,
                               std::optional<std::string_view> disposition) {
  return blob_path("redirect", verifier, blob, disposition);
}

std::string blob_proxy_path(const compat::MessageVerifier& verifier, const Blob& blob,
                            std::optional<std::string_view> disposition) {
  return blob_path("proxy", verifier, blob, disposition);
}

std::string representation_redirect_path(const compat::MessageVerifier& verifier, const Blob& blob,
                                         const compat::Variation& variation) {
  return representation_path("redirect", verifier, blob, variation);
}

std::string representation_proxy_path(const compat::MessageVerifier& verifier, const Blob& blob,
                                      const compat::Variation& variation) {
  return representation_path("proxy", verifier, blob, variation);
}

}  // namespace campfire::storage::paths
