#include "road_phase.h"

#include <capnp/serialize.h>
#include <kj/array.h>

#include <cstring>
#include <ctime>

#include "cereal/gen/cpp/log.capnp.h"
#include "gps_peek.h"

namespace commaview::road {
namespace {

uint64_t monotonic_ns() {
  timespec ts {};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

// Runs fn on the event in data, or returns nothing for bytes that aren't one.
template <typename Fn>
auto with_event(const uint8_t* data, size_t size, Fn fn) -> decltype(fn(std::declval<cereal::Event::Reader>())) {
  if (data == nullptr || size < sizeof(capnp::word) || size % sizeof(capnp::word) != 0) return std::nullopt;
  kj::Array<capnp::word> words = kj::heapArray<capnp::word>(size / sizeof(capnp::word));
  std::memcpy(words.begin(), data, size);
  try {
    capnp::ReaderOptions options;
    options.traversalLimitInWords = 1 << 16;
    capnp::FlatArrayMessageReader reader(words, options);
    return fn(reader.getRoot<cereal::Event>());
  } catch (...) {
    return std::nullopt;
  }
}

bool fresh(uint64_t log_mono_ns, uint64_t now_ns, uint64_t within_ns = kRoadPhaseFreshNs) {
  if (log_mono_ns == 0) return false;
  if (log_mono_ns >= now_ns) return log_mono_ns - now_ns <= within_ns;
  return now_ns - log_mono_ns <= within_ns;
}

// The queue outlives card: a carState from before this onroad session says nothing about it.
bool car_this_session(const RoadPhaseInputs& in) {
  if (!in.car) return false;
  return !(in.started_mono_ns && *in.started_mono_ns != 0 && in.car->log_mono_ns < *in.started_mono_ns);
}

// No carState this onroad session: onroad long enough for card to have published one, not
// engaged, and the GPS showing no movement is no car (a bench supply or harness box turning the
// ignition on). Anything short of that is driving, as it always was.
RoadPhaseReading without_car_state(const RoadPhaseInputs& in) {
  const auto driving = [](const char* why) { return RoadPhaseReading{RoadPhase::kDriving, why}; };
  const bool started_long_enough = in.started_mono_ns && *in.started_mono_ns != 0 &&
                                   in.now_mono_ns >= *in.started_mono_ns &&
                                   in.now_mono_ns - *in.started_mono_ns >= kNoCarAfterStartNs;
  if (!started_long_enough) return driving("car-state-missing");
  if (in.gps && fresh(in.gps->log_mono_ns, in.now_mono_ns, kGpsFreshNs) && in.gps->speed_ms > kGpsMovingMs) {
    return driving("moving");
  }
  // Missing is not engaged; an engaged selfdriveState from an earlier session says nothing now.
  if (in.selfdrive && in.selfdrive->enabled && in.selfdrive->log_mono_ns >= *in.started_mono_ns) {
    return driving("engaged");
  }
  return {RoadPhase::kParked, "no-car"};
}

template <typename Decode>
auto peek(const std::string& path, Decode decode) -> decltype(decode(nullptr, 0)) {
  commaview::gps::QueuePeek queue(path);
  const auto message = queue.newest();
  if (!message) return std::nullopt;
  return decode(message->data(), message->size());
}

}  // namespace

const char* road_phase_name(RoadPhase phase) {
  switch (phase) {
    case RoadPhase::kOffroad: return "offroad";
    case RoadPhase::kParked: return "parked";
    case RoadPhase::kDriving: return "driving";
  }
  return "driving";
}

std::optional<CarSample> car_sample_from_event(const uint8_t* data, size_t size) {
  return with_event(data, size, [](cereal::Event::Reader event) -> std::optional<CarSample> {
    if (!event.isCarState()) return std::nullopt;
    const auto car = event.getCarState();
    CarSample sample;
    sample.log_mono_ns = event.getLogMonoTime();
    const auto gear = car.getGearShifter();
    sample.gear_known = gear != cereal::CarState::GearShifter::UNKNOWN;
    sample.in_park = gear == cereal::CarState::GearShifter::PARK;
    sample.standstill = car.getStandstill();
    sample.v_ego = car.getVEgo();
    return sample;
  });
}

std::optional<uint64_t> started_mono_from_event(const uint8_t* data, size_t size) {
  return with_event(data, size, [](cereal::Event::Reader event) -> std::optional<uint64_t> {
    if (!event.isDeviceState()) return std::nullopt;
    const uint64_t started = event.getDeviceState().getStartedMonoTime();
    if (started == 0) return std::nullopt;
    return started;
  });
}

std::optional<SelfdriveSample> selfdrive_sample_from_event(const uint8_t* data, size_t size) {
  return with_event(data, size, [](cereal::Event::Reader event) -> std::optional<SelfdriveSample> {
    if (!event.isSelfdriveState()) return std::nullopt;
    SelfdriveSample sample;
    sample.log_mono_ns = event.getLogMonoTime();
    sample.enabled = event.getSelfdriveState().getEnabled();
    return sample;
  });
}

RoadPhaseReading decide_road_phase(const RoadPhaseInputs& in) {
  if (!in.onroad) return {RoadPhase::kOffroad, "offroad"};
  const auto driving = [](const char* why) { return RoadPhaseReading{RoadPhase::kDriving, why}; };
  if (!car_this_session(in)) return without_car_state(in);
  if (!fresh(in.car->log_mono_ns, in.now_mono_ns)) return driving("car-state-stale");
  if (!in.car->gear_known) return driving("gear-unknown");
  if (!in.car->in_park) return driving("not-in-park");
  if (!in.car->standstill) return driving("moving");
  if (!in.selfdrive) return driving("selfdrive-state-missing");
  if (!fresh(in.selfdrive->log_mono_ns, in.now_mono_ns)) return driving("selfdrive-state-stale");
  if (in.selfdrive->enabled) return driving("engaged");
  // sunnypilot's always-on steering (MADS) may stay on: in Park at a standstill it can't move the
  // car, and the person has disengaged openpilot.
  return {RoadPhase::kParked, "parked"};
}

RoadPhaseReading read_road_phase(bool onroad, const std::string& msgq_dir) {
  RoadPhaseInputs in;
  in.onroad = onroad;
  if (onroad) {
    in.car = peek(msgq_dir + "/msgq_carState", car_sample_from_event);
    in.selfdrive = peek(msgq_dir + "/msgq_selfdriveState", selfdrive_sample_from_event);
    in.started_mono_ns = peek(msgq_dir + "/msgq_deviceState", started_mono_from_event);
    in.now_mono_ns = monotonic_ns();
    // The GPS only matters when there is no carState this session.
    if (!car_this_session(in)) {
      for (const char* queue : {"/msgq_gpsLocationExternal", "/msgq_gpsLocation"}) {
        const auto fix = peek(msgq_dir + queue, commaview::gps::fix_from_event);
        if (fix && (!in.gps || fix->log_mono_ns > in.gps->log_mono_ns)) in.gps = GpsSample{fix->log_mono_ns, fix->speed_ms};
      }
    }
  }
  return decide_road_phase(in);
}

}  // namespace commaview::road
