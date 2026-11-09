// =============================================================================
//  apps/hem_stem_to_midi/decode_mf.cpp — MP3 / M4A / FLAC / WMA decoding with Windows Media
//  Foundation (part of Windows: no extra library). Output: 32-bit float at the file's own rate.
// =============================================================================
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include "audio_load.hpp"
#include <cstring>
#include <mutex>

namespace hem {
namespace {
template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
struct MfStarted {
    HRESULT hr = E_FAIL;
    MfStarted() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); hr = MFStartup(MF_VERSION, MFSTARTUP_LITE); }
};
}

std::string decodeWithMediaFoundation(const std::filesystem::path& file, hc::wav::Audio& out) {
    static MfStarted mf;   // started once, kept for the life of the program
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // this thread too (harmless if already done)
    if (FAILED(mf.hr)) return "Windows Media Foundation is not available on this PC";
    const auto name = file.filename().u8string();
    const std::string shown(reinterpret_cast<const char*>(name.data()), name.size());
    IMFSourceReader* reader = nullptr; IMFMediaType* want = nullptr; IMFMediaType* got = nullptr;
    std::string err;
    if (FAILED(MFCreateSourceReaderFromURL(file.wstring().c_str(), nullptr, &reader))) return shown + ": Windows can't open this file type";
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, want))) err = shown + ": no audio Windows can decode";
    else if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &got))) err = shown + ": can't read the audio format";
    UINT32 ch = 0, sr = 0;
    if (err.empty()) { ch = MFGetAttributeUINT32(got, MF_MT_AUDIO_NUM_CHANNELS, 0); sr = MFGetAttributeUINT32(got, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0); if (!ch || !sr) err = shown + ": unknown channels or sample rate"; }
    hc::wav::Audio a; a.sampleRate = sr;
    while (err.empty()) {
        DWORD flags = 0; IMFSample* sample = nullptr;
        if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, nullptr, &sample))) { err = shown + ": decode error"; break; }
        if (sample) {
            IMFMediaBuffer* buf = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
                BYTE* p = nullptr; DWORD len = 0;
                if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
                    const std::size_t frames = len / (sizeof(float) * ch);
                    const float* f = reinterpret_cast<const float*>(p);
                    for (std::size_t i = 0; i < frames; ++i) {
                        const float l = f[i * ch], r = ch > 1 ? f[i * ch + 1] : l;
                        a.ch[0].push_back(l); a.ch[1].push_back(r);
                    }
                    buf->Unlock();
                }
                release(buf);
            }
            release(sample);
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    release(got); release(want); release(reader);
    if (err.empty() && a.frames() == 0) err = shown + ": the file has no audio";
    if (err.empty()) out = std::move(a);
    return err;
}

} // namespace hem
