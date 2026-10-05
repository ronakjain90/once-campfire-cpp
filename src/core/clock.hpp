// Time.current behind an interface. Rust: crates/rails_compat/src/clock.rs and crates/kit/src/clock.rs.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "core/error.hpp"
#include "core/timestamp.hpp"

namespace campfire {

inline constexpr std::string_view kFrozenTimeEnv = "CAMPFIRE_FROZEN_TIME";

// A source of the current time. Models, cookies and sessions read it through this interface, so
// a test or a parity run can control it. Any thread can call `now()`.
class Clock {
 public:
  virtual ~Clock() = default;
  [[nodiscard]] virtual Timestamp now() const = 0;
};

// The real time.
class SystemClock final : public Clock {
 public:
  [[nodiscard]] Timestamp now() const override;
};

// Real time moved by `travel`, or frozen by `travel_to`. This is ActiveSupport TimeHelpers.
class TestClock final : public Clock {
 public:
  TestClock() = default;
  [[nodiscard]] static std::shared_ptr<TestClock> frozen_at(Timestamp at);

  [[nodiscard]] Timestamp now() const override;
  // `travel_to`: freezes time at `at`.
  void travel_to(Timestamp at);
  // `travel`: moves time forward by `seconds`. A frozen clock stays frozen.
  void travel(std::int64_t seconds);
  // `travel_back`: the real time again.
  void travel_back();

 private:
  mutable std::mutex mutex_;
  std::int64_t offset_seconds_ = 0;
  std::optional<Timestamp> frozen_;
};

using SharedClock = std::shared_ptr<const Clock>;

// The process clock. If `CAMPFIRE_FROZEN_TIME` is set to a text that is not blank, the clock is
// frozen at that RFC 3339 time. A blank value means the real time. A text that is not a time
// gives an error.
[[nodiscard]] Result<SharedClock> clock_from_lookup(
    const std::function<std::optional<std::string>(std::string_view)>& get);
[[nodiscard]] Result<SharedClock> clock_from_env();

}  // namespace campfire
