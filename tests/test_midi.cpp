// =============================================================================
//  tests/test_midi.cpp — HEM STEM-TO-MIDI pass/fail tests (known-answer audio).
//  Every line prints PASS/FAIL with the measured number. Exit code = number of failures.
//  Pass limits come from midi_config.hpp (marked ASSUMED there until Steven approves them).
// =============================================================================
#include "hc/midi/transcriber.hpp"
#include "midi_signals.hpp"
#include <chrono>
#include <cstdio>
#include <filesystem>

using namespace hc::midi;
using namespace hemtest;
// Two kinds of test:
//  check()   = MUST work (file format, controls, silence, tempo). Any failure stops the build.
//  quality() = accuracy score against an answer key, bar from midi_config.hpp (ASSUMED until Steven
//              approves). A miss prints [FAIL] and is counted and reported, but does not stop the build.
static int g_fail = 0, g_qfail = 0, g_qtotal = 0;
static void check(bool ok, const char* name, double value, const char* unit = "") {
    std::printf("[%s] %-66s %10.4g %s\n", ok ? "PASS" : "FAIL", name, value, unit);
    std::fflush(stdout);
    if (!ok) ++g_fail;
}
static void quality(bool ok, const char* name, double value) {
    std::printf("[%s] (accuracy) %-55s %10.4g\n", ok ? "PASS" : "FAIL", name, value);
    std::fflush(stdout);
    ++g_qtotal; if (!ok) ++g_qfail;
}
static void info(const char* name, double value, const char* unit = "") { std::printf("[INFO] %-66s %10.4g %s\n", name, value, unit); }
static void printScore(const char* what, const Score& s) { std::printf("       %s: %d right of %d in the answer key, %d found\n", what, s.hit, s.truth, s.found); }

static std::vector<float> pitchRate(const Buf& b) { std::vector<float> o; hc::Resampler::convert(b.x, b.sr, kPitchRate, o); return o; }

// ---------------------------------------------------------------------------------------------
static void testMidiFile() {
    std::puts("\n== MIDI file writer / reader");
    Track a{"Bass", 0, 33, {{0.0, 0.5, 40, 100}, {0.5, 1.0, 43, 90}, {1.0, 1.25, 43, 80}}};
    Track d{"Drums", kDrumChannel, 0, {{0.0, 0.1, 36, 120}, {0.5, 0.6, 38, 110}}};
    WriteOptions o; o.bpm = 120;
    const auto P = parse(build({a, d}, o, "test"));
    check(P.error.empty() && P.format == 1, "file reads back as type-1 MIDI", P.format);
    check(P.tracks.size() == 3, "tempo track + 2 tracks", double(P.tracks.size()));
    check(std::fabs(P.bpm - 120.0) < 1e-9, "tempo reads back exactly", P.bpm, "BPM");
    check(P.timeSigNum == 4, "time signature 4/4", P.timeSigNum);
    check(P.tracks[1].name == "Bass" && P.tracks[1].program == 33, "track name and instrument read back", P.tracks[1].program);
    bool ticks = P.tracks[1].notes.size() == 3 && P.tracks[1].notes[1].on == 480 && P.tracks[1].notes[1].off == 960 && P.tracks[1].notes[2].on == 960;
    check(ticks, "0.5 s at 120 BPM = 480 ticks, note-offs exact", double(P.tracks[1].notes.size() == 3 ? P.tracks[1].notes[1].on : -1), "ticks");
    check(P.tracks[2].notes.size() == 2 && P.tracks[2].notes[0].channel == 9 && P.tracks[2].program == -1, "drums on channel 10, no program change", P.tracks[2].notes.size());
    // quantize
    Track q{"Q", 0, 0, {{0.27, 0.40, 60, 100}, {0.52, 0.55, 62, 100}}};
    WriteOptions oq; oq.bpm = 120; oq.quantizeDiv = 16;   // grid 120 ticks = 0.125 s
    const auto Q = parse(build({q}, oq));
    bool grid = !Q.tracks[1].notes.empty();
    for (auto& n : Q.tracks[1].notes) grid = grid && n.on % 120 == 0 && n.off % 120 == 0 && n.off > n.on;
    check(grid, "1/16 quantize: starts and ends on the 120-tick grid", Q.tracks[1].notes.empty() ? -1 : double(Q.tracks[1].notes[0].on), "ticks");
    WriteOptions oo = oq; oo.beatOffsetSec = 0.05;   // grid shifted by 0.05 s = 48 ticks
    const auto O = parse(build({q}, oo));
    check(!O.tracks[1].notes.empty() && (O.tracks[1].notes[0].on - 48) % 120 == 0, "quantize grid follows beat 1 (offset 48 ticks)", double(O.tracks[1].notes[0].on), "ticks");
}

