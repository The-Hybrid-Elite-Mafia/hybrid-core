// =============================================================================
//  tests/midi_signals.hpp — known-answer test audio for HEM STEM-TO-MIDI.
//  Every signal comes with the exact notes that made it (the "answer key"), so the
//  converter's output can be scored: right pitch, start within 50 ms.
// =============================================================================
#pragma once
#include "hc/io/wav.hpp"
#include "hc/midi/smf.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

namespace hemtest {
using hc::midi::Note;
inline constexpr double kTwoPi = 2.0 * std::numbers::pi;
inline double hz(double midi) { return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0); }

struct Buf {
    double sr; std::vector<float> x;
    Buf(double rate, double seconds) : sr(rate), x(std::size_t(rate * seconds), 0.0f) {}
    hc::wav::Audio audio() const { hc::wav::Audio a; a.sampleRate = sr; a.ch[0] = x; a.ch[1] = x; return a; }
    void normalize(double peak) { float m = 0; for (float v : x) m = std::max(m, std::fabs(v)); if (m > 0) for (auto& v : x) v = float(v * peak / m); }
};

// Harmonic tone: nh harmonics, amplitude 1/h^tilt, exponential decay, optional vibrato.
// `formant` > 0 shapes harmonics with two vowel-like bumps (rough voice).
inline void tone(Buf& b, double t0, double dur, double midi, double amp, double decayPerS, int nh, double tilt,
                 double vibCents = 0, double vibHz = 5.5, bool formant = false) {
    const std::size_t i0 = std::size_t(t0 * b.sr), n = std::size_t(dur * b.sr);
    const double f0 = hz(midi);
    double ph[40] = {};
    for (std::size_t i = 0; i < n && i0 + i < b.x.size(); ++i) {
        const double t = double(i) / b.sr;
        const double env = std::exp(-t * decayPerS) * std::min(1.0, t / 0.005) * std::min(1.0, (dur - t) / 0.012);
        const double f = f0 * std::pow(2.0, vibCents / 1200.0 * std::sin(kTwoPi * vibHz * t));
        double v = 0;
        for (int h = 1; h <= nh && h < 40; ++h) {
            const double fh = f * h;
            if (fh > 0.45 * b.sr) break;
            ph[h] += kTwoPi * fh / b.sr;
            double g = 1.0 / std::pow(double(h), tilt);
            if (formant) g *= 0.25 + std::exp(-std::pow((fh - 650.0) / 250.0, 2)) + 0.7 * std::exp(-std::pow((fh - 1150.0) / 300.0, 2));
            v += g * std::sin(ph[h]);
        }
        b.x[i0 + i] += float(amp * env * v);
    }
}

struct Drums { std::mt19937 rng{11}; std::normal_distribution<float> g{0.0f, 1.0f}; };
inline void kick(Buf& b, Drums& d, double t0, double amp) {
    // pitch-swept sine body + a short beater click (noise low-passed around 4 kHz, like a real kick)
    const std::size_t i0 = std::size_t(t0 * b.sr), n = std::size_t(0.35 * b.sr); double ph = 0, lp = 0;
    const double a = 1.0 - std::exp(-kTwoPi * 4000.0 / b.sr);
    for (std::size_t i = 0; i < n && i0 + i < b.x.size(); ++i) {
        const double t = double(i) / b.sr; const double f = 45.0 + 75.0 * std::exp(-t * 35.0);
        ph += kTwoPi * f / b.sr;
        lp += a * (d.g(d.rng) - lp);
        const double click = t < 0.004 ? 0.15 * lp : 0.0;
        b.x[i0 + i] += float(amp * (std::exp(-t * 9.0) * std::sin(ph) + click));
    }
}
inline void snare(Buf& b, Drums& d, double t0, double amp) {
    // 190 Hz body + wire noise (high-passed, then low-passed around 7 kHz: most energy 1-7 kHz)
    const std::size_t i0 = std::size_t(t0 * b.sr), n = std::size_t(0.25 * b.sr); float p1 = 0, lp = 0;
    const float a = float(1.0 - std::exp(-kTwoPi * 7000.0 / b.sr));
    for (std::size_t i = 0; i < n && i0 + i < b.x.size(); ++i) {
        const double t = double(i) / b.sr; const float w = d.g(d.rng); lp += a * ((w - p1) - lp); p1 = w; const float hp = 1.6f * lp;
        b.x[i0 + i] += float(amp * (0.55 * std::exp(-t * 22.0) * hp + 0.5 * std::exp(-t * 30.0) * std::sin(kTwoPi * 190.0 * t)));
    }
}
inline void hat(Buf& b, Drums& d, double t0, double amp, bool open) {
    // noise, high-passed (second difference) then low-passed around 12 kHz: most energy 6-14 kHz like a real hat
    const std::size_t i0 = std::size_t(t0 * b.sr), n = std::size_t((open ? 0.4 : 0.06) * b.sr); float p1 = 0, p2 = 0, lp = 0;
    const float a = float(1.0 - std::exp(-kTwoPi * 12000.0 / b.sr));
    for (std::size_t i = 0; i < n && i0 + i < b.x.size(); ++i) {
        const double t = double(i) / b.sr; const float w = d.g(d.rng);
        lp += a * ((w - 2 * p1 + p2) - lp); p2 = p1; p1 = w; const float hp = 1.5f * lp;
        b.x[i0 + i] += float(amp * 0.35 * hp * std::exp(-t * (open ? 7.0 : 70.0)));
    }
}

