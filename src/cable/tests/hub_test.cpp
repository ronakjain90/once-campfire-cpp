// Hub tests, with many workers and threads (run them under tsan). Rust: crates/cable/src/pubsub.rs tests.
#include "cable/hub.hpp"

#include <doctest.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <thread>

#include "cable/protocol.hpp"
#include "cable/tests/support.hpp"

using namespace campfire::cable;

namespace {

struct Recorder final : Sink {
  void deliver(const FramePtr& f) override { frames.push_back(f); }
  void flush() override { ++flushes; }
  std::vector<FramePtr> frames;
  int flushes = 0;
};

// A worker loop: it drains when the hub wakes it.
class Workers {
 public:
  explicit Workers(unsigned n) : state_(n), hub_(n, [this](unsigned w) { wake(w); }) {
    for (unsigned w = 0; w < n; ++w) threads_.emplace_back([this, w] { loop(w); });
  }
  ~Workers() { stop(); }
  void stop() {
    if (stopped_) return;
    stopped_ = true;
    for (auto& s : state_) {
      std::lock_guard lock(s.m);
      s.quit = true;
      s.cv.notify_all();
    }
    for (auto& t : threads_) t.join();
  }
  // Runs a function on the worker thread and waits.
  void run(unsigned w, std::function<void()> fn) {
    std::promise<void> done;
    {
      std::lock_guard lock(state_[w].m);
      state_[w].tasks.push_back([&] {
        fn();
        done.set_value();
      });
      state_[w].cv.notify_all();
    }
    done.get_future().wait();
  }
  Hub& hub() { return hub_; }

 private:
  struct State {
    std::mutex m;
    std::condition_variable cv;
    bool woken = false;
    bool quit = false;
    std::vector<std::function<void()>> tasks;
  };
  void wake(unsigned w) {
    std::lock_guard lock(state_[w].m);
    state_[w].woken = true;
    state_[w].cv.notify_all();
  }
  void loop(unsigned w) {
    State& s = state_[w];
    while (true) {
      std::vector<std::function<void()>> tasks;
      bool quit = false;
      {
        std::unique_lock lock(s.m);
        s.cv.wait(lock, [&] { return s.woken || s.quit || !s.tasks.empty(); });
        s.woken = false;
        quit = s.quit;
        tasks.swap(s.tasks);
      }
      for (auto& t : tasks) t();
      hub_.drain(w);
      if (quit) return;
    }
  }
  std::deque<State> state_;
  Hub hub_;
  std::vector<std::thread> threads_;
  bool stopped_ = false;
};

}  // namespace

TEST_CASE("a broadcast reaches each subscriber once and wraps the identifier once") {
  Hub hub(2, nullptr);
  Recorder a, b, c;
  auto id1 = std::make_shared<const std::string>(R"("id1")");
  auto id2 = std::make_shared<const std::string>(R"("id2")");
  GroupId g1 = hub.subscribe(0, "s", id1, &a);
  GroupId g1b = hub.subscribe(1, "s", id1, &b);
  GroupId g2 = hub.subscribe(0, "s", id2, &c);
  CHECK(g1 == g1b);
  CHECK(g1 != g2);
  CHECK(hub.broadcast_encoded("s", "1") == 3);
  CHECK(hub.broadcast_encoded("other", "1") == 0);
  CHECK(hub.drain(0) == 2);
  CHECK(hub.drain(1) == 1);
  CHECK(hub.drain(0) == 0);
  REQUIRE(a.frames.size() == 1);
  REQUIRE(b.frames.size() == 1);
  REQUIRE(c.frames.size() == 1);
  CHECK(a.frames[0]->text() == R"({"identifier":"id1","message":1})");
  CHECK(a.frames[0].get() == b.frames[0].get());  // one frame for the group, across workers
  CHECK(c.frames[0]->text() == R"({"identifier":"id2","message":1})");
  CHECK(a.flushes == 1);
  hub.unsubscribe(0, "s", g1, &a);
  hub.unsubscribe(1, "s", g1, &b);
  hub.unsubscribe(0, "s", g2, &c);
  CHECK(hub.stream_count() == 0);
  CHECK(hub.broadcast_encoded("s", "1") == 0);
}

