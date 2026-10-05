// WebSocket codec. Rust: crates/cable/src/socket.rs. See websocket.hpp.
#include "cable/websocket.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "compat/base64.hpp"
#include "compat/crypto.hpp"
#include "compat/json.hpp"

namespace campfire::cable::ws {

namespace {

constexpr std::string_view kAcceptGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::string_view kDeflateTail("\x00\x00\xff\xff", 4);

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) s.remove_suffix(1);
  return s;
}

bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

template <class F>
void for_each_part(std::string_view text, char sep, F&& f) {
  while (true) {
    auto at = text.find(sep);
    f(text.substr(0, at));
    if (at == std::string_view::npos) return;
    text.remove_prefix(at + 1);
  }
}

bool parse_bits(std::string_view v, int& out) {
  if (v.empty() || v.size() > 3) return false;
  int n = 0;
  for (char c : v) {
    if (c < '0' || c > '9') return false;
    n = n * 10 + (c - '0');
  }
  out = n;
  return true;
}

// A permessage-deflate offer that the response kDeflateResponse answers: any parameter except a
// server window that is smaller than 15 bits (browsers do not ask for it).
bool acceptable_deflate_offer(std::string_view offer) {
  bool first = true;
  bool ok = true;
  for_each_part(offer, ';', [&](std::string_view raw) {
    auto param = trim(raw);
    if (first) {
      first = false;
      ok = param == "permessage-deflate";
      return;
    }
    if (!ok) return;
    std::string_view name = param;
    std::optional<std::string_view> value;
    if (auto eq = param.find('='); eq != std::string_view::npos) {
      name = trim(param.substr(0, eq));
      auto v = trim(param.substr(eq + 1));
      while (!v.empty() && v.front() == '"') v.remove_prefix(1);
      while (!v.empty() && v.back() == '"') v.remove_suffix(1);
      value = v;
    }
    if (name == "server_no_context_takeover" || name == "client_no_context_takeover") {
      ok = !value.has_value();
    } else if (name == "client_max_window_bits") {
      int bits = 0;
      ok = !value.has_value() || (parse_bits(*value, bits) && bits >= 8 && bits <= 15);
    } else if (name == "server_max_window_bits") {
      ok = value == std::optional<std::string_view>("15");
    } else {
      ok = false;
    }
  });
  return ok;
}

struct DeflateState {
  z_stream s{};
  DeflateState() { deflateInit2(&s, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY); }
  ~DeflateState() { deflateEnd(&s); }
  DeflateState(const DeflateState&) = delete;
  DeflateState& operator=(const DeflateState&) = delete;
};

struct InflateState {
  z_stream s{};
  InflateState() { inflateInit2(&s, -15); }
  ~InflateState() { inflateEnd(&s); }
  InflateState(const InflateState&) = delete;
  InflateState& operator=(const InflateState&) = delete;
};

// A close frame payload: no payload, or a code that a peer may send and a UTF-8 reason.
std::expected<std::optional<std::uint16_t>, std::uint16_t> close_code(std::string_view payload) {
  if (payload.empty()) return std::optional<std::uint16_t>{};
  if (payload.size() == 1) return std::unexpected(kCloseProtocolError);
  auto code = static_cast<std::uint16_t>((static_cast<unsigned char>(payload[0]) << 8) |
                                         static_cast<unsigned char>(payload[1]));
  bool valid = (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1014) || (code >= 3000 && code <= 4999);
  if (!valid || !compat::json::valid_utf8(payload.substr(2))) return std::unexpected(kCloseProtocolError);
  return std::optional<std::uint16_t>{code};
}

}  // namespace

// ---- Handshake -----------------------------------------------------------------------------------

std::string HandshakeResponse::response_head() const {
  std::string out = "HTTP/1.1 101 Switching Protocols\r\nupgrade: websocket\r\nconnection: upgrade\r\n";
  out += "sec-websocket-accept: " + accept + "\r\n";
  if (deflate) {
    out += "sec-websocket-extensions: ";
    out += kDeflateResponse;
    out += "\r\n";
  }
  if (!protocol.empty()) {
    out += "sec-websocket-protocol: ";
    out += protocol;
    out += "\r\n";
  }
  out += "\r\n";
  return out;
}

