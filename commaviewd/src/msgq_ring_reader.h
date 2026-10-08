#pragma once

// openpilot's msgq queues read without subscribing.
//
// msgq gives every subscriber a reader slot in the queue's shared header and never gives it back
// (deleting a SubSocket only unmaps the ring, and a dead process's slots stay taken). A queue has
// NUM_READERS slots, and a subscriber that finds them all taken evicts every reader on the queue,
// openpilot's own included: loggerd on the encoder queues, which then loses frames and can die
// ("Process Not Running"). A subscriber that lives as long as the bridge process still took one
// slot per bridge restart.
//
// So commaviewd never subscribes. It maps the queue file read-only, keeps its read position to
// itself, and never writes to the shared memory: no reader slot, no read pointer, no valid flag,
// no uid, no signal, no ftruncate. openpilot's publisher and subscribers can't tell it is there, so
// any number of bridge restarts and client reconnects leave the queue exactly as they found it.
//
// The ring, as msgq_msg_send writes it (checked by scripts/upstream-interface-guard.sh):
//   header  msgq_header_t: num_readers, write_pointer, write_uid, then three NUM_READERS arrays.
//           NUM_READERS differs between forks (15 upstream, 25 in sunnypilot/openpilot), so the
//           header's size is worked out from the file at run time, never from this build's msgq.h.
//   data    messages back to back: an int64 size tag, the bytes, padded to 8. A -1 size tag means
//           the rest of the lap is unused and the next message is at offset 0 of the next lap.
//   write_pointer  (lap << 32) | offset of the next message. The writer stores the size tag and the
//           bytes, then a full barrier, then moves the pointer, so everything behind it is whole.
//   A message is at most a third of the ring (msgq asserts 3 * size <= ring size).
// The writer never waits for readers: a reader it laps just has its slot marked invalid. Here that
// is detected by comparing our private position with the write pointer before and after each copy.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace commaview::ipc {

struct RingLayout {
  size_t header_bytes = 0;
  size_t data_bytes = 0;
};

// The layout of a msgq queue file of this size: header_bytes is 24 + 24 * NUM_READERS. openpilot's
// queue sizes are whole KiB (QueueSize: 250 KiB, 2 MiB, 10 MiB), so the header is what is left over
// past a KiB boundary. Nothing when the size fits no msgq layout.
std::optional<RingLayout> ring_layout_for_file_size(size_t file_size);

// Where msgq keeps a service's queue: /dev/shm/msgq_<service>, or /dev/shm/msgq_<prefix>/<service>
// with OPENPILOT_PREFIX set, as msgq_new_queue builds it.
std::string msgq_queue_path(const std::string& service);

// A msgq queue file mapped read-only. Never creates, resizes or writes it.
class RingMap {
 public:
  explicit RingMap(std::string path, std::vector<std::string> alternate_paths = {});
  ~RingMap();
  RingMap(const RingMap&) = delete;
  RingMap& operator=(const RingMap&) = delete;

  // Maps the file, or maps it again when it was replaced or resized. False while it is missing or
  // fits no msgq layout. *remapped is set when the mapping changed (old positions mean nothing).
  bool refresh(bool* remapped = nullptr);
  void unmap();

  bool mapped() const { return mem_ != nullptr; }
  const std::string& path() const { return path_; }
  size_t data_size() const { return layout_.data_bytes; }
  size_t header_size() const { return layout_.header_bytes; }
  const char* data() const { return mem_ + layout_.header_bytes; }
  // Acquire loads: what the writer stored before publishing the pointer is visible after it.
  uint64_t write_pointer() const;
  int64_t size_tag_at(uint64_t offset) const;

 private:
  std::string path_;
  std::vector<std::string> alternate_paths_;
  const char* mem_ = nullptr;
  size_t map_size_ = 0;
  RingLayout layout_;
  ino_t inode_ = 0;
  dev_t device_ = 0;
};

struct StreamReaderStats {
  uint64_t messages = 0;
  uint64_t bytes = 0;
  // The writer came round the ring past our position: what was in between is gone.
  uint64_t lapped = 0;
  // A size tag or write pointer that can't be right; the position started over at the newest.
  uint64_t invalid = 0;
  // The write pointer went backwards (the queue was recreated or reset).
  uint64_t resets = 0;
  // The queue file was replaced or resized and mapped again.
  uint64_t remaps = 0;
  // More than a quarter of the ring behind the writer (seconds of video): skipped to the newest
  // rather than hand out stale frames.
  uint64_t fell_behind = 0;
};

enum class ReadStatus { kMessage, kEmpty, kUnavailable };

// Follows one queue from the newest message on, message by message, without a reader slot.
//
// A position the writer has overtaken (or anything that doesn't add up) starts over at the write
// pointer, and the next message comes back with *discontinuity set: messages were skipped, so a
// video consumer must wait for the next keyframe. With no reader slot there is no msgq wake-up
// signal either; next() polls the write pointer (one atomic load) every idle_poll_us while the
// queue is quiet, and once messages arrive at a steady rate (encoderd's 20 Hz) it sleeps until
// shortly before the next one is due and polls from there.
class QueueStreamReader {
 public:
  static constexpr int kDefaultIdlePollMicros = 4000;
  // While the queue file doesn't exist (openpilot offroad, encoderd not running).
  static constexpr int kUnavailablePollMicros = 100000;

  explicit QueueStreamReader(std::string path, int idle_poll_us = kDefaultIdlePollMicros);

  // Aliases are tried while a queue is unavailable, including a session opened offroad.
  QueueStreamReader(std::string path, std::vector<std::string> alternate_paths,
                    int idle_poll_us = kDefaultIdlePollMicros);

  // Start again at the write pointer: only messages published from now on are returned.
  void restart();

  // The next message without waiting.
  ReadStatus try_next(std::vector<uint8_t>* out, bool* discontinuity);

  // The next message, waiting up to timeout_ms. False on timeout.
  bool next(int timeout_ms, std::vector<uint8_t>* out, bool* discontinuity);

  const StreamReaderStats& stats() const { return stats_; }
  const std::string& path() const { return ring_.path(); }

 private:
  enum class Resync { kLapped, kInvalid, kReset, kBehind };
  void resync(Resync why);
  bool still_ours(uint64_t write_pointer, uint32_t lap, uint64_t offset) const;

  // Paced polling: frames closer together or further apart than this aren't a rhythm to follow.
  static constexpr int64_t kMinPacedIntervalNs = 10LL * 1000 * 1000;
  static constexpr int64_t kMaxPacedIntervalNs = 200LL * 1000 * 1000;
  // How long before the next frame is due polling resumes, for the encoder's jitter.
  static constexpr int64_t kPacedWakeEarlyNs = 8LL * 1000 * 1000;

  RingMap ring_;
  const int idle_poll_us_;
  int64_t last_message_ns_ = 0;
  int64_t interval_ns_ = 0;
  bool cursor_valid_ = false;
  uint64_t cursor_ = 0;
  bool pending_discontinuity_ = false;
  bool refresh_needed_ = true;
  int64_t last_refresh_ns_ = 0;
  StreamReaderStats stats_;
};

}  // namespace commaview::ipc
