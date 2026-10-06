// Micro-benchmark of the database: reads, dependency tracking overhead, group commit.
// Usage: db_bench [directory]   (default: /var/lib/campfire-bench/t7)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "core/arena.hpp"
#include "core/scheduler.hpp"
#include "core/task.hpp"
#include "db/connection.hpp"
#include "db/database.hpp"

namespace {
using namespace campfire;
using namespace campfire::db;
using Clock = std::chrono::steady_clock;

struct Item {
  std::int64_t id;
  std::string_view name;
  std::optional<std::int64_t> n;
  static Item read(RowReader& r) { return {r.i64(0), r.text(1), r.i64_opt(2)}; }
};
const Query<void(std::string_view, std::optional<std::int64_t>)> Insert{"INSERT INTO items (name, n) VALUES (?, ?)"};
const Query<Item(std::int64_t)> Forty{"SELECT id, name, n FROM items WHERE id > ? ORDER BY id LIMIT 40"};

void must(bool ok) {
  if (!ok) {
    std::puts("database error");
    std::exit(1);
  }
}

double seconds(Clock::time_point a) {
  return std::chrono::duration<double>(Clock::now() - a).count();
}

double reads_per_second(Connection& conn, DependencyScope* scope, int iterations) {
  Arena arena;
  conn.set_scope(scope);
  const auto start = Clock::now();
  std::size_t rows = 0;
  for (int i = 0; i < iterations; ++i) {
    arena.reset();
    if (scope != nullptr) {
      scope->reset();
    }
    rows += conn.all(Forty, arena, i % 100)->size();
  }
  conn.set_scope(nullptr);
  if (rows != static_cast<std::size_t>(iterations) * 40) {
    std::puts("unexpected row count");
  }
  return iterations / seconds(start);
}

Task<Result<int>> one_write(Database& db, Scheduler& s) {
  co_return co_await db.write(s, [](Tx& tx) -> Result<int> {
    if (auto r = tx.conn().exec(Insert, "bench message body", std::optional<std::int64_t>(1)); !r) {
      return std::unexpected(r.error());
    }
    return 1;
  });
}

double writes_per_second(const std::string& path, std::size_t group, int threads, int each) {
  std::filesystem::remove(path);
  std::filesystem::remove(path + "-wal");
  std::filesystem::remove(path + "-shm");
  DatabaseOptions options;
  options.group_size = group;
  auto db = Database::open(path, options);
  {
    auto conn = Connection::open(path, Role::Writer);
    must(conn->exec_sql("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT NOT NULL, n INTEGER)").has_value());
  }
  const auto start = Clock::now();
  std::vector<std::thread> pool;
  pool.reserve(static_cast<std::size_t>(threads));
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&] {
      QueueScheduler sched;
      for (int i = 0; i < each; ++i) {
        auto task = one_write(**db, sched);
        task.start();
        while (!task.done()) {
          sched.run_one_for(std::chrono::seconds(10));
        }
      }
    });
  }
  for (auto& th : pool) {
    th.join();
  }
  const double elapsed = seconds(start);
  const auto stats = (*db)->stats();
  std::printf("  group_size=%zu threads=%d: %llu writes, %llu commits\n", group, threads,
              static_cast<unsigned long long>(stats.writes), static_cast<unsigned long long>(stats.commits));
  return threads * each / elapsed;
}
}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path dir = argc > 1 ? argv[1] : "/var/lib/campfire-bench/t7";
  std::filesystem::create_directories(dir);
  const std::string path = (dir / "bench.sqlite3").string();
  std::filesystem::remove(path);
  {
    auto conn = Connection::open(path, Role::Writer);
    must(conn->exec_sql("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT NOT NULL, n INTEGER)").has_value());
    must(conn->exec_sql("BEGIN").has_value());
    for (int i = 0; i < 200; ++i) {
      must(conn->exec(Insert, "item name of some typical length", std::optional<std::int64_t>(i)).has_value());
    }
    must(conn->exec_sql("COMMIT").has_value());
  }
  auto reader = Connection::open(path, Role::Reader);
  (void)reads_per_second(*reader, nullptr, 20000);  // warm up
  std::puts("reads of a 40-row query, one connection (best of 5, 100000 reads each):");
  double plain = 0;
  double tracked = 0;
  DependencyScope scope;
  for (int round = 0; round < 5; ++round) {
    plain = std::max(plain, reads_per_second(*reader, nullptr, 100000));
    tracked = std::max(tracked, reads_per_second(*reader, &scope, 100000));
  }
  std::printf("  untracked: %.0f reads/s\n  tracked:   %.0f reads/s\n  dependency tracking overhead: %.1f %%\n", plain,
              tracked, (plain / tracked - 1.0) * 100.0);
  std::puts("writes (VM-local file, journal_mode=wal, synchronous=normal):");
  const double grouped = writes_per_second(path, 64, 64, 200);
  std::printf("  group commit (64 submitters): %.0f writes/s\n", grouped);
  const double single = writes_per_second(path, 1, 64, 200);
  std::printf("  one transaction per write (64 submitters): %.0f writes/s\n", single);
  return 0;
}
