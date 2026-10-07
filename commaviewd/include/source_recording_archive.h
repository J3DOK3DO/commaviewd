#pragma once

#include "http_server.h"

#include <string>

namespace commaview::runtime {

// The caller must authenticate before invoking this.
commaview::api::HttpResponse source_recording_archive_response(
    const std::string& request_path);
commaview::api::HttpResponse source_recording_current_response(
    const std::string& route_id);
// The caller reads route/status/route in that order, off the frame path.
commaview::api::HttpResponse source_recording_preflight_response(
    const std::string& route_before, const std::string& status_json,
    const std::string& route_after, bool onroad_after);

}  // namespace commaview::runtime
