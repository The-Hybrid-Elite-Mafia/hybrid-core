// =============================================================================
//  THE HYBRID CORE 2.0 — Hybrid Core Studio (Windows x64)
//  studio/main.cpp — the application.
//
//  The window shows the HEM faceplate art. Every knob, meter, screen and
//  readout painted on it was cut out (studio/skin_layout.py) and is drawn here
//  live, at the exact same spot, wired to the real engine:
//    track rows ........ 13 stems: name, volume (drag), mute, solo, waveform
//    transport ......... menu, rewind, play, stop, record = export master,
//                        time, tempo, master level, analyze, settings, view
//    STFT DESTROYER .... artifact repair (per selected stem) + live spectrogram
//    RESONANCE ......... harsh-resonance suppressor + live cut curve
//    TRANSIENT ......... transient designer + live attack/sustain activity
//    MASTERING ......... width, glue, true-peak limiter, LUFS / TP meters,
//                        loudness target for export
//
//  Threads: UI (this file) · WASAPI render thread (MMCSS) · DSP workers ·
//  export worker. The UI never blocks audio: it reads meters from triple
//  buffers / rings and writes parameters through atomics.
// =============================================================================
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include "gfx.hpp"
#include "app_util.hpp"
#include "skin_layout.hpp"
#include "resource.h"
#include "hc/app/offline_render.hpp"
#include "hc/engine/builtin_processors.hpp"
#include "hc/engine/stem_engine.hpp"
#include "hc/platform/thread_priority.hpp"
#include "hc/platform/wasapi_output.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace hc;
using namespace hc::gfx;
using namespace hc::studio;
namespace LY = hc::studio::layout;

namespace {

// ------------------------------------------------------------------ palette
constexpr Color kCyan = 0x3EC8FFFF, kCyanDim = 0x1B5E78FF, kOrange = 0xFF9A3EFF, kGreen = 0x5BE37AFF;
constexpr Color kYellow = 0xE8C547FF, kRed = 0xFF4A4AFF, kText = 0xD8E6EEFF, kDim = 0x7F93A0FF;
constexpr Color kScreen = 0x070B0FFF, kPanel = 0x12171CFF, kWhite = 0xF2F4F6FF;
constexpr const char* kVersion = "2.0.0";

constexpr std::uint32_t kBlock = 1024;
constexpr UINT WM_EXPORT_DONE = WM_APP + 1;

Rect R(const LY::Box& b) { return {b.x0, b.y0, b.x1, b.y1}; }
float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
bool stemColorIsOrange(int i) { return i == 0 || i == 4 || i == 6 || i == 10; }

std::string fmt(const char* f, double v) { char b[64]; std::snprintf(b, sizeof b, f, v); return b; }
std::string timeStr(double sec) {
    if (sec < 0) sec = 0;
    const int m = static_cast<int>(sec / 60.0);
    const double s = sec - 60.0 * m;
    char b[32]; std::snprintf(b, sizeof b, "%02d:%06.3f", m, s);
    return b;
}
std::string lufsStr(float v) { return std::isfinite(v) && v > -70.0f ? fmt("%.1f", v) : std::string("--"); }

// ------------------------------------------------------------------ spectrogram (UI thread)
struct Spectrogram {
    static constexpr int kCols = 224, kRows = 56, kN = 1024, kHop = 512;
    float cell[kCols][kRows] = {};
    int head = 0;
    RealFFT fft;
    std::vector<float> ring, win, frame, re, im;
    std::size_t pos = 0, since = 0;
    double sr = 48000.0;
    int rowBin[kRows + 1] = {};
    float cal = 0.0f;

    void prepare(double rate) {
        sr = rate; fft.prepare(kN);
        ring.assign(kN, 0); frame.assign(kN, 0); re.assign(kN / 2 + 1, 0); im.assign(kN / 2 + 1, 0); win.resize(kN);
        double ws = 0;
        for (int i = 0; i < kN; ++i) { win[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * 3.14159265358979 * i / kN)); ws += win[i]; }
        cal = static_cast<float>(-20.0 * std::log10(ws / 2.0));
        for (int r = 0; r <= kRows; ++r) {
            const double f = 40.0 * std::pow(20000.0 / 40.0, static_cast<double>(r) / kRows);
            rowBin[r] = std::clamp(static_cast<int>(f * kN / sr), 1, kN / 2);
        }
        for (auto& c : cell) for (auto& v : c) v = -140.0f;
        pos = since = 0; head = 0;
    }
    void feed(const float* x, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            ring[pos] = x[i]; pos = (pos + 1) % kN;
            if (++since == kHop) { since = 0; column(); }
        }
    }
    void column() {
        for (int i = 0; i < kN; ++i) frame[i] = ring[(pos + i) % kN] * win[i];
        fft.forward(frame.data(), re.data(), im.data());
        float* c = cell[head];
        for (int r = 0; r < kRows; ++r) {
            float m = 0.0f;
            for (int k = rowBin[r]; k <= std::max(rowBin[r], rowBin[r + 1] - 1); ++k) m = std::max(m, re[k] * re[k] + im[k] * im[k]);
            c[r] = 10.0f * std::log10(m + 1e-20f) + cal;
        }
        head = (head + 1) % kCols;
    }
};

Color heat(float t) {   // spectrogram colour map: black → blue → cyan → yellow → orange → red
    t = clampf(t, 0, 1);
    static const Color stops[] = {0x03060AFF, 0x0B2E6BFF, 0x0F8FBFFF, 0x3FD3C8FF, 0xE9D23BFF, 0xFF8A1EFF, 0xFF3B1EFF};
    const float x = t * 6.0f;
    const int i = std::min(5, static_cast<int>(x));
    return Color::lerp(stops[i], stops[i + 1], x - static_cast<float>(i));
}

// ------------------------------------------------------------------ input
struct Input {
    float mx = 0, my = 0;            // image coordinates
    float devX = 0, devY = 0;        // device pixels
    bool down = false, pressed = false, released = false, dbl = false, rpressed = false;
    float wheel = 0;
    bool shift = false, ctrl = false;
};

// ------------------------------------------------------------------ stem slot
struct Slot {
    bool loaded = false;
    std::wstring path;
    std::string name;
    wav::Audio audio;
    std::vector<float> peaks;
    ArtifactProcessor* ds = nullptr;
    ResonanceProcessor* rs = nullptr;
    TransientProcessor* tr = nullptr;
    StemMeterSnapshot meter;
    float dotLevel = 0;
};

// Suno names every stem "<Song> (<Part>)". When all loaded stems share a
// prefix, rows show only the part ("Bass", "Drums") and the song title is
// used for export / session names.
std::string trimName(std::string s) {
    auto junk = [](char c) { return c == ' ' || c == '(' || c == ')' || c == '-' || c == '_' || c == '[' || c == ']' || c == '.'; };
    while (!s.empty() && junk(s.front())) s.erase(s.begin());
    while (!s.empty() && junk(s.back())) s.pop_back();
    return s;
}
std::string commonPrefix(const std::vector<std::string>& v) {
    if (v.empty()) return {};
    std::string p = v[0];
    for (const auto& s : v) {
        std::size_t i = 0;
        while (i < p.size() && i < s.size() && p[i] == s[i]) ++i;
        p.resize(i);
    }
    // cut back to a word boundary so "Two hundred forty days (B" → "Two hundred forty days ("
    while (!p.empty() && p.back() != ' ' && p.back() != '(' && p.back() != '-' && p.back() != '_') p.pop_back();
    return p;
}

enum class Glue { Clean = 0, Punch = 1, Loud = 2 };

struct TargetPreset { const char* name; double lufs; bool normalize; };
constexpr TargetPreset kPresets[] = {
    {"STREAMING -14", -14.0, true}, {"APPLE -16", -16.0, true}, {"CLUB -9", -9.0, true},
    {"BROADCAST -23", -23.0, true}, {"NO NORMALIZE", -14.0, false}};
constexpr int kNumPresets = 5;

// ======================================================================================
class App {
public:
    bool init(HINSTANCE inst, int show);
    void openFromCommandLine();
    int run();
    LRESULT wndProc(HWND h, UINT m, WPARAM w, LPARAM l);

private:
    // audio / engine
    void startAudio();
    void stopAudio();
    void rebuildEngine(double sr, const Session* fromSession = nullptr);
    ChainState captureStem(int s) const;
    ChainState captureMaster() const;
    void publishStemChain(int s, const ChainState* st);
    void publishMasterChain(const ChainState* st);
    StripState stripOf(int s) const;
    void applyGlue();
    static void audioCallback(void* user, float* L, float* R, std::uint32_t n) noexcept;

    // files
    void loadFiles(const std::vector<std::wstring>& files, int slot = -1);
    void clearSlot(int s);
    void clearAll();
    void saveSessionAs();
    void openSession();
    void exportMaster();
    void onExportDone();
    int loadedCount() const;
    std::uint64_t songLength() const;

    // ui
    void frame();
    void updateTransform();
    void drawTransport();
    void drawArrange();
    void drawDestroyer();
    void drawResonance();
    void drawTransient();
    void drawMastering();
    void drawOverlays();
    float knob(int id, const LY::Knob& k, float value, float mn, float mx, Taper taper, float def, bool bipolar,
               Color accent, const std::string& valueText, bool dim);
    bool button(int id, const Rect& r);
    void toast(const std::string& s) { toast_ = s; toastUntil_ = now() + 4.0; }
    static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    void showMainMenu();
    void showSettingsMenu();
    void showAbout();
    void stemContextMenu(int s);
    Slot& sel() { return slots_[static_cast<std::size_t>(selected_)]; }
    std::string shownName(int i) const;
    std::string songTitle() const;

    HWND hwnd_ = nullptr;
    Renderer gfx_;
    Input in_;
    int activeId_ = 0, hotId_ = 0;
    float dragStartY_ = 0, dragStartX_ = 0, dragStartVal_ = 0;
    bool dragMoved_ = false;
    std::string tooltip_;
    float tipX_ = 0, tipY_ = 0;
    bool fullView_ = true;
    float viewScale_ = 1, viewOffX_ = 0, viewOffY_ = 0;

    std::unique_ptr<StemEngine> eng_;
    WasapiOutput dev_;
    bool exclusive_ = false, audioRunning_ = false, nullAudio_ = false;
    std::thread nullThread_;
    std::atomic<bool> nullRun_{false};
    double sr_ = 48000.0;
    std::string deviceDesc_ = "No audio device";

    std::array<Slot, kNumStems> slots_;
    int selected_ = 0;
    WidthProcessor* width_ = nullptr;
    CompProcessor* glue_ = nullptr;
    LimiterProcessor* lim_ = nullptr;
    Glue glueMode_ = Glue::Clean;
    int preset_ = 0;
    double target_ = -14.0;
    bool normalize_ = true;
    double bpm_ = 120.0;
    bool analyze_ = false;
    std::array<bool, kNumStems> rebuildPending_{};
    std::array<ChainState, kNumStems> rebuildState_{};

    Spectrogram spec_;
    int specFor_ = -1;
    std::vector<TransientViz> trHist_;
    std::size_t trHead_ = 0;
    ResonanceView resView_{};
    StemMeterSnapshot master_{};
    float tpHold_[2] = {-90, -90}, lvl_[2] = {-90, -90};
    double tpHoldT_[2] = {0, 0};

    std::string toast_;
    double toastUntil_ = 0;

    // export
    std::thread exportThread_;
    std::atomic<bool> exporting_{false}, exportCancel_{false};
    std::atomic<double> exportProgress_{0.0};
    RenderResult exportResult_;
    std::wstring exportPath_;
    std::string exportError_;

