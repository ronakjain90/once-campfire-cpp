// Failure reasons for signed and encrypted messages (Rust: crates/rails_compat/src/lib.rs).
#pragma once

namespace campfire::compat {

enum class Error {
  // Malformed message or bad signature (Ruby's :invalid_message_format).
  InvalidSignature,
  // Authentic, but the payload does not deserialize (:invalid_message_serialization).
  InvalidMessage,
  Expired,
  // Wrong purpose, or a purpose was expected and the message carries no metadata.
  PurposeMismatch,
};

// ActiveSupport::Messages::Rotator falls back to the next rotation only on format and
// serialization errors. An expired or mismatched message stops at the first verifier.
constexpr bool rotates(Error e) { return e == Error::InvalidSignature || e == Error::InvalidMessage; }

}  // namespace campfire::compat
