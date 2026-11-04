// =============================================================================
//  dsp/resampler.hpp — sample-rate converter for loading stems (NON-RT only).
//
//  Polyphase windowed-sinc (Kaiser window, beta 9). The rates are reduced to a
//  whole-number ratio L/M (44100 -> 48000 is 160/147), one filter row is built
//  per output phase, and every row is normalised to exactly 1.0 at DC.
//  The filter is centred: no delay, output sample n sits at exactly n*src/dst.
//
//  Cutoff is 97.5 % of the lower of the two Nyquist frequencies, with 64 zero
//  crossings on each side, so down-conversion removes what would alias.
//  Used offline while a stem is loaded — it never runs on the audio thread.
//
//  Unsupported: rate pairs whose reduced ratio needs more than 4096 phases
//  (the function returns an error string instead of guessing).
// =============================================================================
#pragma once
#include "hc/io/wav.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <numbers>
#include <string>
#include <vector>

namespace hc {

class Resampler {
public:
    static constexpr int kZeroCrossings = 64;
    static constexpr double kCutoff = 0.975;
    static constexpr double kKaiserBeta = 9.0;
    static constexpr std::uint64_t kMaxPhases = 4096;

    // Converts one channel. Returns "" on success.
    static std::string convert(const std::vector<float>& in, double srcRate, double dstRate, std::vector<float>& out) {
        if (!(srcRate > 0.0) || !(dstRate > 0.0)) return "sample rate must be above 0";
        const std::uint64_t s = static_cast<std::uint64_t>(std::llround(srcRate));
        const std::uint64_t d = static_cast<std::uint64_t>(std::llround(dstRate));
        if (s == 0 || d == 0) return "sample rate must be above 0";
        const std::uint64_t g = std::gcd(s, d);
        const std::uint64_t M = s / g, L = d / g;   // input step per output sample = M / L
        if (L > kMaxPhases) return "cannot convert " + std::to_string(s) + " Hz to " + std::to_string(d) + " Hz (ratio too awkward)";
        if (M == L) { out = in; return {}; }

        const double c = kCutoff * std::min(1.0, static_cast<double>(d) / static_cast<double>(s));
        const int K = static_cast<int>(std::ceil(kZeroCrossings / c));   // taps on each side of the centre
        const int taps = 2 * K;
        const double pi = std::numbers::pi;

        // One row per output phase.
        std::vector<float> table(static_cast<std::size_t>(L) * static_cast<std::size_t>(taps));
        const double i0b = besselI0(kKaiserBeta);
        for (std::uint64_t ph = 0; ph < L; ++ph) {
            const double frac = static_cast<double>(ph) / static_cast<double>(L);
            double sum = 0.0;
            std::vector<double> row(static_cast<std::size_t>(taps));
            for (int k = 0; k < taps; ++k) {
                const double x = static_cast<double>(k - K + 1) - frac;       // source samples from the output instant
                const double r = x / static_cast<double>(K);
                double w = 0.0;
                if (std::fabs(r) < 1.0) w = besselI0(kKaiserBeta * std::sqrt(1.0 - r * r)) / i0b;
                const double a = pi * c * x;
                const double sinc = std::fabs(a) < 1e-12 ? 1.0 : std::sin(a) / a;
                row[static_cast<std::size_t>(k)] = c * sinc * w;
                sum += row[static_cast<std::size_t>(k)];
            }
            for (int k = 0; k < taps; ++k)
                table[static_cast<std::size_t>(ph) * static_cast<std::size_t>(taps) + static_cast<std::size_t>(k)] =
                    static_cast<float>(row[static_cast<std::size_t>(k)] / sum);
        }

        const std::uint64_t nIn = in.size();
        const std::uint64_t nOut = static_cast<std::uint64_t>(std::llround(static_cast<double>(nIn) * static_cast<double>(d) / static_cast<double>(s)));
        out.assign(static_cast<std::size_t>(nOut), 0.0f);

        // Zero-padded copy so the inner loop never needs bounds checks.
        std::vector<float> pad(static_cast<std::size_t>(nIn) + static_cast<std::size_t>(taps) * 2, 0.0f);
        std::copy(in.begin(), in.end(), pad.begin() + taps);

        for (std::uint64_t n = 0; n < nOut; ++n) {
            const std::uint64_t pos = n * M;
            const std::uint64_t i0 = pos / L, ph = pos % L;
            // first tap reads source sample (i0 - K + 1)
            const float* x = pad.data() + (static_cast<std::int64_t>(i0) - K + 1 + taps);
            const float* h = table.data() + static_cast<std::size_t>(ph) * static_cast<std::size_t>(taps);
            float acc = 0.0f;
            for (int k = 0; k < taps; ++k) acc += x[k] * h[k];
            out[static_cast<std::size_t>(n)] = acc;
        }
        return {};
    }

    // Converts a whole stereo file. Returns "" on success.
    static std::string convert(const wav::Audio& in, double dstRate, wav::Audio& out) {
        wav::Audio r;
        r.sampleRate = dstRate;
        for (int c = 0; c < 2; ++c)
            if (auto e = convert(in.ch[c], in.sampleRate, dstRate, r.ch[c]); !e.empty()) return e;
        out = std::move(r);
        return {};
    }

private:
    static double besselI0(double x) {
        double sum = 1.0, term = 1.0;
        const double q = x * x * 0.25;
        for (int k = 1; k < 60; ++k) {
            term *= q / (static_cast<double>(k) * static_cast<double>(k));
            sum += term;
            if (term < sum * 1e-17) break;
        }
        return sum;
    }
};

} // namespace hc
