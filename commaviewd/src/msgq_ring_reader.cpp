#include "msgq_ring_reader.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>
#include <chrono>
#include <utility>

namespace commaview::ipc {
namespace {

// msgq_header_t is three uint64 fields and three uint64[NUM_READERS] arrays.
constexpr size_t kHeaderFixedBytes = 3 * sizeof(uint64_t);
constexpr size_t kHeaderBytesPerReader = 3 * sizeof(uint64_t);
// The KiB boundary the queue sizes are whole multiples of; a header must fit below it.
constexpr size_t kQueueSizeQuantum = 1024;
constexpr size_t kMinDataBytes = 1024;
constexpr int64_t kStatIntervalNs = 1000LL * 1000 * 1000;
constexpr int kMaxStepsPerRead = 4;  // a wrap tag, then the message

uint64_t align8(uint64_t n) {
  return (n + 7) & ~uint64_t{7};
}

uint64_t pack(uint32_t lap, uint64_t offset) {
  return (uint64_t{lap} << 32) | (offset & 0xFFFFFFFFu);
}

int64_t monotonic_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

}  // namespace

std::optional<RingLayout> ring_layout_for_file_size(size_t file_size) {
  const size_t header = file_size % kQueueSizeQuantum;
  if (header < kHeaderFixedBytes + kHeaderBytesPerReader) return std::nullopt;
  if ((header - kHeaderFixedBytes) % kHeaderBytesPerReader != 0) return std::nullopt;
  if (file_size < header + kMinDataBytes) return std::nullopt;
  return RingLayout{header, file_size - header};
}

std::string msgq_queue_path(const std::string& service) {
  std::string base = "/dev/shm/msgq_";
  const char* prefix = std::getenv("OPENPILOT_PREFIX");
  if (prefix != nullptr && prefix[0] != '\0') base += std::string(prefix) + "/";
  return base + service;
}

RingMap::RingMap(std::string path, std::vector<std::string> alternate_paths)
    : path_(std::move(path)), alternate_paths_(std::move(alternate_paths)) {}

RingMap::~RingMap() {
  unmap();
}

void RingMap::unmap() {
  if (mem_ != nullptr) munmap(const_cast<char*>(mem_), map_size_);
  mem_ = nullptr;
  map_size_ = 0;
  layout_ = RingLayout{};
  inode_ = 0;
  device_ = 0;
}

bool RingMap::refresh(bool* remapped) {
  if (remapped != nullptr) *remapped = false;
  struct stat st {};
  auto usable = [](const std::string& path, struct stat* info) {
    return stat(path.c_str(), info) == 0 && S_ISREG(info->st_mode) && info->st_size > 0 &&
           ring_layout_for_file_size(static_cast<size_t>(info->st_size)).has_value();
  };
  bool found = usable(path_, &st);
  // Queue files can survive a fork change. Follow the encoder queue opened most recently (opening
  // a queue truncates it, which sets its mtime): the current one keeps ties, and a stale alias
  // gives way as soon as the other is opened again. Checked at every refresh, so a session never
  // stays on a queue nobody writes. Never create a queue.
  for (auto& alternate : alternate_paths_) {
    struct stat candidate {};
    if (usable(alternate, &candidate) && (!found ||
        candidate.st_mtim.tv_sec > st.st_mtim.tv_sec ||
        (candidate.st_mtim.tv_sec == st.st_mtim.tv_sec && candidate.st_mtim.tv_nsec > st.st_mtim.tv_nsec))) {
      std::swap(path_, alternate);
      st = candidate;
      found = true;
    }
  }
  if (!found) {
    if (mem_ != nullptr && remapped != nullptr) *remapped = true;
    unmap();
    return false;
  }
  const size_t file_size = static_cast<size_t>(st.st_size);
  if (mem_ != nullptr && st.st_ino == inode_ && st.st_dev == device_ && file_size == map_size_) return true;
  if (mem_ != nullptr && remapped != nullptr) *remapped = true;
  unmap();
  const auto layout = ring_layout_for_file_size(file_size);
  if (!layout) return false;
  // Read-only, and never created or resized here: the queue stays openpilot's.
  const int fd = open(path_.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  void* mem = mmap(nullptr, file_size, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (mem == MAP_FAILED) return false;
  mem_ = static_cast<const char*>(mem);
  map_size_ = file_size;
  layout_ = *layout;
  inode_ = st.st_ino;
  device_ = st.st_dev;
  if (remapped != nullptr) *remapped = true;
  return true;
}

uint64_t RingMap::write_pointer() const {
  // write_pointer is the header's second field in every msgq layout.
  return __atomic_load_n(reinterpret_cast<const uint64_t*>(mem_ + sizeof(uint64_t)), __ATOMIC_ACQUIRE);
}

int64_t RingMap::size_tag_at(uint64_t offset) const {
  return __atomic_load_n(reinterpret_cast<const int64_t*>(data() + offset), __ATOMIC_ACQUIRE);
}

QueueStreamReader::QueueStreamReader(std::string path, int idle_poll_us)
    : ring_(std::move(path)), idle_poll_us_(std::max(idle_poll_us, 100)) {}

QueueStreamReader::QueueStreamReader(std::string path, std::vector<std::string> alternate_paths, int idle_poll_us)
    : ring_(std::move(path), std::move(alternate_paths)), idle_poll_us_(std::max(idle_poll_us, 100)) {}

void QueueStreamReader::restart() {
  cursor_valid_ = false;
  last_message_ns_ = 0;
  pending_discontinuity_ = false;
  refresh_needed_ = true;
}

void QueueStreamReader::resync(Resync why) {
  switch (why) {
    case Resync::kLapped: stats_.lapped += 1; break;
    case Resync::kInvalid: stats_.invalid += 1; break;
    case Resync::kReset: stats_.resets += 1; break;
    case Resync::kBehind: stats_.fell_behind += 1; break;
  }
  cursor_valid_ = false;
  pending_discontinuity_ = true;
  // Something didn't add up: look at the file again in case it was replaced.
  if (why == Resync::kInvalid || why == Resync::kReset) refresh_needed_ = true;
}

// Whether the bytes at (lap, offset) can't have been written over yet, with the writer at
// write_pointer. In our lap the writer is past them. One lap on, it may be partway through a
// message it hasn't published, up to a third of the ring past its pointer, so that much room is
// kept between the pointer and us.
bool QueueStreamReader::still_ours(uint64_t write_pointer, uint32_t lap, uint64_t offset) const {
  const uint32_t write_lap = static_cast<uint32_t>(write_pointer >> 32);
  const uint64_t write_offset = write_pointer & 0xFFFFFFFFu;
  if (write_lap == lap) return true;
  if (write_lap != lap + 1) return false;
  const uint64_t in_flight = ring_.data_size() / 3 + sizeof(int64_t);
  return write_offset + in_flight <= offset;
}

ReadStatus QueueStreamReader::try_next(std::vector<uint8_t>* out, bool* discontinuity) {
  if (discontinuity != nullptr) *discontinuity = false;
  const int64_t now_ns = monotonic_ns();
  if (refresh_needed_ || !ring_.mapped() || now_ns - last_refresh_ns_ >= kStatIntervalNs) {
    bool remapped = false;
    const bool was_mapped = ring_.mapped();
    const bool ok = ring_.refresh(&remapped);
    last_refresh_ns_ = now_ns;
    refresh_needed_ = false;
    if (!ok) {
      if (cursor_valid_) pending_discontinuity_ = true;
      cursor_valid_ = false;
      return ReadStatus::kUnavailable;
    }
    if (remapped) {
      if (was_mapped) stats_.remaps += 1;
      if (cursor_valid_) pending_discontinuity_ = true;
      cursor_valid_ = false;
    }
  }

  const uint64_t data_size = ring_.data_size();
  for (int step = 0; step < kMaxStepsPerRead; ++step) {
    const uint64_t write_pointer = ring_.write_pointer();
    const uint32_t write_lap = static_cast<uint32_t>(write_pointer >> 32);
    const uint64_t write_offset = write_pointer & 0xFFFFFFFFu;
    // The writer always leaves room for a wrap tag after its pointer.
    if (write_offset + sizeof(int64_t) > data_size) {
      resync(Resync::kInvalid);
      return ReadStatus::kEmpty;
    }
    if (!cursor_valid_) {
      cursor_ = write_pointer;
      cursor_valid_ = true;
      return ReadStatus::kEmpty;
    }
    if (write_pointer == cursor_) return ReadStatus::kEmpty;

    const uint32_t lap = static_cast<uint32_t>(cursor_ >> 32);
    const uint64_t offset = cursor_ & 0xFFFFFFFFu;
    const bool same_lap = write_lap == lap;
    if (same_lap && write_offset < offset) {
      resync(Resync::kReset);
      return ReadStatus::kEmpty;
    }
    if (!same_lap && write_lap != lap + 1) {
      resync(static_cast<uint32_t>(write_lap - lap) > 0x80000000u ? Resync::kReset : Resync::kLapped);
      return ReadStatus::kEmpty;
    }
    if (!still_ours(write_pointer, lap, offset)) {
      resync(Resync::kLapped);
      return ReadStatus::kEmpty;
    }
    if (offset > data_size) {
      resync(Resync::kInvalid);
      return ReadStatus::kEmpty;
    }
    const uint64_t behind = same_lap ? write_offset - offset : (data_size - offset) + write_offset;
    if (behind > data_size / 4) {
      resync(Resync::kBehind);
      return ReadStatus::kEmpty;
    }
    if (offset + sizeof(int64_t) > data_size) {
      resync(Resync::kInvalid);
      return ReadStatus::kEmpty;
    }

    const int64_t size = ring_.size_tag_at(offset);
    if (size == -1) {
      // The rest of this lap is unused. Only a writer that has moved on to the next lap leaves one.
      if (same_lap) {
        resync(Resync::kInvalid);
        return ReadStatus::kEmpty;
      }
      cursor_ = pack(lap + 1, 0);
      continue;
    }
    if (size <= 0 || static_cast<uint64_t>(size) > data_size - offset - sizeof(int64_t) ||
        (same_lap && offset + sizeof(int64_t) + static_cast<uint64_t>(size) > write_offset)) {
      resync(Resync::kInvalid);
      return ReadStatus::kEmpty;
    }

    const char* bytes = ring_.data() + offset + sizeof(int64_t);
    out->resize(static_cast<size_t>(size));
    std::memcpy(out->data(), bytes, out->size());
    // The copy's loads complete before the pointer is looked at again (seqlock-style check).
    std::atomic_thread_fence(std::memory_order_acquire);
    if (!still_ours(ring_.write_pointer(), lap, offset)) {
      out->clear();
      resync(Resync::kLapped);
      return ReadStatus::kEmpty;
    }

    cursor_ = pack(lap, align8(offset + sizeof(int64_t) + static_cast<uint64_t>(size)));
    stats_.messages += 1;
    stats_.bytes += static_cast<uint64_t>(size);
    if (discontinuity != nullptr) *discontinuity = pending_discontinuity_;
    pending_discontinuity_ = false;
    return ReadStatus::kMessage;
  }
  return ReadStatus::kEmpty;
}

bool QueueStreamReader::next(int timeout_ms, std::vector<uint8_t>* out, bool* discontinuity) {
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
  while (true) {
    const ReadStatus status = try_next(out, discontinuity);
    const int64_t now_ns = monotonic_ns();
    if (status == ReadStatus::kMessage) {
      // encoderd publishes at a steady rate (20 Hz): learn the interval, so the wait for the next
      // frame can sleep through most of it instead of polling every idle_poll_us.
      const int64_t gap = now_ns - last_message_ns_;
      if (last_message_ns_ != 0 && gap >= kMinPacedIntervalNs && gap <= kMaxPacedIntervalNs) {
        interval_ns_ = interval_ns_ == 0 ? gap : (interval_ns_ * 7 + gap) / 8;
      } else if (last_message_ns_ != 0 && gap > kMaxPacedIntervalNs) {
        interval_ns_ = 0;  // a pause: poll until the rhythm is back
      }
      last_message_ns_ = now_ns;
      return true;
    }
    const auto now = Clock::now();
    if (now >= deadline) return false;
    int64_t wait_ns = 1000LL * (status == ReadStatus::kUnavailable ? kUnavailablePollMicros : idle_poll_us_);
    if (status == ReadStatus::kEmpty && interval_ns_ != 0 && last_message_ns_ != 0) {
      // Sleep until shortly before the next frame is due, then poll.
      const int64_t wake_ns = last_message_ns_ + interval_ns_ - kPacedWakeEarlyNs;
      if (wake_ns - now_ns > wait_ns) wait_ns = wake_ns - now_ns;
    }
    std::this_thread::sleep_for(std::min<Clock::duration>(std::chrono::nanoseconds(wait_ns), deadline - now));
  }
}

}  // namespace commaview::ipc
