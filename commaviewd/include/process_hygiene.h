#pragma once

// Descriptor hygiene for a daemon that runs next to openpilot and starts helpers.

#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace commaview::runtime {

// In a forked child, before exec: closes every descriptor from `lowest` up, so nothing the parent
// holds (its listening sockets, a client's connection, the discovery socket) lives on in a helper
// such as the deferred queue's waiter or an install. Async-signal-safe calls only (the parent is
// threaded); close_range where the kernel has it, else one close per possible descriptor.
inline void close_fds_from(int lowest) {
#ifdef SYS_close_range
  if (::syscall(SYS_close_range, static_cast<unsigned>(lowest), ~0U, 0U) == 0) return;
#endif
  long limit = 4096;
  struct rlimit lim {};
  if (::getrlimit(RLIMIT_NOFILE, &lim) == 0 && lim.rlim_cur != RLIM_INFINITY) {
    limit = static_cast<long>(lim.rlim_cur);
  }
  if (limit > 65536) limit = 65536;
  for (long fd = lowest; fd < limit; ++fd) ::close(static_cast<int>(fd));
}

// One process per role: a second one exits before it touches anything. The lock is held for the
// life of the process (close-on-exec, so no helper inherits it). False only when another process
// holds it; a lock file that can't be opened leaves things as they were without one.
inline bool hold_instance_lock(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return true;
  if (::flock(fd, LOCK_EX | LOCK_NB) == 0) return true;  // kept open: the lock goes with the process
  const bool held_elsewhere = errno == EWOULDBLOCK;
  ::close(fd);
  return !held_elsewhere;
}

}  // namespace commaview::runtime
