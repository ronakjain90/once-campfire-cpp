// Micro-benchmark: deliveries per second of a 10 KB broadcast to 10,000 in-memory subscribers on
// 4 worker queues. Usage: bench_cable_broadcast [broadcasts] [deflate: 0|1]
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "cable/hub.hpp"

using namespace campfire::cable;

namespace {

constexpr unsigned kWorkers = 4;
constexpr int kSubscribers = 10000;
constexpr std::size_t kPayload = 10 * 1024;

struct CountingSink final : Sink {
  explicit CountingSink(bool d) : deflate(d) {}
  void deliver(const FramePtr& f) override {
    // The transport would queue these bytes for writev. The sink touches the shared buffer.
    bytes += f->wire(deflate)->size();
    ++count;
  }
  bool deflate;
  std::uint64_t bytes = 0;
  std::uint64_t count = 0;
};

struct WorkerState {
  std::mutex m;
  std::condition_variable cv;
  bool woken = false;
  bool quit = false;
};

}  // namespace

int main(int argc, char** argv) {
  int broadcasts = argc > 1 ? std::atoi(argv[1]) : 300;
  bool deflate = argc > 2 && std::atoi(argv[2]) != 0;
  std::deque<WorkerState> states(kWorkers);
  std::atomic<std::uint64_t> delivered{0};
  Hub hub(kWorkers, [&](unsigned w) {
    std::lock_guard lock(states[w].m);
    states[w].woken = true;
    states[w].cv.notify_one();
  });

  std::vector<std::unique_ptr<CountingSink>> sinks;
  auto identifier = std::make_shared<const std::string>(R"("{\"channel\":\"RoomChannel\",\"room_id\":1}")");
  for (int i = 0; i < kSubscribers; ++i) {
    sinks.push_back(std::make_unique<CountingSink>(deflate));
    hub.subscribe(static_cast<unsigned>(i) % kWorkers, "room:1", identifier, sinks.back().get());
  }

  std::vector<std::thread> threads;
  for (unsigned w = 0; w < kWorkers; ++w) {
    threads.emplace_back([&, w] {
      while (true) {
        bool quit = false;
        {
          std::unique_lock lock(states[w].m);
          states[w].cv.wait(lock, [&] { return states[w].woken || states[w].quit; });
          states[w].woken = false;
          quit = states[w].quit;
        }
        delivered += hub.drain(w);
        if (quit) return;
      }
    });
  }

  // 10 KB of text that compresses like a rendered message partial.
  std::string payload = "\"";
  while (payload.size() < kPayload) payload += "<div class=\\\"message\\\">hello world 0123456789</div>";
  payload.resize(kPayload - 1);
  payload += "\"";

  auto total_deliveries = static_cast<std::uint64_t>(broadcasts) * kSubscribers;
  auto count_all = [&] {
    std::uint64_t n = 0;
    for (auto& s : sinks) n += s->count;
    return n;
  };
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < broadcasts; ++i) hub.broadcast_encoded("room:1", payload);
  // The sinks run on the worker threads. Wait until the queues are empty, then read the counts.
  while (true) {
    std::uint64_t frames = delivered.load();
    if (frames >= static_cast<std::uint64_t>(broadcasts) * kWorkers) break;
    std::this_thread::yield();
  }
  auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  for (auto& s : states) {
    std::lock_guard lock(s.m);
    s.quit = true;
    s.cv.notify_one();
  }
  for (auto& t : threads) t.join();
  std::uint64_t counted = count_all();
  std::printf("subscribers=%d workers=%u payload=%zu deflate=%d broadcasts=%d\n", kSubscribers, kWorkers, kPayload,
              deflate ? 1 : 0, broadcasts);
  std::printf("deliveries=%llu (expected %llu) time=%.3fs rate=%.0f deliveries/s (%.1f broadcasts/s)\n",
              static_cast<unsigned long long>(counted), static_cast<unsigned long long>(total_deliveries), elapsed,
              static_cast<double>(counted) / elapsed, broadcasts / elapsed);
  return counted == total_deliveries ? 0 : 1;
}
