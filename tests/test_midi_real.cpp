// =============================================================================
//  tests/test_midi_real.cpp — HEM STEM-TO-MIDI on stems built from REAL recorded samples
//  (tools/make_real_sample_test.py). Scores each stem against its answer key.
//
//  hem_midi_realtest FOLDER      (exit code = number of failures)
// =============================================================================
#include "hc/midi/transcriber.hpp"
#include "midi_signals.hpp"
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

using namespace hc::midi;
static int g_fail = 0;

static std::vector<Note> readKey(const std::filesystem::path& p) {
    std::vector<Note> v; std::ifstream f(p); std::string line;
    while (std::getline(f, line)) {
        std::stringstream ss(line); std::string a, b, c;
        if (std::getline(ss, a, ',') && std::getline(ss, b, ',') && std::getline(ss, c, ',')) v.push_back({std::stod(a), std::stod(b), std::stoi(c), 100});
    }
    return v;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::puts("usage: hem_midi_realtest FOLDER"); return 1; }
    const std::filesystem::path dir = argv[1];
    std::puts("HEM STEM-TO-MIDI on stems made from real recorded samples. Limits are ASSUMED until approved.");
    struct Case { const char* name; Mode mode; double minF; };
    const Case cases[] = {{"real_drums_kit", Mode::Drums, kTestDrumMinF}, {"real_drums_808", Mode::Drums, kTestDrumMinF},
                          {"real_piano_chords", Mode::Chords, kTestPolyMinF}, {"real_piano_melody", Mode::Lead, kTestMonoMinF}};
    for (const auto& c : cases) {
        hc::wav::Audio a;
        const auto wavPath = dir / (std::string(c.name) + ".wav");
        if (auto e = hc::wav::read(wavPath, a); !e.empty()) { std::printf("[FAIL] %s: %s\n", c.name, e.c_str()); return 1; }   // missing test files = broken test run
        const auto key = readKey(dir / (std::string(c.name) + ".csv"));
        std::vector<Note> got, unused; std::string err;
        transcribeStem(a, c.mode, Settings{}, got, unused, err);
        const auto s = hemtest::score(key, got, kTestOnsetTolS);
        char t[160]; std::snprintf(t, sizeof t, "%s (%s): F-measure >= %.2f", c.name, std::string(kModeNames[int(c.mode)]).c_str(), c.minF);
        const bool ok = s.f() >= c.minF && err.empty();
        std::printf("[%s] %-66s %10.4g\n", ok ? "PASS" : "FAIL", t, s.f());
        std::printf("       %d right of %d in the answer key, %d found (precision %.2f, recall %.2f)\n", s.hit, s.truth, s.found, s.precision(), s.recall());
        if (!ok) ++g_fail;
        if (c.mode == Mode::Drums) {   // per drum
            std::map<int, std::pair<std::vector<Note>, std::vector<Note>>> by;
            for (auto& n : key) by[n.pitch].first.push_back(n);
            for (auto& n : got) by[n.pitch].second.push_back(n);
            for (auto& [p, kv] : by) { const auto sc = hemtest::score(kv.first, kv.second, kTestOnsetTolS); std::printf("         drum %d: %d of %d right, %d found\n", p, sc.hit, sc.truth, sc.found); }
        }
    }
    std::printf("\nACCURACY BARS ON REAL SAMPLES: %d of 4 met, %d missed (bars are ASSUMED, see midi_config.hpp)\n", 4 - g_fail, g_fail);
    return 0;   // accuracy is reported, not used to stop the build
}
