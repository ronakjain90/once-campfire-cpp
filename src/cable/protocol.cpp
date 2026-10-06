// Action Cable frames. Rust: crates/cable/src/protocol.rs, naming.rs.
#include "cable/protocol.hpp"

#include <cctype>

#include "compat/json.hpp"

namespace campfire::cable::protocol {

namespace {
std::string enc(std::string_view s) { return compat::json::encode(compat::json::Value(s)); }
}  // namespace

std::string_view to_string(DisconnectReason reason) {
  switch (reason) {
    case DisconnectReason::Unauthorized: return "unauthorized";
    case DisconnectReason::InvalidRequest: return "invalid_request";
    case DisconnectReason::ServerRestart: return "server_restart";
    case DisconnectReason::Remote: return "remote";
  }
  return "";
}

std::string welcome() { return R"({"type":"welcome"})"; }

std::string ping(std::int64_t unix_seconds) {
  return std::string(R"({"type":"ping","message":)") + std::to_string(unix_seconds) + "}";
}

std::string disconnect(std::optional<DisconnectReason> reason, std::string_view reconnect_json) {
  std::string out = R"({"type":"disconnect","reason":)";
  if (reason) {
    out += '"';
    out += to_string(*reason);
    out += '"';
  } else {
    out += "null";
  }
  out += R"(,"reconnect":)";
  out += reconnect_json;
  out += '}';
  return out;
}

std::string confirmation(std::string_view identifier) {
  return R"({"identifier":)" + enc(identifier) + R"(,"type":"confirm_subscription"})";
}

std::string rejection(std::string_view identifier) {
  return R"({"identifier":)" + enc(identifier) + R"(,"type":"reject_subscription"})";
}

std::string message(std::string_view encoded_identifier, std::string_view encoded_message) {
  std::string out = R"({"identifier":)";
  out += encoded_identifier;
  out += R"(,"message":)";
  out += encoded_message;
  out += '}';
  return out;
}

std::string remote_disconnect_payload(bool reconnect) {
  return std::string(R"({"type":"disconnect","reconnect":)") + (reconnect ? "true" : "false") + "}";
}

std::string internal_channel(std::string_view id) { return "action_cable/" + std::string(id); }

namespace {
// ActiveSupport::Inflector.underscore without acronyms (Campfire defines none).
std::string underscore(std::string_view word) {
  std::string in(word);
  std::string out;
  for (std::size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c == ':' && i + 1 < in.size() && in[i + 1] == ':') {
      out += '/';
      ++i;
      continue;
    }
    bool upper = std::isupper(static_cast<unsigned char>(c)) != 0;
    if (upper && i > 0) {
      char prev = in[i - 1];
      bool prev_lower_digit = std::islower(static_cast<unsigned char>(prev)) != 0 || std::isdigit(static_cast<unsigned char>(prev)) != 0;
      bool prev_upper_digit = std::isupper(static_cast<unsigned char>(prev)) != 0 || std::isdigit(static_cast<unsigned char>(prev)) != 0;
      bool next_lower = i + 1 < in.size() && std::islower(static_cast<unsigned char>(in[i + 1])) != 0;
      if (prev_lower_digit || (prev_upper_digit && next_lower)) out += '_';
    }
    out += c == '-' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}
}  // namespace

std::string channel_name(std::string_view class_name) {
  if (class_name.ends_with("Channel")) class_name.remove_suffix(7);
  std::string replaced;
  for (std::size_t i = 0; i < class_name.size(); ++i) {
    if (class_name.substr(i, 2) == "::") {
      replaced += ':';
      ++i;
    } else {
      replaced += class_name[i];
    }
  }
  return underscore(replaced);
}

std::string broadcasting_for(std::string_view class_name, std::span<const std::string_view> parts) {
  std::string out = channel_name(class_name);
  for (auto p : parts) {
    out += ':';
    out += p;
  }
  return out;
}

std::string stream_name_from(std::span<const std::string_view> parts) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) out += ':';
    out += parts[i];
  }
  return out;
}

}  // namespace campfire::cable::protocol
