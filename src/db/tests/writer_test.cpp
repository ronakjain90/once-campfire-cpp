// Tests of the writer: group commit, after-commit queue, change events, checkpoints.
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#include "db/tests/test_util.hpp"

namespace campfire::db {
using namespace testing;
using namespace std::chrono_literals;

namespace {

Task<Result<std::int64_t>> insert_item(Database& db, Scheduler& s, std::string name, std::vector<std::string>* order,
                                       std::thread::id* after_thread, bool should_fail = false) {
  auto result = co_await db.write(s, [&](Tx& tx) -> Result<std::int64_t> {
    if (should_fail) {
      return fail(Errc::InvalidArgument, "asked to fail");
    }
    if (auto r = tx.conn().exec(InsertItem, name, std::optional<std::int64_t>(1)); !r) {
      return std::unexpected(r.error());
    }
    const std::int64_t id = tx.conn().last_insert_rowid();
    tx.changed(schema::Table::Messages, id);
    tx.after_commit([order, name, after_thread] {
      if (order != nullptr) {
        order->push_back(name + ":1");
      }
      if (after_thread != nullptr) {
        *after_thread = std::this_thread::get_id();
      }
    });
    tx.after_commit([order, name] {
      if (order != nullptr) {
        order->push_back(name + ":2");
      }
    });
    return id;
  });
  co_return result;
}

std::int64_t count(Database& db) {
  auto reader = std::move(*db.open_reader());
  Arena arena;
  return **reader.first(CountItems, arena);
}

std::unique_ptr<Database> open_db(const TempDir& dir, DatabaseOptions options = {}) {
  auto db = Database::open(dir.file("a.sqlite3"), std::move(options));
  REQUIRE(db);
  create_items_table(**db);
  return std::move(*db);
}

}  // namespace

TEST_CASE("a write returns the value and runs the after-commit queue on the caller thread") {
  const TempDir dir;
  auto db = open_db(dir);
  QueueScheduler sched;
  std::vector<std::string> order;
  std::thread::id after{};
  auto id = run_task(sched, insert_item(*db, sched, "a", &order, &after));
  REQUIRE(id);
  CHECK(*id == 1);
  CHECK(after == std::this_thread::get_id());
  CHECK(order == std::vector<std::string>{"a:1", "a:2"});
  CHECK(count(*db) == 1);
}

TEST_CASE("group commit: a failing write in the middle does not stop the others") {
  const TempDir dir;
  auto db = open_db(dir);
  QueueScheduler sched;
  std::vector<std::string> order;
  std::vector<Task<Result<std::int64_t>>> tasks;
  tasks.reserve(4);
  tasks.push_back(insert_item(*db, sched, "a", &order, nullptr));
  tasks.push_back(insert_item(*db, sched, "b", &order, nullptr, true));
  tasks.push_back(insert_item(*db, sched, "c", &order, nullptr));
  tasks.push_back(insert_item(*db, sched, "d", &order, nullptr));
  for (auto& t : tasks) {
    t.start();  // each one submits and waits
  }
  while (!std::ranges::all_of(tasks, [](auto& t) { return t.done(); })) {
    sched.run_one_for(10s);
  }
  CHECK(tasks[0].result().has_value());
  auto failed = tasks[1].result();
  REQUIRE(!failed);
  CHECK(failed.error().message == "asked to fail");
  CHECK(tasks[2].result().has_value());
  CHECK(tasks[3].result().has_value());
  CHECK(count(*db) == 3);
  // Each write runs its own queue in order. The failed write has no queue.
  CHECK(std::ranges::count(order, "b:1") == 0);
  for (const char* n : {"a", "c", "d"}) {
    const auto first = std::ranges::find(order, std::string(n) + ":1");
    const auto second = std::ranges::find(order, std::string(n) + ":2");
    REQUIRE(first != order.end());
    REQUIRE(second != order.end());
    CHECK(first < second);
  }
  CHECK(db->stats().commits <= 4);
}

TEST_CASE("an exception in a write rolls back that write only") {
  const TempDir dir;
  auto db = open_db(dir);
  QueueScheduler sched;
  auto task = [](Database& d, Scheduler& s) -> Task<Result<int>> {
    co_return co_await d.write(s, [](Tx& tx) -> int {
      REQUIRE(tx.conn().exec(InsertItem, "x", std::optional<std::int64_t>()));
      throw std::runtime_error("boom");
    });
  };
  auto r = run_task(sched, task(*db, sched));
  REQUIRE(!r);
  CHECK(r.error().message.find("boom") != std::string::npos);
  CHECK(count(*db) == 0);
  auto ok = run_task(sched, insert_item(*db, sched, "after", nullptr, nullptr));
  CHECK(ok.has_value());
  CHECK(count(*db) == 1);
}

TEST_CASE("a constraint failure rolls back to the savepoint") {
  const TempDir dir;
  auto db = open_db(dir);
  QueueScheduler sched;
  auto task = [](Database& d, Scheduler& s) -> Task<Result<int>> {
    co_return co_await d.write(s, [](Tx& tx) -> Result<int> {
      if (auto r = tx.conn().exec(InsertItem, "ok", std::optional<std::int64_t>()); !r) {
        return std::unexpected(r.error());
      }
      if (auto r = tx.conn().exec(BadInsertItem, 1, "dup"); !r) {  // id 1 exists now
        return std::unexpected(r.error());
      }
      return 1;
    });
  };
  auto r = run_task(sched, task(*db, sched));
  REQUIRE(!r);
  CHECK(r.error().code == Errc::InvalidArgument);
  CHECK(count(*db) == 0);
}

TEST_CASE("change events are published after the commit, for the writes that committed") {
  const TempDir dir;
  auto db = open_db(dir);
  QueueScheduler sched;
  std::vector<Change> seen;
  std::int64_t rows_at_publish = -1;
  const auto id = db->subscribe([&](std::span<const Change> changes) {
    seen.insert(seen.end(), changes.begin(), changes.end());
    rows_at_publish = count(*db);  // the commit is visible to a reader
  });
  REQUIRE(run_task(sched, insert_item(*db, sched, "a", nullptr, nullptr)));
  CHECK(seen == std::vector<Change>{{schema::Table::Messages, 1}});
  CHECK(rows_at_publish == 1);
  auto failed = run_task(sched, insert_item(*db, sched, "b", nullptr, nullptr, true));
  CHECK(!failed);
  CHECK(seen.size() == 1);
  db->unsubscribe(id);
  REQUIRE(run_task(sched, insert_item(*db, sched, "c", nullptr, nullptr)));
  CHECK(seen.size() == 1);
}

TEST_CASE("timestamps use the Rails format") {
  const TempDir dir;
  DatabaseOptions options;
  options.clock = TestClock::frozen_at(Timestamp::from_micros(1'700'000'000'123'456));
  auto db = open_db(dir, std::move(options));
  QueueScheduler sched;
  auto task = [](Database& d, Scheduler& s) -> Task<Result<std::string>> {
    co_return co_await d.write(s, [](Tx& tx) -> std::string { return tx.now_db(); });
  };
  auto r = run_task(sched, task(*db, sched));
  REQUIRE(r);
  CHECK(*r == "2023-11-14 22:13:20.123456");
}

TEST_CASE("64 concurrent submitters") {
  const TempDir dir;
  auto db = open_db(dir);
  constexpr int kThreads = 64;
  constexpr int kEach = 20;
  std::atomic<int> after_calls{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  std::atomic<int> failures{0};
  // QueueScheduler::post notifies its condition variable after it unlocks the mutex (src/core, not
  // T7). A scheduler that is destroyed at once can race with that call. Keep the schedulers until
  // all threads end.
  std::mutex keep_mutex;
  std::vector<std::unique_ptr<QueueScheduler>> keep;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      auto owned = std::make_unique<QueueScheduler>();
      QueueScheduler& sched = *owned;
      for (int i = 0; i < kEach; ++i) {
        auto task = [](Database& d, Scheduler& s, std::atomic<int>& calls,
                       std::string name) -> Task<Result<std::int64_t>> {
          co_return co_await d.write(s, [&](Tx& tx) -> Result<std::int64_t> {
            if (auto r = tx.conn().exec(InsertItem, name, std::optional<std::int64_t>(1)); !r) {
              return std::unexpected(r.error());
            }
            tx.after_commit([&calls] { calls.fetch_add(1); });
            return tx.conn().last_insert_rowid();
          });
        };
        auto r = run_task(sched, task(*db, sched, after_calls, "t" + std::to_string(t)));
        if (!r) {
          failures.fetch_add(1);
        }
      }
      const std::lock_guard lock(keep_mutex);
      keep.push_back(std::move(owned));
    });
  }
  for (auto& th : threads) {
    th.join();
  }
  CHECK(failures.load() == 0);
  CHECK(after_calls.load() == kThreads * kEach);
  CHECK(count(*db) == kThreads * kEach);
  const auto stats = db->stats();
  CHECK(stats.writes == static_cast<std::uint64_t>(kThreads * kEach));
  CHECK(stats.commits <= stats.writes);
}

TEST_CASE("checkpoints run when the WAL grows") {
  const TempDir dir;
  DatabaseOptions options;
  options.autocheckpoint_pages = 20;
  options.wal_limit_pages = 120;
  auto db = open_db(dir, std::move(options));
  QueueScheduler sched;
  const std::string big(3000, 'z');
  auto task = [](Database& d, Scheduler& s, std::string_view text) -> Task<Result<int>> {
    co_return co_await d.write(s, [&](Tx& tx) -> Result<int> {
      if (auto r = tx.conn().exec(InsertItem, text, std::optional<std::int64_t>()); !r) {
        return std::unexpected(r.error());
      }
      return 1;
    });
  };
  for (int i = 0; i < 600; ++i) {
    REQUIRE(run_task(sched, task(*db, sched, big)));
  }
  // The checkpointer thread runs on its own: wait for it.
  for (int i = 0; i < 200 && db->stats().passive_checkpoints == 0; ++i) {
    std::this_thread::sleep_for(10ms);
  }
  const auto stats = db->stats();
  CHECK(stats.passive_checkpoints >= 1);
  CHECK(stats.restart_checkpoints >= 1);
  CHECK(count(*db) == 600);
  std::error_code ec;
  const auto wal = std::filesystem::file_size(dir.file("a.sqlite3-wal"), ec);
  CHECK(wal < 4096ull * 400);  // the WAL stays near the limit
}

}  // namespace campfire::db
