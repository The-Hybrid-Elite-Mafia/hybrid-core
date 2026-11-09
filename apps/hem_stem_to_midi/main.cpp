// =============================================================================
//  apps/hem_stem_to_midi/main.cpp — HEM STEM-TO-MIDI for Windows (Win32, C++23, no JUCE/Qt).
//
//  Load 1 song or up to 13 stems (WAV, MP3, M4A, FLAC...), pick each stem's part type, set
//  tempo / quantize / sensitivity / shortest note, press CONVERT. Writes, into the output folder:
//      <song> - ALL STEMS.mid      one track per stem
//      <song> - <stem>.mid         one file per stem
//  then reads every file back and checks it. The work runs on a background thread.
//
//  Every control here feeds hc::midi::Settings / StemIn::mode (see readSettings()); nothing is
//  for show. "--selftest OUT FILES..." drives the real controls with window messages, converts
//  twice with different settings, checks the files, saves PNG pictures, writes result.txt.
// =============================================================================
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#ifndef UNICODE
  #define UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <uxtheme.h>
#include "audio_load.hpp"
#include "hc/midi/transcriber.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace hc::midi;
namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------- look (HEM: lime on dark)
constexpr COLORREF kBg = RGB(0x12, 0x14, 0x18), kPanel = RGB(0x1B, 0x1E, 0x24), kRowAlt = RGB(0x17, 0x1A, 0x1F);
constexpr COLORREF kText = RGB(0xE6, 0xE8, 0xEC), kDim = RGB(0x8A, 0x90, 0x9A), kLime = RGB(0xC0, 0xFF, 0x3E);
constexpr COLORREF kRed = RGB(0xFF, 0x5A, 0x5A), kField = RGB(0x0D, 0x0F, 0x12);
constexpr wchar_t kTitle[] = L"HEM STEM-TO-MIDI";

enum : int {
    ID_LOAD_SONG = 101, ID_LOAD_STEMS, ID_CLEAR,
    ID_ROW_NAME = 200, ID_ROW_TYPE = 300, ID_ROW_RESULT = 400, ID_ROW_REMOVE = 500,
    ID_BPM = 601, ID_QUANT, ID_SENS, ID_SENS_VAL, ID_SHORT, ID_SHORT_VAL,
    ID_OUT_PATH = 701, ID_OUT_CHANGE, ID_CONVERT = 801, ID_PROGRESS, ID_STATUS, ID_OPEN_FOLDER,
};
constexpr UINT WM_APP_PROGRESS = WM_APP + 1, WM_APP_DONE = WM_APP + 2, WM_APP_SELFTEST = WM_APP + 3;
constexpr int kQuantDivs[5] = {0, 4, 8, 16, 32};
constexpr const wchar_t* kQuantNames[5] = {L"Off", L"1/4", L"1/8", L"1/16", L"1/32"};

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(std::size_t(n), L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n); return w;
}
std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(std::size_t(n), '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr); return s;
}
std::wstring getText(HWND h) { std::wstring s(std::size_t(GetWindowTextLengthW(h)) + 1, L'\0'); GetWindowTextW(h, s.data(), int(s.size())); s.resize(wcslen(s.c_str())); return s; }

struct Loaded { fs::path path; std::string name; double seconds = 0; double rate = 0; };

struct App {
    HWND wnd = nullptr;
    HFONT font = nullptr, fontBold = nullptr, fontTitle = nullptr, fontSmall = nullptr;
    HBRUSH bgBrush = nullptr, panelBrush = nullptr, fieldBrush = nullptr, altBrush = nullptr;
    double scale = 1.0;
    std::array<HWND, kMaxStems> rowName{}, rowType{}, rowResult{}, rowRemove{};
    HWND btnSong = nullptr, btnStems = nullptr, btnClear = nullptr, bpm = nullptr, quant = nullptr, sens = nullptr, sensVal = nullptr;
    HWND shortest = nullptr, shortVal = nullptr, outPath = nullptr, btnOut = nullptr, btnConvert = nullptr, progress = nullptr, status = nullptr, btnOpen = nullptr;
    std::vector<Loaded> files;
    std::vector<StemIn> stems;          // audio (mono) kept here, used in place by the worker
    fs::path outDir; bool outDirChosen = false;
    std::string songName;
    std::thread worker; std::atomic<bool> busy{false};
    ConvertResult last; Settings lastSettings;
    std::atomic<int> progDone{0}, progTotal{1};
    std::string progWhat;
    // self-test
    bool selftest = false; fs::path selfOut; std::vector<fs::path> selfFiles; int selfStep = 0; std::vector<std::string> selfLog; int selfFail = 0;
    ConvertResult selfR1; std::size_t selfTracks1 = 0;
} g;

int S(int v) { return int(v * g.scale + 0.5); }