bool is_upgrade_request(std::string_view method, std::span<const std::string_view> connection,
                        std::string_view upgrade) {
  if (method != "GET" || !iequals(trim(upgrade), "websocket")) return false;
  bool found = false;
  for (auto value : connection) {
    for_each_part(value, ',', [&](std::string_view token) { found = found || iequals(trim(token), "upgrade"); });
  }
  return found;
}

std::string_view negotiate_protocol(std::span<const std::string_view> client_lists) {
  for (auto list : client_lists) {
    std::string_view found;
    for_each_part(list, ',', [&](std::string_view raw) {
      auto requested = trim(raw);
      if (!found.empty()) return;
      for (auto supported : kProtocols) {
        if (supported == requested) found = supported;
      }
    });
    if (!found.empty()) return found;
  }
  return {};
}

std::optional<HandshakeResponse> accept_handshake(const HandshakeRequest& request) {
  if (request.version != "13") return std::nullopt;
  auto decoded = compat::base64::strict_decode(request.key);
  if (!decoded || decoded->size() != 16) return std::nullopt;
  HandshakeResponse response;
  std::string material(request.key);
  material += kAcceptGuid;
  response.accept = compat::base64::strict_encode(compat::crypto::sha1(material));
  for (auto header : request.extensions) {
    for_each_part(header, ',', [&](std::string_view offer) {
      response.deflate = response.deflate || acceptable_deflate_offer(offer);
    });
  }
  response.protocol = negotiate_protocol(request.protocols);
  return response;
}

// ---- Frames --------------------------------------------------------------------------------------

std::size_t encode_header(std::uint8_t out[10], Opcode opcode, bool compressed, std::size_t payload_size) {
  out[0] = static_cast<std::uint8_t>(0x80 | (compressed ? 0x40 : 0) | static_cast<std::uint8_t>(opcode));
  if (payload_size < 126) {
    out[1] = static_cast<std::uint8_t>(payload_size);
    return 2;
  }
  if (payload_size <= 0xffff) {
    out[1] = 126;
    out[2] = static_cast<std::uint8_t>(payload_size >> 8);
    out[3] = static_cast<std::uint8_t>(payload_size);
    return 4;
  }
  out[1] = 127;
  for (int i = 0; i < 8; ++i) out[2 + i] = static_cast<std::uint8_t>(std::uint64_t{payload_size} >> (56 - 8 * i));
  return 10;
}

std::string encode_frame(Opcode opcode, bool compressed, std::string_view payload) {
  std::uint8_t head[10];
  std::size_t n = encode_header(head, opcode, compressed, payload.size());
  std::string out;
  out.reserve(n + payload.size());
  out.append(reinterpret_cast<const char*>(head), n);
  out.append(payload);
  return out;
}

std::string encode_close(std::uint16_t code) {
  char payload[2] = {static_cast<char>(code >> 8), static_cast<char>(code & 0xff)};
  return encode_frame(Opcode::Close, false, std::string_view(payload, 2));
}

std::string encode_close_reply(std::optional<std::uint16_t> code) {
  return code ? encode_close(*code) : encode_frame(Opcode::Close, false, {});
}

void apply_mask(std::uint8_t* data, std::size_t size, const std::uint8_t mask[4]) {
  std::uint64_t word = 0;
  for (int i = 0; i < 8; ++i) word |= std::uint64_t{mask[i % 4]} << (8 * i);
  std::size_t i = 0;
  for (; i + 8 <= size; i += 8) {
    std::uint64_t v;
    std::memcpy(&v, data + i, 8);
    v ^= word;
    std::memcpy(data + i, &v, 8);
  }
  for (; i < size; ++i) data[i] ^= mask[i % 4];
}

