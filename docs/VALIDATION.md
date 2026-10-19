# SteadyVoice 1.5.0 validation

Validation date: 9 October 2026 (Asia/Manila).

The delivered Windows x64 DLL was built from the included C++17 source with
GCC 14.2 MinGW-w64 (POSIX), binutils 2.44, baseline x86-64/SSE2, static compiler
runtimes, and no fast-math or CPU-specific flags. The plugin identity is preserved.
DLL SHA-256: `38b7bd91bbd911256245aba9978c348150f32e84c5dfadea44ca6667a3c35bc7`.

## Completed behavior checks

| Check | Result |
| --- | --- |
| Release CMake build / CTest | All seven registered suites passed. |
| Original DSP | 158 assertions passed. |
| Adaptive RMS / lookahead | 1,207 assertions passed. |
| Loud-response regressions | 46 assertions passed. |
| Adaptive guard quality | 271 assertions passed. |
| Independent oversampled detector | 4,559 assertions passed. |
| Oversampled guard / output reconstruction | 1,055 assertions passed. |
| Linux actual-plugin host | 222,706 assertions passed. |
| Delivered Windows DLL / native editor under Wine 10 and Xvfb | 222,859 assertions passed. |

Host checks cover real 68-byte v1–v3 and 100-byte v4 chunks, strict 108-byte v5
state, all presets, three peak modes, numeric and keyboard editing, reopening,
concurrent controls, float/double and allocation-free audio callbacks. Latency
and bypass checks span 8–384 kHz with arbitrary host blocks. Requested lookahead
below 5 ms in oversampled modes is preserved in state, but actual guard lookahead
is 5 ms; automating 0/1/4 ms does not flush audio or send redundant latency notices.
Both oversampled modes add 63 detector samples. Guard Off and bypass retain the
same declared latency. Quality voice is 303 samples / 6.3125 ms at 48 kHz and
1,023 samples / 5.328125 ms at 192 kHz.

The Windows tests loaded the final PE DLL. Wine is a compatibility environment,
not a physical Windows microphone or an OBS/Equalizer APO application test.
Wine returned zero for GetGuiResources before and after editor testing, so this
does not establish actual Windows GDI leak counts. Included screenshots capture
editor test states, rather than untouched factory preset states.

## Guard quality

The adaptive guard learns a 10–50 ms hold, retains the 80 ms release, and uses
hysteretic crests plus a 0.1 dB refresh tolerance to reject noise-driven recutting.
In isolated continuous limiting by 8.56 dB, the coherent steady-tone residual was:

| Frequency | Legacy guard residual | Adaptive guard residual |
| ---: | ---: | ---: |
| 20 Hz | 5.1770% | 0.00000349% |
| 40 Hz | 0.85765% | 0.00000345% |

The measurement includes distortion and numerical residual. Noise fixtures,
near-threshold limiting, recovery, plosives, stereo, live controls, resets, and
multiple rates also pass. These severe synthetic bass tests establish reduced
modulation, not a listening-quality guarantee for every voice. Defaults remain
Off. A separate 180-case double-sample comparison against 1.4 was bit-identical;
the performance harness also matched all 36 legacy float-output hashes, with an
additional eight-case repeat against the final release source.

## Estimated peak protection

Both 4× and 8× use a 128-tap Kaiser beta-3 detector-only FIR. The audible signal
is not resampled or filtered. Portable four-accumulator double-precision sums
reduce dependency-chain cost; entirely silent channel histories skip convolution
while retaining interpolation tails. This uses no fast-math or new audio-thread
allocation.

Independent detector tests use analytic interval peaks and separate 16× long-FIR
Blackman reconstruction. Through 0.45 Fs, worst tested analytic underread was
0.562 dB at 4× and 0.173 dB at 8×. Tested near-Nyquist Gaussian bursts underread
by 0.172 / 0.117 dB. These sweeps do not bound every waveform.

