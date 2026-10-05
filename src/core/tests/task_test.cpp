// Tests of core/task.hpp and core/scheduler.hpp.
#include "core/task.hpp"

#include <doctest.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace campfire;

namespace {

Task<int> leaf(int v) {
  co_return v;
}

Task<int> middle(int v) {
  const int a = co_await leaf(v);
  const int b = co_await leaf(v + 1);
  co_return a + b;
}

Task<int> outer() {
  int sum = 0;
  for (int i = 0; i < 3; ++i) {
    sum += co_await middle(i * 10);
  }
  co_return sum;
}

Task<int> thrower() {
  throw std::runtime_error("boom");
  co_return 0;
}

Task<std::string> catches() {
  try {
    (void)co_await thrower();
  } catch (const std::runtime_error& e) {
    co_return std::string("caught ") + e.what();
  }
  co_return "not reached";
}

Task<int> propagates() {
  co_return co_await thrower();
}

Task<void> void_task(int& out) {
  out = 5;
  co_return;
}

Task<std::string> move_only_value() {
  co_return std::string(100, 'x');
}

Task<int> deep(int n) {
  if (n == 0) {
    co_return 0;
  }
  co_return 1 + co_await deep(n - 1);
}

Task<std::thread::id> await_offload(Scheduler& owner, std::thread::id& worker_id, std::thread::id& before) {
  before = std::this_thread::get_id();
  auto [completion, setter] = make_completion<int>(owner);
  std::thread worker([setter, &worker_id] {
    worker_id = std::this_thread::get_id();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    setter.set_value(41);
  });
  const int v = co_await std::move(completion);
  worker.join();
  CHECK(v == 41);
  co_return std::this_thread::get_id();
}

}  // namespace

TEST_CASE("a Task is lazy") {
  int out = 0;
  Task<void> t = void_task(out);
  CHECK(out == 0);
  CHECK_FALSE(t.done());
  t.start();
  CHECK(out == 5);
  CHECK(t.done());
  t.result();
}

TEST_CASE("nested co_await of Tasks") {
  Task<int> t = outer();
  t.start();
  REQUIRE(t.done());
  // middle(v) = v + (v + 1); for v = 0, 10, 20: 1 + 21 + 41
  CHECK(t.result() == 63);
}

TEST_CASE("a deep chain does not overflow the stack") {
  Task<int> t = deep(100000);
  t.start();
  REQUIRE(t.done());
  CHECK(t.result() == 100000);
}

TEST_CASE("exceptions surface at the awaiter") {
  Task<std::string> c = catches();
  c.start();
  CHECK(c.result() == "caught boom");

  Task<int> p = propagates();
  p.start();
  REQUIRE(p.done());
  CHECK_THROWS_WITH_AS(p.result(), "boom", std::runtime_error);

  Task<int> root = thrower();
  root.start();
  CHECK_THROWS_AS(root.result(), std::runtime_error);
}

TEST_CASE("Task values move out and Tasks move") {
  Task<std::string> t = move_only_value();
  Task<std::string> moved = std::move(t);
  CHECK_FALSE(t.valid());  // NOLINT(bugprone-use-after-move): the moved-from state is specified
  moved.start();
  CHECK(moved.result().size() == 100);
}

TEST_CASE("a Task that never ran, or never finished, frees its frame") {
  {
    Task<int> unused = outer();
  }
  QueueScheduler scheduler;
  std::thread::id a;
  std::thread::id b;
  {
    // Suspended on a Completion that is done later: finish it, then destroy.
    Task<std::thread::id> t = await_offload(scheduler, a, b);
    t.start();
    while (!t.done()) {
      scheduler.run_one_for(std::chrono::seconds(5));
    }
  }
  {
    Task<int> half = middle(1);
  }
}

TEST_CASE("a Completion from another thread resumes on the owner thread") {
  QueueScheduler scheduler;
  std::thread::id worker;
  std::thread::id before;
  Task<std::thread::id> t = await_offload(scheduler, worker, before);
  t.start();
  CHECK_FALSE(t.done());  // suspended: the other thread has not finished
  while (!t.done()) {
    REQUIRE(scheduler.run_one_for(std::chrono::seconds(5)));
  }
  const std::thread::id after = t.result();
  CHECK(before == std::this_thread::get_id());
  CHECK(after == std::this_thread::get_id());
  CHECK(worker != std::this_thread::get_id());
}

TEST_CASE("a Completion that is already done does not suspend") {
  QueueScheduler scheduler;
  auto [completion, setter] = make_completion<std::string>(scheduler);
  setter.set_value("early");
  const auto run = [](Completion<std::string> c) -> Task<std::string> { co_return co_await std::move(c); };
  Task<std::string> t = run(std::move(completion));
  t.start();
  REQUIRE(t.done());
  CHECK(t.result() == "early");
  CHECK(scheduler.run_pending() == 0);
}

TEST_CASE("a Completion carries an exception and void") {
  QueueScheduler scheduler;
  {
    auto [completion, setter] = make_completion<int>(scheduler);
    const auto run = [](Completion<int> c) -> Task<int> { co_return co_await std::move(c); };
    Task<int> t = run(std::move(completion));
    t.start();
    std::thread other([setter] { setter.set_exception(std::make_exception_ptr(std::logic_error("late"))); });
    while (!t.done()) {
      REQUIRE(scheduler.run_one_for(std::chrono::seconds(5)));
    }
    other.join();
    CHECK_THROWS_AS(t.result(), std::logic_error);
  }
  {
    auto [completion, setter] = make_completion<void>(scheduler);
    const auto run = [](Completion<void> c) -> Task<int> {
      co_await std::move(c);
      co_return 7;
    };
    Task<int> t = run(std::move(completion));
    t.start();
    std::thread other([setter] { setter.set_value(); });
    while (!t.done()) {
      REQUIRE(scheduler.run_one_for(std::chrono::seconds(5)));
    }
    other.join();
    CHECK(t.result() == 7);
  }
}

TEST_CASE("many Tasks complete from many threads") {
  QueueScheduler scheduler;
  constexpr int kCount = 64;
  std::vector<Task<int>> tasks;
  std::vector<std::thread> threads;
  const auto run = [](Completion<int> c) -> Task<int> { co_return (co_await std::move(c)) * 2; };
  for (int i = 0; i < kCount; ++i) {
    auto [completion, setter] = make_completion<int>(scheduler);
    tasks.push_back(run(std::move(completion)));
    tasks.back().start();
    threads.emplace_back([setter, i] { setter.set_value(i); });
  }
  int finished = 0;
  while (finished < kCount) {
    scheduler.run_one_for(std::chrono::seconds(5));
    finished = 0;
    for (const Task<int>& t : tasks) {
      finished += t.done() ? 1 : 0;
    }
  }
  for (std::thread& t : threads) {
    t.join();
  }
  for (int i = 0; i < kCount; ++i) {
    CHECK(tasks[static_cast<std::size_t>(i)].result() == i * 2);
  }
}

TEST_CASE("Yield resumes from the queue") {
  QueueScheduler scheduler;
  int step = 0;
  const auto run = [&]() -> Task<void> {
    step = 1;
    co_await Yield(scheduler);
    step = 2;
  };
  Task<void> t = run();
  t.start();
  CHECK(step == 1);
  CHECK(scheduler.run_pending() == 1);
  CHECK(step == 2);
  CHECK(t.done());
}
