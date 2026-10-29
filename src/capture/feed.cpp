#include "capture/feed.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "audio/spsc_ring.h"
#include "errors.h"
#include "stream/session.h"

namespace echoir::capture {
namespace {

// How much audio the ring holds. A feeder that falls this far behind drops frames.
constexpr double kRingSeconds = 2.0;
// How often the feeder drains the ring when nothing wakes it.
constexpr std::chrono::milliseconds kDrainInterval{20};
// A lost default device is retried quickly at first, while the system switches devices, then
// slowly, until a device comes back or the capture is stopped.
constexpr int kQuickReopenAttempts = 5;
constexpr std::chrono::milliseconds kQuickReopenInterval{200};
constexpr std::chrono::milliseconds kSlowReopenInterval{1000};
// The largest piece that one push hands to the session, in seconds of audio.
constexpr double kPushSeconds = 0.1;

// The ERROR detail for a device that cannot be used any more.
constexpr const char* kDeviceLost = "device_lost";

std::string default_name(const std::vector<ma_device_info>& list) {
    for (const ma_device_info& info : list) {
        if (info.isDefault != 0) {
            return std::string(info.name);
        }
    }
    return "system default";
}

}  // namespace

// Lifecycle. The device is opened in the constructor. Only the owner thread calls start and stop.
// The feeder thread drains the ring, and reopens a default device that was lost. device_mu_ guards
// the device object: it is held while the device opens, starts, stops or closes. The audio thread
// reads the ring and the format, and both change only while the device is closed.
struct CaptureFeed::Impl {
    Impl(Source source, std::string device_id, Session& session)
        : source_(source), chosen_id_(std::move(device_id)), session_(session) {
        if (source_ == Source::loopback && !context_.loopback_supported()) {
            throw NotSupported(
                "loopback is not available with this audio backend (it needs WASAPI on Windows)");
        }
        std::lock_guard<std::mutex> lock(device_mu_);
        open_device();
    }

    ~Impl() {
        stop();
        std::lock_guard<std::mutex> lock(device_mu_);
        close_locked();
    }

    void start() {
        if (started_) {
            throw StateError("the capture is already started");
        }
        {
            std::lock_guard<std::mutex> lock(device_mu_);
            start_locked();
        }
        started_ = true;
        try {
            feeder_ = std::thread([this] { run_feeder_guarded(); });
        } catch (...) {
            std::lock_guard<std::mutex> lock(device_mu_);
            close_locked();
            throw;
        }
    }

    void stop() {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        // Set first, so a reopen that is waiting for the device lock gives up.
        {
            std::lock_guard<std::mutex> lock(wake_mu_);
            stop_requested_ = true;
        }
        wake_cv_.notify_all();
        {
            std::lock_guard<std::mutex> lock(device_mu_);
            // The stop is not a loss, so the notification it causes is ignored.
            closing_ = true;
            if (device_) {
                ma_device_stop(device_.get());
            }
        }
        // The device has stopped, so every sample it wrote is in the ring. The feeder drains once
        // more when it sees finish_, then exits.
        {
            std::lock_guard<std::mutex> lock(wake_mu_);
            finish_ = true;
        }
        wake_cv_.notify_all();
        if (feeder_.joinable()) {
            feeder_.join();
        }
    }

    // Opens the device, sized from its real format. Called with device_mu_ held, and the device
    // must be closed. The device is not started.
    void open_device() {
        const RawDevices devices = enumerate_devices(context_.get());
        const bool loopback = source_ == Source::loopback;
        const std::vector<ma_device_info>& list = loopback ? devices.playback : devices.capture;
        const char* what = loopback ? "loopback" : "capture";

        ma_device_id id{};
        ma_device_config config =
            ma_device_config_init(loopback ? ma_device_type_loopback : ma_device_type_capture);
        config.capture.format = ma_format_f32;
        // Zero takes the device's own rate and channels, which are read back after init.
        config.sampleRate = 0;
        config.capture.channels = 0;
        // Android: the voice recognition preset gives unprocessed input with no system effects.
        config.aaudio.inputPreset = ma_aaudio_input_preset_voice_recognition;
        config.dataCallback = &Impl::on_data;
        config.notificationCallback = &Impl::on_notification;
        config.pUserData = this;

        std::string name;
        if (chosen_id_.empty()) {
            name = default_name(list);
        } else {
            const auto it =
                std::find_if(list.begin(), list.end(), [this](const ma_device_info& info) {
                    return device_id_text(info.id) == chosen_id_;
                });
            if (it == list.end()) {
                throw std::invalid_argument("no " + std::string(what) + " device has id " +
                                            chosen_id_);
            }
            id = it->id;
            config.capture.pDeviceID = &id;
            name = it->name;
        }

        auto device = std::make_unique<ma_device>();
        const ma_result result = ma_device_init(&context_.get(), &config, device.get());
        if (result != MA_SUCCESS) {
            throw std::runtime_error("cannot open the " + std::string(what) + " device " + name +
                                     ": " + ma_result_description(result));
        }
        const std::uint32_t rate = device->sampleRate;
        const std::uint32_t channels = device->capture.channels;
        if (rate == 0 || channels == 0) {
            ma_device_uninit(device.get());
            throw std::runtime_error("the " + std::string(what) + " device " + name +
                                     " reports no usable format");
        }
        const auto ring_samples =
            static_cast<std::size_t>(static_cast<double>(rate) * kRingSeconds) * channels;

        device_ = std::move(device);
        ring_ = std::make_unique<audio::SpscRing<float>>(ring_samples);
        rate_ = rate;
        channels_ = channels;
        closing_ = false;
        set_name(std::move(name));
    }

