// Placeholder server for the production image smoke test. Not part of the app.
// Answers GET /up with 200 on HTTP_PORT (default 80). Replaced by src/app/main.cpp.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

int main() {
  const char* port_env = std::getenv("HTTP_PORT");
  int port = port_env != nullptr ? std::atoi(port_env) : 80;
  std::signal(SIGPIPE, SIG_IGN);

  int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) {
    std::perror("socket");
    return 1;
  }
  int one = 1;
  ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 ||
      ::listen(listener, 128) < 0) {
    std::perror("bind/listen");
    return 1;
  }
  std::printf("placeholder listening on %d\n", port);
  std::fflush(stdout);

  for (;;) {
    int fd = ::accept(listener, nullptr, nullptr);
    if (fd < 0) continue;
    char buf[4096];
    ssize_t n = ::read(fd, buf, sizeof buf);
    std::string_view req(buf, n > 0 ? static_cast<size_t>(n) : 0);
    bool up = req.starts_with("GET /up ") || req.starts_with("GET /up?");
    const char* ok = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    const char* nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    const char* r = up ? ok : nf;
    ssize_t ignored = ::write(fd, r, std::strlen(r));
    (void)ignored;
    ::close(fd);
  }
}
