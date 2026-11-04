// =============================================================================
//  apps/hc_demo_stems.cpp — writes 13 synthetic demo stems (any OS).
//
//  hc_demo_stems OUT_FOLDER [seconds]
//
//  Made-up music, 100 BPM, A minor: it exists so the app, the installer test and the
//  screenshots have something to load. Stem 7 ("Guitar") is written at 44.1 kHz on purpose,
//  so every run also exercises the sample-rate converter in the real app.
// =============================================================================
#include "hc/io/wav.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <numbers>
#include <random>
#include <string>
#include <vector>

using namespace hc;

namespace {
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kBpm = 100.0;

struct Voice { double sr; std::mt19937 rng{7}; std::normal_distribution<float> g{0.0f, 1.0f}; };

double midiHz(int n) { return 440.0 * std::pow(2.0, (n - 69) / 12.0); }

wav::Audio blank(double sr, double seconds) {
    wav::Audio a; a.sampleRate = sr;
    a.ch[0].assign(static_cast<std::size_t>(sr * seconds), 0.0f); a.ch[1] = a.ch[0];
    return a;
}

// Adds a decaying tone at time t0.
void note(wav::Audio& a, double t0, double dur, double hz, double amp, double decay, int harmonics, double pan) {
    const std::size_t i0 = static_cast<std::size_t>(t0 * a.sampleRate), n = static_cast<std::size_t>(dur * a.sampleRate);
    for (std::size_t i = 0; i < n && i0 + i < a.frames(); ++i) {
        const double t = double(i) / a.sampleRate;
        const double env = std::exp(-t * decay) * std::min(1.0, t / 0.004);
        double v = 0;
        for (int h = 1; h <= harmonics; ++h) v += std::sin(kTwoPi * hz * h * t) / double(h);
        v *= amp * env;
        a.ch[0][i0 + i] += static_cast<float>(v * (1.0 - 0.5 * pan));
        a.ch[1][i0 + i] += static_cast<float>(v * (1.0 + 0.5 * pan));
    }
}
void burst(wav::Audio& a, Voice& vc, double t0, double dur, double amp, double decay, double tonehz, double toneAmt, bool highpass) {
    const std::size_t i0 = static_cast<std::size_t>(t0 * a.sampleRate), n = static_cast<std::size_t>(dur * a.sampleRate);
    float p1 = 0, p2 = 0;
    for (std::size_t i = 0; i < n && i0 + i < a.frames(); ++i) {
        const double t = double(i) / a.sampleRate;
        float w = vc.g(vc.rng);
        if (highpass) { const float x = w; w = x - 2.0f * p1 + p2; p2 = p1; p1 = x; w *= 0.4f; }
        const double v = amp * std::exp(-t * decay) * ((1.0 - toneAmt) * w + toneAmt * std::sin(kTwoPi * tonehz * t * (1.0 + 0.6 * std::exp(-t * 30))));
        a.ch[0][i0 + i] += static_cast<float>(v); a.ch[1][i0 + i] += static_cast<float>(v * 0.97);
    }
}
void finish(wav::Audio& a, double peak) {
    float pk = 0; for (int c = 0; c < 2; ++c) for (float v : a.ch[c]) pk = std::max(pk, std::fabs(v));
    if (pk > 0) for (int c = 0; c < 2; ++c) for (float& v : a.ch[c]) v *= static_cast<float>(peak / pk);
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: hc_demo_stems OUT_FOLDER [seconds]\n"); return 2; }
    const std::filesystem::path out = argv[1];
    const double seconds = argc > 2 ? std::atof(argv[2]) : 24.0;
    std::filesystem::create_directories(out);
    const double beat = 60.0 / kBpm, bar = 4.0 * beat;
    const int bars = static_cast<int>(seconds / bar);
    const int chordRoots[4] = {57, 53, 48, 55};   // Am F C G
    struct Spec { const char* name; double sr; double peak; };
    const Spec specs[13] = {{"Drums", 48000, 0.8}, {"Bass", 48000, 0.7}, {"Keys", 48000, 0.5}, {"Pad", 48000, 0.4}, {"Lead Synth", 48000, 0.5},
                            {"Lead Vocal", 48000, 0.6}, {"Guitar", 44100, 0.5}, {"Backing Vocals", 48000, 0.4}, {"Percussion", 48000, 0.5},
                            {"Strings", 48000, 0.45}, {"FX", 48000, 0.35}, {"Choir", 48000, 0.4}, {"Hats", 48000, 0.5}};
    for (int s = 0; s < 13; ++s) {
        Voice vc{specs[s].sr}; vc.rng.seed(100u + unsigned(s));
        wav::Audio a = blank(specs[s].sr, seconds);
        for (int b = 0; b < bars; ++b) {
            const double t0 = b * bar;
            const int root = chordRoots[b % 4];
            switch (s) {
            case 0:   // kick / snare
                for (int k = 0; k < 4; ++k) {
                    burst(a, vc, t0 + k * beat, 0.35, 0.9, 11, 55, 0.95, false);
                    if (k % 2 == 1) burst(a, vc, t0 + k * beat, 0.3, 0.7, 14, 190, 0.35, false);
                }
                break;
            case 1: for (int k = 0; k < 8; ++k) note(a, t0 + k * beat * 0.5, beat * 0.45, midiHz(root - 24 + ((k % 4 == 3) ? 7 : 0)), 0.5, 4, 3, 0); break;
            case 2: for (int k : {0, 4, 7, 12}) note(a, t0, bar * 0.95, midiHz(root + k), 0.18, 1.6, 4, 0.3); break;
            case 3: for (int k : {0, 7, 12, 16}) note(a, t0, bar, midiHz(root + k - 12), 0.14, 0.25, 6, -0.4); break;
            case 4: for (int k = 0; k < 4; ++k) note(a, t0 + k * beat, beat * 0.9, midiHz(root + 24 + ((k * 3) % 5) * 2), 0.3, 3, 8, 0.6); break;
            case 5: for (int k = 0; k < 2; ++k) { note(a, t0 + k * 2 * beat, beat * 1.7, midiHz(root + 12 + k * 3), 0.3, 1.4, 12, 0); burst(a, vc, t0 + k * 2 * beat + 0.02, 0.18, 0.18, 12, 0, 0.0, true); } break;
            case 6: for (int k = 0; k < 8; ++k) note(a, t0 + k * beat * 0.5, beat * 0.5, midiHz(root + 12 + (k % 3) * 4), 0.3, 6, 10, -0.5); break;
            case 7: for (int k : {0, 7}) note(a, t0 + beat, bar * 0.7, midiHz(root + 19 + k), 0.2, 1.8, 6, 0.2); break;
            case 8: for (int k = 0; k < 8; ++k) burst(a, vc, t0 + k * beat * 0.5 + (k % 2) * 0.01, 0.12, 0.35 + 0.25 * (k % 2), 14, 0, 0.0, true); break;
            case 9: for (int k : {0, 4, 7}) note(a, t0, bar, midiHz(root + k), 0.16, 0.5, 14, -0.2); break;
            case 10: if (b % 2 == 1) burst(a, vc, t0, bar * 0.8, 0.25, 2.5, 0, 0.0, true); break;
            case 11: for (int k : {0, 3, 7}) note(a, t0, bar, midiHz(root + 12 + k), 0.15, 0.3, 5, 0.1); break;
            default: for (int k = 0; k < 16; ++k) burst(a, vc, t0 + k * beat * 0.25, 0.06, k % 4 == 2 ? 0.5 : 0.28, 40, 0, 0.0, true); break;
            }
        }
        finish(a, specs[s].peak);
        char file[128]; std::snprintf(file, sizeof file, "%02d %s.wav", s + 1, specs[s].name);
        if (auto e = wav::write(out / file, a); !e.empty()) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
        std::printf("wrote %s (%.0f Hz, %.1f s)\n", file, a.sampleRate, seconds);
    }
    return 0;
}