// ---------------------------------------------------------------- picture of the window (PNG via WIC)
std::string savePng(HWND h, const fs::path& file) {
    RECT rc; GetClientRect(h, &rc);
    const int w = rc.right, ht = rc.bottom;
    HDC wdc = GetDC(h), mdc = CreateCompatibleDC(wdc);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -ht; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr; HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    if (!PrintWindow(h, mdc, PW_CLIENTONLY | 0x2 /*PW_RENDERFULLCONTENT*/)) BitBlt(mdc, 0, 0, w, ht, wdc, 0, 0, SRCCOPY);
    GdiFlush();
    auto* px = static_cast<std::uint8_t*>(bits);
    for (int i = 0; i < w * ht; ++i) px[i * 4 + 3] = 255;
    std::string err;
    IWICImagingFactory* fac = nullptr; IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac)))) err = "no image encoder";
    else if (FAILED(fac->CreateStream(&st)) || FAILED(st->InitializeFromFilename(file.wstring().c_str(), GENERIC_WRITE))) err = "cannot create picture file";
    else if (FAILED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(st, WICBitmapEncoderNoCache))) err = "PNG encoder failed";
    else if (FAILED(enc->CreateNewFrame(&fr, nullptr)) || FAILED(fr->Initialize(nullptr))) err = "PNG frame failed";
    else {
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(fr->SetSize(UINT(w), UINT(ht))) || FAILED(fr->SetPixelFormat(&pf)) || FAILED(fr->WritePixels(UINT(ht), UINT(w * 4), UINT(w * ht * 4), px)) || FAILED(fr->Commit()) || FAILED(enc->Commit())) err = "PNG write failed";
    }
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    if (fac) fac->Release();
    SelectObject(mdc, old); DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(h, wdc);
    return err;
}

// ---------------------------------------------------------------- settings from the controls
// The ONE place the window's controls are turned into engine settings.
bool readSettings(Settings& st, std::wstring& problem) {
    const std::wstring b = getText(g.bpm);
    st.bpm = 0;
    if (!b.empty() && b != L"auto" && b != L"Auto") {
        try { st.bpm = std::stod(utf8(b)); } catch (...) { st.bpm = -1; }
        if (!(st.bpm >= 40 && st.bpm <= 300)) { problem = L"BPM must be a number from 40 to 300, or empty for auto."; return false; }
    }
    const int qi = int(SendMessageW(g.quant, CB_GETCURSEL, 0, 0));
    st.quantizeDiv = kQuantDivs[std::clamp(qi, 0, 4)];
    st.sensitivity = int(SendMessageW(g.sens, TBM_GETPOS, 0, 0));
    st.shortestMs = int(SendMessageW(g.shortest, TBM_GETPOS, 0, 0));
    for (std::size_t i = 0; i < g.stems.size(); ++i) g.stems[i].mode = Mode(std::clamp(int(SendMessageW(g.rowType[i], CB_GETCURSEL, 0, 0)), 0, kModeCount - 1));
    return true;
}

void setStatus(const std::wstring& s, bool bad = false) { SetWindowTextW(g.status, s.c_str()); SetPropW(g.status, L"bad", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(bad ? 1 : 0))); InvalidateRect(g.status, nullptr, TRUE); }

void refreshRows() {
    for (std::size_t i = 0; i < kMaxStems; ++i) {
        const bool used = i < g.files.size();
        if (used) {
            wchar_t buf[512];
            swprintf(buf, 512, L"%ls   (%.0f s, %.1f kHz)", wide(g.files[i].name).c_str(), g.files[i].seconds, g.files[i].rate / 1000.0);
            SetWindowTextW(g.rowName[i], buf);
        } else { SetWindowTextW(g.rowName[i], L"—"); SetWindowTextW(g.rowResult[i], L""); SendMessageW(g.rowType[i], CB_SETCURSEL, 0, 0); }
        EnableWindow(g.rowType[i], used && !g.busy);
        ShowWindow(g.rowRemove[i], used ? SW_SHOW : SW_HIDE);
    }
    const bool any = !g.files.empty();
    EnableWindow(g.btnConvert, any && !g.busy);
    EnableWindow(g.btnClear, any && !g.busy);
    EnableWindow(g.btnSong, !g.busy); EnableWindow(g.btnStems, !g.busy);
    std::wstring p = g.outDir.empty() ? L"(the folder of the first file) \\ HEM MIDI" : g.outDir.wstring();
    SetWindowTextW(g.outPath, p.c_str());
    InvalidateRect(g.wnd, nullptr, FALSE);
}