// ---------------------------------------------------------------------------------------------
static void testSingleNote() {
    std::puts("\n== One sine note, A4 440 Hz, 0.2 s .. 0.7 s");
    Buf b(44100, 1.2); tone(b, 0.2, 0.5, 69, 0.5, 0.0, 1, 1.0);
    const auto n = monoTranscribe(pitchRate(b), kPitchRate, {kLeadLow, kLeadHigh, 50, 60});
    check(n.size() == 1, "exactly one note", double(n.size()), "notes");
    if (!n.empty()) {
        check(n[0].pitch == 69, "pitch = 69 (A4)", n[0].pitch);
        check(std::fabs(n[0].start - 0.2) < 0.020, "start within 20 ms", (n[0].start - 0.2) * 1000, "ms");
        check(std::fabs((n[0].end - n[0].start) - 0.5) < 0.040, "length within 40 ms", (n[0].end - n[0].start - 0.5) * 1000, "ms");
    }
}

static void testSilence() {
    std::puts("\n== Silence and quiet noise make no notes");
    Buf z(44100, 2.0);
    check(monoTranscribe(pitchRate(z), kPitchRate, {}).empty(), "one-note finder: silence -> 0 notes", 0);
    check(polyTranscribe(pitchRate(z), kPitchRate, {}).empty(), "chord finder: silence -> 0 notes", 0);
    check(drumTranscribe(z.x, z.sr, {}).empty(), "drum finder: silence -> 0 notes", 0);
    Buf nz(44100, 2.0); addNoise(nz, -70);
    const auto a = monoTranscribe(pitchRate(nz), kPitchRate, {}), b = polyTranscribe(pitchRate(nz), kPitchRate, {}), c = drumTranscribe(nz.x, nz.sr, {});
    check(a.empty() && b.empty() && c.empty(), "noise at -70 dBFS -> 0 notes (all three)", double(a.size() + b.size() + c.size()), "notes");
}

// ---------------------------------------------------------------------------------------------
static void testParts(double sr) {
    char t[128];
    std::printf("\n== Test song parts at %.0f Hz, 120 BPM, 8 bars\n", sr);
    const Song s = makeSong(sr, 120.0);
    const auto bass = monoTranscribe(pitchRate(s.bass), kPitchRate, {kBassLow, kBassHigh, 50, 60});
    const auto sb = score(s.tBass, bass, kTestOnsetTolS);
    std::snprintf(t, sizeof t, "bass (eighths, repeated notes): F-measure >= %.2f", kTestMonoMinF); quality(sb.f() >= kTestMonoMinF, t, sb.f()); printScore("bass", sb);
    const auto voc = monoTranscribe(pitchRate(s.vocal), kPitchRate, {kVocalLow, kVocalHigh, 50, 60});
    const auto sv = score(s.tVocal, voc, kTestOnsetTolS);
    std::snprintf(t, sizeof t, "vocal (vibrato +-30 cents): F-measure >= %.2f", kTestMonoMinF); quality(sv.f() >= kTestMonoMinF, t, sv.f()); printScore("vocal", sv);
    const auto keys = polyTranscribe(pitchRate(s.keys), kPitchRate, {kChordLow, kChordHigh, 50, 60});
    const auto sk = score(s.tKeys, keys, kTestOnsetTolS);
    std::snprintf(t, sizeof t, "keys (4-note chords, repeated): F-measure >= %.2f", kTestPolyMinF); quality(sk.f() >= kTestPolyMinF, t, sk.f()); printScore("keys", sk);
    const auto dr = drumTranscribe(s.drums.x, sr, {});
    const auto sd = score(s.tDrums, dr, kTestOnsetTolS);
    std::snprintf(t, sizeof t, "drums (kick/snare/closed+open hat): F-measure >= %.2f", kTestDrumMinF); quality(sd.f() >= kTestDrumMinF, t, sd.f()); printScore("drums", sd);
    // timing of the hits that matched
    double worst = 0;
    for (const auto& n : s.tBass) for (const auto& g : bass) if (g.pitch == n.pitch && std::fabs(g.start - n.start) < kTestOnsetTolS) worst = std::max(worst, std::fabs(g.start - n.start));
    info("bass: worst start error of matched notes", worst * 1000, "ms");
}