std::string deflate_message(std::string_view input) {
  thread_local DeflateState state;
  z_stream& s = state.s;
  deflateReset(&s);
  std::string out;
  out.resize(deflateBound(&s, static_cast<uLong>(input.size())) + 32);
  s.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
  s.avail_in = static_cast<uInt>(input.size());
  while (true) {
    s.next_out = reinterpret_cast<Bytef*>(out.data()) + s.total_out;
    s.avail_out = static_cast<uInt>(out.size() - s.total_out);
    deflate(&s, Z_SYNC_FLUSH);
    if (s.avail_in == 0 && s.avail_out > 0) break;
    out.resize(out.size() * 2);
  }
  out.resize(s.total_out);
  if (out.size() >= kDeflateTail.size() && std::string_view(out).ends_with(kDeflateTail)) {
    out.resize(out.size() - kDeflateTail.size());
  }
  return out;
}

std::expected<std::string, std::uint16_t> inflate_message(std::string_view input, std::size_t max_size) {
  thread_local InflateState state;
  z_stream& s = state.s;
  inflateReset(&s);
  std::string data(input);
  data += kDeflateTail;
  s.next_in = reinterpret_cast<Bytef*>(data.data());
  s.avail_in = static_cast<uInt>(data.size());
  std::string out;
  out.resize(std::min<std::size_t>(std::max<std::size_t>(input.size() * 4, 256), max_size + 1));
  while (true) {
    s.next_out = reinterpret_cast<Bytef*>(out.data()) + s.total_out;
    s.avail_out = static_cast<uInt>(out.size() - s.total_out);
    int rc = inflate(&s, Z_SYNC_FLUSH);
    if (rc == Z_NEED_DICT || rc == Z_DATA_ERROR || rc == Z_MEM_ERROR || rc == Z_STREAM_ERROR) {
      return std::unexpected(kCloseProtocolError);
    }
    if (s.total_out > max_size) return std::unexpected(kCloseTooLarge);
    if (rc == Z_STREAM_END || (s.avail_in == 0 && s.avail_out > 0)) break;
    if (s.avail_out == 0) out.resize(std::min<std::size_t>(out.size() * 2, max_size + 1));
    else if (rc == Z_BUF_ERROR) break;
  }
  out.resize(s.total_out);
  return out;
}

// ---- Parser --------------------------------------------------------------------------------------