// Loads files into the list (append). Returns how many were added.
int addFiles(const std::vector<fs::path>& in, bool singleSong) {
    std::vector<fs::path> list;
    for (const auto& p : in) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            std::vector<fs::path> inside;
            for (auto& e : fs::directory_iterator(p, ec)) if (e.is_regular_file() && hem::isAudioFile(e.path())) inside.push_back(e.path());
            std::sort(inside.begin(), inside.end());
            list.insert(list.end(), inside.begin(), inside.end());
        } else if (hem::isAudioFile(p)) list.push_back(p);
    }
    if (list.empty()) { setStatus(L"No audio files there (WAV, MP3, M4A, FLAC, AAC, WMA).", true); return 0; }
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    int added = 0; std::wstring errors;
    for (const auto& p : list) {
        if (g.files.size() >= kMaxStems) { errors += L"Only 13 stems fit; the rest were left out. "; break; }
        wchar_t msg[600]; swprintf(msg, 600, L"Opening %ls ...", p.filename().wstring().c_str()); setStatus(msg); UpdateWindow(g.status);
        StemIn s; s.name = utf8(p.stem().wstring());
        if (auto e = hem::loadAudio(p, s.audio); !e.empty()) { errors += wide(e) + L". "; continue; }
        const double secs = double(s.audio.frames()) / s.audio.sampleRate, rate = s.audio.sampleRate;
        toMonoInPlace(s.audio);   // the engine listens in mono; halves memory for 13 long stems
        const std::size_t row = g.files.size();
        g.files.push_back({p, s.name, secs, rate});
        g.stems.push_back(std::move(s));
        // part type: "Full song" for a single song, else Auto (from the name)
        SendMessageW(g.rowType[row], CB_SETCURSEL, singleSong ? int(Mode::FullMix) : int(Mode::Auto), 0);
        SetWindowTextW(g.rowResult[row], L"");
        ++added;
    }
    SetCursor(old);
    if (!g.files.empty() && !g.outDirChosen) g.outDir = g.files[0].path.parent_path() / L"HEM MIDI";
    g.songName = g.files.size() == 1 ? g.files[0].name : utf8(g.files[0].path.parent_path().filename().wstring());
    if (g.songName.empty()) g.songName = "HEM";
    refreshRows();
    wchar_t msg[700];
    swprintf(msg, 700, L"%d file(s) loaded, %d of 13 rows used. %ls", added, int(g.files.size()), errors.c_str());
    setStatus(msg, !errors.empty());
    return added;
}

void clearAll() {
    g.files.clear(); g.stems.clear();
    if (!g.outDirChosen) g.outDir.clear();
    refreshRows(); setStatus(L"Cleared. Load 1 song or up to 13 stems.");
}

void removeRow(std::size_t i) {
    if (i >= g.files.size()) return;
    std::vector<int> types;
    for (std::size_t k = 0; k < g.files.size(); ++k) types.push_back(int(SendMessageW(g.rowType[k], CB_GETCURSEL, 0, 0)));
    g.files.erase(g.files.begin() + std::ptrdiff_t(i)); g.stems.erase(g.stems.begin() + std::ptrdiff_t(i)); types.erase(types.begin() + std::ptrdiff_t(i));
    for (std::size_t k = 0; k < kMaxStems; ++k) {
        SendMessageW(g.rowType[k], CB_SETCURSEL, k < types.size() ? types[k] : 0, 0);
        wchar_t t[256]; GetWindowTextW(g.rowResult[std::min(k + 1, kMaxStems - 1)], t, 256);
        if (k >= i) SetWindowTextW(g.rowResult[k], k + 1 < kMaxStems && k < types.size() ? t : L"");
    }
    refreshRows();
}

std::vector<fs::path> pickFiles(bool multi) {
    std::vector<wchar_t> buf(1 << 16, L'\0');
    OPENFILENAMEW of{}; of.lStructSize = sizeof(of); of.hwndOwner = g.wnd;
    of.lpstrFilter = L"Audio (WAV, MP3, M4A, FLAC, AAC, WMA)\0*.wav;*.mp3;*.m4a;*.flac;*.aac;*.wma\0All files\0*.*\0";
    of.lpstrFile = buf.data(); of.nMaxFile = DWORD(buf.size());
    of.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | (multi ? OFN_ALLOWMULTISELECT : 0);
    of.lpstrTitle = multi ? L"Pick up to 13 stems (Ctrl+A in a folder picks them all)" : L"Pick one song";
    std::vector<fs::path> out;
    if (!GetOpenFileNameW(&of)) return out;
    const wchar_t* p = buf.data();
    std::wstring first = p; p += first.size() + 1;
    if (!*p) { out.push_back(first); return out; }
    for (; *p; p += wcslen(p) + 1) out.push_back(fs::path(first) / p);
    std::sort(out.begin(), out.end());
    return out;
}

fs::path pickFolder() {
    fs::path res; IFileOpenDialog* d = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
        DWORD o = 0; d->GetOptions(&o); d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        d->SetTitle(L"Where should the MIDI files go?");
        if (SUCCEEDED(d->Show(g.wnd))) { IShellItem* it = nullptr; if (SUCCEEDED(d->GetResult(&it))) { PWSTR s = nullptr; if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &s))) { res = s; CoTaskMemFree(s); } it->Release(); } }
        d->Release();
    }
    return res;
}

