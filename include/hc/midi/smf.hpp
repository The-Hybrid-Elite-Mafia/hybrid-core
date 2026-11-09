// =============================================================================
//  midi/smf.hpp — Standard MIDI File (type 1) writer and reader. No libraries.
//
//  Notes are kept in SECONDS while transcribing. When the file is written they become ticks
//  using the tempo that is also written into the file, so timing in seconds is exact whatever
//  the tempo is; the tempo only decides where the bar lines fall.
//
//  Optional quantize snaps note starts/ends to a grid (1/4 .. 1/32) anchored on the first
//  detected beat (beatOffsetSec), so the grid follows the music rather than time zero.
// =============================================================================
#pragma once
#include "hc/midi/midi_config.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace hc::midi {

struct Note {
    double start = 0, end = 0;   // seconds
    int pitch = 60;              // MIDI note number
    int velocity = 100;          // 1..127
};

struct Track {
    std::string name;
    int channel = 0;             // 0..15 (9 = drums)
    int program = 0;             // 0-based General MIDI program; ignored on channel 9
    std::vector<Note> notes;
};

struct WriteOptions {
    double bpm = kFallbackBpm;
    double beatOffsetSec = 0.0;  // where beat 1 of the grid sits
    int quantizeDiv = 0;         // 0 = off, else 4, 8, 16, 32 (notes per whole bar of 4/4)
};

// Ticks for a time in seconds (before quantize).
inline std::int64_t secondsToTicks(double sec, double bpm) {
    return static_cast<std::int64_t>(std::llround(sec * bpm / 60.0 * kPpq));
}

// Grid step in ticks for a quantize division (4 = quarter notes, 16 = sixteenths).
inline std::int64_t gridTicks(int div) { return div > 0 ? (std::int64_t(kPpq) * 4) / div : 0; }

struct TickNote { std::int64_t on = 0, off = 0; int pitch = 0, velocity = 0, channel = 0; };

inline std::vector<TickNote> toTicks(const Track& t, const WriteOptions& o) {
    std::vector<TickNote> out;
    out.reserve(t.notes.size());
    const std::int64_t g = gridTicks(o.quantizeDiv);
    const std::int64_t off = secondsToTicks(o.beatOffsetSec, o.bpm);
    for (const auto& n : t.notes) {
        std::int64_t a = secondsToTicks(n.start, o.bpm), b = secondsToTicks(n.end, o.bpm);
        if (g > 0) {
            auto snap = [&](std::int64_t x) {
                const double k = std::round(double(x - off) / double(g));
                return off + static_cast<std::int64_t>(k) * g;
            };
            a = snap(a); b = snap(b);
            if (b <= a) b = a + g;                    // a note never vanishes
            if (a < 0) { b -= a; a = 0; }
        }
        if (b <= a) b = a + 1;
        out.push_back({a, b, std::clamp(n.pitch, 0, 127), std::clamp(n.velocity, 1, 127), t.channel});
    }
    // Same pitch overlapping itself after quantize: cut the earlier one short.
    std::sort(out.begin(), out.end(), [](const TickNote& x, const TickNote& y) { return x.on < y.on || (x.on == y.on && x.pitch < y.pitch); });
    for (std::size_t i = 0; i < out.size(); ++i)
        for (std::size_t j = i + 1; j < out.size() && out[j].on < out[i].off; ++j)
            if (out[j].pitch == out[i].pitch) out[i].off = std::max(out[i].on + 1, out[j].on);
    // Exact duplicates (same pitch, same start) are removed.
    out.erase(std::unique(out.begin(), out.end(), [](const TickNote& x, const TickNote& y) { return x.on == y.on && x.pitch == y.pitch; }), out.end());
    return out;
}