    std::wstring sessionPath_;
};

App* g_app = nullptr;

LRESULT CALLBACK StaticWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_app) return g_app->wndProc(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

// ====================================================================================== init
bool App::init(HINSTANCE inst, int show) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"HybridCoreStudio";
    RegisterClassExW(&wc);

    // Size the window to the faceplate's aspect inside the monitor work area.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const float ww = static_cast<float>(work.right - work.left), wh = static_cast<float>(work.bottom - work.top);
    fullView_ = wh >= 1300.0f;   // tall screens show the crown + title; laptops show the console
    const LY::Box v = fullView_ ? LY::view_full : LY::view_console;
    const float s = std::min(ww * 0.96f / (v.x1 - v.x0), wh * 0.96f / (v.y1 - v.y0));
    RECT rc{0, 0, static_cast<LONG>((v.x1 - v.x0) * s), static_cast<LONG>((v.y1 - v.y0) * s)};
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    hwnd_ = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"THE HYBRID CORE 2.0 — Hybrid Elite Mafia", WS_OVERLAPPEDWINDOW,
                            work.left + (static_cast<int>(ww) - w) / 2, work.top + (static_cast<int>(wh) - h) / 2, w, h,
                            nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;

    if (auto e = gfx_.init(hwnd_); !e.empty()) {
        MessageBoxW(hwnd_, widen("The graphics engine couldn't start:\n" + e).c_str(), L"Hybrid Core", MB_ICONERROR);
        return false;
    }
    const void* data = nullptr;
    std::size_t size = 0;
    std::vector<std::uint8_t> rgba;
    int iw = 0, ih = 0;
    if (!loadResource(IDR_SKIN, data, size) || !decodeImage(data, size, rgba, iw, ih)) {
        MessageBoxW(hwnd_, L"The faceplate image is missing from this build.", L"Hybrid Core", MB_ICONERROR);
        return false;
    }
    gfx_.setSkin(rgba.data(), iw, ih);

    trHist_.assign(280, TransientViz{});
    rebuildEngine(48000.0);
    ShowWindow(hwnd_, show);
    UpdateWindow(hwnd_);
    toast("Drop up to 13 WAV stems on the window, or use the menu (≡) → Load Stems");
    return true;
}

int App::run() {
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                exportCancel_ = true;
                if (exportThread_.joinable()) exportThread_.join();
                stopAudio();
                if (eng_) eng_->shutdown();
                return static_cast<int>(msg.wParam);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (IsIconic(hwnd_)) { Sleep(50); continue; }
        frame();
    }
}

// Files passed on the command line (or via "Open with"): WAV stems are
// loaded into slots; a .hcs session is opened. "--play" starts playback.
void App::openFromCommandLine() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;
    std::vector<std::wstring> wavs;
    bool play = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        std::wstring low = a;
        for (auto& c : low) c = static_cast<wchar_t>(towlower(c));
        if (low == L"--play") { play = true; continue; }
        if (low.size() > 4 && low.substr(low.size() - 4) == L".hcs") {
            Session sn;
            if (loadSession(a, sn, kNumStems).empty()) {
                double rate = 0.0;
                for (std::size_t k = 0; k < kNumStems; ++k) {
                    if (sn.paths[k].empty()) continue;
                    wav::Audio au;
                    if (!wav::read(std::filesystem::path(sn.paths[k]), au).empty()) continue;
                    if (rate == 0.0) rate = au.sampleRate;
                    if (au.sampleRate != rate) continue;
                    Slot& sl = slots_[k];
                    sl.loaded = true; sl.path = sn.paths[k]; sl.name = stemNameFromPath(sn.paths[k]);
                    sl.audio = std::move(au); sl.peaks = computePeaks(sl.audio, 2048);
                }
                bpm_ = sn.bpm; target_ = sn.targetLufs; normalize_ = sn.normalize; preset_ = std::clamp(sn.preset, 0, kNumPresets - 1);
                rebuildEngine(rate > 0 ? rate : sr_, &sn);
                sessionPath_ = a;
            }
        } else if (low.size() > 4 && low.substr(low.size() - 4) == L".wav") wavs.push_back(a);
    }
    LocalFree(argv);
    if (!wavs.empty()) loadFiles(wavs);
    if (play && loadedCount()) eng_->play();
}

// ====================================================================================== audio
void App::audioCallback(void* user, float* L, float* R, std::uint32_t n) noexcept {
    static_cast<StemEngine*>(user)->process(L, R, n);
}

void App::startAudio() {
    if (audioRunning_ || !eng_) return;
    WasapiOutput::Config cfg;
    cfg.mode = exclusive_ ? WasapiOutput::Mode::Exclusive : WasapiOutput::Mode::SharedLowLatency;
    cfg.sampleRate = sr_;
    cfg.maxBlock = kBlock;
    std::string err = dev_.open(cfg);
    if (!err.empty() && exclusive_) {
        toast("Exclusive mode unavailable (" + err + ") — using shared mode");
        exclusive_ = false;
        cfg.mode = WasapiOutput::Mode::SharedLowLatency;
        err = dev_.open(cfg);
    }
    if (err.empty()) err = dev_.start(&App::audioCallback, eng_.get());
    if (err.empty()) {
        deviceDesc_ = dev_.info().description;
        audioRunning_ = true; nullAudio_ = false;
        return;
    }
    // No usable output device: keep the engine clocked in real time so meters,
    // chain swaps and editing still work, and say so.
    deviceDesc_ = "No audio output (" + err + ")";
    toast("No audio device available — editing and export still work");
    nullRun_ = true;
    StemEngine* e = eng_.get();
    nullThread_ = std::thread([this, e] {
        std::vector<float> L(512), R(512);
        auto t = std::chrono::steady_clock::now();
        const auto step = std::chrono::duration<double>(512.0 / sr_);
        while (nullRun_.load()) {
            e->process(L.data(), R.data(), 512);
            t += std::chrono::duration_cast<std::chrono::steady_clock::duration>(step);
            std::this_thread::sleep_until(t);
        }
    });
    audioRunning_ = true; nullAudio_ = true;
}

void App::stopAudio() {
    if (!audioRunning_) return;
    if (nullAudio_) { nullRun_ = false; if (nullThread_.joinable()) nullThread_.join(); }
    else dev_.close();
    audioRunning_ = false;
}

StripState App::stripOf(int s) const {
    StripState st;
    if (!eng_) return st;
    auto& e = const_cast<StemEngine&>(*eng_).stem(static_cast<std::size_t>(s));
    st.faderDb = e.faderDb.target(); st.pan = e.pan.target(); st.mute = e.mute.load(); st.solo = e.solo.load();
    return st;
}

ChainState App::captureStem(int s) const {
    const Slot& sl = slots_[static_cast<std::size_t>(s)];
    ChainState c;
    if (sl.ds) c.push_back(captureProc(*sl.ds));
    if (sl.rs) c.push_back(captureProc(*sl.rs));
    if (sl.tr) c.push_back(captureProc(*sl.tr));
    return c;
}

ChainState App::captureMaster() const {
    ChainState c;
    if (width_) c.push_back(captureProc(*width_));
    if (glue_) c.push_back(captureProc(*glue_));
    if (lim_) c.push_back(captureProc(*lim_));
    return c;
}

void App::publishStemChain(int s, const ChainState* st) {
    auto reg = makeBuiltinRegistry();
    auto chain = std::make_unique<ProcessorChain>();
    std::unique_ptr<IProcessor> ds, rs, tr;
    if (st) for (const auto& p : *st) {
        if (p.id == "hc.artifact") ds = restoreProc(reg, p);
        else if (p.id == "hc.resonance") rs = restoreProc(reg, p);
        else if (p.id == "hc.transient") tr = restoreProc(reg, p);
    }
    if (!ds) ds = reg.create("hc.artifact");
    if (!rs) rs = reg.create("hc.resonance");
    if (!tr) tr = reg.create("hc.transient");
    Slot& sl = slots_[static_cast<std::size_t>(s)];
    sl.ds = static_cast<ArtifactProcessor*>(ds.get());
    sl.rs = static_cast<ResonanceProcessor*>(rs.get());
    sl.tr = static_cast<TransientProcessor*>(tr.get());
    chain->add(std::move(ds)); chain->add(std::move(rs)); chain->add(std::move(tr));
    eng_->publishChain(static_cast<std::size_t>(s), std::move(chain));
    if (s == selected_) specFor_ = -1;
}

void App::publishMasterChain(const ChainState* st) {
    auto reg = makeBuiltinRegistry();
    auto chain = std::make_unique<ProcessorChain>();
    std::unique_ptr<IProcessor> w, g, l;
    if (st) for (const auto& p : *st) {
        if (p.id == "hc.width") w = restoreProc(reg, p);
        else if (p.id == "hc.comp") g = restoreProc(reg, p);
        else if (p.id == "hc.limiter") l = restoreProc(reg, p);
    }
    if (!w) w = reg.create("hc.width");
    if (!g) { g = reg.create("hc.comp"); g->setBypassed(true); }
    if (!l) l = reg.create("hc.limiter");
    width_ = static_cast<WidthProcessor*>(w.get());
    glue_ = static_cast<CompProcessor*>(g.get());
    lim_ = static_cast<LimiterProcessor*>(l.get());
    chain->add(std::move(w)); chain->add(std::move(g)); chain->add(std::move(l));
    eng_->publishChain(StemEngine::kMaster, std::move(chain));
}

void App::applyGlue() {
    if (!glue_) return;
    switch (glueMode_) {
    case Glue::Clean: glue_->setBypassed(true); break;
    case Glue::Punch:   // gentle 2:1 bus glue
        glue_->param(CompProcessor::kThresh).set(-12); glue_->param(CompProcessor::kRatio).set(2);
        glue_->param(CompProcessor::kKnee).set(6);     glue_->param(CompProcessor::kAttack).set(30);
        glue_->param(CompProcessor::kRelease).set(150); glue_->param(CompProcessor::kMakeup).set(0);
        glue_->setBypassed(false); break;
    case Glue::Loud:    // firmer 3:1, faster
        glue_->param(CompProcessor::kThresh).set(-16); glue_->param(CompProcessor::kRatio).set(3);
        glue_->param(CompProcessor::kKnee).set(6);     glue_->param(CompProcessor::kAttack).set(10);
        glue_->param(CompProcessor::kRelease).set(100); glue_->param(CompProcessor::kMakeup).set(0);
        glue_->setBypassed(false); break;
    }
}

void App::rebuildEngine(double sr, const Session* fromSession) {
    stopAudio();
    std::array<ChainState, kNumStems> chains;
    std::array<StripState, kNumStems> strips;
    ChainState master;
    bool haveState = false;
    if (fromSession) {
        for (std::size_t i = 0; i < kNumStems; ++i) { chains[i] = fromSession->chains[i]; strips[i] = fromSession->strips[i]; }
        master = fromSession->master;
        haveState = true;
    } else if (eng_) {
        for (std::size_t i = 0; i < kNumStems; ++i) { chains[i] = captureStem(static_cast<int>(i)); strips[i] = stripOf(static_cast<int>(i)); }
        master = captureMaster();
        haveState = true;
    }
    if (eng_) eng_->shutdown();
    eng_ = std::make_unique<StemEngine>();
    for (std::size_t i = 0; i < kNumStems; ++i) {
        auto& st = eng_->stem(i);
        st.faderDb.set(strips[i].faderDb); st.pan.set(strips[i].pan);
        st.mute.store(strips[i].mute); st.solo.store(strips[i].solo);
    }
    sr_ = sr;
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    eng_->prepare(sr, kBlock, std::min(hw - 1, 12u), &platform::initRealtimeWorker);
    for (std::size_t i = 0; i < kNumStems; ++i) {
        Slot& sl = slots_[i];
        if (sl.loaded) eng_->setSource(i, sl.audio.ch[0].data(), sl.audio.ch[1].data(), sl.audio.frames());
        publishStemChain(static_cast<int>(i), haveState && !chains[i].empty() ? &chains[i] : nullptr);
    }
    publishMasterChain(haveState && !master.empty() ? &master : nullptr);
    if (fromSession) glueMode_ = static_cast<Glue>(std::clamp(fromSession->glueMode, 0, 2));
    applyGlue();
    rebuildPending_.fill(false);
    specFor_ = -1;
    startAudio();
}


