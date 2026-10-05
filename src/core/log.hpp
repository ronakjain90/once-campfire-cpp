// Small logger. Level names and the RAILS_LOG_LEVEL variable match config/environments/production.rb.
#pragma once

#include <atomic>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace campfire {

// The levels of Ruby's Logger, from the lowest to the highest.
enum class LogLevel : std::uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3, Fatal = 4, Unknown = 5 };

// Reads a level name, as Rails reads `config.log_level`: ASCII case is not significant.
// Returns nothing for a name that is not a level.
[[nodiscard]] std::optional<LogLevel> parse_log_level(std::string_view name) noexcept;
[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;

// The process logger. A call writes one line to stderr with one `write` call, so lines of
// different threads do not mix. The line has this form:
//   2026-01-01T12:00:00.123Z WARN message
// The logger always uses the real time, not the frozen clock.
class Logger {
 public:
  using Sink = std::function<void(LogLevel, std::string_view line)>;

  [[nodiscard]] static Logger& instance() noexcept;

  void set_level(LogLevel level) noexcept { level_.store(static_cast<int>(level), std::memory_order_relaxed); }
  [[nodiscard]] LogLevel level() const noexcept {
    return static_cast<LogLevel>(level_.load(std::memory_order_relaxed));
  }
  [[nodiscard]] bool enabled(LogLevel level) const noexcept {
    return static_cast<int>(level) >= level_.load(std::memory_order_relaxed);
  }

  // Sets the level from RAILS_LOG_LEVEL text. If the text is not a level, the level stays and
  // the function returns false.
  bool set_level_from(std::string_view name) noexcept;

  // Writes one line. A line has no newline in the argument; the logger adds one.
  void write(LogLevel level, std::string_view message);

  // Replaces stderr with `sink` (for tests). An empty `sink` restores stderr.
  void set_sink(Sink sink);

 private:
  Logger() = default;
  std::atomic<int> level_{static_cast<int>(LogLevel::Info)};
};

template <class... Args>
void log(LogLevel level, std::format_string<Args...> format, Args&&... args) {
  Logger& logger = Logger::instance();
  if (logger.enabled(level)) {
    logger.write(level, std::format(format, std::forward<Args>(args)...));
  }
}

template <class... Args>
void log_debug(std::format_string<Args...> format, Args&&... args) {
  log(LogLevel::Debug, format, std::forward<Args>(args)...);
}
template <class... Args>
void log_info(std::format_string<Args...> format, Args&&... args) {
  log(LogLevel::Info, format, std::forward<Args>(args)...);
}
template <class... Args>
void log_warn(std::format_string<Args...> format, Args&&... args) {
  log(LogLevel::Warn, format, std::forward<Args>(args)...);
}
template <class... Args>
void log_error(std::format_string<Args...> format, Args&&... args) {
  log(LogLevel::Error, format, std::forward<Args>(args)...);
}

}  // namespace campfire
