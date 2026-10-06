// Errors of Active Storage (Rust: crates/storage/src/lib.rs Error). All use core Result.
#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "core/error.hpp"

namespace campfire::storage {

// Each failure has a fixed message prefix, so a caller can test the kind with the predicates.
inline constexpr std::string_view kFileNotFound = "file not found";
inline constexpr std::string_view kIntegrity = "checksum mismatch";
inline constexpr std::string_view kInvalidVariation = "invalid variation: ";
inline constexpr std::string_view kInvariable = "can't transform blob with content_type=";
inline constexpr std::string_view kUnpreviewable = "no previewer found for content_type=";
inline constexpr std::string_view kUnrepresentable = "no previewer found and can't transform blob with content_type=";
inline constexpr std::string_view kVips = "libvips: ";
inline constexpr std::string_view kAnalyze = "analysis failed: ";

inline std::unexpected<Error> file_not_found() {
  return fail(Errc::NotFound, std::string(kFileNotFound));
}
inline std::unexpected<Error> integrity_error() {
  return fail(Errc::Internal, std::string(kIntegrity));
}
inline std::unexpected<Error> io_error(std::string message) {
  return fail(Errc::Io, std::move(message));
}
inline std::unexpected<Error> prefixed(Errc code, std::string_view prefix, std::string_view detail) {
  return fail(code, std::string(prefix) + std::string(detail));
}

inline bool is_file_not_found(const Error& e) {
  return e.code == Errc::NotFound && e.message == kFileNotFound;
}
inline bool is_integrity(const Error& e) {
  return e.message == kIntegrity;
}
inline bool starts_with_prefix(const Error& e, std::string_view prefix) {
  return e.message.starts_with(prefix);
}

}  // namespace campfire::storage