std::string App::shownName(int i) const {
    const Slot& s = slots_[static_cast<std::size_t>(i)];
    if (!s.loaded) return {};
    std::vector<std::string> names;
    for (const auto& x : slots_) if (x.loaded) names.push_back(x.name);
    if (names.size() < 2) return s.name;
    const std::string p = commonPrefix(names);
    if (p.size() < 4 || p.size() >= s.name.size()) return s.name;
    const std::string part = trimName(s.name.substr(p.size()));
    return part.empty() ? s.name : part;
}

std::string App::songTitle() const {
    std::vector<std::string> names;
    for (const auto& x : slots_) if (x.loaded) names.push_back(x.name);
    if (names.empty()) return "HEM";
    std::string t = names.size() > 1 ? trimName(commonPrefix(names)) : names[0];
    if (names.size() == 1) {   // "Song (Vocals)" → "Song"
        if (const auto b = t.rfind(" ("); b != std::string::npos && t.back() == ')') t = t.substr(0, b);
    }
    t = trimName(t);
    return t.empty() ? std::string("HEM") : t;
}

// ====================================================================================== files
int App::loadedCount() const { int n = 0; for (auto& s : slots_) n += s.loaded ? 1 : 0; return n; }
std::uint64_t App::songLength() const { std::uint64_t n = 0; for (auto& s : slots_) if (s.loaded) n = std::max(n, s.audio.frames()); return n; }

void App::loadFiles(const std::vector<std::wstring>& files, int slot) {
    if (exporting_) { toast("Wait for the export to finish"); return; }
    if (files.empty()) return;
    std::vector<int> targets;
    if (slot >= 0) targets.push_back(slot);
    else for (int i = 0; i < static_cast<int>(kNumStems) && targets.size() < files.size(); ++i) if (!slots_[static_cast<std::size_t>(i)].loaded) targets.push_back(i);
    std::string errors;
    std::size_t skipped = files.size() > targets.size() ? files.size() - targets.size() : 0;
    double rate = loadedCount() > 0 ? sr_ : 0.0;
    struct Loaded { int slot; std::wstring path; wav::Audio a; };
    std::vector<Loaded> ok;
    SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_WAIT)));
    for (std::size_t f = 0; f < files.size() && f < targets.size(); ++f) {
        wav::Audio a;
        const std::string e = wav::read(std::filesystem::path(files[f]), a);
        if (!e.empty()) { errors += e + "\n"; continue; }
        if (a.frames() == 0) { errors += stemNameFromPath(files[f]) + ": no audio\n"; continue; }
        if (rate == 0.0) rate = a.sampleRate;
        if (a.sampleRate != rate) {
            errors += stemNameFromPath(files[f]) + fmt(": %.0f Hz", a.sampleRate) + fmt(" (the session is %.0f Hz)\n", rate);
            continue;
        }
        ok.push_back({targets[f], files[f], std::move(a)});
    }
    if (!ok.empty()) {
        stopAudio();
        eng_->stop();
        for (auto& l : ok) {
            Slot& s = slots_[static_cast<std::size_t>(l.slot)];
            s.loaded = true; s.path = l.path; s.name = stemNameFromPath(l.path);
            s.audio = std::move(l.a);
            s.peaks = computePeaks(s.audio, 2048);
        }
        const bool first = loadedCount() == static_cast<int>(ok.size());
        if (rate != sr_) rebuildEngine(rate);
        else {
            for (auto& l : ok) {
                Slot& s = slots_[static_cast<std::size_t>(l.slot)];
                eng_->setSource(static_cast<std::size_t>(l.slot), s.audio.ch[0].data(), s.audio.ch[1].data(), s.audio.frames());
            }
            startAudio();
        }
        eng_->seek(0);
        if (first || slot < 0) {
            std::vector<const wav::Audio*> v;
            for (auto& s : slots_) if (s.loaded) v.push_back(&s.audio);
            bpm_ = estimateTempo(v, sr_);
        }
        if (slot >= 0) selected_ = slot;
        toast(fmt("Loaded %.0f stem", static_cast<double>(ok.size())) + (ok.size() == 1 ? "" : "s") + fmt("  ·  tempo estimate %.1f BPM (drag the tempo box to fix)", bpm_));
    }
    SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW)));
    if (skipped) errors += fmt("%.0f file(s) skipped: all 13 stem slots are full.\n", static_cast<double>(skipped));
    if (!errors.empty()) MessageBoxW(hwnd_, widen("Some files weren't loaded:\n\n" + errors).c_str(), L"Hybrid Core", MB_ICONWARNING);
}

void App::clearSlot(int s) {
    if (exporting_) { toast("Wait for the export to finish"); return; }
    stopAudio();
    Slot& sl = slots_[static_cast<std::size_t>(s)];
    eng_->setSource(static_cast<std::size_t>(s), nullptr, nullptr, 0);
    sl.loaded = false; sl.path.clear(); sl.name.clear(); sl.audio = wav::Audio{}; sl.peaks.clear();
    startAudio();
}

void App::clearAll() {
    if (exporting_) { toast("Wait for the export to finish"); return; }
    stopAudio();
    eng_->stop();
    for (std::size_t i = 0; i < kNumStems; ++i) {
        eng_->setSource(i, nullptr, nullptr, 0);
        Slot& sl = slots_[i];
        sl.loaded = false; sl.path.clear(); sl.name.clear(); sl.audio = wav::Audio{}; sl.peaks.clear();
    }
    sessionPath_.clear();
    startAudio();
    eng_->seek(0);
}

void App::saveSessionAs() {
    Session s;
    s.paths.resize(kNumStems); s.chains.resize(kNumStems); s.strips.resize(kNumStems);
    for (std::size_t i = 0; i < kNumStems; ++i) {
        s.paths[i] = slots_[i].loaded ? slots_[i].path : L"";
        s.chains[i] = captureStem(static_cast<int>(i));
        s.strips[i] = stripOf(static_cast<int>(i));
    }
    s.master = captureMaster();
    s.bpm = bpm_; s.targetLufs = target_; s.normalize = normalize_; s.glueMode = static_cast<int>(glueMode_); s.preset = preset_;
    const std::wstring def = widen(songTitle()) + L" - HEM Session";
    const std::wstring path = saveFileDialog(hwnd_, L"Save Hybrid Core Session", {L"Hybrid Core Session (*.hcs)", L"*.hcs"}, L"hcs",
                                             sessionPath_.empty() ? def : sessionPath_);
    if (path.empty()) return;
    if (auto e = saveSession(path, s); !e.empty()) { MessageBoxW(hwnd_, widen(e).c_str(), L"Hybrid Core", MB_ICONERROR); return; }
    sessionPath_ = path;
    toast("Session saved");
}

void App::openSession() {
    if (exporting_) { toast("Wait for the export to finish"); return; }
    auto files = openFilesDialog(hwnd_, L"Open Hybrid Core Session", {L"Hybrid Core Session (*.hcs)", L"*.hcs"}, false);
    if (files.empty()) return;
    Session s;
    if (auto e = loadSession(files[0], s, kNumStems); !e.empty()) { MessageBoxW(hwnd_, widen(e).c_str(), L"Hybrid Core", MB_ICONERROR); return; }
    stopAudio();
    std::string missing;
    double rate = 0.0;
    for (std::size_t i = 0; i < kNumStems; ++i) {
        Slot& sl = slots_[i];
        sl.loaded = false; sl.audio = wav::Audio{}; sl.peaks.clear(); sl.name.clear(); sl.path.clear();
        if (s.paths[i].empty()) continue;
        wav::Audio a;
        if (auto e = wav::read(std::filesystem::path(s.paths[i]), a); !e.empty()) { missing += narrow(s.paths[i]) + "\n"; continue; }
        if (rate == 0.0) rate = a.sampleRate;
        if (a.sampleRate != rate) { missing += narrow(s.paths[i]) + " (different sample rate)\n"; continue; }
        sl.loaded = true; sl.path = s.paths[i]; sl.name = stemNameFromPath(s.paths[i]);
        sl.audio = std::move(a); sl.peaks = computePeaks(sl.audio, 2048);
    }
    bpm_ = s.bpm; target_ = s.targetLufs; normalize_ = s.normalize; preset_ = std::clamp(s.preset, 0, kNumPresets - 1);
    rebuildEngine(rate > 0 ? rate : sr_, &s);
    eng_->seek(0);
    sessionPath_ = files[0];
    if (!missing.empty()) MessageBoxW(hwnd_, widen("These stems couldn't be opened:\n\n" + missing).c_str(), L"Hybrid Core", MB_ICONWARNING);
    toast("Session opened");
}

void App::exportMaster() {
    if (exporting_) { exportCancel_ = true; toast("Cancelling export…"); return; }
    if (loadedCount() == 0) { toast("Load some stems first"); return; }
    const std::wstring def = widen(songTitle()) + L" - HEM Master";
    const std::wstring path = saveFileDialog(hwnd_, L"Export Master (24-bit WAV)", {L"WAV audio (*.wav)", L"*.wav"}, L"wav", def);
    if (path.empty()) return;
    RenderJob job;
    job.sampleRate = sr_;
    for (std::size_t i = 0; i < kNumStems; ++i) {
        const Slot& s = slots_[i];
        if (!s.loaded) continue;
        StemJob sj;
        sj.L = s.audio.ch[0].data(); sj.R = s.audio.ch[1].data(); sj.frames = s.audio.frames();
        sj.chain = captureStem(static_cast<int>(i));
        const StripState st = stripOf(static_cast<int>(i));
        sj.faderDb = st.faderDb; sj.pan = st.pan; sj.mute = st.mute; sj.solo = st.solo;
        job.stems.push_back(std::move(sj));
    }
    job.master = captureMaster();
    job.normalize = normalize_;
    job.targetLufs = target_;
    if (exportThread_.joinable()) exportThread_.join();
    exportPath_ = path;
    exportCancel_ = false;
    exportProgress_ = 0.0;
    exporting_ = true;
    HWND hwnd = hwnd_;
    exportThread_ = std::thread([this, job = std::move(job), path, hwnd]() mutable {
        exportResult_ = renderMix(job, [this](double p) { exportProgress_ = p; return !exportCancel_.load(); });
        exportError_.clear();
        if (exportResult_.ok) {
            if (auto e = wav::write(std::filesystem::path(path), exportResult_.audio, false); !e.empty()) exportError_ = e;
            exportResult_.audio = wav::Audio{};
        }
        PostMessageW(hwnd, WM_EXPORT_DONE, 0, 0);
    });
}

