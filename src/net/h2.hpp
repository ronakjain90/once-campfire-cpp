// HTTP/2 with nghttp2: the session and its streams. Rust: crates/kit/src/front/conn.rs (hyper's
// HTTP/2 server), Thruster: HTTP/2 over TLS, or cleartext with H2C_ENABLED.
#pragma once

#include <nghttp2/nghttp2.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/arena.hpp"
#include "core/task.hpp"
#include "net/ctx.hpp"
#include "net/front/front.hpp"
#include "net/http.hpp"
#include "net/response.hpp"

namespace campfire::net {

class Worker;
struct Conn;

// The first bytes of a connection that speaks HTTP/2 without a negotiation (the client preface).
inline constexpr std::string_view kH2Preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

// One HTTP/2 stream. Its request runs the same handlers as over HTTP/1, and its response goes back
// as HEADERS and DATA frames.
struct H2Stream {
  std::int32_t id = 0;
  std::uint64_t started_ms = 0;
  bool ended = false;       // END_STREAM seen (with the head, or after the body)
  bool dispatched = false;  // the handler starts, or started
  bool handler_active = false;
  bool in_start = false;
  bool answered = false;   // the response is in
  bool too_large = false;  // the body is over the limit: answer 413
  bool refused = false;    // the head is not a valid HTTP/2 request: answer 400
  bool reset = false;      // the peer gave up on the stream

  // The pseudo-header fields, then the header fields, as nghttp2 gave them.
  std::vector<std::pair<std::string, std::string>> fields;
  std::string method_text;
  std::string target;
  std::string body;       // the DATA frames of the request
  std::string send_body;  // the body of the response, while nghttp2 sends it

  std::unique_ptr<Arena> arena;
  std::span<const Header> headers;  // in the arena, when the head is complete
  Request request;
  std::optional<Ctx> ctx;
  HandlerFn handler = nullptr;
  Task<void> task;
  front::FrontState front_state;
  std::optional<Response> response;
};

// The connection session. nghttp2 makes its frames into `out_`; the worker writes `out_` to the
// socket (plain or TLS).
class H2Session {
 public:
  H2Session(Worker& worker, Conn& conn, bool prior_knowledge);
  ~H2Session();
  H2Session(const H2Session&) = delete;
  H2Session& operator=(const H2Session&) = delete;

  // Gives bytes to nghttp2. False if the session ended with an error.
  [[nodiscard]] bool receive(std::string_view bytes);
  // Makes every frame that nghttp2 has, then writes as many as the socket takes. `wrote` is the
  // number of bytes that reached the socket.
  bool flush(std::size_t& wrote);

  // True while no stream waits for a response (a shutdown closes the connection then).
  [[nodiscard]] bool idle() const noexcept;
  // The stream with this id, or null.
  [[nodiscard]] H2Stream* find(std::int32_t id) noexcept;
  // The streams that wait for their response to go out.
  [[nodiscard]] std::vector<H2Stream*> pending();

  // Sends the response of a stream. A stream that is answered already sends nothing.
  void answer(H2Stream& stream, Response&& response);
  void answer_error(H2Stream& stream, int status);
  // Ends the session: a GOAWAY, then the connection closes.
  void goaway();

  // The nghttp2 callbacks.
  int on_begin_headers(const nghttp2_frame& frame);
  int on_header(const std::uint8_t* name, std::size_t name_len, const std::uint8_t* value, std::size_t value_len,
                std::uint8_t flags);
  int on_data_chunk(std::int32_t id, const std::uint8_t* bytes, std::size_t length);
  int on_frame_recv(const nghttp2_frame& frame);
  int on_stream_close(std::int32_t id);
  ssize_t send_callback(const std::uint8_t* data, std::size_t length);
  ssize_t read_data(std::uint8_t* data, std::size_t length, std::uint32_t* data_flags);

  [[nodiscard]] Conn& conn() const noexcept { return *conn_; }
  [[nodiscard]] bool broken() const noexcept { return broken_; }

 private:
  void queue_settings(const nghttp2_settings_entry* settings, std::size_t count);
  void begin_request(H2Stream& stream);
  void release(H2Stream& stream);
  void on_error(const char* what, int code);
  [[nodiscard]] H2Stream* open_stream(std::int32_t id);

  Worker& worker_;
  Conn* conn_;
  nghttp2_session* session_ = nullptr;
  nghttp2_session_callbacks* callbacks_ = nullptr;
  std::unordered_map<std::int32_t, std::unique_ptr<H2Stream>> streams_;
  std::string out_;  // the frames that nghttp2 made, waiting for the socket
  std::size_t limit_ = 0;
  std::string field_name_;  // the header field that is in parts
  std::string field_value_;
  std::string_view data_view_;  // the body that nghttp2 sends now
  std::size_t data_at_ = 0;
  std::int32_t data_stream_ = 0;
  std::int32_t last_stream_ = 0;
  bool broken_ = false;
};

}  // namespace campfire::net