    // Called with device_mu_ held.
    void start_locked() {
        const ma_result result = ma_device_start(device_.get());
        if (result != MA_SUCCESS) {
            throw std::runtime_error(std::string("cannot start the capture device: ") +
                                     ma_result_description(result));
        }
    }

    // Stops and closes the device, if one is open. Called with device_mu_ held.
    void close_locked() {
        if (!device_) {
            return;
        }
        closing_ = true;
        ma_device_stop(device_.get());
        ma_device_uninit(device_.get());
        device_.reset();
    }

    void set_name(std::string name) {
        std::lock_guard<std::mutex> lock(name_mu_);
        name_ = std::move(name);
    }

    // The data callback runs on the audio thread. It copies whole frames into the ring, and never
    // locks or allocates.
    static void on_data(ma_device* device, void* /*output*/, const void* input,
                        ma_uint32 frame_count) {
        auto* self = static_cast<Impl*>(device->pUserData);
        if (input == nullptr || self->suspended_.load()) {
            return;
        }
        audio::SpscRing<float>* ring = self->ring_.get();
        const std::size_t channels = self->channels_.load();
        if (ring == nullptr || channels == 0) {
            return;
        }
        const std::size_t free_samples = ring->capacity() - ring->size();
        const std::size_t fit = std::min<std::size_t>(frame_count, free_samples / channels);
        ring->push(std::span<const float>(static_cast<const float*>(input), fit * channels));
        self->dropped_ += frame_count - fit;
    }

    // Runs on miniaudio's notification thread, so it only sets flags. A stop the feed did not ask
    // for is a loss. A reroute keeps running, unless the new device has another format.
    static void on_notification(const ma_device_notification* notification) {
        auto* self = static_cast<Impl*>(notification->pDevice->pUserData);
        if (self->closing_.load()) {
            return;
        }
        const ma_device* device = notification->pDevice;
        const bool stopped = notification->type == ma_device_notification_type_stopped;
        const bool format_changed =
            notification->type == ma_device_notification_type_rerouted &&
            (device->sampleRate != self->rate_.load() ||
             device->capture.channels != self->channels_.load());
        if (stopped || format_changed) {
            self->suspended_ = true;
            self->reopen_needed_ = true;
        }
    }

    enum class Reopen { reopened, stopped, lost };

    // Brings a lost device back. A chosen device does not come back. A default device is reopened
    // until one opens or the capture is stopped, because the system can be without any device for
    // a while, such as when the only headset is unplugged.
    Reopen reopen() {
        if (!chosen_id_.empty()) {
            std::lock_guard<std::mutex> lock(device_mu_);
            close_locked();
            return Reopen::lost;
        }
        for (int attempt = 0;; ++attempt) {
            if (attempt > 0) {
                waiting_ = attempt >= kQuickReopenAttempts;
                std::unique_lock<std::mutex> lock(wake_mu_);
                wake_cv_.wait_for(lock,
                                  waiting_ ? kSlowReopenInterval : kQuickReopenInterval,
                                  [this] { return stop_requested_.load(); });
            }
            std::lock_guard<std::mutex> lock(device_mu_);
            if (stop_requested_.load()) {
                waiting_ = false;
                return Reopen::stopped;
            }
            close_locked();
            try {
                open_device();
                start_locked();
            } catch (const std::exception&) {
                close_locked();
                continue;
            }
            suspended_ = false;
            reopen_needed_ = false;
            waiting_ = false;
            return Reopen::reopened;
        }
    }

