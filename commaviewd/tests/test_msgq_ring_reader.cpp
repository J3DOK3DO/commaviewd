// The encoder queues read without a msgq reader slot, against real msgq (commaviewd's linked copy)
// on private queues in /dev/shm: bridge restarts and client reconnects never touch loggerd's place
// in the queue, a reader the encoder laps starts over on the next keyframe, and nothing it hands
// out is ever torn.
//
// --cpu-bench IDLE_POLL_US SECONDS measures what the polling costs: three queues published at
// 20 Hz like encoderd's, one reader thread each, and the readers' own CPU time.
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "msgq/ipc.h"
#include "msgq/msgq.h"
#include "msgq_ring_reader.h"
#include "video_transport_policy.h"

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
      std::abort();                                                      \
    }                                                                    \
  } while (0)

using commaview::ipc::QueueStreamReader;
using commaview::ipc::ReadStatus;

namespace {

constexpr size_t kRingSize = 1024 * 1024;
constexpr size_t kSmallRing = 64 * 1024;

struct TestQueue {
  std::string endpoint;
  std::string path;
  explicit TestQueue(const char* name) {
    static int count = 0;
    endpoint = std::string("commaview_ring_") + name + "_" + std::to_string(getpid()) + "_" +
               std::to_string(count++);
    path = "/dev/shm/msgq_" + endpoint;
  }
  ~TestQueue() { unlink(path.c_str()); }
};

// The queue's shared header, mapped the way msgq maps it, to look at the reader slots.
class Header {
 public:
  Header(const std::string& endpoint, size_t size) { CHECK(msgq_new_queue(&q_, endpoint.c_str(), size) == 0); }
  ~Header() { msgq_close_queue(&q_); }
  uint64_t num_readers() const { return *q_.num_readers; }
  uint64_t uid(size_t slot) const { return *q_.read_uids[slot]; }
  // Everything but the write pointer, which the publisher moves.
  std::vector<char> slots() const {
    const char* h = q_.mmap_p;
    std::vector<char> out(h, h + sizeof(msgq_header_t));
    std::memset(out.data() + offsetof(msgq_header_t, write_pointer), 0, sizeof(uint64_t));
    return out;
  }

