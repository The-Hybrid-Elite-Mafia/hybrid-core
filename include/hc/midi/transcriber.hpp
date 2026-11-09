// =============================================================================
//  midi/transcriber.hpp — HEM STEM-TO-MIDI: one song or up to 13 stems in, MIDI files out.
//
//  convert() does the whole job:
//    1. picks each stem's part type (from the menu, or from its file name on Auto),
//    2. finds one shared tempo from all stems together (or uses the BPM typed in),
//    3. runs the right note finder on each stem (stems run in parallel),
//    4. writes  <song> - ALL STEMS.mid  (one track per stem) and one  <song> - <stem>.mid  each,
//    5. reads every written file back and checks it (note count, tempo, quantize grid).
// =============================================================================
#pragma once
#include "hc/dsp/resampler.hpp"
#include "hc/io/wav.hpp"
#include "hc/midi/midi_config.hpp"
#include "hc/midi/smf.hpp"
#include "hc/midi/trackers.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace hc::midi {

inline constexpr std::size_t kMaxStems = 13;
inline constexpr double kPitchRate = 16000.0;   // pitched parts are analysed at 16 kHz (harmonics to 8 kHz)

struct Settings {
    int sensitivity = kSensitivityDefault;       // 0..100
    int shortestMs = kShortestNoteDefaultMs;     // kShortestNoteMinMs..kShortestNoteMaxMs
    int quantizeDiv = 0;                         // 0 off, 4, 8, 16, 32
    double bpm = 0;                              // 0 = find it; else use this
};

struct StemIn {
    std::string name;          // shown name, usually the file name without extension
    wav::Audio audio;
    Mode mode = Mode::Auto;
};

struct StemOut {
    std::string name;
    Mode mode = Mode::Auto;    // the type actually used
    std::size_t notes = 0;
    std::filesystem::path file;
    std::string error;
};

struct ConvertResult {
    double bpm = kFallbackBpm, beatOffsetSec = 0, tempoConfidence = 0;
    bool bpmFound = false, bpmTyped = false;
    std::vector<StemOut> stems;
    std::filesystem::path combinedFile;
    std::vector<std::string> checks;   // "PASS ..." / "FAIL ..." lines from reading the files back
    bool allChecksPassed = true;
    std::string error;
};

inline std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }

// Part type + GM program from a file name (Auto).
inline std::pair<Mode, int> modeFromName(const std::string& name) {
    const std::string n = lower(name);
    for (const auto& r : kNameRules) if (n.find(r.word) != std::string::npos) return {r.mode, r.gmProgram};
    return {kUnknownNameMode, kUnknownNameProgram};
}

inline int programFor(Mode m, const std::string& name) {
    const auto [mm, prog] = modeFromName(name);
    if (mm == m || (m == Mode::Chords && mm != Mode::Drums)) return prog;
    switch (m) {
        case Mode::Bass: return 33;   case Mode::Vocal: return 53;   case Mode::Lead: return 80;   // ASSUMED GM picks
        default: return 0;
    }
}

// Mono mix. Audio stored as mono (ch[1] empty, to save memory with 13 long stems) is used as is.
inline std::vector<float> mono(const wav::Audio& a) {
    if (a.ch[1].size() != a.ch[0].size()) return a.ch[0];
    std::vector<float> m(a.frames());
    for (std::size_t i = 0; i < m.size(); ++i) m[i] = 0.5f * (a.ch[0][i] + a.ch[1][i]);
    return m;
}
inline void toMonoInPlace(wav::Audio& a) {
    if (a.ch[1].size() == a.ch[0].size()) for (std::size_t i = 0; i < a.ch[0].size(); ++i) a.ch[0][i] = 0.5f * (a.ch[0][i] + a.ch[1][i]);
    a.ch[1].clear(); a.ch[1].shrink_to_fit();
}

inline std::string toPitchRate(const std::vector<float>& m, double sr, std::vector<float>& out) {
    if (std::fabs(sr - kPitchRate) < 0.5) { out = m; return {}; }
    return Resampler::convert(m, sr, kPitchRate, out);
}

