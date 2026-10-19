# SteadyVoice 1.5.0 quick start

Extract **SteadyVoice-1.5.0-Windows-x64.zip**. Unload the old plugin and close
applications using it before replacing `SteadyVoice.dll`, then restart them.

1. In 64-bit Equalizer APO, add a VST plugin filter and browse to the DLL.
   Use one active SteadyVoice instance in the microphone path. For an input-1
   mic on a stereo recording endpoint, keep `Copy: R=L` immediately after
   SteadyVoice in the microphone configuration if that routing resolved your
   mono level loss. Do not add a second SteadyVoice to the same APO-processed
   input in OBS.
2. Select **Quality voice**. It starts at Target **−16 dBFS RMS**, Boost/Cut
   **24/40 dB**, Lift/Reduce **150/20 ms**, Ceiling **−1 dBFS**, Guard **On**,
   Adaptive RMS **On**, Adaptive guard **On**, **8×** peak detection, and
   Lookahead **5 ms**. Keep Output trim at 0 dB and Bypass Off.
3. Set Noise floor above room noise and below soft speech; the preset starts
   at **−50 dBFS**, with **600 ms** pause hold. Record quiet speech followed
   immediately by a loud phrase, normal speech, and a pause.
4. Open **RMS / Quality** for detector settings. The five RMS durations
   default to **200/120/80/24/8 ms**. Median range defaults to **6 dB** full
   width centered on Target. These are power-detector time constants, not
   exact windows; Lift and Reduce separately smooth the gain changes.
5. Compare **Sample / 4× / 8×** peak detection. Sample retains the original
   native guard. Oversampled modes estimate peaks between samples using a
   detector-only filter; the audible stream stays at the host sample rate.
   Fourfold uses less CPU and more headroom; eightfold gives a closer estimate.
   Guard reserves are **0.9 dB / 0.8 dB** for 4× / 8×.
6. Adjust Lookahead and peak mode while paused: changing them flushes buffered
   audio and can briefly interrupt output. Lower Target if guard reduction is
   frequent, and raise Maximum boost only as far as quiet speech needs.

**Latency:** oversampled modes use **at least 5 ms** of guard lookahead and add
**63 native samples** for detection. Requested values below 5 ms still use that
minimum, because instantaneous gain cuts can create new reconstruction peaks.
At 48 kHz, Quality voice adds **6.3125 ms**; at 192 kHz, **5.328125 ms**.
Processed audio, Guard Off, and bypass share the reported delay. Host compensation
cannot remove live microphone delay. Restart the host pipeline if it does not
honor latency updates.

**Sample rate:** Windows and Equalizer APO determine the capture rate. The
plugin's **Instance rate** is a readout, while peak detection is selectable.
An APO editor/analysis instance may show a different rate and idle meters from
the live processing instance. For streaming, matching capture and OBS to 48 kHz
usually avoids excess sample work and downstream conversion.

**Meters and peaks:** the large readings are RMS; the smaller readings are
Sample peak. Oversampled guarding estimates reconstruction peaks, and the
native sample ceiling remains enforced with Guard On and Bypass fully Off.
It cannot repair upstream clipping or guarantee arbitrary-waveform, mixed,
resampled, or encoded dBTP limits. A target of −8 dBFS RMS with 12 dB crest
factor would demand +4 dBFS peaks; lowering the target leaves room for them.

**Compatibility:** the five older presets retain their settings. Version-4
chunks load with Adaptive guard Off and Sample detection; real version-1–3
chunks also migrate. New version-5 saves require this update. The first 21
parameter indices remain unchanged, with two new parameters appended.

The package includes full source, build recipes, licenses, validation records,
and a standalone synthetic Windows test host. See `docs/VALIDATION.md` for
measured results and their limits. Actual speech recordings remain useful for
judging sound quality in your signal chain.

For 64-bit OBS hosting, install the DLL under
`C:\Program Files\Common Files\VST2\`, restart OBS, and add its VST 2.x filter.
OBS scan locations: https://obsproject.com/kb/vst-2-x-plugin-filter
