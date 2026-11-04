// =============================================================================
//  platform/asio_output.hpp — ASIO render backend (Windows).
//
//  Driver names come from the registry (always available, no SDK needed). The output itself is
//  compiled only when the Steinberg ASIO SDK is present (HC_HAS_ASIO_SDK, see cmake/asio.cmake).
//  ASIO is a single-instance API: one open stream per process.
//
//  THREAD: the driver owns the callback thread. This layer only converts planar float to the
//  driver's sample type there; it allocates nothing, takes no lock and does no I/O.
//  Output only: two driver output channels (the first pair) carry the engine's stereo master.
// =============================================================================
#pragma once
#include "hc/platform/audio_device.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hc {

struct AsioDriverEntry {
    std::wstring name;         // registry key name = what the user sees
    std::wstring description;
};

// Lists the 64-bit ASIO drivers installed on this PC (HKLM\SOFTWARE\ASIO). Safe without the SDK.
std::vector<AsioDriverEntry> enumerateAsioDrivers();

#if defined(HC_HAS_ASIO_SDK)
class AsioOutput final : public IAudioOutput {
public:
    struct Config {
        std::wstring driverName;
        double sampleRate = 48000.0;
        std::uint32_t bufferFrames = 0;   // 0 = the driver's preferred size
        std::uint32_t maxBlock = 4096;    // engine block cap
    };

    AsioOutput();
    ~AsioOutput() override;
    AsioOutput(const AsioOutput&) = delete;
    AsioOutput& operator=(const AsioOutput&) = delete;

    // Loads the driver and creates its buffers. Returns "" on success. UI thread only.
    std::string open(const Config& cfg);
    std::string start(RenderCallback cb, void* user) override;
    void stop() override;
    void close();
    const DeviceInfo& info() const override { return info_; }
    std::uint64_t glitchCount() const override { return glitches_.load(std::memory_order_relaxed); }
    // True once if the driver asked the host to reset (sample-rate change, control panel, device unplugged).
    bool takeResetRequest() noexcept { return resetRequested_.exchange(false, std::memory_order_acq_rel); }

    struct Impl;   // public only so the driver's C callbacks can reach it
private:
    std::unique_ptr<Impl> impl_;
    DeviceInfo info_;
    std::atomic<std::uint64_t> glitches_{0};
    std::atomic<bool> resetRequested_{false};
};
#endif

} // namespace hc
