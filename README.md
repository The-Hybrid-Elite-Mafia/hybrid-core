# THE HYBRID CORE 2.0 — Hybrid Elite Mafia Studio Suite

A 13-stem repair, mixing and mastering studio for Windows, written from scratch in C++23 (MSVC).
It uses no JUCE and no Qt — only the C++ standard library and Windows itself (WASAPI audio, Direct3D 11
graphics), plus the Steinberg ASIO SDK for ASIO output. The program is released under GPLv3 (see LICENSE and
THIRD-PARTY-NOTICES.txt); the ASIO SDK is downloaded at build time and never stored in this repo.

## Install

1. Download **`dist/HybridCore-Setup.exe`**.
2. Run it. Windows may show "Windows protected your PC" because the file isn't code-signed.
   Click **More info → Run anyway**.
3. Click **Yes**. It installs to `%LOCALAPPDATA%\Programs\HEM Hybrid Core 2`, adds Start menu and desktop
   shortcuts, and makes `.hcs` session files open in Hybrid Core. No admin rights are needed.

To uninstall: **Settings → Apps → THE HYBRID CORE 2.0 → Uninstall**. Your stems, sessions and exports
are never touched.

## Using it

Three pages, tabs top-right: **CONSOLE** (the tracks fill the window), **RACK** (the four panels; `< STEM 7 - VOCALS >` top-left picks which stem the DESTROYER / RESONANCE / TRANSIENT panels are on — each stem has its own copy of those three, MASTERING is the whole mix; double-click one to fill the window with it, again or Esc to go back) and **SHOW** (the whole 16:9 faceplate). Tab flips CONSOLE / RACK, F9 = SHOW, F11 = full screen. Every knob, meter and screen is live.

| Part | What it does |
|---|---|
| Track rows 1–13 | Drag the stems — or their whole folder — onto the window. Or click the box in the middle (≡ → Load Stems, Ctrl+L): the picker shows the WAVs; click any one and its whole folder loads, or select just the ones you want. Click a name to select it; drag left/right for volume; double-click for 0 dB; right-click for load/remove. M = mute, S = solo. |
| Transport | Every button is drawn live (lights, presses, hover) — nothing painted. ≡ menu · ⏮ rewind · ▶ play · ■ stop · ● export master · ⟳ LOOP · time · tempo (drag to set) · master level · analyze (per-stem LUFS on the lanes) · ⚙ audio settings · ⊞ full screen |
| HEM STFT DESTROYER | Artifact repair for the selected stem: LIGHT/MEDIUM/HEAVY, STFT resolution, Threshold, Bleed (HF stereo phase fix), Decay. Live spectrogram. The switch or the title toggles bypass. |
| HEM RESONANCE | Dynamic harsh-resonance suppressor: Sensitivity, Frequency, Bandwidth, Reduction, Compensation, SOFT/NORMAL/FAST. The screen shows the spectrum and the live cut curve. |
| HEM TRANSIENT | Transient designer: Attack, Sustain, Punch/Speed, Gain, Mix. Live activity screens. The LED toggles bypass. |
| HEM MASTERING | Master bus: Width → glue (MODE: CLEAN/PUNCH/LOUD) → true-peak limiter (Ceiling, Threshold, Release). LUFS M/S and L/R true-peak meters; Integrated, Short-term, PSR, PLR, LRA. The selector and TARGET set the export loudness. |

Loop: drag across the bar numbers to set the section (snaps to whole bars; Shift = free), click the bar numbers to jump; LOOP or **L** turns it on/off; with no section set it loops the whole song. The wrap is sample-exact inside the audio engine (no gap). Export always renders the whole song. Saved in the session.

Keys: Space play/stop · Home rewind · L loop · Ctrl+L load · Ctrl+E export · Ctrl+O / Ctrl+S open/save session · Ctrl+Shift+S save as. Unsaved changes show a * in the title bar; closing, Clear All Stems or Open Session asks "Save changes?" first.
Knobs: drag up/down (Shift = fine), mouse wheel, double-click = default.

Export writes a 24-bit WAV. With a target set, it renders three passes (measure → limit → verify) so the
master lands on the target LUFS under the true-peak ceiling.

## Build

- **From Linux (the tested path):** `./build_windows.sh` → `dist/HybridCore-Setup.exe` (MinGW-w64).
- **Windows / Visual Studio 2022:** `cmake -B build -G "Visual Studio 17 2022" -A x64` then
  `cmake --build build --config Release`. This adds a `HybridCore` target. The MSVC build of the Studio
  app has **not** been tested yet; the MinGW build has.
- **Engine tests (any OS):** build and run `hc_tests` — it should end `ALL PASSED`.

## Status

- Tested: engine DSP (FFT, STFT, BS.1770 loudness, true peak, limiter, Artifact Destroyer, Resonance,
  Transient, Width), the 13-stem engine (sanitizers clean), the installer and uninstaller, and the app
  rendering and playing 13 stems under Wine; every build now also runs all tests, the real app (screenshots) and an
  install / start / uninstall test on a Windows runner.
- ASIO output is built in (Settings menu lists the 64-bit ASIO drivers installed on the PC; the choice is remembered in
  %LOCALAPPDATA%\HEM Hybrid Core 2\settings.ini; an unavailable driver falls back to Windows audio). It compiles and the
  app starts on the build machine, but it has NOT been tried with a real ASIO driver or sound card yet.
- Real audio playback (WASAPI) and the real GPU have not been tested on the build machine (it has no sound device and
  uses a software graphics driver). Test results, screenshots and the install test come from GitHub Actions (branch ci-results-*).
- Stems at different sample rates are converted when loaded (to the rate of the first stem). Tested: see the resampler section of the test run.

## HEM STEM-TO-MIDI (branch `stem-to-midi`)

A second program built on the same C++23 core: load 1 song or up to 13 stems and get MIDI files for Suno
(one per stem + one with every stem). Engine: `include/hc/midi/` (every guess in `midi_config.hpp`, marked
ASSUMED). App: `apps/hem_stem_to_midi/`. Tests: `tests/test_midi.cpp` (built-in test song) and
`tests/test_midi_real.cpp` (stems made from real recorded samples by `tools/make_real_sample_test.py`).
Windows build, tests, pictures and the pre-release: `.github/workflows/stem-to-midi.yml`.
