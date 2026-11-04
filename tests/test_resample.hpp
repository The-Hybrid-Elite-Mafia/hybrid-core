// =============================================================================
//  tests/test_resample.hpp — sample-rate converter, measured.
// =============================================================================
#pragma once
#include "hc/dsp/resampler.hpp"
#include <chrono>

namespace resamp {

inline std::vector<float> sine(double f, double sr, double seconds, double amp = 0.5) {
    std::vector<float> v(static_cast<std::size_t>(sr * seconds));
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * f * double(i) / sr));
    return v;
}
// RMS in dB of the middle of the signal (edges skipped).
inline double rmsDb(const std::vector<float>& v, std::size_t skip = 4000) {
    double e = 0; std::size_t c = 0;
    for (std::size_t i = skip; i + skip < v.size(); ++i) { e += double(v[i]) * v[i]; ++c; }
    return 10.0 * std::log10(e / double(c) + 1e-30);
}

// Amplitude (dB re 1.0) of a known frequency, found by a least-squares sine/cosine fit.
// Works for any number of cycles, so different-length signals compare fairly.
inline double fitDb(const std::vector<float>& v, double f, double sr, std::size_t skip = 4000) {
    double ss = 0, cc = 0, sc = 0, sx = 0, cx = 0;
    for (std::size_t i = skip; i + skip < v.size(); ++i) {
        const double w = 2.0 * std::numbers::pi * f * double(i) / sr, s = std::sin(w), c = std::cos(w);
        ss += s * s; cc += c * c; sc += s * c; sx += s * v[i]; cx += c * v[i];
    }
    const double det = ss * cc - sc * sc, A = (sx * cc - cx * sc) / det, B = (cx * ss - sx * sc) / det;
    return 20.0 * std::log10(std::sqrt(A * A + B * B) + 1e-30);
}

} // namespace resamp

