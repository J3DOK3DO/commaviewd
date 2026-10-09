// offroad / parked / driving from carState and selfdriveState read straight from their msgq rings,
// without subscribing. Anything that can't show the car is parked is driving.
//
// --write-road-queues DIR GEAR STANDSTILL ENABLED [AGE_MS] writes msgq_carState and
// msgq_selfdriveState queue files laid out by this build's own msgq and schema (GEAR is unknown,
// park or drive), for the control API's integration tests. --write-device-state DIR STARTED_AGO_MS
// writes msgq_deviceState with openpilot started that long ago, and --write-gps DIR SPEED_MS a
// fresh gpsLocationExternal fix at that speed. --append-car-state FILE V_EGO COUNT INTERVAL_MS
// publishes COUNT carStates at V_EGO m/s, INTERVAL_MS apart and ending now, onto the queue in FILE
// (made if missing) the way msgq_msg_send does, for the drive watcher's distance.
#include "road_phase.h"

#include <capnp/dynamic.h>
#include <capnp/message.h>
#include <capnp/serialize.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "cereal/gen/cpp/log.capnp.h"
#include "msgq/msgq.h"

using commaview::road::CarSample;
using commaview::road::GpsSample;
using commaview::road::RoadPhase;
using commaview::road::RoadPhaseInputs;
using commaview::road::SelfdriveSample;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    failures++;
  }
}