// ---------------------------------------------------------------- convert (background thread)
void startConvert() {
    if (g.busy || g.stems.empty()) return;
    Settings st; std::wstring problem;
    if (!readSettings(st, problem)) { setStatus(problem, true); return; }
    if (g.worker.joinable()) g.worker.join();
    g.busy = true; g.lastSettings = st;
    refreshRows();
    for (std::size_t i = 0; i < g.files.size(); ++i) SetWindowTextW(g.rowResult[i], L"working...");
    SendMessageW(g.progress, PBM_SETPOS, 0, 0);
    setStatus(L"Converting...");
    g.worker = std::thread([st] {
        auto R = convert(g.stems, st, g.outDir, g.songName, [](int d, int t, const std::string&) {
            g.progDone = d; g.progTotal = std::max(1, t); PostMessageW(g.wnd, WM_APP_PROGRESS, 0, 0);
        });
        g.last = std::move(R);
        PostMessageW(g.wnd, WM_APP_DONE, 0, 0);
    });
}

void finishConvert() {
    if (g.worker.joinable()) g.worker.join();
    g.busy = false;
    const auto& R = g.last;
    for (std::size_t i = 0; i < R.stems.size() && i < kMaxStems; ++i) {
        const auto& s = R.stems[i];
        wchar_t t[300];
        if (s.mode == Mode::Skip) swprintf(t, 300, L"skipped");
        else if (!s.error.empty()) swprintf(t, 300, L"ERROR: %ls", wide(s.error).c_str());
        else swprintf(t, 300, L"%ls: %zu notes", wide(std::string(kModeNames[std::size_t(s.mode)])).c_str(), s.notes);
        SetWindowTextW(g.rowResult[i], t);
    }
    SendMessageW(g.progress, PBM_SETPOS, 1000, 0);
    refreshRows();
    wchar_t msg[700];
    if (!R.error.empty()) swprintf(msg, 700, L"Failed: %ls", wide(R.error).c_str());
    else {
        int fails = 0; for (auto& c : R.checks) fails += c.rfind("FAIL", 0) == 0;
        swprintf(msg, 700, L"Done. Tempo %.1f BPM (%ls). %zu file(s) written and read back: %ls.  Saved in: %ls",
                 R.bpm, R.bpmTyped ? L"typed" : (R.bpmFound ? L"found" : L"not found, 120 used"), R.stems.size() + 1,
                 fails ? L"SOME CHECKS FAILED" : L"all checks passed", g.outDir.wstring().c_str());
    }
    setStatus(msg, !R.error.empty() || !R.allChecksPassed);
    EnableWindow(g.btnOpen, R.error.empty());
    if (g.selftest) PostMessageW(g.wnd, WM_APP_SELFTEST, 0, 0);
}

// ---------------------------------------------------------------- self-test (drives the real controls)
void selfLog(bool ok, const std::string& what) { g.selfLog.push_back(std::string(ok ? "PASS " : "FAIL ") + what); if (!ok) ++g.selfFail; }
void click(HWND btn) { SendMessageW(g.wnd, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(btn), BN_CLICKED), LPARAM(btn)); }
void pickCombo(HWND c, int i) { SendMessageW(c, CB_SETCURSEL, i, 0); SendMessageW(g.wnd, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(c), CBN_SELCHANGE), LPARAM(c)); }
void moveSlider(HWND s, int v) { SendMessageW(s, TBM_SETPOS, TRUE, v); SendMessageW(g.wnd, WM_HSCROLL, MAKEWPARAM(TB_THUMBPOSITION, v), LPARAM(s)); }
void shot(const wchar_t* name) { const auto e = savePng(g.wnd, g.selfOut / name); selfLog(e.empty(), "picture " + utf8(name) + (e.empty() ? " saved" : ": " + e)); }

