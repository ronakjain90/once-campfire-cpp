// Logger. Level names match Ruby's Logger and config/environments/production.rb.
#include "core/log.hpp"

#include <unistd.h>

#include <cctype>
#include <ctime>
#include <mutex>
#include <utility>

#include "core/time_format.hpp"

namespace campfire {
namespace {

std::mutex& sink_mutex() {
  static std::mutex mutex;
  return mutex;
}

Logger::Sink& sink_slot() {
  static Logger::Sink sink;
  return sink;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::optional<LogLevel> parse_log_level(std::string_view name) noexcept {
  static constexpr std::pair<std::string_view, LogLevel> kLevels[] = {
      {"debug", LogLevel::Debug}, {"info", LogLevel::Info},   {"warn", LogLevel::Warn},
      {"error", LogLevel::Error}, {"fatal", LogLevel::Fatal}, {"unknown", LogLevel::Unknown},
  };
  for (const auto& [text, level] : kLevels) {
    if (iequals(name, text)) {
      return level;
    }
  }
  return std::nullopt;
}

std::string_view to_string(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Fatal: return "FATAL";
    case LogLevel::Unknown: return "ANY";
  }
  return "ANY";
}

Logger& Logger::instance() noexcept {
  static Logger logger;
  return logger;
}

bool Logger::set_level_from(std::string_view name) noexcept {
  const std::optional<LogLevel> parsed = parse_log_level(name);
  if (!parsed) {
    return false;
  }
  set_level(*parsed);
  return true;
}

void Logger::set_sink(Sink sink) {
  const std::lock_guard lock(sink_mutex());
  sink_slot() = std::move(sink);
}

void Logger::write(LogLevel level, std::string_view message) {
  timespec now{};
  clock_gettime(CLOCK_REALTIME, &now);
  std::string line = format_iso8601_millis(Timestamp{now.tv_sec, static_cast<std::int32_t>(now.tv_nsec)});
  line += ' ';
  line += to_string(level);
  line += ' ';
  line += message;
  {
    const std::lock_guard lock(sink_mutex());
    if (const Sink& sink = sink_slot(); sink) {
      sink(level, line);
      return;
    }
  }
  line += '\n';
  // One write call: lines from different threads do not mix. A failed write has no remedy.
  const ssize_t written = ::write(STDERR_FILENO, line.data(), line.size());
  (void)written;
}

}  // namespace campfire
