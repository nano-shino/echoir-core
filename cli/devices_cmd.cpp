#include "devices_cmd.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <string>
#include <system_error>
#include <vector>

#include "options.h"

namespace echoir::cli {
namespace {

void print_devices(const std::vector<capture::Device>& devices) {
    for (std::size_t i = 0; i < devices.size(); ++i) {
        std::printf("  [%zu] %s%s\n", i, devices[i].name.c_str(),
                    devices[i].is_default ? " (default)" : "");
    }
}

std::string lowercase(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

// "[0] Name, [1] Name" for the devices at the given positions, for an error message.
std::string listing(const std::vector<capture::Device>& devices,
                    const std::vector<std::size_t>& positions) {
    if (positions.empty()) {
        return "none";
    }
    std::string out;
    for (const std::size_t i : positions) {
        if (!out.empty()) {
            out += ", ";
        }
        out += "[" + std::to_string(i) + "] " + devices[i].name;
    }
    return out;
}

}  // namespace

void run_devices_list() {
    const capture::DeviceList devices = capture::list_devices();

    std::printf("capture:\n");
    if (devices.capture.empty()) {
        std::printf("  (none)\n");
    }
    print_devices(devices.capture);

    std::printf("loopback:\n");
    if (devices.loopback.empty()) {
        if (capture::loopback_available()) {
            std::printf("  (none)\n");
        } else {
            std::printf("  (none: not available with this audio backend)\n");
        }
    }
    print_devices(devices.loopback);
    std::fflush(stdout);
}

void check_source_available(capture::Source source) {
    if (source == capture::Source::loopback && !capture::loopback_available()) {
        throw UsageError(
            "loopback is not available with this audio backend (it needs WASAPI on Windows); on "
            "other platforms route the audio to a loopback device (such as BlackHole on macOS) and "
            "pass it as --input mic:<device>");
    }
}

std::string device_id_for(capture::Source source, const std::string& selector) {
    if (selector.empty()) {
        return {};
    }
    const capture::DeviceList list = capture::list_devices();
    const bool loopback = source == capture::Source::loopback;
    const std::vector<capture::Device>& devices = loopback ? list.loopback : list.capture;
    const std::string what = loopback ? "loopback" : "capture";

    std::vector<std::size_t> all(devices.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        all[i] = i;
    }

    const bool digits = std::all_of(selector.begin(), selector.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
    });
    if (digits) {
        std::size_t index = 0;
        const auto parsed =
            std::from_chars(selector.data(), selector.data() + selector.size(), index);
        if (parsed.ec == std::errc{} && index < devices.size()) {
            return devices[index].id;
        }
        throw UsageError("no " + what + " device has index " + selector +
                         "; available: " + listing(devices, all));
    }

    const std::string needle = lowercase(selector);
    std::vector<std::size_t> hits;
    for (std::size_t i = 0; i < devices.size(); ++i) {
        if (lowercase(devices[i].name).find(needle) != std::string::npos) {
            hits.push_back(i);
        }
    }
    if (hits.size() == 1) {
        return devices[hits[0]].id;
    }
    if (hits.empty()) {
        throw UsageError("no " + what + " device matches '" + selector +
                         "'; available: " + listing(devices, all));
    }
    throw UsageError("'" + selector + "' matches " + std::to_string(hits.size()) + " " + what +
                     " devices: " + listing(devices, hits) + "; use an index instead");
}

}  // namespace echoir::cli