// Notes for one stem, given its resolved mode. FullMix returns chords in `a` and drums in `b`.
inline void transcribeStem(const wav::Audio& audio, Mode mode, const Settings& s, std::vector<Note>& a, std::vector<Note>& b, std::string& err) {
    a.clear(); b.clear();
    const auto m = mono(audio);
    if (m.empty()) { err = "empty file"; return; }
    if (mode == Mode::Drums || mode == Mode::FullMix) b = drumTranscribe(m, audio.sampleRate, {s.sensitivity});
    if (mode == Mode::Drums) { a.swap(b); return; }
    std::vector<float> p;
    if (auto e = toPitchRate(m, audio.sampleRate, p); !e.empty()) { err = e; return; }
    switch (mode) {
        case Mode::Bass:  a = monoTranscribe(p, kPitchRate, {kBassLow, kBassHigh, s.sensitivity, s.shortestMs}); break;
        case Mode::Vocal: a = monoTranscribe(p, kPitchRate, {kVocalLow, kVocalHigh, s.sensitivity, s.shortestMs}); break;
        case Mode::Lead:  a = monoTranscribe(p, kPitchRate, {kLeadLow, kLeadHigh, s.sensitivity, s.shortestMs}); break;
        case Mode::Chords: case Mode::FullMix:
            a = polyTranscribe(p, kPitchRate, {kChordLow, kChordHigh, s.sensitivity, s.shortestMs}); break;
        default: break;
    }
}

inline std::string safeFileName(std::string s) {
    for (auto& c : s) if (std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 32) c = '_';
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s.empty() ? std::string("stem") : s;
}

