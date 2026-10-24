# THE HYBRID CORE 2.0 — Hybrid Elite Mafia Studio Suite

A 13-stem repair, mixing and mastering studio for Windows, written from scratch in C++20.
It uses no JUCE, no Qt and no third-party libraries — only the C++ standard library and Windows itself
(WASAPI audio, Direct3D 11 graphics).

## Install

1. Download **`dist/HybridCore-Setup.exe`**.
2. Run it. Windows may show "Windows protected your PC" because the file isn't code-signed.
   Click **More info → Run anyway**.
3. Click **Yes**. It installs to `%LOCALAPPDATA%\Programs\HEM Hybrid Core 2`, adds Start menu and desktop
   shortcuts, and makes `.hcs` session files open in Hybrid Core. No admin rights are needed.

To uninstall: **Settings → Apps → THE HYBRID CORE 2.0 → Uninstall**. Your stems, sessions and exports
are never touched.

## Using it

The window is the HEM faceplate in 16:9 (title on top, console left, four rack panels right). Every knob, meter and screen on it is live. F11 = full screen.

| Part | What it does |
|---|---|
| Track rows 1–13 | Drop WAV stems on the window (or ≡ → Load Stems). Click a name to select it; drag left/right for volume; double-click for 0 dB; right-click for load/remove. M = mute, S = solo. |
| Transport | ≡ menu · ⏮ rewind · ▶ play · ■ stop · ● export master · time · tempo (drag to set) · master level · analyze (per-stem LUFS on the lanes) · ⚙ audio settings · ⊞ full screen |
| HEM STFT DESTROYER | Artifact repair for the selected stem: LIGHT/MEDIUM/HEAVY, STFT resolution, Threshold, Bleed (HF stereo phase fix), Decay. Live spectrogram. The switch or the title toggles bypass. |
| HEM RESONANCE | Dynamic harsh-resonance suppressor: Sensitivity, Frequency, Bandwidth, Reduction, Compensation, SOFT/NORMAL/FAST. The screen shows the spectrum and the live cut curve. |
| HEM TRANSIENT | Transient designer: Attack, Sustain, Punch/Speed, Gain, Mix. Live activity screens. The LED toggles bypass. |
| HEM MASTERING | Master bus: Width → glue (MODE: CLEAN/PUNCH/LOUD) → true-peak limiter (Ceiling, Threshold, Release). LUFS M/S and L/R true-peak meters; Integrated, Short-term, PSR, PLR, LRA. The selector and TARGET set the export loudness. |

Keys: Space play/stop · Home rewind · Ctrl+L load · Ctrl+E export · Ctrl+O / Ctrl+S open/save session.
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
  rendering and playing 13 stems under Wine.
- Not built yet: ASIO (it needs Steinberg's SDK; the faceplate art mentions ASIO, but playback is WASAPI).
- Stems must all have the same sample rate (there's no resampler yet).
