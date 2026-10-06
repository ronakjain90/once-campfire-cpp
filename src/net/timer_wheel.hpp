// Hashed timer wheel. Rust: crates/kit/src/front/conn.rs (Go timeouts); one wheel for each worker.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace campfire::net {

// A timer. It lives inside its owner (a connection), so arming a timer allocates nothing.
struct TimerNode {
  TimerNode* prev = nullptr;
  TimerNode* next = nullptr;
  std::uint64_t expire_tick = 0;
  std::uint32_t kind = 0;
  bool armed = false;
  void* owner = nullptr;  // the object that the node belongs to
};

// The wheel has `slots` lists. A timer goes to the list of (expire tick mod slots). A timer that
// is more than one turn away stays in its list until its tick comes. One thread uses one wheel.
class TimerWheel {
 public:
  static constexpr std::uint64_t kTickMs = 50;

  explicit TimerWheel(std::uint64_t now_ms, std::size_t slots = 2048)
      : slots_(slots), heads_(slots, nullptr), tick_(now_ms / kTickMs) {}
  TimerWheel(const TimerWheel&) = delete;
  TimerWheel& operator=(const TimerWheel&) = delete;

  // Arms `node` to fire `delay_ms` after `now_ms` (rounded up to a tick). Re-arms if armed.
  void arm(TimerNode& node, std::uint64_t now_ms, std::uint64_t delay_ms, std::uint32_t kind);
  void cancel(TimerNode& node) noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return count_; }

  // Milliseconds from `now_ms` until the next tick, or -1 if no timer is armed.
  [[nodiscard]] int next_wait_ms(std::uint64_t now_ms) const noexcept;

  // Fires the timers that are due at `now_ms`. `fire(TimerNode&)` may arm or cancel any timer.
  template <class F>
  void advance(std::uint64_t now_ms, F&& fire) {
    const std::uint64_t target = now_ms / kTickMs;
    while (tick_ < target) {
      ++tick_;
      if (count_ == 0) {
        tick_ = target;
        break;
      }
      run_slot(tick_, fire);
    }
  }

 private:
  template <class F>
  void run_slot(std::uint64_t tick, F& fire) {
    TimerNode* node = heads_[tick % slots_];
    while (node != nullptr) {
      TimerNode* next = node->next;
      if (node->expire_tick <= tick) {
        cancel(*node);
        fire(*node);
      }
      node = next;
    }
  }

  std::size_t slots_;
  std::vector<TimerNode*> heads_;
  std::uint64_t tick_;
  std::size_t count_ = 0;
};

}  // namespace campfire::net
