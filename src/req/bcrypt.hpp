// has_secure_password (Rails: ActiveModel::SecurePassword, bcrypt gem 3.1.22; Rust: crates/rails_compat bcrypt).
// Both calls block for about 250 ms at cost 12. Callers offload them to a job thread.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace campfire::req::bcrypt {

inline constexpr int kDefaultCost = 12;  // BCrypt::Engine::DEFAULT_COST
inline constexpr int kMinCost = 4;       // BCrypt::Engine::MIN_COST, used by Rails in the test environment

// BCrypt::Password.create(password, cost:): a "$2a$" digest with a random salt. Only the first
// 72 bytes of the password count. The result is empty if the cost is not in the range 4 to 31.
[[nodiscard]] std::string hash_password(std::string_view password, int cost = kDefaultCost);

// BCrypt::Password.new(digest).is_password?(password). A digest that is not valid gives false.
[[nodiscard]] bool verify_password(std::string_view password, std::string_view digest);

}  // namespace campfire::req::bcrypt
