// Shared declarations of the gate server.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crypto.hpp"
#include "db.hpp"

namespace gate {

struct Config {
  int port = 80;
  std::string db_path = "/rails/storage/db/production.sqlite3";
  std::string secret;
  std::string app_version, git_rev;
  std::string fixtures = "/app/fixtures";
  bool trace = false;
  int workers = 1;
};

// A unit of work for the writer thread.
struct Job {
  enum Kind { POST, SESSION } kind = POST;
  int worker = 0;
  uint64_t conn_id = 0;
  // POST input
  int64_t room_id = 0, user_id = 0;
  std::string cid, body_html;
  // SESSION input
  std::string ip, ua;
  // output
  bool ok = false;
  std::string err;
  int64_t message_id = 0;
  std::string token;
};

// Completed jobs travel from the writer to a worker through this queue and an eventfd.
struct Completions {
  std::mutex m;
  std::vector<std::unique_ptr<Job>> done;
  int efd = -1;
};

class Writer {
 public:
  bool start(const Config& cfg, std::vector<Completions*> workers);
  void submit(std::unique_ptr<Job> j);

 private:
  void loop();
  void run_post(Job& j, int64_t* t1);
  void checkpointer_loop();
  Config cfg_;
  Conn conn_;
  std::vector<Completions*> workers_;
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<std::unique_ptr<Job>> q_;
  // checkpointer
  std::mutex cp_m_, running_m_;
  std::condition_variable cp_cv_;
  bool cp_wake_ = false;
  int cp_woken_at_ = 0;
  Conn cp_conn_;
};

std::string plain_text_of_html(std::string_view html);
std::string html_escape(std::string_view s);
bool bcrypt_verify(const std::string& password, const std::string& digest);

}  // namespace gate