void App::onExportDone() {
    if (exportThread_.joinable()) exportThread_.join();
    exporting_ = false;
    if (exportResult_.cancelled) { toast("Export cancelled"); return; }
    if (!exportResult_.ok || !exportError_.empty()) {
        MessageBoxW(hwnd_, widen("Export failed: " + (exportError_.empty() ? exportResult_.error : exportError_)).c_str(), L"Hybrid Core", MB_ICONERROR);
        return;
    }
    const auto& r = exportResult_.reading;
    std::string msg = "Master exported (24-bit WAV).\n\n";
    msg += "Integrated:   " + lufsStr(r.integrated) + " LUFS" + (normalize_ ? fmt("   (target %.1f)", target_) : std::string("   (not normalized)")) + "\n";
    msg += "True peak:    " + fmt("%.2f dBTP", r.truePeakDb) + "\n";
    msg += "Short-term max: " + lufsStr(r.maxShortTerm) + " LUFS\n";
    msg += "LRA:          " + fmt("%.1f LU", r.lra) + "\n\n" + narrow(exportPath_);
    MessageBoxW(hwnd_, widen(msg).c_str(), L"Hybrid Core — Export complete", MB_ICONINFORMATION);
}

// ====================================================================================== menus
enum MenuId {
    ID_LOAD = 100, ID_CLEAR_ALL, ID_OPEN_SESSION, ID_SAVE_SESSION, ID_EXPORT, ID_EXIT,
    ID_SHARED, ID_EXCLUSIVE, ID_RESET_METERS, ID_SLOT_LOAD, ID_SLOT_CLEAR, ID_SLOT_RESET, ID_VIEW_TOGGLE
};

void App::showMainMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_LOAD, L"Load Stems…\tCtrl+L");
    AppendMenuW(m, MF_STRING | (loadedCount() ? 0 : MF_GRAYED), ID_CLEAR_ALL, L"Clear All Stems");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_OPEN_SESSION, L"Open Session…\tCtrl+O");
    AppendMenuW(m, MF_STRING, ID_SAVE_SESSION, L"Save Session…\tCtrl+S");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (loadedCount() ? 0 : MF_GRAYED), ID_EXPORT, exporting_ ? L"Cancel Export" : L"Export Master…\tCtrl+E");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_VIEW_TOGGLE, fullView_ ? L"Console View (hide title art)" : L"Full Faceplate View");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    POINT p; GetCursorPos(&p);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    switch (cmd) {
    case ID_LOAD: loadFiles(openFilesDialog(hwnd_, L"Load Stems (up to 13 WAV files)", {L"WAV audio (*.wav)", L"*.wav"}, true)); break;
    case ID_CLEAR_ALL: clearAll(); break;
    case ID_OPEN_SESSION: openSession(); break;
    case ID_SAVE_SESSION: saveSessionAs(); break;
    case ID_EXPORT: exportMaster(); break;
    case ID_VIEW_TOGGLE: fullView_ = !fullView_; break;
    case ID_EXIT: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    default: break;
    }
}

void App::showSettingsMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (exclusive_ ? 0 : MF_CHECKED), ID_SHARED, L"Audio: Shared (plays with other apps)");
    AppendMenuW(m, MF_STRING | (exclusive_ ? MF_CHECKED : 0), ID_EXCLUSIVE, L"Audio: Exclusive (lowest latency)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, widen(deviceDesc_).c_str());
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, widen("Graphics: " + gfx_.adapterName()).c_str());
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, widen(fmt("Plugin latency: %.0f samples", eng_ ? eng_->totalLatency() : 0)).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_RESET_METERS, L"Reset Loudness Meters");
    POINT p; GetCursorPos(&p);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd == ID_SHARED || cmd == ID_EXCLUSIVE) {
        const bool want = cmd == ID_EXCLUSIVE;
        if (want != exclusive_) { stopAudio(); exclusive_ = want; startAudio(); toast(deviceDesc_); }
    } else if (cmd == ID_RESET_METERS && eng_) { eng_->requestMeterReset(); toast("Meters reset"); }
}

void App::showAbout() {
    const std::string s = std::string("THE HYBRID CORE ") + kVersion + "\nHybrid Elite Mafia Studio Suite\n\n"
        "Scratch-built C++20 audio engine · WASAPI · AVX2 · Direct3D 11\n\n"
        "Keys:\n  Space  play / stop\n  Home  back to start\n  Ctrl+L  load stems    Ctrl+E  export master\n"
        "  Ctrl+O / Ctrl+S  open / save session\n\n"
        "Knobs: drag up/down (Shift = fine), mouse wheel, double-click = default.\n"
        "Track name: click = select, drag left/right = volume, double-click = 0 dB, right-click = menu.\n"
        "Plugin titles: click to bypass.";
    MessageBoxW(hwnd_, widen(s).c_str(), L"About Hybrid Core", MB_ICONINFORMATION);
}

void App::stemContextMenu(int s) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_SLOT_LOAD, slots_[static_cast<std::size_t>(s)].loaded ? L"Replace Stem…" : L"Load Stem…");
    AppendMenuW(m, MF_STRING | (slots_[static_cast<std::size_t>(s)].loaded ? 0 : MF_GRAYED), ID_SLOT_CLEAR, L"Remove Stem");
    AppendMenuW(m, MF_STRING, ID_SLOT_RESET, L"Reset Volume to 0 dB");
    POINT p; GetCursorPos(&p);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd == ID_SLOT_LOAD) loadFiles(openFilesDialog(hwnd_, L"Load Stem", {L"WAV audio (*.wav)", L"*.wav"}, false), s);
    else if (cmd == ID_SLOT_CLEAR) clearSlot(s);
    else if (cmd == ID_SLOT_RESET) eng_->stem(static_cast<std::size_t>(s)).faderDb.set(0);
}

// ====================================================================================== widgets
bool App::button(int id, const Rect& r) {
    const bool hot = r.contains(in_.mx, in_.my);
    if (hot) hotId_ = id;
    if (hot && in_.pressed) { activeId_ = id; return false; }
    if (activeId_ == id && in_.released) { activeId_ = 0; return hot; }
    return false;
}

float App::knob(int id, const LY::Knob& k, float value, float mn, float mx, Taper taper, float def, bool bipolar,
                Color accent, const std::string& valueText, bool dim) {
    auto toNorm = [&](float v) {
        if (taper == Taper::Log && mn > 0) return std::log(v / mn) / std::log(mx / mn);
        return (v - mn) / (mx - mn);
    };
    auto fromNorm = [&](float n) {
        n = clampf(n, 0, 1);
        if (taper == Taper::Log && mn > 0) return mn * std::pow(mx / mn, n);
        return mn + n * (mx - mn);
    };
    const float r = k.r - 8.0f;
    const float dx = in_.mx - k.cx, dy = in_.my - k.cy;
    const bool hot = dx * dx + dy * dy <= r * r;
    float norm = clampf(toNorm(value), 0, 1);
    if (hot) hotId_ = id;
    if (hot && in_.dbl) { norm = clampf(toNorm(def), 0, 1); activeId_ = 0; }
    else if (hot && in_.pressed) { activeId_ = id; dragStartY_ = in_.devY; dragStartVal_ = norm; }
    if (activeId_ == id) {
        if (in_.down) norm = clampf(dragStartVal_ + (dragStartY_ - in_.devY) / (in_.shift ? 1200.0f : 240.0f), 0, 1);
        if (in_.released) activeId_ = 0;
    }
    if (hot && in_.wheel != 0) norm = clampf(norm + in_.wheel * (in_.shift ? 0.005f : 0.025f), 0, 1);
    const float out = fromNorm(norm);

    // ---- draw: tick dots, track ring, value arc, metal body, pointer ----
    const float a0 = 0.75f * 3.14159265f, sweep = 1.5f * 3.14159265f;
    const float ringR = k.r - 12.0f, bodyR = k.r - 22.0f;
    const float alpha = dim ? 0.35f : 1.0f;
    for (int i = 0; i <= 10; ++i) {
        const float a = a0 + sweep * i / 10.0f;
        gfx_.circle(k.cx + (k.r - 3.0f) * std::cos(a), k.cy + (k.r - 3.0f) * std::sin(a), i % 5 == 0 ? 2.2f : 1.4f, Color(0x8A9AA6FF).alpha(0.7f * alpha));
    }
    gfx_.arc(k.cx, k.cy, ringR, a0, a0 + sweep, 6.0f, Color(0x16232BFF).alpha(alpha));
    const float va = a0 + sweep * norm;
    const float from = bipolar ? a0 + sweep * 0.5f : a0;
    if (std::fabs(va - from) > 0.01f) {
        gfx_.arc(k.cx, k.cy, ringR, std::min(from, va), std::max(from, va), 12.0f, accent.alpha(0.18f * alpha));
        gfx_.arc(k.cx, k.cy, ringR, std::min(from, va), std::max(from, va), 5.0f, accent.alpha(alpha));
    }
    gfx_.circle(k.cx, k.cy + 2.0f, bodyR + 3.0f, Color(0x000000FF).alpha(0.6f));
    gfx_.circleGradient(k.cx, k.cy, bodyR + 1.5f, Color(0x2A2E33FF), Color(0x0B0D10FF));
    gfx_.circleGradient(k.cx, k.cy, bodyR - 1.0f, Color(0xD5DADFFF).alpha(alpha), Color(0x3A3F45FF).alpha(alpha));
    gfx_.circleGradient(k.cx, k.cy, bodyR - 7.0f, Color(0x555B62FF).alpha(alpha), Color(0xBFC4CAFF).alpha(alpha));
    const float px = std::cos(va), py = std::sin(va);
    gfx_.line(k.cx + px * bodyR * 0.25f, k.cy + py * bodyR * 0.25f, k.cx + px * (bodyR - 3.0f), k.cy + py * (bodyR - 3.0f), 3.5f, Color(0xFFFFFFFF).alpha(alpha));
    if (hot || activeId_ == id) { tooltip_ = valueText; tipX_ = k.cx; tipY_ = k.cy - k.r - 6.0f; }
    return out;
}

// ====================================================================================== frame
void App::updateTransform() {
    const LY::Box v = fullView_ ? LY::view_full : LY::view_console;
    const float W = static_cast<float>(gfx_.width()), H = static_cast<float>(gfx_.height());
    const float s = std::min(W / (v.x1 - v.x0), H / (v.y1 - v.y0));
    viewScale_ = s;
    viewOffX_ = 0.5f * (W - (v.x1 - v.x0) * s) - v.x0 * s;
    viewOffY_ = 0.5f * (H - (v.y1 - v.y0) * s) - v.y0 * s;
    gfx_.setTransform(s, viewOffX_, viewOffY_);
    gfx_.setSkinView({v.x0, v.y0, v.x1, v.y1});
}

void App::frame() {
    updateTransform();
    POINT p; GetCursorPos(&p); ScreenToClient(hwnd_, &p);
    in_.devX = static_cast<float>(p.x); in_.devY = static_cast<float>(p.y);
    in_.mx = (in_.devX - viewOffX_) / viewScale_;
    in_.my = (in_.devY - viewOffY_) / viewScale_;
    in_.down = (GetKeyState(VK_LBUTTON) & 0x8000) != 0;
    in_.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    in_.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (!in_.down && activeId_ && !in_.released) in_.released = true;   // button released outside the window
    hotId_ = 0;
    tooltip_.clear();

    // Pending chain rebuilds (resolution changes) — retry until the audio thread takes them.
    for (std::size_t i = 0; i < kNumStems; ++i)
        if (rebuildPending_[i]) { rebuildPending_[i] = false; publishStemChain(static_cast<int>(i), &rebuildState_[i]); }
    if (eng_) eng_->collectGarbage();

    // Meters.
    for (std::size_t i = 0; i < kNumStems; ++i) {
        auto& tb = eng_->stem(i).snapshot;
        tb.update();
        slots_[i].meter = tb.read_buffer();
        const float pk = std::max(slots_[i].meter.peakDb[0], slots_[i].meter.peakDb[1]);
        const float lv = clampf((pk + 48.0f) / 48.0f, 0, 1);
        slots_[i].dotLevel = std::max(lv, slots_[i].dotLevel * 0.9f);
    }
    eng_->masterSnapshot().update();
    master_ = eng_->masterSnapshot().read_buffer();

    gfx_.beginFrame(0x040506FF);
    drawTransport();
    drawArrange();
    drawDestroyer();
    drawResonance();
    drawTransient();
    drawMastering();
    drawOverlays();
    gfx_.endFrame();

    in_.pressed = in_.released = in_.dbl = in_.rpressed = false;
    in_.wheel = 0;
    // Stop at the end of the song.
    if (eng_->isPlaying() && loadedCount() && eng_->playhead() > songLength() + eng_->totalLatency() + static_cast<std::uint64_t>(sr_ * 0.25))
        eng_->stop();
}