 private:
  msgq_queue_t q_{};
};

std::unique_ptr<PubSocket> make_publisher(Context* ctx, const std::string& endpoint, size_t size = kRingSize) {
  std::unique_ptr<PubSocket> pub(PubSocket::create(ctx, endpoint, true, size));
  CHECK(pub != nullptr);
  return pub;
}

// Subscribes the way loggerd does on the encoder queues: not conflated, every message.
std::unique_ptr<SubSocket> make_loggerd(Context* ctx, const std::string& endpoint, size_t size = kRingSize) {
  std::unique_ptr<SubSocket> sock(SubSocket::create(ctx, endpoint, "127.0.0.1", false, true, size));
  CHECK(sock != nullptr);
  return sock;
}

void publish(PubSocket* pub, const std::string& text) {
  CHECK(pub->send(const_cast<char*>(text.data()), text.size()) == static_cast<int>(text.size()));
}

std::vector<std::string> drain(SubSocket* sock) {
  std::vector<std::string> out;
  while (true) {
    std::unique_ptr<Message> msg(sock->receive(true));
    if (msg == nullptr) break;
    out.emplace_back(msg->getData(), msg->getSize());
  }
  return out;
}

std::string text_of(const std::vector<uint8_t>& bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// A frame whose every byte follows from its sequence number, so a torn copy can't pass.
std::string frame(uint64_t seq, size_t size, bool keyframe) {
  std::string out(size, '\0');
  CHECK(size >= 16);
  std::memcpy(&out[0], &seq, sizeof(seq));
  const uint64_t flags = keyframe ? 1 : 0;
  std::memcpy(&out[8], &flags, sizeof(flags));
  for (size_t i = 16; i < size; ++i) out[i] = static_cast<char>((seq * 131 + i * 7) & 0xFF);
  return out;
}

bool frame_intact(const std::vector<uint8_t>& bytes, uint64_t* seq, bool* keyframe) {
  if (bytes.size() < 16) return false;
  uint64_t s = 0, flags = 0;
  std::memcpy(&s, bytes.data(), sizeof(s));
  std::memcpy(&flags, bytes.data() + 8, sizeof(flags));
  for (size_t i = 16; i < bytes.size(); ++i) {
    if (bytes[i] != static_cast<uint8_t>((s * 131 + i * 7) & 0xFF)) return false;
  }
  *seq = s;
  *keyframe = flags == 1;
  return true;
}

std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void test_layout_from_file_size() {
  using commaview::ipc::ring_layout_for_file_size;
  for (size_t readers : {15u, 25u, 1u, 41u}) {
    const size_t header = 24 + 24 * readers;
    for (size_t data : {size_t{250} * 1024, size_t{2} << 20, size_t{10} << 20, size_t{4096}}) {
      const auto layout = ring_layout_for_file_size(data + header);
      CHECK(layout.has_value());
      CHECK(layout->header_bytes == header);
      CHECK(layout->data_bytes == data);
    }
  }
  CHECK(!ring_layout_for_file_size(10 << 20).has_value());       // no header at all
  CHECK(!ring_layout_for_file_size((10 << 20) + 100).has_value());  // not 24 + 24 * n
  CHECK(!ring_layout_for_file_size(384).has_value());               // a header without a ring
  // This build's msgq agrees.
  const auto own = ring_layout_for_file_size(kRingSize + sizeof(msgq_header_t));
  CHECK(own.has_value() && own->header_bytes == sizeof(msgq_header_t));
}

// What a subscriber per bridge process did: each restart took a slot msgq never gives back, and
// the one that found them all taken evicted loggerd, which lost the frames it hadn't read.
void test_subscriber_per_restart_evicts_loggerd() {
  TestQueue queue("evict");
  std::unique_ptr<Context> ctx(Context::create());
  auto pub = make_publisher(ctx.get(), queue.endpoint);
  auto loggerd = make_loggerd(ctx.get(), queue.endpoint);
  Header header(queue.endpoint, kRingSize);
  const uint64_t loggerd_uid = header.uid(0);
  for (int restart = 1; restart < NUM_READERS; ++restart) {
    delete SubSocket::create(ctx.get(), queue.endpoint, "127.0.0.1", true, true, kRingSize);
  }
  CHECK(header.uid(0) == loggerd_uid);
  publish(pub.get(), "frame-a");
  delete SubSocket::create(ctx.get(), queue.endpoint, "127.0.0.1", true, true, kRingSize);
  CHECK(header.uid(0) != loggerd_uid);
  CHECK(drain(loggerd.get()).empty());
}

// Reads one message published after it started, the way a client session does.
std::string read_one_fresh(QueueStreamReader& reader, PubSocket* pub, const std::string& text) {
  std::vector<uint8_t> out;
  bool gap = false;
  CHECK(reader.try_next(&out, &gap) != ReadStatus::kMessage);  // starts at the write pointer
  publish(pub, text);
  CHECK(reader.next(1000, &out, &gap));
  CHECK(!gap);
  return text_of(out);
}

void test_restarts_and_reconnects_never_touch_loggerd() {
  TestQueue queue("restarts");
  std::unique_ptr<Context> ctx(Context::create());
  auto pub = make_publisher(ctx.get(), queue.endpoint);
  auto loggerd = make_loggerd(ctx.get(), queue.endpoint);
  Header header(queue.endpoint, kRingSize);
  const uint64_t loggerd_uid = header.uid(0);
  const off_t file_size = [&] {
    struct stat st {};
    CHECK(stat(queue.path.c_str(), &st) == 0);
    return st.st_size;
  }();

  std::vector<std::string> published;
  std::vector<std::string> logged;
  // Client reconnects and bridge restarts in one process: a reader per session.
  const int sessions = 20 * NUM_READERS;
  for (int session = 0; session < sessions; ++session) {
    QueueStreamReader reader(queue.path);
    const std::string text = "s" + std::to_string(session);
    published.push_back(text);
    CHECK(read_one_fresh(reader, pub.get(), text) == text);
    for (auto& m : drain(loggerd.get())) logged.push_back(m);
    CHECK(header.num_readers() == 1);
    CHECK(header.uid(0) == loggerd_uid);
  }

  // Whole bridge processes starting, reading and exiting.
  const int processes = 3 * NUM_READERS;
  for (int restart = 0; restart < processes; ++restart) {
    int ready[2];
    int go[2];
    CHECK(pipe(ready) == 0 && pipe(go) == 0);
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
      QueueStreamReader reader(queue.path);
      std::vector<uint8_t> out;
      bool gap = false;
      reader.try_next(&out, &gap);
      char byte = 1;
      if (write(ready[1], &byte, 1) != 1) _exit(3);
      if (read(go[0], &byte, 1) != 1) _exit(4);
      const bool ok = reader.next(2000, &out, &gap) && text_of(out) == "p" + std::to_string(restart);
      _exit(ok ? 0 : 2);
    }
    char byte = 0;
    CHECK(read(ready[0], &byte, 1) == 1);
    const std::string text = "p" + std::to_string(restart);
    publish(pub.get(), text);
    published.push_back(text);
    CHECK(write(go[1], &byte, 1) == 1);
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    for (int fd : {ready[0], ready[1], go[0], go[1]}) close(fd);
    for (auto& m : drain(loggerd.get())) logged.push_back(m);
  }

  CHECK(header.num_readers() == 1);  // loggerd's slot only, after hundreds of sessions
  CHECK(header.uid(0) == loggerd_uid);
  CHECK(logged == published);  // never evicted, so loggerd missed nothing
  struct stat st {};
  CHECK(stat(queue.path.c_str(), &st) == 0 && st.st_size == file_size);  // never resized
}

// Nothing in the shared header changes while the reader follows the queue.
void test_reader_never_writes_the_header() {
  TestQueue queue("readonly");
  std::unique_ptr<Context> ctx(Context::create());
  auto pub = make_publisher(ctx.get(), queue.endpoint);
  auto loggerd = make_loggerd(ctx.get(), queue.endpoint);
  publish(pub.get(), "before");
  drain(loggerd.get());
  Header header(queue.endpoint, kRingSize);
  const auto before = header.slots();
  const std::string file_before = read_file(queue.path);
  {
    QueueStreamReader reader(queue.path);
    std::vector<uint8_t> out;
    bool gap = false;
    for (int i = 0; i < 50; ++i) reader.try_next(&out, &gap);
    reader.next(30, &out, &gap);
  }
  CHECK(header.slots() == before);
  CHECK(read_file(queue.path) == file_before);
}

void test_follows_every_message_across_laps() {
  TestQueue queue("laps");
  std::unique_ptr<Context> ctx(Context::create());
  auto pub = make_publisher(ctx.get(), queue.endpoint, kSmallRing);
  QueueStreamReader reader(queue.path);
  std::vector<uint8_t> out;
  bool gap = false;
  publish(pub.get(), frame(999, 64, true));  // published before the reader started: not seen
  CHECK(reader.try_next(&out, &gap) == ReadStatus::kEmpty);
  uint64_t expected = 0;
  // Sizes that don't divide the ring, so the wrap tag lands in different places each lap.
  for (uint64_t seq = 0; seq < 2000; ++seq) {
    publish(pub.get(), frame(seq, 100 + (seq * 37) % 3000, seq % 20 == 0));
    if (seq % 3 != 2 && seq != 1999) continue;  // read in bursts, a few messages behind
    while (reader.try_next(&out, &gap) == ReadStatus::kMessage) {
      uint64_t got = 0;
      bool key = false;
      CHECK(frame_intact(out, &got, &key));
      CHECK(!gap);
      CHECK(got == expected);
      expected += 1;
    }
  }
  CHECK(expected == 2000);
  CHECK(reader.stats().lapped == 0 && reader.stats().invalid == 0);
}

// The encoder laps a reader that fell behind. It starts over at the newest message, says so, and
// the bridge's keyframe gate then holds the stream until the next keyframe.
void test_lapped_reader_recovers_on_a_keyframe() {
  TestQueue queue("lapped");
  std::unique_ptr<Context> ctx(Context::create());
  auto pub = make_publisher(ctx.get(), queue.endpoint, kSmallRing);
  auto loggerd = make_loggerd(ctx.get(), queue.endpoint, kSmallRing);
  QueueStreamReader reader(queue.path);
  std::vector<uint8_t> out;
  bool gap = false;
  CHECK(reader.try_next(&out, &gap) == ReadStatus::kEmpty);

  uint64_t seq = 0;
  publish(pub.get(), frame(seq++, 4000, true));
  CHECK(reader.next(100, &out, &gap) && !gap);
  drain(loggerd.get());
  // The reader stalls (a slow client) while the encoder writes several laps.
  for (int i = 0; i < 100; ++i, ++seq) publish(pub.get(), frame(seq, 4000, seq % 15 == 0));
  for (int i = 0; i < 3; ++i) {
    CHECK(reader.try_next(&out, &gap) != ReadStatus::kMessage);  // overtaken: starts over
  }
  CHECK(reader.stats().lapped >= 1);

  commaview::video::KeyframeStartGate gate(60);
  CHECK(gate.admit(true));
  uint64_t first_admitted = 0;
  bool saw_gap = false;
  for (int i = 0; i < 40 && first_admitted == 0; ++i) {
    const bool key = seq % 15 == 0;
    publish(pub.get(), frame(seq++, 4000, key));
    CHECK(reader.next(100, &out, &gap));
    uint64_t got = 0;
    bool got_key = false;
    CHECK(frame_intact(out, &got, &got_key));
    CHECK(got == seq - 1);  // the newest, never an overwritten one
    if (gap) {
      CHECK(!saw_gap);  // reported once, on the first message after the jump
      saw_gap = true;
      gate = commaview::video::KeyframeStartGate(60);  // what handle_video_client does
    }
    if (gate.admit(got_key)) first_admitted = got;
  }
  CHECK(saw_gap);
  CHECK(first_admitted != 0 && first_admitted % 15 == 0);  // resumed on a keyframe
  // loggerd, subscribed the real way, was never disturbed by any of it.
  Header header(queue.endpoint, kSmallRing);
  CHECK(header.num_readers() == 1);
}

// A publisher hammering a small ring while a slow reader copies from it: whatever is handed out is
// whole, in order, and every gap is reported.
void test_concurrent_writer_never_yields_torn_messages() {
  TestQueue queue("torn");
  std::unique_ptr<Context> ctx(Context::create());
  auto pub = make_publisher(ctx.get(), queue.endpoint, kSmallRing);
  QueueStreamReader reader(queue.path, 200);
  std::vector<uint8_t> out;
  bool gap = false;
  reader.try_next(&out, &gap);

  std::atomic<bool> done{false};
  std::thread writer([&] {
    // Paced so the reader mostly keeps up, and laps it whenever it stalls.
    for (uint64_t seq = 1; seq <= 40000; ++seq) {
      publish(pub.get(), frame(seq, 64 + (seq * 101) % 6000, seq % 10 == 0));
      if (seq % 8 == 0) std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    done = true;
  });
  uint64_t last = 0, delivered = 0, gaps = 0;
  while (true) {
    const bool writer_done = done.load();
    if (!reader.next(5, &out, &gap)) {
      if (writer_done) break;
      continue;
    }
    uint64_t got = 0;
    bool key = false;
    CHECK(frame_intact(out, &got, &key));
    if (gap) {
      gaps += 1;
      CHECK(got > last + 1 || last == 0);
    } else if (last != 0) {
      CHECK(got == last + 1);
    }
    last = got;
    delivered += 1;
    if (delivered % 500 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));  // a stall
  }
  writer.join();
  CHECK(delivered > 200);
  CHECK(gaps > 0);
  printf("  concurrent: delivered=%llu gaps=%llu lapped=%llu invalid=%llu\n",
         static_cast<unsigned long long>(delivered), static_cast<unsigned long long>(gaps),
         static_cast<unsigned long long>(reader.stats().lapped),
         static_cast<unsigned long long>(reader.stats().invalid));
}

// encoderd starts with every drive: a new publisher on the same queue, or a new queue file.
void test_follows_publisher_restart_and_new_queue_file() {
  TestQueue queue("restart");
  std::unique_ptr<Context> ctx(Context::create());
  auto pub = make_publisher(ctx.get(), queue.endpoint);
  QueueStreamReader reader(queue.path);
  CHECK(read_one_fresh(reader, pub.get(), "before") == "before");

  pub.reset();
  pub = make_publisher(ctx.get(), queue.endpoint);
  std::vector<uint8_t> out;
  bool gap = false;
  publish(pub.get(), "after");
  CHECK(reader.next(500, &out, &gap) && text_of(out) == "after");

  // The file itself replaced (a reboot of openpilot without one of the device).
  pub.reset();
  unlink(queue.path.c_str());
  pub = make_publisher(ctx.get(), queue.endpoint);
  std::string got;
  for (int i = 0; i < 100 && got.empty(); ++i) {
    publish(pub.get(), "new-file-" + std::to_string(i));
    if (reader.next(30, &out, &gap)) got = text_of(out);
  }
  CHECK(got.rfind("new-file-", 0) == 0);
  CHECK(reader.stats().remaps >= 1);
}

void test_missing_queue_waits_quietly() {
  QueueStreamReader reader("/dev/shm/msgq_commaview_ring_never_" + std::to_string(getpid()));
  std::vector<uint8_t> out;
  bool gap = false;
  CHECK(reader.try_next(&out, &gap) == ReadStatus::kUnavailable);
  const auto start = std::chrono::steady_clock::now();
  CHECK(!reader.next(30, &out, &gap));
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500));
}

