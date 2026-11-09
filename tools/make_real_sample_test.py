#!/usr/bin/env python3
"""HEM STEM-TO-MIDI: builds test stems from REAL recorded samples, with an answer key.

  python make_real_sample_test.py OUT_DIR DIRT_SAMPLES_DIR SPLENDID_PIANO_DIR

Sources (downloaded at test time, never stored in this repo):
  - github.com/tidalcycles/Dirt-Samples  : 'gretsch' acoustic drum kit one-shots; '808bd', '808sd',
                                           '808/CH' (closed hat), '808oh' drum-machine one-shots (no licence file in
                                           that repo, so they are only used for testing, never shipped)
  - github.com/sfzinstruments/SplendidGrandPiano : public-domain Steinway samples (MF layer)

Writes, for each stem, NAME.wav (44.1 kHz, 16-bit stereo) and NAME.csv (start_s,end_s,midi_note).
Stems:
  real_drums_kit   acoustic kit, 96 BPM, light swing and +-8 ms human timing, ghost snares
  real_drums_808   808 kit, 140 BPM, 16th hats, long 808 kick
  real_piano_chords  piano chords (4-5 notes, bass note below) + light reverb, 96 BPM
  real_piano_melody  piano single-note melody, 96 BPM
"""
import csv, os, re, sys
import numpy as np
import soundfile as sf

SR = 44100
rng = np.random.default_rng(7)


def load(path):
    d, sr = sf.read(path, dtype="float64", always_2d=True)
    d = d.mean(axis=1)
    if sr != SR:
        raise SystemExit(f"{path}: expected 44.1 kHz, got {sr}")
    return d


def place(buf, t, sample, gain):
    i = int(round(t * SR))
    n = min(len(sample), len(buf) - i)
    if n > 0:
        buf[i:i + n] += gain * sample[:n]


def write(out, name, buf, key):
    peak = np.max(np.abs(buf)) or 1.0
    buf = buf * (0.7 / peak)
    buf += rng.normal(0, 10 ** (-72 / 20), len(buf))          # -72 dB noise floor
    sf.write(os.path.join(out, name + ".wav"), np.stack([buf, buf], axis=1), SR, subtype="PCM_16")
    with open(os.path.join(out, name + ".csv"), "w", newline="") as f:
        w = csv.writer(f)
        for s, e, p in sorted(key):
            w.writerow([f"{s:.4f}", f"{e:.4f}", p])
    print(f"{name}: {len(key)} notes, {len(buf) / SR:.1f} s")


def drums_kit(dirt, out):
    g = os.path.join(dirt, "gretsch")
    kick, snare, ghost = load(f"{g}/013_kick.wav"), load(f"{g}/020_snare.wav"), load(f"{g}/021_snareghost.wav")
    ch, oh = load(f"{g}/004_closedhat.wav"), load(f"{g}/017_openhat.wav")
    bpm, bars, lead = 96.0, 8, 0.3
    beat = 60 / bpm
    buf = np.zeros(int((bars * 4 * beat + 2) * SR))
    key = []
    for bar in range(bars):
        for e in range(8):
            swing = 0.06 * beat if e % 2 else 0.0
            t = lead + (bar * 4 + e * 0.5) * beat + swing + rng.uniform(-0.008, 0.008)
            if e in (0, 5) or (e == 3 and bar % 2):
                place(buf, t, kick, rng.uniform(0.8, 1.0)); key.append((t, t + 0.1, 36))
            if e in (2, 6):
                place(buf, t, snare, rng.uniform(0.85, 1.0)); key.append((t, t + 0.1, 38))
            if e == 7 and bar % 4 == 3:
                place(buf, t, ghost, 0.5); key.append((t, t + 0.1, 38))
            if e == 7 and bar % 2 == 1:
                place(buf, t, oh, 0.6); key.append((t, t + 0.1, 46))
            elif e not in (2, 6):
                place(buf, t, ch, rng.uniform(0.5, 0.7)); key.append((t, t + 0.1, 42))
    write(out, "real_drums_kit", buf, key)


