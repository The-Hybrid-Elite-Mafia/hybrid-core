// =============================================================================
//  platform/asio_output.cpp — ASIO output + driver list. See asio_output.hpp.
//  The output half is compiled only with the Steinberg ASIO SDK (GPLv3 option, see LICENSE.txt).
// =============================================================================
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include "hc/platform/asio_output.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace hc {

namespace {
std::wstring readRegString(HKEY root, const wchar_t* subKey, const wchar_t* value) {
    DWORD bytes = 0;
    if (RegGetValueW(root, subKey, value, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t)) return {};
    std::wstring s(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, subKey, value, RRF_RT_REG_SZ, nullptr, s.data(), &bytes) != ERROR_SUCCESS) return {};
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
}
} // namespace

// ASIO drivers register themselves under HKLM\SOFTWARE\ASIO\<name> { CLSID, Description }.
// Only the 64-bit view is read: a 32-bit driver cannot be loaded into this 64-bit program.
std::vector<AsioDriverEntry> enumerateAsioDrivers() {
    std::vector<AsioDriverEntry> out;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS) return out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD len = static_cast<DWORD>(std::size(name));
        const LSTATUS st = RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st != ERROR_SUCCESS) continue;
        AsioDriverEntry e;
        e.name = name;
        e.description = readRegString(root, name, L"Description");
        if (!readRegString(root, name, L"CLSID").empty()) out.push_back(std::move(e));
    }
    RegCloseKey(root);
    return out;
}

} // namespace hc

#if defined(HC_HAS_ASIO_SDK)

#include "hc/core/aligned_buffer.hpp"
#include "hc/dsp/simd.hpp"
#include "hc/platform/thread_priority.hpp"
#include <objbase.h>
#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"

// Provided by the Steinberg SDK (host/asiodrivers.cpp).
extern AsioDrivers* asioDrivers;
bool loadAsioDriver(char* name);

namespace hc {

struct AsioOutput::Impl {
    AsioOutput* owner = nullptr;
    std::uint32_t frames = 0;
    std::uint32_t numOut = 0;
    bool driverLoaded = false, buffersCreated = false, postOutput = false;
    ASIOBufferInfo infos[2]{};
    ASIOSampleType types[2]{ASIOSTInt32LSB, ASIOSTInt32LSB};
    AlignedBuffer<float> L, R;
    RenderCallback cb = nullptr;
    void* user = nullptr;
    std::atomic<bool> running{false};

    static bool supported(ASIOSampleType t) noexcept {
        return t == ASIOSTInt16LSB || t == ASIOSTInt24LSB || t == ASIOSTInt32LSB || t == ASIOSTFloat32LSB || t == ASIOSTFloat64LSB;
    }
    static inline float clip(float v) noexcept { return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v); }

    static void fromFloat(ASIOSampleType t, const float* src, void* dst, std::uint32_t n) noexcept {
        switch (t) {
        case ASIOSTInt16LSB: {
            auto* d = static_cast<std::int16_t*>(dst);
            for (std::uint32_t i = 0; i < n; ++i) d[i] = static_cast<std::int16_t>(std::lrintf(clip(src[i]) * 32767.0f));
            break;
        }
        case ASIOSTInt24LSB: {
            auto* d = static_cast<std::uint8_t*>(dst);
            for (std::uint32_t i = 0; i < n; ++i) {
                const std::int32_t v = static_cast<std::int32_t>(std::lrintf(clip(src[i]) * 8388607.0f));
                d[3 * i] = static_cast<std::uint8_t>(v); d[3 * i + 1] = static_cast<std::uint8_t>(v >> 8); d[3 * i + 2] = static_cast<std::uint8_t>(v >> 16);
            }
            break;
        }
        case ASIOSTInt32LSB: {
            auto* d = static_cast<std::int32_t*>(dst);
            for (std::uint32_t i = 0; i < n; ++i) d[i] = static_cast<std::int32_t>(std::llrintf(clip(src[i]) * 2147483647.0f));
            break;
        }
        case ASIOSTFloat32LSB: std::memcpy(dst, src, static_cast<std::size_t>(n) * sizeof(float)); break;
        case ASIOSTFloat64LSB: {
            auto* d = static_cast<double*>(dst);
            for (std::uint32_t i = 0; i < n; ++i) d[i] = static_cast<double>(src[i]);
            break;
        }
        default: break;
        }
    }

