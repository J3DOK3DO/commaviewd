#include "socket.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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

bool enable_dead_peer_detection(int fd) {
  const int on = 1;
  const int idle = kKeepaliveIdleSec;
  const int interval = kKeepaliveIntervalSec;
  const int probes = kKeepaliveProbes;
  const unsigned int user_timeout = kUserTimeoutMs;
  bool ok = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) == 0;
  ok = setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) == 0 && ok;
  ok = setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) == 0 && ok;
  ok = setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &probes, sizeof(probes)) == 0 && ok;
  ok = setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout, sizeof(user_timeout)) == 0 && ok;
  return ok;
}

StreamSlot::Claim StreamSlot::claim(const std::string& peer) {
  std::lock_guard<std::mutex> lock(mutex_);
  Claim result;
  if (session_ != 0 && (peer.empty() || peer != peer_)) {
    result.admission = Admission::kBusy;
    result.holder = peer_;
    return result;
  }
  result.admission = session_ == 0 ? Admission::kAdmitted : Admission::kReplaced;
  session_ = ++last_session_;
  peer_ = peer;
  result.session = session_;
  return result;
}

bool StreamSlot::holds(uint64_t session) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return session != 0 && session == session_;
}

void StreamSlot::release(uint64_t session) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (session == 0 || session != session_) return;
  session_ = 0;
  peer_.clear();
}

}  // namespace commaview::net
