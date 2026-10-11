// A short POSIX write need not set errno. Exercise the real damage writer with
// that failure, without filling the host's disk or requiring an AF_UNIX socket.
#include "ui_export_socket.h"
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

static bool short_marker_write = true;
extern "C" ssize_t __real_write(int fd, const void* data, size_t size);
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t size) {
  const char marker[] = "recording metadata incomplete\n";
  if (short_marker_write && size == sizeof(marker) - 1 && std::memcmp(data, marker, size) == 0) {
    short_marker_write = false;
    const ssize_t written = __real_write(fd, data, 2);
    assert(written == 2);
    errno = ENOENT;  // Deliberately stale, unrelated to this successful partial write.
    return written;
  }
  return __real_write(fd, data, size);
}

namespace commaview::ui_export {
struct RecordingMetadataTestAccess {
  static void enable(SocketServer& server, const char* stamp) {
    server.recipe_enabled_ = true;
    server.recipe_stamp_ = stamp;
  }
  static void close(SocketServer& server) {
    if (server.recipe_file_) std::fclose(server.recipe_file_);
    server.recipe_file_ = nullptr;
    server.recipe_enabled_ = false;
  }
  static void old_route_failure(SocketServer& server) {
    enable(server, "1000-2");
    server.observe_route("old--drive");
    auto old_epoch = server.active_epoch_;
    server.observe_route("new--drive");
    short_marker_write = true;
    server.mark_route_damage(old_epoch, "route_transition_discarded");
    // Failure to persist the discarded OLD backlog must not fabricate a
    // metadata write failure for the new drive (which is terminal in Android).
    assert(!server.metadata_damaged_);
    assert(server.recipe_write_failures_ == 0);
    assert(!old_epoch->damage_recorded);
    assert(server.recording_faults_.back().error_number == EIO);
    server.mark_route_damage(old_epoch, "route_transition_discarded");
    assert(old_epoch->damage_recorded);
    assert(std::filesystem::exists(server.recipe_dir_ + "/ui-invalid-old--drive"));
    assert(!std::filesystem::exists(server.recipe_dir_ + "/ui-invalid-new--drive"));
    assert(server.recipe_write_failures_ == 0);
    // The fault row is in the old route's own recipe file, never in the new drive's.
    std::ifstream old_recipe(server.recipe_dir_ + "/ui-source-old--drive.1000-2.jsonl");
    const std::string text((std::istreambuf_iterator<char>(old_recipe)), {});
    assert(text.find("\"routeId\":\"old--drive\",\"kind\":\"recording_fault\"") != std::string::npos);
    assert(!std::filesystem::exists(server.recipe_dir_ + "/ui-source-new--drive.1000-2.jsonl"));
    close(server);
  }

  static void exercise(SocketServer& server, const std::string& route) {
    enable(server, "1000-1");
    server.observe_route(route);
    server.mark_route_damage(server.active_epoch_, "route_transition_discarded");
    assert(server.metadata_damaged_ && !server.active_epoch_->damage_recorded);
    assert(server.recipe_write_failures_ == 1);
    assert(server.recording_faults_.size() == 1);
    assert(server.recording_faults_.front().error_number == EIO);
    assert(!std::filesystem::exists(server.recipe_dir_ + "/ui-invalid-" + route));

    // Recovery persists the marker but must retain the known integrity failure.
    server.mark_route_damage(server.active_epoch_, "route_transition_discarded");
    assert(server.active_epoch_->damage_recorded && server.metadata_damaged_);
    assert(server.recipe_write_failures_ == 1);
    assert(std::filesystem::exists(server.recipe_dir_ + "/ui-invalid-" + route));
    close(server);
  }
};
}

int main() {
  char temp[] = "/tmp/commaview-marker-XXXXXX";
  const char* directory = mkdtemp(temp); assert(directory != nullptr);
  {
    commaview::ui_export::SocketServer server(std::string(directory) + "/unused.sock", directory);
    commaview::ui_export::RecordingMetadataTestAccess::exercise(server, "short--write");
    assert(server.stats().metadata_damaged && server.stats().recipe_write_failures == 1);
  }
  {
    commaview::ui_export::SocketServer server(std::string(directory) + "/unused.sock", directory);
    commaview::ui_export::RecordingMetadataTestAccess::old_route_failure(server);
  }
  std::filesystem::remove_all(directory);
  std::cout << "PASS: partial marker write reports EIO, retries persistence and retains integrity failure\n";
}
