// =============================================================================
//  apps/hem_stem_to_midi/audio_load.hpp — open any song/stem file.
//  .wav  -> the Hybrid Core WAV reader (all OSes)
//  other -> Windows Media Foundation (MP3, M4A/AAC, FLAC, WMA...) — Windows only
// =============================================================================
#pragma once
#include "hc/io/wav.hpp"
#include <filesystem>
#include <string>

namespace hem {

#if defined(_WIN32)
std::string decodeWithMediaFoundation(const std::filesystem::path& file, hc::wav::Audio& out);   // decode_mf.cpp
#endif

inline bool isAudioFile(const std::filesystem::path& p) {
    std::wstring e = p.extension().wstring();
    for (auto& c : e) c = static_cast<wchar_t>(towlower(c));
    return e == L".wav" || e == L".mp3" || e == L".m4a" || e == L".aac" || e == L".flac" || e == L".wma" || e == L".aif" || e == L".aiff";
}

inline std::string loadAudio(const std::filesystem::path& file, hc::wav::Audio& out) {
    std::wstring e = file.extension().wstring();
    for (auto& c : e) c = static_cast<wchar_t>(towlower(c));
    if (e == L".wav") {
        auto err = hc::wav::read(file, out);
#if defined(_WIN32)
        if (!err.empty()) { hc::wav::Audio b; if (decodeWithMediaFoundation(file, b).empty()) { out = std::move(b); return {}; } }   // odd WAV kinds
#endif
        return err;
    }
#if defined(_WIN32)
    return decodeWithMediaFoundation(file, out);
#else
    return "only .wav files can be opened on this system";
#endif
}

} // namespace hem
