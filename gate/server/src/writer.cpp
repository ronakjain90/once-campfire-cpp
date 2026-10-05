// The writer thread: group commit (up to 64 writes per transaction, one SAVEPOINT each), then the
// per-write after-commit work, then replies to the workers through queue + eventfd.
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "gate.hpp"

namespace gate {

static constexpr int kGroup = 64;
static constexpr int kAutocheckpointPages = 1000;  // SQLite default, kept by Rails and the Rust app
static constexpr int kWalLimitPages = 10000;       // Rust: WAL_LIMIT_PAGES

static std::atomic<int> g_wal_pages{0};
static int wal_hook(void*, sqlite3*, const char*, int pages) {
  g_wal_pages.store(pages, std::memory_order_relaxed);
  return SQLITE_OK;
}

std::string html_escape(std::string_view s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&#39;"; break;
      default: o += c;
    }
  }
  return o;
}

std::string plain_text_of_html(std::string_view h) {
  std::string o;
  for (size_t i = 0; i < h.size(); i++) {
    if (h[i] == '<') {
      size_t e = h.find('>', i);
      if (e == std::string_view::npos) break;
      i = e;
    } else if (h[i] == '&') {
      size_t e = h.find(';', i);
      std::string_view ent = e == std::string_view::npos ? "" : h.substr(i, e - i + 1);
      if (ent == "&amp;") { o += '&'; i = e; }
      else if (ent == "&lt;") { o += '<'; i = e; }
      else if (ent == "&gt;") { o += '>'; i = e; }
      else if (ent == "&quot;") { o += '"'; i = e; }
      else if (ent == "&#39;") { o += '\''; i = e; }
      else o += '&';
    } else o += h[i];
  }
  return o;
}

bool Writer::start(const Config& cfg, std::vector<Completions*> workers) {
  cfg_ = cfg;
  workers_ = std::move(workers);
  if (!conn_.open(cfg.db_path, "writer", false, cfg.trace)) { fprintf(stderr, "writer open: %s\n", conn_.error().c_str()); return false; }
  if (!cp_conn_.open(cfg.db_path, "checkpointer", false, false)) return false;
  // Replaces the automatic checkpoint (wal_autocheckpoint=0 in effect): the hook only notes the WAL size.
  sqlite3_wal_hook(conn_.db(), wal_hook, nullptr);
  std::thread([this] { checkpointer_loop(); }).detach();
  std::thread([this] { set_trace_thread_id(100); loop(); }).detach();
  return true;
}

void Writer::submit(std::unique_ptr<Job> j) {
  {
    std::lock_guard<std::mutex> l(m_);
    q_.push_back(std::move(j));
  }
  cv_.notify_one();
}

void Writer::checkpointer_loop() {
  set_trace_thread_id(101);
  for (;;) {
    {
      std::unique_lock<std::mutex> l(cp_m_);
      cp_cv_.wait(l, [&] { return cp_wake_; });
      cp_wake_ = false;
    }
    std::lock_guard<std::mutex> r(running_m_);
    RowSet rs;
    sqlite3_exec(cp_conn_.db(), "PRAGMA wal_checkpoint(PASSIVE)", nullptr, nullptr, nullptr);
  }
}

// One message post inside its savepoint. Timestamps are taken per statement, as the Rust app does.
void Writer::run_post(Job& j, int64_t* t1out) {
  RowSet rs;
  char c1[27], c2[27], c3[27], c4[27];
  int64_t t1 = now_us();
  fmt_us(t1, c1);
  *t1out = t1;
  int rc = conn_.run(Q_InsertMessage, {j.cid, std::string_view(c1, 26), j.user_id, j.room_id, std::string_view(c1, 26)}, rs);
  if (rc != SQLITE_OK || rs.nrows != 1) { j.err = conn_.error(); return; }
  j.message_id = rs.num(0, 0);
  fmt_us(now_us(), c2);
  rc = conn_.run(Q_InsertRichText, {j.body_html, std::string_view(c2, 26), "body", j.message_id, "Message", std::string_view(c2, 26)}, rs);
  if (rc != SQLITE_OK) { j.err = conn_.error(); return; }
  fmt_us(now_us(), c3);
  rc = conn_.run(Q_TouchMessage, {std::string_view(c3, 26), j.message_id}, rs);
  if (rc != SQLITE_OK) { j.err = conn_.error(); return; }
  fmt_us(now_us(), c4);
  rc = conn_.run(Q_TouchRoom, {std::string_view(c4, 26), j.room_id}, rs);
  if (rc != SQLITE_OK) { j.err = conn_.error(); return; }
  j.ok = true;
}

