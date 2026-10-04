#pragma once

// Whether the car is offroad, parked or driving, for the control API.
//
// openpilot itself only knows offroad and onroad (IsOffroad). A car that is on but in Park, at a
// standstill and not engaged is onroad to openpilot but is not being driven; the API reports it as
// "parked". Everything that can't be shown to be parked is "driving": a car that never reports a
// gear, a stale message, an engaged openpilot (or sunnypilot MADS), a missing queue.
//
// One exception: no car at all. hardwared starts openpilot onroad whenever a panda reports
// ignition (ignitionLine or ignitionCan), which a bench power supply or harness box does with no
// car plugged in; card then never publishes a carState. Onroad for at least kNoCarAfterStartNs
// (deviceState.startedMonoTime) with no carState at all this onroad session, openpilot not engaged
// and the GPS showing no movement is "parked" with reason "no-car". In a car (recognized, or
// dashcam / unrecognized) card publishes carState within seconds of starting, so the in-car rules
// above decide; a carState that stops arriving mid-session is "car-state-stale", never "no-car".
// Without a startedMonoTime nothing is long enough: "car-state-missing", driving.
//
// carState, selfdriveState, deviceState and the GPS queues are read from their msgq queues the way
// gps_peek.h reads GPS: mapped read-only, no subscriber, no reader slot, and only when a request
// asks.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace commaview::road {

enum class RoadPhase { kOffroad, kParked, kDriving };

const char* road_phase_name(RoadPhase phase);

// A message older than this (by its logMonoTime) says nothing about now.
inline constexpr uint64_t kRoadPhaseFreshNs = 1000ULL * 1000 * 1000;
// Onroad this long with no carState at all is no car (card publishes one within seconds).
inline constexpr uint64_t kNoCarAfterStartNs = 30ULL * 1000 * 1000 * 1000;
// A GPS fix this fresh, at this speed or more, is the comma moving.
inline constexpr uint64_t kGpsFreshNs = 3ULL * 1000 * 1000 * 1000;
inline constexpr float kGpsMovingMs = 1.0f;

struct CarSample {
  uint64_t log_mono_ns = 0;
  bool gear_known = false;  // gearShifter is not "unknown"
  bool in_park = false;
  bool standstill = false;
};

struct SelfdriveSample {
  uint64_t log_mono_ns = 0;
  bool enabled = false;
};

struct GpsSample {
  uint64_t log_mono_ns = 0;
  float speed_ms = 0.0f;
};

std::optional<CarSample> car_sample_from_event(const uint8_t* data, size_t size);
std::optional<SelfdriveSample> selfdrive_sample_from_event(const uint8_t* data, size_t size);
// deviceState.startedMonoTime (CLOCK_MONOTONIC ns when openpilot went onroad), or nothing when the
// event isn't a deviceState or openpilot isn't started (0).
std::optional<uint64_t> started_mono_from_event(const uint8_t* data, size_t size);

struct RoadPhaseInputs {
  bool onroad = true;
  std::optional<CarSample> car;
  std::optional<SelfdriveSample> selfdrive;
  std::optional<uint64_t> started_mono_ns;  // deviceState.startedMonoTime, when known
  std::optional<GpsSample> gps;             // the freshest GPS fix
  uint64_t now_mono_ns = 0;
};

struct RoadPhaseReading {
  RoadPhase phase = RoadPhase::kDriving;
  // Why, in a word or two. Offroad: "offroad". Parked: "parked", "no-car". Driving:
  // "car-state-missing", "car-state-stale", "gear-unknown", "not-in-park", "moving", "engaged",
  // "selfdrive-state-missing", "selfdrive-state-stale".
  std::string reason;
};

RoadPhaseReading decide_road_phase(const RoadPhaseInputs& inputs);

// Reads the queues under msgq_dir (only when onroad) and decides.
RoadPhaseReading read_road_phase(bool onroad, const std::string& msgq_dir);

}  // namespace commaview::road
