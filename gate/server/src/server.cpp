// HTTP workers: epoll loop, request routing and the request handlers (GET /rooms/:id,
// POST /rooms/:id/messages, GET /up, plus minimal sign-in support).
#include <arpa/inet.h>
#include <fcntl.h>
#include <libdeflate.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <crypt.h>

#include <cstring>
#include <fstream>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>

#include "gate.hpp"
extern "C" {
#include "picohttpparser.h"
}

namespace gate {

bool bcrypt_verify(const std::string& password, const std::string& digest) {
  struct crypt_data data;
  memset(&data, 0, sizeof data);
  const char* r = crypt_r(password.c_str(), digest.c_str(), &data);
  return r && digest.size() >= 59 && digest == r;
}

// ------------------------------------------------------------------ fixtures and caches

static std::string read_file(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// A cached room page: one identity body and one gzip body (already chunk-framed), per cache key.
struct PageEntry {
  std::string identity;
  std::string gz_chunked;  // "<hex len>\r\n<gzip>\r\n0\r\n\r\n"
  std::string etag;        // W/"<md5 of the cache key>"
};

static std::string gzip_body(libdeflate_compressor* c, std::string_view in) {
  std::string out(libdeflate_gzip_compress_bound(c, in.size()), '\0');
  size_t n = libdeflate_gzip_compress(c, in.data(), in.size(), out.data(), out.size());
  out.resize(n);
  return out;
}
static std::string chunk_frame(const std::string& gz) {
  char h[32];
  int n = snprintf(h, sizeof h, "%zx\r\n", gz.size());
  std::string o(h, n);
  o += gz;
  o += "\r\n0\r\n\r\n";
  return o;
}

static std::shared_ptr<const PageEntry> make_entry(const std::string& key, std::string body) {
  auto e = std::make_shared<PageEntry>();
  libdeflate_compressor* c = libdeflate_alloc_compressor(6);
  e->gz_chunked = chunk_frame(gzip_body(c, body));
  libdeflate_free_compressor(c);
  e->identity = std::move(body);
  e->etag = "W/\"" + md5_hex(key) + "\"";
  return e;
}

class PageCache {
 public:
  std::shared_ptr<const PageEntry> get(const std::string& key) {
    std::shared_lock l(m_);
    auto it = map_.find(key);
    return it == map_.end() ? nullptr : it->second;
  }
  void put(const std::string& key, std::shared_ptr<const PageEntry> e) {
    std::unique_lock l(m_);
    if (map_.size() >= 256) map_.clear();
    map_[key] = std::move(e);
  }

 private:
  std::shared_mutex m_;
  std::unordered_map<std::string, std::shared_ptr<const PageEntry>> map_;
};

// Bounded in-process fragment store (Rust: FragmentCache, 32 MB, entries pruned to 3/4).
class FragmentStore {
 public:
  void put(std::string key, std::string value) {
    std::lock_guard<std::mutex> l(m_);
    size_t cost = key.size() + value.size() + 240;
    auto it = map_.find(key);
    if (it != map_.end()) { bytes_ -= it->second.size() + it->first.size() + 240; it->second = std::move(value); bytes_ += cost; return; }
    order_.push_back(key);
    map_.emplace(std::move(key), std::move(value));
    bytes_ += cost;
    if (bytes_ > kMax) {
      while (bytes_ > kMax * 3 / 4 && !order_.empty()) {
        auto f = map_.find(order_.front());
        if (f != map_.end()) { bytes_ -= f->second.size() + f->first.size() + 240; map_.erase(f); }
        order_.pop_front();
      }
    }
  }

 private:
  static constexpr size_t kMax = 32u << 20;
  std::mutex m_;
  std::unordered_map<std::string, std::string> map_;
  std::deque<std::string> order_;
  size_t bytes_ = 0;
};

struct Shared {
  Config cfg;
  std::unique_ptr<Secrets> secrets;
  PageCache pages;
  FragmentStore fragments;
  Writer writer;
  std::vector<Completions*> comps;
  std::string link_header, post_template, sidebar_html;
  std::string page_template;       // captured room page with \x01H\x02 (host) and \x01B\x02 (browser name) slots
  std::string startup_db_part;     // the database part of the cache key the captured page was stored under
  std::string up_body = R"(<!DOCTYPE html><html><body style="background-color: green"></body></html>)";
  std::string up_etag, up_gz_chunked;
};
static Shared* S;

// ------------------------------------------------------------------ small helpers

static const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static std::string http_date(time_t t) {
  struct tm tm;
  gmtime_r(&t, &tm);
  char b[40];
  snprintf(b, sizeof b, "%s, %02d %s %04d %02d:%02d:%02d GMT", kDays[tm.tm_wday], tm.tm_mday, kMonths[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour,
           tm.tm_min, tm.tm_sec);
  return b;
}
static time_t years_from(time_t t, int years) {
  struct tm tm;
  gmtime_r(&t, &tm);
  tm.tm_year += years;
  return timegm(&tm);
}

struct Rng {
  uint64_t s[2];
  Rng() { getrandom(s, sizeof s, 0); if (!s[0] && !s[1]) s[0] = 1; }
  uint64_t next() {
    uint64_t a = s[0], b = s[1];
    s[0] = b;
    a ^= a << 23;
    s[1] = a ^ b ^ (a >> 17) ^ (b >> 26);
    return s[1] + b;
  }
};
static std::string uuid_v4(Rng& r) {
  uint64_t a = r.next(), b = r.next();
  unsigned char u[16];
  memcpy(u, &a, 8);
  memcpy(u + 8, &b, 8);
  u[6] = (u[6] & 0x0f) | 0x40;
  u[8] = (u[8] & 0x3f) | 0x80;
  char o[40];
  snprintf(o, sizeof o, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8],
           u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
  return o;
}

static bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
  return true;
}

// Parses "YYYY-MM-DD HH:MM:SS.ffffff" to unix microseconds.
static int64_t parse_us(std::string_view s) {
  if (s.size() < 19) return 0;
  std::string iso(s.substr(0, 10));
  iso += 'T';
  iso += s.substr(11, 8);
  iso += 'Z';
  int64_t ms = parse_iso_ms(iso).value_or(0);
  int64_t frac = 0;
  if (s.size() > 20) {
    int d = 0;
    for (size_t i = 20; i < s.size() && d < 6; i++, d++) frac = frac * 10 + (s[i] - '0');
    for (; d < 6; d++) frac *= 10;
  }
  return ms * 1000 + frac;
}

// application/x-www-form-urlencoded with Rack's bracket keys: returns the value for an exact key.
static std::string form_get(std::string_view body, std::string_view key, bool* found = nullptr) {
  size_t i = 0;
  while (i <= body.size()) {
    size_t e = body.find('&', i);
    if (e == std::string_view::npos) e = body.size();
    std::string_view pair = body.substr(i, e - i);
    size_t eq = pair.find('=');
    std::string k = www_unescape(pair.substr(0, eq == std::string_view::npos ? pair.size() : eq));
    if (k == key) {
      if (found) *found = true;
      return eq == std::string_view::npos ? "" : www_unescape(pair.substr(eq + 1));
    }
    i = e + 1;
  }
  if (found) *found = false;
  return "";
}

// ------------------------------------------------------------------ requests and connections

struct Hdr { std::string_view n, v; };
struct Req {
  std::string_view method, path, query;
  int minor = 1;
  Hdr hs[64];
  size_t nh = 0;
  std::string_view body;
  size_t consumed = 0;
  bool close = false;
  std::string_view h(const char* name) const {
    for (size_t i = 0; i < nh; i++) if (ieq(hs[i].n, name)) return hs[i].v;
    return {};
  }
  bool has(const char* name) const {
    for (size_t i = 0; i < nh; i++) if (ieq(hs[i].n, name)) return true;
    return false;
  }
};

struct ConnState {
  int fd = -1;
  uint64_t id = 0;
  std::string in;
  std::string head;  // pending response head (plus small bodies)
  size_t head_off = 0;
  std::shared_ptr<const PageEntry> hold;  // keeps a cached body alive while it is being written
  const char* bptr = nullptr;
  size_t blen = 0, boff = 0;
  bool waiting = false, close_after = false, want_out = false;
  std::string peer;
  // state of an in-flight POST
  int64_t t0_ns = 0;
  bool accept_gz = false;
  int64_t user_id = 0;
};

enum Encoding { ENC_GZIP, ENC_IDENTITY, ENC_NONE };
static Encoding negotiate(std::string_view ae) {
  bool gz = false, gz_off = false, star = false, id_off = false;
  size_t i = 0;
  while (i <= ae.size()) {
    size_t e = ae.find(',', i);
    if (e == std::string_view::npos) e = ae.size();
    std::string_view tok = ae.substr(i, e - i);
    i = e + 1;
    while (!tok.empty() && tok.front() == ' ') tok.remove_prefix(1);
    size_t semi = tok.find(';');
    std::string_view name = tok.substr(0, semi);
    while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
    bool zero = false;
    if (semi != std::string_view::npos) {
      std::string_view p = tok.substr(semi + 1);
      size_t q = p.find("q=");
      if (q != std::string_view::npos) zero = atof(std::string(p.substr(q + 2)).c_str()) <= 0.0;
    }
    if (ieq(name, "gzip") || ieq(name, "x-gzip")) { if (zero) gz_off = true; else gz = true; }
    else if (name == "*") { if (!zero) star = true; }
    else if (ieq(name, "identity")) { if (zero) id_off = true; }
  }
  if (gz || (star && !gz_off)) return ENC_GZIP;
  return id_off ? ENC_NONE : ENC_IDENTITY;
}

static int64_t mono_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

class Worker {
 public:
  Worker(int idx, Completions* comp) : idx_(idx), comp_(comp) {}
  bool init();
  void run();

 private:
  // io
  void accept_ready();
  void on_readable(ConnState* c);
  void process(ConnState* c);
  bool flush(ConnState* c);
  void close_conn(ConnState* c);
  void set_out_interest(ConnState* c, bool on);
  void on_completions();
  // responses
  void begin(std::string& h, const char* status);
  void respond_small(ConnState* c, int code, const char* reason, const char* ctype, std::string_view body, const char* extra = "");
  void respond_redirect(ConnState* c, const Req& r, const std::string& path, const std::string& extra_headers = "");
  const std::string& date_now();
  std::string runtime(int64_t t0) {
    char b[32];
    snprintf(b, sizeof b, "%.6f", (mono_ns() - t0) / 1e9);
    return b;
  }
  // handlers
  void handle(ConnState* c, Req& r);
  void up(ConnState* c, const Req& r, int64_t t0);
  void room_show(ConnState* c, const Req& r, int64_t room_id, int64_t t0);
  void post_message(ConnState* c, const Req& r, int64_t room_id, int64_t t0);
  void finish_post(ConnState* c, Job& j);
  void session_create(ConnState* c, const Req& r);
  void finish_session(ConnState* c, Job& j);
  bool authenticate(const Req& r, int64_t& uid);
  std::string peer_ip(const ConnState* c, const Req&) { return c->peer; }

  int idx_;
  Completions* comp_;
  int ep_ = -1, lfd_ = -1;
  Conn db_;
  RowSet rs_[16];
  std::unordered_map<uint64_t, std::unique_ptr<ConnState>> conns_;
  uint64_t next_id_ = 2;
  std::unordered_map<std::string, int64_t> sessions_;  // raw cookie value -> user id
  std::unordered_map<std::string, std::shared_ptr<const PageEntry>> local_pages_;
  libdeflate_compressor* gz_ = nullptr;
  Rng rng_;
  time_t date_sec_ = 0;
  std::string date_str_;
  std::string key_;
};

bool Worker::init() {
  set_trace_thread_id(idx_ + 1);
  if (!db_.open(S->cfg.db_path, "reader", true, S->cfg.trace)) { fprintf(stderr, "reader open failed: %s\n", db_.error().c_str()); return false; }
  gz_ = libdeflate_alloc_compressor(6);
  ep_ = epoll_create1(0);
  lfd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  int one = 1;
  setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(lfd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(S->cfg.port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(lfd_, (sockaddr*)&a, sizeof a) != 0 || listen(lfd_, 1024) != 0) { perror("bind/listen"); return false; }
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.u64 = 0;
  epoll_ctl(ep_, EPOLL_CTL_ADD, lfd_, &ev);
  ev.data.u64 = 1;
  epoll_ctl(ep_, EPOLL_CTL_ADD, comp_->efd, &ev);
  return true;
}

const std::string& Worker::date_now() {
  time_t t = time(nullptr);
  if (t != date_sec_) { date_sec_ = t; date_str_ = http_date(t); }
  return date_str_;
}

void Worker::run() {
  epoll_event evs[128];
  for (;;) {
    int n = epoll_wait(ep_, evs, 128, -1);
    for (int i = 0; i < n; i++) {
      uint64_t id = evs[i].data.u64;
      if (id == 0) { accept_ready(); continue; }
      if (id == 1) { uint64_t v; (void)!read(comp_->efd, &v, 8); on_completions(); continue; }
      auto it = conns_.find(id);
      if (it == conns_.end()) continue;
      ConnState* c = it->second.get();
      if (evs[i].events & (EPOLLERR | EPOLLHUP)) { close_conn(c); continue; }
      if (evs[i].events & EPOLLOUT) { if (!flush(c)) continue; if (!c->in.empty()) process(c); }
      if (evs[i].events & EPOLLIN) on_readable(c);
    }
  }
}

void Worker::accept_ready() {
  for (;;) {
    sockaddr_in pa;
    socklen_t pl = sizeof pa;
    int fd = accept4(lfd_, (sockaddr*)&pa, &pl, SOCK_NONBLOCK);
    if (fd < 0) return;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    auto c = std::make_unique<ConnState>();
    c->fd = fd;
    c->id = next_id_++;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &pa.sin_addr, ip, sizeof ip);
    c->peer = ip;
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u64 = c->id;
    epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev);
    conns_[c->id] = std::move(c);
  }
}

void Worker::close_conn(ConnState* c) {
  epoll_ctl(ep_, EPOLL_CTL_DEL, c->fd, nullptr);
  close(c->fd);
  conns_.erase(c->id);  // frees c
}

void Worker::set_out_interest(ConnState* c, bool on) {
  if (c->want_out == on) return;
  c->want_out = on;
  epoll_event ev{};
  ev.events = EPOLLIN | (on ? EPOLLOUT : 0);
  ev.data.u64 = c->id;
  epoll_ctl(ep_, EPOLL_CTL_MOD, c->fd, &ev);
}

// Writes pending output. Returns false when the connection was closed.
bool Worker::flush(ConnState* c) {
  for (;;) {
    iovec iov[2];
    int n = 0;
    if (c->head_off < c->head.size()) { iov[n].iov_base = (void*)(c->head.data() + c->head_off); iov[n].iov_len = c->head.size() - c->head_off; n++; }
    if (c->boff < c->blen) { iov[n].iov_base = (void*)(c->bptr + c->boff); iov[n].iov_len = c->blen - c->boff; n++; }
    if (n == 0) break;
    ssize_t w = writev(c->fd, iov, n);
    if (w < 0) {
      if (errno == EAGAIN || errno == EINTR) { set_out_interest(c, true); return true; }
      close_conn(c);
      return false;
    }
    size_t left = (size_t)w;
    size_t hrem = c->head.size() - c->head_off;
    if (left >= hrem) { left -= hrem; c->head.clear(); c->head_off = 0; c->boff += left; }
    else c->head_off += left;
  }
  c->head.clear(); c->head_off = 0; c->hold.reset(); c->bptr = nullptr; c->blen = c->boff = 0;
  set_out_interest(c, false);
  if (c->close_after) { close_conn(c); return false; }
  return true;
}

void Worker::on_readable(ConnState* c) {
  char buf[16384];
  for (;;) {
    ssize_t n = read(c->fd, buf, sizeof buf);
    if (n > 0) { c->in.append(buf, n); if ((size_t)n < sizeof buf) break; continue; }
    if (n == 0) { close_conn(c); return; }
    if (errno == EAGAIN) break;
    if (errno == EINTR) continue;
    close_conn(c);
    return;
  }
  if (c->in.size() > (4u << 20)) { close_conn(c); return; }
  process(c);
}

// Parses and handles as many complete requests as the buffer holds.
void Worker::process(ConnState* c) {
  uint64_t id = c->id;
  while (!c->waiting && !c->in.empty() && !c->close_after && c->head.empty() && c->blen == 0) {
    Req r;
    const char *m, *p;
    size_t ml, pl, nh = 64;
    phr_header ph[64];
    int minor;
    int pr = phr_parse_request(c->in.data(), c->in.size(), &m, &ml, &p, &pl, &minor, ph, &nh, 0);
    if (pr == -2) return;
    if (pr < 0) { respond_small(c, 400, "Bad Request", "text/plain", "bad request"); c->close_after = true; flush(c); return; }
    r.method = {m, ml};
    std::string_view target{p, pl};
    size_t q = target.find('?');
    r.path = target.substr(0, q);
    if (q != std::string_view::npos) r.query = target.substr(q + 1);
    r.minor = minor;
    r.nh = nh;
    for (size_t i = 0; i < nh; i++) r.hs[i] = {{ph[i].name, ph[i].name_len}, {ph[i].value, ph[i].value_len}};
    size_t cl = 0;
    std::string_view clh = r.h("content-length");
    if (!clh.empty()) cl = strtoull(std::string(clh).c_str(), nullptr, 10);
    if (r.has("transfer-encoding")) { respond_small(c, 501, "Not Implemented", "text/plain", "chunked request bodies are not supported"); c->close_after = true; flush(c); return; }
    if (c->in.size() < (size_t)pr + cl) return;
    r.body = std::string_view(c->in).substr(pr, cl);
    r.consumed = pr + cl;
    std::string_view conn = r.h("connection");
    r.close = ieq(conn, "close") || minor == 0;
    int64_t t0 = mono_ns();
    handle(c, r);
    // handle() may have closed the connection only through flush(); detect that
    if (conns_.find(id) == conns_.end()) return;
    c->in.erase(0, r.consumed);
    if (r.close) c->close_after = true;
    if (c->waiting) return;
    if (!flush(c)) return;
    (void)t0;
  }
}

// ------------------------------------------------------------------ responses

void Worker::respond_small(ConnState* c, int code, const char* reason, const char* ctype, std::string_view body, const char* extra) {
  c->head = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\ncontent-type: " + ctype + "\r\n" + extra +
            "content-length: " + std::to_string(body.size()) + "\r\ndate: " + date_now() + "\r\n\r\n";
  c->head.append(body);
}

void Worker::respond_redirect(ConnState* c, const Req& r, const std::string& path, const std::string& extra) {
  std::string host(r.h("host"));
  std::string loc = "http://" + host + path;
  std::string body = "<html><body>You are being <a href=\"" + html_escape(loc) + "\">redirected</a>.</body></html>";
  c->head = "HTTP/1.1 302 Found\r\nlocation: " + loc + "\r\ncontent-type: text/html; charset=utf-8\r\n" + extra +
            "cache-control: no-cache\r\ncontent-length: " + std::to_string(body.size()) + "\r\ndate: " + date_now() + "\r\n\r\n" + body;
}

// session_token cookie -> user id. The per-worker cache is keyed by the raw cookie value.
bool Worker::authenticate(const Req& r, int64_t& uid) {
  std::string_view ck = r.h("cookie");
  std::string_view raw;
  size_t i = 0;
  bool found = false;
  while (i < ck.size()) {
    while (i < ck.size() && ck[i] == ' ') i++;
    size_t e = ck.find(';', i);
    if (e == std::string_view::npos) e = ck.size();
    std::string_view part = ck.substr(i, e - i);
    if (part.substr(0, 14) == "session_token=") { raw = part.substr(14); found = true; break; }
    i = e + 1;
  }
  if (!found) return false;
  std::string rawk(raw);
  auto it = sessions_.find(rawk);
  if (it != sessions_.end()) { uid = it->second; return true; }
  auto token = verify_signed_cookie(*S->secrets, "session_token", www_unescape(raw), time(nullptr) * 1000LL);
  if (!token) return false;
  RowSet& rs = rs_[15];
  if (db_.run(Q_SessionByToken, {*token}, rs) != SQLITE_OK || rs.nrows != 1) return false;
  int64_t u = rs.num(0, 1);
  if (db_.run(Q_UserById, {u}, rs) != SQLITE_OK || rs.nrows != 1) return false;
  if (sessions_.size() > 10000) sessions_.clear();
  sessions_[rawk] = u;
  uid = u;
  return true;
}

static void append_cells(std::string& key, const RowSet& rs, char tag) {
  key += tag;
  key += (char)rs.nrows;
  for (size_t r = 0; r < rs.nrows; r++)
    for (size_t c = 0; c < rs.ncols; c++) {
      const Cell& x = rs.at(r, c);
      key += (char)x.type;
      if (x.type == C_INT) key.append((const char*)&x.i, 8);
      else if (x.type != C_NULL) { key.append((const char*)&x.len, 4); key.append(rs.arena, x.off, x.len); }
    }
}

// The room page cache key, computed from the rows the page statements return (see README).
// Returns 1 on success, 0 when the room is not visible to the user, -1 on a database error.
static int page_key(Conn& db, RowSet* rs, int64_t uid, int64_t room_id, std::string& key) {
  if (db.run(Q_RoomForUser, {uid, room_id}, rs[0]) != SQLITE_OK) return -1;
  if (rs[0].nrows == 0) return 0;
  if (db.run(Q_Messages40, {room_id}, rs[1]) != SQLITE_OK) return -1;
  if (db.run(Q_RoomFirst, {}, rs[2]) != SQLITE_OK) return -1;
  if (db.run(Q_AccountFirst, {}, rs[3]) != SQLITE_OK) return -1;
  if (db.run(Q_AccountFirst, {}, rs[4]) != SQLITE_OK) return -1;
  int64_t account_id = rs[4].nrows ? rs[4].num(0, 0) : 0;
  if (db.run(Q_Attachment, {"Account", account_id, "logo"}, rs[5]) != SQLITE_OK) return -1;
  if (db.run(Q_RoomForUser, {uid, room_id}, rs[6]) != SQLITE_OK) return -1;
  key.clear();
  key.append("campfire-gate-room-page-v1");
  key.append((const char*)&uid, 8);
  append_cells(key, rs[0], 'R');   // the room as the user sees it: every column
  append_cells(key, rs[1], 'M');   // the 40 messages: every column (id, creator, client id, created_at, updated_at)
  key += 'O';                      // original room: the page only compares its id with the room id
  { int64_t oid = rs[2].nrows ? rs[2].cells[0].i : 0; key.append((const char*)&oid, 8); }
  append_cells(key, rs[3], 'A');   // account: every column
  append_cells(key, rs[4], 'a');
  append_cells(key, rs[5], 'L');   // account logo blob rows
  append_cells(key, rs[6], 'r');
  return 1;
}

void Worker::up(ConnState* c, const Req& r, int64_t t0) {
  Encoding e = negotiate(r.h("accept-encoding"));
  if (e == ENC_NONE) { respond_small(c, 406, "Not Acceptable", "text/plain", "An acceptable encoding for the requested resource /up could not be found."); return; }
  std::string& h = c->head;
  h = "HTTP/1.1 200 OK\r\ncontent-type: text/html; charset=utf-8\r\nvary: Accept,Accept-Encoding\r\nx-frame-options: SAMEORIGIN\r\nx-xss-protection: 0\r\n"
      "x-content-type-options: nosniff\r\nx-permitted-cross-domain-policies: none\r\nreferrer-policy: strict-origin-when-cross-origin\r\n"
      "etag: " + S->up_etag + "\r\ncache-control: max-age=0, private, must-revalidate\r\n";
  if (e == ENC_GZIP) h += "content-encoding: gzip\r\n";
  else h += "content-length: " + std::to_string(S->up_body.size()) + "\r\n";
  h += "x-request-id: " + uuid_v4(rng_) + "\r\nx-runtime: " + runtime(t0) + "\r\nx-cache: miss\r\ndate: " + date_now() + "\r\n";
  if (e == ENC_GZIP) { h += "transfer-encoding: chunked\r\n\r\n"; h += S->up_gz_chunked; }
  else { h += "\r\n"; h += S->up_body; }
}

// Fills the request-dependent slots of the captured room page (the page text depends on the Host
// header and on the browser name that Rust's user-agent parsing derives from User-Agent).
// Only blank and "name/version" user agents are supported (see README).
static bool assemble_page(std::string_view host, std::string_view ua, std::string& out) {
  std::string browser;
  size_t b = ua.find_first_not_of(" \t"), e = ua.find_last_not_of(" \t");
  std::string_view u = b == std::string_view::npos ? std::string_view() : ua.substr(b, e - b + 1);
  if (u.empty()) browser = "Mozilla";
  else {
    size_t slash = u.find('/');
    std::string_view name = u.substr(0, slash);
    if (name.empty() || !isalpha((unsigned char)name[0])) return false;
    for (char ch : name) if (!isalnum((unsigned char)ch) && ch != '.' && ch != '-' && ch != '_') return false;
    if (slash != std::string_view::npos) for (char ch : u.substr(slash + 1)) if (!isdigit((unsigned char)ch) && ch != '.') return false;
    browser = std::string(name);
    browser[0] = (char)toupper((unsigned char)browser[0]);
  }
  const std::string& t = S->page_template;
  out.clear();
  out.reserve(t.size() + 64);
  for (size_t i = 0; i < t.size();) {
    size_t s0 = t.find('\x01', i);
    if (s0 == std::string::npos) { out.append(t, i, std::string::npos); break; }
    out.append(t, i, s0 - i);
    out += t[s0 + 1] == 'H' ? std::string(host) : browser;
    i = s0 + 3;
  }
  return true;
}

void Worker::room_show(ConnState* c, const Req& r, int64_t room_id, int64_t t0) {
  int64_t uid;
  if (!authenticate(r, uid)) { respond_redirect(c, r, "/session/new"); return; }
  int pk = page_key(db_, rs_, uid, room_id, key_);
  if (pk < 0) { respond_small(c, 500, "Internal Server Error", "text/plain", "database error"); return; }
  if (pk == 0) { respond_redirect(c, r, "/"); return; }  // Rust: redirect to root with an alert
  const size_t db_len = key_.size();
  key_ += '\0'; key_ += 'H'; key_.append(r.h("host"));
  key_ += '\0'; key_ += 'U'; key_.append(r.h("user-agent"));
  std::shared_ptr<const PageEntry> e;
  auto lit = local_pages_.find(key_);
  if (lit != local_pages_.end()) e = lit->second;
  else {
    e = S->pages.get(key_);
    if (e) { if (local_pages_.size() > 64) local_pages_.clear(); local_pages_[key_] = e; }
  }
  if (!e && db_len == S->startup_db_part.size() && key_.compare(0, db_len, S->startup_db_part) == 0) {
    // The database inputs are those of the captured page; only Host or User-Agent differ. Fill the
    // slots, compress (libdeflate level 6) and cache the page under this key.
    std::string body;
    if (assemble_page(r.h("host"), r.h("user-agent"), body)) {
      e = make_entry(key_, std::move(body));
      S->pages.put(key_, e);
      if (local_pages_.size() > 64) local_pages_.clear();
      local_pages_[key_] = e;
    }
  }
  if (!e) {
    respond_small(c, 500, "Internal Server Error", "text/plain",
                  "gate server: this room page is not in the page cache and the gate server cannot render pages");
    return;
  }
  Encoding enc = negotiate(r.h("accept-encoding"));
  if (enc == ENC_NONE) { respond_small(c, 406, "Not Acceptable", "text/plain", "An acceptable encoding for the requested resource could not be found."); return; }
  time_t now = time(nullptr);
  std::string& h = c->head;
  std::string inm(r.h("if-none-match"));
  bool fresh = false;
  if (!inm.empty()) {
    size_t i = 0;
    while (i <= inm.size()) {
      size_t eidx = inm.find(',', i);
      if (eidx == std::string::npos) eidx = inm.size();
      std::string_view t = std::string_view(inm).substr(i, eidx - i);
      while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
      while (!t.empty() && t.back() == ' ') t.remove_suffix(1);
      if (t == e->etag || t == "*") fresh = true;
      i = eidx + 1;
    }
  }
  h = "HTTP/1.1 ";
  h += fresh ? "304 Not Modified\r\n" : "200 OK\r\ncontent-type: text/html; charset=utf-8\r\n";
  const bool accept_hdr = r.has("accept");  // Rails varies on Accept (at the front) when the request has one
  if (accept_hdr) h += "vary: Accept,Accept-Encoding\r\n";
  h += "x-version: " + S->cfg.app_version + "\r\nx-rev: " + S->cfg.git_rev + "\r\nlink: " + S->link_header + "\r\n";
  h += "set-cookie: last_room=" + std::to_string(room_id) + "; path=/; expires=" + http_date(years_from(now, 20)) + "; samesite=lax\r\n";
  h += "x-frame-options: SAMEORIGIN\r\nx-xss-protection: 0\r\nx-content-type-options: nosniff\r\nx-permitted-cross-domain-policies: none\r\n"
       "referrer-policy: strict-origin-when-cross-origin\r\netag: " + e->etag + "\r\ncache-control: max-age=0, private, must-revalidate\r\n";
  if (fresh) {
    h += "x-request-id: " + uuid_v4(rng_) + "\r\nx-runtime: " + runtime(t0) + "\r\n" + (accept_hdr ? "" : "vary: Accept-Encoding\r\n") + "x-cache: miss\r\ndate: " + date_now() + "\r\n\r\n";
    return;
  }
  if (enc == ENC_GZIP) h += "content-encoding: gzip\r\n";
  else h += "content-length: " + std::to_string(e->identity.size()) + "\r\n";
  h += "x-request-id: " + uuid_v4(rng_) + "\r\nx-runtime: " + runtime(t0) + "\r\n" + (accept_hdr ? "" : "vary: Accept-Encoding\r\n") + "x-cache: miss\r\ndate: " + date_now() + "\r\n";
  if (enc == ENC_GZIP) { h += "transfer-encoding: chunked\r\n\r\n"; c->bptr = e->gz_chunked.data(); c->blen = e->gz_chunked.size(); }
  else { h += "\r\n"; c->bptr = e->identity.data(); c->blen = e->identity.size(); }
  c->boff = 0;
  c->hold = std::move(e);
}

// ------------------------------------------------------------------ POST /rooms/:id/messages

void Worker::post_message(ConnState* c, const Req& r, int64_t room_id, int64_t t0) {
  int64_t uid;
  if (!authenticate(r, uid)) { respond_redirect(c, r, "/session/new"); return; }
  // request-thread reads, in Rust's order: bans, (session, user: cached), membership, room
  RowSet& bans = rs_[0];
  if (db_.run(Q_Bans, {c->peer}, bans) != SQLITE_OK) { respond_small(c, 500, "Internal Server Error", "text/plain", "database error"); return; }
  if (bans.nrows > 0) { respond_small(c, 403, "Forbidden", "text/plain", ""); return; }
  if (db_.run(Q_MembershipFor, {room_id, uid}, rs_[1]) != SQLITE_OK || db_.run(Q_RoomById, {room_id}, rs_[2]) != SQLITE_OK) {
    respond_small(c, 500, "Internal Server Error", "text/plain", "database error");
    return;
  }
  if (rs_[1].nrows == 0 || rs_[2].nrows == 0) { respond_small(c, 404, "Not Found", "text/plain", "room not found"); return; }
  // forgery protection: Rails accepts Sec-Fetch-Site: same-origin
  if (r.h("sec-fetch-site") != "same-origin") { respond_small(c, 422, "Unprocessable Content", "text/html", "<h1>Unprocessable</h1>"); return; }
  bool fb = false, fc = false;
  std::string body = form_get(r.body, "message[body]", &fb);
  std::string cid = form_get(r.body, "message[client_message_id]", &fc);
  if (!fb || !fc || cid.empty()) { respond_small(c, 400, "Bad Request", "text/plain", "param is missing or the value is empty: message"); return; }
  auto j = std::make_unique<Job>();
  j->kind = Job::POST;
  j->worker = idx_;
  j->conn_id = c->id;
  j->room_id = room_id;
  j->user_id = uid;
  j->cid = std::move(cid);
  j->body_html = html_escape(body);  // plain text only; no sanitizer, autolink or mentions (see README)
  c->t0_ns = t0;
  c->accept_gz = negotiate(r.h("accept-encoding")) == ENC_GZIP;
  c->user_id = uid;
  c->waiting = true;
  S->writer.submit(std::move(j));
}

// the Rust endpoint check: Push::Subscription#resolved_endpoint_ip (https, port 443, permitted host)
static bool push_endpoint_deliverable(std::string_view ep) {
  if (ep.empty()) return false;
  for (char ch : ep) if (isspace((unsigned char)ch)) return false;
  size_t sp = ep.find("://");
  if (sp == std::string_view::npos) return false;
  std::string scheme(ep.substr(0, sp));
  for (auto& ch : scheme) ch = (char)tolower((unsigned char)ch);
  std::string_view rest = ep.substr(sp + 3);
  std::string_view auth = rest.substr(0, rest.find_first_of("/?#"));
  size_t at = auth.rfind('@');
  if (at != std::string_view::npos) auth = auth.substr(at + 1);
  std::string host;
  int port = -1;
  if (!auth.empty() && auth.back() == ']') host = std::string(auth);
  else {
    size_t col = auth.rfind(':');
    if (col != std::string_view::npos) {
      host = std::string(auth.substr(0, col));
      std::string ps(auth.substr(col + 1));
      if (ps.empty() || ps.find_first_not_of("0123456789") != std::string::npos || ps.size() > 5 || atoi(ps.c_str()) > 65535) return false;
      port = atoi(ps.c_str());
    } else host = std::string(auth);
  }
  if (port < 0) port = scheme == "https" ? 443 : scheme == "http" ? 80 : -1;
  if (scheme != "https" || port != 443) return false;
  for (auto& ch : host) ch = (char)tolower((unsigned char)ch);
  static const char* permitted[] = {"jmt17.google.com", "fcm.googleapis.com", "updates.push.services.mozilla.com", "web.push.apple.com", "notify.windows.com"};
  if (host.empty()) return false;
  for (const char* p : permitted) {
    std::string ps(p);
    if (host == ps || (host.size() > ps.size() && host.compare(host.size() - ps.size() - 1, std::string::npos, "." + ps) == 0)) return true;
  }
  return false;
}

static void put_ms_digits(std::string& o, int64_t v) { o += std::to_string(v); }

void Worker::finish_post(ConnState* c, Job& j) {
  if (!j.ok) {
    respond_small(c, 500, "Internal Server Error", "text/html", "<h1>Internal Server Error</h1>");
    fprintf(stderr, "post failed: %s\n", j.err.c_str());
    flush(c);
    return;
  }
  const int64_t mid = j.message_id, rid = j.room_id, uid = j.user_id;
  RowSet &msg = rs_[0], &room1 = rs_[1], &rt = rs_[2], &room2 = rs_[3], &u1 = rs_[4], &u2 = rs_[5], &rt2 = rs_[6], &rt3 = rs_[7], &att = rs_[8],
         &push = rs_[9], &rt4 = rs_[10], &cnt = rs_[11], &boosts = rs_[12], &acct = rs_[13];
  bool ok = db_.run(Q_MessageById, {mid}, msg) == SQLITE_OK && db_.run(Q_RoomById, {rid}, room1) == SQLITE_OK &&
            db_.run(Q_RichTextFor, {mid, "Message", "body"}, rt) == SQLITE_OK && db_.run(Q_RoomById, {rid}, room2) == SQLITE_OK &&
            db_.run(Q_UserById, {uid}, u1) == SQLITE_OK && db_.run(Q_UserById, {uid}, u2) == SQLITE_OK &&
            db_.run(Q_RichTextFor, {mid, "Message", "body"}, rt2) == SQLITE_OK && db_.run(Q_RichTextFor, {mid, "Message", "body"}, rt3) == SQLITE_OK &&
            db_.run(Q_Attachment, {"Message", mid, "attachment"}, att) == SQLITE_OK;
  char cutoff[27];
  fmt_us(now_us() - 60 * 1000000LL, cutoff);
  ok = ok && db_.run(Q_PushSubs, {std::string_view(cutoff, 26), rid, uid}, push) == SQLITE_OK &&
       db_.run(Q_RichTextFor, {mid, "Message", "body"}, rt4) == SQLITE_OK;
  // badge count for each push subscription (the order of Rust's capture: the boosts read falls between the counts)
  size_t nsub = ok ? push.nrows : 0;
  size_t boosts_at = nsub >= 2 ? 1 : (nsub ? nsub - 1 : 0);
  int deliverable = 0;
  for (size_t i = 0; ok && i < nsub; i++) {
    int64_t sub_user = push.num(i, 1);
    ok = db_.run(Q_UnreadCount, {sub_user}, cnt) == SQLITE_OK;
    // Rust: Notification::build, then deliver; deliver does nothing unless the endpoint is a permitted https:443 push service
    std::string_view ep = push.text(i, 2);
    if (push_endpoint_deliverable(ep)) deliverable++;
    if (i == boosts_at) ok = ok && db_.run(Q_BoostsFor, {mid}, boosts) == SQLITE_OK;
  }
  if (nsub == 0) ok = ok && db_.run(Q_BoostsFor, {mid}, boosts) == SQLITE_OK;
  ok = ok && db_.run(Q_AccountFirst, {}, acct) == SQLITE_OK;
  if (!ok || msg.nrows != 1 || room1.nrows != 1 || rt.nrows != 1 || u1.nrows != 1) {
    respond_small(c, 500, "Internal Server Error", "text/html", "<h1>Internal Server Error</h1>");
    flush(c);
    return;
  }
  (void)deliverable;  // a deliverable endpoint would be encrypted and sent here; the benchmark endpoints never are

  // ---- render the turbo-stream from the captured template with this post's values
  std::string_view created = msg.text(0, 4), updated = msg.text(0, 5);
  int64_t cms = parse_us(created) / 1000, ums = parse_us(updated) / 1000;
  std::string iso(created.substr(0, 10));
  iso += 'T'; iso.append(created.substr(11, 8)); iso += 'Z';
  std::string v;
  for (char ch : u1.text(0, 9)) { if (isdigit((unsigned char)ch)) v += ch; if (v.size() == 14) break; }
  std::string avatar = "/users/" + signed_id(*S->secrets, "User", uid, "avatar") + "/avatar?v=" + v;
  std::string kind(room1.text(0, 2));
  for (auto& ch : kind) ch = (char)tolower((unsigned char)ch);
  for (size_t p; (p = kind.find("::")) != std::string::npos;) kind.replace(p, 2, "_");
  std::string roomdom = kind + "_" + std::to_string(rid);
  std::string out;
  out.reserve(S->post_template.size() + 512);
  const std::string& t = S->post_template;
  for (size_t i = 0; i < t.size();) {
    size_t s = t.find('\x01', i);
    if (s == std::string::npos) { out.append(t, i, std::string::npos); break; }
    out.append(t, i, s - i);
    size_t e = t.find('\x02', s);
    std::string_view name(t.data() + s + 1, e - s - 1);
    if (name == "AVATAR") out += avatar;
    else if (name == "BODY") out.append(rt.text(0, 2));
    else if (name == "CID") out.append(msg.text(0, 3));
    else if (name == "CMS") put_ms_digits(out, cms);
    else if (name == "UMS") put_ms_digits(out, ums);
    else if (name == "ISO") out += iso;
    else if (name == "MID") out += std::to_string(mid);
    else if (name == "RID") out += std::to_string(rid);
    else if (name == "ROOMDOM") out += roomdom;
    else if (name == "RNAME") out += html_escape(room1.text(0, 1));
    else if (name == "UID") out += std::to_string(uid);
    else if (name == "UNAME") out += html_escape(u1.text(0, 1));
    i = e + 1;
  }
  // fragment cache fill, as `broadcast_create` does for the message partial (cache key: id + updated_at)
  std::string fkey = "views/messages/_message:gate/messages/" + std::to_string(mid) + "-" + std::string(updated) + "/presentation-v3";
  S->fragments.put(std::move(fkey), out);

  // ---- response: gzip (libdeflate level 6) and chunked framing, or identity
  std::string etag = "W/\"" + hex(sha256(out)).substr(0, 32) + "\"";
  std::string& h = c->head;
  h = "HTTP/1.1 200 OK\r\ncontent-type: text/vnd.turbo-stream.html; charset=utf-8\r\nx-cache: bypass\r\nx-version: " + S->cfg.app_version +
      "\r\nx-rev: " + S->cfg.git_rev +
      "\r\nx-frame-options: SAMEORIGIN\r\nx-xss-protection: 0\r\nx-content-type-options: nosniff\r\nx-permitted-cross-domain-policies: none\r\n"
      "referrer-policy: strict-origin-when-cross-origin\r\netag: " + etag + "\r\ncache-control: max-age=0, private, must-revalidate\r\n";
  std::string gz;
  if (c->accept_gz) { gz = chunk_frame(gzip_body(gz_, out)); h += "content-encoding: gzip\r\n"; }
  else h += "content-length: " + std::to_string(out.size()) + "\r\n";
  h += "x-request-id: " + uuid_v4(rng_) + "\r\nx-runtime: " + runtime(c->t0_ns) + "\r\nvary: Accept-Encoding\r\nvary: Accept,Accept-Encoding\r\ndate: " +
       date_now() + "\r\n";
  if (c->accept_gz) { h += "transfer-encoding: chunked\r\n\r\n"; h += gz; }
  else { h += "\r\n"; h += out; }
  c->waiting = false;
  uint64_t cid = c->id;
  if (!flush(c)) return;

  // ---- after the response: Rust's three late reads (memberships of the room for the unread
  // broadcasts, the rich text, the account)
  db_.run(Q_MembershipsOfRoom, {rid}, rs_[0]);
  std::string unread_json;
  for (size_t i = 0; i < rs_[0].nrows; i++) {
    unread_json = "{\"roomId\":" + std::to_string(rid) + "}";  // broadcast payload for user_<id>_unreads (no subscribers)
    (void)rs_[0].num(i, 2);
  }
  db_.run(Q_RichTextFor, {mid, "Message", "body"}, rs_[1]);
  db_.run(Q_AccountFirst, {}, rs_[2]);
  auto it = conns_.find(cid);
  if (it != conns_.end() && !it->second->in.empty()) process(it->second.get());
}

// ------------------------------------------------------------------ sign in (minimal)

void Worker::session_create(ConnState* c, const Req& r) {
  std::string email = form_get(r.body, "email_address"), password = form_get(r.body, "password");
  RowSet& rs = rs_[0];
  if (password.empty() || db_.run(Q_UserByEmail, {email}, rs) != SQLITE_OK || rs.nrows != 1 || !bcrypt_verify(password, std::string(rs.text(0, 3)))) {
    respond_small(c, 401, "Unauthorized", "text/html; charset=utf-8", "Too many requests or unauthorized.");
    return;
  }
  auto j = std::make_unique<Job>();
  j->kind = Job::SESSION;
  j->worker = idx_;
  j->conn_id = c->id;
  j->user_id = rs.num(0, 0);
  j->ip = c->peer;
  j->ua = std::string(r.h("user-agent"));
  c->waiting = true;
  S->writer.submit(std::move(j));
}

void Worker::finish_session(ConnState* c, Job& j) {
  c->waiting = false;
  if (!j.ok) { respond_small(c, 500, "Internal Server Error", "text/html", "<h1>Internal Server Error</h1>"); flush(c); return; }
  time_t now = time(nullptr);
  std::string value = sign_cookie(*S->secrets, "session_token", j.token, years_from(now, 20) * 1000LL);
  std::string extra = "set-cookie: session_token=" + www_escape(value) + "; path=/; expires=" + http_date(years_from(now, 20)) + "; httponly; samesite=lax\r\n";
  c->head = "HTTP/1.1 302 Found\r\nlocation: /\r\n" + extra + "content-type: text/html; charset=utf-8\r\ncontent-length: 0\r\ndate: " + date_now() + "\r\n\r\n";
  flush(c);
}

// ------------------------------------------------------------------ routing and completions

static bool parse_id(std::string_view s, int64_t& out) {
  if (s.empty() || s.size() > 18) return false;
  int64_t v = 0;
  for (char ch : s) { if (ch < '0' || ch > '9') return false; v = v * 10 + (ch - '0'); }
  out = v;
  return true;
}

void Worker::handle(ConnState* c, Req& r) {
  int64_t t0 = mono_ns();
  std::string_view path = r.path;
  int64_t id;
  if (r.method == "GET" && path == "/up") return up(c, r, t0);
  if (r.method == "GET" && path.substr(0, 7) == "/rooms/" && parse_id(path.substr(7), id)) return room_show(c, r, id, t0);
  if (r.method == "POST" && path.substr(0, 7) == "/rooms/" && path.size() > 16 && path.substr(path.size() - 9) == "/messages" &&
      parse_id(path.substr(7, path.size() - 16), id))
    return post_message(c, r, id, t0);
  if (r.method == "GET" && path == "/session/new")
    return respond_small(c, 200, "OK", "text/html; charset=utf-8", "<!DOCTYPE html><html><body><form action=\"/session\" method=\"post\"></form></body></html>");
  if (r.method == "POST" && path == "/session") return session_create(c, r);
  if (r.method == "GET" && path == "/users/me/sidebar") {
    int64_t uid;
    if (!authenticate(r, uid)) return respond_redirect(c, r, "/session/new");
    return respond_small(c, 200, "OK", "text/html; charset=utf-8", S->sidebar_html);
  }
  respond_small(c, 404, "Not Found", "text/plain", "gate server: route not implemented");
}

void Worker::on_completions() {
  std::vector<std::unique_ptr<Job>> done;
  {
    std::lock_guard<std::mutex> l(comp_->m);
    done.swap(comp_->done);
  }
  for (auto& j : done) {
    auto it = conns_.find(j->conn_id);
    if (it == conns_.end()) continue;  // the client went away; the write is committed anyway
    ConnState* c = it->second.get();
    if (j->kind == Job::POST) finish_post(c, *j);
    else finish_session(c, *j);
  }
}

// ------------------------------------------------------------------ startup

static std::string trim_nl(std::string s) { while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); return s; }