TEST_CASE("a sink without an identifier gets the raw payload") {
  Hub hub(1, nullptr);
  Recorder r;
  hub.subscribe(0, "raw", nullptr, &r);
  hub.broadcast_encoded("raw", R"({"type":"x"})");
  hub.drain(0);
  REQUIRE(r.frames.size() == 1);
  CHECK(r.frames[0]->text() == R"({"type":"x"})");
}

TEST_CASE("the wake call runs when a queue goes from empty to not empty") {
  std::atomic<int> wakes{0};
  Hub hub(1, [&](unsigned) { ++wakes; });
  Recorder r;
  hub.subscribe(0, "s", nullptr, &r);
  hub.broadcast_encoded("s", "1");
  hub.broadcast_encoded("s", "2");
  CHECK(wakes == 1);
  hub.drain(0);
  hub.broadcast_encoded("s", "3");
  CHECK(wakes == 2);
}

TEST_CASE("a sink may unsubscribe itself while it handles a frame") {
  Hub hub(1, nullptr);
  struct Once final : Sink {
    Hub* hub = nullptr;
    GroupId group = 0;
    int got = 0;
    void deliver(const FramePtr&) override {
      ++got;
      hub->unsubscribe(0, "s", group, this);
    }
  };
  Once a, b;
  a.hub = b.hub = &hub;
  a.group = hub.subscribe(0, "s", nullptr, &a);
  b.group = hub.subscribe(0, "s", nullptr, &b);
  hub.broadcast_encoded("s", "1");
  hub.broadcast_encoded("s", "2");
  hub.drain(0);
  CHECK(a.got == 1);
  CHECK(b.got == 1);
  CHECK(hub.stream_count() == 0);
}

TEST_CASE("frames deflate once, and short frames never") {
  auto small = Frame::make("tiny");
  CHECK(small->wire(true) == small->wire(false));
  std::string big(2000, 'z');
  auto f = Frame::make(big);
  auto plain = f->wire(false);
  auto z1 = f->wire(true);
  auto z2 = f->wire(true);
  CHECK(z1.get() == z2.get());
  CHECK(z1->size() < plain->size());
  CHECK(campfire::cable::testing::decode_server(*z1) == std::vector<std::string>{"T:" + big});
  CHECK(campfire::cable::testing::decode_server(*plain) == std::vector<std::string>{"T:" + big});
}

TEST_CASE("many workers, many subscribers, many broadcasters") {
  constexpr unsigned kWorkers = 4;
  constexpr int kSinksPerWorker = 50;
  constexpr int kBroadcasters = 3;
  constexpr int kMessages = 200;
  constexpr int kStreams = 3;
  Workers workers(kWorkers);
  Hub& hub = workers.hub();

  struct Seq final : Sink {
    void deliver(const FramePtr& f) override {
      std::string_view t = f->text();
      // {"identifier":"<stream>","message":"b<B>:<N>:xxxx..."}
      auto at = t.find("\"b");
      int b = t[at + 2] - '0';
      int n = std::stoi(std::string(t.substr(at + 4, t.find(':', at + 4) - at - 4)));
      if (n != last[b] + 1) ++gaps;
      last[b] = n;
      ++count;
      CHECK(f->wire(true)->size() > 0);  // the shared deflate runs on many threads
    }
    int last[kBroadcasters] = {-1, -1, -1};
    int count = 0;
    int gaps = 0;
  };
  std::vector<std::vector<std::unique_ptr<Seq>>> sinks(kWorkers);
  std::vector<std::vector<GroupId>> groups(kWorkers);
  for (unsigned w = 0; w < kWorkers; ++w) {
    workers.run(w, [&, w] {
      for (int i = 0; i < kSinksPerWorker; ++i) {
        sinks[w].push_back(std::make_unique<Seq>());
        groups[w].push_back(
            hub.subscribe(w, "stream", std::make_shared<const std::string>(R"("x")"), sinks[w].back().get()));
      }
    });
  }
  std::string pad(300, 'p');
  std::vector<std::thread> senders;
  senders.reserve(kBroadcasters);
  for (int b = 0; b < kBroadcasters; ++b) {
    senders.emplace_back([&, b] {
      for (int n = 0; n < kMessages; ++n) {
        std::string payload = "\"b" + std::to_string(b) + ":" + std::to_string(n) + ":" + pad + "\"";
        CHECK(hub.broadcast_encoded("stream", payload) == kWorkers * kSinksPerWorker);
      }
    });
  }
  for (auto& t : senders) t.join();
  for (unsigned w = 0; w < kWorkers; ++w) workers.run(w, [] {});  // a barrier: the queue is drained
  for (unsigned w = 0; w < kWorkers; ++w) {
    workers.run(w, [&, w] {
      for (auto& s : sinks[w]) {
        CHECK(s->count == kBroadcasters * kMessages);
        CHECK(s->gaps == 0);
      }
      for (std::size_t i = 0; i < sinks[w].size(); ++i) hub.unsubscribe(w, "stream", groups[w][i], sinks[w][i].get());
    });
  }
  CHECK(hub.stream_count() == 0);
  (void)kStreams;
}

