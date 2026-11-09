// =============================================================================
//  midi/trackers.hpp — HEM STEM-TO-MIDI: the three note finders and the tempo finder.
//
//  MonoTracker  — one note at a time (bass, lead vocal, lead). YIN pitch (de Cheveigné &
//                 Kawahara 2002), computed with the FFT, then cut into notes on pitch changes,
//                 silences and re-attacks (a new pluck/word on the same pitch).
//  PolyTracker  — several notes at once (keys, guitar, strings, full song). Harmonic-sum
//                 pitch strength with "pick the strongest, take its harmonics out, repeat"
//                 (in the spirit of Klapuri 2003/2006), then per-pitch on/off tracking.
//  DrumTracker  — kick / snare / closed hat / open hat from energy jumps in three bands.
//  Tempo        — beat period from the self-similarity of the onset curve, refined over
//                 four beats; plus where beat 1 of the grid sits.
//
//  All of this runs offline (it never runs on an audio thread). Every guess it uses is in
//  midi_config.hpp.
// =============================================================================
#pragma once
#include "hc/dsp/fft.hpp"
#include "hc/midi/midi_config.hpp"
#include "hc/midi/smf.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <numeric>
#include <vector>

namespace hc::midi {

// ---- small helpers ---------------------------------------------------------------------------
inline double hzToMidi(double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }
inline double midiToHz(double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }
inline double toDb(double x) { return 10.0 * std::log10(std::max(x, 1e-20)); }   // x is a power

inline std::vector<float> hann(std::size_t n) {
    std::vector<float> w(n);
    for (std::size_t i = 0; i < n; ++i) w[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / double(n)));
    return w;
}

inline int velocityFromDb(double db, double peakDb) {
    const double t = std::clamp((db - (peakDb - kVelocityRangeDb)) / kVelocityRangeDb, 0.0, 1.0);
    return static_cast<int>(std::lround(kVelocityMin + t * (kVelocityMax - kVelocityMin)));
}

// Level (mean power, dB) of frames of length `win` centred every `hop` samples.
inline std::vector<double> frameLevels(const std::vector<float>& x, std::size_t hop, std::size_t win, std::size_t frames) {
    std::vector<double> cum(x.size() + 1, 0.0);
    for (std::size_t i = 0; i < x.size(); ++i) cum[i + 1] = cum[i] + double(x[i]) * x[i];
    std::vector<double> lv(frames);
    for (std::size_t f = 0; f < frames; ++f) {
        const std::int64_t c = std::int64_t(f * hop), h = std::int64_t(win / 2);
        const std::size_t a = std::size_t(std::clamp<std::int64_t>(c - h, 0, std::int64_t(x.size())));
        const std::size_t b = std::size_t(std::clamp<std::int64_t>(c + h, 0, std::int64_t(x.size())));
        lv[f] = toDb(b > a ? (cum[b] - cum[a]) / double(win) : 0.0);
    }
    return lv;
}

// Onset strength curve (positive log-magnitude change summed over bins in [loHz, hiHz]).
// Frame f is centred at sample f*hop. Used by every tracker and by the tempo finder.
struct Spectrogram {
    std::size_t hop = 0, fft = 0, bins = 0, frames = 0; double sr = 0, binHz = 0;
    std::vector<float> mag;   // frames x bins
    const float* frame(std::size_t f) const { return mag.data() + f * bins; }
};

inline Spectrogram spectrogram(const std::vector<float>& x, double sr, std::size_t fft, std::size_t hop) {
    Spectrogram s; s.hop = hop; s.fft = fft; s.bins = fft / 2 + 1; s.sr = sr; s.binHz = sr / double(fft);
    s.frames = x.empty() ? 0 : x.size() / hop + 1;
    s.mag.assign(s.frames * s.bins, 0.0f);
    RealFFT F(fft);
    const auto w = hann(fft);
    std::vector<float> buf(fft), re(s.bins), im(s.bins);
    const float norm = 4.0f / float(fft);   // full-scale sine -> about 1.0
    for (std::size_t f = 0; f < s.frames; ++f) {
        const std::int64_t start = std::int64_t(f * hop) - std::int64_t(fft / 2);
        for (std::size_t i = 0; i < fft; ++i) {
            const std::int64_t j = start + std::int64_t(i);
            buf[i] = (j >= 0 && j < std::int64_t(x.size())) ? x[std::size_t(j)] * w[i] : 0.0f;
        }
        F.forward(buf.data(), re.data(), im.data());
        float* m = s.mag.data() + f * s.bins;
        for (std::size_t k = 0; k < s.bins; ++k) m[k] = norm * std::sqrt(re[k] * re[k] + im[k] * im[k]);
    }
    return s;
}

inline std::vector<double> bandFlux(const Spectrogram& s, double loHz, double hiHz, double lambda = 1000.0) {
    std::vector<double> fl(s.frames, 0.0);
    const std::size_t k0 = std::size_t(std::max(1.0, std::floor(loHz / s.binHz)));
    const std::size_t k1 = std::min(s.bins - 1, std::size_t(std::ceil(hiHz / s.binHz)));
    for (std::size_t f = 1; f < s.frames; ++f) {
        const float* a = s.frame(f); const float* b = s.frame(f - 1);
        double sum = 0;
        for (std::size_t k = k0; k <= k1; ++k) {
            const double d = std::log1p(lambda * a[k]) - std::log1p(lambda * b[k]);
            if (d > 0) sum += d;
        }
        fl[f] = sum;
    }
    return fl;
}

inline std::vector<double> bandPower(const Spectrogram& s, double loHz, double hiHz) {
    std::vector<double> p(s.frames, 0.0);
    const std::size_t k0 = std::size_t(std::max(1.0, std::floor(loHz / s.binHz)));
    const std::size_t k1 = std::min(s.bins - 1, std::size_t(std::ceil(hiHz / s.binHz)));
    for (std::size_t f = 0; f < s.frames; ++f) {
        const float* a = s.frame(f); double sum = 0;
        for (std::size_t k = k0; k <= k1; ++k) sum += double(a[k]) * a[k];
        p[f] = sum;
    }
    return p;
}

