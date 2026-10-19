# SteadyVoice changes

## 1.5.0 — 9 October 2026

- Added optional Adaptive guard: signal-aware 10–50 ms hold with hysteretic crest tracking, noise-jitter tolerance, and the existing 80 ms release. Defaults Off; native previous-version settings retain their output.
- Added Sample / 4× / 8× peak detection. Oversampled modes use a 128-tap detector-only FIR, at least 5 ms guard lookahead, 63 native samples of detector delay, and 0.9 / 0.8 dB internal reserves. The audible stream remains at the host rate and is not filtered. This estimates reconstruction peaks; it does not guarantee arbitrary-waveform or downstream encoded true peaks.
- Added Quality voice, based on Smooth boost with Adaptive guard On and 8× detection. Five existing presets remain unchanged. Total preset delay is 6.3125 ms at 48 kHz.
- Accelerated FIR peak estimation with portable parallel partial sums and skipped convolution for entirely silent channel histories, without fast-math or filtering the audio stream.
- Clarified Input RMS / Output RMS and Sample peak meter labels. Added an Instance rate readout, with an Equalizer APO separate-editor-instance caveat.
- Appended two parameters without reordering the existing 21. Saves use version-5 chunks; genuine version-4 and version-1–3 chunks remain loadable with new guard options at their compatibility defaults.
- Added guard-quality, independent peak-detector, output reconstruction, latency, saved-state, and editor checks. See the current validation record for measured quality, CPU, build results, and application-level limits.

## 1.4.0 — 9 October 2026

- Added independent Softest, Soft, Median, Loud, and Loudest RMS controls (1–1000 ms), plus a target-centered median range (0–24 dB full width). Adaptive RMS is Off by default.
- Five continuously active high-passed/full-band envelope pairs blend smoothly according to an independent 12 ms full-band input classifier. Quiet anchors sit 12/24 dB below the median band; loud anchors compress toward 0 dBFS when the upper edge is below full scale. A fast catch remains available for rises more than 6 dB above the selected envelope.
- Added optional 0–20 ms lookahead for the peak guard, defaulting to 0 ms. Main leveling remains causal; processed audio and bypass share the reported sample delay. Fixed buffers and peak scheduling allocate nothing in the audio callback.
- Lookahead schedules a linear attack ahead of known peak overloads, with a 10 ms cap hold and 80 ms release. The guard retains its absolute-gain cap and the original 1.3.0 behavior at 0 ms. Protection is a fully active sample ceiling, not true-peak or downstream protection.
- Added Smooth boost: Target −16 dBFS RMS, Boost/Cut 24/40 dB, Lift/Reduce 150/20 ms, Guard On, Ceiling −1 dBFS, Adaptive RMS On, and Lookahead 5 ms. The original four presets retain their values and zero delay.
- Added Main and RMS / Lookahead tabs within the existing 900 × 720 editor. All original parameter indices and mappings remain; eight controls are appended.
- New saves use version-4 chunks. Actual 68-byte version-1–3 chunks migrate with their physical settings preserved and appended controls at defaults. Latency changes are reported to the host; changing Lookahead flushes buffered audio and should be done while paused.
- Linux DSP, loud-response, adaptive/lookahead, and actual-plugin host suites pass. Adaptive checks include exact bypass delay, band continuity, retained soft gain, overload protection, float/double, block/rate independence, parameter changes, silence, and invalid input.

No user audio was supplied; synthetic test results do not establish that the
reported audible issue is resolved. See the current validation record for
Windows build/editor results and host-specific testing limits.

## 1.3.0 — 6 October 2026

- Added a 12 ms full-band loud detector alongside the slow, high-passed lifting detector. Low-frequency energy cannot hide from the loud catch behind the detector high-pass.
- Reduce time now sets the normal response; an above-target loud onset progressively accelerates reduction to shed held boost more decisively.
- Peak guard now holds an absolute total-gain cap, avoiding an extra volume sag when the leveler or output trim takes over from the guard.
- Guard-off removes held attenuation on the next processed sample; re-enabling catches the current sample immediately.
- Retained zero added audio latency, all parameter ranges and mappings, version-3 state format, earlier state migration, factory control values, and the #6667ab motif. The response of saved settings to loud audio is intentionally revised.
- Added a test-only 1.2.0 DSP reference and comparative loud-transition, low-frequency overboost, guard handoff/toggle, timing/target/trim, sample-rate, and block/in-place regressions.
- 158 DSP, 46 loud-response, and 1,227 actual-plugin host assertions pass, including sanitizer runs. Rebuilt the Windows DLL and packaged source and results.

Guarded samples held their ceiling in both 1.2.0 and 1.3.0 tests; clipping in the
user's signal chain has not been reproduced or localized. Windows editor and
actual OBS/Equalizer APO execution remain unverified in this environment.

## 1.2.0 — 6 October 2026

- Maximum cut now spans 0 to 40 dB.
- Lift time now spans 0 to 4000 ms; Reduce time now spans 0 to 1000 ms.
- Time sliders use a zero-inclusive logarithmic scale. Zero disables gain smoothing and interpolation for that direction at the controller update; divide-by-zero is explicitly avoided.
- Positive sub-millisecond settings are displayed without being rounded to zero.
- Version-1 and version-2 saved chunks migrate automatically, retaining physical dB and millisecond values. New saves use version 3.
- Factory settings, parameter order, plugin identity, #6667ab motif, pause protection, and zero added audio latency are retained.
- Added measured 40 dB cut, same-tick zero-time response, near-zero time, floating-point exception, migration, and Windows editor endpoint checks.
- Rebuilt the complete release ZIP; verified CRC and SHA-256 integrity before delivery.

DSP and Linux host tests and memory-safety checks pass. Windows editor execution
and actual OBS/Equalizer APO operation remain unverified in the build environment.

## 1.1.0 — 6 October 2026

- Target level now spans -36 to 0 dBFS RMS.
- Maximum boost now spans 0 to +40 dB.
- Peak ceiling now spans -12 to 0 dBFS.
- Native editor now uses #6667ab with monochrome tints and shades, including numeric entry.
- Version-1 saved plugin chunks migrate automatically while preserving physical dB values; new saves use version 2.
- Plugin identity, parameter order, factory settings, and zero-sample added latency are retained.
- Added endpoint, high-gain transient, migration, malformed-state, host-display, and Windows editor regression checks.

DSP and Linux host tests and memory-safety checks pass. Windows editor execution
and actual OBS/Equalizer APO operation remain unverified in the build environment.

## 1.0.0

Initial release candidate.