Accurate detection alone was insufficient: instantaneous gain cuts produced
new reconstruction peaks. Oversampled modes therefore use at least 5 ms guard
lookahead, plus internal reserves of **0.9 dB at 4× / 0.8 dB at 8×**. Output
validation reconstructs the final limited float samples with a separate
16×/512-tap Blackman oracle. The broad Gaussian sweep uses 120 frequency/phase/
width combinations per mode and requested lookahead. Worst tested burst peak
was 0.1994 dB below the configured ceiling at 4× and 0.1386 dB below it at 8×.
Settled sines, multitone audio, and quiet-to-loud gain changes also passed.

A live ceiling decrease retains each queued frame's original oversampled peak
estimate and applies the new ceiling at output. A regression catches the old
native-only fallback, which could leave reconstruction peaks about 3.10 dB above
the new ceiling while old forecasts remained queued. Already emitted audio cannot
be retroactively reduced; the post-change regression excludes prior loud audio
from its reconstruction window.

Native sample ceilings remain enforced with Guard On and Bypass fully Off.
Finite interpolation and changing gain envelopes preclude an arbitrary-waveform
true-peak guarantee. Later mixing, gain, sample-rate conversion, and encoding can
raise peaks; no downstream encoder guarantee is made. The UI meters explicitly
show RMS and sample peaks, not dBTP measurements.

## CPU measurements

The version-neutral harness invokes the full Plugin::render<float> callback,
including parameter sync, denormal protection, DSP, and meter publication. It
uses pre-generated fixtures, warmed processors, three calibrated repetitions,
and thread CPU time on a pinned logical CPU. Final benchmark source hashes match
the delivered source. Linux KVM guest: AMD EPYC 9V74, G++ 14.2 O3.

Final Quality voice 8× measurements, 5 ms blocks and 5 ms requested lookahead:

| Rate and routing | Typical: one core / CPU time per block | Heavy limiting: one core / CPU time per block |
| --- | ---: | ---: |
| 48 kHz, left + silent right | 1.06% / 52.9 µs | 1.11% / 55.3 µs |
| 48 kHz, both channels active | 1.88% / 94.1 µs | 2.23% / 111.5 µs |
| 192 kHz, left + silent right | 4.12% / 205.9 µs | 4.37% / 218.4 µs |
| 192 kHz, both channels active | 6.98% / 349.0 µs | 7.31% / 365.3 µs |

The heavy fixture uses Target −8, Boost 24, Lift/Reduce 4 ms, Trim +12, and
Ceiling −1 dBFS with bass and noise. An unused hardware ADC input can contain
noise, so silent-right savings must not be assumed for every stereo endpoint.
Parallel accumulation reduced the earlier typical 48 kHz mono 8× result from
about 2.79% to about 1.1% of one core in these runs.

These averages exclude APO, interface/driver, OBS, other filters, and GUI work.
They do not measure the user's Ryzen 5800X3D. VM callback outliers were substantial:
a final 192 kHz stereo heavy probe observed 8.53 ms thread CPU in a 5 ms callback,
and a prior run observed a scheduling wall-time miss. These results establish a
modest average compute budget here, not a Windows real-time deadline guarantee.
The CSV files preserve average and observed-outlier evidence.

## Reproduce and limits

From source, run `bash tools/test-linux.sh` or the CMake/CTest recipe in BUILD.md.
Build the Windows DLL with `bash tools/build-mingw.sh` or the MSVC batch recipe.
The packaged tools/ValidatePlugin.bat invokes synthetic audio and native-editor
checks without opening the microphone. Performance source, data, and instructions
are included under docs/performance/.

Memory-check results are recorded in SANITIZERS.md and the included logs.
No recording of the user's voice was supplied. Synthetic checks demonstrate
numerical behavior and reduced guard modulation, while actual speech recordings
are needed to assess perceived muffling in the user's signal chain. An RMS target
whose crest factor demands peaks above the ceiling still requires dynamics
reduction. Lookahead anticipates the guard; main RMS leveling remains causal.