    // Pushes everything the ring holds into the session, a piece at a time. Feeder thread only.
    void drain() {
        const std::uint32_t rate = rate_.load();
        const std::uint32_t channels = channels_.load();
        const std::size_t piece_frames = std::max<std::size_t>(
            1, static_cast<std::size_t>(static_cast<double>(rate) * kPushSeconds));
        buffer_.resize(piece_frames * channels);
        audio::SpscRing<float>* ring = ring_.get();
        for (std::size_t got = ring->pop(buffer_); got > 0; got = ring->pop(buffer_)) {
            const std::span<float> samples(buffer_.data(), got);
            float peak = peak_.load();
            for (float& sample : samples) {
                // A device that sends a non-finite sample would make the session refuse the push.
                if (!std::isfinite(sample)) {
                    sample = 0.0f;
                }
                peak = std::max(peak, std::fabs(sample));
            }
            peak_ = peak;
            session_.push(samples, static_cast<std::int32_t>(rate),
                          static_cast<std::int32_t>(channels));
            delivered_ += got / channels;
        }
    }

    // The feeder loop. Each pass takes the audio that is already in the ring. It exits after the
    // pass that follows finish_, so no captured audio is left behind.
    void run_feeder() {
        while (true) {
            const bool finishing = finish_.load();
            if (reopen_needed_.load()) {
                // Audio the old device captured goes first, so the format change is not mixed in.
                drain();
                const Reopen outcome = reopen();
                if (outcome == Reopen::stopped) {
                    return;
                }
                if (outcome == Reopen::lost) {
                    lost_ = true;
                    session_.report_error(kDeviceLost);
                    return;
                }
                continue;
            }
            drain();
            if (finishing) {
                return;
            }
            std::unique_lock<std::mutex> lock(wake_mu_);
            wake_cv_.wait_for(lock, kDrainInterval, [this] { return finish_.load(); });
        }
    }

    // The feeder must not let an exception escape its thread. A failure ends the capture, and the
    // session is told why.
    void run_feeder_guarded() {
        try {
            run_feeder();
        } catch (const std::exception& e) {
            suspended_ = true;
            lost_ = true;
            session_.report_error(std::string("capture failed: ") + e.what());
        } catch (...) {
            suspended_ = true;
            lost_ = true;
            session_.report_error("capture failed");
        }
    }

    const Source source_;
    const std::string chosen_id_;  // empty: the default device, followed through changes
    Session& session_;
    AudioContext context_;  // declared before the device, so it outlives it

    std::mutex device_mu_;
    std::unique_ptr<ma_device> device_;              // device_mu_
    std::unique_ptr<audio::SpscRing<float>> ring_;   // see above
    std::atomic<std::uint32_t> rate_{0};
    std::atomic<std::uint32_t> channels_{0};
    // The device is being closed on purpose, so its notifications are not a loss.
    std::atomic<bool> closing_{false};
    // The audio thread drops its input. Set while the device is lost or being reopened.
    std::atomic<bool> suspended_{false};
    std::atomic<bool> reopen_needed_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> finish_{false};
    std::atomic<bool> lost_{false};
    // A default device was lost, and no device has opened for a second or more.
    std::atomic<bool> waiting_{false};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> delivered_{0};
    std::atomic<float> peak_{0.0f};

    mutable std::mutex name_mu_;
    std::string name_;  // name_mu_

    std::mutex wake_mu_;
    std::condition_variable wake_cv_;

    std::thread feeder_;
    std::vector<float> buffer_;  // feeder thread only
    bool started_ = false;       // owner thread only
    bool stopped_ = false;       // owner thread only
};

CaptureFeed::CaptureFeed(Source source, const std::string& device_id, Session& session)
    : impl_(std::make_unique<Impl>(source, device_id, session)) {}

CaptureFeed::~CaptureFeed() = default;

void CaptureFeed::start() { impl_->start(); }

void CaptureFeed::stop() { impl_->stop(); }

std::uint32_t CaptureFeed::rate() const { return impl_->rate_.load(); }

std::uint32_t CaptureFeed::channels() const { return impl_->channels_.load(); }

std::string CaptureFeed::device_name() const {
    std::lock_guard<std::mutex> lock(impl_->name_mu_);
    return impl_->name_;
}

bool CaptureFeed::ended() const { return impl_->lost_.load(); }

bool CaptureFeed::waiting() const { return impl_->waiting_.load(); }

std::uint64_t CaptureFeed::frames_delivered() const { return impl_->delivered_.load(); }

std::uint64_t CaptureFeed::dropped_frames() const { return impl_->dropped_.load(); }

float CaptureFeed::peak() const { return impl_->peak_.load(); }

}  // namespace echoir::capture