    // Driver callback thread.
    void process(long index) noexcept {
        static thread_local platform::RealtimeThreadScope scope;   // first call registers this driver thread with MMCSS
        simd::ScopedFlushDenormals ftz;
        (void)scope;
        if (!cb) return;
        cb(user, L.data(), R.data(), frames);
        fromFloat(types[0], L.data(), infos[0].buffers[index], frames);
        fromFloat(types[1], R.data(), infos[1].buffers[index], frames);
        if (postOutput) ASIOOutputReady();
    }
};

namespace {
std::atomic<AsioOutput::Impl*> gActive{nullptr};

void onBufferSwitch(long index, ASIOBool) {
    if (auto* p = gActive.load(std::memory_order_acquire); p && p->running.load(std::memory_order_relaxed)) p->process(index);
}
ASIOTime* onBufferSwitchTimeInfo(ASIOTime* t, long index, ASIOBool) {
    if (auto* p = gActive.load(std::memory_order_acquire); p && p->running.load(std::memory_order_relaxed)) p->process(index);
    return t;
}
void onSampleRateChanged(ASIOSampleRate) {
    if (auto* p = gActive.load(std::memory_order_acquire)) p->owner->resetRequested_.store(true, std::memory_order_release);
}
long onAsioMessage(long selector, long value, void*, double*) {
    auto* p = gActive.load(std::memory_order_acquire);
    switch (selector) {
    case kAsioSelectorSupported:
        return (value == kAsioEngineVersion || value == kAsioResetRequest || value == kAsioResyncRequest ||
                value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo || value == kAsioOverload) ? 1L : 0L;
    case kAsioEngineVersion: return 2L;
    case kAsioResetRequest:
    case kAsioResyncRequest: if (p) p->owner->resetRequested_.store(true, std::memory_order_release); return 1L;
    case kAsioLatenciesChanged: return 1L;
    case kAsioOverload: if (p) p->owner->glitches_.fetch_add(1, std::memory_order_relaxed); return 1L;
    case kAsioSupportsTimeInfo: return 1L;
    default: return 0L;
    }
}
ASIOCallbacks gCallbacks{&onBufferSwitch, &onSampleRateChanged, &onAsioMessage, &onBufferSwitchTimeInfo};
} // namespace

AsioOutput::AsioOutput() : impl_(std::make_unique<Impl>()) { impl_->owner = this; }
AsioOutput::~AsioOutput() { close(); }