// ------------------------------------------------------------------ transport
void App::drawTransport() {
    const bool playing = eng_->isPlaying();
    // Painted buttons stay; we light them and make them clickable.
    if (button(1, R(LY::btnMenu)) || button(2, R(LY::btnTopLeft))) showMainMenu();
    if (button(3, R(LY::btnRewind))) { eng_->seek(0); }
    if (button(4, R(LY::btnPlay))) {
        if (!loadedCount()) toast("Load some stems first (menu ≡ → Load Stems, or drop WAVs here)");
        else eng_->play();
    }
    if (button(5, R(LY::btnStop))) { if (!playing) eng_->seek(0); eng_->stop(); }
    if (button(6, R(LY::btnRecord))) exportMaster();
    if (button(7, R(LY::btnAnalyze))) { analyze_ = !analyze_; toast(analyze_ ? "Analyze: per-stem loudness shown on the lanes" : "Analyze off"); }
    if (button(8, R(LY::btnSettings))) showSettingsMenu();
    if (button(9, R(LY::btnView))) fullView_ = !fullView_;
    if (button(10, R(LY::btnTopRight))) showAbout();

    if (playing) gfx_.roundRect(R(LY::btnPlay).inset(2), 6, Color(kCyan).alpha(0.22f));
    if (exporting_) {
        const float blink = 0.5f + 0.5f * std::sin(static_cast<float>(now()) * 6.0f);
        gfx_.roundRect(R(LY::btnRecord).inset(2), 6, Color(kRed).alpha(0.15f + 0.35f * blink));
    }
    if (hotId_ >= 1 && hotId_ <= 10) {
        const LY::Box all[] = {LY::btnMenu, LY::btnTopLeft, LY::btnRewind, LY::btnPlay, LY::btnStop,
                               LY::btnRecord, LY::btnAnalyze, LY::btnSettings, LY::btnView, LY::btnTopRight};
        gfx_.roundRect(R(all[hotId_ - 1]).inset(1), 6, Color(0xFFFFFFFF).alpha(activeId_ == hotId_ ? 0.12f : 0.06f));
    }
    if (analyze_) gfx_.roundRect(R(LY::btnAnalyze).inset(2), 6, Color(kCyan).alpha(0.18f));

    // Time display (audible position = engine playhead minus plugin latency).
    const double lat = eng_->totalLatency();
    const double pos = std::max(0.0, static_cast<double>(eng_->playhead()) - (playing ? lat : 0.0)) / sr_;
    gfx_.textIn(R(LY::timeDisplay), timeStr(pos), Font::Huge, kCyan);

    // Tempo box: drag up/down to change, double-click to re-estimate.
    const Rect tb = R(LY::tempoDisplay);
    const bool tHot = tb.contains(in_.mx, in_.my);
    if (tHot) hotId_ = 20;
    if (tHot && in_.dbl && loadedCount()) {
        std::vector<const wav::Audio*> v;
        for (auto& s : slots_) if (s.loaded) v.push_back(&s.audio);
        bpm_ = estimateTempo(v, sr_);
        toast(fmt("Tempo re-estimated: %.1f BPM", bpm_));
    } else if (tHot && in_.pressed) { activeId_ = 20; dragStartY_ = in_.devY; dragStartVal_ = static_cast<float>(bpm_); }
    if (activeId_ == 20) {
        if (in_.down) bpm_ = std::clamp(std::round((dragStartVal_ + (dragStartY_ - in_.devY) * (in_.shift ? 0.02f : 0.2f)) * 10.0) / 10.0, 40.0, 240.0);
        if (in_.released) activeId_ = 0;
    }
    if (tHot && in_.wheel != 0) bpm_ = std::clamp(bpm_ + in_.wheel * (in_.shift ? 0.1 : 1.0), 40.0, 240.0);
    gfx_.textIn({tb.x0, tb.y0 + 2, tb.x1, tb.y0 + 30}, fmt("%.2f", bpm_), Font::Large, kText);
    gfx_.textIn({tb.x0, tb.y0 + 28, tb.x1, tb.y1 - 2}, "4/4", Font::Small, kDim);
    if (tHot || activeId_ == 20) { tooltip_ = "Tempo (estimate) · drag / wheel to set · double-click to re-detect"; tipX_ = tb.cx(); tipY_ = tb.y1 + 34; }

    // Master output level meter: two rows of segments, peak with 20 dB/s fall.
    const Rect lm = R(LY::levelMeter);
    const int segs = 36;
    for (int ch = 0; ch < 2; ++ch) {
        const float pk = master_.peakDb[ch];
        lvl_[ch] = std::max(std::isfinite(pk) ? pk : -90.0f, lvl_[ch] - 0.33f);
        const float y0 = lm.y0 + 9 + ch * 16.0f;
        const float sw = (lm.w() - 16) / segs;
        for (int i = 0; i < segs; ++i) {
            const float db = -48.0f + 48.0f * (i + 1) / segs;
            const bool lit = lvl_[ch] >= db - 48.0f / segs;
            Color c = db > -3 ? kRed : (db > -9 ? kOrange : (db > -18 ? kYellow : kCyan));
            gfx_.rect({lm.x0 + 8 + i * sw, y0, lm.x0 + 8 + (i + 1) * sw - 2.0f, y0 + 12}, lit ? c : Color(c).alpha(0.10f));
        }
    }
}

// ------------------------------------------------------------------ arrange
void App::drawArrange() {
    const Rect lanes = R(LY::lanes), ruler = R(LY::ruler);
    const float lx0 = lanes.x0 + 4, lx1 = lanes.x1 - 4;
    const std::uint64_t len = songLength();
    const double secs = len ? static_cast<double>(len) / sr_ : 0.0;
    gfx_.rect(lanes, kScreen);
    gfx_.rect(ruler, 0x0A1117FF);
    gfx_.line(ruler.x0, ruler.y1, ruler.x1, ruler.y1, 1.5f, 0x1E2C36FF);

    // Bar grid + ruler numbers (from the tempo box).
    if (len) {
        const double barSec = 4.0 * 60.0 / bpm_;
        const int bars = static_cast<int>(secs / barSec) + 1;
        const float pxPerBar = static_cast<float>((lx1 - lx0) * barSec / secs);
        int every = 1;
        while (every * pxPerBar < 44.0f) every *= 2;
        for (int b = 0; b <= bars; b += every) {
            const float x = lx0 + static_cast<float>(b * barSec / secs) * (lx1 - lx0);
            if (x > lx1) break;
            gfx_.line(x, lanes.y0, x, lanes.y1, 1.0f, Color(0x1A2730FF));
            gfx_.line(x, ruler.y1 - 8, x, ruler.y1, 1.5f, kDim);
            gfx_.text(x + 4, ruler.y0 + 4, std::to_string(b + 1), Font::Small, kText);
        }
    }

    // Seek by clicking / dragging the ruler or lanes.
    const Rect seekArea{lx0, ruler.y0, lx1, lanes.y1};
    if (len && seekArea.contains(in_.mx, in_.my) && in_.pressed) activeId_ = 30;
    if (activeId_ == 30) {
        const double f = clampf((in_.mx - lx0) / (lx1 - lx0), 0, 1);
        if (in_.down) eng_->seek(static_cast<std::uint64_t>(f * static_cast<double>(len)));
        if (in_.released) activeId_ = 0;
    }

    // Rows.
    bool anySolo = false;
    for (std::size_t i = 0; i < kNumStems; ++i) anySolo |= eng_->stem(i).solo.load();
    for (int i = 0; i < static_cast<int>(kNumStems); ++i) {
        Slot& s = slots_[static_cast<std::size_t>(i)];
        auto& st = eng_->stem(static_cast<std::size_t>(i));
        const float cy = LY::kRow0 + LY::kRowStep * static_cast<float>(i);
        const Rect nameR{104, cy - 19, 238, cy + 19};
        const bool isSel = i == selected_;
        gfx_.rect(nameR, isSel ? Color(0x0F2A36FF) : Color(0x101519FF));
        if (isSel) gfx_.rect({nameR.x0, nameR.y0, nameR.x0 + 3, nameR.y1}, kCyan);
        const Color base = stemColorIsOrange(i) ? kOrange : kCyan;
        const bool audible = !st.mute.load() && (!anySolo || st.solo.load());
        gfx_.circle(116, cy - 3, 9.0f, base.alpha(0.12f + 0.45f * s.dotLevel));
        gfx_.circle(116, cy - 3, 5.5f, s.loaded ? base : Color(0x33414BFF));
        gfx_.textIn({128, cy - 17, 236, cy + 8}, s.loaded ? shownName(i) : std::string("empty"), Font::Normal, s.loaded ? kText : Color(0x46555FFF), Align::Left);
        // volume bar (fader −60…+6 dB)
        const float fdb = st.faderDb.target();
        const float vf = clampf((fdb + 60.0f) / 66.0f, 0, 1);
        gfx_.rect({128, cy + 11, 234, cy + 14}, 0x1C2830FF);
        gfx_.rect({128, cy + 11, 128 + 106 * vf, cy + 14}, s.loaded ? base.alpha(0.8f) : Color(0x2A3640FF));
        gfx_.rect({128 + 106 * (60.0f / 66.0f) - 0.75f, cy + 9, 128 + 106 * (60.0f / 66.0f) + 0.75f, cy + 16}, kDim);

        const int nid = 100 + i;
        const bool hot = nameR.contains(in_.mx, in_.my);
        if (hot) hotId_ = nid;
        if (hot && in_.dbl) {
            if (!s.loaded) loadFiles(openFilesDialog(hwnd_, L"Load Stem", {L"WAV audio (*.wav)", L"*.wav"}, false), i);
            else st.faderDb.set(0.0f);
            activeId_ = 0;
        } else if (hot && in_.pressed) { activeId_ = nid; dragStartX_ = in_.devX; dragStartVal_ = fdb; dragMoved_ = false; }
        if (hot && in_.rpressed) { selected_ = i; stemContextMenu(i); }
        if (activeId_ == nid) {
            if (in_.down && std::fabs(in_.devX - dragStartX_) > 3.0f) dragMoved_ = true;
            if (dragMoved_) st.faderDb.set(clampf(dragStartVal_ + (in_.devX - dragStartX_) * (in_.shift ? 0.02f : 0.15f), -60.0f, 6.0f));
            if (in_.released) { if (!dragMoved_) selected_ = i; activeId_ = 0; }
        }
        if (hot && in_.wheel != 0) st.faderDb.set(clampf(fdb + in_.wheel * (in_.shift ? 0.1f : 1.0f), -60.0f, 6.0f));
        if (hot || activeId_ == nid) {
            tooltip_ = s.loaded ? (fdb <= -59.9f ? std::string("-inf dB") : fmt("%+.1f dB", fdb)) + "  ·  drag = volume" : "double-click to load a stem here";
            tipX_ = 171; tipY_ = cy - 22;
        }

        // M / S
        const Rect mR{LY::rowM.x0, cy - 13, LY::rowM.x1, cy + 13}, sR{LY::rowS.x0, cy - 13, LY::rowS.x1, cy + 13};
        if (button(200 + i, mR)) st.mute.store(!st.mute.load());
        if (button(220 + i, sR)) st.solo.store(!st.solo.load());
        if (st.mute.load()) { gfx_.roundRect(mR.inset(1), 3, Color(kYellow).alpha(0.9f)); gfx_.textIn(mR, "M", Font::Normal, 0x101010FF); }
        if (st.solo.load()) { gfx_.roundRect(sR.inset(1), 3, Color(kGreen).alpha(0.9f)); gfx_.textIn(sR, "S", Font::Normal, 0x101010FF); }

        // Lane + waveform
        const Rect lane{lx0, cy - 18, lx1, cy + 18};
        gfx_.rect(lane, i % 2 ? Color(0x0B1218FF) : Color(0x0D151CFF));
        if (isSel) gfx_.rectOutline(lane, Color(kCyan).alpha(0.25f), 1.5f);
        if (s.loaded && !s.peaks.empty() && len) {
            const float wfrac = static_cast<float>(static_cast<double>(s.audio.frames()) / static_cast<double>(len));
            const float x1 = lx0 + (lx1 - lx0) * wfrac;
            const int cols = static_cast<int>((x1 - lx0) / 1.5f);
            Color wc = audible ? base : Color(0x3A464FFF);
            for (int c = 0; c < cols; ++c) {
                const std::size_t b0 = s.peaks.size() * static_cast<std::size_t>(c) / static_cast<std::size_t>(cols);
                const std::size_t b1 = std::max(b0 + 1, s.peaks.size() * static_cast<std::size_t>(c + 1) / static_cast<std::size_t>(cols));
                float m = 0;
                for (std::size_t b = b0; b < b1 && b < s.peaks.size(); ++b) m = std::max(m, s.peaks[b]);
                const float h = 16.0f * std::sqrt(clampf(m, 0, 1));
                const float x = lx0 + 1.5f * c;
                gfx_.rect({x, cy - h, x + 1.2f, cy + h}, wc.alpha(0.85f));
            }
        }
        if (analyze_ && s.loaded) {
            const auto& r = s.meter.loudness;
            const std::string t = "S " + lufsStr(r.shortTerm) + " LUFS   I " + lufsStr(r.integrated) + fmt("   TP %.1f", r.truePeakDb);
            const float tw = gfx_.textWidth(t, Font::Small);
            gfx_.roundRect({lx1 - tw - 16, cy - 11, lx1 - 4, cy + 11}, 4, 0x000000C0);
            gfx_.text(lx1 - tw - 10, cy - 9, t, Font::Small, kText);
        }
    }

    // Empty-state hint.
    if (!loadedCount()) {
        const Rect hint{lanes.x0 + 120, lanes.cy() - 50, lanes.x1 - 120, lanes.cy() + 50};
        gfx_.roundRect(hint, 10, 0x000000B0);
        gfx_.textIn({hint.x0, hint.y0 + 8, hint.x1, hint.cy()}, "DROP UP TO 13 WAV STEMS HERE", Font::Large, kCyan);
        gfx_.textIn({hint.x0, hint.cy(), hint.x1, hint.y1 - 8}, "or click the menu button → Load Stems", Font::Normal, kDim);
    }

    // Playhead.
    if (len) {
        const double lat = eng_->totalLatency();
        const double pos = std::max(0.0, static_cast<double>(eng_->playhead()) - (eng_->isPlaying() ? lat : 0.0));
        const float x = lx0 + static_cast<float>(std::min(1.0, pos / static_cast<double>(len))) * (lx1 - lx0);
        gfx_.line(x, ruler.y0 + 6, x, lanes.y1, 2.0f, kWhite);
        const float t[] = {x - 9, ruler.y0 + 2, x + 9, ruler.y0 + 2, x, ruler.y0 + 14};
        gfx_.line(t[0], t[1], t[2], t[3], 3, kWhite);
        gfx_.line(t[0], t[1], t[4], t[5], 3, kWhite);
        gfx_.line(t[2], t[3], t[4], t[5], 3, kWhite);
    }
}

