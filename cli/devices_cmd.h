// Device selection for the CLI: `echoir devices`, and the mic and loopback inputs of transcribe and
// serve.
#pragma once

#include <string>

#include "capture/devices.h"

namespace echoir::cli {

// Prints one line per device on standard output. Throws std::runtime_error when the audio system
// cannot be opened.
void run_devices_list();

// Throws UsageError when the source cannot be captured with the audio backend in use. The message
// says what to do instead.
void check_source_available(capture::Source source);

// The id of the device that selector names: a position in the list from `echoir devices`, or part
// of a device name, matched without regard to case. An empty selector is the default device, which
// the capture follows, so the result is empty too. Throws UsageError when no device or more than
// one device matches.
std::string device_id_for(capture::Source source, const std::string& selector);

}  // namespace echoir::cli