// Peak picking with an adaptive threshold: median of the surrounding window * k + delta*max.
inline std::vector<std::size_t> pickPeaks(const std::vector<double>& v, std::size_t halfWin, double k, double deltaFrac, std::size_t minGap) {
    std::vector<std::size_t> out;
    if (v.empty()) return out;
    const double mx = *std::max_element(v.begin(), v.end());
    if (mx <= 0) return out;
    std::vector<double> tmp;
    for (std::size_t i = 1; i + 1 < v.size(); ++i) {
        if (!(v[i] > v[i - 1] && v[i] >= v[i + 1])) continue;
        const std::size_t a = i > halfWin ? i - halfWin : 0, b = std::min(v.size(), i + halfWin + 1);
        bool isMax = true;
        const std::size_t la = i > minGap / 2 ? i - minGap / 2 : 0, lb = std::min(v.size(), i + minGap / 2 + 1);
        for (std::size_t j = la; j < lb; ++j) if (v[j] > v[i]) { isMax = false; break; }
        if (!isMax) continue;
        tmp.assign(v.begin() + std::ptrdiff_t(a), v.begin() + std::ptrdiff_t(b));
        std::nth_element(tmp.begin(), tmp.begin() + std::ptrdiff_t(tmp.size() / 2), tmp.end());
        const double thr = tmp[tmp.size() / 2] * k + deltaFrac * mx;
        if (v[i] <= thr) continue;
        if (!out.empty() && i - out.back() < minGap) { if (v[i] > v[out.back()]) out.back() = i; continue; }
        out.push_back(i);
    }
    return out;
}

// =============================================================================================
//  MonoTracker
// =============================================================================================
struct MonoSettings { int lowNote = kBassLow, highNote = kBassHigh; int sensitivity = kSensitivityDefault; int shortestMs = kShortestNoteDefaultMs; };

struct PitchFrame { double midi = 0; double aperiodicity = 1; double levelDb = -200; bool voiced = false; };

// YIN on a mono signal. Returns one PitchFrame per hop, frame f centred at f*hop.
inline std::vector<PitchFrame> yinTrack(const std::vector<float>& x, double sr, std::size_t hop, double fMin, double fMax, double threshold) {
    const std::size_t tauMax = std::size_t(std::ceil(sr / fMin)) + 2;
    const std::size_t tauMin = std::max<std::size_t>(2, std::size_t(std::floor(sr / fMax)));
    std::size_t W = 1; while (W < std::max<std::size_t>(tauMax, std::size_t(0.032 * sr))) W <<= 1;   // window >= one longest period, >= 32 ms
    const std::size_t L = W + tauMax;
    std::size_t N = 1; while (N < W + L) N <<= 1;
    RealFFT F(N);
    const std::size_t bins = N / 2 + 1;
    std::vector<float> a(N), b(N), ar(bins), ai(bins), br(bins), bi(bins), cr(bins), ci(bins), r(N);
    std::vector<double> d(tauMax + 1), cm(tauMax + 1), e2c(L + 1);
    const std::size_t frames = x.empty() ? 0 : x.size() / hop + 1;
    std::vector<PitchFrame> out(frames);
    for (std::size_t f = 0; f < frames; ++f) {
        const std::int64_t start = std::int64_t(f * hop) - std::int64_t(W / 2);
        for (std::size_t i = 0; i < N; ++i) {
            const std::int64_t j = start + std::int64_t(i);
            const float v = (i < L && j >= 0 && j < std::int64_t(x.size())) ? x[std::size_t(j)] : 0.0f;
            b[i] = v; a[i] = i < W ? v : 0.0f;
        }
        // r(tau) = sum_{j<W} x_j x_{j+tau}  (cross-correlation via FFT)
        F.forward(a.data(), ar.data(), ai.data());
        F.forward(b.data(), br.data(), bi.data());
        for (std::size_t k = 0; k < bins; ++k) { cr[k] = ar[k] * br[k] + ai[k] * bi[k]; ci[k] = ar[k] * bi[k] - ai[k] * br[k]; }
        F.inverse(cr.data(), ci.data(), r.data());
        e2c[0] = 0; for (std::size_t i = 0; i < L; ++i) e2c[i + 1] = e2c[i] + double(b[i]) * b[i];
        const double e1 = e2c[W];
        PitchFrame& pf = out[f];
        pf.levelDb = toDb(e1 / double(W));
        if (e1 <= 1e-12) continue;
        d[0] = 0; cm[0] = 1; double run = 0;
        for (std::size_t t = 1; t <= tauMax; ++t) {
            const double e2 = e2c[t + W] - e2c[t];
            d[t] = std::max(0.0, e1 + e2 - 2.0 * double(r[t]));
            run += d[t];
            cm[t] = run > 0 ? d[t] * double(t) / run : 1.0;
        }
        std::size_t best = 0;
        for (std::size_t t = tauMin; t + 1 <= tauMax; ++t)
            if (cm[t] < threshold) { while (t + 1 < tauMax && cm[t + 1] < cm[t]) ++t; best = t; break; }
        if (best == 0) {   // nothing under the threshold: keep the global minimum as "unvoiced" info
            best = tauMin; for (std::size_t t = tauMin; t < tauMax; ++t) if (cm[t] < cm[best]) best = t;
            pf.aperiodicity = cm[best]; pf.voiced = false;
        } else { pf.aperiodicity = cm[best]; pf.voiced = true; }
        double tau = double(best);
        if (best > 1 && best + 1 <= tauMax) {
            const double y0 = cm[best - 1], y1 = cm[best], y2 = cm[best + 1], den = y0 - 2 * y1 + y2;
            if (std::fabs(den) > 1e-12) tau += std::clamp(0.5 * (y0 - y2) / den, -1.0, 1.0);
        }
        pf.midi = hzToMidi(sr / tau);
    }
    return out;
}

