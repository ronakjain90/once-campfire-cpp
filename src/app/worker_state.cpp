// Per-worker state. Design: plans/architecture.md sections 3 and 6.
#include "app/worker_state.hpp"

#include <cstdlib>

#include "core/log.hpp"
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
    : hub_(app.changes),
      fragments_(app.fragments),
      reader_(open_reader_or_die(app)),
      fragment_cache_(*fragments_) {
  hub_->add(&inbox_);
}

WorkerState::~WorkerState() { hub_->remove(&inbox_); }

SessionCache& WorkerState::sessions() {
  if (inbox_.pending()) {
    const auto changes = inbox_.take();
    sessions_.invalidate(changes);
  }
  return sessions_;
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
