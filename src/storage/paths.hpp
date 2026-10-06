// Active Storage route helpers (activestorage/config/routes.rb) and blob signed ids
// (Rust: crates/storage/src/paths.rs). urls_expire_in is unset in Campfire, so signed ids never expire.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "compat/message_verifier.hpp"
#include "compat/variation.hpp"
#include "storage/blob.hpp"

namespace campfire::storage::paths {

inline constexpr std::string_view kPrefix = "/rails/active_storage";

// blob.signed_id: purpose "blob_id" on the Active Storage verifier.
std::string signed_blob_id(const compat::MessageVerifier& verifier, int64_t blob_id,
                           std::optional<compat::Timestamp> expires_at = std::nullopt);
// find_signed's verification step.
std::optional<int64_t> verify_signed_blob_id(const compat::MessageVerifier& verifier, std::string_view signed_id,
                                             compat::Timestamp now);

// rails_blob_path(blob, disposition:): /rails/active_storage/blobs/redirect/:signed_id/*filename.
std::string blob_redirect_path(const compat::MessageVerifier& verifier, const Blob& blob,
                               std::optional<std::string_view> disposition = std::nullopt);
// rails_storage_proxy_path(blob).
std::string blob_proxy_path(const compat::MessageVerifier& verifier, const Blob& blob,
                            std::optional<std::string_view> disposition = std::nullopt);
// url_for(blob.representation(...)): /rails/active_storage/representations/redirect/:signed_blob_id/
// :variation_key/*filename. `blob` is the original blob (the video for a preview).
std::string representation_redirect_path(const compat::MessageVerifier& verifier, const Blob& blob,
                                         const compat::Variation& variation);
std::string representation_proxy_path(const compat::MessageVerifier& verifier, const Blob& blob,
                                      const compat::Variation& variation);

}  // namespace campfire::storage::paths