// ------------------------------------------------------------------ STFT DESTROYER
void App::drawDestroyer() {
    ArtifactProcessor* ds = sel().ds;
    if (!ds) return;
    const bool by = ds->bypassed();
    if (button(300, R(LY::dsTitle)) || button(301, R(LY::dsToggle))) ds->setBypassed(!by);
    if (by) {
        gfx_.rect(R(LY::dsTitle), 0x000000A0);
        gfx_.text(LY::dsTitle.x0 + 6, LY::dsTitle.y0 + 8, "BYPASSED", Font::Normal, kRed);
    }
    // Header selector: repair strength LIGHT / MEDIUM / HEAVY (shimmer amount).
    static const char* modes[] = {"LIGHT", "MEDIUM", "HEAVY"};
    static const float amounts[] = {0.35f, 0.6f, 1.0f};
    const float amt = ds->param(ArtifactProcessor::kShimmer).target();
    int mode = amt < 0.47f ? 0 : (amt < 0.8f ? 1 : 2);
    if (button(302, R(LY::dsPrev))) mode = (mode + 2) % 3, ds->param(ArtifactProcessor::kShimmer).set(amounts[mode]);
    if (button(303, R(LY::dsNext)) || button(304, R(LY::dsHeaderBox))) mode = (mode + 1) % 3, ds->param(ArtifactProcessor::kShimmer).set(amounts[mode]);
    gfx_.textIn(R(LY::dsHeaderBox), modes[mode], Font::Normal, kText);

    // Spectrogram of the selected stem (post-processing, live while playing).
    if (specFor_ != selected_) {
        spec_.prepare(sr_);
        float junk[2048];
        auto& tap = eng_->stem(static_cast<std::size_t>(selected_)).tap;
        while (tap.pop_n(junk, 2048) > 0) {}
        specFor_ = selected_;
    }
    {
        float buf[2048];
        std::size_t n;
        auto& tap = eng_->stem(static_cast<std::size_t>(selected_)).tap;
        const bool live = eng_->isPlaying();   // freeze the picture when stopped
        while ((n = tap.pop_n(buf, 2048)) > 0) if (live) spec_.feed(buf, n);
    }
    const Rect sc = R(LY::dsScreen).inset(3);
    const Rect area{sc.x0, sc.y0, sc.x1 - 16, sc.y1};
    const float cw = area.w() / Spectrogram::kCols, ch = area.h() / Spectrogram::kRows;
    for (int c = 0; c < Spectrogram::kCols; ++c) {
        const int col = (spec_.head + c) % Spectrogram::kCols;
        for (int r = 0; r < Spectrogram::kRows; ++r) {
            const float v = (spec_.cell[col][r] + 110.0f) / 95.0f;
            if (v <= 0.02f) continue;
            const float x = area.x0 + c * cw, y = area.y1 - (r + 1) * ch;
            gfx_.rect({x, y, x + cw + 0.4f, y + ch + 0.4f}, heat(v));
        }
    }
    for (int i = 0; i < 40; ++i) {   // colour bar
        const float y0 = sc.y1 - (i + 1) * sc.h() / 40.0f;
        gfx_.rect({sc.x1 - 10, y0, sc.x1 - 3, y0 + sc.h() / 40.0f + 0.5f}, heat(static_cast<float>(i) / 39.0f));
    }
    for (double f : {100.0, 1000.0, 10000.0}) {
        const float y = area.y1 - static_cast<float>(std::log(f / 40.0) / std::log(20000.0 / 40.0)) * area.h();
        gfx_.line(area.x0, y, area.x0 + 10, y, 1.0f, kDim);
        gfx_.text(area.x0 + 13, y - 9, f >= 1000 ? fmt("%.0fk", f / 1000) : fmt("%.0f", f), Font::Small, Color(kDim).alpha(0.9f));
    }
    const std::string selName = sel().loaded ? shownName(selected_) : std::string();
    gfx_.text(area.x1 - gfx_.textWidth(selName, Font::Small) - 6, area.y0 + 4, selName, Font::Small, kText);
    const auto& t = ds->telemetry();
    gfx_.roundRect({area.x0 + 62, area.y1 - 24, area.x0 + 470, area.y1 - 2}, 4, 0x000000B0);
    gfx_.text(area.x0 + 70, area.y1 - 22, fmt("HF jitter %.1f dB", t.hfJitterDb.load()) + fmt("   HF change %+.1f dB", t.shimmerCutDb.load()) + fmt("   coherence %.2f", t.hfCoherence.load()),
              Font::Small, Color(kText).alpha(0.85f));

    // Resolution selector (rebuilds this stem's chain off the audio thread).
    static const char* res[] = {"1024", "2048", "4096"};
    int ri = static_cast<int>(std::lround(ds->param(ArtifactProcessor::kRes).target()));
    int nri = ri;
    if (button(305, R(LY::dsResPrev))) nri = (ri + 2) % 3;
    if (button(306, R(LY::dsResNext)) || button(307, R(LY::dsResBox))) nri = (ri + 1) % 3;
    if (nri != ri) {
        ChainState cs = captureStem(selected_);
        for (auto& p : cs) if (p.id == "hc.artifact") p.values[ArtifactProcessor::kRes] = static_cast<float>(nri);
        rebuildState_[static_cast<std::size_t>(selected_)] = cs;
        rebuildPending_[static_cast<std::size_t>(selected_)] = true;
        ri = nri;
        toast(fmt("STFT resolution %.0f (latency changes; all stems stay aligned)", static_cast<double>(1024 << ri)));
    }
    gfx_.textIn(R(LY::dsResBox), res[std::clamp(ri, 0, 2)], Font::Normal, kText);

    // Toggle switch (bypass) — lever up = on.
    const Rect tg = R(LY::dsToggle);
    gfx_.roundRect(tg.inset(4), 4, 0x0C0F12FF);
    gfx_.roundRect({tg.x0 + 7, by ? tg.cy() : tg.y0 + 6, tg.x1 - 7, by ? tg.y1 - 6 : tg.cy()}, 3, by ? Color(0x5A6168FF) : kCyan);

    auto& thr = ds->param(ArtifactProcessor::kShimTol);
    auto& bl = ds->param(ArtifactProcessor::kPhase);
    auto& dc = ds->param(ArtifactProcessor::kDecay);
    thr.set(knob(310, LY::dsThreshold, thr.target(), 0, 12, Taper::Linear, 1.5f, false, kCyan, fmt("Threshold %.1f dB above envelope", thr.target()), by));
    bl.set(knob(311, LY::dsBleed, bl.target(), 0, 1, Taper::Linear, 0.3f, false, kCyan, fmt("Bleed (HF stereo phase fix) %.0f %%", bl.target() * 100), by));
    dc.set(knob(312, LY::dsDecay, dc.target(), 5, 200, Taper::Log, 10, false, kCyan, fmt("Decay %.0f ms", dc.target()), by));
}

