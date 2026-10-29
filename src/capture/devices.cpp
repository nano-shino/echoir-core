#include "capture/devices.h"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "json.hpp"
#include "text/env.h"

namespace echoir::capture {
namespace {

std::vector<Device> to_public(const std::vector<ma_device_info>& list) {
    std::vector<Device> out;
    out.reserve(list.size());
    for (const ma_device_info& info : list) {
        out.push_back(Device{device_id_text(info.id), std::string(info.name), info.isDefault != 0});
    }
    return out;
}

nlohmann::json device_array(const std::vector<Device>& list) {
    nlohmann::json out = nlohmann::json::array();
    for (const Device& device : list) {
        out.push_back({{"id", device.id}, {"name", device.name}, {"default", device.is_default}});
    }
    return out;
}

}  // namespace

AudioContext::AudioContext() {
    ma_context_config config = ma_context_config_init();
    // iOS: record and play at once, mixed with other audio, with output on the speaker.
    config.coreaudio.sessionCategory = ma_ios_session_category_play_and_record;
    config.coreaudio.sessionCategoryOptions = static_cast<ma_uint32>(
        ma_ios_session_category_option_mix_with_others |
        ma_ios_session_category_option_default_to_speaker);

    ma_result result = MA_SUCCESS;
    const std::optional<std::string> forced = text::get_env("ECHOIR_AUDIO_BACKEND");
    if (forced && *forced == "null") {
        const ma_backend null_backend = ma_backend_null;
        result = ma_context_init(&null_backend, 1, &config, &context_);
    } else {
        result = ma_context_init(nullptr, 0, &config, &context_);
    }
    if (result != MA_SUCCESS) {
        throw std::runtime_error(std::string("cannot open the audio system: ") +
                                 ma_result_description(result));
    }
}

AudioContext::~AudioContext() { ma_context_uninit(&context_); }

bool AudioContext::loopback_supported() const {
    return ma_is_loopback_supported(context_.backend) == MA_TRUE;
}

RawDevices enumerate_devices(ma_context& context) {
    ma_device_info* playback = nullptr;
    ma_uint32 playback_count = 0;
    ma_device_info* capture = nullptr;
    ma_uint32 capture_count = 0;
    const ma_result result =
        ma_context_get_devices(&context, &playback, &playback_count, &capture, &capture_count);
    if (result != MA_SUCCESS) {
        throw std::runtime_error(std::string("cannot list audio devices: ") +
                                 ma_result_description(result));
    }
    RawDevices out;
    out.playback.assign(playback, playback + playback_count);
    out.capture.assign(capture, capture + capture_count);
    return out;
}

std::string device_id_text(const ma_device_id& id) {
    static constexpr char kHex[] = "0123456789abcdef";
    const auto* bytes = reinterpret_cast<const unsigned char*>(&id);
    // The id is a union sized for the largest backend, so a short id leaves zero padding. Dropping
    // trailing zeros keeps the text short and stable.
    std::size_t size = sizeof(ma_device_id);
    while (size > 1 && bytes[size - 1] == 0) {
        --size;
    }
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0x0F]);
    }
    return out;
}

DeviceList list_devices() {
    AudioContext context;
    const RawDevices raw = enumerate_devices(context.get());
    DeviceList out;
    out.capture = to_public(raw.capture);
    if (context.loopback_supported()) {
        out.loopback = to_public(raw.playback);
    }
    return out;
}

bool loopback_available() {
    try {
        const AudioContext context;
        return context.loopback_supported();
    } catch (const std::runtime_error&) {
        return false;
    }
}

std::string devices_json(const DeviceList& devices) {
    const nlohmann::json doc = {{"capture", device_array(devices.capture)},
                                {"loopback", device_array(devices.loopback)}};
    // Names come from the OS, so bad UTF-8 is replaced rather than failing the whole list.
    return doc.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace echoir::capture
