// =============================================================================
//  apps/hem_stem_to_midi/cli.cpp — HEM STEM-TO-MIDI without the window (same engine).
//
//  hem_midi_cli [options] OUT_FOLDER FILE [FILE ...]      (1 song or up to 13 stems)
//    --bpm N            use this tempo (default: find it)
//    --quantize N       0 (off), 4, 8, 16, 32
//    --sensitivity N    0..100 (default 50)
//    --shortest MS      shortest note in ms (20..250, default 60)
//    --type I=NAME      part type for file number I (1-based): Auto Drums Bass Vocal Lead Chords Full Skip
//    --song NAME        name used for the output files
//  Prints what it did and the read-back checks. Exit code 0 = files written and all checks passed.
// =============================================================================
#include "hc/midi/transcriber.hpp"
#include "audio_load.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>

using namespace hc::midi;

#if defined(_WIN32)
#include <windows.h>
static std::filesystem::path argPath(int i) {   // real Unicode file names on Windows
    int n = 0; LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    std::filesystem::path p = (w && i < n) ? std::filesystem::path(w[i]) : std::filesystem::path();
    LocalFree(w); return p;
}
#else
static char** g_argv = nullptr;
static std::filesystem::path argPath(int i) { return std::filesystem::path(g_argv[i]); }
#endif

static std::string u8(const std::filesystem::path& p) { const auto s = p.u8string(); return std::string(reinterpret_cast<const char*>(s.data()), s.size()); }

int main(int argc, char** argv) {
#if !defined(_WIN32)
    g_argv = argv;
#endif
    Settings st; std::map<int, Mode> types; std::string song;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; ++i) {
        const std::string a = argv[i];
        if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); return 2; }
        const std::string v = argv[++i];
        if (a == "--bpm") st.bpm = std::stod(v);
        else if (a == "--quantize") st.quantizeDiv = std::stoi(v);
        else if (a == "--sensitivity") st.sensitivity = std::stoi(v);
        else if (a == "--shortest") st.shortestMs = std::stoi(v);
        else if (a == "--song") song = v;
        else if (a == "--type") {
            const auto eq = v.find('='); if (eq == std::string::npos) { std::fprintf(stderr, "--type needs I=NAME\n"); return 2; }
            const int idx = std::stoi(v.substr(0, eq)); const std::string name = lower(v.substr(eq + 1));
            Mode m = Mode::Auto; bool found = false;
            for (int k = 0; k < kModeCount; ++k) { std::string mn = lower(std::string(kModeNames[std::size_t(k)])); if (mn == name || mn.rfind(name, 0) == 0) { m = Mode(k); found = true; break; } }
            if (!found) { std::fprintf(stderr, "unknown type %s\n", name.c_str()); return 2; }
            types[idx] = m;
        } else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (st.quantizeDiv != 0 && st.quantizeDiv != 4 && st.quantizeDiv != 8 && st.quantizeDiv != 16 && st.quantizeDiv != 32) { std::fprintf(stderr, "--quantize must be 0, 4, 8, 16 or 32\n"); return 2; }
    st.sensitivity = std::clamp(st.sensitivity, 0, 100);
    st.shortestMs = std::clamp(st.shortestMs, kShortestNoteMinMs, kShortestNoteMaxMs);
    if (argc - i < 2) { std::puts("usage: hem_midi_cli [options] OUT_FOLDER FILE [FILE ...]"); return 2; }
    const auto outDir = argPath(i++);
    std::vector<StemIn> stems;
    for (int k = 1; i < argc; ++i, ++k) {
        const auto p = argPath(i);
        StemIn s; s.name = u8(p.stem()); s.mode = types.count(k) ? types[k] : Mode::Auto;
        const auto t0 = std::chrono::steady_clock::now();
        if (auto e = hem::loadAudio(p, s.audio); !e.empty()) { std::fprintf(stderr, "cannot open %s: %s\n", u8(p).c_str(), e.c_str()); return 3; }
        std::printf("loaded %-28s %7.2f s  %6.0f Hz  (%.2f s to open)\n", s.name.c_str(), double(s.audio.frames()) / s.audio.sampleRate, s.audio.sampleRate,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        stems.push_back(std::move(s));
    }
    if (stems.size() > kMaxStems) { std::fprintf(stderr, "at most 13 files\n"); return 2; }
    if (song.empty()) song = stems.size() == 1 ? stems[0].name : u8(argPath(i - 1).parent_path().filename());
    if (song.empty()) song = "HEM";
    const auto t0 = std::chrono::steady_clock::now();
    auto R = convert(stems, st, outDir, song);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!R.error.empty()) { std::fprintf(stderr, "error: %s\n", R.error.c_str()); return 4; }
    std::printf("tempo %.2f BPM (%s), beat 1 at %.3f s, quantize %s, sensitivity %d, shortest note %d ms\n", R.bpm, R.bpmTyped ? "typed" : (R.bpmFound ? "found" : "not found, fallback"),
                R.beatOffsetSec, st.quantizeDiv ? ("1/" + std::to_string(st.quantizeDiv)).c_str() : "off", st.sensitivity, st.shortestMs);
    for (const auto& s : R.stems)
        std::printf("  %-28s %-10s %5zu notes  %s%s\n", s.name.c_str(), std::string(kModeNames[std::size_t(s.mode)]).c_str(), s.notes, s.file.empty() ? "" : u8(s.file.filename()).c_str(), s.error.empty() ? "" : ("  ERROR: " + s.error).c_str());
    std::printf("  combined: %s\n", u8(R.combinedFile.filename()).c_str());
    for (const auto& c : R.checks) std::printf("  read-back %s\n", c.c_str());
    std::printf("converted in %.2f s\n", secs);
    return R.allChecksPassed ? 0 : 5;
}