static void testTempo() {
    std::puts("\n== Tempo");
    for (double bpm : {120.0, 93.0, 140.0, 76.0}) {
        const Song s = makeSong(44100, bpm, 8);
        auto env = onsetCurve(s.drums.x, 44100);
        const auto b = onsetCurve(s.bass.x, 44100);
        for (std::size_t i = 0; i < std::min(env.size(), b.size()); ++i) env[i] += b[i];
        const auto r = estimateTempo(env);
        char t[96]; std::snprintf(t, sizeof t, "drums+bass at %.0f BPM found within %.1f BPM", bpm, kTestTempoTolBpm);
        check(r.found && std::fabs(r.bpm - bpm) <= kTestTempoTolBpm, t, r.bpm, "BPM");
        const double per = 60.0 / bpm, ph = std::fmod(r.beatOffsetSec - 0.25 + 10 * per, per);
        info("   beat-1 position error (song starts at 0.25 s), modulo one beat", std::min(ph, per - ph) * 1000, "ms");
    }
}

// ---------------------------------------------------------------------------------------------
// Each control must move the result in the expected direction (wiring proof).
static void testControls() {
    std::puts("\n== Controls change the result (wiring)");
    // Sensitivity: loud notes and very quiet notes.
    Buf b(44100, 4.0);
    for (int i = 0; i < 4; ++i) tone(b, 0.2 + i * 0.9, 0.4, 60 + i * 2, i % 2 ? 0.003 : 0.5, 0.5, 8, 1.0);   // quiet ones at about -44 dB
    const auto lo = monoTranscribe(pitchRate(b), kPitchRate, {kLeadLow, kLeadHigh, 0, 60});
    const auto hi = monoTranscribe(pitchRate(b), kPitchRate, {kLeadLow, kLeadHigh, 100, 60});
    check(lo.size() == 2 && hi.size() == 4, "Sensitivity 0 keeps 2 loud notes, 100 keeps all 4", double(hi.size() * 10 + lo.size()), "(hi*10+lo)");
    // Shortest note: 40 ms notes.
    Buf c(44100, 2.0);
    for (int i = 0; i < 5; ++i) tone(c, 0.2 + i * 0.3, 0.04, 64 + i, 0.5, 0.0, 6, 1.0);
    const auto keep = monoTranscribe(pitchRate(c), kPitchRate, {kLeadLow, kLeadHigh, 50, 20});
    const auto drop = monoTranscribe(pitchRate(c), kPitchRate, {kLeadLow, kLeadHigh, 50, 100});
    check(keep.size() == 5 && drop.empty(), "Shortest note 20 ms keeps five 40 ms notes, 100 ms drops them", double(keep.size() * 10 + drop.size()), "(keep*10+drop)");
    // Sensitivity also moves the chord and drum finders.
    const Song s = makeSong(44100, 120.0, 4);
    Buf quiet = s.drums; for (std::size_t i = 0; i < quiet.x.size(); ++i) quiet.x[i] = s.drums.x[i] + 0.08f * s.keys.x[i];
    const auto d0 = drumTranscribe(quiet.x, 44100, {0}), d100 = drumTranscribe(quiet.x, 44100, {100});
    check(d100.size() > d0.size(), "drum finder: Sensitivity 100 finds more hits than 0", double(d100.size()) - double(d0.size()), "more hits");
    const auto k0 = polyTranscribe(pitchRate(s.keys), kPitchRate, {kChordLow, kChordHigh, 0, 60}), k100 = polyTranscribe(pitchRate(s.keys), kPitchRate, {kChordLow, kChordHigh, 100, 60});
    check(k100.size() > k0.size(), "chord finder: Sensitivity 100 finds more notes than 0", double(k100.size()) - double(k0.size()), "more notes");
}