TEST_CASE("subscribe and unsubscribe while others broadcast") {
  constexpr unsigned kWorkers = 4;
  Workers workers(kWorkers);
  Hub& hub = workers.hub();
  std::atomic<bool> stop{false};
  std::atomic<long> sent{0};
  std::vector<std::thread> senders;
  senders.reserve(2);
  for (int b = 0; b < 2; ++b) {
    senders.emplace_back([&] {
      while (!stop) {
        for (int s = 0; s < 4; ++s) sent += static_cast<long>(hub.broadcast_encoded("churn" + std::to_string(s), "1"));
      }
    });
  }
  std::atomic<long> received{0};
  std::vector<std::thread> churners;
  churners.reserve(kWorkers);
  for (unsigned w = 0; w < kWorkers; ++w) {
    churners.emplace_back([&, w] {
      for (int round = 0; round < 300; ++round) {
        workers.run(w, [&] {
          struct Count final : Sink {
            explicit Count(std::atomic<long>& r) : total(r) {}
            void deliver(const FramePtr&) override { ++total; }
            std::atomic<long>& total;
          };
          Count a(received), b(received);
          std::string s1 = "churn" + std::to_string(round % 4), s2 = "churn" + std::to_string((round + 1) % 4);
          GroupId g1 = hub.subscribe(w, s1, nullptr, &a);
          GroupId g2 = hub.subscribe(w, s2, nullptr, &b);
          hub.drain(w);
          hub.unsubscribe(w, s1, g1, &a);
          hub.unsubscribe(w, s2, g2, &b);
          hub.drain(w);  // frames queued for a group that is gone are dropped
        });
      }
    });
  }
  for (auto& t : churners) t.join();
  stop = true;
  for (auto& t : senders) t.join();
  workers.stop();
  CHECK(hub.stream_count() == 0);
  CHECK(received.load() <= sent.load());
}

TEST_CASE("beat and restart reach only the peers of one worker") {
  Hub hub(2, nullptr);
  struct P final : Peer {
    void on_beat(const FramePtr& f) override { beats.push_back(std::string(f->text())); }
    void on_restart() override { ++restarts; }
    std::vector<std::string> beats;
    int restarts = 0;
  };
  P a, b;
  hub.attach(0, &a);
  hub.attach(1, &b);
  hub.beat(0, 42);
  CHECK(a.beats == std::vector<std::string>{R"({"type":"ping","message":42})"});
  CHECK(b.beats.empty());
  hub.restart(1);
  CHECK(a.restarts == 0);
  CHECK(b.restarts == 1);
  hub.detach(0, &a);
  hub.beat(0, 43);
  CHECK(a.beats.size() == 1);
}