inline void addNoise(Buf& b, double rmsDb, unsigned seed = 5) {
    std::mt19937 r(seed); std::normal_distribution<float> g(0.0f, 1.0f);
    const float a = float(std::pow(10.0, rmsDb / 20.0));
    for (auto& v : b.x) v += a * g(r);
}

// ---- Scoring: note F-measure, onset within tolerance and same pitch, one match per note ----
struct Score { int truth = 0, found = 0, hit = 0; double f() const { return truth + found == 0 ? 1.0 : 2.0 * hit / double(truth + found); }
               double precision() const { return found ? double(hit) / found : 1.0; } double recall() const { return truth ? double(hit) / truth : 1.0; } };
inline Score score(const std::vector<Note>& truth, const std::vector<Note>& got, double tol) {
    Score s; s.truth = int(truth.size()); s.found = int(got.size());
    std::vector<char> used(got.size(), char(0));
    for (const auto& t : truth) {
        int best = -1; double bd = tol + 1e-9;
        for (std::size_t j = 0; j < got.size(); ++j)
            if (!used[j] && got[j].pitch == t.pitch && std::fabs(got[j].start - t.start) <= bd) { bd = std::fabs(got[j].start - t.start); best = int(j); }
        if (best >= 0) { used[std::size_t(best)] = 1; ++s.hit; }
    }
    return s;
}

// ---- The test song: 120 BPM (or any), E minor, 8 bars ----------------------------------------
struct Song { Buf drums, bass, vocal, keys; std::vector<Note> tDrums, tBass, tVocal, tKeys; double bpm; };

inline Song makeSong(double sr, double bpm, int bars = 8, double noiseDb = -200) {
    const double beat = 60.0 / bpm, len = bars * 4 * beat + 1.0;
    Song s{Buf(sr, len), Buf(sr, len), Buf(sr, len), Buf(sr, len), {}, {}, {}, {}, bpm};
    Drums d;
    const double lead = 0.25;   // song starts a quarter second in
    for (int bar = 0; bar < bars; ++bar)
        for (int e = 0; e < 8; ++e) {             // eighth notes
            const double t = lead + (bar * 4 + e * 0.5) * beat;
            if (e == 0 || e == 3 || e == 4) { kick(s.drums, d, t, 0.9); s.tDrums.push_back({t, t + 0.1, 36, 100}); }
            if (e == 2 || e == 6) { snare(s.drums, d, t, 0.7); s.tDrums.push_back({t, t + 0.1, 38, 100}); }
            const bool open = (e == 7 && bar % 2 == 1);
            if (e != 2 && e != 6) { hat(s.drums, d, t, open ? 0.5 : 0.4, open); s.tDrums.push_back({t, t + 0.1, open ? 46 : 42, 100}); }
        }
    // bass: eighths, with repeated notes (re-attack test)
    const int roots[4] = {40, 36, 43, 38};                 // E2 C2 G2 D2
    const int pat[8] = {0, 0, 12, 0, 7, 7, 0, 10};
    for (int bar = 0; bar < bars; ++bar)
        for (int e = 0; e < 8; ++e) {
            const double t = lead + (bar * 4 + e * 0.5) * beat; const int p = roots[bar % 4] + pat[e];
            tone(s.bass, t, 0.5 * beat * 0.9, p, 0.5, 1.2, 8, 1.0); s.tBass.push_back({t, t + 0.45 * beat, p, 100});
        }
    // vocal: quarter and half notes with vibrato
    const int mel[16] = {64, 67, 71, 69, 67, 64, 62, 64, 71, 72, 71, 69, 67, 66, 64, 64};
    const double dur[16] = {1, 1, 1, 1, 2, 1, 1, 2, 1, 1, 1, 1, 1, 1, 2, 2};
    double pos = 0;
    for (int bar = 0; bar < bars; bar += 4) {
        for (int i = 0; i < 16 && pos < (bar + 4) * 4; ++i) {
            const double t = lead + pos * beat;
            tone(s.vocal, t, dur[i] * beat * 0.92, mel[i], 0.35, 0.3, 14, 0.6, 30.0, 5.5, true);
            s.tVocal.push_back({t, t + dur[i] * beat * 0.92, mel[i], 100});
            pos += dur[i];
        }
        pos = (bar + 4) * 4;
    }
    // keys: one chord per half bar (repeated chords = re-attack test)
    const int ch[4][4] = {{52, 55, 59, 64}, {48, 52, 55, 60}, {55, 59, 62, 67}, {50, 54, 57, 62}};   // Em C G D
    for (int bar = 0; bar < bars; ++bar)
        for (int h = 0; h < 2; ++h) {
            const double t = lead + (bar * 4 + h * 2) * beat;
            for (int k = 0; k < 4; ++k) { tone(s.keys, t, 2 * beat * 0.95, ch[bar % 4][k], 0.18, 1.6, 12, 0.8); s.tKeys.push_back({t, t + 2 * beat * 0.95, ch[bar % 4][k], 100}); }
        }
    for (Buf* b : {&s.drums, &s.bass, &s.vocal, &s.keys}) { b->normalize(0.7); if (noiseDb > -150) addNoise(*b, noiseDb); }
    return s;
}

} // namespace hemtest