def drums_808(dirt, out):
    bd, sd = load(f"{dirt}/808bd/BD0010.WAV"), load(f"{dirt}/808sd/SD0010.WAV")
    hc, oh = load(f"{dirt}/808/CH.WAV"), load(f"{dirt}/808oh/OH10.WAV")   # 808/CH = closed hat (808hc is a conga)
    bpm, bars, lead = 140.0, 8, 0.3
    beat = 60 / bpm
    buf = np.zeros(int((bars * 4 * beat + 3) * SR))
    key = []
    kicks = [0, 6, 10]               # 16th steps
    for bar in range(bars):
        for s in range(16):
            t = lead + (bar * 4 + s * 0.25) * beat
            if s in kicks:
                # an 808 rings for seconds: cut it at the next kick like a real 808 pattern (choke)
                nxt = [k for k in kicks if k > s]
                length = ((nxt[0] - s) * 0.25 * beat) if nxt else (16 - s) * 0.25 * beat
                smp = bd[: int(length * SR)].copy()
                f = min(len(smp), int(0.01 * SR)); smp[-f:] *= np.linspace(1, 0, f)
                place(buf, t, smp, 1.0); key.append((t, t + 0.1, 36))
            if s in (4, 12):
                place(buf, t, sd, 0.8); key.append((t, t + 0.1, 38))
            if s == 14 and bar % 2:
                place(buf, t, oh, 0.45); key.append((t, t + 0.1, 46))
            elif s % 2 == 0 or (bar % 2 == 0 and s in (13, 15)):
                place(buf, t, hc, rng.uniform(0.35, 0.5)); key.append((t, t + 0.1, 42))
    write(out, "real_drums_808", buf, key)


def piano_map(piano):
    rows = []
    for line in open(os.path.join(piano, "Data", "MF.txt"), encoding="utf-8", errors="ignore"):
        m = re.search(r"lokey=(\d+) hikey=(\d+) pitch_keycenter=(\d+) sample=(.+?)\.\$EXT", line)
        if m:
            rows.append((int(m[1]), int(m[2]), int(m[3]), m[4].strip()))
    return rows


class Piano:
    def __init__(self, piano):
        self.dir = piano
        self.map = piano_map(piano)
        self.cache = {}

    def note(self, midi, dur):
        for lo, hi, centre, name in self.map:
            if lo <= midi <= hi:
                if name not in self.cache:
                    self.cache[name] = load(os.path.join(self.dir, "Samples", name + ".flac"))
                s = self.cache[name]
                ratio = 2 ** ((midi - centre) / 12)         # sampler-style repitch
                n = int(min(len(s) / ratio, (dur + 0.25) * SR))
                idx = np.arange(n) * ratio
                y = np.interp(idx, np.arange(len(s)), s)
                cut = int(dur * SR)
                if cut < n:
                    y[cut:] *= np.exp(-np.arange(n - cut) / (0.04 * SR))
                return y
        raise SystemExit(f"no piano sample for note {midi}")


def reverb(buf, wet_db=-14):
    n = int(1.2 * SR)
    ir = rng.normal(0, 1, n) * np.exp(-np.arange(n) / (0.25 * SR))
    ir /= np.sqrt(np.sum(ir ** 2))
    wet = np.fft.irfft(np.fft.rfft(buf, len(buf) + n) * np.fft.rfft(ir, len(buf) + n))[: len(buf)]
    return buf + 10 ** (wet_db / 20) * wet


def piano_chords(piano, out):
    p = Piano(piano)
    bpm, lead = 96.0, 0.3
    beat = 60 / bpm
    prog = [[45, 57, 60, 64, 67], [41, 57, 60, 65, 69], [48, 55, 60, 64, 67], [43, 55, 59, 62, 67],
            [45, 60, 64, 69], [40, 56, 59, 64], [41, 60, 65, 69], [43, 59, 62, 65, 67]]   # Am7 F C G / Am E F G7
    buf = np.zeros(int((len(prog) * 2 * 2 * beat + 3) * SR))
    key = []
    for i, ch in enumerate(prog):
        for rep in range(2):                      # each chord struck twice (half notes)
            t = lead + (i * 4 + rep * 2) * beat + rng.uniform(-0.006, 0.006)
            dur = 2 * beat * 0.92
            for m in ch:
                place(buf, t + rng.uniform(0, 0.008), p.note(m, dur), rng.uniform(0.75, 1.0))
                key.append((t, t + dur, m))
    write(out, "real_piano_chords", reverb(buf), key)


def piano_melody(piano, out):
    p = Piano(piano)
    bpm, lead = 96.0, 0.3
    beat = 60 / bpm
    mel = [(72, 1), (74, .5), (76, .5), (79, 1), (76, 1), (74, .5), (72, .5), (69, 1), (72, 2),
           (76, 1), (77, .5), (79, .5), (81, 1), (79, 1), (76, 1), (74, 1), (72, 2), (72, 1), (71, 1)]
    total = sum(d for _, d in mel)
    buf = np.zeros(int((total * beat + 3) * SR))
    key, pos = [], 0.0
    for m, d in mel:
        t = lead + pos * beat
        dur = d * beat * 0.9
        place(buf, t, p.note(m, dur), rng.uniform(0.8, 1.0))
        key.append((t, t + dur, m))
        pos += d
    write(out, "real_piano_melody", reverb(buf, -18), key)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    out, dirt, piano = sys.argv[1:]
    os.makedirs(out, exist_ok=True)
    drums_kit(dirt, out)
    drums_808(dirt, out)
    piano_chords(piano, out)
    piano_melody(piano, out)
