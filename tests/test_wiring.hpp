// =============================================================================
//  tests/test_wiring.hpp — "every control is wired to its own engine".
//
//  For EVERY processor in the registry and EVERY parameter it declares:
//    1. render a rich test signal with the processor "armed" (settings chosen so
//       that every control has something to act on),
//    2. move ONE parameter to its minimum and to its maximum, render again,
//    3. the output must change. The size of the change is printed.
//  A parameter that changes nothing in either direction FAILS (a dead control).
//
//  Also checked for every processor: the faceplate only points at real
//  parameters, no NaN/Inf comes out, and process() allocates nothing.
//
//  Parameters that are declared but deliberately unused are listed in kUnused
//  below. The test asserts they really are inert, so the list cannot go stale.
// =============================================================================
#pragma once
#include "hc/engine/builtin_processors.hpp"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace wiring {

using namespace hc;

struct Signal { std::vector<float> L, R; };

// 2 s of stereo "mix": noise bed, tones, drum hits, a 3.2 kHz ring, jittery
// decorrelated top end, sibilant bursts. Peak is about -1 dBFS so a limiter has work to do.
inline Signal makeSignal(double sr) {
    const std::size_t n = static_cast<std::size_t>(sr * 2.0);
    Signal s; s.L.assign(n, 0.0f); s.R.assign(n, 0.0f);
    std::mt19937 rng(1234);
    std::normal_distribution<float> g(0.0f, 1.0f);
    const float twoPi = 2.0f * static_cast<float>(std::numbers::pi);
    float lpL = 0, lpR = 0, hpPrevL = 0, hpPrevR = 0, hpPrev2L = 0, hpPrev2R = 0;
    float jitter = 1.0f;
    const std::size_t jitterLen = static_cast<std::size_t>(sr * 0.005);
    for (std::size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sr);
        const float wl = g(rng), wr = g(rng);
        // pink-ish bed
        lpL += 0.05f * (wl - lpL); lpR += 0.05f * (wr - lpR);
        float l = 0.35f * lpL, r = 0.35f * lpR;
        // tones (centre)
        const float tone = 0.07f * std::sin(twoPi * 220.0f * t) + 0.05f * std::sin(twoPi * 440.0f * t);
        l += tone; r += tone;
        // narrow resonance
        const float ring = 0.08f * std::sin(twoPi * 3200.0f * t);
        l += ring; r += ring;
        // drum hits every 0.25 s
        const float ht = std::fmod(t, 0.25f);
        const float env = std::exp(-ht * 22.0f) * (ht < 0.0015f ? ht / 0.0015f : 1.0f);
        const float hit = 0.55f * env * (0.6f * std::sin(twoPi * 120.0f * ht) + 0.4f * wl);
        l += hit; r += hit * 0.9f;
        // jittery decorrelated top end (differenced noise = high-passed)
        if (i % jitterLen == 0) jitter = 0.4f + 1.2f * std::fabs(g(rng));
        const float hfL = wl - 2.0f * hpPrevL + hpPrev2L, hfR = wr - 2.0f * hpPrevR + hpPrev2R;
        hpPrev2L = hpPrevL; hpPrevL = wl; hpPrev2R = hpPrevR; hpPrevR = wr;
        l += 0.035f * jitter * hfL; r += 0.035f * jitter * hfR;
        // sibilant bursts (mid-high noise, same in both channels)
        const float st = std::fmod(t, 0.5f);
        if (st < 0.12f) { const float sib = 0.10f * (wl - hpPrevL * 0.2f) * std::sin(std::numbers::pi_v<float> * st / 0.12f); l += sib; r += sib; }
        s.L[i] = l; s.R[i] = r;
    }
    float pk = 0; for (std::size_t i = 0; i < n; ++i) pk = std::max({pk, std::fabs(s.L[i]), std::fabs(s.R[i])});
    const float k = 0.89f / pk;   // -1 dBFS
    for (std::size_t i = 0; i < n; ++i) { s.L[i] *= k; s.R[i] *= k; }
    return s;
}

struct Setting { std::size_t index; float value; };

// "Armed" starting point per processor: settings that make every control audible.
inline std::vector<Setting> armed(std::string_view id) {
    if (id == "hc.eq6")        return {{4, 6}, {7, 6}, {10, 6}, {13, 6}, {15, 8000}};
    if (id == "hc.comp")       return {{CompProcessor::kThresh, -30}, {CompProcessor::kRatio, 4}};
    if (id == "hc.artifact")   return {{ArtifactProcessor::kSib, 0.6f}, {ArtifactProcessor::kSibThr, -12}, {ArtifactProcessor::kPhase, 0.6f}, {ArtifactProcessor::kShimmer, 0.8f}, {ArtifactProcessor::kShimTol, 0.5f}};
    if (id == "hc.limiter")    return {{LimiterProcessor::kDrive, 6}};
    if (id == "hc.resonance")  return {{ResonanceProcessor::kSensitivity, 70}, {ResonanceProcessor::kReduction, 9}};
    if (id == "hc.transient")  return {{TransientProcessor::kAttack, 9}, {TransientProcessor::kSustain, 9}};
    return {};
}