// Rising-level onsets (frame indices) for re-attack detection.
inline std::vector<std::size_t> levelOnsets(const std::vector<double>& lv, std::size_t span, double riseDb) {
    std::vector<double> rise(lv.size(), 0.0);
    for (std::size_t i = span; i < lv.size(); ++i) {
        double mn = lv[i]; for (std::size_t j = i - span; j < i; ++j) mn = std::min(mn, lv[j]);
        rise[i] = std::max(0.0, lv[i] - mn);
    }
    std::vector<std::size_t> on;
    for (std::size_t i = 1; i + 1 < rise.size(); ++i)
        if (rise[i] >= riseDb && rise[i] >= rise[i - 1] && rise[i] > rise[i + 1]) {
            if (!on.empty() && i - on.back() < span * 2) continue;
            // the note really starts where the level was lowest just before the rise
            std::size_t m = i; for (std::size_t j = i - span; j < i; ++j) if (lv[j] < lv[m]) m = j;
            on.push_back(m + 1 <= i ? m + 1 : i);
        }
    return on;
}

inline std::vector<Note> monoTranscribe(const std::vector<float>& x, double sr, const MonoSettings& ms) {
    const Tuned tn = tuned(ms.sensitivity);
    const std::size_t hop = std::max<std::size_t>(1, std::size_t(std::lround(kHopSeconds * sr)));
    const double fMin = midiToHz(ms.lowNote - 0.5), fMax = midiToHz(ms.highNote + 0.5);
    auto pf = yinTrack(x, sr, hop, fMin, fMax, tn.yinThreshold);
    if (pf.empty()) return {};
    const std::size_t lvWin = std::size_t(0.020 * sr);
    const auto lv = frameLevels(x, hop, lvWin, pf.size());
    const double peak = *std::max_element(lv.begin(), lv.end());
    const double gate = std::max(peak - tn.gateBelowPeakDb, tn.absoluteFloorDb);
    for (std::size_t f = 0; f < pf.size(); ++f) {
        auto& p = pf[f];
        if (p.voiced && (pf[f].levelDb < gate || lv[f] < gate - 6.0 || p.midi < ms.lowNote - 0.5 || p.midi > ms.highNote + 0.5)) p.voiced = false;
    }
    // Median smoothing of pitch over voiced neighbours.
    std::vector<double> pm(pf.size(), 0.0);
    for (std::size_t f = 0; f < pf.size(); ++f) {
        if (!pf[f].voiced) continue;
        double v[16]; int n = 0; const int h = kMedianFrames / 2;
        for (int k = -h; k <= h; ++k) {
            const std::int64_t j = std::int64_t(f) + k;
            if (j >= 0 && j < std::int64_t(pf.size()) && pf[std::size_t(j)].voiced) v[n++] = pf[std::size_t(j)].midi;
        }
        std::sort(v, v + n); pm[f] = v[n / 2];
    }
    const std::size_t span = std::max<std::size_t>(2, std::size_t(std::lround(kReattackWindowS / kHopSeconds)));
    const auto ons = levelOnsets(lv, span, kReattackRiseDb);
    std::vector<char> isOnset(pf.size(), 0);
    for (auto o : ons) isOnset[o] = 1;

    const double tHop = double(hop) / sr;
    const std::size_t minFrames = std::max<std::size_t>(1, std::size_t(std::lround(ms.shortestMs / 1000.0 / tHop)));
    std::vector<Note> notes;
    struct Cur { bool on = false; std::size_t start = 0; std::vector<double> p; double ref = 0; } cur;
    auto close = [&](std::size_t endFrame) {
        if (cur.on && endFrame > cur.start && endFrame - cur.start >= minFrames) {
            auto p = cur.p; std::nth_element(p.begin(), p.begin() + std::ptrdiff_t(p.size() / 2), p.end());
            double pk = -300; for (std::size_t f = cur.start; f < endFrame; ++f) pk = std::max(pk, lv[f]);
            notes.push_back({cur.start * tHop, endFrame * tHop, int(std::lround(p[p.size() / 2])), velocityFromDb(pk, peak)});
        }
        cur = {};
    };
    auto open = [&](std::size_t f) { cur.on = true; cur.start = f; cur.p.clear(); cur.p.push_back(pm[f]); cur.ref = pm[f]; };
    std::size_t unvoicedRun = 0, changeRun = 0, changeStart = 0;
    for (std::size_t f = 0; f < pf.size(); ++f) {
        const bool v = pf[f].voiced;
        if (!cur.on) { if (v) { open(f); unvoicedRun = changeRun = 0; } continue; }
        if (!v) {
            if (++unvoicedRun >= 2) { close(f + 1 - unvoicedRun); unvoicedRun = 0; changeRun = 0; }
            continue;
        }
        unvoicedRun = 0;
        if (isOnset[f] && f - cur.start >= minFrames) { close(f); open(f); changeRun = 0; continue; }
        if (std::fabs(pm[f] - cur.ref) > kPitchHoldSemis) {
            if (changeRun++ == 0) changeStart = f;
            if (changeRun >= std::size_t(kPitchChangeFrames)) { close(changeStart); open(changeStart); for (std::size_t j = changeStart + 1; j <= f; ++j) cur.p.push_back(pm[j]); cur.ref = pm[f]; changeRun = 0; }
            continue;
        }
        changeRun = 0;
        cur.p.push_back(pm[f]);
        // reference = running median of the first part of the note, then frozen-ish mean
        if (cur.p.size() <= 8) { auto t = cur.p; std::nth_element(t.begin(), t.begin() + std::ptrdiff_t(t.size() / 2), t.end()); cur.ref = t[t.size() / 2]; }
        else cur.ref += (pm[f] - cur.ref) * 0.02;
    }
    close(pf.size());
    for (auto& n : notes) n.pitch = std::clamp(n.pitch, ms.lowNote, ms.highNote);
    return notes;
}

