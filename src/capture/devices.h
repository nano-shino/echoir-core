// Audio devices, listed and opened through miniaudio. The audio backend is the system's own. With
// ECHOIR_AUDIO_BACKEND=null it is miniaudio's null backend only, which has one silent device, so
// tests and CI can run without audio hardware.
#pragma once

#include <string>
#include <vector>

#include "audio/miniaudio_config.h"

namespace echoir::capture {

// What a capture reads: a capture device (the microphone), or the output of a playback device
// (loopback, where the audio backend has it).
enum class Source { mic, loopback };

struct Device {
    // Stable across runs: lowercase hex of the bytes of miniaudio's device id, without trailing
    // zero bytes. Passed back to the library to pick this device.
    std::string id;
    std::string name;  // UTF-8
    bool is_default = false;
};

struct DeviceList {
    std::vector<Device> capture;
    // The playback devices, which loopback records. Empty when the audio backend has no loopback.
    std::vector<Device> loopback;
};

// A miniaudio context, closed when it goes out of scope. It is needed to list and open devices.
class AudioContext {
public:
    // Throws std::runtime_error when the audio system cannot be opened.
    AudioContext();
    ~AudioContext();
    AudioContext(const AudioContext&) = delete;
    AudioContext& operator=(const AudioContext&) = delete;

    ma_context& get() { return context_; }
    // True when the backend can record playback devices.
    bool loopback_supported() const;

private:
    ma_context context_{};
};

// The raw device infos of a context. The entries are copies, so they outlive the next listing.
struct RawDevices {
    std::vector<ma_device_info> capture;
    std::vector<ma_device_info> playback;
};

// Throws std::runtime_error when the devices cannot be listed.
RawDevices enumerate_devices(ma_context& context);

// The id text of a device, as Device::id gives it.
std::string device_id_text(const ma_device_id& id);

// Lists the devices. Throws std::runtime_error when the audio system cannot be opened.
DeviceList list_devices();

// True when loopback can be captured with the audio backend in use. False as well when the audio
// system cannot be opened.
bool loopback_available();

// The list as JSON: {"capture":[{"id","name","default"}],"loopback":[...]}, UTF-8.
std::string devices_json(const DeviceList& devices);

}  // namespace echoir::capture