// A queue laid out by a fork with 25 reader slots (sunnypilot/openpilot), written by hand.
void test_reads_a_25_reader_layout() {
  const std::string path = "/dev/shm/msgq_commaview_ring_25_" + std::to_string(getpid());
  const size_t header = 24 + 24 * 25;
  const size_t data = 64 * 1024;
  {
    std::vector<char> zeros(header + data, 0);
    std::ofstream f(path, std::ios::binary);
    f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
  }
  QueueStreamReader reader(path);
  std::vector<uint8_t> out;
  bool gap = false;
  CHECK(reader.try_next(&out, &gap) == ReadStatus::kEmpty);
  const std::string text = frame(7, 200, true);
  {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    const int64_t size = static_cast<int64_t>(text.size());
    f.seekp(static_cast<std::streamoff>(header));
    f.write(reinterpret_cast<const char*>(&size), sizeof(size));
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    const uint64_t pointer = (sizeof(int64_t) + text.size() + 7) & ~uint64_t{7};
    f.seekp(8);
    f.write(reinterpret_cast<const char*>(&pointer), sizeof(pointer));
  }
  CHECK(reader.next(100, &out, &gap));
  CHECK(text_of(out) == text);
  unlink(path.c_str());
}

double thread_cpu_seconds() {
  struct rusage usage {};
  getrusage(RUSAGE_THREAD, &usage);
  return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}