// =============================================================================================
//  PolyTracker
// =============================================================================================
struct PolySettings { int lowNote = kChordLow, highNote = kChordHigh; int sensitivity = kSensitivityDefault; int shortestMs = kShortestNoteDefaultMs; };

inline std::vector<Note> polyTranscribe(const std::vector<float>& x, double sr, const PolySettings& ps) {
    const Tuned tn = tuned(ps.sensitivity);
    const std::size_t hop = std::max<std::size_t>(1, std::size_t(std::lround(2 * kHopSeconds * sr)));
    std::size_t fft = 1; while (double(fft) < 0.25 * sr) fft <<= 1;      // ~256 ms window (4096 at 16 kHz)
    const Spectrogram S = spectrogram(x, sr, fft, hop);
    if (S.frames == 0) return {};
    const double tHop = double(hop) / sr;
    const int P0 = ps.lowNote, P1 = ps.highNote, NP = P1 - P0 + 1;
    const double maxHz = std::min(7000.0, 0.45 * sr);

    // Short-window onsets for timing (the long window above is for pitch only).
    std::size_t sfft = 1; while (double(sfft) < 0.021 * sr) sfft <<= 1;          // ~32 ms at 16 kHz
    const Spectrogram So = spectrogram(x, sr, sfft, hop);
    const auto onFlux = bandFlux(So, 30.0, std::min(8000.0, 0.45 * sr));
    const auto onsets = pickPeaks(onFlux, std::size_t(0.1 / tHop), kPolyOnsetK, kPolyOnsetDelta, std::max<std::size_t>(1, std::size_t(0.04 / tHop)));
    std::vector<char> isOnset(S.frames, 0); for (auto o : onsets) if (o < S.frames) isOnset[o] = 1;

    const auto lv = frameLevels(x, hop, std::size_t(0.046 * sr), S.frames);
    const double peak = *std::max_element(lv.begin(), lv.end());
    const double gate = std::max(peak - tn.gateBelowPeakDb, tn.absoluteFloorDb);

    // Precompute, for each pitch and harmonic, the bin range to search.
    struct HR { std::size_t a = 0, b = 0; double w = 0; };
    std::vector<HR> hr(std::size_t(NP) * kPolyHarmonics);
    for (int p = 0; p < NP; ++p) {
        const double f0 = midiToHz(P0 + p);
        for (int h = 1; h <= kPolyHarmonics; ++h) {
            const double fh = f0 * h;
            HR r;
            if (fh < maxHz) {
                const double lo = fh * std::pow(2.0, -0.4 / 12.0) / S.binHz, hi = fh * std::pow(2.0, 0.4 / 12.0) / S.binHz, c = fh / S.binHz;
                r.a = std::size_t(std::max(1.0, std::min(std::floor(lo), std::round(c) - 1)));
                r.b = std::min(S.bins - 1, std::size_t(std::max(std::ceil(hi), std::round(c) + 1)));
                r.w = std::pow(kPolyHarmonicDecay, h - 1);
            }
            hr[std::size_t(p) * kPolyHarmonics + std::size_t(h - 1)] = r;
        }
    }

    std::vector<float> act(std::size_t(NP) * S.frames, 0.0f);    // picked salience per pitch per frame
    std::vector<float> Y(S.bins), tmp;
    std::vector<double> sal(static_cast<std::size_t>(NP));
    std::vector<char> used(static_cast<std::size_t>(NP));
    for (std::size_t f = 0; f < S.frames; ++f) {
        if (lv[f] < gate) continue;
        const float* m = S.frame(f);
        for (std::size_t k = 0; k < S.bins; ++k) Y[k] = std::pow(m[k], kPolyCompress);   // compressed magnitude
        tmp.assign(Y.begin(), Y.end());
        std::nth_element(tmp.begin(), tmp.begin() + std::ptrdiff_t(tmp.size() / 2), tmp.end());
        const float floor = 3.0f * tmp[tmp.size() / 2] + 1e-4f;
        std::fill(used.begin(), used.end(), 0);
        double first = 0;
        for (int it = 0; it < kPolyMaxNotes; ++it) {
            int bestP = -1; double bestS = 0;
            for (int p = 0; p < NP; ++p) {
                if (used[std::size_t(p)]) continue;
                double s = 0; float a1 = 0, a2 = 0;
                for (int h = 0; h < kPolyHarmonics; ++h) {
                    const HR& r = hr[std::size_t(p) * kPolyHarmonics + std::size_t(h)];
                    if (r.w == 0) break;
                    // only a real spectral peak inside the range counts (not the skirt of a neighbour's peak)
                    std::size_t kk = r.a; for (std::size_t k = r.a; k <= r.b; ++k) if (Y[k] > Y[kk]) kk = k;
                    const bool isPeak = kk > 0 && kk + 1 < S.bins && Y[kk] >= Y[kk - 1] && Y[kk] >= Y[kk + 1] && kk != r.a && kk != r.b;
                    const float v = isPeak ? std::max(0.0f, Y[kk] - floor) : 0.0f;
                    if (h == 0) a1 = v; else if (h == 1) a2 = v;
                    s += r.w * v;
                }
                if (a1 <= 0 && (P0 + p >= kPolyNeedFundamentalFrom || a2 <= 0)) continue;   // needs its fundamental (low notes: or 2nd harmonic)
                sal[std::size_t(p)] = s;
                if (s > bestS) { bestS = s; bestP = p; }
            }
            if (bestP < 0) break;
            if (it == 0) first = bestS;
            else if (bestS < tn.polyRelThreshold * first) break;
            used[std::size_t(bestP)] = 1;
            act[std::size_t(bestP) * S.frames + f] = static_cast<float>(bestS);
            // Take this note's share out of each harmonic peak (spectral-smoothness estimate).
            float A[kPolyHarmonics] = {}; std::size_t K[kPolyHarmonics] = {};
            int nh = 0;
            for (int h = 0; h < kPolyHarmonics; ++h) {
                const HR& r = hr[std::size_t(bestP) * kPolyHarmonics + std::size_t(h)];
                if (r.w == 0) break;
                std::size_t kk = r.a; for (std::size_t k = r.a; k <= r.b; ++k) if (Y[k] > Y[kk]) kk = k;
                A[h] = Y[kk]; K[h] = kk; nh = h + 1;
            }
            for (int h = 0; h < nh; ++h) {
                const float lo = h > 0 ? A[h - 1] : A[h], hi = h + 1 < nh ? A[h + 1] : A[h];
                const float est = std::min(A[h], 0.5f * (lo + hi));   // what a smooth harmonic series would have here
                const float keep = A[h] > 0 ? (A[h] - est) / A[h] : 0.0f;
                const std::size_t k0 = K[h] >= 2 ? K[h] - 2 : 0, k1 = std::min(S.bins - 1, K[h] + 2);
                for (std::size_t k = k0; k <= k1; ++k) Y[k] *= keep;
            }
        }
    }

    // Per-pitch on/off with gap merge, then cut at re-attacks and snap starts to real onsets.
    const std::size_t gapFrames = std::size_t(std::lround(kPolyGapMergeS / tHop));
    const std::size_t halfWin = std::size_t(std::lround(0.5 * double(fft) / sr / tHop));
    const std::size_t snapBack = std::size_t(std::lround(0.040 / tHop)), snapFwd = halfWin;   // the long window sees a note early
    double maxAct = 0; for (float v : act) maxAct = std::max(maxAct, double(v));
    const double maxActDb = 20.0 * std::log10(std::max(maxAct, 1e-12));
    // power near a pitch's 1st and 2nd harmonic in the SHORT window (good timing, rough pitch)
    auto pitchPower = [&](int midi, std::size_t f) {
        double sum = 0; const float* m = So.frame(f);
        for (int h = 1; h <= 2; ++h) {
            const double c = midiToHz(midi) * h / So.binHz;
            const std::size_t a = std::size_t(std::max(1.0, std::floor(c - 1))), b = std::min(So.bins - 1, std::size_t(std::ceil(c + 1)));
            for (std::size_t k = a; k <= b; ++k) sum += double(m[k]) * m[k];
        }
        return sum;
    };
    const std::size_t rePre = 4, rePost = 3;
    std::vector<Note> notes;
    for (int p = 0; p < NP; ++p) {
        const float* a = act.data() + std::size_t(p) * S.frames;
        std::size_t f = 0;
        while (f < S.frames) {
            if (a[f] <= 0) { ++f; continue; }
            std::size_t s = f, e = f; double pk = a[f]; std::size_t gap = 0;
            for (std::size_t g = f + 1; g < S.frames; ++g) {
                if (a[g] > 0) { e = g; gap = 0; pk = std::max(pk, double(a[g])); }
                else if (++gap > gapFrames) break;
            }
            f = e + 1;
            // start: nearest onset (the long window sees the note up to half a window early)
            // with no onset close by, the note became visible about a quarter window before it started
            std::size_t best = std::min(e, s + halfWin / 2), bestD = std::numeric_limits<std::size_t>::max();
            const std::size_t lo = s > snapBack ? s - snapBack : 0, hi = std::min(S.frames - 1, s + snapFwd);
            for (std::size_t j = lo; j <= hi; ++j) if (isOnset[j]) { const std::size_t d = j > s ? j - s : s - j; if (d < bestD) { bestD = d; best = j; } }
            // cut points: onsets inside the note where this pitch's own energy jumps (a new strike)
            // seen late (it was masked by louder notes at first)? look back for an onset where this
            // pitch's own energy jumped, up to kPolyLookBackS earlier
            if (bestD == std::numeric_limits<std::size_t>::max()) {
                const std::size_t back = std::size_t(std::lround(kPolyLookBackS / tHop));
                for (std::size_t j = s; j-- > (s > back ? s - back : 0);) {
                    if (!isOnset[j]) continue;
                    double before = std::numeric_limits<double>::max(), after = 0;
                    for (std::size_t q = (j > rePre ? j - rePre : 0); q < j; ++q) before = std::min(before, pitchPower(P0 + p, q));
                    for (std::size_t q = j; q < std::min(So.frames, j + rePost); ++q) after = std::max(after, pitchPower(P0 + p, q));
                    if (toDb(after) - toDb(before) >= kPolyReattackDb) { best = j; break; }
                }
            }
            std::vector<std::size_t> cuts = {best};
            for (std::size_t o : onsets) {
                if (o <= cuts.back() + std::max<std::size_t>(1, std::size_t(std::lround(ps.shortestMs / 1000.0 / tHop))) || o + halfWin / 2 > e) continue;
                double before = std::numeric_limits<double>::max(), after = 0;
                for (std::size_t j = (o > rePre ? o - rePre : 0); j < o; ++j) before = std::min(before, pitchPower(P0 + p, j));
                for (std::size_t j = o; j < std::min(So.frames, o + rePost); ++j) after = std::max(after, pitchPower(P0 + p, j));
                if (toDb(after) - toDb(before) >= kPolyReattackDb) cuts.push_back(o);
            }
            // the long window also lingers after a note stops: end at the last frame - quarter window
            const double lastT = std::max(cuts.back() * tHop + tHop, (e + 1) * tHop - 0.25 * double(fft) / sr);
            for (std::size_t c = 0; c < cuts.size(); ++c) {
                const double st = cuts[c] * tHop, en = c + 1 < cuts.size() ? cuts[c + 1] * tHop : lastT;
                if (en - st >= ps.shortestMs / 1000.0 - 1e-9)
                    notes.push_back({st, en, P0 + p, velocityFromDb(20.0 * std::log10(std::max(pk, 1e-12)), maxActDb)});
            }
        }
    }
    // the same pitch can't overlap itself: join near-duplicates, otherwise end the first at the second
    std::sort(notes.begin(), notes.end(), [](const Note& a1, const Note& a2) { return a1.pitch < a2.pitch || (a1.pitch == a2.pitch && a1.start < a2.start); });
    std::vector<Note> clean;
    for (const auto& n : notes) {
        if (!clean.empty() && clean.back().pitch == n.pitch && n.start < clean.back().end) {
            if (n.start - clean.back().start < ps.shortestMs / 1000.0) { clean.back().end = std::max(clean.back().end, n.end); clean.back().velocity = std::max(clean.back().velocity, n.velocity); continue; }
            clean.back().end = n.start;
        }
        clean.push_back(n);
    }
    std::sort(clean.begin(), clean.end(), [](const Note& a1, const Note& a2) { return a1.start < a2.start || (a1.start == a2.start && a1.pitch < a2.pitch); });
    return clean;
}