void FrameParser::feed(std::span<const std::uint8_t> bytes) {
  buf_.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void FrameParser::feed(std::string_view bytes) { buf_.append(bytes); }

std::unexpected<std::uint16_t> FrameParser::fail(std::uint16_t code) {
  error_ = code;
  return std::unexpected(code);
}

// Checks a header in the order of websocket-driver (Hybi#parse_opcode, #parse_length,
// #check_frame_length). The checks do not change the state, so a short buffer repeats them.
std::expected<bool, std::uint16_t> FrameParser::parse_header() {
  std::size_t avail = buf_.size() - pos_;
  if (avail < 2) return false;
  auto b0 = static_cast<std::uint8_t>(buf_[pos_]);
  auto b1 = static_cast<std::uint8_t>(buf_[pos_ + 1]);
  bool fin = (b0 & 0x80) != 0;
  bool rsv1 = (b0 & 0x40) != 0;
  std::uint8_t opcode = b0 & 0x0f;
  bool control = opcode >= 0x8;
  bool is_data = opcode == 0x1 || opcode == 0x2;
  // RSV1 may only start a data message, and only if deflate is on.
  bool reserved = (b0 & 0x30) != 0 || (rsv1 && !(options_.deflate && is_data));
  bool known = opcode == 0x0 || is_data || opcode == 0x8 || opcode == 0x9 || opcode == 0xA;
  bool interrupts = is_data && partial_.has_value();
  if (reserved || !known || (control && !fin) || interrupts) return fail(kCloseProtocolError);
  bool masked = (b1 & 0x80) != 0;
  if (!masked && options_.require_mask) return fail(kCloseUnacceptable);
  std::size_t ext = (b1 & 0x7f) == 126 ? 2 : ((b1 & 0x7f) == 127 ? 8 : 0);
  if (avail < 2 + ext) return false;
  std::uint64_t length = b1 & 0x7f;
  if (ext != 0) {
    length = 0;
    for (std::size_t i = 0; i < ext; ++i) length = (length << 8) | static_cast<std::uint8_t>(buf_[pos_ + 2 + i]);
  }
  if (control && length > 125) return fail(kCloseProtocolError);
  std::uint64_t so_far = (!control && partial_) ? partial_->data.size() : 0;
  if (length > options_.max_message || so_far + length > options_.max_message) return fail(kCloseTooLarge);
  std::size_t header_size = 2 + ext + (masked ? 4 : 0);
  if (avail < header_size) return false;
  Header h;
  h.fin = fin;
  h.compressed = rsv1;
  h.opcode = opcode;
  h.length = length;
  h.masked = masked;
  h.header_size = header_size;
  if (masked) std::memcpy(h.mask, buf_.data() + pos_ + header_size - 4, 4);
  header_ = h;
  return true;
}

std::expected<std::optional<Message>, std::uint16_t> FrameParser::finish(Header& header, std::string payload) {
  switch (header.opcode) {
    case 0x8: {
      auto code = close_code(payload);
      if (!code) return fail(code.error());
      Message m;
      m.type = Message::Type::Close;
      m.close_code = *code;
      return std::optional<Message>(std::move(m));
    }
    case 0x9: return std::optional<Message>(Message{Message::Type::Ping, std::move(payload), std::nullopt});
    case 0xA: return std::optional<Message>(Message{Message::Type::Pong, std::move(payload), std::nullopt});
    default: break;
  }
  if (header.opcode == 0x1 || header.opcode == 0x2) {
    partial_ = Partial{header.opcode, header.compressed, std::move(payload)};
  } else if (partial_) {
    partial_->data += payload;
  } else {
    return fail(kCloseProtocolError);  // a continuation of nothing
  }
  if (!header.fin) return std::optional<Message>{};
  Partial done = std::move(*partial_);
  partial_.reset();
  if (done.compressed) {
    auto inflated = inflate_message(done.data, options_.max_message);
    if (!inflated) return fail(inflated.error());
    done.data = std::move(*inflated);
  }
  if (done.opcode == 0x2) return std::optional<Message>(Message{Message::Type::Binary, std::move(done.data), std::nullopt});
  if (!compat::json::valid_utf8(done.data)) return fail(kCloseEncodingError);
  return std::optional<Message>(Message{Message::Type::Text, std::move(done.data), std::nullopt});
}

std::expected<std::optional<Message>, std::uint16_t> FrameParser::next() {
  if (error_) return std::unexpected(*error_);
  while (true) {
    if (!header_) {
      auto ready = parse_header();
      if (!ready) return std::unexpected(ready.error());
      if (!*ready) return std::optional<Message>{};
    }
    Header header = *header_;
    std::size_t total = header.header_size + static_cast<std::size_t>(header.length);
    if (buf_.size() - pos_ < total) return std::optional<Message>{};
    std::string payload(buf_, pos_ + header.header_size, static_cast<std::size_t>(header.length));
    if (header.masked) apply_mask(reinterpret_cast<std::uint8_t*>(payload.data()), payload.size(), header.mask);
    pos_ += total;
    header_.reset();
    if (pos_ == buf_.size()) {
      buf_.clear();
      pos_ = 0;
      if (buf_.capacity() > 64 * 1024) buf_.shrink_to_fit();
    } else if (pos_ >= 4096 && pos_ * 2 >= buf_.size()) {
      buf_.erase(0, pos_);
      pos_ = 0;
    }
    auto result = finish(header, std::move(payload));
    if (!result) return result;
    if (*result) return result;
  }
}

}  // namespace campfire::cable::ws