void Writer::loop() {
  std::vector<std::unique_ptr<Job>> batch;
  for (;;) {
    batch.clear();
    {
      std::unique_lock<std::mutex> l(m_);
      cv_.wait(l, [&] { return !q_.empty(); });
      while (!q_.empty() && (int)batch.size() < kGroup) { batch.push_back(std::move(q_.front())); q_.pop_front(); }
    }
    std::vector<int64_t> t1s(batch.size(), 0);
    bool began = conn_.exec("BEGIN IMMEDIATE") == SQLITE_OK;
    for (size_t i = 0; i < batch.size(); i++) {
      Job& j = *batch[i];
      if (!began) { j.err = "begin: " + conn_.error(); continue; }
      conn_.exec("SAVEPOINT gate_write");
      if (j.kind == Job::POST) run_post(j, &t1s[i]);
      else {
        RowSet rs;
        char ts[27];
        int64_t t = now_us();
        fmt_us(t, ts);
        const char* alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
        unsigned char rnd[24];
        FILE* f = fopen("/dev/urandom", "rb");
        if (f) { size_t n = fread(rnd, 1, 24, f); (void)n; fclose(f); }
        j.token.clear();
        for (int k = 0; k < 24; k++) j.token += alphabet[rnd[k] % 58];
        int rc = conn_.run(Q_InsertSession, {std::string_view(ts, 26), j.ip, std::string_view(ts, 26), j.token, std::string_view(ts, 26), j.ua, j.user_id}, rs);
        if (rc != SQLITE_OK) j.err = conn_.error(); else j.ok = true;
      }
      if (j.ok) conn_.exec("RELEASE gate_write");
      else { conn_.exec("ROLLBACK TO gate_write"); conn_.exec("RELEASE gate_write"); }
    }
    if (began && conn_.exec("COMMIT") != SQLITE_OK) {
      for (auto& j : batch) { j->ok = false; j->err = "commit: " + conn_.error(); }
      conn_.exec("ROLLBACK");
    }
    // after-commit work, in order, outside any transaction
    for (size_t i = 0; i < batch.size(); i++) {
      Job& j = *batch[i];
      if (j.kind != Job::POST || !j.ok) continue;
      RowSet rs;
      int rc = conn_.run(Q_RichTextFor, {j.message_id, "Message", "body"}, rs);
      if (rc != SQLITE_OK || rs.nrows != 1) { j.err = "rich text read"; j.ok = false; continue; }
      std::string plain = plain_text_of_html(rs.text(0, 2));
      if (conn_.run(Q_FtsInsert, {j.message_id, plain}, rs) != SQLITE_OK) { j.err = "fts: " + conn_.error(); j.ok = false; continue; }
      int64_t t5 = now_us();
      char unread[27], upd[27], cutoff[27];
      fmt_us(t1s[i], unread);
      fmt_us(t5, upd);
      fmt_us(t5 - 60 * 1000000LL, cutoff);
      if (conn_.run(Q_UpdateUnread, {std::string_view(unread, 26), std::string_view(upd, 26), j.room_id, "invisible", std::string_view(cutoff, 26), j.user_id}, rs) != SQLITE_OK) {
        j.err = "memberships: " + conn_.error(); j.ok = false;
      }
    }
    // checkpoints are driven by the WAL size, as in the Rust app
    int pages = g_wal_pages.exchange(0);
    if (pages >= kWalLimitPages) {
      std::lock_guard<std::mutex> r(running_m_);
      sqlite3_exec(conn_.db(), "PRAGMA wal_checkpoint(RESTART)", nullptr, nullptr, nullptr);
      cp_woken_at_ = 0;
    } else if (pages > 0) {
      if (pages < cp_woken_at_) cp_woken_at_ = 0;
      if (pages - cp_woken_at_ >= kAutocheckpointPages) {
        { std::lock_guard<std::mutex> l(cp_m_); cp_wake_ = true; }
        cp_cv_.notify_one();
        cp_woken_at_ = pages;
      }
    }
    // reply: one queue push and one eventfd write per worker
    std::vector<bool> touched(workers_.size(), false);
    for (auto& jp : batch) {
      int w = jp->worker;
      {
        std::lock_guard<std::mutex> l(workers_[w]->m);
        workers_[w]->done.push_back(std::move(jp));
      }
      touched[w] = true;
    }
    for (size_t w = 0; w < workers_.size(); w++)
      if (touched[w]) { uint64_t one = 1; (void)!write(workers_[w]->efd, &one, 8); }
  }
}

}  // namespace gate
