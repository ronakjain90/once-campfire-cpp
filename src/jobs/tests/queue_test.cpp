// Tests of the job queues. Rust: crates/campfire/src/jobs/tests.rs.
#include "jobs/queue.hpp"

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

using namespace campfire::jobs;
using namespace std::chrono_literals;

TEST_CASE("a queued job runs on a thread of its kind") {
  JobQueues queues(1);
  std::promise<std::thread::id> ran;
  auto future = ran.get_future();
  REQUIRE(queues.enqueue(JobKind::PushMessage, [&ran] { ran.set_value(std::this_thread::get_id()); }));
  REQUIRE(future.wait_for(5s) == std::future_status::ready);
  CHECK(future.get() != std::this_thread::get_id());
}

TEST_CASE("a slow kind does not hold up another kind") {
  JobQueues queues(1);
  std::promise<void> release;
  std::shared_future<void> gate = release.get_future().share();
  REQUIRE(queues.enqueue(JobKind::DeliverWebhook, [gate] { gate.wait(); }));
  std::promise<void> done;
  auto future = done.get_future();
  REQUIRE(queues.enqueue(JobKind::PushMessage, [&done] { done.set_value(); }));
  CHECK(future.wait_for(5s) == std::future_status::ready);
  release.set_value();
}

TEST_CASE("a full queue drops the new job") {
  JobQueues queues(1, 2);
  std::promise<void> release;
  std::shared_future<void> gate = release.get_future().share();
  std::atomic<bool> started{false};
  REQUIRE(queues.enqueue(JobKind::PurgeBlob, [gate, &started] {
    started = true;
    gate.wait();
  }));
  while (!started) std::this_thread::sleep_for(1ms);
  CHECK(queues.enqueue(JobKind::PurgeBlob, [] {}));
  CHECK(queues.enqueue(JobKind::PurgeBlob, [] {}));
  CHECK_FALSE(queues.enqueue(JobKind::PurgeBlob, [] {}));
  release.set_value();
}

TEST_CASE("a job that throws does not stop the thread") {
  JobQueues queues(1);
  REQUIRE(queues.enqueue(JobKind::RemoveBannedContent, [] { throw std::runtime_error("boom"); }));
  std::promise<void> done;
  auto future = done.get_future();
  REQUIRE(queues.enqueue(JobKind::RemoveBannedContent, [&done] { done.set_value(); }));
  CHECK(future.wait_for(5s) == std::future_status::ready);
}

TEST_CASE("shutdown runs the queued jobs and then refuses new ones") {
  std::atomic<int> count{0};
  JobQueues queues(2);
  for (int i = 0; i < 20; ++i) REQUIRE(queues.enqueue(JobKind::PushMessage, [&count] { ++count; }));
  queues.shutdown(5s);
  CHECK(count == 20);
  CHECK_FALSE(queues.enqueue(JobKind::PushMessage, [] {}));
}

TEST_CASE("the thread hooks run once for each thread") {
  std::atomic<int> started{0};
  std::atomic<int> finished{0};
  {
    JobQueues queues(2, 8, {[&started] { ++started; }, [&finished] { ++finished; }});
  }
  CHECK(started == 8);
  CHECK(finished == 8);
}