namespace detail {
inline void vlq(std::vector<std::uint8_t>& b, std::uint32_t v) {
    std::uint8_t tmp[5]; int n = 0;
    tmp[n++] = v & 0x7F;
    while (v >>= 7) tmp[n++] = static_cast<std::uint8_t>((v & 0x7F) | 0x80);
    while (n) b.push_back(tmp[--n]);
}
inline void be32(std::vector<std::uint8_t>& b, std::uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<std::uint8_t>(v >> s)); }
inline void be16(std::vector<std::uint8_t>& b, std::uint16_t v) { b.push_back(static_cast<std::uint8_t>(v >> 8)); b.push_back(static_cast<std::uint8_t>(v)); }
inline void meta(std::vector<std::uint8_t>& b, std::uint8_t type, const std::string& s) {
    vlq(b, 0); b.push_back(0xFF); b.push_back(type); vlq(b, static_cast<std::uint32_t>(s.size())); b.insert(b.end(), s.begin(), s.end());
}
inline void chunk(std::vector<std::uint8_t>& file, const char* id, const std::vector<std::uint8_t>& body) {
    file.insert(file.end(), id, id + 4); be32(file, static_cast<std::uint32_t>(body.size())); file.insert(file.end(), body.begin(), body.end());
}
} // namespace detail

// Builds the bytes of a type-1 file: track 0 = tempo map, then one track per Track.
inline std::vector<std::uint8_t> build(const std::vector<Track>& tracks, const WriteOptions& o, const std::string& songName = "HEM") {
    using namespace detail;
    std::vector<std::uint8_t> file;
    std::vector<std::uint8_t> hdr; be16(hdr, 1); be16(hdr, static_cast<std::uint16_t>(tracks.size() + 1)); be16(hdr, kPpq);
    chunk(file, "MThd", hdr);

    std::vector<std::uint8_t> t0;
    meta(t0, 0x03, songName);
    const std::uint32_t usPerQ = static_cast<std::uint32_t>(std::llround(60'000'000.0 / std::max(1.0, o.bpm)));
    vlq(t0, 0); t0.insert(t0.end(), {0xFF, 0x51, 0x03, std::uint8_t(usPerQ >> 16), std::uint8_t(usPerQ >> 8), std::uint8_t(usPerQ)});
    vlq(t0, 0); t0.insert(t0.end(), {0xFF, 0x58, 0x04, 4, 2, 24, 8});     // 4/4
    vlq(t0, 0); t0.insert(t0.end(), {0xFF, 0x2F, 0x00});
    chunk(file, "MTrk", t0);

    for (const auto& tr : tracks) {
        std::vector<std::uint8_t> b;
        meta(b, 0x03, tr.name);
        const std::uint8_t ch = static_cast<std::uint8_t>(tr.channel & 0x0F);
        if (tr.channel != kDrumChannel) { vlq(b, 0); b.push_back(0xC0 | ch); b.push_back(static_cast<std::uint8_t>(std::clamp(tr.program, 0, 127))); }
        struct Ev { std::int64_t t; int kind; int pitch, vel; };   // kind 0 = off (sorted first), 1 = on
        std::vector<Ev> ev;
        for (const auto& n : toTicks(tr, o)) { ev.push_back({n.on, 1, n.pitch, n.velocity}); ev.push_back({n.off, 0, n.pitch, 0}); }
        std::sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& c) { return a.t < c.t || (a.t == c.t && (a.kind < c.kind || (a.kind == c.kind && a.pitch < c.pitch))); });
        std::int64_t last = 0;
        for (const auto& e : ev) {
            vlq(b, static_cast<std::uint32_t>(e.t - last)); last = e.t;
            b.push_back(static_cast<std::uint8_t>((e.kind ? 0x90 : 0x80) | ch));
            b.push_back(static_cast<std::uint8_t>(e.pitch)); b.push_back(static_cast<std::uint8_t>(e.kind ? e.vel : 64));
        }
        vlq(b, 0); b.insert(b.end(), {0xFF, 0x2F, 0x00});
        chunk(file, "MTrk", b);
    }
    return file;
}

inline std::string write(const std::filesystem::path& p, const std::vector<Track>& tracks, const WriteOptions& o, const std::string& songName = "HEM") {
    const auto bytes = build(tracks, o, songName);
    std::ofstream f(p, std::ios::binary);
    if (!f) return "cannot create MIDI file";
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return f ? std::string{} : "write failed";
}

