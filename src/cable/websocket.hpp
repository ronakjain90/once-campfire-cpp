// WebSocket (RFC 6455) and permessage-deflate (RFC 7692) for Action Cable.
// Rust: crates/cable/src/socket.rs. Rails: websocket-driver (Hybi) close codes.
// This file has no socket code. A transport gives bytes in and takes bytes out.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace campfire::cable::ws {

// A buffer that the transport sends. Many connections share one buffer for a broadcast.
using Bytes = std::shared_ptr<const std::string>;

// The longest message that a client may send. websocket-driver allows 64 MiB. Action Cable
// commands are small, so the Rust port and this port use 1 MiB.
inline constexpr std::size_t kMaxMessage = std::size_t{1} << 20;
// A text frame that is shorter than this goes out uncompressed, also when deflate is on.
inline constexpr std::size_t kMinCompressed = 256;

enum class Opcode : std::uint8_t { Continuation = 0x0, Text = 0x1, Binary = 0x2, Close = 0x8, Ping = 0x9, Pong = 0xA };

// Close codes that websocket-driver uses for a client error (WebSocket::Driver::Hybi::ERRORS).
inline constexpr std::uint16_t kCloseNormal = 1000;
inline constexpr std::uint16_t kCloseProtocolError = 1002;
inline constexpr std::uint16_t kCloseUnacceptable = 1003;   // an unmasked frame
inline constexpr std::uint16_t kCloseEncodingError = 1007;  // a text message that is not UTF-8
inline constexpr std::uint16_t kCloseTooLarge = 1009;

// ---- Opening handshake -------------------------------------------------------------------------

// The header values of the client request that the handshake reads. An empty view means that the
// header is absent. The caller already checked the method, "Connection: upgrade" and
// "Upgrade: websocket" (see is_upgrade_request).
struct HandshakeRequest {
  std::string_view version;                      // Sec-WebSocket-Version
  std::string_view key;                          // Sec-WebSocket-Key
  std::span<const std::string_view> extensions;  // each Sec-WebSocket-Extensions header value
  std::span<const std::string_view> protocols;   // each Sec-WebSocket-Protocol header value
};

struct HandshakeResponse {
  std::string accept;         // Sec-WebSocket-Accept
  bool deflate = false;       // permessage-deflate accepted (no context takeover)
  std::string_view protocol;  // chosen Sec-WebSocket-Protocol, empty if none

  // The 101 response, with the status line, the headers and the empty line. The header names
  // are lower case, as the Rust port sends them.
  [[nodiscard]] std::string response_head() const;
};

// The value of the Sec-WebSocket-Extensions header in the response, if deflate is on.
inline constexpr std::string_view kDeflateResponse =
    "permessage-deflate; server_no_context_takeover; client_no_context_takeover";

// WebSocket::Driver.websocket?: a GET with "Connection: upgrade" and "Upgrade: websocket".
// `connection` holds each Connection header value.
[[nodiscard]] bool is_upgrade_request(std::string_view method, std::span<const std::string_view> connection,
                                      std::string_view upgrade);

// Checks the key and the version, and chooses the extension and the sub-protocol. Returns nothing
// if the handshake is not valid. Sec-WebSocket-Accept is the SHA-1 of the key and the GUID.
[[nodiscard]] std::optional<HandshakeResponse> accept_handshake(const HandshakeRequest& request);

// Sec-WebSocket-Protocol values that the server accepts (ActionCable::INTERNAL[:protocols]).
inline constexpr std::array<std::string_view, 2> kProtocols = {"actioncable-v1-json", "actioncable-unsupported"};

// The first protocol in the client list that Action Cable supports (empty if none).
[[nodiscard]] std::string_view negotiate_protocol(std::span<const std::string_view> client_lists);

// ---- Frames --------------------------------------------------------------------------------------

// Writes a final frame header (FIN, RSV1 if `compressed`) and returns its length (2, 4 or 10).
std::size_t encode_header(std::uint8_t out[10], Opcode opcode, bool compressed, std::size_t payload_size);
// A server frame (not masked): header and payload in one string.
[[nodiscard]] std::string encode_frame(Opcode opcode, bool compressed, std::string_view payload);
// A close frame with a code and no reason.
[[nodiscard]] std::string encode_close(std::uint16_t code);
// A close frame that echoes the client code (empty payload if the client sent none).
[[nodiscard]] std::string encode_close_reply(std::optional<std::uint16_t> code);

// Raw deflate, level 6, sync flush, without the 4 byte tail (RFC 7692 section 7.2.1).
[[nodiscard]] std::string deflate_message(std::string_view input);
// The reverse. The error is a close code: 1002 for invalid data, 1009 if the result is too large.
[[nodiscard]] std::expected<std::string, std::uint16_t> inflate_message(std::string_view input,
                                                                        std::size_t max_size = kMaxMessage);

// Masks or unmasks in place (RFC 6455 section 5.3).
void apply_mask(std::uint8_t* data, std::size_t size, const std::uint8_t mask[4]);

// ---- Parser --------------------------------------------------------------------------------------

struct Message {
  enum class Type : std::uint8_t { Text, Binary, Ping, Pong, Close };
  Type type = Type::Text;
  std::string data;                         // text or binary payload, ping or pong payload
  std::optional<std::uint16_t> close_code;  // Close only
};

struct ParserOptions {
  bool deflate = false;      // RSV1 is valid when permessage-deflate is on
  bool require_mask = true;  // true for a server (client frames must be masked)
  std::size_t max_message = kMaxMessage;
};

// Reads frames from a byte stream in any chunking. It reassembles fragments, unmasks and inflates.
// The checks run in the order of websocket-driver, which decides the close code if a frame is wrong
// in more than one way. After an error, every call returns the same error.
class FrameParser {
 public:
  explicit FrameParser(ParserOptions options = {}) : options_(options) {}

  void feed(std::span<const std::uint8_t> bytes);
  void feed(std::string_view bytes);

  // The next message: a value, nothing (more bytes needed) or a close code for a protocol error.
  [[nodiscard]] std::expected<std::optional<Message>, std::uint16_t> next();

  // Bytes that are buffered and not yet used.
  [[nodiscard]] std::size_t buffered() const { return buf_.size() - pos_; }

 private:
  struct Header {
    bool fin = false;
    bool compressed = false;
    std::uint8_t opcode = 0;
    std::uint64_t length = 0;
    std::uint8_t mask[4] = {0, 0, 0, 0};
    bool masked = false;
    std::size_t header_size = 0;
  };
  struct Partial {
    std::uint8_t opcode = 0;
    bool compressed = false;
    std::string data;
  };

  std::expected<bool, std::uint16_t> parse_header();
  std::expected<std::optional<Message>, std::uint16_t> finish(Header& header, std::string payload);
  std::unexpected<std::uint16_t> fail(std::uint16_t code);

  ParserOptions options_;
  std::string buf_;
  std::size_t pos_ = 0;
  std::optional<Header> header_;
  std::optional<Partial> partial_;
  std::optional<std::uint16_t> error_;
};

}  // namespace campfire::cable::ws
