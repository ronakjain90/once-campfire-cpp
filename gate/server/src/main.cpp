#include <sched.h>

#include <cstdlib>
#include <cstring>

#include "gate.hpp"

namespace gate { bool server_main(const Config&); }

int main() {
  gate::Config cfg;
  auto env = [](const char* n, const char* d) { const char* v = getenv(n); return std::string(v && *v ? v : d); };
  cfg.port = atoi(env("HTTP_PORT", "80").c_str());
  cfg.secret = env("SECRET_KEY_BASE", "");
  cfg.app_version = env("APP_VERSION", "");
  cfg.git_rev = env("GIT_REVISION", "");
  cfg.db_path = "/rails/storage/db/" + env("RAILS_ENV", "production") + ".sqlite3";
  cfg.fixtures = env("GATE_FIXTURES", "/app/fixtures");
  cfg.trace = getenv("GATE_SQL_TRACE") != nullptr;
  cpu_set_t set;
  CPU_ZERO(&set);
  sched_getaffinity(0, sizeof set, &set);
  cfg.workers = CPU_COUNT(&set);
  if (const char* w = getenv("GATE_WORKERS")) cfg.workers = atoi(w);
  if (cfg.secret.empty()) { fprintf(stderr, "SECRET_KEY_BASE is required\n"); return 1; }
  return gate::server_main(cfg) ? 0 : 1;
}
