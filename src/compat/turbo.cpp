// Turbo signed stream names (see turbo.hpp).
#include "compat/turbo.hpp"

namespace campfire::compat::turbo {

std::string signed_stream_name(const Secrets& secrets, std::span<const std::string_view> streamables) {
  std::string name;
  for (size_t i = 0; i < streamables.size(); ++i) {
    if (i > 0) name += ':';
    name += streamables[i];
  }
  return secrets.turbo_stream_verifier().generate(json::Value(std::move(name)));
}

std::optional<std::string> verified_stream_name(const Secrets& secrets, std::string_view signed_name) {
  auto value = secrets.turbo_stream_verifier().verify(signed_name, std::nullopt, Timestamp{});
  if (!value) return std::nullopt;
  if (const std::string* s = value->get_string()) return *s;
  if (value->is_number()) return json::generate(*value);
  return std::nullopt;
}

}  // namespace campfire::compat::turbo
