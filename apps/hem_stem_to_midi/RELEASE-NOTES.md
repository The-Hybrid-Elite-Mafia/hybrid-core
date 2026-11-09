## HEM STEM-TO-MIDI 1.0.0 (Windows, pre-release)

Load 1 song or up to 13 stems (WAV, MP3, M4A, FLAC, AAC, WMA) and convert them to MIDI files for Suno: one file per stem plus one file with every stem as its own track. Engine: The Hybrid Core, C++23, no JUCE.

**Download:** `HEM-StemToMIDI-1.0.0.exe` (just run it) or the zip (program + command-line tool + how-to). Windows will warn because it is not code-signed: More info, then Run anyway.

### Measured accuracy (pass bars are guesses until Steven approves them)
F-measure = how many notes match an answer key (right pitch, start within 50 ms). 1.00 is perfect.

| Test audio | Part type | Result | Bar |
|---|---|---|---|
| Built-in test song: bass | Bass | 1.00 | 0.95 met |
| Built-in test song: vocal-like tone with vibrato | Vocal | 1.00 | 0.95 met |
| Built-in test song: 4-note chords | Chords | 1.00 | 0.85 met |
| Built-in test song: drums | Drums | 0.77-0.84 | 0.95 **missed** |
| Real piano (public-domain Steinway) melody | Lead | 1.00 | 0.95 met |
| Real piano chords, 4-5 notes, reverb | Chords | 0.76 | 0.85 **missed** |
| Real 808 drum kit, 140 BPM | Drums | 0.99 | 0.95 met |
| Real acoustic drum kit, swing + ghost notes | Drums | 0.91 | 0.95 **missed** |

Not tested yet: real sung vocals, real bass guitar / 808 bass lines, real guitar, and real Suno stems. Send one Suno song's stems to test with.
