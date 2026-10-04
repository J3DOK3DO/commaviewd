#pragma once

#include "http_server.h"

#include <cstddef>
#include <string>

namespace commaview::runtime {

// Recording records: what a paired device keeps of each recording it made (the drive, the
// interval, the look; a few KB of JSON) and its thumbnail, kept on the comma so every paired
// device lists them and can make the replay from the comma's own drive. The heavy files are the
// comma's drives, never these.
//
//   GET  /commaview/recordings                 every record, newest first
//   GET  /commaview/recordings/thumb?id=<id>   a record's thumbnail (image/jpeg)
//   POST /commaview/recordings/journal?id=<id> save a record (a JSON object)
//   POST /commaview/recordings/thumb?id=<id>   save its thumbnail (a JPEG)
//   POST /commaview/recordings/delete?id=<id>  delete a record and its thumbnail
//
// The caller must authenticate first. `dir` is where the records live; it's created on the
// first save.
struct RecordingRecordLimits {
  size_t max_journal_bytes = 32 * 1024;
  size_t max_thumb_bytes = 256 * 1024;
  // The oldest go when there are more; a recording older than its drive is no use anyway.
  size_t max_records = 300;
  // A listing stops before it grows past this.
  size_t max_listing_bytes = 2 * 1024 * 1024;
};

bool is_recording_records_path(const std::string& request_path);

commaview::api::HttpResponse recording_records_response(const std::string& method,
                                                        const std::string& request_path,
                                                        const std::string& body,
                                                        const std::string& dir,
                                                        const RecordingRecordLimits& limits = {});

}  // namespace commaview::runtime
