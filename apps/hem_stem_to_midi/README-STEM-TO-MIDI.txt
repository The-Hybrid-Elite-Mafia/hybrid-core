HEM STEM-TO-MIDI 1.0.0  -  Hybrid Elite Mafia
================================================

WHAT IT DOES
Turns 1 song, or up to 13 stems, into MIDI files you can upload to Suno.

HOW TO USE
1. Double-click HEM-StemToMIDI.exe. Windows may say "Windows protected your PC" because the
   program is not code-signed: click "More info", then "Run anyway".
2. Click LOAD STEMS (UP TO 13) and pick your stems (Ctrl+A in the folder picks all of them).
   Or click LOAD 1 SONG for a single full song. You can also drag files or a whole folder
   onto the window. Files: WAV, MP3, M4A, FLAC, AAC, WMA.
3. Check the PART TYPE of each row. "Auto" guesses from the file name (Drums, Bass, Vocals,
   Guitar, Keyboard...). Change it if it guessed wrong, or pick Skip to leave a stem out.
     Drums      kick, snare, closed hat, open hat (MIDI channel 10)
     Bass       one note at a time, low
     Vocal      one note at a time, singing range (the melody of the lead vocal)
     Lead       one note at a time, wide range (lead synth, solo, flute, sax...)
     Chords     several notes at once (keys, guitar, strings, pads, backing vocals)
     Full song  a whole mix in one file: a chords track + a drums track (messier)
4. Optional settings:
     TEMPO (BPM)    empty = the program finds it. Type a number to use your own.
     QUANTIZE       snaps notes to a grid (1/4 .. 1/32). Off keeps the real timing.
     SENSITIVITY    higher = keeps quieter / less certain notes. Lower = only clear notes.
     SHORTEST NOTE  notes shorter than this are dropped.
5. Click CONVERT TO MIDI. Files go to a "HEM MIDI" folder next to your audio (or the
   folder you pick with CHANGE...):
     <song> - ALL STEMS.mid   one track per stem
     <song> - <stem>.mid      one file per stem
   Every file is read back and checked after it is written.

SUNO
Suno's Create page takes .mid / .midi uploads (desktop website, Pro or Premier plan, since
Nov 4 2026). The Suno phone app did not take MIDI at launch.

BE AWARE
MIDI from audio is always a best guess. Check it in Suno or a DAW. Drum and chord parts are
the hardest; one-note parts (bass, vocal melody, lead) are the most accurate. See the release
notes for measured accuracy.

hem_midi_cli.exe does the same job from a command prompt (for testing).