inline std::filesystem::path u8path(const std::string& s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

using Progress = std::function<void(int done, int total, const std::string& what)>;

inline ConvertResult convert(std::vector<StemIn>& stems, const Settings& s, const std::filesystem::path& outDir, const std::string& songName, const Progress& progress = {}) {
    ConvertResult R;
    if (stems.empty()) { R.error = "no audio loaded"; return R; }
    if (stems.size() > kMaxStems) { R.error = "more than 13 stems"; return R; }
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) { R.error = "cannot create output folder: " + ec.message(); return R; }

    // 1. resolve modes
    std::vector<Mode> modes(stems.size());
    for (std::size_t i = 0; i < stems.size(); ++i) {
        Mode m = stems[i].mode;
        if (m == Mode::Auto) m = stems.size() == 1 && modeFromName(stems[i].name).first == kUnknownNameMode ? Mode::FullMix : modeFromName(stems[i].name).first;
        modes[i] = m;
    }
    const int total = int(stems.size()) + 2;
    int done = 0;
    if (progress) progress(done, total, "finding the tempo");

    // 2. tempo from every stem's onset curve together
    R.bpmTyped = s.bpm > 0;
    {
        std::vector<double> env;
        for (std::size_t i = 0; i < stems.size(); ++i) {
            if (modes[i] == Mode::Skip || stems[i].audio.frames() == 0) continue;
            const auto c = onsetCurve(mono(stems[i].audio), stems[i].audio.sampleRate);
            if (c.size() > env.size()) env.resize(c.size(), 0.0);
            for (std::size_t k = 0; k < c.size(); ++k) env[k] += c[k];
        }
        const auto t = estimateTempo(env);
        R.bpmFound = t.found; R.tempoConfidence = t.confidence; R.beatOffsetSec = t.beatOffsetSec;
        R.bpm = R.bpmTyped ? s.bpm : t.bpm;
        if (R.bpmTyped && t.found) {   // keep the found beat-1 position, but on the typed grid
            const double per = 60.0 / s.bpm;
            R.beatOffsetSec = std::fmod(t.beatOffsetSec, per);
        }
    }
    if (progress) progress(++done, total, "listening to the stems");

    // 3. notes, stems in parallel
    std::vector<std::vector<Note>> A(stems.size()), B(stems.size());
    std::vector<std::string> errs(stems.size());
    std::atomic<int> next{0}, finished{0};
    const unsigned nThreads = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), unsigned(stems.size())));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nThreads; ++t)
        pool.emplace_back([&] {
            for (int i; (i = next.fetch_add(1)) < int(stems.size());) {
                if (modes[std::size_t(i)] != Mode::Skip) transcribeStem(stems[std::size_t(i)].audio, modes[std::size_t(i)], s, A[std::size_t(i)], B[std::size_t(i)], errs[std::size_t(i)]);
                finished.fetch_add(1);
            }
        });
    for (auto& t : pool) t.join();
    done += int(stems.size());
    if (progress) progress(done, total, "writing MIDI files");

    // 4. tracks and files
    WriteOptions wo; wo.bpm = R.bpm; wo.beatOffsetSec = R.beatOffsetSec; wo.quantizeDiv = s.quantizeDiv;
    std::vector<Track> all;
    static constexpr int kMelodicChannels[15] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15};
    int nextCh = 0;
    auto channel = [&]() { return kMelodicChannels[(nextCh++) % 15]; };
    const std::string base = safeFileName(songName);
    for (std::size_t i = 0; i < stems.size(); ++i) {
        StemOut so; so.name = stems[i].name; so.mode = modes[i]; so.error = errs[i];
        if (modes[i] == Mode::Skip) { R.stems.push_back(so); continue; }
        std::vector<Track> mine;
        if (modes[i] == Mode::Drums) mine.push_back({stems[i].name, kDrumChannel, 0, A[i]});
        else if (modes[i] == Mode::FullMix) {
            mine.push_back({stems[i].name + " notes", channel(), 0, A[i]});
            mine.push_back({stems[i].name + " drums", kDrumChannel, 0, B[i]});
        } else mine.push_back({stems[i].name, channel(), programFor(modes[i], stems[i].name), A[i]});
        for (auto& t : mine) so.notes += t.notes.size();
        if (so.error.empty()) {
            so.file = outDir / u8path(base + " - " + safeFileName(stems[i].name) + ".mid");
            if (auto e = write(so.file, mine, wo, stems[i].name); !e.empty()) so.error = e;
        }
        for (auto& t : mine) all.push_back(std::move(t));
        R.stems.push_back(so);
    }
    R.combinedFile = outDir / u8path(base + " - ALL STEMS.mid");
    if (auto e = write(R.combinedFile, all, wo, songName); !e.empty()) R.error = "combined file: " + e;

    // 5. read everything back and check it
    auto check = [&](bool ok, const std::string& what) { R.checks.push_back(std::string(ok ? "PASS " : "FAIL ") + what); if (!ok) R.allChecksPassed = false; };
    const std::int64_t g = gridTicks(s.quantizeDiv), offT = secondsToTicks(R.beatOffsetSec, R.bpm);
    auto verify = [&](const std::filesystem::path& f, std::size_t expectTracks, std::size_t expectNotes, const std::string& label) {
        const Parsed P = parseFile(f);
        if (!P.error.empty()) { check(false, label + ": " + P.error); return; }
        std::size_t n = 0; bool onGrid = true;
        for (std::size_t t = 1; t < P.tracks.size(); ++t) for (const auto& nt : P.tracks[t].notes) {
            ++n; if (g > 0 && ((nt.on - offT) % g + g) % g != 0) onGrid = false;
        }
        check(P.format == 1 && P.ppq == kPpq, label + ": type-1 MIDI file, 480 ticks per beat");
        check(P.tracks.size() == expectTracks + 1, label + ": " + std::to_string(P.tracks.size() - 1) + " track(s)");
        check(std::fabs(P.bpm - R.bpm) < 0.01, label + ": tempo in file " + std::to_string(P.bpm).substr(0, 6) + " BPM");
        check(n == expectNotes, label + ": " + std::to_string(n) + " notes read back, " + std::to_string(expectNotes) + " written");
        if (g > 0) check(onGrid, label + ": every note starts on the 1/" + std::to_string(s.quantizeDiv) + " grid");
    };
    // "written" = what the writer must put in the file after quantize (it joins notes that land on the same tick)
    std::size_t totalNotes = 0;
    std::vector<std::size_t> perTrack;
    for (const auto& t : all) { perTrack.push_back(toTicks(t, wo).size()); totalNotes += perTrack.back(); }
    if (R.error.empty()) verify(R.combinedFile, all.size(), totalNotes, "ALL STEMS");
    std::size_t ti = 0;
    for (auto& so : R.stems) {
        if (so.mode == Mode::Skip) continue;
        const std::size_t nt = so.mode == Mode::FullMix ? 2 : 1;
        std::size_t expect = 0; for (std::size_t k = 0; k < nt && ti < perTrack.size(); ++k) expect += perTrack[ti++];
        so.notes = expect;
        if (!so.file.empty() && so.error.empty()) verify(so.file, nt, expect, so.name);
    }
    if (progress) progress(total, total, "done");
    return R;
}

} // namespace hc::midi