uint64_t now_ns() {
  timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

// A queue file laid out as msgq lays it out, written the way msgq_msg_send writes it.
void write_queue(const std::string& path, const std::vector<std::vector<uint8_t>>& messages) {
  const size_t size = 1 << 16;
  std::vector<char> file(sizeof(msgq_header_t) + size, 0);
  uint64_t offset = 0;
  for (const auto& message : messages) {
    const int64_t n = static_cast<int64_t>(message.size());
    std::memcpy(&file[sizeof(msgq_header_t) + offset], &n, sizeof(n));
    std::memcpy(&file[sizeof(msgq_header_t) + offset + sizeof(n)], message.data(), message.size());
    offset = (offset + sizeof(n) + message.size() + 7) & ~uint64_t{7};
  }
  std::memcpy(&file[offsetof(msgq_header_t, write_pointer)], &offset, sizeof(offset));
  FILE* f = std::fopen(path.c_str(), "wb");
  std::fwrite(file.data(), 1, file.size(), f);
  std::fclose(f);
}

std::vector<uint8_t> bytes_of(capnp::MallocMessageBuilder& builder) {
  auto words = capnp::messageToFlatArray(builder);
  auto bytes = words.asBytes();
  return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

cereal::CarState::GearShifter gear_named(const std::string& name) {
  if (name == "park") return cereal::CarState::GearShifter::PARK;
  if (name == "drive") return cereal::CarState::GearShifter::DRIVE;
  return cereal::CarState::GearShifter::UNKNOWN;
}

std::vector<uint8_t> car_event(cereal::CarState::GearShifter gear, bool standstill, uint64_t mono_ns,
                               float v_ego = -1.0f) {
  capnp::MallocMessageBuilder builder;
  auto event = builder.initRoot<cereal::Event>();
  event.setLogMonoTime(mono_ns);
  auto car = event.initCarState();
  car.setGearShifter(gear);
  car.setStandstill(standstill);
  car.setVEgo(v_ego >= 0.0f ? v_ego : (standstill ? 0.0f : 12.0f));
  return bytes_of(builder);
}

// Publishes carStates onto a live queue as msgq_msg_send does: each message whole, then the write
// pointer moved past it (a -1 tag where a lap ends). A reader following the queue sees only these.
int append_car_state(const std::string& path, float v_ego, int count, int interval_ms) {
  const size_t data_size = 4u << 20;  // a whole number of KiB, as openpilot's queue sizes are
  const size_t file_size = sizeof(msgq_header_t) + data_size;
  const int fd = open(path.c_str(), O_RDWR | O_CREAT, 0600);
  if (fd < 0) return 1;
  struct stat st {};
  if (fstat(fd, &st) != 0 || (static_cast<size_t>(st.st_size) != file_size && ftruncate(fd, file_size) != 0)) {
    close(fd);
    return 1;
  }
  void* mapped = mmap(nullptr, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (mapped == MAP_FAILED) return 1;
  char* header = static_cast<char*>(mapped);
  char* data = header + sizeof(msgq_header_t);
  auto* write_pointer = reinterpret_cast<uint64_t*>(header + offsetof(msgq_header_t, write_pointer));
  uint64_t pointer = __atomic_load_n(write_pointer, __ATOMIC_ACQUIRE);
  uint64_t lap = pointer >> 32, offset = pointer & 0xFFFFFFFFu;
  const uint64_t now = now_ns();
  for (int i = 0; i < count; ++i) {
    const uint64_t at = now - static_cast<uint64_t>(count - 1 - i) * static_cast<uint64_t>(interval_ms) * 1000000ULL;
    const auto message = car_event(cereal::CarState::GearShifter::DRIVE, v_ego < 0.3f, at, v_ego);
    const uint64_t total = sizeof(int64_t) + message.size();
    if (offset + total + sizeof(int64_t) > data_size) {
      const int64_t wrap = -1;
      std::memcpy(data + offset, &wrap, sizeof(wrap));
      lap += 1;
      offset = 0;
    }
    const int64_t n = static_cast<int64_t>(message.size());
    std::memcpy(data + offset, &n, sizeof(n));
    std::memcpy(data + offset + sizeof(n), message.data(), message.size());
    offset = (offset + total + 7) & ~uint64_t{7};
    __atomic_store_n(write_pointer, (lap << 32) | offset, __ATOMIC_RELEASE);
  }
  munmap(mapped, file_size);
  return 0;
}

std::vector<uint8_t> selfdrive_event(bool enabled, uint64_t mono_ns) {
  capnp::MallocMessageBuilder builder;
  auto event = builder.initRoot<cereal::Event>();
  event.setLogMonoTime(mono_ns);
  event.initSelfdriveState().setEnabled(enabled);
  return bytes_of(builder);
}

std::vector<uint8_t> device_event(uint64_t started_mono_ns, uint64_t mono_ns) {
  capnp::MallocMessageBuilder builder;
  auto event = builder.initRoot<cereal::Event>();
  event.setLogMonoTime(mono_ns);
  auto device = event.initDeviceState();
  device.setStarted(started_mono_ns != 0);
  device.setStartedMonoTime(started_mono_ns);
  return bytes_of(builder);
}

std::vector<uint8_t> gps_event(float speed_ms, uint64_t mono_ns) {
  capnp::MallocMessageBuilder builder;
  auto event = builder.initRoot<cereal::Event>();
  event.setLogMonoTime(mono_ns);
  auto gps = event.initGpsLocationExternal();
  gps.setLatitude(37.7749);
  gps.setLongitude(-122.4194);
  gps.setHasFix(true);
  gps.setSpeed(speed_ms);
  gps.setUnixTimestampMillis(1780000000000);
  return bytes_of(builder);
}

RoadPhaseInputs parked_inputs(uint64_t now) {
  RoadPhaseInputs in;
  in.onroad = true;
  in.now_mono_ns = now;
  in.car = CarSample{now - 10'000'000, true, true, true};
  in.selfdrive = SelfdriveSample{now - 10'000'000, false};
  return in;
}

void test_decisions() {
  using commaview::road::decide_road_phase;
  const uint64_t now = 100ULL * 1000 * 1000 * 1000;

  RoadPhaseInputs off;
  off.onroad = false;
  check(decide_road_phase(off).phase == RoadPhase::kOffroad, "IsOffroad says offroad");

  check(decide_road_phase(parked_inputs(now)).phase == RoadPhase::kParked, "park + standstill + not engaged is parked");

  auto in = parked_inputs(now);
  in.car.reset();
  check(decide_road_phase(in).phase == RoadPhase::kDriving, "no carState is driving");

  in = parked_inputs(now);
  in.car->gear_known = false;
  in.car->in_park = false;
  auto reading = decide_road_phase(in);
  check(reading.phase == RoadPhase::kDriving && reading.reason == "gear-unknown", "a car that reports no gear is never guessed parked");

  in = parked_inputs(now);
  in.car->in_park = false;
  check(decide_road_phase(in).phase == RoadPhase::kDriving, "drive is driving, even stopped");

  in = parked_inputs(now);
  in.car->standstill = false;
  check(decide_road_phase(in).phase == RoadPhase::kDriving, "rolling in park is driving");

  in = parked_inputs(now);
  in.selfdrive->enabled = true;
  check(decide_road_phase(in).reason == "engaged", "engaged is driving");

  in = parked_inputs(now);
  in.car->log_mono_ns = now - 1'500'000'000;
  check(decide_road_phase(in).reason == "car-state-stale", "a carState over 1 s old is driving");

  in = parked_inputs(now);
  in.selfdrive->log_mono_ns = now - 1'500'000'000;
  check(decide_road_phase(in).reason == "selfdrive-state-stale", "a selfdriveState over 1 s old is driving");

  in = parked_inputs(now);
  in.selfdrive.reset();
  check(decide_road_phase(in).phase == RoadPhase::kDriving, "no selfdriveState is driving");

  // The in-car rules don't change once openpilot's start time is known.
  in = parked_inputs(now);
  in.started_mono_ns = now - 40'000'000'000ULL;
  check(decide_road_phase(in).reason == "parked", "a car in Park, started 40 s ago, is parked");
  in.car->in_park = false;
  check(decide_road_phase(in).reason == "not-in-park", "a car in Drive, started 40 s ago, is driving");
  in = parked_inputs(now);
  in.started_mono_ns = now - 40'000'000'000ULL;
  in.car->log_mono_ns = now - 1'500'000'000;
  check(decide_road_phase(in).reason == "car-state-stale", "a carState this session that stopped is stale, never no car");
  in = parked_inputs(now);
  in.started_mono_ns = now - 40'000'000'000ULL;
  in.gps = GpsSample{now, 20.0f};
  check(decide_road_phase(in).reason == "parked", "with a carState the GPS isn't asked");

  // sunnypilot: its always-on steering (MADS) may stay on; disengaged and in Park is parked.
  in = parked_inputs(now);
  in.selfdrive->enabled = false;
  check(decide_road_phase(in).phase == RoadPhase::kParked, "disengaged in Park is parked, whatever MADS says");
}

// No car: onroad (a bench supply or harness box turning the ignition on) with no carState at all.
RoadPhaseInputs no_car_inputs(uint64_t now) {
  RoadPhaseInputs in;
  in.onroad = true;
  in.now_mono_ns = now;
  in.started_mono_ns = now - 40'000'000'000ULL;
  return in;
}

void test_no_car() {
  using commaview::road::decide_road_phase;
  const uint64_t now = 100ULL * 1000 * 1000 * 1000;

  auto reading = decide_road_phase(no_car_inputs(now));
  check(reading.phase == RoadPhase::kParked && reading.reason == "no-car",
        "no carState, started 40 s ago, not engaged, no GPS: no car, parked");

  auto in = no_car_inputs(now);
  in.selfdrive = SelfdriveSample{now - 10'000'000, false};
  in.gps = GpsSample{now - 500'000'000, 0.3f};
  check(decide_road_phase(in).reason == "no-car", "disengaged and a still GPS fix: no car");

  in = no_car_inputs(now);
  in.started_mono_ns = now - 10'000'000'000ULL;
  reading = decide_road_phase(in);
  check(reading.phase == RoadPhase::kDriving && reading.reason == "car-state-missing",
        "started 10 s ago: card may not have published yet, driving");

  in = no_car_inputs(now);
  in.started_mono_ns.reset();
  check(decide_road_phase(in).reason == "car-state-missing", "no start time is never long enough");

  in = no_car_inputs(now);
  in.started_mono_ns = 0;
  check(decide_road_phase(in).reason == "car-state-missing", "a start time of 0 is not started");

  in = no_car_inputs(now);
  in.started_mono_ns = now + 1'000'000'000ULL;
  check(decide_road_phase(in).reason == "car-state-missing", "a start time after now is not long enough");

  in = no_car_inputs(now);
  in.selfdrive = SelfdriveSample{now - 10'000'000, true};
  check(decide_road_phase(in).reason == "engaged", "no carState but engaged is driving");

  in = no_car_inputs(now);
  in.selfdrive = SelfdriveSample{now - 60'000'000'000ULL, true};
  check(decide_road_phase(in).reason == "no-car", "an engaged selfdriveState from before this session says nothing");

  in = no_car_inputs(now);
  in.gps = GpsSample{now - 500'000'000, 8.0f};
  reading = decide_road_phase(in);
  check(reading.phase == RoadPhase::kDriving && reading.reason == "moving", "no carState but the GPS moving is driving");

  in = no_car_inputs(now);
  in.gps = GpsSample{now - 10'000'000'000ULL, 8.0f};
  check(decide_road_phase(in).reason == "no-car", "an old GPS fix says nothing about now");

  // The carState queue outlives card: one from an earlier session is no carState this session.
  in = no_car_inputs(now);
  in.car = CarSample{now - 60'000'000'000ULL, true, false, false};
  check(decide_road_phase(in).reason == "no-car", "a carState from before this session is none");
  in.started_mono_ns = now - 10'000'000'000ULL;
  check(decide_road_phase(in).reason == "car-state-missing", "...and early in the session still driving");

  // A carState this session, however old, is the car: stale is driving, never no car.
  in = no_car_inputs(now);
  in.car = CarSample{now - 20'000'000'000ULL, true, true, true};
  in.selfdrive = SelfdriveSample{now - 10'000'000, false};
  reading = decide_road_phase(in);
  check(reading.phase == RoadPhase::kDriving && reading.reason == "car-state-stale", "a stale carState this session is driving");
}

void test_events() {
  const uint64_t now = now_ns();
  const auto car = car_event(cereal::CarState::GearShifter::PARK, true, now);
  const auto sample = commaview::road::car_sample_from_event(car.data(), car.size());
  check(sample && sample->gear_known && sample->in_park && sample->standstill && sample->log_mono_ns == now,
        "carState gear, standstill and time are read");
  const auto moving = car_event(cereal::CarState::GearShifter::DRIVE, false, now, 27.5f);
  const auto moving_sample = commaview::road::car_sample_from_event(moving.data(), moving.size());
  check(moving_sample && moving_sample->v_ego == 27.5f && !moving_sample->standstill, "carState.vEgo is read");
  const auto device = device_event(now - 5, now);
  const auto started = commaview::road::started_mono_from_event(device.data(), device.size());
  check(started && *started == now - 5, "deviceState.startedMonoTime is read");
  const auto offroad_device = device_event(0, now);
  check(!commaview::road::started_mono_from_event(offroad_device.data(), offroad_device.size()),
        "a deviceState not started has no start time");
  const auto sd = selfdrive_event(true, now);
  const auto sd_sample = commaview::road::selfdrive_sample_from_event(sd.data(), sd.size());
  check(sd_sample && sd_sample->enabled, "selfdriveState.enabled is read");
  check(!commaview::road::car_sample_from_event(sd.data(), sd.size()), "a selfdriveState is no carState");
  std::vector<uint8_t> junk(64, 0xAB);
  check(!commaview::road::car_sample_from_event(junk.data(), junk.size()), "junk is no carState");
}

void test_from_queues() {
  char dir_template[] = "/tmp/commaview-road-phase-XXXXXX";
  const std::string dir = mkdtemp(dir_template);
  using commaview::road::read_road_phase;
  check(read_road_phase(false, dir).phase == RoadPhase::kOffroad, "offroad reads no queue");
  check(read_road_phase(true, dir).reason == "car-state-missing", "onroad with no queues is driving");

  const uint64_t now = now_ns();
  write_queue(dir + "/msgq_carState", {car_event(cereal::CarState::GearShifter::DRIVE, false, now - 20'000'000),
                                       car_event(cereal::CarState::GearShifter::PARK, true, now)});
  write_queue(dir + "/msgq_selfdriveState", {selfdrive_event(false, now)});
  struct stat st_before {};
  stat((dir + "/msgq_carState").c_str(), &st_before);
  const auto reading = read_road_phase(true, dir);
  check(reading.phase == RoadPhase::kParked, "the newest carState (park, standstill) and a disengaged selfdriveState are parked");

  write_queue(dir + "/msgq_selfdriveState", {selfdrive_event(true, now)});
  check(read_road_phase(true, dir).reason == "engaged", "engaged from the queue is driving");

  // No car: no carState queue at all, deviceState and GPS from their queues.
  std::remove((dir + "/msgq_carState").c_str());
  std::remove((dir + "/msgq_selfdriveState").c_str());
  write_queue(dir + "/msgq_deviceState", {device_event(now - 10'000'000'000ULL, now)});
  check(read_road_phase(true, dir).reason == "car-state-missing", "no carState, started 10 s ago, is driving");
  write_queue(dir + "/msgq_deviceState", {device_event(now - 40'000'000'000ULL, now)});
  auto no_car = read_road_phase(true, dir);
  check(no_car.phase == RoadPhase::kParked && no_car.reason == "no-car", "no carState, started 40 s ago, is no car");
  write_queue(dir + "/msgq_gpsLocationExternal", {gps_event(10.0f, now_ns())});
  check(read_road_phase(true, dir).reason == "moving", "the GPS moving from its queue is driving");
  write_queue(dir + "/msgq_gpsLocationExternal", {gps_event(0.0f, now_ns())});
  check(read_road_phase(true, dir).reason == "no-car", "a still GPS fix from its queue is no car");
  write_queue(dir + "/msgq_selfdriveState", {selfdrive_event(true, now)});
  check(read_road_phase(true, dir).reason == "engaged", "engaged with no carState is driving");
  write_queue(dir + "/msgq_selfdriveState", {selfdrive_event(false, now)});
  write_queue(dir + "/msgq_carState", {car_event(cereal::CarState::GearShifter::PARK, true, now - 60'000'000'000ULL)});
  check(read_road_phase(true, dir).reason == "no-car", "a carState left from an earlier session is no carState");
  write_queue(dir + "/msgq_carState", {car_event(cereal::CarState::GearShifter::PARK, true, now - 2'000'000'000ULL)});
  check(read_road_phase(true, dir).reason == "car-state-stale", "a carState this session that stopped is stale");

  std::string cleanup = "rm -rf '" + dir + "'";
  if (std::system(cleanup.c_str()) != 0) std::fprintf(stderr, "warning: could not remove %s\n", dir.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 6 && std::string(argv[1]) == "--write-road-queues") {
    const std::string dir = argv[2];
    const uint64_t age_ns = argc >= 7 ? std::strtoull(argv[6], nullptr, 10) * 1000000ULL : 0;
    const uint64_t at = now_ns() - age_ns;
    write_queue(dir + "/msgq_carState", {car_event(gear_named(argv[3]), std::string(argv[4]) == "1", at)});
    write_queue(dir + "/msgq_selfdriveState", {selfdrive_event(std::string(argv[5]) == "1", at)});
    return 0;
  }
  if (argc >= 4 && std::string(argv[1]) == "--write-device-state") {
    const uint64_t at = now_ns();
    write_queue(std::string(argv[2]) + "/msgq_deviceState", {device_event(at - std::strtoull(argv[3], nullptr, 10) * 1000000ULL, at)});
    return 0;
  }
  if (argc >= 6 && std::string(argv[1]) == "--append-car-state") {
    return append_car_state(argv[2], std::strtof(argv[3], nullptr), std::atoi(argv[4]), std::atoi(argv[5]));
  }
  if (argc >= 4 && std::string(argv[1]) == "--write-gps") {
    write_queue(std::string(argv[2]) + "/msgq_gpsLocationExternal", {gps_event(std::strtof(argv[3], nullptr), now_ns())});
    return 0;
  }
  test_decisions();
  test_no_car();
  test_events();
  test_from_queues();
  if (failures == 0) std::printf("PASS: road phase (offroad / parked / driving) read without subscribing\n");
  return failures == 0 ? 0 : 1;
}