static void testResampler() {
    using namespace resamp;
    std::printf("\n--- Resampler (stems at other sample rates are converted when loaded) ---\n");
    char name[160];

    // Length: output has the right number of samples.
    {
        std::vector<float> in(44100, 0.0f), out;
        Resampler::convert(in, 44100, 48000, out);
        check(out.size() == 48000, "44.1 kHz -> 48 kHz, 1 s: output length (samples)", double(out.size()));
        Resampler::convert(std::vector<float>(96000, 0.0f), 96000, 48000, out);
        check(out.size() == 48000, "96 kHz -> 48 kHz, 1 s: output length (samples)", double(out.size()));
    }
    // Same rate: untouched.
    {
        const auto in = sine(1000, 48000, 0.5); std::vector<float> out;
        Resampler::convert(in, 48000, 48000, out);
        check(out == in, "48 kHz -> 48 kHz: samples unchanged", out == in ? 1.0 : 0.0);
    }
    // Passband level: a sine in, the same sine level out.
    struct Pair { double src, dst; };
    for (Pair p : {Pair{44100, 48000}, Pair{48000, 44100}, Pair{96000, 48000}, Pair{22050, 48000}, Pair{88200, 48000}, Pair{48000, 96000}}) {
        double worst = 0, worstF = 0;
        for (double f : {100.0, 1000.0, 5000.0, 10000.0, 15000.0, 18000.0, 20000.0}) {
            if (f > 0.45 * std::min(p.src, p.dst)) continue;
            const auto in = sine(f, p.src, 1.0); std::vector<float> out;
            Resampler::convert(in, p.src, p.dst, out);
            const double d = fitDb(out, f, p.dst) - fitDb(in, f, p.src);
            if (std::fabs(d) > std::fabs(worst)) { worst = d; worstF = f; }
        }
        std::snprintf(name, sizeof name, "%.0f -> %.0f Hz: passband level, worst error (at %.0f Hz)", p.src, p.dst, worstF);
        check(std::fabs(worst) < 0.005, name, worst, "dB");
    }
    // Frequency is kept: zero-crossing rate of the output sine.
    {
        const auto in = sine(1000, 44100, 1.0); std::vector<float> out;
        Resampler::convert(in, 44100, 48000, out);
        std::size_t zc = 0; for (std::size_t i = 4001; i + 4000 < out.size(); ++i) if ((out[i - 1] < 0) != (out[i] < 0)) ++zc;
        const double hz = double(zc) / 2.0 / (double(out.size() - 8001) / 48000.0);
        check(std::fabs(hz - 1000.0) < 1.0, "44.1 -> 48 kHz: a 1000 Hz sine stays at (Hz)", hz, "Hz");
    }
    // Down-conversion removes what would alias. At 96 kHz a tone above 24 kHz would fold back below it.
    for (double f : {24500.0, 25000.0, 30000.0}) {
        const auto in = sine(f, 96000, 1.0); std::vector<float> out;
        Resampler::convert(in, 96000, 48000, out);
        const double d = fitDb(out, 48000.0 - f, 48000) - fitDb(in, f, 96000);
        std::snprintf(name, sizeof name, "96 -> 48 kHz: %.1f kHz tone (would fold to %.1f kHz) removed by", f / 1000, (48000.0 - f) / 1000);
        check(d < -80.0, name, d, "dB");
    }
    {   // and the top of the audible band is kept
        const auto in = sine(20000, 96000, 1.0); std::vector<float> out;
        Resampler::convert(in, 96000, 48000, out);
        const double d = fitDb(out, 20000, 48000) - fitDb(in, 20000, 96000);
        check(std::fabs(d) < 0.01, "96 -> 48 kHz: a 20 kHz tone keeps its level (error)", d, "dB");
    }
    // Up-conversion makes no images: 1 kHz at 22.05 kHz -> 48 kHz, nothing but the 1 kHz line.
    {
        const auto in = sine(1000, 22050, 1.0); std::vector<float> out;
        Resampler::convert(in, 22050, 48000, out);
        // remove the best-fit 1 kHz sine, what is left is distortion + images
        double sc = 0, ss = 0, cc = 0, sx = 0, cx = 0; const std::size_t a = 4000, b = out.size() - 4000;
        for (std::size_t i = a; i < b; ++i) {
            const double w = 2 * std::numbers::pi * 1000.0 * double(i) / 48000.0, s = std::sin(w), c = std::cos(w);
            ss += s * s; cc += c * c; sc += s * c; sx += s * out[i]; cx += c * out[i];
        }
        const double det = ss * cc - sc * sc, A = (sx * cc - cx * sc) / det, B = (cx * ss - sx * sc) / det;
        double e = 0, sig = 0;
        for (std::size_t i = a; i < b; ++i) {
            const double w = 2 * std::numbers::pi * 1000.0 * double(i) / 48000.0;
            const double fit = A * std::sin(w) + B * std::cos(w);
            e += std::pow(out[i] - fit, 2); sig += fit * fit;
        }
        const double r = 10 * std::log10(e / sig);
        check(r < -90.0, "22.05 -> 48 kHz: everything except the 1 kHz tone (images, noise)", r, "dB");
    }
    // Round trip 44.1 -> 48 -> 44.1 keeps a mix-like signal.
    {
        std::vector<float> in(44100 * 2, 0.0f);
        const auto a = sine(440, 44100, 2.0, 0.3), b = sine(3000, 44100, 2.0, 0.2), c = sine(12000, 44100, 2.0, 0.1);
        for (std::size_t i = 0; i < in.size(); ++i) in[i] = a[i] + b[i] + c[i];
        std::vector<float> mid, back;
        Resampler::convert(in, 44100, 48000, mid); Resampler::convert(mid, 48000, 44100, back);
        double e = 0, s = 0; const std::size_t n = std::min(in.size(), back.size());
        for (std::size_t i = 8000; i + 8000 < n; ++i) { e += std::pow(in[i] - back[i], 2); s += double(in[i]) * in[i]; }
        const double r = 10 * std::log10(e / s);
        check(r < -85.0, "44.1 -> 48 -> 44.1 kHz round trip: error", r, "dB");
    }
    // Stereo file, both channels kept apart.
    {
        wav::Audio a; a.sampleRate = 44100; a.ch[0] = sine(1000, 44100, 0.5); a.ch[1] = sine(2000, 44100, 0.5, 0.25);
        wav::Audio o; const std::string e = Resampler::convert(a, 48000, o);
        const double dl = fitDb(o.ch[0], 1000, 48000) - fitDb(a.ch[0], 1000, 44100), dr = fitDb(o.ch[1], 2000, 48000) - fitDb(a.ch[1], 2000, 44100);
        check(e.empty() && o.sampleRate == 48000 && std::fabs(dl) < 0.005 && std::fabs(dr) < 0.005, "Stereo file 44.1 -> 48 kHz: both channels keep their level (worst error)", std::max(std::fabs(dl), std::fabs(dr)), "dB");
    }
    // Awkward ratio is refused with a message, not guessed.
    {
        std::vector<float> in(1000, 0.0f), out;
        const std::string e = Resampler::convert(in, 44100, 47999, out);
        check(!e.empty(), "Awkward rate pair (44100 -> 47999) is refused with a message", e.empty() ? 0.0 : 1.0);
        std::printf("        message: %s\n", e.c_str());
    }
    // Speed (this machine, not the rig).
    {
        const auto in = sine(1000, 44100, 30.0); std::vector<float> out;
        const auto t0 = std::chrono::steady_clock::now();
        Resampler::convert(in, 44100, 48000, out);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("[BENCH] 44.1 -> 48 kHz, 30 s mono: %.3f s  (%.0fx real-time, this machine)\n", sec, 30.0 / sec);
    }
}
