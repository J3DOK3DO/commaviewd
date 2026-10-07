// Deterministic backlog/fault regressions using the real producer and writer.
// No AF_UNIX socket is required; socket integration is tested separately.
#include "ui_export_socket.h"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace commaview::ui_export {
struct RecordingMetadataTestAccess {
  static void prepare(SocketServer& server, const std::string& route) {
    server.recipe_file_ = tmpfile();
    assert(server.recipe_file_ != nullptr);
    server.observe_route(route);
  }
  static void recipe(SocketServer& server, const LatestFrame& frame) { server.offer_recipe_event(frame); }
  static void snapshot(SocketServer& server, const LatestFrame& frame) { server.offer_snapshot(frame); }
  static void fault(SocketServer& server, const char* reason) {
    std::lock_guard<std::mutex> lock(server.recipe_mutex_);
    server.offer_recording_fault(reason, 123, 0, 0, 0, 0);
    assert(server.recording_faults_.size() <= 8);
  }
  static void cap(SocketServer& server) {
    server.snapshot_file_ = tmpfile();
    assert(server.snapshot_file_ != nullptr);
    server.snapshot_route_ = server.active_route_;
    server.snapshot_bytes_ = 512ULL * 1024 * 1024;
  }
  static void drain(SocketServer& server) { server.recipe_stopping_ = true; server.recipe_writer_loop(); }
  static std::string recipe_text(SocketServer& server) {
    std::fflush(server.recipe_file_); std::rewind(server.recipe_file_);
    std::string text; char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), server.recipe_file_)) text += buffer;
    return text;
  }
  static void close(SocketServer& server) {
    if (server.snapshot_file_) std::fclose(server.snapshot_file_);
    std::fclose(server.recipe_file_);
    server.snapshot_file_ = server.recipe_file_ = nullptr;
  }
};
}

using namespace commaview::ui_export;
using Access = RecordingMetadataTestAccess;
LatestFrame frame(uint8_t service, const std::string& payload, uint64_t time = 1000) {
  LatestFrame out; out.available = true; out.service_index = service; out.updated_at_ms = time;
  out.payload.assign(payload.begin(), payload.end()); return out;
}