// =============================================================================================
//  DrumTracker
// =============================================================================================
struct DrumSettings { int sensitivity = kSensitivityDefault; };

// Drums by NMF (non-negative matrix factorisation, in the spirit of Dittmar & Gärtner 2014):
// the spectrogram (48 log-spaced bands) is explained as  sum over parts of  shape x loudness-over-time.
// Parts 0..2 start as generic kick / snare / hat shapes and are allowed to adapt to this kit;
// two extra free parts soak up everything else (toms, cymbals, bleed). Hits are the sudden rises
// in each drum part's loudness curve. Because parts ADD, a hat played with a kick is still a hat.
inline constexpr int kNmfBands = 48;
inline constexpr int kNmfDrums = 3, kNmfExtra = 2, kNmfParts = kNmfDrums + kNmfExtra;

inline double drumShape(int d, double hz) {   // generic starting shapes                                   // ASSUMED
    const double l = std::log2(hz);
    auto bump = [&](double centreHz, double widthOct) { const double z = (l - std::log2(centreHz)) / widthOct; return std::exp(-0.5 * z * z); };
    switch (d) {
        case 0: return bump(60, 0.8) + 0.08 * bump(3000, 1.0);                       // kick: low boom + a little click
        case 1: return 0.7 * bump(200, 0.6) + bump(3000, 1.4);                       // snare: body + wide wire noise
        case 2: return bump(9000, 0.8);                                              // hat
        case 3: return bump(500, 2.0);                                               // free part (mid)
        default: return bump(1500, 3.0);                                             // free part (broad)
    }
}

