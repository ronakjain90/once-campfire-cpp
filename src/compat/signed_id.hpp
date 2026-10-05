// ActiveRecord::SignedId: signed_id(purpose:, expires_in:) and find_signed (Rust: signed_id.rs).
// The purpose is "<base class name underscored>/<purpose>", e.g. "user/avatar", or just "user".
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "compat/secrets.hpp"
#include "compat/time.hpp"

namespace campfire::compat::signed_id {

// `model_name` is the record's base class name, such as "User" or "Room" (not "Rooms::Open").
std::string generate(const Secrets& secrets, std::string_view model_name, int64_t id,
                     std::optional<std::string_view> purpose, std::optional<Timestamp> expires_at);

// find_signed's verification step: the id to look up, or nullopt. Rejects tampered, expired
// and wrong-purpose values.
std::optional<int64_t> verify(const Secrets& secrets, std::string_view model_name, std::string_view signed_id,
                              std::optional<std::string_view> purpose, Timestamp now);

// combine_signed_id_purposes: [base_class.name.underscore, purpose.to_s].compact_blank.join("/").
std::string combine_purposes(std::string_view model_name, std::optional<std::string_view> purpose);

// Active Storage's blob signed id: the ActiveStorage verifier with purpose "blob_id".
std::string blob_signed_id(const Secrets& secrets, int64_t blob_id, std::optional<Timestamp> expires_at = std::nullopt);
std::optional<int64_t> verify_blob_signed_id(const Secrets& secrets, std::string_view signed_id, Timestamp now);

}  // namespace campfire::compat::signed_id