// ---------------------------------------------------------------------------------------------
static void testConvert() {
    std::puts("\n== Whole job: 4 stems in, MIDI files out, read back");
    const auto dir = std::filesystem::temp_directory_path() / "hem_midi_test";
    std::filesystem::remove_all(dir);
    Song s = makeSong(48000, 120.0, 8, -60.0);   // with a -60 dB noise floor
    std::vector<StemIn> in = {{"Drums", s.drums.audio(), Mode::Auto}, {"Bass", s.bass.audio(), Mode::Auto},
                              {"Vocals", s.vocal.audio(), Mode::Auto}, {"Keyboard", s.keys.audio(), Mode::Auto}};
    Settings st;
    const auto t0 = std::chrono::steady_clock::now();
    auto R = convert(in, st, dir, "Test Song");
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(R.error.empty(), "convert() finished without an error", R.error.empty() ? 0 : 1);
    check(R.stems.size() == 4 && R.stems[0].mode == Mode::Drums && R.stems[1].mode == Mode::Bass && R.stems[2].mode == Mode::Vocal && R.stems[3].mode == Mode::Chords,
          "Auto picked Drums / Bass / Vocal / Chords from the names", double(R.stems.size()));
    check(std::fabs(R.bpm - 120.0) <= kTestTempoTolBpm, "tempo found from all stems", R.bpm, "BPM");
    for (auto& c : R.checks) std::printf("       read-back %s\n", c.c_str());
    check(R.allChecksPassed, "every written file read back correctly", double(R.checks.size()), "checks");
    info("time to convert 4 stems x 17 s (this machine)", secs, "s");
    // score the combined file against the answer key
    const auto P = parseFile(R.combinedFile);
    auto back = [&](std::size_t tr) { std::vector<Note> v; for (auto& n : P.tracks[tr].notes) v.push_back({n.on * 60.0 / (R.bpm * kPpq), n.off * 60.0 / (R.bpm * kPpq), n.pitch, n.velocity}); return v; };
    Score total;
    const std::vector<Note>* keys[4] = {&s.tDrums, &s.tBass, &s.tVocal, &s.tKeys};
    for (std::size_t i = 0; i < 4 && i + 1 < P.tracks.size(); ++i) { const auto sc = score(*keys[i], back(i + 1), kTestOnsetTolS); total.truth += sc.truth; total.found += sc.found; total.hit += sc.hit; printScore(P.tracks[i + 1].name.c_str(), sc); }
    char t[96]; std::snprintf(t, sizeof t, "all 4 stems from the MIDI file: F-measure >= %.2f", kTestSongMinF);
    quality(total.f() >= kTestSongMinF, t, total.f());

    // Controls through convert(): typed BPM + quantize
    Settings q; q.bpm = 100; q.quantizeDiv = 16;
    std::vector<StemIn> in2 = {{"Bass", s.bass.audio(), Mode::Auto}};
    auto R2 = convert(in2, q, dir / "q", "Q");
    const auto P2 = parseFile(R2.combinedFile);
    check(std::fabs(P2.bpm - 100.0) < 0.01, "typed BPM 100 is the tempo in the file", P2.bpm, "BPM");
    bool onGrid = P2.tracks.size() > 1 && !P2.tracks[1].notes.empty();
    const std::int64_t off = secondsToTicks(R2.beatOffsetSec, 100.0);
    for (auto& n : P2.tracks[1].notes) onGrid = onGrid && ((n.on - off) % 120 + 120) % 120 == 0;
    check(onGrid, "Quantize 1/16: every note on the grid", double(P2.tracks.size() > 1 ? P2.tracks[1].notes.size() : 0), "notes");
    Settings nq; nq.bpm = 100;
    auto R3 = convert(in2, nq, dir / "nq", "NQ");
    const auto P3 = parseFile(R3.combinedFile);
    bool allOn = true; for (auto& n : P3.tracks[1].notes) allOn = allOn && ((n.on - off) % 120 + 120) % 120 == 0;
    check(!allOn, "Quantize Off: notes keep their own timing (not all on grid)", allOn ? 1 : 0);
    // Type menu: the same audio as Drums vs Bass gives different tracks
    std::vector<StemIn> in3 = {{"Bass", s.bass.audio(), Mode::Drums}};
    auto R4 = convert(in3, Settings{}, dir / "t", "T");
    const auto P4 = parseFile(R4.combinedFile);
    check(P4.tracks.size() == 2 && !P4.tracks[1].notes.empty() && P4.tracks[1].notes[0].channel == 9, "Type menu: bass audio set to Drums goes to channel 10", P4.tracks.size() > 1 ? double(P4.tracks[1].notes.size()) : -1, "hits");
    std::vector<StemIn> in4 = {{"Bass", s.bass.audio(), Mode::Skip}, {"Keyboard", s.keys.audio(), Mode::Auto}};
    auto R5 = convert(in4, Settings{}, dir / "s", "S");
    check(parseFile(R5.combinedFile).tracks.size() == 2, "Type menu: Skip leaves the stem out", double(parseFile(R5.combinedFile).tracks.size() - 1), "tracks");

    // 13 stems
    std::vector<StemIn> many;
    const char* names[13] = {"Vocals", "Backing Vocals", "Drums", "Bass", "Guitar", "Keyboard", "Synth", "Strings", "Percussion", "Brass", "Woodwinds", "Lead", "Pad"};
    for (int i = 0; i < 13; ++i) many.push_back({names[i], (i == 2 || i == 8) ? s.drums.audio() : i == 3 ? s.bass.audio() : (i == 0 || i == 11) ? s.vocal.audio() : s.keys.audio(), Mode::Auto});
    auto R6 = convert(many, Settings{}, dir / "13", "Thirteen");
    const auto P6 = parseFile(R6.combinedFile);
    check(P6.tracks.size() == 14 && R6.allChecksPassed, "13 stems -> one file with 13 tracks + 13 single files, all read back", double(P6.tracks.size() - 1), "tracks");
    int files = 0; for (auto& e : std::filesystem::directory_iterator(dir / "13")) files += e.path().extension() == ".mid";
    check(files == 14, "14 .mid files written (13 stems + ALL STEMS)", files);

    // single full song in one file
    Buf mix(48000, double(s.drums.x.size()) / 48000.0);
    for (std::size_t i = 0; i < mix.x.size(); ++i) mix.x[i] = 0.35f * (s.drums.x[i] + s.bass.x[i] + s.vocal.x[i] + s.keys.x[i]);
    std::vector<StemIn> one = {{"My Song", mix.audio(), Mode::Auto}};
    auto R7 = convert(one, Settings{}, dir / "one", "My Song");
    check(R7.stems.size() == 1 && R7.stems[0].mode == Mode::FullMix && R7.allChecksPassed, "single file with a plain name -> Full song (notes + drums tracks)", double(parseFile(R7.combinedFile).tracks.size() - 1), "tracks");
    const auto P7 = parseFile(R7.combinedFile);
    if (P7.tracks.size() == 3) {
        std::vector<Note> allTruth = s.tBass; allTruth.insert(allTruth.end(), s.tVocal.begin(), s.tVocal.end()); allTruth.insert(allTruth.end(), s.tKeys.begin(), s.tKeys.end());
        auto toSec = [&](const ParsedTrack& tr) { std::vector<Note> v; for (auto& n : tr.notes) v.push_back({n.on * 60.0 / (R7.bpm * kPpq), 0, n.pitch, 0}); return v; };
        const auto sn = score(allTruth, toSec(P7.tracks[1]), kTestOnsetTolS), sd = score(s.tDrums, toSec(P7.tracks[2]), kTestOnsetTolS);
        info("full song in ONE file, notes track F-measure (no pass bar: expected messy)", sn.f()); printScore("full-song notes", sn);
        info("full song in ONE file, drums track F-measure (no pass bar)", sd.f()); printScore("full-song drums", sd);
    }
    std::filesystem::remove_all(dir);
}

int main() {
    std::puts("HEM STEM-TO-MIDI tests (C++23). Limits are ASSUMED until approved.");
    testMidiFile();
    testSingleNote();
    testSilence();
    testParts(44100);
    testParts(48000);
    testTempo();
    testControls();
    testConvert();
    std::printf("\nMUST-WORK CHECKS: %s (%d failure%s)\n", g_fail ? "SOME FAILED" : "ALL PASSED", g_fail, g_fail == 1 ? "" : "s");
    std::printf("ACCURACY BARS: %d of %d met, %d missed (bars are ASSUMED, see midi_config.hpp)\n", g_qtotal - g_qfail, g_qtotal, g_qfail);
    return g_fail;
}
