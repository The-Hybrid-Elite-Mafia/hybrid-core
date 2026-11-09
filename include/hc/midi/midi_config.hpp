// =============================================================================
//  midi/midi_config.hpp — HEM STEM-TO-MIDI: the ONE place every working guess lives.
//
//  Every number below that has not been checked against Steven's real Suno stems is marked
//  // ASSUMED. Things nobody knows yet are marked UNKNOWN. Change a guess here, nowhere else.
//  The pass/fail limits the tests use are in this file too, so they can't be loosened quietly.
// =============================================================================
#pragma once
#include <array>
#include <string_view>

namespace hc::midi {

// ---- What kind of part a stem is -------------------------------------------------------------
enum class Mode : int {
    Auto = 0,   // pick from the file name (see kNameRules)
    Drums,      // kick / snare / hats / percussion  -> GM drum notes on MIDI channel 10
    Bass,       // one note at a time, low range
    Vocal,      // one note at a time, voice range (lead vocal melody)
    Lead,       // one note at a time, wide range (lead synth, solo guitar, whistle, sax...)
    Chords,     // several notes at once (keys, guitar, strings, pads, backing vocals)
    FullMix,    // a whole song in one file: chords track + drums track
    Skip,       // leave this stem out
};
inline constexpr int kModeCount = 8;
inline constexpr std::array<std::string_view, kModeCount> kModeNames = {
    "Auto", "Drums", "Bass", "Vocal", "Lead", "Chords", "Full song", "Skip"};

// ---- File name -> part type (Auto). First match wins. --------------------------------- ASSUMED
// Suno's stem names (e.g. "Vocals", "Backing Vocals", "Drums", "Bass", "Guitar", "Keyboard",
// "Synth", "Strings", "Percussion", "FX") were not checked against a real Suno export. UNKNOWN
// which exact names Suno writes today.
struct NameRule { std::string_view word; Mode mode; int gmProgram; };   // gmProgram: 0-based GM
inline constexpr std::array<NameRule, 30> kNameRules = {{
    {"backing", Mode::Chords, 52},   // backing vocals = harmonies -> chords     // ASSUMED
    {"harmon", Mode::Chords, 52},                                              // ASSUMED
    {"choir", Mode::Chords, 52},                                               // ASSUMED
    {"drum", Mode::Drums, 0}, {"kick", Mode::Drums, 0}, {"snare", Mode::Drums, 0},
    {"hat", Mode::Drums, 0}, {"perc", Mode::Drums, 0}, {"808 drum", Mode::Drums, 0},
    {"bass", Mode::Bass, 33}, {"808", Mode::Bass, 38}, {"sub", Mode::Bass, 38},  // ASSUMED: "808" = bass
    {"vocal", Mode::Vocal, 53}, {"vox", Mode::Vocal, 53}, {"voice", Mode::Vocal, 53},
    {"sing", Mode::Vocal, 53}, {"lead", Mode::Lead, 80}, {"melody", Mode::Lead, 80},
    {"piano", Mode::Chords, 0}, {"key", Mode::Chords, 4}, {"organ", Mode::Chords, 16},
    {"guitar", Mode::Chords, 25}, {"string", Mode::Chords, 48}, {"pad", Mode::Chords, 89},
    {"synth", Mode::Chords, 81}, {"brass", Mode::Chords, 61}, {"horn", Mode::Chords, 61},
    {"wood", Mode::Chords, 73}, {"flute", Mode::Lead, 73}, {"fx", Mode::Skip, 0},   // ASSUMED: FX skipped
}};
inline constexpr Mode kUnknownNameMode = Mode::Chords;      // ASSUMED: unknown stem -> chords
inline constexpr int  kUnknownNameProgram = 0;              // ASSUMED: piano

// ---- Pitch ranges per part (MIDI note numbers) ---------------------------------------- ASSUMED
inline constexpr int kBassLow = 23, kBassHigh = 67;       // B0 .. G4
inline constexpr int kVocalLow = 38, kVocalHigh = 86;     // D2 .. D6
inline constexpr int kLeadLow = 36, kLeadHigh = 96;       // C2 .. C7
inline constexpr int kChordLow = 28, kChordHigh = 100;    // E1 .. E7

// ---- Shared front end ------------------------------------------------------------------------
inline constexpr double kHopSeconds = 0.0058;       // analysis step ~5.8 ms (all trackers)
inline constexpr double kGateBelowPeakDb = 42.0;    // ASSUMED: quieter than peak-42 dB = silence
inline constexpr double kAbsoluteFloorDb = -66.0;   // ASSUMED: never treat below -66 dBFS as a note

// "Sensitivity" control 0..100 (default 50) moves the gates by this many dB each side.
inline constexpr double kSensitivityRangeDb = 12.0;         // ASSUMED
inline constexpr int    kSensitivityDefault = 50;
// "Shortest note" control in ms.
inline constexpr int kShortestNoteMinMs = 20, kShortestNoteMaxMs = 250;
inline constexpr int kShortestNoteDefaultMs = 60;            // ASSUMED

// ---- One-note-at-a-time tracker (YIN pitch) -------------------------------------------------
inline constexpr double kYinThreshold = 0.20;       // ASSUMED (YIN paper uses 0.10-0.15 on speech)
inline constexpr double kPitchHoldSemis = 0.70;     // ASSUMED: drift allowed inside one note (vibrato)
inline constexpr int    kPitchChangeFrames = 4;     // ASSUMED: new pitch must hold ~23 ms to split
inline constexpr double kReattackRiseDb = 5.0;      // ASSUMED: level jump that restarts a same-pitch note
inline constexpr double kReattackWindowS = 0.025;   // ASSUMED
inline constexpr int    kMedianFrames = 5;          // pitch median smoothing

// ---- Several-notes tracker (harmonic salience) ----------------------------------------------
inline constexpr int    kPolyMaxNotes = 6;          // ASSUMED: most notes at once
inline constexpr int    kPolyHarmonics = 10;        // ASSUMED
inline constexpr double kPolyHarmonicDecay = 0.90;  // ASSUMED: weight of harmonic h = decay^(h-1)  (picked by a sweep on real piano + test keys)
inline constexpr float  kPolyCompress = 0.75f;      // ASSUMED: magnitude^0.75 before summing harmonics (same sweep)
inline constexpr double kPolyRelThreshold = 0.26;   // ASSUMED: a note must reach 26 % of the frame's strongest (same sweep)
inline constexpr double kPolyGapMergeS = 0.035;     // ASSUMED: gaps shorter than this are joined
inline constexpr double kPolyReattackDb = 4.0;      // ASSUMED: a pitch's own energy jump (dB) at an onset that re-strikes it
inline constexpr double kPolyOnsetK = 1.5, kPolyOnsetDelta = 0.10;   // ASSUMED: onset picking for chord parts
inline constexpr double kPolyLookBackS = 0.6;      // ASSUMED: a chord note found late can start up to 0.6 s earlier, at its own onset
inline constexpr int    kPolyNeedFundamentalFrom = 40; // ASSUMED: from E2 up, a chord note must show its own fundamental

// ---- Drum tracker (NMF: kick / snare / hat parts, see trackers.hpp) -------------------------
inline constexpr double kDrumMinGapS = 0.045;       // ASSUMED: same drum can't hit twice inside 45 ms
inline constexpr double kDrumPeakWindowS = 0.10;    // ASSUMED: adaptive threshold window
inline constexpr double kDrumThresholdK = 1.6;      // ASSUMED: a part's rise must beat (local median * k) + delta
inline constexpr double kDrumThresholdDelta = 0.15; // ASSUMED: delta as a fraction of that part's biggest rise (picked by a sweep on 2 real kits + test kit)
inline constexpr double kDrumWithinDb = 12.0;       // ASSUMED: a drum counts if within 12 dB of its typical strong hit (Sensitivity +-6 dB)
inline constexpr int    kNmfIterations = 40;        // ASSUMED: NMF update rounds (half with fixed drum shapes)
inline constexpr double kNmfRange[3][2] = {{20, 1500}, {80, 12000}, {1500, 22000}};   // ASSUMED: kick / snare / hat may only adapt inside these Hz
inline constexpr double kNmfAdapt = 0.5;            // ASSUMED: how far the kick/snare/hat shapes may move toward this kit each round
inline constexpr double kDrumBleedDb = 6.0;         // ASSUMED: a hit 6+ dB weaker (vs its own drum) than another drum's hit at the same moment = bleed
inline constexpr double kDrumAttackShare = 0.5;     // ASSUMED: a hit must at least double its drum's level within ~17 ms (else it's ringing, not a hit)
inline constexpr double kOpenHatDecayS = 0.16;      // ASSUMED: hat ringing longer than this = open hat
inline constexpr int kGmKick = 36, kGmSnare = 38, kGmClosedHat = 42, kGmOpenHat = 46;

// ---- Tempo ----------------------------------------------------------------------------------
inline constexpr double kTempoMinBpm = 60.0, kTempoMaxBpm = 190.0;
inline constexpr double kTempoPreferBpm = 100.0;    // ASSUMED: centre of the "likely tempo" weighting
inline constexpr double kTempoPreferWidthOct = 0.9; // ASSUMED
inline constexpr double kFallbackBpm = 120.0;       // used when no beat can be found

// ---- MIDI file ------------------------------------------------------------------------------
inline constexpr int kPpq = 480;                    // ticks per quarter note
inline constexpr int kDrumChannel = 9;              // MIDI channel 10
inline constexpr int kVelocityMin = 30, kVelocityMax = 127;
inline constexpr double kVelocityRangeDb = 36.0;    // ASSUMED: peak-36 dB -> min velocity

// ---- Pass/fail limits used by the tests ------------------------------------------- ASSUMED
// (Steven has not approved these yet. If a result misses, report the failure; never widen these.)
inline constexpr double kTestOnsetTolS = 0.050;     // standard MIREX note-onset tolerance is 50 ms
inline constexpr double kTestMonoMinF = 0.95;       // one-note parts: note F-measure
inline constexpr double kTestPolyMinF = 0.85;       // chord parts: note F-measure
inline constexpr double kTestDrumMinF = 0.95;       // drum hits (right drum, right time)
inline constexpr double kTestTempoTolBpm = 1.0;
inline constexpr double kTestSongMinF = 0.80;       // full test song, every stem together

// ---- How the "Sensitivity" control (0..100) moves each tracker ------------------------ ASSUMED
// s = 50 gives the base values above. Higher = more notes found (quieter / less certain notes kept).
struct Tuned {
    double gateBelowPeakDb, absoluteFloorDb, yinThreshold, polyRelThreshold, drumThresholdK;
};
inline Tuned tuned(int sensitivity) {
    const double u = (static_cast<double>(sensitivity < 0 ? 0 : sensitivity > 100 ? 100 : sensitivity) - 50.0) / 50.0;  // -1..+1
    double p = 1.0;   // 2^u without <cmath> in this header
    if (u >= 0) p = 1.0 + u; else p = 1.0 / (1.0 - u);
    return {
        kGateBelowPeakDb + kSensitivityRangeDb * u,     // s=100: 54 dB, s=0: 30 dB         // ASSUMED
        kAbsoluteFloorDb - kSensitivityRangeDb * u,                                         // ASSUMED
        kYinThreshold + 0.08 * u,                       // 0.12 .. 0.28                     // ASSUMED
        kPolyRelThreshold / p,                          // 0.44 .. 0.11                     // ASSUMED
        kDrumThresholdK / p,                            // 3.2 .. 0.8                       // ASSUMED
    };
}

} // namespace hc::midi