inline std::vector<Note> drumTranscribe(const std::vector<float>& x, double sr, const DrumSettings& ds) {
    const Tuned tn = tuned(ds.sensitivity);
    const std::size_t hop = std::max<std::size_t>(1, std::size_t(std::lround(kHopSeconds * sr)));
    std::size_t fft = 1; while (double(fft) < 0.021 * sr) fft <<= 1;     // ~23 ms (1024 at 44.1/48 kHz)
    const Spectrogram S = spectrogram(x, sr, fft, hop);
    if (S.frames < 8) return {};
    const double tHop = double(hop) / sr;
    const std::size_t T = S.frames;
    // 1. 48 log bands, 30 Hz .. min(16 kHz, Nyquist)
    const double fLo = 30.0, fHi = std::min(16000.0, 0.47 * sr);
    std::vector<std::size_t> edge(kNmfBands + 1);
    for (int b = 0; b <= kNmfBands; ++b) edge[std::size_t(b)] = std::min(S.bins - 1, std::size_t(std::lround(fLo * std::pow(fHi / fLo, double(b) / kNmfBands) / S.binHz)));
    for (int b = 1; b <= kNmfBands; ++b) edge[std::size_t(b)] = std::max(edge[std::size_t(b)], edge[std::size_t(b - 1)] + 1);
    std::vector<float> V(std::size_t(kNmfBands) * T);       // band-major: V[b*T + t]
    double vmax = 0;
    for (std::size_t t = 0; t < T; ++t) {
        const float* m = S.frame(t);
        for (int b = 0; b < kNmfBands; ++b) {
            double sum = 0; for (std::size_t k = edge[std::size_t(b)]; k < std::min(S.bins, edge[std::size_t(b + 1)]); ++k) sum += m[k];
            V[std::size_t(b) * T + t] = float(sum); vmax = std::max(vmax, sum);
        }
    }
    if (vmax <= 0) return {};
    const float eps = float(vmax * 1e-9);
    // frame loudness for the gates
    const std::vector<double> frameDb = frameLevels(x, hop, fft, T);   // mean power in dBFS (same scale as the other finders)
    const double peakDb = *std::max_element(frameDb.begin(), frameDb.end());
    // 2. NMF, KL divergence, multiplicative updates
    std::vector<double> W(std::size_t(kNmfBands) * kNmfParts), W0;
    for (int k = 0; k < kNmfParts; ++k) {
        double n = 0;
        for (int b = 0; b < kNmfBands; ++b) { const double hz = fLo * std::pow(fHi / fLo, (b + 0.5) / kNmfBands); W[std::size_t(b) * kNmfParts + std::size_t(k)] = drumShape(k, hz) + 1e-3; n += W[std::size_t(b) * kNmfParts + std::size_t(k)]; }
        for (int b = 0; b < kNmfBands; ++b) W[std::size_t(b) * kNmfParts + std::size_t(k)] /= n;
    }
    W0 = W;
    std::vector<double> H(std::size_t(kNmfParts) * T, double(vmax) * 0.1), R(std::size_t(kNmfBands) * T);
    for (int it = 0; it < kNmfIterations; ++it) {
        for (int b = 0; b < kNmfBands; ++b) for (std::size_t t = 0; t < T; ++t) {
            double v = 0; for (int k = 0; k < kNmfParts; ++k) v += W[std::size_t(b) * kNmfParts + std::size_t(k)] * H[std::size_t(k) * T + t];
            R[std::size_t(b) * T + t] = V[std::size_t(b) * T + t] / (v + eps);
        }
        for (int k = 0; k < kNmfParts; ++k) {
            double ws = 0; for (int b = 0; b < kNmfBands; ++b) ws += W[std::size_t(b) * kNmfParts + std::size_t(k)];
            for (std::size_t t = 0; t < T; ++t) { double num = 0; for (int b = 0; b < kNmfBands; ++b) num += W[std::size_t(b) * kNmfParts + std::size_t(k)] * R[std::size_t(b) * T + t]; H[std::size_t(k) * T + t] *= num / (ws + 1e-30); }
        }
        if (it < kNmfIterations / 2) continue;   // first half: shapes fixed (stay kick/snare/hat), then let them adapt
        for (int b = 0; b < kNmfBands; ++b) for (std::size_t t = 0; t < T; ++t) {
            double v = 0; for (int k = 0; k < kNmfParts; ++k) v += W[std::size_t(b) * kNmfParts + std::size_t(k)] * H[std::size_t(k) * T + t];
            R[std::size_t(b) * T + t] = V[std::size_t(b) * T + t] / (v + eps);
        }
        for (int k = 0; k < kNmfParts; ++k) {
            double hs = 0; for (std::size_t t = 0; t < T; ++t) hs += H[std::size_t(k) * T + t];
            double n = 0;
            for (int b = 0; b < kNmfBands; ++b) {
                double num = 0; for (std::size_t t = 0; t < T; ++t) num += R[std::size_t(b) * T + t] * H[std::size_t(k) * T + t];
                double& w = W[std::size_t(b) * kNmfParts + std::size_t(k)];
                const double upd = w * num / (hs + 1e-30);
                w = k < kNmfDrums ? (1.0 - kNmfAdapt) * W0[std::size_t(b) * kNmfParts + std::size_t(k)] + kNmfAdapt * upd : upd;   // drums stay near their role
                if (k < kNmfDrums) {   // each drum may only use its own frequency range (keeps a ringing 808 from swallowing the hats)
                    const double hz = fLo * std::pow(fHi / fLo, (b + 0.5) / kNmfBands);
                    if (hz < kNmfRange[k][0] || hz > kNmfRange[k][1]) w = 0.0;
                }
                n += w;
            }
            for (int b = 0; b < kNmfBands; ++b) W[std::size_t(b) * kNmfParts + std::size_t(k)] /= n;
            for (std::size_t t = 0; t < T; ++t) H[std::size_t(k) * T + t] *= n;
        }
    }
    // 3. hits = sudden rises of each drum part's loudness curve
    const std::size_t half = std::size_t(std::lround(kDrumPeakWindowS / tHop));
    const std::size_t minGap = std::max<std::size_t>(1, std::size_t(std::lround(kDrumMinGapS / tHop)));
    const double within = kDrumWithinDb + 0.5 * (tn.gateBelowPeakDb - kGateBelowPeakDb);
    const int gm[3] = {kGmKick, kGmSnare, kGmClosedHat};
    struct Hit { std::size_t t; int d; double lvl, rel; bool keep = true; };   // rel = dB vs this drum's typical strong hit
    std::vector<Hit> hits;
    double ref[kNmfDrums] = {1, 1, 1};
    for (int d = 0; d < kNmfDrums; ++d) {
        const double* h = H.data() + std::size_t(d) * T;
        std::vector<double> nov(T, 0.0);
        for (std::size_t t = 1; t < T; ++t) nov[t] = std::max(0.0, h[t] - h[t - 1]);
        auto pk = pickPeaks(nov, half, tn.drumThresholdK, kDrumThresholdDelta, minGap);
        std::vector<double> lvl;
        for (auto t : pk) { double m = 0; for (std::size_t j = t; j < std::min(T, t + 4); ++j) m = std::max(m, h[j]); lvl.push_back(m); }
        if (lvl.empty()) continue;
        auto sorted = lvl; std::sort(sorted.begin(), sorted.end());
        ref[d] = std::max(1e-30, sorted[std::size_t(0.9 * double(sorted.size() - 1))]);
        for (std::size_t i = 0; i < pk.size(); ++i) {
            double fl = -300; for (std::size_t j = pk[i]; j < std::min(T, pk[i] + 4); ++j) fl = std::max(fl, frameDb[j]);
            const double rel = 20.0 * std::log10(std::max(lvl[i], 1e-30) / ref[d]);
            if (rel < -within || fl < tn.absoluteFloorDb || fl < peakDb - tn.gateBelowPeakDb) continue;
            hits.push_back({pk[i], d, lvl[i], rel});
        }
    }
    // bleed: a weak hit of one drum at the same moment as a much stronger hit of another drum is that
    // other drum leaking into this part (a snare's body showing up as a soft kick, etc.)
    const std::size_t same = std::max<std::size_t>(1, std::size_t(std::lround(0.020 / tHop)));
    for (auto& hx : hits) for (const auto& y : hits)
        if (y.d != hx.d && (hx.d == 1 || y.d == 1)   // kick and hat share almost no frequencies, so only pairs with the snare can bleed
            && (hx.t > y.t ? hx.t - y.t : y.t - hx.t) <= same && hx.rel < -kDrumBleedDb && y.rel - hx.rel > kDrumBleedDb) hx.keep = false;
    // ringing: a real hit jumps up from a clearly lower level. A wobble in a drum that is still
    // ringing (an open hat's shimmer, an 808 tail) only nudges the curve, so it is not a new hit.
    for (auto& hx : hits) {
        const double* h = H.data() + std::size_t(hx.d) * T;
        double before = std::numeric_limits<double>::max();
        for (std::size_t j = (hx.t > 3 ? hx.t - 3 : 0); j < hx.t; ++j) before = std::min(before, h[j]);
        if (hx.t == 0) before = 0;
        if (hx.lvl - before < kDrumAttackShare * hx.lvl) hx.keep = false;
    }
    std::vector<Note> notes;
    std::vector<std::size_t> hatTimes; for (const auto& hx : hits) if (hx.keep && hx.d == 2) hatTimes.push_back(hx.t);
    std::sort(hatTimes.begin(), hatTimes.end());
    for (const auto& hx : hits) {
        if (!hx.keep) continue;
        const double* h = H.data() + std::size_t(hx.d) * T;
        int pitch = gm[hx.d]; double len = 0.1;
        if (hx.d == 2) {   // open hat: still ringing (within 20 dB) after kOpenHatDecayS with no new hat before then
            const std::size_t probe = hx.t + std::size_t(std::lround(kOpenHatDecayS / tHop));
            const auto nxt = std::upper_bound(hatTimes.begin(), hatTimes.end(), hx.t);
            const bool interrupted = nxt != hatTimes.end() && *nxt <= probe;
            if (probe < T && !interrupted && 20.0 * std::log10(std::max(h[probe], 1e-30) / hx.lvl) > -20.0) { pitch = kGmOpenHat; len = 0.25; }
        }
        std::size_t s0 = hx.t; while (s0 > 0 && h[s0 - 1] < h[s0] && hx.t - s0 < 3) --s0;   // where the rise began
        const double st = s0 * tHop;
        notes.push_back({st, st + len, pitch, velocityFromDb(20.0 * std::log10(hx.lvl), 20.0 * std::log10(ref[hx.d]))});
    }
    std::sort(notes.begin(), notes.end(), [](const Note& p, const Note& q) { return p.start < q.start || (p.start == q.start && p.pitch < q.pitch); });
    return notes;
}

