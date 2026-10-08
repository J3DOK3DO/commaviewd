#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace commaview::net {

bool client_socket_alive(int fd);
int create_server(int port);

// Lets the kernel notice a client that went away without closing its connection (out of Wi-Fi
// range, powered off, moved to another network). Keepalive probes an idle connection and
// TCP_USER_TIMEOUT ends one whose data goes unacknowledged, each within about ten seconds; the
// socket then reads as closed (client_socket_alive) and its sends fail. Without them such a
// connection stays open for as long as nothing is sent on it, and about 15 minutes when something
// is. False when an option couldn't be set.
bool enable_dead_peer_detection(int fd);
inline constexpr int kKeepaliveIdleSec = 5;
inline constexpr int kKeepaliveIntervalSec = 2;
inline constexpr int kKeepaliveProbes = 3;
inline constexpr unsigned int kUserTimeoutMs = 10000;

// Who streams one camera port: one client at a time. A client connecting from the address that
// already holds the port replaces that connection, which the same device has given up on (an app
// that retried, restarted or lost its network) even when the bridge hasn't seen it close. A client
// from another address is turned away while the port is held, so two live viewers can't take it
// from each other in turn.
class StreamSlot {
 public:
  enum class Admission { kAdmitted, kReplaced, kBusy };
  struct Claim {
    Admission admission = Admission::kBusy;
    uint64_t session = 0;  // 0 when busy
    std::string holder;    // the address holding the port when busy
  };

  // peer is the client's address; an empty one (unknown) never replaces anything.
  Claim claim(const std::string& peer);
  // Whether session still holds the port: false once a reconnect replaced it.
  bool holds(uint64_t session) const;
  // Gives the port up if session still holds it.
  void release(uint64_t session);

 private:
  mutable std::mutex mutex_;
  uint64_t last_session_ = 0;
  uint64_t session_ = 0;
  std::string peer_;
};

}  // namespace commaview::net
