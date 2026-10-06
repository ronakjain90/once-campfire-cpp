// Hashed timer wheel. Rust: crates/kit/src/front/conn.rs.
#include "net/timer_wheel.hpp"

namespace campfire::net {

void TimerWheel::arm(TimerNode& node, std::uint64_t now_ms, std::uint64_t delay_ms, std::uint32_t kind) {
  if (node.armed) cancel(node);
  std::uint64_t expire = (now_ms + delay_ms + kTickMs - 1) / kTickMs;
  if (expire <= tick_) expire = tick_ + 1;
  node.expire_tick = expire;
  node.kind = kind;
  TimerNode*& head = heads_[expire % slots_];
  node.prev = nullptr;
  node.next = head;
  if (head != nullptr) head->prev = &node;
  head = &node;
  node.armed = true;
  ++count_;
}

void TimerWheel::cancel(TimerNode& node) noexcept {
  if (!node.armed) return;
  if (node.prev != nullptr) {
    node.prev->next = node.next;
  } else {
    heads_[node.expire_tick % slots_] = node.next;
  }
  if (node.next != nullptr) node.next->prev = node.prev;
  node.prev = node.next = nullptr;
  node.armed = false;
  --count_;
}

int TimerWheel::next_wait_ms(std::uint64_t now_ms) const noexcept {
  if (count_ == 0) return -1;
  const std::uint64_t next_ms = (tick_ + 1) * kTickMs;
  return next_ms > now_ms ? static_cast<int>(next_ms - now_ms) : 0;
}

}  // namespace campfire::net