void selfTestStep() {
    switch (g.selfStep++) {
    case 0: {   // load through the same code as the buttons / drag-and-drop, set controls by window messages
        g.outDir = g.selfOut / L"midi"; g.outDirChosen = true;
        const int n = addFiles(g.selfFiles, false);
        selfLog(n == int(g.selfFiles.size()), "loaded " + std::to_string(n) + " of " + std::to_string(g.selfFiles.size()) + " files");
        SetWindowTextW(g.bpm, L"");
        pickCombo(g.quant, 3);          // 1/16
        moveSlider(g.sens, 50); moveSlider(g.shortest, 60);
        RedrawWindow(g.wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        shot(L"1_loaded.png");
        click(g.btnConvert);
        selfLog(g.busy.load(), "CONVERT button started the job");
        break;
    }
    case 1: {   // first result: auto tempo, 1/16 quantize
        g.selfR1 = g.last;
        const auto& R = g.last;
        selfLog(R.error.empty(), "first conversion finished" + (R.error.empty() ? std::string() : ": " + R.error));
        selfLog(R.allChecksPassed, "first conversion: every file read back correctly (" + std::to_string(R.checks.size()) + " checks)");
        selfLog(g.lastSettings.quantizeDiv == 16 && g.lastSettings.bpm == 0 && g.lastSettings.sensitivity == 50 && g.lastSettings.shortestMs == 60, "controls -> settings: quantize 1/16, BPM auto, sensitivity 50, shortest 60 ms");
        const auto P = parseFile(R.combinedFile);
        g.selfTracks1 = P.tracks.size();   // the 2nd run writes the same file names, so count now
        bool grid = P.tracks.size() > 1; const std::int64_t off = secondsToTicks(R.beatOffsetSec, R.bpm);
        for (std::size_t t = 1; t < P.tracks.size(); ++t) for (auto& n : P.tracks[t].notes) grid = grid && ((n.on - off) % 120 + 120) % 120 == 0;
        selfLog(grid, "Quantize 1/16 in the window -> every note in the file on the 1/16 grid");
        selfLog(std::fabs(P.bpm - R.bpm) < 0.01, "file tempo = found tempo (" + std::to_string(R.bpm) + ")");
        wchar_t t0[256]; GetWindowTextW(g.rowResult[0], t0, 256);
        selfLog(wcsstr(t0, L"notes") != nullptr, "row 1 shows its result: " + utf8(t0));
        RedrawWindow(g.wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        shot(L"2_converted.png");
        // second run: typed BPM 100, quantize off, sensitivity 100, shortest 20 ms, row 1 = Skip
        SetWindowTextW(g.bpm, L"100");
        pickCombo(g.quant, 0);
        moveSlider(g.sens, 100); moveSlider(g.shortest, 20);
        pickCombo(g.rowType[0], int(Mode::Skip));
        click(g.btnConvert);
        break;
    }
    case 2: {
        const auto& R = g.last; const auto& R1 = g.selfR1;
        selfLog(R.error.empty() && R.allChecksPassed, "second conversion finished, every file read back correctly");
        selfLog(g.lastSettings.quantizeDiv == 0 && g.lastSettings.bpm == 100 && g.lastSettings.sensitivity == 100 && g.lastSettings.shortestMs == 20, "controls -> settings: BPM 100, quantize off, sensitivity 100, shortest 20 ms");
        const auto P = parseFile(R.combinedFile);
        selfLog(std::fabs(P.bpm - 100.0) < 0.01, "typed BPM 100 -> tempo in file " + std::to_string(P.bpm));
        selfLog(!R.stems.empty() && R.stems[0].mode == Mode::Skip && P.tracks.size() + 1 == g.selfTracks1, "row 1 type = Skip -> one track fewer (" + std::to_string(g.selfTracks1 - 1) + " -> " + std::to_string(P.tracks.size() - 1) + ")");
        std::size_t n1 = 0, n2 = 0;
        for (std::size_t i = 1; i < R.stems.size() && i < R1.stems.size(); ++i) { n1 += R1.stems[i].notes; n2 += R.stems[i].notes; }
        selfLog(n2 > n1, "sensitivity 50 -> 100 and shortest 60 -> 20 ms: more notes on the other rows (" + std::to_string(n1) + " -> " + std::to_string(n2) + ")");
        bool allGrid = true; const std::int64_t off = secondsToTicks(R.beatOffsetSec, 100.0);
        for (std::size_t t = 1; t < P.tracks.size(); ++t) for (auto& n : P.tracks[t].notes) allGrid = allGrid && ((n.on - off) % 120 + 120) % 120 == 0;
        selfLog(!allGrid, "Quantize Off -> notes keep their own timing");
        RedrawWindow(g.wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        shot(L"3_second_run.png");
        std::ofstream f(g.selfOut / L"result.txt");
        for (auto& l : g.selfLog) f << l << "\n";
        f << (g.selfFail ? "SOME FAILED" : "ALL PASSED") << " (" << g.selfFail << " failure(s))\n";
        PostQuitMessage(g.selfFail);
        break;
    }
    }
}

// ---------------------------------------------------------------- window
HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT f = nullptr) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g.wnd, HMENU(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, WPARAM(f ? f : g.font), TRUE);
    return c;
}

void createControls() {
    const int L = 24;
    g.btnSong = mk(L"BUTTON", L"LOAD 1 SONG", BS_OWNERDRAW, L, 78, 170, 34, ID_LOAD_SONG, g.fontBold);
    g.btnStems = mk(L"BUTTON", L"LOAD STEMS (UP TO 13)", BS_OWNERDRAW, L + 180, 78, 230, 34, ID_LOAD_STEMS, g.fontBold);
    g.btnClear = mk(L"BUTTON", L"CLEAR", BS_OWNERDRAW, L + 420, 78, 90, 34, ID_CLEAR, g.fontBold);
    for (std::size_t i = 0; i < kMaxStems; ++i) {
        const int y = 146 + int(i) * 28;
        g.rowName[i] = mk(L"STATIC", L"—", SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX, L + 34, y, 430, 24, ID_ROW_NAME + int(i));
        g.rowType[i] = mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, L + 470, y, 130, 300, ID_ROW_TYPE + int(i));
        for (int k = 0; k < kModeCount; ++k) SendMessageW(g.rowType[i], CB_ADDSTRING, 0, LPARAM(wide(std::string(kModeNames[std::size_t(k)])).c_str()));
        SendMessageW(g.rowType[i], CB_SETCURSEL, 0, 0);
        g.rowResult[i] = mk(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX, L + 612, y, 300, 24, ID_ROW_RESULT + int(i));
        g.rowRemove[i] = mk(L"BUTTON", L"×", BS_OWNERDRAW, L + 920, y + 1, 24, 22, ID_ROW_REMOVE + int(i), g.fontBold);
    }
    const int sy = 520;
    mk(L"STATIC", L"TEMPO (BPM)", SS_LEFT, L, sy, 120, 20, -1, g.fontSmall);
    g.bpm = mk(L"EDIT", L"", ES_LEFT | ES_AUTOHSCROLL | WS_TABSTOP, L, sy + 22, 100, 26, ID_BPM);
    SendMessageW(g.bpm, EM_SETCUEBANNER, TRUE, LPARAM(L"auto"));
    mk(L"STATIC", L"QUANTIZE", SS_LEFT, L + 130, sy, 100, 20, -1, g.fontSmall);
    g.quant = mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, L + 130, sy + 22, 100, 200, ID_QUANT);
    for (auto n : kQuantNames) SendMessageW(g.quant, CB_ADDSTRING, 0, LPARAM(n));
    SendMessageW(g.quant, CB_SETCURSEL, 0, 0);
    mk(L"STATIC", L"SENSITIVITY", SS_LEFT, L + 260, sy, 140, 20, -1, g.fontSmall);
    g.sens = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, L + 255, sy + 22, 220, 28, ID_SENS);
    SendMessageW(g.sens, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100)); SendMessageW(g.sens, TBM_SETPOS, TRUE, kSensitivityDefault);
    g.sensVal = mk(L"STATIC", L"50", SS_LEFT | SS_CENTERIMAGE, L + 480, sy + 22, 60, 26, ID_SENS_VAL, g.fontBold);
    mk(L"STATIC", L"SHORTEST NOTE", SS_LEFT, L + 560, sy, 160, 20, -1, g.fontSmall);
    g.shortest = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, L + 555, sy + 22, 220, 28, ID_SHORT);
    SendMessageW(g.shortest, TBM_SETRANGE, TRUE, MAKELPARAM(kShortestNoteMinMs, kShortestNoteMaxMs)); SendMessageW(g.shortest, TBM_SETPOS, TRUE, kShortestNoteDefaultMs);
    g.shortVal = mk(L"STATIC", L"60 ms", SS_LEFT | SS_CENTERIMAGE, L + 780, sy + 22, 90, 26, ID_SHORT_VAL, g.fontBold);
    mk(L"STATIC", L"SAVE TO", SS_LEFT, L, sy + 62, 120, 20, -1, g.fontSmall);
    g.outPath = mk(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_PATHELLIPSIS | SS_NOPREFIX, L, sy + 82, 760, 26, ID_OUT_PATH);
    g.btnOut = mk(L"BUTTON", L"CHANGE...", BS_OWNERDRAW, L + 770, sy + 82, 120, 28, ID_OUT_CHANGE, g.fontBold);
    g.btnConvert = mk(L"BUTTON", L"CONVERT TO MIDI", BS_OWNERDRAW, L, sy + 124, 260, 44, ID_CONVERT, g.fontTitle);
    g.progress = mk(PROGRESS_CLASSW, L"", PBS_SMOOTH, L + 272, sy + 124, 488, 18, ID_PROGRESS);
    SendMessageW(g.progress, PBM_SETRANGE32, 0, 1000);
    SendMessageW(g.progress, PBM_SETBARCOLOR, 0, kLime); SendMessageW(g.progress, PBM_SETBKCOLOR, 0, kField);
    g.btnOpen = mk(L"BUTTON", L"OPEN FOLDER", BS_OWNERDRAW, L + 770, sy + 124, 120, 44, ID_OPEN_FOLDER, g.fontBold);
    EnableWindow(g.btnOpen, FALSE);
    g.status = mk(L"STATIC", L"Load 1 song or up to 13 stems. You can also drag files or a folder onto this window.", SS_LEFT | SS_NOPREFIX, L + 272, sy + 146, 488, 44, ID_STATUS, g.fontSmall);
    // dark drop-down menus (Windows 10 1809+ dark theme; older Windows keeps the light look) and a lime progress bar
    for (HWND c : g.rowType) SetWindowTheme(c, L"DarkMode_CFD", nullptr);
    SetWindowTheme(g.quant, L"DarkMode_CFD", nullptr);
    SetWindowTheme(g.progress, L"", L"");
    SendMessageW(g.progress, PBM_SETBARCOLOR, 0, kLime); SendMessageW(g.progress, PBM_SETBKCOLOR, 0, kField);
}

