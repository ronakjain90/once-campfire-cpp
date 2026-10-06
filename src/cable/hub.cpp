// The hub. Rust: crates/cable/src/pubsub.rs. See hub.hpp.
#include "cable/hub.hpp"

#include <algorithm>

#include "cable/protocol.hpp"
#include "compat/json.hpp"

namespace campfire::cable {

Hub::Hub(unsigned workers, Wake wake) : wake_(std::move(wake)) {
  for (unsigned i = 0; i < workers; ++i) locals_.push_back(std::make_unique<Local>());
}

GroupId Hub::subscribe(unsigned worker, std::string_view stream, EncodedIdentifier identifier, Sink* sink) {
  GroupId id = 0;
  {
    std::unique_lock lock(mutex_);
    auto it = streams_.find(stream);
    if (it == streams_.end()) it = streams_.emplace(std::string(stream), std::vector<Group>{}).first;
    Group* found = nullptr;
    for (auto& g : it->second) {
      bool same = (!g.identifier && !identifier) || (g.identifier && identifier && *g.identifier == *identifier);
      if (same) {
        found = &g;
        break;
      }
    }
    if (found == nullptr) {
      it->second.push_back(Group{++next_id_, std::move(identifier), std::vector<std::uint32_t>(locals_.size(), 0)});
      found = &it->second.back();
    }
    ++found->counts[worker];
    id = found->id;
  }
  locals_[worker]->groups[id].push_back(sink);
  return id;
}

void Hub::unsubscribe(unsigned worker, std::string_view stream, GroupId group, Sink* sink) {
  Local& local = *locals_[worker];
  if (local.draining) std::erase(local.touched, sink);
  if (auto it = local.groups.find(group); it != local.groups.end()) {
    auto& sinks = it->second;
    auto pos = std::find(sinks.rbegin(), sinks.rend(), sink);
    if (pos != sinks.rend()) {
      if (local.delivering == group) {
        *pos = nullptr;  // keep the indexes stable while deliver() runs
        local.dirty = true;
      } else {
        *pos = sinks.back();
        sinks.pop_back();
      }
    }
    if (sinks.empty() && local.delivering != group) local.groups.erase(it);
  }
  std::unique_lock lock(mutex_);
  auto it = streams_.find(stream);
  if (it == streams_.end()) return;
  auto& groups = it->second;
  for (auto g = groups.begin(); g != groups.end(); ++g) {
    if (g->id != group) continue;
    if (g->counts[worker] > 0) --g->counts[worker];
    if (std::all_of(g->counts.begin(), g->counts.end(), [](std::uint32_t c) { return c == 0; })) groups.erase(g);
    break;
  }
  if (groups.empty()) streams_.erase(it);
}

void Hub::push(unsigned worker, Delivery delivery) {
  Local& local = *locals_[worker];
  bool was_empty = false;
  {
    std::lock_guard lock(local.queue_mutex);
    was_empty = local.queue.empty();
    local.queue.push_back(std::move(delivery));
  }
  if (was_empty && wake_) wake_(worker);
}

std::size_t Hub::broadcast_encoded(std::string_view stream, std::string_view payload_json) {
  struct Target {
    GroupId id;
    EncodedIdentifier identifier;
    std::vector<unsigned> workers;
  };
  std::vector<Target> targets;
  std::size_t receivers = 0;
  {
    std::shared_lock lock(mutex_);
    auto it = streams_.find(stream);
    if (it == streams_.end()) return 0;
    for (const auto& g : it->second) {
      Target t{g.id, g.identifier, {}};
      for (unsigned w = 0; w < g.counts.size(); ++w) {
        if (g.counts[w] != 0) {
          t.workers.push_back(w);
          receivers += g.counts[w];
        }
      }
      targets.push_back(std::move(t));
    }
  }
  for (auto& t : targets) {
    FramePtr frame =
        t.identifier ? Frame::make(protocol::message(*t.identifier, payload_json)) : Frame::make(payload_json);
    for (unsigned w : t.workers) push(w, Delivery{t.id, frame});
  }
  return receivers;
}

std::size_t Hub::broadcast(std::string_view stream, const compat::json::Value& message) {
  return broadcast_encoded(stream, compat::json::encode(message));
}

std::size_t Hub::drain(unsigned worker) {
  Local& local = *locals_[worker];
  std::size_t handled = 0;
  while (true) {
    local.scratch.clear();
    local.draining = false;
    {
      std::lock_guard lock(local.queue_mutex);
      local.scratch.swap(local.queue);
    }
    if (local.scratch.empty()) return handled;
    // A sink can subscribe or unsubscribe while it handles a frame, so work on a private batch.
    std::vector<Delivery> batch;
    batch.swap(local.scratch);
    local.draining = true;
    for (auto& d : batch) {
      ++handled;
      auto it = local.groups.find(d.group);
      if (it == local.groups.end()) continue;
      local.delivering = d.group;
      for (std::size_t i = 0; i < it->second.size(); ++i) {
        Sink* sink = it->second[i];
        if (sink != nullptr) {
          sink->deliver(d.frame);
          local.touched.push_back(sink);
        }
        it = local.groups.find(d.group);  // the map can rehash if a sink subscribes
        if (it == local.groups.end()) break;
      }
      local.delivering = 0;
      if (local.dirty) {
        local.dirty = false;
        it = local.groups.find(d.group);
        if (it != local.groups.end()) {
          std::erase(it->second, nullptr);
          if (it->second.empty()) local.groups.erase(it);
        }
      }
    }
    std::sort(local.touched.begin(), local.touched.end());
    local.touched.erase(std::unique(local.touched.begin(), local.touched.end()), local.touched.end());
    // A flush can close a connection, which unsubscribes and edits `touched`: work on a copy.
    std::vector<Sink*> touched;
    touched.swap(local.touched);
    local.draining = false;
    for (Sink* sink : touched) sink->flush();
  }
}

void Hub::attach(unsigned worker, Peer* peer) {
  locals_[worker]->peers.insert(peer);
}
void Hub::detach(unsigned worker, Peer* peer) {
  locals_[worker]->peers.erase(peer);
}

void Hub::beat(unsigned worker, std::int64_t unix_seconds) {
  auto& peers = locals_[worker]->peers;
  if (peers.empty()) return;
  FramePtr ping = Frame::make(protocol::ping(unix_seconds));
  std::vector<Peer*> copy(peers.begin(), peers.end());
  for (Peer* p : copy) {
    if (peers.contains(p)) p->on_beat(ping);
  }
}

void Hub::restart(unsigned worker) {
  auto& peers = locals_[worker]->peers;
  std::vector<Peer*> copy(peers.begin(), peers.end());
  for (Peer* p : copy) {
    if (peers.contains(p)) p->on_restart();
  }
}

std::size_t Hub::stream_count() const {
  std::shared_lock lock(mutex_);
  return streams_.size();
}

}  // namespace campfire::cable
