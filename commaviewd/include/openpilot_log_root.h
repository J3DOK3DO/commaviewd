#pragma once

#include <string>
#include <sys/stat.h>

namespace commaview::runtime {

// Where loggerd writes drives (system/hardware/hw.py Paths.log_root()). StarPilot moves it while its
// High Bitrate Recording (/cache/use_HD) or Konik (/cache/use_konik) marker exists. Other forks
// ignore those markers, which outlive a fork change on /cache, so they count only under StarPilot.
inline std::string openpilot_log_root(const std::string& openpilot_dir = "/data/openpilot",
                                      const std::string& cache_dir = "/cache",
                                      const std::string& media_dir = "/data/media/0") {
  struct stat st {};
  auto is_file = [&st](const std::string& path) { return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode); };
  if (stat((openpilot_dir + "/starpilot").c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    if (is_file(cache_dir + "/use_HD")) return media_dir + "/realdata_HD";
    if (is_file(cache_dir + "/use_konik")) return media_dir + "/realdata_konik";
  }
  return media_dir + "/realdata";
}

}  // namespace commaview::runtime