// ------------------------------------------------------------------ RESONANCE
void App::drawResonance() {
    ResonanceProcessor* rs = sel().rs;
    if (!rs) return;
    const bool by = rs->bypassed();
    if (button(400, R(LY::rsTitle))) rs->setBypassed(!by);
    if (by) { gfx_.rect(R(LY::rsTitle), 0x000000A0); gfx_.text(LY::rsTitle.x0 + 6, LY::rsTitle.y0 + 8, "BYPASSED", Font::Normal, kRed); }
    static const char* modes[] = {"SOFT", "NORMAL", "FAST"};
    auto& mp = rs->param(ResonanceProcessor::kMode);
    int m = static_cast<int>(std::lround(mp.target()));
    if (button(401, R(LY::rsPrev))) mp.set(static_cast<float>((m + 2) % 3));
    if (button(402, R(LY::rsNext)) || button(403, R(LY::rsHeaderBox))) mp.set(static_cast<float>((m + 1) % 3));
    gfx_.textIn(R(LY::rsHeaderBox), modes[std::clamp(static_cast<int>(std::lround(mp.target())), 0, 2)], Font::Normal, kText);

    if (rs->view().update()) resView_ = rs->view().read_buffer();
    const Rect sc = R(LY::rsScreen).inset(6);
    auto xOf = [&](double f) { return sc.x0 + static_cast<float>(std::log(f / 20.0) / std::log(1000.0)) * sc.w(); };
    // focus band
    const float fc = rs->param(ResonanceProcessor::kFrequency).target(), bw = rs->param(ResonanceProcessor::kBandwidth).target();
    const float fx0 = std::max(sc.x0, xOf(fc / std::pow(2.0, bw * 0.5))), fx1 = std::min(sc.x1, xOf(fc * std::pow(2.0, bw * 0.5)));
    gfx_.rect({fx0, sc.y0, fx1, sc.y1}, Color(kCyan).alpha(by ? 0.03f : 0.07f));
    for (double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
        const float x = xOf(f);
        gfx_.line(x, sc.y0, x, sc.y1, 1.0f, 0x15222BFF);
        gfx_.text(x + 3, sc.y1 - 18, f >= 1000 ? fmt("%.0fk", f / 1000) : fmt("%.0f", f), Font::Small, Color(kDim).alpha(0.8f));
    }
    // spectrum (cyan fill)
    const std::size_t B = ResonanceView::kBands;
    std::vector<float> pts(2 * B), cut(2 * B);
    const float zeroY = sc.y0 + sc.h() * 0.22f;
    for (std::size_t b = 0; b < B; ++b) {
        const float x = sc.x0 + sc.w() * (static_cast<float>(b) + 0.5f) / B;
        pts[2 * b] = x;
        pts[2 * b + 1] = sc.y1 - clampf((resView_.levelDb[b] + 100.0f) / 100.0f, 0, 1) * sc.h();
        cut[2 * b] = x;
        cut[2 * b + 1] = zeroY - clampf(resView_.cutDb[b], -18.0f, 6.0f) / 18.0f * (sc.y1 - zeroY - 4);
    }
    gfx_.fillUnder(pts.data(), B, sc.y1, Color(kCyan).alpha(0.30f), Color(kCyan).alpha(0.02f));
    gfx_.polyline(pts.data(), B, 1.6f, Color(kCyan).alpha(0.85f));
    gfx_.line(sc.x0, zeroY, sc.x1, zeroY, 1.0f, Color(kOrange).alpha(0.25f));
    gfx_.text(sc.x0 + 4, zeroY - 18, "0 dB", Font::Small, Color(kOrange).alpha(0.7f));
    gfx_.polyline(cut.data(), B, 2.6f, by ? Color(0x6A6F74FF) : kOrange);
    float maxCut = 0;
    for (std::size_t b = 0; b < B; ++b) maxCut = std::min(maxCut, resView_.cutDb[b]);
    gfx_.text(sc.x1 - 150, sc.y0 + 4, fmt("max cut %.1f dB", -maxCut), Font::Small, kOrange);

    auto& se = rs->param(ResonanceProcessor::kSensitivity);
    auto& fr = rs->param(ResonanceProcessor::kFrequency);
    auto& bwp = rs->param(ResonanceProcessor::kBandwidth);
    auto& rd = rs->param(ResonanceProcessor::kReduction);
    auto& cp = rs->param(ResonanceProcessor::kCompensation);
    se.set(knob(410, LY::rsSensitivity, se.target(), 0, 100, Taper::Linear, 50, false, kCyan, fmt("Sensitivity %.0f %%", se.target()), by));
    fr.set(knob(411, LY::rsFrequency, fr.target(), 200, 16000, Taper::Log, 3500, false, kCyan,
                fr.target() >= 1000 ? fmt("Focus %.2f kHz", fr.target() / 1000) : fmt("Focus %.0f Hz", fr.target()), by));
    bwp.set(knob(412, LY::rsBandwidth, bwp.target(), 0.5f, 8, Taper::Linear, 5, false, kCyan, fmt("Bandwidth %.1f octaves", bwp.target()), by));
    rd.set(knob(413, LY::rsReduction, rd.target(), 0, 18, Taper::Linear, 6, false, kOrange, fmt("Max reduction %.1f dB", rd.target()), by));
    cp.set(knob(414, LY::rsCompensation, cp.target(), 0, 100, Taper::Linear, 50, false, kCyan, fmt("Compensation %.0f %%", cp.target()), by));
}

// ------------------------------------------------------------------ TRANSIENT
void App::drawTransient() {
    TransientProcessor* tr = sel().tr;
    if (!tr) return;
    const bool by = tr->bypassed();
    if (button(500, R(LY::trTitle)) || button(501, R(LY::trLed))) tr->setBypassed(!by);
    if (by) { gfx_.rect(R(LY::trTitle), 0x000000A0); gfx_.text(LY::trTitle.x0 + 6, LY::trTitle.y0 + 8, "BYPASSED", Font::Normal, kRed); }
    const Rect led = R(LY::trLed);
    gfx_.circle(led.x1 - 10, led.cy(), 8, 0x000000FF);
    gfx_.circle(led.x1 - 10, led.cy(), 6, by ? Color(0x3A2A1FFF) : (tr->active() ? kOrange : Color(0x8A4E1FFF)));
    if (!by && tr->active()) gfx_.circle(led.x1 - 10, led.cy(), 12, Color(kOrange).alpha(0.25f));

    // History from the audio thread.
    TransientViz v;
    while (tr->viz().pop(v)) { trHist_[trHead_] = v; trHead_ = (trHead_ + 1) % trHist_.size(); }
    auto drawScreen = [&](const Rect& sc, bool attack, Color col, const char* label) {
        const Rect a = sc.inset(6);
        const float mid = a.cy();
        const std::size_t N = trHist_.size();
        const float bw = a.w() / static_cast<float>(N);
        for (std::size_t i = 0; i < N; ++i) {
            const TransientViz& h = trHist_[(trHead_ + i) % N];
            const float pk = clampf((20.0f * std::log10(h.peak + 1e-6f) + 60.0f) / 60.0f, 0, 1);
            const float x = a.x0 + bw * static_cast<float>(i);
            const float amp = pk * (a.h() * 0.45f);
            gfx_.rect({x, mid - amp, x + bw * 0.8f, mid + amp}, col.alpha(0.22f));
            const float g = attack ? h.attackDb : h.sustainDb;
            if (std::fabs(g) > 0.05f) {
                const float gh = clampf(std::fabs(g) / 15.0f, 0, 1) * amp;
                gfx_.rect({x, mid - gh, x + bw * 0.8f, mid + gh}, (g > 0 ? col : Color(0xB0BEC8FF)).alpha(0.9f));
            }
        }
        gfx_.line(a.x0, mid, a.x1, mid, 1.0f, col.alpha(0.35f));
        gfx_.textIn({a.x0, a.y1 - 26, a.x1, a.y1 - 2}, label, Font::Normal, kText);
        const float cur = attack ? tr->param(TransientProcessor::kAttack).target() : tr->param(TransientProcessor::kSustain).target();
        gfx_.text(a.x1 - 70, a.y0 + 2, fmt("%+.1f dB", cur), Font::Small, col);
    };
    drawScreen(R(LY::trAttackScreen), true, kCyan, "ATTACK");
    drawScreen(R(LY::trSustainScreen), false, kOrange, "SUSTAIN");

    // PUNCH / SPEED switch.
    auto& sp = tr->param(TransientProcessor::kSpeed);
    const Rect sw = R(LY::trSwitch);
    if (button(502, sw)) sp.set(sp.target() > 0.5f ? 0.0f : 1.0f);
    const bool speed = sp.target() > 0.5f;
    gfx_.roundRect(sw.inset(2), 5, 0x0B0E11FF);
    gfx_.roundRect({sw.x0 + 6, speed ? sw.cy() + 1 : sw.y0 + 5, sw.x1 - 6, speed ? sw.y1 - 5 : sw.cy() - 1}, 3, kCyan);
    if (sw.contains(in_.mx, in_.my)) { tooltip_ = speed ? "SPEED: fast detector (tight, clicky hits)" : "PUNCH: slower detector (bigger, fuller hits)"; tipX_ = sw.cx(); tipY_ = sw.y0 - 30; }

    auto& at = tr->param(TransientProcessor::kAttack);
    auto& su = tr->param(TransientProcessor::kSustain);
    auto& ga = tr->param(TransientProcessor::kGain);
    auto& mx = tr->param(TransientProcessor::kMix);
    at.set(knob(510, LY::trAttack, at.target(), -15, 15, Taper::Linear, 0, true, kCyan, fmt("Attack %+.1f dB", at.target()), by));
    su.set(knob(511, LY::trSustain, su.target(), -15, 15, Taper::Linear, 0, true, kCyan, fmt("Sustain %+.1f dB", su.target()), by));
    ga.set(knob(512, LY::trGain, ga.target(), -12, 12, Taper::Linear, 0, true, kOrange, fmt("Output gain %+.1f dB", ga.target()), by));
    mx.set(knob(513, LY::trMix, mx.target(), 0, 100, Taper::Linear, 100, false, kCyan, fmt("Mix %.0f %%", mx.target()), by));
}