int main() {
  char temp[] = "/tmp/commaview-metadata-XXXXXX";
  const char* directory = mkdtemp(temp); assert(directory);
  const std::string root(directory), route = "review--routeA", next = "review--routeB", current = root + "/CurrentRoute";
  assert(setenv("COMMAVIEWD_CURRENT_ROUTE_FILE", current.c_str(), 1) == 0);
  std::ofstream(current) << route; std::ofstream(root + "/IsOffroad") << "0";
  std::filesystem::create_directory(root + "/recipes");
  const int saved = dup(STDERR_FILENO); assert(saved >= 0);
  FILE* faults = std::fopen((root + "/faults.log").c_str(), "w"); assert(faults);
  std::fflush(stderr); assert(dup2(fileno(faults), STDERR_FILENO) >= 0);
  {
    SocketServer server(root + "/unused.sock", root + "/recipes"); Access::prepare(server, "");
    std::filesystem::remove(current);
    Access::snapshot(server, frame(0, "{}")); Access::drain(server);
    assert(server.stats().snapshot_missing_route == 0 && !server.stats().metadata_damaged);
    Access::recipe(server, frame(kRecipeEventServiceIndex, R"({"kind":"anchor","sequence":1,"camera":"road","logMonoTime":123,"roadTimestampEof":456})", 2000));
    std::ofstream(current) << route; Access::drain(server);
    assert(server.stats().recipe_events == 0 && !server.stats().metadata_damaged);
    assert(!std::filesystem::exists(root + "/recipes/ui-invalid-" + route));
    Access::recipe(server, frame(kRecipeEventServiceIndex, R"({"kind":"anchor","sequence":2,"camera":"road","logMonoTime":234,"roadTimestampEof":567})", 3000));
    Access::snapshot(server, frame(0, "{}", 3000)); Access::drain(server);
    assert(server.stats().recipe_events == 1 && server.stats().snapshot_events == 1);
    Access::close(server);
  }
  {
    SocketServer server(root + "/unused.sock", root + "/recipes"); Access::prepare(server, route);
    Access::recipe(server, frame(kRecipeEventServiceIndex, R"({"kind":"anchor","sequence":1,"camera":"road","logMonoTime":123,"roadTimestampEof":456,"sourceFrameId":120})"));
    Access::snapshot(server, frame(18, R"({"camera":"road","logMonoTime":123,"frameId":120})"));
    std::ofstream(current) << next;
    Access::drain(server);
    auto stats = server.stats();
    assert(stats.metadata_route == next && stats.route_generation == 2);
    assert(stats.recipe_events == 0 && stats.snapshot_events == 0);
    assert(stats.route_transition_discarded == 2);
    const auto damaged = Access::recipe_text(server);
    assert(damaged.find("\"kind\":\"recording_fault\"") != std::string::npos);
    assert(damaged.find("\"routeId\":\"" + route + "\"") != std::string::npos);
    Access::recipe(server, frame(kRecipeEventServiceIndex, R"({"kind":"anchor","sequence":2,"camera":"road","logMonoTime":234,"roadTimestampEof":567,"sourceFrameId":7})", 2000));
    Access::snapshot(server, frame(18, R"({"camera":"road","logMonoTime":234,"frameId":7})", 2000));
    Access::drain(server); stats = server.stats();
    assert(stats.recipe_events == 1 && stats.snapshot_events == 1);
    assert(stats.recipe_route == next && stats.snapshot_route == next);
    const auto text = Access::recipe_text(server);
    assert(text.find("\"sourceFrameId\":120") == std::string::npos);
    assert(text.find("\"sourceFrameId\":7") != std::string::npos);
    assert(text.find("\"kind\":\"recording_fault\"") == text.rfind("\"kind\":\"recording_fault\"")); // One durable marker, not one per frame.
    Access::close(server);
  }
  std::ofstream(current) << route;
  {
    SocketServer server(root + "/unused.sock", root + "/recipes"); Access::prepare(server, route);
    assert(server.stats().metadata_damaged);
    assert(server.stats().recipe_dropped == 0); // No fabricated counters on restart.
    Access::close(server);
  }
  {
    SocketServer server(root + "/unused.sock", root + "/recipes"); Access::prepare(server, route);
    Access::snapshot(server, frame(0, "private-invalid-payload"));
    Access::snapshot(server, frame(0, "{}", 2000));
    std::filesystem::remove(current);
    Access::drain(server);
    assert(server.stats().snapshot_dropped == 1 && server.stats().snapshot_missing_route == 1);
    std::ofstream(current) << route;
    Access::snapshot(server, frame(0, "{}", 3000)); Access::drain(server);
    assert(server.stats().route_generation == 1 && server.stats().snapshot_dropped == 1 && server.stats().snapshot_missing_route == 1);
    Access::close(server);
  }
  {
    SocketServer server(root + "/unused.sock", root + "/recipes"); Access::prepare(server, route); Access::cap(server);
    Access::snapshot(server, frame(0, "private-invalid-payload", 1000));
    Access::snapshot(server, frame(0, "private-invalid-payload", 2000));
    Access::snapshot(server, frame(0, "{}", 3000));
    Access::drain(server); assert(server.stats().snapshot_dropped == 3); Access::close(server);
  }
  {
    SocketServer server(root + "/unused.sock", root + "/recipes"); Access::prepare(server, route);
    for (const char* reason : {"fault1", "fault2", "fault3", "fault4", "fault5", "fault6", "fault7", "fault8", "fault9"}) Access::fault(server, reason);
    assert(server.stats().recording_faults_suppressed == 1);
    Access::drain(server); Access::close(server);
  }
  std::fflush(stderr); assert(dup2(saved, STDERR_FILENO) >= 0); close(saved); std::fclose(faults);
  std::ifstream logged(root + "/faults.log"); const std::string text((std::istreambuf_iterator<char>(logged)), {});
  assert(text.find("reason=snapshot_file_limit count=1 ") != std::string::npos);
  assert(text.find("reason=recording_fault_queue_overflow count=1 ") != std::string::npos);
  assert(text.find("private-invalid-payload") == std::string::npos);
  assert(text.find(route) == std::string::npos && text.find(next) == std::string::npos);
  std::filesystem::remove_all(root); unsetenv("COMMAVIEWD_CURRENT_ROUTE_FILE");
  std::cout << "PASS: generation-bound backlog, fresh-route readiness, distinct first fault and bounded overflow accounting\n";
}