// Declared on purpose but not used by the DSP. Kept only so saved sessions keep their parameter numbers.
struct Unused { std::string_view id; std::size_t index; const char* why; };
inline constexpr Unused kUnused[] = {
    {"hc.eq6", 1,  "HPF has no gain; slot kept so old sessions load (not on the faceplate)"},
    {"hc.eq6", 16, "LPF has no gain; slot kept so old sessions load (not on the faceplate)"},
};
inline const Unused* findUnused(std::string_view id, std::size_t i) {
    for (const auto& u : kUnused) if (u.id == id && u.index == i) return &u;
    return nullptr;
}

struct Render { std::vector<float> L, R; bool finite = true; long allocs = 0; };

inline Render render(const ProcessorRegistry& reg, std::string_view id, const std::vector<Setting>& base,
                      const Setting* extra, const Signal& sig, double sr) {
    auto p = reg.create(id);
    for (const auto& s : base) p->param(s.index).set(s.value);
    if (extra) p->param(extra->index).set(extra->value);
    p->prepare(sr, 512);
    Render r; r.L = sig.L; r.R = sig.R;
    const std::size_t n = r.L.size();
    g_allocs.store(0);
    g_countAllocs.store(true);
    for (std::size_t o = 0; o + 512 <= n; o += 512) {
        AudioBlock b{{r.L.data() + o, r.R.data() + o}, 2, 512};
        p->process(b);
    }
    g_countAllocs.store(false);
    r.allocs = g_allocs.load();
    for (std::size_t i = 0; i < n; ++i) if (!std::isfinite(r.L[i]) || !std::isfinite(r.R[i])) { r.finite = false; break; }
    return r;
}

inline double diffDb(const Render& a, const Render& ref) {
    double e = 0, s = 0;
    for (std::size_t i = 0; i < ref.L.size(); ++i) {
        e += std::pow(double(a.L[i] - ref.L[i]), 2) + std::pow(double(a.R[i] - ref.R[i]), 2);
        s += std::pow(double(ref.L[i]), 2) + std::pow(double(ref.R[i]), 2);
    }
    if (e <= 0.0) return -300.0;
    return 10.0 * std::log10(e / s);
}

} // namespace wiring

static void testEveryControlIsWired() {
    using namespace wiring;
    std::printf("\n--- Every control moves its own engine (render with the control at min and max, compare) ---\n");
    const double sr = 48000.0;
    const Signal sig = makeSignal(sr);
    const ProcessorRegistry reg = makeBuiltinRegistry();
    int controls = 0, wired = 0;
    for (const auto& e : reg.entries()) {
        auto probe = reg.create(e.id);
        const ProcessorInfo& info = probe->info();
        const std::vector<Setting> base = armed(e.id);
        const Render ref = render(reg, e.id, base, nullptr, sig, sr);

        char name[160];
        std::snprintf(name, sizeof name, "%s: parameter table matches the engine (declared / real)", std::string(e.id).c_str());
        check(info.params.size() == probe->numParams(), name, double(info.params.size()));
        std::snprintf(name, sizeof name, "%s: output is finite, nothing blows up", std::string(e.id).c_str());
        check(ref.finite, name, ref.finite ? 1.0 : 0.0);
        std::snprintf(name, sizeof name, "%s: process() allocates nothing (allocations)", std::string(e.id).c_str());
        check(ref.allocs == 0, name, double(ref.allocs));

        // The faceplate may only point at real parameters.
        bool faceOk = true;
        for (const auto& c : info.faceplate.controls)
            if (c.paramIndex >= static_cast<std::int16_t>(info.params.size())) faceOk = false;
        std::snprintf(name, sizeof name, "%s: faceplate only points at real parameters", std::string(e.id).c_str());
        check(faceOk, name, faceOk ? 1.0 : 0.0);

        for (std::size_t i = 0; i < info.params.size(); ++i) {
            const ParamDesc& pd = info.params[i];
            const Setting lo{i, pd.minValue}, hi{i, pd.maxValue};
            const double dLo = diffDb(render(reg, e.id, base, &lo, sig, sr), ref);
            const double dHi = diffDb(render(reg, e.id, base, &hi, sig, sr), ref);
            const double best = std::max(dLo, dHi);
            const Unused* u = findUnused(e.id, i);
            ++controls;
            std::snprintf(name, sizeof name, "%s / %s: moving it changes the sound (dB vs reference)", std::string(e.id).c_str(), std::string(pd.id).c_str());
            if (u) {
                check(best < -100.0, name, best, "dB (NOT USED, listed on purpose)");
                std::printf("        note: %s\n", u->why);
            } else {
                const bool ok = best > -60.0;
                if (ok) ++wired;
                check(ok, name, best, "dB");
            }
        }
    }
    std::printf("        controls checked: %d, wired and audible: %d, declared-but-unused: %d\n",
                controls, wired, controls - wired);
}