// =============================================================================================
//  Tempo
// =============================================================================================
struct TempoResult { double bpm = kFallbackBpm; double beatOffsetSec = 0; double confidence = 0; bool found = false; };

// Onset curve at a fixed rate of 1/kHopSeconds frames per second, for summing across stems.
inline std::vector<double> onsetCurve(const std::vector<float>& x, double sr) {
    const std::size_t hop = std::max<std::size_t>(1, std::size_t(std::lround(kHopSeconds * sr)));
    std::size_t fft = 1; while (double(fft) < 0.021 * sr) fft <<= 1;
    const auto S = spectrogram(x, sr, fft, hop);
    auto fl = bandFlux(S, 30.0, std::min(16000.0, 0.47 * sr));
    const double mx = fl.empty() ? 0 : *std::max_element(fl.begin(), fl.end());
    if (mx > 0) for (auto& v : fl) v /= mx;
    return fl;
}

inline TempoResult estimateTempo(const std::vector<double>& env) {
    TempoResult r;
    if (env.size() < 400) return r;
    const std::size_t n = env.size();
    // remove the slow trend so sustained parts don't dominate
    std::vector<double> e(n);
    const std::size_t w = std::size_t(std::lround(0.4 / kHopSeconds));
    std::vector<double> cum(n + 1, 0.0); for (std::size_t i = 0; i < n; ++i) cum[i + 1] = cum[i] + env[i];
    for (std::size_t i = 0; i < n; ++i) { const std::size_t a = i > w ? i - w : 0, b = std::min(n, i + w + 1); e[i] = std::max(0.0, env[i] - (cum[b] - cum[a]) / double(b - a)); }
    auto ac = [&](double lag) {   // autocorrelation at a fractional lag (linear interpolation)
        const std::size_t L = std::size_t(lag); const double fr = lag - double(L);
        if (L + 1 >= n) return 0.0;
        double s = 0; for (std::size_t i = 0; i + L + 1 < n; ++i) s += e[i] * (e[i + L] * (1 - fr) + e[i + L + 1] * fr);
        return s / double(n - L);
    };
    const double lagMin = 60.0 / kTempoMaxBpm / kHopSeconds, lagMax = 60.0 / kTempoMinBpm / kHopSeconds;
    double bestLag = 0, bestScore = -1;
    for (double lag = std::floor(lagMin); lag <= std::ceil(lagMax); lag += 1.0) {
        const double bpm = 60.0 / (lag * kHopSeconds);
        const double oct = std::log2(bpm / kTempoPreferBpm) / kTempoPreferWidthOct;
        const double weight = std::exp(-0.5 * oct * oct);
        // beat + its half (the off-beat subdivision) + half bar + bar. No 3x term: it causes 2/3-tempo mistakes.
        const double s = weight * (ac(lag) + 0.5 * ac(0.5 * lag) + 0.5 * ac(2 * lag) + 0.25 * ac(4 * lag));
        if (s > bestScore) { bestScore = s; bestLag = lag; }
    }
    if (bestScore <= 0) return r;
    // refine on four beats (4x the precision of one beat)
    double L4 = 4 * bestLag, best4 = L4, bs4 = -1;
    for (double l = L4 - 6; l <= L4 + 6; l += 0.25) { const double s = ac(l); if (s > bs4) { bs4 = s; best4 = l; } }
    double lag = best4 / 4.0;
    double bpm = 60.0 / (lag * kHopSeconds);
    if (std::fabs(bpm - std::round(bpm)) < 0.35) bpm = std::round(bpm);           // ASSUMED: songs sit on whole BPM
    lag = 60.0 / bpm / kHopSeconds;
    // phase: where do onsets line up best with a grid of this period?
    double bestPh = 0, bestPs = -1;
    for (double ph = 0; ph < lag; ph += 0.5) {
        double s = 0; for (double t = ph; t < double(n - 1); t += lag) { const std::size_t i = std::size_t(t); s += e[i] + e[std::min(n - 1, i + 1)]; }
        if (s > bestPs) { bestPs = s; bestPh = ph; }
    }
    double ac0 = ac(0.0);
    r.bpm = bpm; r.beatOffsetSec = bestPh * kHopSeconds; r.confidence = ac0 > 0 ? ac(lag) / ac0 : 0; r.found = true;
    return r;
}

} // namespace hc::midi