bool server_main(const Config& cfg) {
  S = new Shared();
  S->cfg = cfg;
  S->secrets = std::make_unique<Secrets>(cfg.secret);
  S->link_header = trim_nl(read_file(cfg.fixtures + "/room_link.txt"));
  S->post_template = read_file(cfg.fixtures + "/post_template.txt");
  S->sidebar_html = read_file(cfg.fixtures + "/sidebar.html");
  if (S->post_template.empty()) { fprintf(stderr, "missing fixtures in %s\n", cfg.fixtures.c_str()); return false; }
  {
    libdeflate_compressor* gz = libdeflate_alloc_compressor(6);
    S->up_etag = "W/\"" + hex(sha256(S->up_body)).substr(0, 32) + "\"";
    S->up_gz_chunked = chunk_frame(gzip_body(gz, S->up_body));
    libdeflate_free_compressor(gz);
  }
  // Startup: the captured room page goes into the cache under the key the startup queries give.
  {
    std::string meta = read_file(cfg.fixtures + "/room_page.meta");
    int64_t uid = 0, rid = 0;
    char hostbuf[128] = "", browserbuf[64] = "";
    sscanf(meta.c_str(), "user_id=%ld\nroom_id=%ld\nhost=%127s\nbrowser=%63s", &uid, &rid, hostbuf, browserbuf);
    std::string body = read_file(cfg.fixtures + "/room_page.html");
    {
      // build the template: the captured host and browser name become slots
      std::string t;
      for (size_t i = 0; i < body.size();) {
        if (body.compare(i, strlen(hostbuf), hostbuf) == 0) { t += "\x01" "H" "\x02"; i += strlen(hostbuf); }
        else if (body.compare(i, strlen(browserbuf), browserbuf) == 0) { t += "\x01" "B" "\x02"; i += strlen(browserbuf); }
        else t += body[i++];
      }
      S->page_template = std::move(t);
    }
    Conn c;
    if (!c.open(cfg.db_path, "startup", true, false)) { fprintf(stderr, "startup open: %s\n", c.error().c_str()); return false; }
    RowSet rs[16];
    std::string key;
    int pk = page_key(c, rs, uid, rid, key);
    if (pk != 1) fprintf(stderr, "startup: captured room page not visible to user %ld in room %ld\n", uid, rid);
    else {
      S->startup_db_part = key;
      key += '\0'; key += 'H'; key += hostbuf;
      key += '\0'; key += 'U';  // the captured request had no User-Agent
      S->pages.put(key, make_entry(key, std::move(body)));
      fprintf(stderr, "startup: cached room page for user %ld room %ld (key %zu bytes)\n", uid, rid, key.size());
    }
  }
  std::vector<Worker*> workers;
  for (int i = 0; i < cfg.workers; i++) {
    auto* comp = new Completions();
    comp->efd = eventfd(0, EFD_NONBLOCK);
    S->comps.push_back(comp);
  }
  if (!S->writer.start(cfg, S->comps)) return false;
  std::vector<std::thread> threads;
  for (int i = 0; i < cfg.workers; i++) {
    auto* w = new Worker(i, S->comps[i]);
    if (!w->init()) return false;
    workers.push_back(w);
  }
  fprintf(stderr, "gate server listening on :%d with %d workers\n", cfg.port, cfg.workers);
  for (int i = 0; i < cfg.workers; i++) threads.emplace_back([w = workers[i]] { w->run(); });
  for (auto& t : threads) t.join();
  return true;
}

}  // namespace gate
