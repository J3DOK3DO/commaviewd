#include "socket.h"

#include <arpa/inet.h>
#include <cassert>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using commaview::net::StreamSlot;

void test_free_slot_admits_and_release_frees_it() {
  StreamSlot slot;
  const auto first = slot.claim("192.168.2.40");
  assert(first.admission == StreamSlot::Admission::kAdmitted);
  assert(first.session != 0);
  assert(slot.holds(first.session));

  slot.release(first.session);
  assert(!slot.holds(first.session));
  const auto other = slot.claim("192.168.2.41");
  assert(other.admission == StreamSlot::Admission::kAdmitted);
}

void test_same_address_replaces_its_earlier_connection() {
  StreamSlot slot;
  const auto stale = slot.claim("192.168.2.40");
  const auto fresh = slot.claim("192.168.2.40");
  assert(fresh.admission == StreamSlot::Admission::kReplaced);
  assert(fresh.session != stale.session);
  assert(!slot.holds(stale.session) && "the replaced session must see it lost the camera");
  assert(slot.holds(fresh.session));

  // The replaced session ends later; its release must not free the camera from its replacement.
  slot.release(stale.session);
  assert(slot.holds(fresh.session));
  assert(slot.claim("192.168.2.41").admission == StreamSlot::Admission::kBusy);
}

void test_another_address_is_turned_away_while_held() {
  StreamSlot slot;
  const auto holder = slot.claim("192.168.2.40");
  const auto other = slot.claim("192.168.2.41");
  assert(other.admission == StreamSlot::Admission::kBusy);
  assert(other.session == 0);
  assert(other.holder == "192.168.2.40");
  assert(slot.holds(holder.session));
}

void test_unknown_address_never_replaces() {
  StreamSlot slot;
  const auto unknown = slot.claim("");
  assert(unknown.admission == StreamSlot::Admission::kAdmitted);
  assert(slot.claim("").admission == StreamSlot::Admission::kBusy);
  assert(slot.claim("192.168.2.40").admission == StreamSlot::Admission::kBusy);
  assert(slot.holds(unknown.session));
}

int get_int_option(int fd, int level, int name) {
  int value = -1;
  socklen_t len = sizeof(value);
  assert(getsockopt(fd, level, name, &value, &len) == 0);
  return value;
}

void test_dead_peer_detection_options() {
  const int server = socket(AF_INET, SOCK_STREAM, 0);
  assert(server >= 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
  assert(listen(server, 1) == 0);
  socklen_t len = sizeof(addr);
  assert(getsockname(server, reinterpret_cast<sockaddr*>(&addr), &len) == 0);

  const int client = socket(AF_INET, SOCK_STREAM, 0);
  assert(client >= 0);
  assert(connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
  const int accepted = accept(server, nullptr, nullptr);
  assert(accepted >= 0);

  assert(commaview::net::enable_dead_peer_detection(accepted));
  assert(get_int_option(accepted, SOL_SOCKET, SO_KEEPALIVE) == 1);
  assert(get_int_option(accepted, IPPROTO_TCP, TCP_KEEPIDLE) == commaview::net::kKeepaliveIdleSec);
  assert(get_int_option(accepted, IPPROTO_TCP, TCP_KEEPINTVL) == commaview::net::kKeepaliveIntervalSec);
  assert(get_int_option(accepted, IPPROTO_TCP, TCP_KEEPCNT) == commaview::net::kKeepaliveProbes);
  assert(static_cast<unsigned int>(get_int_option(accepted, IPPROTO_TCP, TCP_USER_TIMEOUT)) ==
         commaview::net::kUserTimeoutMs);
  // A dead client is noticed well before a reconnecting app gives up on its own stalled stream
  // twice over (the app's stall timeout is 10 s).
  assert(commaview::net::kKeepaliveIdleSec +
             commaview::net::kKeepaliveIntervalSec * commaview::net::kKeepaliveProbes <= 15);

  close(accepted);
  close(client);
  close(server);
}

}  // namespace

int main() {
  test_free_slot_admits_and_release_frees_it();
  test_same_address_replaces_its_earlier_connection();
  test_another_address_is_turned_away_while_held();
  test_unknown_address_never_replaces();
  test_dead_peer_detection_options();
  return 0;
}
