// The hub: stream -> workers -> subscribers. Architecture section 10.
// Rust: crates/cable/src/pubsub.rs (a tokio broadcast channel per stream; this port uses one
// queue for each worker and one wake call for each queue).
//
// Threads. subscribe, unsubscribe, drain, attach, detach, beat and restart belong to the thread
// of the worker that they name. broadcast and the other readers run on any thread.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cable/frame.hpp"

namespace campfire::compat::json {
class Value;
}

namespace campfire::cable {

// A receiver of broadcast frames. It lives on one worker.
class Sink {
 public:
  virtual ~Sink() = default;
  virtual void deliver(const FramePtr& frame) = 0;
  // Runs once for each batch of deliveries that gave this sink a frame, so that it can write them
  // together.
  virtual void flush() {}
};

// A connection that gets the heartbeat and the restart signal. It lives on one worker.
class Peer {
 public:
  virtual ~Peer() = default;
  virtual void on_beat(const FramePtr& ping) = 0;
  virtual void on_restart() = 0;
};

using GroupId = std::uint64_t;
using EncodedIdentifier = std::shared_ptr<const std::string>;

class Hub {
 public:
  // `wake(worker)` tells the worker to call drain(worker). T5 gives an eventfd write. The hub calls
  // it when a queue goes from empty to not empty, from any thread.
  using Wake = std::function<void(unsigned worker)>;

  Hub(unsigned workers, Wake wake);
  Hub(const Hub&) = delete;
  Hub& operator=(const Hub&) = delete;

  [[nodiscard]] unsigned workers() const { return static_cast<unsigned>(locals_.size()); }

  // A sink gets each broadcast of `stream`, wrapped as {"identifier":...,"message":...} for
  // `identifier` (a JSON string, already encoded). With no identifier the sink gets the raw
  // payload. The hub encodes one frame for all sinks of a (stream, identifier) group.
  GroupId subscribe(unsigned worker, std::string_view stream, EncodedIdentifier identifier, Sink* sink);
  void unsubscribe(unsigned worker, std::string_view stream, GroupId group, Sink* sink);

  // Sends the payload (encoded JSON) to every subscriber. Returns the number of subscribers.
  std::size_t broadcast_encoded(std::string_view stream, std::string_view payload_json);
  // ActiveSupport::JSON.encode of the value, then broadcast.
  std::size_t broadcast(std::string_view stream, const compat::json::Value& message);

  // Delivers the queued frames of this worker to its sinks. Returns how many frames it handled.
  std::size_t drain(unsigned worker);

  void attach(unsigned worker, Peer* peer);
  void detach(unsigned worker, Peer* peer);
  // Sends one ping frame to each peer of the worker (call it every kBeatIntervalSeconds).
  void beat(unsigned worker, std::int64_t unix_seconds);
  // Calls on_restart on each peer of the worker.
  void restart(unsigned worker);

  // The number of streams that have a subscriber.
  [[nodiscard]] std::size_t stream_count() const;

 private:
  struct Group {
    GroupId id = 0;
    EncodedIdentifier identifier;
    std::vector<std::uint32_t> counts;  // subscribers on each worker
  };
  struct Delivery {
    GroupId group = 0;
    FramePtr frame;
  };
  struct Local {
    // Only the worker thread touches these members.
    std::unordered_map<GroupId, std::vector<Sink*>> groups;
    std::unordered_set<Peer*> peers;
    std::vector<Delivery> scratch;
    GroupId delivering = 0;
    bool draining = false;
    std::vector<Sink*> touched;  // sinks that got a frame in the batch that runs
    bool dirty = false;
    // Any thread pushes to the queue.
    std::mutex queue_mutex;
    std::vector<Delivery> queue;
  };
  struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
  };

  void push(unsigned worker, Delivery delivery);

  Wake wake_;
  std::vector<std::unique_ptr<Local>> locals_;
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, std::vector<Group>, StringHash, std::equal_to<>> streams_;
  GroupId next_id_ = 0;
};

}  // namespace campfire::cable
