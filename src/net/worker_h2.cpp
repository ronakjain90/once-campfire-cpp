// The HTTP/2 session: nghttp2 frames, streams and their responses. Rust: crates/kit/src/front/conn.rs
// (hyper's HTTP/2 server), Thruster: internal/handler.go.
#include <algorithm>
#include <cstring>

#include "core/log.hpp"
#include "net/front.hpp"
#include "net/front/static_files.hpp"
#include "net/h2.hpp"
#include "net/worker.hpp"

namespace campfire::net {

namespace {

struct CallbacksDeleter {
  void operator()(nghttp2_session_callbacks* p) const noexcept { nghttp2_session_callbacks_del(p); }
};

// Go's http2 server allows 250 concurrent streams; hyper uses 100 by default. The Rust port sets
// no limit of its own, so the limit is ours.
constexpr std::int32_t kMaxConcurrentStreams = 250;
// The size of one read from the socket (the same step as the HTTP/1 connection).
constexpr std::size_t kReadStep = 4096;
// The flow control window of a connection and of a stream (the HTTP/2 default is 64 KiB).
constexpr std::int32_t kInitialWindow = 1 << 20;

// The headers that HTTP/2 forbids (RFC 9113 section 8.2.2). Hyper drops them.
bool forbidden_field(std::string_view name) {
  return name == "connection" || name == "keep-alive" || name == "proxy-connection" || name == "transfer-encoding" ||
         name == "upgrade";
}

std::string lower(std::string_view text) {
  std::string result(text);
  for (char& c : result) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return result;
}

// `Request.path` and `Request.query` of the `:path` pseudo-header.
void split_target(std::string_view target, std::string_view& path, std::string_view& query) {
  const std::size_t mark = target.find('?');
  if (mark == std::string_view::npos) {
    path = target;
    query = {};
  } else {
    path = target.substr(0, mark);
    query = target.substr(mark + 1);
  }
}

// A response with no body: its size on the wire is zero, whatever the headers say.
bool bodiless(const Response& response) {
  const int status = response.status;
  return status == 204 || status == 304 || (status >= 100 && status < 200);
}

}  // namespace

H2Session::H2Session(Worker& worker, Conn& conn) : worker_(worker), conn_(&conn) {
  nghttp2_session_callbacks* raw = nullptr;
  if (nghttp2_session_callbacks_new(&raw) != 0 || raw == nullptr) {
    broken_ = true;
    return;
  }
  callbacks_ = raw;
  nghttp2_session_callbacks_set_on_begin_headers_callback(
      callbacks_, [](nghttp2_session*, const nghttp2_frame* frame, void* data) -> int {
        return static_cast<H2Session*>(data)->on_begin_headers(*frame);
      });
  nghttp2_session_callbacks_set_on_header_callback(
      callbacks_,
      [](nghttp2_session*, const nghttp2_frame*, const std::uint8_t* name, std::size_t name_len,
         const std::uint8_t* value, std::size_t value_len, std::uint8_t flags, void* data) -> int {
        return static_cast<H2Session*>(data)->on_header(name, name_len, value, value_len, flags);
      });
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
      callbacks_,
      [](nghttp2_session*, std::uint8_t, std::int32_t id, const std::uint8_t* data, std::size_t length,
         void* user_data) -> int { return static_cast<H2Session*>(user_data)->on_data_chunk(id, data, length); });
  nghttp2_session_callbacks_set_on_frame_recv_callback(
      callbacks_, [](nghttp2_session*, const nghttp2_frame* frame, void* data) -> int {
        return static_cast<H2Session*>(data)->on_frame_recv(*frame);
      });
  nghttp2_session_callbacks_set_on_stream_close_callback(
      callbacks_, [](nghttp2_session*, std::int32_t id, std::uint32_t, void* data) -> int {
        return static_cast<H2Session*>(data)->on_stream_close(id);
      });
  nghttp2_session_callbacks_set_on_invalid_frame_recv_callback(
      callbacks_, [](nghttp2_session*, const nghttp2_frame*, int, void*) -> int { return 0; });