// ---- Reader (used by the tests and the self-check after every conversion) -------------------
struct ParsedTrack { std::string name; int program = -1; std::vector<TickNote> notes; };
struct Parsed {
    int format = -1, ppq = 0; double bpm = 0; int timeSigNum = 0;
    std::vector<ParsedTrack> tracks;
    std::string error;
};

inline Parsed parse(const std::vector<std::uint8_t>& d) {
    Parsed r;
    auto be32r = [&](std::size_t p) { return (std::uint32_t(d[p]) << 24) | (std::uint32_t(d[p + 1]) << 16) | (std::uint32_t(d[p + 2]) << 8) | d[p + 3]; };
    if (d.size() < 14 || std::string(d.begin(), d.begin() + 4) != "MThd") { r.error = "not a MIDI file"; return r; }
    r.format = (d[8] << 8) | d[9];
    const int ntr = (d[10] << 8) | d[11];
    r.ppq = (d[12] << 8) | d[13];
    std::size_t p = 8 + be32r(4);
    for (int t = 0; t < ntr; ++t) {
        if (p + 8 > d.size() || std::string(d.begin() + p, d.begin() + p + 4) != "MTrk") { r.error = "bad track header"; return r; }
        const std::size_t len = be32r(p + 4), end = p + 8 + len;
        if (end > d.size()) { r.error = "track runs past end of file"; return r; }
        p += 8;
        ParsedTrack tr;
        std::int64_t now = 0; std::uint8_t status = 0;
        std::vector<std::vector<std::pair<std::int64_t, int>>> open(16 * 128);
        // open[ch][pitch] holds (onTick, velocity) waiting for their note-off
        while (p < end) {
            std::uint32_t dt = 0; std::uint8_t c;
            do { c = d[p++]; dt = (dt << 7) | (c & 0x7F); } while ((c & 0x80) && p < end);
            now += dt;
            std::uint8_t s = d[p];
            if (s & 0x80) { status = s; ++p; } else s = status;
            if (s == 0xFF) {
                const std::uint8_t type = d[p++];
                std::uint32_t l = 0; do { c = d[p++]; l = (l << 7) | (c & 0x7F); } while (c & 0x80);
                if (type == 0x03) tr.name.assign(d.begin() + p, d.begin() + p + l);
                if (type == 0x51 && l == 3) r.bpm = 60'000'000.0 / double((d[p] << 16) | (d[p + 1] << 8) | d[p + 2]);
                if (type == 0x58 && l >= 1) r.timeSigNum = d[p];
                p += l;
            } else if (s == 0xF0 || s == 0xF7) {
                std::uint32_t l = 0; do { c = d[p++]; l = (l << 7) | (c & 0x7F); } while (c & 0x80);
                p += l;
            } else {
                const int kind = s & 0xF0, ch = s & 0x0F;
                if (kind == 0xC0 || kind == 0xD0) { if (kind == 0xC0) tr.program = d[p]; p += 1; }
                else {
                    const int a = d[p], b = d[p + 1]; p += 2;
                    if (kind == 0x90 && b > 0) open[ch * 128 + a].push_back({now, b});
                    else if ((kind == 0x80 || (kind == 0x90 && b == 0)) && !open[ch * 128 + a].empty()) {
                        const auto [on, vel] = open[ch * 128 + a].front(); open[ch * 128 + a].erase(open[ch * 128 + a].begin());
                        tr.notes.push_back({on, now, a, vel, ch});
                    }
                }
            }
        }
        std::sort(tr.notes.begin(), tr.notes.end(), [](const TickNote& x, const TickNote& y) { return x.on < y.on || (x.on == y.on && x.pitch < y.pitch); });
        r.tracks.push_back(std::move(tr));
        p = end;
    }
    return r;
}

inline Parsed parseFile(const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) { Parsed r; r.error = "cannot open"; return r; }
    std::vector<std::uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    return parse(d);
}

} // namespace hc::midi
