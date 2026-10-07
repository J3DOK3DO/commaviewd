#include "source_recording_archive.h"

#include <cassert>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using commaview::runtime::source_recording_archive_response;
using commaview::runtime::source_recording_current_response;

int main() {
  char path_template[] = "/tmp/commaview-source-archive-XXXXXX";
  char* base = mkdtemp(path_template);
  assert(base != nullptr);
  const std::string root = base;
  const std::string route = "000004b4--75e1f0ba8f";
  assert(source_recording_current_response(route).body.find(route) != std::string::npos);
  assert(source_recording_current_response("../secret").status == 404);
  const auto preflight = commaview::runtime::source_recording_preflight_response(route, "{\"roadState\":\"onroad\"}", route, true);
  assert(preflight.status == 200);
  assert(preflight.body.find("\"recordingStatus\":{\"roadState\":\"onroad\"}") != std::string::npos);
  assert(commaview::runtime::source_recording_preflight_response(route, "{}", "new--route", true).status == 404);
  assert(commaview::runtime::source_recording_preflight_response(route, "{}", route, false).status == 403);
  const std::string segment = root + "/" + route + "--0";
  const std::string recipes = root + "/recipes";
  assert(mkdir(segment.c_str(), 0700) == 0);
  assert(mkdir(recipes.c_str(), 0700) == 0);
  assert(setenv("COMMAVIEWD_SOURCE_ARCHIVE_ROOT", root.c_str(), 1) == 0);
  assert(setenv("COMMAVIEWD_RECIPE_DIR", recipes.c_str(), 1) == 0);
  {
    std::ofstream media(segment + "/fcamera.hevc", std::ios::binary);
    media << "0123456789";
    std::ofstream(segment + "/ecamera.hevc", std::ios::binary) << "wide";
    std::ofstream(segment + "/rlog.zst", std::ios::binary) << "log";
    std::ofstream recipe(recipes + "/ui-source-1.jsonl");
    recipe << "{\"routeId\":\"" << route << "\",\"sequence\":1}\n";
    recipe << "{\"routeId\":\"other--route\",\"sequence\":2}\n";
    std::ofstream(recipes + "/ui-snapshot-" + route + ".jsonl") << "snapshot\n";
  }
  const std::string range = "/commaview/source-recording/range?route=" + route +
      "&segment=0&kind=road&offset=2&length=4";
  const auto media = source_recording_archive_response(range);
  assert(media.status == 200 && media.body == "2345");
  assert(media.content_type == "application/octet-stream");
  assert(media.headers.at("X-Source-Size") == "10");
  assert(media.headers.at("X-Source-Offset") == "2");
  assert(source_recording_archive_response(range + "&offset=4").status == 400);
  assert(source_recording_archive_response(
      "/commaview/source-recording/range?route=../etc&segment=0&kind=road&offset=0&length=4").status == 400);
  assert(source_recording_archive_response(
      "/commaview/source-recording/range?route=" + route +
      "&segment=0&kind=road&offset=9&length=4").status == 400);
  assert(source_recording_archive_response(
      "/commaview/source-recording/range?route=" + route +
      "&segment=0&kind=road&offset=0&length=262145").status == 400);
  const auto recipe = source_recording_archive_response(
      "/commaview/source-recording/recipe?route=" + route);
  assert(recipe.status == 200 && recipe.body.find("\"sequence\":1") != std::string::npos);
  assert(recipe.body.find("other--route") == std::string::npos);
  const std::string manifest_path = "/commaview/source-recording/manifest?route=" + route;
  assert(source_recording_archive_response(manifest_path).body ==
         "{\"routeId\":\"" + route + "\",\"segments\":[0],\"incompleteSegments\":[]}");
  assert(source_recording_archive_response(
      "/commaview/source-recording/manifest?route=absent--route").status == 404);
  // An empty/missing required file does not mean the entire route was deleted.
  std::ofstream(segment + "/ecamera.hevc", std::ios::binary | std::ios::trunc).close();
  assert(unlink((segment + "/rlog.zst").c_str()) == 0);
  const auto incomplete = source_recording_archive_response(manifest_path);
  assert(incomplete.status == 200);
  assert(incomplete.body == "{\"routeId\":\"" + route +
         "\",\"segments\":[],\"incompleteSegments\":[{\"segment\":0,\"missing\":[\"wide\",\"rlog\"]}]}");
  std::ofstream(segment + "/ecamera.hevc", std::ios::binary) << "wide";
  std::ofstream(segment + "/rlog.zst", std::ios::binary) << "log";
  assert(source_recording_archive_response(manifest_path).body.find("\"segments\":[0]") != std::string::npos);
  // Repeated reads on different clients leave the original segment intact.
  assert(source_recording_archive_response(range).body == "2345");
  assert(source_recording_archive_response(range).body == "2345");
  struct stat original = {};
  assert(stat((segment + "/fcamera.hevc").c_str(), &original) == 0 && original.st_size == 10);
  const std::string link = root + "/" + route + "--1";
  assert(symlink(segment.c_str(), link.c_str()) == 0);
  assert(source_recording_archive_response(
      "/commaview/source-recording/range?route=" + route +
      "&segment=1&kind=road&offset=0&length=4").status == 404);
  assert(source_recording_archive_response(manifest_path).body.find("[0]") != std::string::npos);
  assert(source_recording_archive_response(
      "/commaview/source-recording/range?route=" + route +
      "&segment=0&kind=snapshot&offset=0&length=8").body == "snapshot");
  // The client accepts at most 64 KiB. Even the maximum missing-file manifest fits, and
  // incomplete folders count toward the same route limit as complete ones.
  const std::string bounded_route = "bounded--route";
  for (int number = 0; number < 1000; ++number) {
    assert(mkdir((root + "/" + bounded_route + "--" + std::to_string(number)).c_str(), 0700) == 0);
  }
  const std::string bounded_path = "/commaview/source-recording/manifest?route=" + bounded_route;
  const auto bounded = source_recording_archive_response(bounded_path);
  assert(bounded.status == 200 && bounded.body.size() <= 64 * 1024);
  assert(mkdir((root + "/" + bounded_route + "--1000").c_str(), 0700) == 0);
  assert(source_recording_archive_response(bounded_path).status == 413);
  for (int number = 0; number <= 1000; ++number) {
    assert(rmdir((root + "/" + bounded_route + "--" + std::to_string(number)).c_str()) == 0);
  }
  // A durable metadata fault refuses all archive reads, including after runtime restart,
  // while preserving the comma originals and leaving a different route available.
  const std::string fault = recipes + "/ui-invalid-" + route;
  std::ofstream(fault) << "recording metadata incomplete\n";
  assert(source_recording_archive_response(manifest_path).status == 409);
  assert(source_recording_archive_response(range).status == 409);
  assert(source_recording_archive_response(
      "/commaview/source-recording/recipe?route=" + route).status == 409);
  assert(source_recording_archive_response(
      "/commaview/source-recording/manifest?route=absent--route").status == 404);
  assert(stat((segment + "/fcamera.hevc").c_str(), &original) == 0 && original.st_size == 10);
  assert(unlink(fault.c_str()) == 0);
  assert(source_recording_archive_response(range).body == "2345");
  unlink(link.c_str());
  unlink((segment + "/fcamera.hevc").c_str());
  unlink((segment + "/ecamera.hevc").c_str());
  unlink((segment + "/rlog.zst").c_str());
  unlink((recipes + "/ui-source-1.jsonl").c_str());
  unlink((recipes + "/ui-snapshot-" + route + ".jsonl").c_str());
  rmdir(segment.c_str());
  rmdir(recipes.c_str());
  rmdir(root.c_str());
  std::puts("PASS: source archive reads, damaged/incomplete/absent routes, and traversal guards");
}
