# SteadyVoice 1.5.0

A Windows x64 microphone leveler with optional adaptive RMS detection and
0–20 ms lookahead and selectable oversampled peak estimation. It runs locally on the CPU without an
external runtime, network access, telemetry, or account dependency.

Version 1.5.0 adds an optional adaptive guard and **Sample / 4× / 8×** peak
estimation, plus a **Quality voice** preset. The audio remains at the host
sample rate: oversampling applies only to the peak detector and does not filter
the audible stream. Native gain and detector math use double precision.

The new guard options default to Off / Sample. Existing 1.4.0 presets and saved
settings retain their processing behavior. Validation results, CPU measurements,
and the limits of estimated peak protection are recorded in `docs/VALIDATION.md`.

## Install and start

Close audio applications and unload the previous plugin before replacing
`SteadyVoice.dll`, then restart them.

For 64-bit OBS, copy the DLL to
`C:\Program Files\Common Files\VST2\`, restart OBS, and add **VST 2.x Plug-in**
to the microphone filters. Select **SteadyVoice** and open its interface.
OBS scan locations: https://obsproject.com/kb/vst-2-x-plugin-filter

For 64-bit Equalizer APO, add a VST plugin filter and browse to the DLL. Use
one active instance in the microphone path. If Equalizer APO already processes
the mic, avoid adding a second copy to that processed input in OBS.

Start with **Quality voice** for the quality options, **Smooth boost** for
native-sample protection, or **Broadcast** for the original zero-latency behavior. Set Noise floor above room noise and below
soft speech, then record soft speech immediately followed by a loud phrase,
normal speech, and a pause. Compare the plugin output with the final recording.

| Control | Broadcast | Smooth boost |
| --- | ---: | ---: |
| Target level | −22 dBFS RMS | −16 dBFS RMS |
| Maximum boost / cut | 12 / 24 dB | 24 / 40 dB |
| Lift / Reduce time | 600 / 100 ms | 150 / 20 ms |
| Noise floor / Pause hold | −52 dBFS / 450 ms | −50 dBFS / 600 ms |
| Peak ceiling | −3 dBFS | −1 dBFS |
| Leveling strength | 90% | 100% |
| Detector high-pass | 80 Hz | 40 Hz |
| Output trim | 0 dB | 0 dB |
| Peak guard / Bypass | On / Off | On / Off |
| Adaptive RMS / Quality | Off / 0 ms | On / 5 ms |

Quality voice uses the Smooth boost values above, with **Adaptive guard On**
and **8× peak detection**. Its total added delay is **6.3125 ms at 48 kHz**
(240 lookahead samples plus 63 detector samples), or **5.328125 ms at 192 kHz**.
In Equalizer APO, keep `Copy: R=L` immediately after SteadyVoice if your input-1
microphone occupies only the left channel of a two-channel recording endpoint.
Apply any later gain before your final limiter.

## Why the screenshot settings can clip

Target requests an average RMS level, rather than a peak limit or LUFS value.
Speech at −8 dBFS RMS with peaks 12 dB above its average would need +4 dBFS
peaks. Increasing Maximum boost can lift softer speech, but also leaves more
gain active when a louder passage arrives. Maximum cut specifies available
reduction, not detector speed. With Guard Off, the output can exceed full scale.

The original guard reduces gain before samples leave the plugin; it does not
attenuate already clipped plugin output. Frequent instantaneous cuts can still
alter transients or suppress a passage. A high RMS target may demand continued
peak reduction, and 4 ms gain smoothing can produce audible modulation.
Lowering the target leaves more room for peaks while allowing a substantial
boost on quiet speech. Smooth boost supplies a starting point for that balance.

The plugin cannot repair distortion already present at the mic, preamp,
interface, or an earlier filter. A later fader, mix, filter, resampling step,
or encoder can also raise peaks after this plugin's guard.

## Adaptive RMS

Open the **RMS / Quality** tab and enable **Adaptive RMS**. Each RMS duration
is independently adjustable from 1 to 1000 ms. Defaults are:

| Band | Duration | Input-level anchor |
| --- | ---: | --- |
| Softest | 200 ms | 24 dB below the median band's lower edge, or lower |
| Soft | 120 ms | 12 dB below the median band's lower edge |
| Median | 80 ms | Inside the target-centered median band |
| Loud | 24 ms | Halfway from the upper edge toward 0 dBFS, up to 12 dB above it |
| Loudest | 8 ms | Twice that distance above the upper edge, or higher |

**Median range** is the full width of the band, from 0 to 24 dB; its default
is 6 dB. At Target −16 dBFS, a 6 dB range covers −19 to −13 dBFS. Softest
and Soft anchors are −43 and −31 dBFS. Above the median band, the anchor
spacing is the smaller of 12 dB or half the distance to 0 dBFS: Loud is
−6.5 dBFS and Loudest is 0 dBFS in this example. At Target −8 dBFS with
the same range, Loud is −2.5 dBFS and Loudest remains 0 dBFS. This keeps
the loud ranges useful near full scale. The editor shows the actual anchors.

If the median band's upper edge is already at or above 0 dBFS, upper spacing
falls back to 12/24 dB beyond it. Floating-point hosts can carry such
above-full-scale samples; this does not imply a safe recording level. These
are input classifier levels relative to Target, not measured percentiles or
output-level categories. Median range selects detector behavior; it is not a
band where gain correction stops.

An independent 12 ms full-band RMS detector selects the band. Five high-passed
and five full-band envelopes stay active continuously, including while
Adaptive RMS is Off. Between anchors, the controller smoothly interpolates
their powers, avoiding a reset when the selected range changes. An independent
fast catch remains available for full-band rises more than 6 dB above the
selected envelope.

RMS durations are exponential **power-detector time constants**, not exact
sliding-window lengths or audio delays. Longer durations stabilize the reading
but can retain a loud passage's history into a short quiet passage. Shorter
durations respond faster but follow more syllable and waveform variation.
The controls can use any ordering; the preset uses progressively shorter
durations for louder input.

**Lift time and Reduce time remain separate gain-smoothing controls.** For a
constant requested gain in dB, one time constant applies about 63% of the gain
change and three apply about 95%. Loud overshoots accelerate reduction. At
0 ms, that direction applies the requested gain at the next controller update,
about every 2 ms; detection still takes time. Very fast settings can sound
abrupt. Pause recovery remains smooth.

With Adaptive RMS Off, the original 80 ms high-passed detector and 12 ms
full-band loud detector govern leveling. The input RMS meter remains the
80 ms high-passed measurement; it does not change to show the selected band.
Noise-floor activity uses the fast high-passed detector and opens around 3 dB
above the floor. Below the floor, further lifting stops; held gain can still
raise noise until pause recovery returns it toward unity. This is level
detection, not speech recognition or a noise gate.

## Lookahead and peak protection

**Lookahead** selects 0–20 ms of audio delay, rounded to whole samples. At
48 kHz, native Sample detection with 5 ms adds 240 samples. Processed audio and bypass use the same total delay,
including when Guard is Off. Live monitoring therefore gains that much delay
in addition to the host and interface buffering.

Lookahead anticipates **the peak guard**. The main leveling gain is calculated
causally and stored alongside the delayed samples; it does not use future RMS
information. Longer RMS durations still take time to react.

With **Adaptive guard Off** and **Sample** detection, the original 10 ms cap
hold and 80 ms release remain. Lookahead schedules a finite linear attack before
an upcoming overload. The guard holds an absolute total-gain cap so the main
leveler can take over without unnecessary double attenuation.

**Adaptive guard** learns recurring waveform-peak spacing and adjusts hold from
10 to 50 ms. Hysteretic crest tracking rejects shallow noise ripples; a 0.1 dB
refresh tolerance keeps almost equal peaks from causing repeated release and
recutting. The release time constant stays 80 ms, and peak-spacing history expires
after 100 ms without a qualifying crest. Sustained low-frequency limiting is more
stable, at the cost of retaining attenuation slightly longer in some passages.

**Peak detection** selects Sample, 4×, or 8×. Both oversampled modes use a
128-tap detector-only interpolation filter, adding **63 native samples** to the
effective guard lookahead. Oversampled modes use **at least 5 ms** of guard
lookahead, even if the requested Lookahead is lower: instantaneous gain cuts
can create reconstruction peaks themselves. This fixed FIR part is 1.3125 ms at 48 kHz, 0.65625 ms at 96 kHz, or
0.328125 ms at 192 kHz. A 4× mode uses fewer interpolated positions and costs
less CPU; 8× gives a closer peak estimate. Internal guard reserves are 0.9 dB
for 4× and 0.8 dB for 8×, so sustained limiting may settle below the configured
ceiling. Quiet audio below the guard threshold remains unfiltered.

Oversampled detection is **estimated true-peak protection**. Finite interpolation,
a varying gain envelope, subsequent resampling, mixing, and encoding prevent an
arbitrary-waveform or downstream dBTP guarantee. The small meter readings are
explicitly **sample peaks**, including in 4×/8× modes. With Guard On and Bypass
fully Off, the native sample ceiling is enforced in every mode. Bypass and its
transition include original audio that can exceed the ceiling.

The editor displays **Instance rate**. Windows and the host determine the actual
capture format; this control is a readout. Equalizer APO can create a separate
editor/analysis instance, whose displayed rate and meters need not describe the
live processing instance. To change the microphone capture rate, use Windows
recording-device settings and compatible host settings. A 48 kHz stream usually
avoids the extra work and downstream conversion of a 192 kHz capture path;
internal upsampling cannot recover detail absent from the input.

Processed audio, Guard Off, and bypass share the declared delay. DSP buffers are
fixed and audio callbacks allocate nothing. Main RMS leveling remains causal.

If guard reduction is frequent or large, lower Target or Output trim and
compare the result at matched playback loudness. Increase Maximum boost only
as far as soft speech needs. A gentle upstream compressor can reduce excessive
crest factor; put a final limiter after any later stage that adds gain.
Lookahead can improve transient handling, but cannot make an impossible RMS
target fit below the peak ceiling.

Changing Lookahead or peak-detection mode flushes buffered audio and can cause
a brief dropout: adjust these controls while paused. The plugin reports its sample delay and notifies the
host when its total latency changes. Hosts that do not honor dynamic
latency updates may require reopening the plugin or restarting their audio
pipeline. Host compensation does not remove live microphone delay.

## Editor and saved settings

The native editor remains 900 × 720 pixels with the #6667ab motif, **Main** and
**RMS / Quality** tabs, and 20 Hz meters. Drag or scroll a slider; Shift
provides finer adjustment. Click a value to type it; Enter commits and Escape
cancels. Ctrl-click restores that control's Broadcast default. Tab moves
between controls; arrows adjust, and Home/End select endpoints.

Factory presets are Broadcast, Natural, Wide range, Singing, Smooth boost, and
Quality voice. Selecting one sets all 23 controls. The first four retain their earlier
settings, with Adaptive RMS Off and Lookahead 0 ms. Both channels share gain,
detected from the louder channel. A missing or silent right channel does not
introduce a 3 dB average error; channel routing remains the host's responsibility.

New state saves use version-5 chunks (108 bytes). The loader accepts real
version-4 chunks (100 bytes, 21 fields), restoring Adaptive guard Off and Sample
peak detection. Actual 68-byte version-1–3 chunks retain their physical dB and
millisecond settings with historical migrations and appended controls at defaults.
Older plugin versions cannot load version-5 chunks.

The original thirteen parameter indices and 1.3.0 mappings remain unchanged;
ten controls are appended to the original thirteen. Hosts restoring only the earlier raw parameters leave appended defaults in place. Raw normalized settings or
automation from 1.0.0/1.1.0 cannot identify their old ranges: check Target,
Boost, Ceiling, Cut, Lift, and Reduce when upgrading from those versions.

## Included files and validation

The Windows package contains the DLL, full C++17 source and tests, this guide,
the quick start, validation records, licenses, and an optional standalone
Windows test host. That host uses synthetic audio and does not open the mic.
See `docs/VALIDATION.md` for measured results and their limits, and
`source/docs/BUILD.md` in the release package for rebuilding instructions.

The adaptive tests check exact latency and bypass, quiet/loud overloads,
retained quiet gain, detector-band boundaries, float/double processing,
sample-rate and block-size independence, stereo linkage, live control changes,
pause recovery, and invalid input. These tests verify behavior and numerical
limits; listening to representative before/after recordings remains necessary
to assess the reported muffling.