  const nghttp2_settings_entry settings[] = {
      {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, kMaxConcurrentStreams},
      {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, kInitialWindow},
  };
  nghttp2_session* session = nullptr;
  if (nghttp2_session_server_new(&session, callbacks_, this) != 0) {
    broken_ = true;
    return;
  }
  session_ = session;
  queue_settings(settings, sizeof settings / sizeof settings[0]);
  std::size_t wrote = 0;
  flush(wrote);
}

H2Session::~H2Session() {
  if (session_ != nullptr) nghttp2_session_del(session_);
  if (callbacks_ != nullptr) nghttp2_session_callbacks_del(callbacks_);
}

void H2Session::queue_settings(const nghttp2_settings_entry* settings, std::size_t count) {
  if (session_ == nullptr) return;
  [[maybe_unused]] const int sent = nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, settings, count);
}

ssize_t H2Session::receive(std::string_view bytes) {
  if (broken_ || session_ == nullptr) return -1;
  const ssize_t used =
      nghttp2_session_mem_recv(session_, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
  if (used < 0) {
    on_error("mem_recv", static_cast<int>(used));
    return -1;
  }
  return used;
}

bool H2Session::flush(std::size_t& wrote) {
  wrote = 0;
  if (session_ == nullptr) return false;
  // nghttp2 1.64 hands out the bytes itself (`mem_send2`), so no send callback is needed.
  while (true) {
    const std::uint8_t* frame = nullptr;
    const ssize_t n = nghttp2_session_mem_send2(session_, &frame);
    if (n < 0) {
      on_error("mem_send", static_cast<int>(n));
      return false;
    }
    if (n == 0) break;
    out_.append(reinterpret_cast<const char*>(frame), static_cast<std::size_t>(n));
  }
  while (limit_ < out_.size()) {
    std::size_t put = 0;
    if (worker_.io_write(*conn_, out_.data() + limit_, out_.size() - limit_, put) != Worker::Io::Ok) return false;
    if (put == 0) break;
    limit_ += put;
    wrote += put;
  }
  if (limit_ == out_.size()) {
    out_.clear();
    limit_ = 0;
  } else if (limit_ >= (1u << 20)) {
    // The socket takes little: the sent bytes go away, so the buffer does not grow without end.
    out_.erase(0, limit_);
    limit_ = 0;
  }
  return true;
}

void H2Session::on_error(const char* what, int code) {
  log_debug("http: HTTP/2 {} error: {}", what, code);
  broken_ = true;
}

bool H2Session::idle() const noexcept {
  for (const auto& item : streams_) {
    if (!item.second->answered) return false;
  }
  return true;
}

H2Stream* H2Session::find(std::int32_t id) noexcept {
  const auto it = streams_.find(id);
  return it == streams_.end() ? nullptr : it->second.get();
}

std::vector<H2Stream*> H2Session::pending() {
  std::vector<H2Stream*> list;
  for (const auto& item : streams_) {
    if (!item.second->answered && !item.second->reset) list.push_back(item.second.get());
  }
  return list;
}

H2Stream* H2Session::open_stream(std::int32_t id) {
  auto& slot = streams_[id];
  if (!slot) {
    slot = std::make_unique<H2Stream>();
    slot->id = id;
    slot->started_ms = worker_.now_ms_public();
    slot->arena = worker_.take_arena_public();
  }
  return slot.get();
}

int H2Session::on_begin_headers(const nghttp2_frame& frame) {
  if (frame.hd.type != NGHTTP2_HEADERS || frame.headers.cat != NGHTTP2_HCAT_REQUEST) return 0;
  last_stream_ = frame.hd.stream_id;
  [[maybe_unused]] H2Stream* stream = open_stream(frame.hd.stream_id);
  return 0;
}

int H2Session::on_header(const std::uint8_t* name, std::size_t name_len, const std::uint8_t* value,
                         std::size_t value_len, std::uint8_t flags) {
  (void)flags;  // NGHTTP2_NV_FLAG_*: it says how to forward the field, not that the head ended
  // nghttp2 gives one complete name/value pair for each call (CONTINUATIONs are transparent),
  // and it calls on_frame_recv after the last pair of the frame.
  H2Stream* stream = find(last_stream_);
  if (stream != nullptr) {
    stream->fields.emplace_back(std::string(reinterpret_cast<const char*>(name), name_len),
                                std::string(reinterpret_cast<const char*>(value), value_len));
  }
  return 0;
}

int H2Session::on_data_chunk(std::int32_t id, const std::uint8_t* bytes, std::size_t length) {
  H2Stream* stream = find(id);
  if (stream == nullptr || length == 0) return 0;
  // nghttp2 gives the DATA of a frame without its padding. A body that is too large is not kept: the
  // peer may send DATA until it stops, and the answer (413) goes out at the end of the stream.
  if (!stream->too_large) {
    stream->body.append(reinterpret_cast<const char*>(bytes), length);
    const ServerOptions& options = worker_.options();
    if (stream->body.size() > options.max_buffered_body ||
        (options.max_request_body != 0 && stream->body.size() > options.max_request_body)) {
      stream->too_large = true;
      std::string().swap(stream->body);
    }
  }
  // The window opens again, so the peer may send the next frame of a long body.
  [[maybe_unused]] const int consumed = nghttp2_session_consume(session_, id, length);
  return 0;
}

int H2Session::on_frame_recv(const nghttp2_frame& frame) {
  if (frame.hd.type == NGHTTP2_DATA) {
    H2Stream* stream = find(frame.hd.stream_id);
    if (stream != nullptr && (frame.hd.flags & NGHTTP2_FLAG_END_STREAM) != 0 && !stream->dispatched) {
      stream->ended = true;
      begin_request(*stream);
    }
    return 0;
  }
  if (frame.hd.type == NGHTTP2_HEADERS && frame.headers.cat == NGHTTP2_HCAT_REQUEST) {
    H2Stream* stream = find(frame.hd.stream_id);
    if (stream == nullptr || stream->dispatched) return 0;
    if ((frame.hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
      stream->ended = true;
      begin_request(*stream);
    }
    return 0;
  }
  if (frame.hd.type == NGHTTP2_RST_STREAM) {
    H2Stream* stream = find(frame.hd.stream_id);
    // The peer gave up on this stream. A handler that runs keeps its frame until it ends.
    if (stream != nullptr) stream->reset = true;
  }
  return 0;
}

int H2Session::on_stream_close(std::int32_t id) {
  const auto it = streams_.find(id);
  if (it == streams_.end()) return 0;
  if (it->second->handler_active || it->second->in_start) return 0;  // the handler keeps its frame
  release(*it->second);
  streams_.erase(it);
  return 0;
}

void H2Session::release(H2Stream& stream) {
  if (stream.arena) worker_.give_arena_public(std::move(stream.arena));
  stream.response.reset();
  stream.ctx.reset();
  stream.handler = nullptr;
  stream.task = Task<void>{};
}

void H2Session::begin_request(H2Stream& stream) {
  if (stream.dispatched) return;
  stream.dispatched = true;
  Arena& arena = *stream.arena;
  std::string authority;
  std::vector<std::pair<std::string_view, std::string_view>> regular;
  for (const auto& item : stream.fields) {
    const std::string_view name(item.first);
    const std::string_view value(item.second);
    if (!name.empty() && name.front() == ':') {
      if (name == ":method") {
        stream.method_text = value;
      } else if (name == ":path") {
        stream.target = value;
      } else if (name == ":authority") {
        authority = value;
      } else if (name != ":scheme") {
        stream.refused = true;  // an unknown pseudo-header
      }
      continue;
    }
    regular.emplace_back(name, value);
  }
  if (stream.too_large) {
    answer_error(stream, 413);
    return;
  }
  if (stream.refused || stream.method_text.empty() || stream.target.empty()) {
    answer_error(stream, 400);
    return;
  }
  // The header list lives in the arena: the views of the request stay valid until the response.
  auto* items = static_cast<Header*>(arena.allocate((regular.size() + 1) * sizeof(Header), alignof(Header)));
  std::size_t count = 0;
  bool have_host = false;
  for (const auto& [name, value] : regular) {
    if (name == "host") have_host = true;
    items[count++] = {arena.copy(lower(name)), arena.copy(value)};
  }
  if (!have_host && !authority.empty()) items[count++] = {"host", arena.copy(authority)};
  stream.headers = std::span<const Header>(items, count);

  Request& request = stream.request;
  request.method_text = arena.copy(stream.method_text);
  request.method = parse_method(request.method_text);
  request.target = arena.copy(stream.target);
  split_target(request.target, request.path, request.query);
  request.minor_version = 1;
  request.keep_alive = true;
  request.headers = stream.headers;
  request.body = arena.copy(stream.body);
  request.via_front = conn_->via_front;
  request.remote_ip = std::string_view(conn_->remote, conn_->remote_size);
  worker_.h2_begin_request(*conn_, stream);
}

void H2Session::answer_error(H2Stream& stream, int status) {
  Response response(stream.arena->resource(), status);
  response.add("content-type", "text/plain; charset=utf-8");
  response.add("content-length", "0");
  char buffer[32];
  response.add_copy("date", http_date_now(buffer));
  answer(stream, std::move(response));
}

void H2Session::answer(H2Stream& stream, Response&& response) {
  if (stream.answered) return;
  stream.answered = true;
  stream.handler_active = false;
  if (conn_->via_front && worker_.options().front) {
    worker_.options().front->finish(stream.front_state, stream.request, response);  // adds the date last
  } else {
    suppress_bodiless_headers(response);
    if (!response.has("date")) {
      char buffer[32];
      response.add_copy("date", http_date_now(buffer));
    }
  }
  if (session_ == nullptr) return;
  const std::size_t body_size = bodiless(response) ? 0 : response.body_size();
  const bool no_body = stream.request.method == Method::Head || body_size == 0;
  const std::string status = std::to_string(response.status);
  const std::string length = std::to_string(body_size);

  // The names live until `nghttp2_submit_response` returns; nghttp2 copies every field.
  std::vector<std::string> names;
  std::vector<std::string_view> values;
  names.reserve(response.headers.size() + 1);
  values.reserve(response.headers.size() + 1);
  // hyper keeps the "content-length" of the app where it is, and appends one for a body of known size.
  bool has_length = false;
  for (const Header& h : response.headers) {
    if (forbidden_field(h.name)) continue;
    names.push_back(lower(h.name));
    // The length of a GET answer is the size of the body that goes out (nghttp2 checks it).
    const bool is_length = iequals(h.name, "content-length");
    values.push_back(is_length && !no_body ? std::string_view(length) : h.value);
    if (is_length) has_length = true;
  }
  // A body with a known size (even an empty one) gets a length, except for HEAD.
  const bool streamed = response.chunked || response.framed;
  if (!has_length && !streamed && stream.request.method != Method::Head && !bodiless(response)) {
    names.emplace_back("content-length");
    values.emplace_back(length);
  }
  std::vector<nghttp2_nv> fields;
  fields.reserve(names.size() + 1);
  static constexpr std::uint8_t kStatusName[] = {':', 's', 't', 'a', 't', 'u', 's'};
  // `nghttp2_nv`: name, value, the two lengths, then the flags.
  fields.push_back({const_cast<std::uint8_t*>(kStatusName),
                    reinterpret_cast<std::uint8_t*>(const_cast<char*>(status.data())), sizeof kStatusName,
                    status.size(), NGHTTP2_NV_FLAG_NONE});
  for (std::size_t i = 0; i < names.size(); ++i) {
    fields.push_back({reinterpret_cast<std::uint8_t*>(names[i].data()),
                      reinterpret_cast<std::uint8_t*>(const_cast<char*>(values[i].data())), names[i].size(),
                      values[i].size(), NGHTTP2_NV_FLAG_NONE});
  }
  // The body of the response is one buffer; nghttp2 splits it into frames as it sends.
  stream.send_body.clear();
  if (!no_body) {
    response.body_append_to(stream.send_body);
    stream.send_at = 0;
  }
  nghttp2_data_provider provider{};
  provider.source.ptr = &stream;
  provider.read_callback = [](nghttp2_session*, std::int32_t, std::uint8_t* data, std::size_t length,
                              std::uint32_t* data_flags, nghttp2_data_source* source, void*) -> ssize_t {
    return H2Session::read_data(*static_cast<H2Stream*>(source->ptr), data, length, data_flags);
  };
  [[maybe_unused]] const int sent =
      nghttp2_submit_response(session_, stream.id, fields.data(), fields.size(), no_body ? nullptr : &provider);
  std::size_t wrote = 0;
  if (!flush(wrote)) broken_ = true;
}

ssize_t H2Session::read_data(H2Stream& stream, std::uint8_t* data, std::size_t length, std::uint32_t* data_flags) {
  const std::size_t left = stream.send_body.size() - stream.send_at;
  const std::size_t take = std::min(left, length);
  if (take != 0) std::memcpy(data, stream.send_body.data() + stream.send_at, take);
  stream.send_at += take;
  if (stream.send_at == stream.send_body.size()) *data_flags |= NGHTTP2_DATA_FLAG_EOF;
  return static_cast<ssize_t>(take);
}

void H2Session::goaway() {
  if (session_ != nullptr) {
    [[maybe_unused]] const int sent =
        nghttp2_submit_goaway(session_, NGHTTP2_FLAG_NONE, last_stream_, NGHTTP2_NO_ERROR, nullptr, 0);
  }
  std::size_t wrote = 0;
  flush(wrote);
}

// The connection speaks HTTP/2 from now on. nghttp2 handles the client preface itself, so the
// bytes already read (the preface, for a cleartext client) go to the session.
void Worker::h2_start(Conn& c) {
  c.h2 = std::make_unique<H2Session>(*this, c);
  if (c.h2->broken()) {
    close_conn(c);
    return;
  }
  c.state = ConnState::Http2;
  c.scanned = 0;
  if (!c.rbuf.empty()) {
    const ssize_t used = c.h2->receive(std::string_view(c.rbuf.data(), c.rbuf.size()));
    if (used < 0) {
      close_conn(c);
      return;
    }
    c.rbuf.consume(static_cast<std::size_t>(used));
  }
  std::size_t wrote = 0;
  c.h2->flush(wrote);
  arm(c, options_.idle_timeout_ms, now_ms_, kTimerH2);
}

bool Worker::h2_step(Conn& c) {
  H2Session& session = *c.h2;
  bool progress = false;
  // The responses that the handlers finished go out first, so a connection with no more input
  // still writes them.
  for (H2Stream* stream : session.pending()) {
    if (!stream->response.has_value()) continue;
    stream->task = Task<void>{};  // the coroutine is at its last suspension point
    session.answer(*stream, std::move(*stream->response));
    progress = true;
  }
  bool closed = false;
  if (c.readable) {
    c.rbuf.reserve(c.rbuf.size() + kReadStep);
    const std::size_t room = c.rbuf.room();
    std::size_t got = 0;
    const Io result = io_read(c, c.rbuf.tail(), room, got);
    if (got != 0) {
      c.rbuf.commit(got);
      if (result == Io::Ok && got < room && !c.ssl) c.readable = false;  // TLS: a record is not all the bytes
      progress = true;
    } else if (result == Io::WouldBlock) {
      c.readable = false;
    } else {
      closed = true;  // the peer ended: the bytes still in `rbuf` go to nghttp2 first
    }
  }
  if (!c.rbuf.empty()) {
    log_debug("h2: feed {} bytes", c.rbuf.size());
    const ssize_t used = session.receive(std::string_view(c.rbuf.data(), c.rbuf.size()));
    if (used < 0) {
      session.goaway();
      close_conn(c);
      return false;
    }
    c.rbuf.consume(static_cast<std::size_t>(used));
    progress = true;
  }
  std::size_t wrote = 0;
  if (!session.flush(wrote)) {
    close_conn(c);
    return false;
  }
  if (wrote != 0) progress = true;
  if (closed && c.rbuf.empty()) {
    close_conn(c);
    return false;
  }
  // Rust's `Activity`: the idle timer runs while nothing is in flight, from the end of the last
  // response. A request in flight is bounded by HTTP_WRITE_TIMEOUT instead.
  if (session.idle()) {
    arm(c, options_.idle_timeout_ms, now_ms_, kTimerH2);
  }
  return progress;
}

void Worker::h2_on_timer(Conn& c) {
  H2Session& session = *c.h2;
  if (!session.idle()) {
    // HTTP_WRITE_TIMEOUT: Rust turns the deadline into a connection error.
    log_debug("http: HTTP/2 write timeout remote={}", c.remote);
    close_conn(c);
    return;
  }
  // HTTP_IDLE_TIMEOUT: a graceful shutdown sends a GOAWAY and then ends the connection.
  session.goaway();
  close_conn(c);
}

void Worker::h2_begin_request(Conn& c, H2Stream& stream) {
  Request& request = stream.request;
  stream.ctx.emplace(*this, *stream.arena, request);
  stream.handler = app_.not_found;
  if (app_.routes != nullptr) {
    const Match match = match_route(*app_.routes, request.method, request.path);
    if (match) {
      stream.ctx->params = match.params;
      stream.handler = match.route->handler;
    }
  }
  // A request in flight: the idle timer of the connection gives way to the write timeout.
  arm_earlier(c, stream.started_ms + static_cast<std::uint64_t>(std::max<std::int64_t>(options_.write_timeout_ms, 0)),
              kTimerWrite);
  if (c.via_front && options_.front) {
    options_.front->begin(stream.front_state, request);
    if (stream.front_state.status == front::CacheStatus::Hit) {
      stream.response.emplace(options_.front->hit_response(stream.front_state, request, stream.arena->resource()));
      stream.handler_active = false;
      return;
    }
    options_.front->proxied(request, *stream.arena, c.tls);
  }
  if (options_.serve_static) {
    if (std::optional<Response> served = front::serve_static(*stream.ctx)) {
      if (options_.after_static) options_.after_static(*stream.ctx, *served);
      stream.response.emplace(std::move(*served));
      stream.handler_active = false;
      return;
    }
  }
  stream.handler_active = true;
  stream.in_start = true;
  stream.task = h2_serve(c, stream);
  stream.task.start();
  stream.in_start = false;
}

Task<void> Worker::h2_serve(Conn& c, H2Stream& stream) {
  std::optional<Response> response;
  try {
    if (stream.handler != nullptr) {
      response.emplace(co_await stream.handler(*stream.ctx));
    } else {
      response.emplace(stream.ctx->response(404));
    }
  } catch (const std::exception& error) {
    log_error("handler for {} {} failed: {}", stream.request.method_text, stream.request.path, error.what());
  } catch (...) {
    log_error("handler for {} {} failed", stream.request.method_text, stream.request.path);
  }
  if (!response) {
    response.emplace(stream.ctx->response(500));
    response->add("content-type", "text/html; charset=UTF-8");
    response->add("content-length", "0");
  }
  h2_handler_finished(c, stream, std::move(*response));
}

void Worker::h2_handler_finished(Conn& c, H2Stream& stream, Response&& response) {
  stream.response.emplace(std::move(response));
  stream.handler_active = false;
  if (!stream.in_start) post_finished(c);
}

void Worker::arm_earlier(Conn& c, std::uint64_t due_ms, TimerKind kind) {
  const std::uint64_t tick = due_ms / TimerWheel::kTickMs;
  if (c.timer.armed && c.timer.kind == kind && c.timer.expire_tick <= tick) return;
  arm(c, due_ms > now_ms_ ? static_cast<std::int64_t>(due_ms - now_ms_) : 1, now_ms_, kind);
}

}  // namespace campfire::net