#include "recording_records.h"

#include <cassert>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

using commaview::runtime::is_recording_records_path;
using commaview::runtime::recording_records_response;
using commaview::runtime::RecordingRecordLimits;

namespace {

std::string jpeg(size_t size) {
  std::string data(size, 'x');
  data[0] = static_cast<char>(0xFF);
  data[1] = static_cast<char>(0xD8);
  data[size - 2] = static_cast<char>(0xFF);
  data[size - 1] = static_cast<char>(0xD9);
  return data;
}

bool exists(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0;
}

void set_mtime(const std::string& path, time_t seconds) {
  struct timeval times[2] = {{seconds, 0}, {seconds, 0}};
  assert(utimes(path.c_str(), times) == 0);
}

}  // namespace

int main() {
  char path_template[] = "/tmp/commaview-recording-records-XXXXXX";
  char* base = mkdtemp(path_template);
  assert(base != nullptr);
  const std::string dir = std::string(base) + "/data/recordings";

  assert(is_recording_records_path("/commaview/recordings"));
  assert(is_recording_records_path("/commaview/recordings/thumb?id=a"));
  assert(!is_recording_records_path("/commaview/recordingsx"));
  assert(!is_recording_records_path("/commaview/source-recording/manifest"));

  // Nothing saved yet: an empty list, and the folder isn't made just to list it.
  auto empty = recording_records_response("GET", "/commaview/recordings", "", dir);
  assert(empty.status == 200);
  assert(empty.body == "{\"ok\":true,\"recordings\":[],\"truncated\":false}");
  assert(!exists(dir));

  const std::string journal = "{\"schemaVersion\":1,\"routeId\":\"000004b4--75e1f0ba8f\",\"status\":\"stopped\"}";
  auto saved = recording_records_response("POST", "/commaview/recordings/journal?id=source_1_2", journal, dir);
  assert(saved.status == 200);
  assert(exists(dir + "/source_1_2.json"));
  struct stat st {};
  assert(stat(dir.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);

  auto thumb = recording_records_response("POST", "/commaview/recordings/thumb?id=source_1_2", jpeg(100), dir);
  assert(thumb.status == 200);

  auto list = recording_records_response("GET", "/commaview/recordings", "", dir);
  assert(list.status == 200);
  assert(list.body.find("\"id\":\"source_1_2\"") != std::string::npos);
  assert(list.body.find("\"thumb\":true") != std::string::npos);
  assert(list.body.find("\"journal\":" + journal) != std::string::npos);

  auto got = recording_records_response("GET", "/commaview/recordings/thumb?id=source_1_2", "", dir);
  assert(got.status == 200 && got.content_type == "image/jpeg" && got.body == jpeg(100));
  assert(recording_records_response("GET", "/commaview/recordings/thumb?id=source_9_9", "", dir).status == 404);

  // Ids, bodies and sizes it refuses.
  assert(recording_records_response("POST", "/commaview/recordings/journal?id=../x", journal, dir).status == 400);
  assert(recording_records_response("POST", "/commaview/recordings/journal?id=a/b", journal, dir).status == 400);
  assert(recording_records_response("POST", "/commaview/recordings/journal", journal, dir).status == 400);
  assert(recording_records_response("POST", "/commaview/recordings/journal?id=a&x=1", journal, dir).status == 400);
  assert(recording_records_response("POST", "/commaview/recordings/journal?id=a", "[1]", dir).status == 400);
  assert(recording_records_response("POST", "/commaview/recordings/journal?id=a", "{\"a\":\"\x01\"}", dir).status == 400);
  assert(recording_records_response("POST", "/commaview/recordings/thumb?id=a", "not a jpeg", dir).status == 400);
  RecordingRecordLimits small;
  small.max_journal_bytes = 16;
  assert(recording_records_response("POST", "/commaview/recordings/journal?id=a", journal, dir, small).status == 413);
  assert(recording_records_response("POST", "/commaview/recordings/other?id=a", journal, dir).status == 404);
  assert(recording_records_response("GET", "/commaview/recordings/journal?id=a", "", dir).status == 404);
  assert(!exists(dir + "/a.json"));

  // A thumbnail can come before its record; the listing shows records only.
  assert(recording_records_response("POST", "/commaview/recordings/thumb?id=source_3_3", jpeg(10), dir).status == 200);
  list = recording_records_response("GET", "/commaview/recordings", "", dir);
  assert(list.body.find("source_3_3") == std::string::npos);

  // Newest first, and only the newest max_records are kept (with their thumbnails).
  RecordingRecordLimits three;
  three.max_records = 3;
  for (int i = 0; i < 4; ++i) {
    const std::string id = "source_10" + std::to_string(i) + "_0";
    assert(recording_records_response("POST", "/commaview/recordings/journal?id=" + id, journal, dir, three).status == 200);
    set_mtime(dir + "/" + id + ".json", 1000000 + i);
  }
  set_mtime(dir + "/source_1_2.json", 999);
  assert(recording_records_response("POST", "/commaview/recordings/journal?id=source_104_0", journal, dir, three).status == 200);
  assert(!exists(dir + "/source_1_2.json"));
  assert(!exists(dir + "/source_1_2.jpg"));
  assert(!exists(dir + "/source_100_0.json") && !exists(dir + "/source_101_0.json"));
  assert(exists(dir + "/source_102_0.json") && exists(dir + "/source_104_0.json"));
  list = recording_records_response("GET", "/commaview/recordings", "", dir);
  assert(list.body.find("source_104_0") < list.body.find("source_103_0"));
  assert(list.body.find("source_103_0") < list.body.find("source_102_0"));

  // A listing stops before it would grow past its cap, and says so.
  RecordingRecordLimits tiny;
  tiny.max_listing_bytes = 200;
  list = recording_records_response("GET", "/commaview/recordings", "", dir, tiny);
  assert(list.body.find("\"truncated\":true") != std::string::npos);
  assert(list.body.size() <= 200);

  // Delete takes the record and its thumbnail.
  assert(recording_records_response("POST", "/commaview/recordings/thumb?id=source_104_0", jpeg(10), dir).status == 200);
  assert(recording_records_response("POST", "/commaview/recordings/delete?id=source_104_0", "", dir).status == 200);
  assert(!exists(dir + "/source_104_0.json") && !exists(dir + "/source_104_0.jpg"));

  // A symlink planted as a record is never read.
  assert(symlink("/etc/passwd", (dir + "/source_7_7.json").c_str()) == 0);
  list = recording_records_response("GET", "/commaview/recordings", "", dir);
  assert(list.body.find("source_7_7") == std::string::npos);
  assert(list.body.find("root:") == std::string::npos);

  std::string cmd = std::string("rm -rf ") + base;
  assert(std::system(cmd.c_str()) == 0);
  return 0;
}
