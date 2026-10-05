// Clock. Rust: crates/rails_compat/src/clock.rs, crates/kit/src/clock.rs.
#include "core/clock.hpp"

#include <cstdlib>
#include <ctime>

#include "core/time_format.hpp"

namespace campfire {
namespace {
bool blank(std::string_view s) {
  return s.find_first_not_of(" \t\n\r\f\v") == std::string_view::npos;
}
}  // namespace

Timestamp SystemClock::now() const {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  return Timestamp{ts.tv_sec, static_cast<std::int32_t>(ts.tv_nsec)};
}

std::shared_ptr<TestClock> TestClock::frozen_at(Timestamp at) {
  auto clock = std::make_shared<TestClock>();
  clock->travel_to(at);
  return clock;
}

Timestamp TestClock::now() const {
  const std::lock_guard lock(mutex_);
  if (frozen_) {
    return *frozen_;
  }
  return SystemClock{}.now().plus_seconds(offset_seconds_);
}

void TestClock::travel_to(Timestamp at) {
  const std::lock_guard lock(mutex_);
  frozen_ = at;
}

void TestClock::travel(std::int64_t seconds) {
  const std::lock_guard lock(mutex_);
  if (frozen_) {
    frozen_ = frozen_->plus_seconds(seconds);
  } else {
    offset_seconds_ += seconds;
  }
}

void TestClock::travel_back() {
  const std::lock_guard lock(mutex_);
  frozen_.reset();
  offset_seconds_ = 0;
}

Result<SharedClock> clock_from_lookup(const std::function<std::optional<std::string>(std::string_view)>& get) {
  const std::optional<std::string> value = get(kFrozenTimeEnv);
  if (!value || blank(*value)) {
    return std::make_shared<SystemClock>();
  }
  const Result<Timestamp> at = parse_rfc3339(*value);
  if (!at) {
    return fail(Errc::Config, std::string(kFrozenTimeEnv) + "=\"" + *value + "\" is not an RFC 3339 timestamp: " + at.error().message);
  }
  return TestClock::frozen_at(*at);
}

Result<SharedClock> clock_from_env() {
  return clock_from_lookup([](std::string_view name) -> std::optional<std::string> {
    const char* value = std::getenv(std::string(name).c_str());
    if (value == nullptr) {
      return std::nullopt;
    }
    return std::string(value);
  });
}

}  // namespace campfire
