// Deterministic backlog/fault regressions using the real producer and writer, and its recipe
// files read back by the archive endpoint. No AF_UNIX socket is required; socket integration is
// tested separately.
#include "source_recording_archive.h"
#include "ui_export_socket.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace commaview::ui_export {
struct RecordingMetadataTestAccess {
  static void prepare(SocketServer& server, const std::string& route) {
    // As start() does, with a stamp of its own so each server's recipe files stay apart.
    static int servers = 0;
    server.recipe_enabled_ = true;
    server.recipe_stamp_ = "1000-" + std::to_string(++servers);
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
  static void recheck_road(SocketServer& server) { server.offroad_checked_at_ = {}; }
  // Everything this server wrote to its recipe files, whichever route each is for.
  static std::string recipe_text(SocketServer& server) {
    std::string text;
    for (const auto& entry : std::filesystem::directory_iterator(server.recipe_dir_)) {
      const std::string name = entry.path().filename().string();
      if (name.rfind("ui-source-", 0) != 0 || name.find("." + server.recipe_stamp_ + ".jsonl") == std::string::npos) continue;
      std::ifstream file(entry.path());
      text.append(std::istreambuf_iterator<char>(file), {});
    }
    return text;
  }
  static void close(SocketServer& server) {
    if (server.snapshot_file_) std::fclose(server.snapshot_file_);
    if (server.recipe_file_) std::fclose(server.recipe_file_);
    server.snapshot_file_ = server.recipe_file_ = nullptr;
    server.recipe_enabled_ = false;
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
  assert(setenv("COMMAVIEWD_RECIPE_DIR", (root + "/recipes").c_str(), 1) == 0);
  // No source archive until the retention case below creates one: nothing is pruned before it.
  assert(setenv("COMMAVIEWD_SOURCE_ARCHIVE_ROOT", (root + "/realdata").c_str(), 1) == 0);
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
    // CurrentRoute can change before IsOffroad. A new epoch that has not yet
    // recorded onroad must remain available when that startup flag catches up.
    const std::string startup = "startup--drive";
    std::ofstream(current) << startup; std::ofstream(root + "/IsOffroad") << "1";
    SocketServer server(root + "/unused.sock", root + "/recipes"); Access::prepare(server, startup);
    Access::snapshot(server, frame(0, "{}")); Access::drain(server);
    assert(!server.stats().snapshot_active && server.stats().snapshot_events == 0);
    std::ofstream(root + "/IsOffroad") << "0"; Access::recheck_road(server);
    Access::snapshot(server, frame(0, "{}", 2000)); Access::drain(server);
    assert(server.stats().snapshot_active && server.stats().snapshot_events == 1);
    assert(!server.stats().metadata_damaged);
    Access::close(server);
  }
  {
    // A drive this process is recording loses the backlog still queued when the route changes:
    // that drive is damaged (once), and the next route starts clean.
    std::ofstream(current) << route;
    SocketServer server(root + "/unused.sock", root + "/recipes"); Access::prepare(server, route);
    Access::snapshot(server, frame(18, R"({"camera":"road","logMonoTime":100,"frameId":110})", 500));
    Access::drain(server);
    assert(server.stats().snapshot_events == 1 && server.stats().snapshot_route == route);
    Access::recipe(server, frame(kRecipeEventServiceIndex, R"({"kind":"anchor","sequence":1,"camera":"road","logMonoTime":123,"roadTimestampEof":456,"sourceFrameId":120})"));
    Access::snapshot(server, frame(18, R"({"camera":"road","logMonoTime":123,"frameId":120})"));
    std::ofstream(current) << next;
    Access::drain(server);
    auto stats = server.stats();
    assert(stats.metadata_route == next && stats.route_generation == 2);
    assert(stats.recipe_events == 0 && stats.snapshot_events == 0);
    assert(stats.route_transition_discarded == 2);
    assert(std::filesystem::exists(root + "/recipes/ui-invalid-" + route));
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
  {
    // The UI keeps publishing offroad. Once the snapshot is finalized, that
    // backlog is outside the finished recording and must not damage it when
    // the next drive starts.
    const std::string completed = "completed--drive", following = "following--drive";
    std::ofstream(current) << completed; std::ofstream(root + "/IsOffroad") << "0";
    SocketServer server(root + "/unused.sock", root + "/recipes"); Access::prepare(server, completed);
    Access::recipe(server, frame(kRecipeEventServiceIndex,
        R"({"kind":"anchor","sequence":1,"camera":"road","logMonoTime":123,"roadTimestampEof":456})"));
    Access::snapshot(server, frame(0, "{}")); Access::drain(server);
    assert(server.stats().snapshot_active && server.stats().snapshot_events == 1);

    std::ofstream(root + "/IsOffroad") << "1"; Access::recheck_road(server);
    Access::snapshot(server, frame(0, "{}", 2000)); Access::drain(server);
    assert(!server.stats().snapshot_active && server.stats().snapshot_route.empty());
    Access::snapshot(server, frame(0, "{}", 3000));
    Access::recipe(server, frame(kRecipeEventServiceIndex,
        R"({"kind":"anchor","sequence":2,"camera":"road","logMonoTime":234,"roadTimestampEof":567})", 3000));
    std::ofstream(current) << following; std::ofstream(root + "/IsOffroad") << "0";
    Access::drain(server); // Transition must invalidate the old cached offroad state itself.
    assert(!std::filesystem::exists(root + "/recipes/ui-invalid-" + completed));
    assert(Access::recipe_text(server).find("recording_fault") == std::string::npos);
    assert(server.stats().metadata_route == following && !server.stats().metadata_damaged);
    assert(server.stats().recipe_events == 0 && server.stats().snapshot_events == 0);

    Access::recipe(server, frame(kRecipeEventServiceIndex,
        R"({"kind":"anchor","sequence":3,"camera":"road","logMonoTime":345,"roadTimestampEof":678})", 4000));
    Access::snapshot(server, frame(0, "{}", 4000)); Access::drain(server);
    assert(server.stats().recipe_events == 1 && server.stats().snapshot_events == 1);
    Access::close(server);
  }
  for (const bool cleared_first : {false, true}) {
    // The bridge starts offroad while CurrentRoute still names a finished drive: after a runtime
    // update or restart, or at boot before openpilot clears the previous boot's value. This process
    // never recorded that drive, so its queued offroad traffic is not part of it, and the next
    // ignition must leave it intact whether openpilot clears CurrentRoute first or replaces it.
    // (The same holds for an epoch whose ignition ended before it saw onroad.)
    const std::string finished = cleared_first ? "finished--cleared" : "finished--replaced";
    const std::string following = cleared_first ? "following--cleared" : "following--replaced";
    std::ofstream(current) << finished; std::ofstream(root + "/IsOffroad") << "1";
    SocketServer server(root + "/unused.sock", root + "/recipes"); Access::prepare(server, finished);
    Access::snapshot(server, frame(0, "{}", 1000)); Access::drain(server);
    assert(!server.stats().snapshot_active && server.stats().snapshot_events == 0);
    // At ignition the UI's first records still carry the finished drive's epoch.
    Access::snapshot(server, frame(0, "{}", 2000));
    if (cleared_first) std::filesystem::remove(current);
    else std::ofstream(current) << following;
    std::ofstream(root + "/IsOffroad") << "0"; Access::recheck_road(server);
    Access::drain(server);
    if (cleared_first) {
      std::ofstream(current) << following;
      Access::snapshot(server, frame(0, "{}", 3000)); Access::drain(server);
    }
    assert(!std::filesystem::exists(root + "/recipes/ui-invalid-" + finished));
    assert(Access::recipe_text(server).find("recording_fault") == std::string::npos);
    assert(server.stats().metadata_route == following && !server.stats().metadata_damaged);
    assert(server.stats().route_transition_discarded == 1);
    // The next drive records normally.
    Access::recipe(server, frame(kRecipeEventServiceIndex,
        R"({"kind":"anchor","sequence":1,"camera":"road","logMonoTime":123,"roadTimestampEof":456})", 4000));
    Access::snapshot(server, frame(0, "{}", 4000)); Access::drain(server);
    assert(server.stats().recipe_events == 1 && server.stats().snapshot_events == 1);
    assert(server.stats().snapshot_route == following && !server.stats().metadata_damaged);
    Access::close(server);
    // A restarted bridge does not restore damage that was never recorded.
    SocketServer restarted(root + "/unused.sock", root + "/recipes"); Access::prepare(restarted, finished);
    assert(!restarted.stats().metadata_damaged);
    Access::close(restarted);
  }
  {
    // Finalization must not abandon a known damage marker while its storage
    // is unavailable. A directory at the temporary-file path fails even as root.
    const std::string recovery = "recovery--drive", recipes = root + "/recipes";
    std::ofstream(current) << recovery; std::ofstream(root + "/IsOffroad") << "0";
    SocketServer server(root + "/unused.sock", recipes); Access::prepare(server, recovery);
    Access::snapshot(server, frame(0, "{}")); Access::drain(server);
    assert(server.stats().snapshot_active);
    const std::string blocker = recipes + "/ui-invalid-" + recovery + ".tmp-" + std::to_string(getpid());
    assert(std::filesystem::create_directory(blocker));
    std::filesystem::remove(current);
    Access::snapshot(server, frame(0, "{}", 2000)); Access::drain(server);
    assert(server.stats().metadata_damaged && !std::filesystem::exists(recipes + "/ui-invalid-" + recovery));
    std::ofstream(current) << recovery; std::ofstream(root + "/IsOffroad") << "1"; Access::recheck_road(server);
    Access::snapshot(server, frame(0, "{}", 3000)); Access::drain(server);
    assert(!server.stats().snapshot_active && !std::filesystem::exists(recipes + "/ui-invalid-" + recovery));
    assert(std::filesystem::remove(blocker));
    Access::snapshot(server, frame(0, "{}", 4000));
    Access::snapshot(server, frame(0, "{}", 5000)); Access::drain(server);
    assert(std::filesystem::exists(recipes + "/ui-invalid-" + recovery));
    assert(server.stats().metadata_damaged && !server.stats().snapshot_active);
    Access::close(server);
  }
  {
    // A drive recorded across bridge restarts reads back whole and in order from its own files.
    const std::string split = "split--drive", following = "after--split", gone = "gone--drive";
    const std::string recipes = root + "/recipes", archive = root + "/realdata";
    const auto anchor = [](int sequence, uint64_t time) {
      return frame(kRecipeEventServiceIndex, R"({"kind":"anchor","sequence":)" + std::to_string(sequence) +
          R"(,"camera":"road","logMonoTime":)" + std::to_string(100 + sequence) +
          R"(,"roadTimestampEof":)" + std::to_string(200 + sequence) + "}", time);
    };
    const auto read_recipe = [](const std::string& id) {
      return commaview::runtime::source_recording_archive_response("/commaview/source-recording/recipe?route=" + id);
    };
    std::ofstream(current) << split; std::ofstream(root + "/IsOffroad") << "0";
    SocketServer before(root + "/unused.sock", recipes); Access::prepare(before, split);
    Access::recipe(before, anchor(1, 1000)); Access::drain(before); Access::close(before);
    SocketServer after(root + "/unused.sock", recipes); Access::prepare(after, split);
    Access::recipe(after, anchor(2, 2000)); Access::drain(after);
    assert(after.stats().recipe_events == 1);
    auto recipe = read_recipe(split);
    assert(recipe.status == 200 && recipe.body.rfind("{\"routeId\":\"" + split + "\",", 0) == 0);
    const size_t first = recipe.body.find("\"sequence\":1,"), second = recipe.body.find("\"sequence\":2,");
    assert(first != std::string::npos && second != std::string::npos && first < second);
    assert(std::count(recipe.body.begin(), recipe.body.end(), '\n') == 2);

    // The next drive removes the metadata of drives openpilot has since deleted. Drives with
    // segments left stay, and so does the new drive, whose segments don't exist yet.
    std::filesystem::create_directories(archive + "/" + split + "--0");
    const std::vector<std::string> gone_files = {recipes + "/ui-source-" + gone + ".500-1.jsonl",
        recipes + "/ui-snapshot-" + gone + ".jsonl", recipes + "/ui-invalid-" + gone};
    for (const std::string& path : gone_files) std::ofstream(path) << "{}\n";
    const std::string earlier_following = recipes + "/ui-source-" + following + ".400-1.jsonl";
    std::ofstream(earlier_following) << "{}\n";
    const std::string legacy = recipes + "/ui-source-500-2.jsonl";
    std::ofstream(legacy) << "{\"routeId\":\"" << gone << "\",\"sequence\":1}\n";
    std::ofstream(root + "/IsOffroad") << "1"; Access::recheck_road(after);
    Access::snapshot(after, frame(0, "{}", 3000)); Access::drain(after);  // Finalizes the recorded drive.
    std::ofstream(current) << following; std::ofstream(root + "/IsOffroad") << "0";
    Access::snapshot(after, frame(0, "{}", 4000)); Access::drain(after);
    assert(after.stats().metadata_route == following);
    for (const std::string& path : gone_files) assert(!std::filesystem::exists(path));
    assert(std::filesystem::exists(earlier_following));
    assert(std::filesystem::exists(legacy));  // Older runtimes' files are read only at startup.
    recipe = read_recipe(split);
    assert(recipe.status == 200 && std::count(recipe.body.begin(), recipe.body.end(), '\n') == 2);
    Access::close(after);

    // At startup older runtimes' files are read too: one goes once every drive it names has.
    const std::string kept_legacy = recipes + "/ui-source-500-3.jsonl";
    std::ofstream(kept_legacy) << "{\"routeId\":\"" << gone << "\"}\n{\"routeId\":\"" << split << "\"}\n";
    const std::string empty_legacy = recipes + "/ui-source-500-4.jsonl";
    std::ofstream(empty_legacy).close();
    const std::string unrelated = recipes + "/notes.txt";
    std::ofstream(unrelated) << "kept";
    // Nothing goes while the archive can't be listed or holds no drive (not mounted yet).
    assert(prune_recording_metadata(recipes, root + "/missing", {}, true) == 0);
    std::filesystem::create_directory(root + "/empty");
    assert(prune_recording_metadata(recipes, root + "/empty", {}, true) == 0);
    assert(prune_recording_metadata(recipes, archive, {following}, true) == 2);
    assert(!std::filesystem::exists(legacy) && !std::filesystem::exists(empty_legacy));
    assert(std::filesystem::exists(kept_legacy) && std::filesystem::exists(unrelated));
    assert(std::filesystem::exists(earlier_following));
    assert(prune_recording_metadata(recipes, archive, {}, false) == 1);
    assert(!std::filesystem::exists(earlier_following));
    assert(read_recipe(split).status == 200);
  }
  std::fflush(stderr); assert(dup2(saved, STDERR_FILENO) >= 0); close(saved); std::fclose(faults);
  std::ifstream logged(root + "/faults.log"); const std::string text((std::istreambuf_iterator<char>(logged)), {});
  assert(text.find("reason=snapshot_file_limit count=1 ") != std::string::npos);
  assert(text.find("reason=recording_fault_queue_overflow count=1 ") != std::string::npos);
  assert(text.find("private-invalid-payload") == std::string::npos);
  assert(text.find(route) == std::string::npos && text.find(next) == std::string::npos);
  assert(text.find(" recording metadata files of deleted drives") != std::string::npos);
  std::filesystem::remove_all(root); unsetenv("COMMAVIEWD_CURRENT_ROUTE_FILE");
  unsetenv("COMMAVIEWD_RECIPE_DIR"); unsetenv("COMMAVIEWD_SOURCE_ARCHIVE_ROOT");
  std::cout << "PASS: generation-bound backlog, fresh-route readiness, restarted-offroad routes stay intact, per-route recipes and retention, distinct first fault and bounded overflow accounting\n";
}
