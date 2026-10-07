// Per-worker state. Design: docs/architecture.md sections 3 and 6.
#include "app/worker_state.hpp"

#include <cstdlib>

#include "core/log.hpp"
#include "core/task.hpp"
#include "views/fragment_cache.hpp"

namespace campfire::app {

namespace {
db::Connection open_reader_or_die(const App& app) {
  auto reader = app.db->open_reader();
  if (!reader) {
    log_error("cannot open a read connection: {}", reader.error().message);
    std::abort();
  }
  return std::move(*reader);
}
}  // namespace

WorkerState::WorkerState(const App& app)
    : hub_(app.changes), fragments_(app.fragments), reader_(open_reader_or_die(app)), fragment_cache_(*fragments_) {
  hub_->add(&inbox_);
  // Before a coroutine on this thread waits, end the read transaction and turn read transactions off: other work on
  // the thread must not read the snapshot of the request. When the coroutine continues, set the state again.
  suspend_hook = SuspendHook{
      this,
      [](void* self) noexcept {
        db::Connection& reader = static_cast<WorkerState*>(self)->reader_;
        const bool on = reader.read_transactions();
        reader.set_read_transactions(false);
        return on;
      },
      [](void* self, bool on) noexcept { static_cast<WorkerState*>(self)->reader_.set_read_transactions(on); }};
}

WorkerState::~WorkerState() {
  if (suspend_hook.context == this) suspend_hook = SuspendHook{};
  hub_->remove(&inbox_);
}

SessionCache& WorkerState::sessions() {
  if (inbox_.pending()) {
    // The writer committed changes to sessions or users. A read transaction that started before the commit does not
    // see them: end it, so that the cache loads the new rows.
    reader_.end_read_transaction();
    const auto changes = inbox_.take();
    sessions_.invalidate(changes);
  }
  return sessions_;
}

const std::string& WorkerState::memo(std::string_view key, const std::function<std::string()>& make) {
  constexpr std::size_t kMemoLimit = 4096;
  if (const auto found = memo_.find(key); found != memo_.end()) return found->second;
  if (memo_.size() >= kMemoLimit) memo_.clear();
  return memo_.emplace(std::string(key), make()).first->second;
}

WorkerState& worker_state() {
  thread_local std::unique_ptr<WorkerState> state;
  if (!state) {
    state = std::make_unique<WorkerState>(app());
    views::set_fragment_cache(&state->fragment_cache());
  }
  return *state;
}

}  // namespace campfire::app