int cpu_bench(int idle_poll_us, int seconds) {
  std::unique_ptr<Context> ctx(Context::create());
  std::vector<std::unique_ptr<TestQueue>> queues;
  std::vector<std::unique_ptr<PubSocket>> pubs;
  for (int i = 0; i < 3; ++i) {
    queues.push_back(std::make_unique<TestQueue>("bench"));
    pubs.push_back(make_publisher(ctx.get(), queues.back()->endpoint, 10 << 20));
  }
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> received{0};
  std::vector<double> cpu(3, 0.0);
  std::vector<std::vector<int64_t>> latency_us(3);
  const auto steady_ns = [] {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  };
  std::vector<std::thread> readers;
  for (int i = 0; i < 3; ++i) {
    readers.emplace_back([&, i] {
      QueueStreamReader reader(queues[i]->path, idle_poll_us);
      std::vector<uint8_t> out;
      bool gap = false;
      const double start = thread_cpu_seconds();
      while (!stop.load()) {
        if (reader.next(20, &out, &gap)) {  // the bridge's video loop timeout
          int64_t sent_ns = 0;
          std::memcpy(&sent_ns, out.data(), sizeof(sent_ns));
          latency_us[i].push_back((steady_ns() - sent_ns) / 1000);
          received += 1;
        }
      }
      cpu[i] = thread_cpu_seconds() - start;
    });
  }
  std::string payload = frame(1, 60000, false);  // ~60 KB: road HEVC at 20 Hz
  const auto start = std::chrono::steady_clock::now();
  uint64_t sent = 0;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
    const int64_t now = steady_ns();
    std::memcpy(&payload[0], &now, sizeof(now));
    for (auto& pub : pubs) pub->send(const_cast<char*>(payload.data()), payload.size());
    sent += 3;
    std::this_thread::sleep_until(start + std::chrono::milliseconds(50) * (sent / 3));
  }
  stop = true;
  for (auto& t : readers) t.join();
  double total = 0;
  for (double c : cpu) total += c;
  std::vector<int64_t> all;
  for (auto& v : latency_us) all.insert(all.end(), v.begin(), v.end());
  std::sort(all.begin(), all.end());
  const auto pct = [&](double q) { return all.empty() ? 0LL : static_cast<long long>(all[static_cast<size_t>(q * (all.size() - 1))]); };
  printf("cpu-bench idle_poll_us=%d seconds=%d sent=%llu received=%llu reader_cpu_total=%.3fs "
         "per_reader=%.3f%% of one core latency_us p50=%lld p99=%lld max=%lld\n",
         idle_poll_us, seconds, static_cast<unsigned long long>(sent),
         static_cast<unsigned long long>(received.load()), total, 100.0 * total / 3 / seconds,
         pct(0.5), pct(0.99), pct(1.0));
  return 0;
}

}  // namespace

