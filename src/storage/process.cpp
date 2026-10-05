// See process.hpp.
#include "storage/process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <string_view>
#include <cstring>
#include <mutex>

#include "storage/content_types.hpp"
#include "storage/errors.hpp"

extern char** environ;

namespace campfire::storage {

namespace {

using Clock = std::chrono::steady_clock;

struct Pipe {
  int read_fd = -1;
  int write_fd = -1;
  bool open() {
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) return false;
    read_fd = fds[0];
    write_fd = fds[1];
    return true;
  }
  void close_read() { if (read_fd >= 0) ::close(read_fd); read_fd = -1; }
  void close_write() { if (write_fd >= 0) ::close(write_fd); write_fd = -1; }
  ~Pipe() { close_read(); close_write(); }
};

}  // namespace

Result<ProcessOutput> run_within(const std::vector<std::string>& argv, std::chrono::milliseconds timeout,
                                 bool capture_stderr) {
  Pipe out, err;
  if (!out.open() || (capture_stderr && !err.open())) return io_error("pipe failed");
  // The exec status goes back through a close-on-exec pipe: no data means exec worked.
  Pipe status;
  if (!status.open()) return io_error("pipe failed");

  std::vector<char*> args;
  for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);

  pid_t pid = ::fork();
  if (pid < 0) return io_error(std::string("fork failed: ") + std::strerror(errno));
  if (pid == 0) {
    int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) ::dup2(devnull, 0);
    ::dup2(out.write_fd, 1);
    if (capture_stderr) ::dup2(err.write_fd, 2);
    ::execvp(args[0], args.data());
    int e = errno;
    ssize_t ignored = ::write(status.write_fd, &e, sizeof e);
    (void)ignored;
    ::_exit(127);
  }
  out.close_write();
  err.close_write();
  status.close_write();

  int exec_errno = 0;
  if (::read(status.read_fd, &exec_errno, sizeof exec_errno) == static_cast<ssize_t>(sizeof exec_errno)) {
    ::waitpid(pid, nullptr, 0);
    if (exec_errno == ENOENT) return fail(Errc::NotFound, "program not found: " + argv[0]);
    return io_error("exec failed: " + std::string(std::strerror(exec_errno)));
  }

  ProcessOutput result;
  auto deadline = Clock::now() + timeout;
  bool timed_out = false;
  struct Watch { int* fd; std::string* sink; };
  Watch watches[2] = {{&out.read_fd, &result.out}, {&err.read_fd, &result.err}};
  char buffer[65536];
  for (;;) {
    pollfd fds[2];
    Watch* owners[2];
    nfds_t n = 0;
    for (auto& w : watches) {
      if (*w.fd >= 0) {
        fds[n] = {*w.fd, POLLIN, 0};
        owners[n++] = &w;
      }
    }
    if (n == 0) break;
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    if (left.count() <= 0) { timed_out = true; break; }
    int ready = ::poll(fds, n, static_cast<int>(left.count()));
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) break;
    if (ready == 0) { timed_out = true; break; }
    for (nfds_t i = 0; i < n; ++i) {
      if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      ssize_t got = ::read(*owners[i]->fd, buffer, sizeof buffer);
      if (got > 0) owners[i]->sink->append(buffer, static_cast<size_t>(got));
      else if (got == 0 || errno != EINTR) { ::close(*owners[i]->fd); *owners[i]->fd = -1; }
    }
  }
  if (timed_out) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    return fail(Errc::Timeout, argv[0] + " timed out after " + std::to_string(timeout.count()) + " ms");
  }
  int wstatus = 0;
  // The pipes are closed, so the child is exiting. A child that closed stdout early can still
  // run, so apply the same deadline.
  for (;;) {
    pid_t done = ::waitpid(pid, &wstatus, WNOHANG);
    if (done == pid) break;
    if (done < 0 && errno != EINTR) break;
    if (Clock::now() >= deadline) {
      ::kill(pid, SIGKILL);
      ::waitpid(pid, nullptr, 0);
      return fail(Errc::Timeout, argv[0] + " timed out after " + std::to_string(timeout.count()) + " ms");
    }
    ::usleep(2000);
  }
  result.exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
  return result;
}

bool ffmpeg_exists() {
  static const bool exists = [] {
    auto r = run_within({std::string(content_types::kFfmpeg), "-version"}, std::chrono::seconds(10), true);
    return r && r->exit_code == 0;
  }();
  return exists;
}

Result<std::string> video_preview(const std::filesystem::path& input) {
  std::vector<std::string> argv = {std::string(content_types::kFfmpeg), "-i", input.string()};
  for (auto a : content_types::kVideoPreviewArguments) argv.emplace_back(a);
  argv.emplace_back("-");
  auto r = run_within(argv, kFfmpegTimeout, true);
  if (!r) return std::unexpected(r.error());
  if (r->exit_code != 0) {
    std::string err = r->err;
    while (!err.empty() && (err.back() == '\n' || err.back() == ' ')) err.pop_back();
    return fail(Errc::Internal, std::string(content_types::kFfmpeg) + " failed (status " +
                                    std::to_string(r->exit_code) + "): " + err);
  }
  return std::move(r->out);
}

Result<TempFile> TempFile::create(std::string_view prefix, std::string_view suffix) {
  std::string pattern = (std::filesystem::temp_directory_path() / std::string(prefix)).string() + "XXXXXX" + std::string(suffix);
  int fd = ::mkstemps(pattern.data(), static_cast<int>(suffix.size()));
  if (fd < 0) return io_error(std::string("mkstemps failed: ") + std::strerror(errno));
  ::close(fd);
  TempFile file;
  file.path_ = pattern;
  return file;
}

}  // namespace campfire::storage