std::string AsioOutput::open(const Config& cfg) {
    close();
    Impl& s = *impl_;
    Impl* expected = nullptr;
    if (!gActive.compare_exchange_strong(expected, &s, std::memory_order_acq_rel)) return "another ASIO stream is already open";
    auto fail = [this](const std::string& what) { close(); return what; };
    auto err = [](const char* stage, long code) { return std::string(stage) + " failed (ASIO error " + std::to_string(code) + ")"; };

    if (cfg.driverName.empty()) return fail("no ASIO driver chosen");
    const int bytes = WideCharToMultiByte(CP_ACP, 0, cfg.driverName.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return fail("bad driver name");
    std::vector<char> name(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_ACP, 0, cfg.driverName.c_str(), -1, name.data(), bytes, nullptr, nullptr);
    if (!loadAsioDriver(name.data())) return fail("the ASIO driver could not be loaded");
    s.driverLoaded = true;

    ASIODriverInfo di{};
    di.asioVersion = 2;
    di.sysRef = GetDesktopWindow();
    ASIOError e = ASIOInit(&di);
    if (e != ASE_OK) return fail(err("ASIOInit", e));

    ASIOSampleRate cur = 0;
    if (ASIOGetSampleRate(&cur) != ASE_OK || cur != cfg.sampleRate) {
        if ((e = ASIOCanSampleRate(cfg.sampleRate)) != ASE_OK) return fail("the driver does not support " + std::to_string(static_cast<int>(cfg.sampleRate)) + " Hz");
        if ((e = ASIOSetSampleRate(cfg.sampleRate)) != ASE_OK) return fail(err("ASIOSetSampleRate", e));
    }

    long hwIn = 0, hwOut = 0;
    if ((e = ASIOGetChannels(&hwIn, &hwOut)) != ASE_OK) return fail(err("ASIOGetChannels", e));
    if (hwOut < 2) return fail("the driver has fewer than 2 output channels");

    long minSize = 0, maxSize = 0, preferred = 0, gran = 0;
    if ((e = ASIOGetBufferSize(&minSize, &maxSize, &preferred, &gran)) != ASE_OK) return fail(err("ASIOGetBufferSize", e));
    long size = preferred;
    if (cfg.bufferFrames != 0) {
        const long req = static_cast<long>(cfg.bufferFrames);
        const bool inRange = req >= minSize && req <= maxSize;
        const bool fits = gran == -1 ? ((req & (req - 1)) == 0) : (gran <= 0 || ((req - minSize) % gran) == 0);
        if (inRange && fits) size = req;
    }
    if (size <= 0 || size > static_cast<long>(cfg.maxBlock)) return fail("the driver's buffer size is larger than this program supports");

    for (int c = 0; c < 2; ++c) { s.infos[c].isInput = ASIOFalse; s.infos[c].channelNum = c; }
    if ((e = ASIOCreateBuffers(s.infos, 2, size, &gCallbacks)) != ASE_OK) return fail(err("ASIOCreateBuffers", e));
    s.buffersCreated = true;
    for (int c = 0; c < 2; ++c) {
        ASIOChannelInfo ci{};
        ci.channel = s.infos[c].channelNum; ci.isInput = ASIOFalse;
        if ((e = ASIOGetChannelInfo(&ci)) != ASE_OK) return fail(err("ASIOGetChannelInfo", e));
        if (!Impl::supported(ci.type)) return fail("the driver uses a sample format this program does not support (type " + std::to_string(ci.type) + ")");
        s.types[c] = ci.type;
    }
    s.frames = static_cast<std::uint32_t>(size);
    s.numOut = 2;
    s.postOutput = (ASIOOutputReady() == ASE_OK);
    s.L.resize(s.frames); s.R.resize(s.frames);

    long inLat = 0, outLat = 0;
    ASIOGetLatencies(&inLat, &outLat);
    info_ = {};
    info_.sampleRate = cfg.sampleRate;
    info_.periodFrames = s.frames;
    info_.bufferFrames = s.frames;
    info_.channels = 2;
    info_.outputLatencyMs = 1000.0 * static_cast<double>(outLat) / cfg.sampleRate;
    char drv[64] = {};
    ASIOGetDriverName(drv);
    info_.description = "ASIO: " + std::string(drv[0] ? drv : name.data()) + ", " + std::to_string(static_cast<int>(cfg.sampleRate)) + " Hz, " +
                        std::to_string(s.frames) + " frames";
    resetRequested_.store(false);
    glitches_.store(0);
    return {};
}

std::string AsioOutput::start(RenderCallback cb, void* user) {
    Impl& s = *impl_;
    if (!s.buffersCreated) return "ASIO is not open";
    if (s.running.load(std::memory_order_acquire)) return {};
    s.cb = cb; s.user = user;
    s.running.store(true, std::memory_order_release);
    if (const ASIOError e = ASIOStart(); e != ASE_OK) {
        s.running.store(false, std::memory_order_release);
        return "ASIOStart failed (ASIO error " + std::to_string(e) + ")";
    }
    return {};
}

void AsioOutput::stop() {
    Impl& s = *impl_;
    if (s.running.exchange(false, std::memory_order_acq_rel)) ASIOStop();
}

void AsioOutput::close() {
    stop();
    Impl& s = *impl_;
    if (s.buffersCreated) { ASIODisposeBuffers(); s.buffersCreated = false; }
    if (s.driverLoaded) { ASIOExit(); s.driverLoaded = false; }
    Impl* self = &s;
    gActive.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
    s.cb = nullptr; s.user = nullptr; s.frames = 0;
}

} // namespace hc

#endif // HC_HAS_ASIO_SDK