// Alias discovery also works when the client connects before the encoder starts.
static void test_late_encoder_alias() {
  TestQueue preferred("preferred"), alternate("legacy");
  QueueStreamReader reader(preferred.path, {alternate.path});
  std::vector<uint8_t> out;
  bool gap = false;
  CHECK(reader.try_next(&out, &gap) == ReadStatus::kUnavailable);
  std::unique_ptr<Context> ctx(Context::create());
  auto publisher = make_publisher(ctx.get(), alternate.endpoint);
  Header header(alternate.endpoint, kRingSize);
  const auto untouched = header.slots();
  CHECK(reader.try_next(&out, &gap) == ReadStatus::kEmpty);
  publish(publisher.get(), "StarPilot frame");
  CHECK(reader.next(50, &out, &gap));
  CHECK(text_of(out) == "StarPilot frame");
  CHECK(reader.path() == alternate.path);
  CHECK(header.slots() == untouched);
}

static void test_stale_preferred_queue_does_not_mask_fork_alias() {
  TestQueue preferred("old_fork"), alternate("active_fork");
  std::unique_ptr<Context> ctx(Context::create());
  auto old_publisher = make_publisher(ctx.get(), preferred.endpoint);
  auto active_publisher = make_publisher(ctx.get(), alternate.endpoint);
  const timespec old_time[2] = {{1, 0}, {1, 0}};
  CHECK(utimensat(AT_FDCWD, preferred.path.c_str(), old_time, 0) == 0);
  QueueStreamReader reader(preferred.path, {alternate.path});
  std::vector<uint8_t> out;
  bool gap = false;
  CHECK(reader.try_next(&out, &gap) == ReadStatus::kEmpty);
  publish(active_publisher.get(), "active fork");
  CHECK(reader.next(50, &out, &gap));
  CHECK(text_of(out) == "active fork");
  CHECK(reader.path() == alternate.path);
}

int main(int argc, char** argv) {
  unsetenv("OPENPILOT_PREFIX");
  if (argc == 4 && std::string(argv[1]) == "--cpu-bench") return cpu_bench(std::atoi(argv[2]), std::atoi(argv[3]));
  test_stale_preferred_queue_does_not_mask_fork_alias();
  test_late_encoder_alias();
  test_layout_from_file_size();
  test_subscriber_per_restart_evicts_loggerd();
  test_restarts_and_reconnects_never_touch_loggerd();
  test_reader_never_writes_the_header();
  test_follows_every_message_across_laps();
  test_lapped_reader_recovers_on_a_keyframe();
  test_concurrent_writer_never_yields_torn_messages();
  test_follows_publisher_restart_and_new_queue_file();
  test_missing_queue_waits_quietly();
  test_reads_a_25_reader_layout();
  printf("PASS: msgq ring reader tests (no reader slot)\n");
  return 0;
}