// ------------------------------------------------------------------ MASTERING
void App::drawMastering() {
    if (!lim_ || !width_) return;
    const bool by = lim_->bypassed();
    if (button(600, R(LY::msTitle))) lim_->setBypassed(!by);
    if (by) { gfx_.rect(R(LY::msTitle), 0x000000A0); gfx_.text(LY::msTitle.x0 + 6, LY::msTitle.y0 + 8, "LIMITER OFF", Font::Normal, kRed); }

    // Target preset selector.
    auto setPreset = [&](int p) {
        preset_ = (p + kNumPresets) % kNumPresets;
        normalize_ = kPresets[preset_].normalize;
        if (normalize_) target_ = kPresets[preset_].lufs;
    };
    if (button(601, R(LY::msPrev))) setPreset(preset_ - 1);
    if (button(602, R(LY::msNext)) || button(603, R(LY::msHeaderBox))) setPreset(preset_ + 1);
    gfx_.textIn(R(LY::msHeaderBox), kPresets[preset_].name, Font::Normal, kText);

    const auto& L = master_.loudness;
    // ---- meters ----
    const Rect mt = R(LY::msMeters);
    const float top = mt.y0 + 12, bot = mt.y1 - 10;
    auto yOf = [&](float db) { return bot - clampf((db + 48.0f) / 48.0f, 0, 1) * (bot - top); };
    const float xs[4] = {828, 851, 933, 956};   // LUFS M, LUFS S, TP L, TP R
    for (int t = 0; t <= 8; ++t) {
        const float db = -6.0f * t, y = yOf(db);
        const std::string s = t == 0 ? "0" : fmt("%.0f", db);
        // One scale between the LUFS and TRUE PEAK pairs, plus the outer edges.
        gfx_.text(812 - gfx_.textWidth(s, Font::Small), y - 9, s, Font::Small, kDim);
        gfx_.text(892 - 0.5f * gfx_.textWidth(s, Font::Small), y - 9, s, Font::Small, kDim);
        gfx_.text(972, y - 9, s, Font::Small, kDim);
        gfx_.line(862, y, 868, y, 1.0f, Color(kDim).alpha(0.6f));
        gfx_.line(916, y, 922, y, 1.0f, Color(kDim).alpha(0.6f));
    }
    const double nowT = now();
    float vals[4] = {L.momentary, L.shortTerm, master_.tpDb[0], master_.tpDb[1]};
    for (int c = 2; c < 4; ++c) {
        const float v = std::isfinite(vals[c]) ? vals[c] : -90.0f;
        if (v >= tpHold_[c - 2] || nowT - tpHoldT_[c - 2] > 1.5) { tpHold_[c - 2] = v; tpHoldT_[c - 2] = nowT; }
    }
    const int segs = 40;
    for (int c = 0; c < 4; ++c) {
        const float v = std::isfinite(vals[c]) ? vals[c] : -90.0f;
        for (int i = 0; i < segs; ++i) {
            const float db = -48.0f + 48.0f * (i + 0.5f) / segs;
            const float y1 = yOf(-48.0f + 48.0f * i / segs), y0 = yOf(-48.0f + 48.0f * (i + 1) / segs);
            Color col;
            if (c < 2) col = db > static_cast<float>(target_) + 1.0f ? kOrange : (db > static_cast<float>(target_) - 3.0f ? kYellow : kGreen);
            else col = db > -1.0f ? kRed : (db > -6.0f ? kYellow : kGreen);
            const bool lit = v >= db;
            gfx_.rect({xs[c] - 8, y0 + 1, xs[c] + 8, y1 - 1}, lit ? col : col.alpha(0.08f));
        }
        if (c >= 2 && tpHold_[c - 2] > -48.0f) gfx_.rect({xs[c] - 8, yOf(tpHold_[c - 2]) - 1.5f, xs[c] + 8, yOf(tpHold_[c - 2]) + 1.5f}, kWhite);
    }
    gfx_.line(xs[0] - 12, yOf(static_cast<float>(target_)), xs[1] + 12, yOf(static_cast<float>(target_)), 1.5f, Color(kCyan).alpha(0.8f));
    gfx_.textIn({xs[0] - 12, bot + 1, xs[0] + 12, bot + 14}, "M", Font::Small, kDim);
    gfx_.textIn({xs[1] - 12, bot + 1, xs[1] + 12, bot + 14}, "S", Font::Small, kDim);
    gfx_.textIn({xs[2] - 12, bot + 1, xs[2] + 12, bot + 14}, "L", Font::Small, kDim);
    gfx_.textIn({xs[3] - 12, bot + 1, xs[3] + 12, bot + 14}, "R", Font::Small, kDim);

    // ---- readouts ----
    const bool onTarget = std::isfinite(L.integrated) && std::fabs(L.integrated - target_) <= 1.0;
    gfx_.textIn(R(LY::msIntegrated), lufsStr(L.integrated), Font::Large, onTarget ? kGreen : kOrange);
    gfx_.textIn(R(LY::msShortTerm), lufsStr(L.shortTerm), Font::Large, kOrange);
    gfx_.textIn(R(LY::msPsr), std::isfinite(L.shortTerm) && L.shortTerm > -70 ? fmt("%.1f", L.psr) : "--", Font::Large, kCyan);
    gfx_.textIn(R(LY::msPlr), std::isfinite(L.integrated) && L.integrated > -70 ? fmt("%.1f", L.plr) : "--", Font::Large, kCyan);
    gfx_.textIn(R(LY::msPlrLabel), "PLR", Font::Normal, kText);
    gfx_.textIn(R(LY::msFooterLabel), fmt("LRA %.1f", L.lra), Font::Small, kDim);

    // ---- MODE (glue) and TARGET boxes ----
    static const char* glue[] = {"CLEAN", "PUNCH", "LOUD"};
    if (button(610, R(LY::msModeBox))) { glueMode_ = static_cast<Glue>((static_cast<int>(glueMode_) + 1) % 3); applyGlue(); }
    gfx_.textIn(R(LY::msModeBox), glue[static_cast<int>(glueMode_)], Font::Small, kCyan);
    if (R(LY::msModeBox).contains(in_.mx, in_.my)) { tooltip_ = "Bus glue before the limiter: CLEAN off · PUNCH 2:1 · LOUD 3:1 (click)"; tipX_ = LY::msModeBox.x0 + 40; tipY_ = LY::msModeBox.y0 - 30; }
    const Rect tbx = R(LY::msTargetBox);
    const bool tHot = tbx.contains(in_.mx, in_.my);
    if (tHot) {
        hotId_ = 611;
        tooltip_ = normalize_ ? "Export loudness target · drag / wheel to set" : "Export without normalizing (pick a preset above to enable)";
        tipX_ = tbx.cx(); tipY_ = tbx.y0 - 30;
        if (in_.pressed && normalize_) { activeId_ = 611; dragStartY_ = in_.devY; dragStartVal_ = static_cast<float>(target_); }
        if (in_.wheel != 0 && normalize_) target_ = std::clamp(target_ + in_.wheel * 0.5, -30.0, -6.0);
    }
    if (activeId_ == 611) {
        if (in_.down) target_ = std::clamp(std::round((dragStartVal_ + (dragStartY_ - in_.devY) * 0.05f) * 2.0) / 2.0, -30.0, -6.0);
        if (in_.released) activeId_ = 0;
    }
    gfx_.textIn(tbx, normalize_ ? fmt("%.1f", target_) : "OFF", Font::Small, kCyan);
    gfx_.textIn(R(LY::msTargetLabel), "TARGET", Font::Normal, kText);

    // ---- knobs ----
    auto& ce = lim_->param(LimiterProcessor::kCeiling);
    auto& dr = lim_->param(LimiterProcessor::kDrive);
    auto& re = lim_->param(LimiterProcessor::kRelease);
    auto& wd = width_->param(0);
    ce.set(knob(620, LY::msCeiling, ce.target(), -6, 0, Taper::Linear, -1, false, kCyan, fmt("Ceiling %.1f dBTP", ce.target()), by));
    const float thr = knob(621, LY::msThreshold, -dr.target(), -24, 0, Taper::Linear, 0, false, kCyan,
                           fmt("Threshold %.1f dB (limiter drive)", -dr.target()), by);
    dr.set(-thr);
    re.set(knob(622, LY::msRelease, re.target(), 5, 1000, Taper::Log, 80, false, kCyan, fmt("Release %.0f ms", re.target()), by));
    wd.set(knob(623, LY::msWidth, wd.target(), 0, 200, Taper::Linear, 100, true, kCyan, fmt("Width %.0f %%", wd.target()), false));
    const float gr = lim_->limiter().gainReductionDb();
    lim_->limiter().clearGrMeter();
    if (gr < -0.1f) {   // limiter gain reduction, shown on the threshold knob's top edge
        const std::string g = fmt("GR %.1f dB", gr);
        const float w = gfx_.textWidth(g, Font::Small) + 10;
        const Rect gb{LY::msThreshold.cx - w / 2, LY::msThreshold.cy - LY::msThreshold.r - 20, LY::msThreshold.cx + w / 2, LY::msThreshold.cy - LY::msThreshold.r - 2};
        gfx_.roundRect(gb, 4, 0x000000C8);
        gfx_.textIn(gb, g, Font::Small, kOrange);
    }
}

// ------------------------------------------------------------------ overlays
void App::drawOverlays() {
    if (exporting_) {
        const Rect b{460, 1090, 1220, 1210};
        gfx_.roundRect(b, 12, 0x000000E0);
        gfx_.rectOutline(b, Color(kCyan).alpha(0.5f), 2);
        const double p = exportProgress_.load();
        gfx_.text(b.x0 + 24, b.y0 + 14, fmt("EXPORTING MASTER  %.0f %%", p * 100) + (normalize_ ? fmt("   (normalizing to %.1f LUFS)", target_) : std::string()), Font::Normal, kText);
        const Rect bar{b.x0 + 24, b.y0 + 52, b.x1 - 24, b.y0 + 72};
        gfx_.rect(bar, 0x15202AFF);
        gfx_.rect({bar.x0, bar.y0, bar.x0 + bar.w() * static_cast<float>(p), bar.y1}, kCyan);
        gfx_.text(b.x0 + 24, b.y0 + 82, "click the red record button to cancel", Font::Small, kDim);
    }
    if (!toast_.empty() && now() < toastUntil_) {
        const float w = gfx_.textWidth(toast_, Font::Normal) + 40;
        const Rect t{719 - w / 2, 1190, 719 + w / 2, 1232};
        gfx_.roundRect(t, 8, 0x000000D8);
        gfx_.rectOutline(t, Color(kCyan).alpha(0.35f), 1.5f);
        gfx_.textIn(t, toast_, Font::Normal, kText);
    }
    if (!tooltip_.empty()) {
        const float w = gfx_.textWidth(tooltip_, Font::Small) + 20;
        float x0 = tipX_ - w / 2;
        x0 = clampf(x0, 10, 1428 - w);
        const Rect t{x0, tipY_ - 26, x0 + w, tipY_};
        gfx_.roundRect(t, 5, 0x05080BF0);
        gfx_.rectOutline(t, Color(kCyan).alpha(0.5f), 1.2f);
        gfx_.textIn(t, tooltip_, Font::Small, kText);
    }
}

// ====================================================================================== window procedure
LRESULT App::wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE:
        gfx_.resize(LOWORD(l), HIWORD(l));
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(l);
        mm->ptMinTrackSize.x = 420; mm->ptMinTrackSize.y = 520;
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(l);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_LBUTTONDOWN: SetCapture(h); in_.pressed = true; return 0;
    case WM_LBUTTONUP: ReleaseCapture(); in_.released = true; return 0;
    case WM_LBUTTONDBLCLK: in_.dbl = true; in_.pressed = true; return 0;
    case WM_RBUTTONDOWN: in_.rpressed = true; return 0;
    case WM_MOUSEWHEEL: in_.wheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(w)) / WHEEL_DELTA; return 0;
    case WM_KEYDOWN: {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (w == VK_SPACE) { if (eng_->isPlaying()) eng_->stop(); else if (loadedCount()) eng_->play(); }
        else if (w == VK_HOME) eng_->seek(0);
        else if (ctrl && w == 'L') loadFiles(openFilesDialog(hwnd_, L"Load Stems (up to 13 WAV files)", {L"WAV audio (*.wav)", L"*.wav"}, true));
        else if (ctrl && w == 'O') openSession();
        else if (ctrl && w == 'S') saveSessionAs();
        else if (ctrl && w == 'E') exportMaster();
        return 0;
    }
    case WM_DROPFILES: {
        HDROP d = reinterpret_cast<HDROP>(w);
        const UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> files;
        for (UINT i = 0; i < n; ++i) {
            wchar_t buf[MAX_PATH * 4];
            if (DragQueryFileW(d, i, buf, static_cast<UINT>(std::size(buf)))) {
                std::wstring f = buf;
                std::wstring ext = f.size() > 4 ? f.substr(f.size() - 4) : L"";
                for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
                if (ext == L".wav") files.push_back(f);
                else if (ext == L".hcs") { toast("Use the menu → Open Session for .hcs files"); }
            }
        }
        DragFinish(d);
        std::sort(files.begin(), files.end());
        loadFiles(files);
        return 0;
    }
    case WM_EXPORT_DONE: onExportDone(); return 0;
    case WM_CLOSE:
        if (exporting_ && MessageBoxW(h, L"An export is still running. Quit anyway?", L"Hybrid Core", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    simd::ScopedFlushDenormals ftz;
    static App app;
    g_app = &app;
    if (!app.init(inst, show)) return 1;
    app.openFromCommandLine();
    const int rc = app.run();
    CoUninitialize();
    return rc;
}
