#include "socket.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

namespace commaview::net {

bool client_socket_alive(int fd) {
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = POLLIN | POLLHUP | POLLERR;
#ifdef POLLRDHUP
  pfd.events |= POLLRDHUP;
#endif
  pfd.revents = 0;

  int pr = ::poll(&pfd, 1, 0);
  if (pr <= 0) return true;

  short bad = POLLHUP | POLLERR;
#ifdef POLLRDHUP
  bad |= POLLRDHUP;
#endif
  if (pfd.revents & bad) return false;

  if (pfd.revents & POLLIN) {
    char ch;
    ssize_t n = ::recv(fd, &ch, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0) return false;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
  }
  return true;
}

int create_server(int port) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    perror("socket");
    return -1;
  }

  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    perror("bind");
    close(fd);
    return -1;
  }

  if (listen(fd, 2) < 0) {
    perror("listen");
    close(fd);
    return -1;
  }
  return fd;
}

}  // namespace commaview::net