void drawButton(const DRAWITEMSTRUCT* d) {
    const bool down = d->itemState & ODS_SELECTED, off = d->itemState & ODS_DISABLED;
    const bool primary = d->CtlID == ID_CONVERT;
    const COLORREF edge = off ? RGB(0x40, 0x44, 0x4C) : kLime;
    const COLORREF fill = down ? kLime : (primary && !off ? RGB(0x2A, 0x38, 0x10) : kPanel);
    HBRUSH b = CreateSolidBrush(fill); FillRect(d->hDC, &d->rcItem, b); DeleteObject(b);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, S(primary ? 2 : 1)), edge);
    HGDIOBJ op = SelectObject(d->hDC, pen), ob = SelectObject(d->hDC, GetStockObject(NULL_BRUSH));
    Rectangle(d->hDC, d->rcItem.left, d->rcItem.top, d->rcItem.right, d->rcItem.bottom);
    SelectObject(d->hDC, op); SelectObject(d->hDC, ob); DeleteObject(pen);
    wchar_t t[128]; GetWindowTextW(d->hwndItem, t, 128);
    SetBkMode(d->hDC, TRANSPARENT); SetTextColor(d->hDC, off ? kDim : (down ? kBg : kLime));
    RECT r = d->rcItem; DrawTextW(d->hDC, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void paint(HDC dc) {
    RECT rc; GetClientRect(g.wnd, &rc);
    FillRect(dc, &rc, g.bgBrush);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, g.fontTitle);
    SetTextColor(dc, kLime); RECT t{S(24), S(14), rc.right, S(48)}; DrawTextW(dc, L"HEM STEM → MIDI", -1, &t, DT_LEFT | DT_SINGLELINE);
    SelectObject(dc, g.fontSmall); SetTextColor(dc, kDim);
    RECT st{S(24), S(46), rc.right - S(24), S(70)};
    DrawTextW(dc, L"1 song or up to 13 stems in → MIDI files out (one per stem + one with every stem). Suno takes .mid uploads in Create (desktop website, Pro/Premier).", -1, &st, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    // stem table
    RECT panel{S(16), S(122), rc.right - S(16), S(146 + 13 * 28 + 4)};
    FillRect(dc, &panel, g.panelBrush);
    SelectObject(dc, g.fontSmall); SetTextColor(dc, kDim);
    const int L = 24;
    auto head = [&](int x, const wchar_t* s) { RECT r{S(L + x), S(126), S(L + x + 300), S(146)}; DrawTextW(dc, s, -1, &r, DT_LEFT | DT_SINGLELINE); };
    head(0, L"#"); head(34, L"FILE"); head(470, L"PART TYPE"); head(612, L"RESULT");
    for (int i = 0; i < int(kMaxStems); ++i) {
        const int y = 146 + i * 28;
        if (i % 2) { RECT r{S(16), S(y - 2), rc.right - S(16), S(y + 26)}; FillRect(dc, &r, g.altBrush); }
        SelectObject(dc, g.fontBold); SetTextColor(dc, i < int(g.files.size()) ? kLime : RGB(0x40, 0x44, 0x4C));
        wchar_t n[8]; swprintf(n, 8, L"%d", i + 1); RECT r{S(L), S(y), S(L + 30), S(y + 24)}; DrawTextW(dc, n, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, g.fontSmall); SetTextColor(dc, kDim);
    RECT ft{S(24), rc.bottom - S(24), rc.right - S(24), rc.bottom - S(4)};
    DrawTextW(dc, L"Hybrid Core C++23 engine · v1.0.0 · MIDI from audio is a best guess: check it in Suno or your DAW. Accuracy test results: see the release notes.", -1, &ft, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, of);
}

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g.wnd = h;
        const UINT dpi = GetDpiForWindow(h); g.scale = dpi ? dpi / 96.0 : 1.0;
        auto mkFont = [](int pt, int weight) { return CreateFontW(-S(pt), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI"); };
        g.font = mkFont(15, FW_NORMAL); g.fontBold = mkFont(14, FW_BOLD); g.fontTitle = mkFont(22, FW_BOLD); g.fontSmall = mkFont(13, FW_NORMAL);
        g.bgBrush = CreateSolidBrush(kBg); g.panelBrush = CreateSolidBrush(kPanel); g.fieldBrush = CreateSolidBrush(kField); g.altBrush = CreateSolidBrush(kRowAlt);
        createControls();
        DragAcceptFiles(h, TRUE);
        refreshRows();
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); paint(dc); EndPaint(h, &ps); return 0; }
    case WM_DRAWITEM: drawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(l)); return TRUE;
    case WM_CTLCOLORSTATIC: {
        HDC dc = HDC(w); HWND c = HWND(l);
        const int id = GetDlgCtrlID(c);
        SetBkMode(dc, TRANSPARENT);
        if (id == ID_STATUS) SetTextColor(dc, GetPropW(c, L"bad") ? kRed : kText);
        else if (id == ID_OUT_PATH) SetTextColor(dc, kText);
        else if (id == ID_SENS_VAL || id == ID_SHORT_VAL) SetTextColor(dc, kLime);
        else if (id >= ID_ROW_RESULT && id < ID_ROW_RESULT + int(kMaxStems)) { wchar_t t[16]; GetWindowTextW(c, t, 16); SetTextColor(dc, wcsncmp(t, L"ERROR", 5) == 0 ? kRed : kLime); }
        else if (id >= ID_ROW_NAME && id < ID_ROW_NAME + int(kMaxStems)) SetTextColor(dc, (id - ID_ROW_NAME) < int(g.files.size()) ? kText : RGB(0x40, 0x44, 0x4C));
        else SetTextColor(dc, kDim);
        // rows sit on the panel colour (alternating), the rest on the background
        if (id >= ID_ROW_NAME && id < ID_ROW_REMOVE) { const int row = (id % 100); SetBkColor(dc, row % 2 ? kRowAlt : kPanel); return LRESULT(row % 2 ? g.altBrush : g.panelBrush); }
        SetBkColor(dc, kBg); return LRESULT(g.bgBrush);
    }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN: SetTextColor(HDC(w), kText); SetBkColor(HDC(w), kField); return LRESULT(g.fieldBrush);
    case WM_HSCROLL:
        if (HWND(l) == g.sens) { wchar_t t[16]; swprintf(t, 16, L"%d", int(SendMessageW(g.sens, TBM_GETPOS, 0, 0))); SetWindowTextW(g.sensVal, t); }
        if (HWND(l) == g.shortest) { wchar_t t[16]; swprintf(t, 16, L"%d ms", int(SendMessageW(g.shortest, TBM_GETPOS, 0, 0))); SetWindowTextW(g.shortVal, t); }
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(w);
        if (g.busy && id != ID_OPEN_FOLDER) return 0;
        if (id == ID_LOAD_SONG) { auto f = pickFiles(false); if (!f.empty()) { clearAll(); addFiles(f, true); } }
        else if (id == ID_LOAD_STEMS) { auto f = pickFiles(true); if (!f.empty()) { clearAll(); addFiles(f, false); } }
        else if (id == ID_CLEAR) clearAll();
        else if (id >= ID_ROW_REMOVE && id < ID_ROW_REMOVE + int(kMaxStems)) removeRow(std::size_t(id - ID_ROW_REMOVE));
        else if (id == ID_OUT_CHANGE) { auto p = pickFolder(); if (!p.empty()) { g.outDir = p; g.outDirChosen = true; refreshRows(); } }
        else if (id == ID_CONVERT) startConvert();
        else if (id == ID_OPEN_FOLDER && !g.outDir.empty()) ShellExecuteW(h, L"open", g.outDir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP d = HDROP(w);
        if (!g.busy) {
            const UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
            std::vector<fs::path> f;
            for (UINT i = 0; i < n; ++i) { wchar_t p[MAX_PATH * 4]; DragQueryFileW(d, i, p, MAX_PATH * 4); f.push_back(p); }
            std::sort(f.begin(), f.end());
            const bool single = g.files.empty() && f.size() == 1 && !fs::is_directory(f[0]);
            addFiles(f, single && modeFromName(utf8(f[0].stem().wstring())).first == kUnknownNameMode);
        }
        DragFinish(d); return 0;
    }
    case WM_APP_PROGRESS: SendMessageW(g.progress, PBM_SETPOS, WPARAM(1000 * g.progDone / std::max(1, g.progTotal.load())), 0); return 0;
    case WM_APP_DONE: finishConvert(); return 0;
    case WM_APP_SELFTEST: selfTestStep(); return 0;
    case WM_GETMINMAXINFO: { auto* mi = reinterpret_cast<MINMAXINFO*>(l); mi->ptMinTrackSize = {S(980), S(740)}; return 0; }
    case WM_CLOSE: if (g.busy) { setStatus(L"Still converting - please wait a moment.", true); return 0; } break;
    case WM_DESTROY: if (g.worker.joinable()) g.worker.join(); PostQuitMessage(g.selfFail); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<fs::path> startFiles;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--selftest" && i + 1 < argc) { g.selftest = true; g.selfOut = argv[++i]; continue; }
        startFiles.push_back(argv[i]);
    }
    LocalFree(argv);
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = proc; wc.hInstance = inst; wc.lpszClassName = L"HEMStemToMidi";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    const UINT dpi = GetDpiForSystem(); const double sc = dpi ? dpi / 96.0 : 1.0;
    RECT r{0, 0, int(980 * sc), int(740 * sc)}; AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    HWND h = CreateWindowExW(0, wc.lpszClassName, kTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!h) return 1;
    ShowWindow(h, show); UpdateWindow(h);
    if (g.selftest) {
        std::error_code ec; fs::create_directories(g.selfOut, ec);
        g.selfFiles = startFiles; PostMessageW(h, WM_APP_SELFTEST, 0, 0);
    } else if (!startFiles.empty()) addFiles(startFiles, startFiles.size() == 1 && modeFromName(utf8(startFiles[0].stem().wstring())).first == kUnknownNameMode);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
    CoUninitialize();
    return int(msg.wParam);
}
