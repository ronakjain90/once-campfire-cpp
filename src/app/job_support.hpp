// Helpers of the jobs: raise an error, and write through the writer from a job thread. Rust:
// crates/campfire/src/integrations/jobs.rs.
#pragma once

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

#include "app/job_runner.hpp"

namespace campfire::app::job {

[[noreturn]] inline void raise(const std::string& message) {
  throw std::runtime_error(message);
}

template <class T>
T must(Result<T> result) {
  if (!result) raise(result.error().message);
  return std::move(*result);
}

inline void must(const Status& status) {
  if (!status) raise(status.error().message);
}

// Runs `fn(Tx&)` on the writer thread and waits for it, pumping the scheduler of this job thread. Gives the `Result`.
template <class F>
auto write(App& app, F fn) {
  JobThread& thread = job_thread();
  auto task = app.db->write(thread.scheduler, std::move(fn));
  task.start();
  while (!task.done()) thread.scheduler.run_one_for(std::chrono::milliseconds(50));
  return task.result();
}

// Runs a task of this job thread to its end.
template <class T>
T run(Task<T> task) {
  JobThread& thread = job_thread();
  task.start();
  while (!task.done()) thread.scheduler.run_one_for(std::chrono::milliseconds(50));
  return task.result();
}

}  // namespace campfire::app::job
