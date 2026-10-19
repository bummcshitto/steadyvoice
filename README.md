# SteadyVoice

A Windows x64 VST2 microphone leveler for Equalizer APO, OBS, and compatible
hosts. SteadyVoice lifts quiet speech, reduces loud passages, and provides
optional lookahead and oversampled peak protection. Processing runs locally
on the CPU, with no account, network connection, or external runtime required.

## Download

**[Download SteadyVoice 1.5.0 for Windows x64](https://github.com/bummcshitto/steadyvoice/raw/refs/heads/main/downloads/SteadyVoice-1.5.0-Windows-x64.zip)**

The ZIP includes the DLL, source, documentation, licenses, and an optional
synthetic Windows test host. If GitHub opens a file page, select **Download raw
file**. A [SHA-256 checksum](downloads/SteadyVoice-1.5.0-Windows-x64.zip.sha256)
is included.

## Features

- RMS-based voice leveling with adjustable target, boost/cut limits, lift/reduce
  times, noise floor, pause hold, and output trim.
- Optional adaptive RMS detection with five independently adjustable detector
  durations and a target-centered median band.
- Stereo-linked peak guarding, optional 0–20 ms lookahead, and adaptive guard
  recovery to reduce repeated release/cutting on sustained low frequencies.
- Sample, 4×, and 8× peak detection. Oversampling applies to the detector;
  the audible stream remains at the host sample rate and is not FIR-filtered.
- Native 900 × 720 editor with numeric entry, keyboard controls, six presets,
  RMS/sample-peak meters, and an instance sample-rate readout.
- Compatibility with older SteadyVoice saved chunks; new quality controls are
  disabled when migrating older settings.

## Getting started

Unload the old plugin and close applications using it before replacing the DLL.
In 64-bit Equalizer APO, add a VST plugin filter and select SteadyVoice.dll.
In 64-bit OBS, install it in a supported VST2 scan folder and add a VST 2.x
microphone filter. Use one active instance in a microphone path.

Start with **Quality voice**:

| Setting | Value |
| --- | ---: |
| Target level | −16 dBFS RMS |
| Maximum boost / cut | 24 / 40 dB |
| Lift / reduce | 150 / 20 ms |
| Peak ceiling / output trim | −1 dBFS / 0 dB |
| Adaptive RMS / adaptive guard | On / On |
| Peak detection / lookahead | 8× / 5 ms |

Set Noise floor above room noise and below soft speech. Compare a recording of
quiet speech followed immediately by a loud phrase, normal speech, and a pause.
Target is an RMS average, rather than a peak target; lower it if guard reduction
is frequent. Both channels share gain, but routing remains the host's responsibility.

Read the [quick start](SteadyVoice_Quick_Start.md) and
[complete control guide](docs/USER_GUIDE.md) for installation and tuning.

## Latency and peak detection

Sample detection with zero lookahead adds no audio delay. Oversampled modes use
at least 5 ms guard lookahead plus 63 native samples of detector delay. Quality
voice therefore adds **6.3125 ms at 48 kHz** or **5.328125 ms at 192 kHz**.
Guard Off and bypass retain the declared delay. Changing lookahead or peak mode
flushes buffered audio, so adjust those settings while paused.

Fourfold detection uses less CPU; eightfold checks a finer reconstruction grid.
The modes reserve 0.9 / 0.8 dB of internal headroom. This is estimated true-peak
protection: finite interpolation, later resampling, mixing, and encoding prevent
an arbitrary-waveform or downstream dBTP guarantee. With Guard On and Bypass
fully Off, the native sample ceiling is enforced. UI peak meters show sample
peaks. Windows and the host control capture format; an APO editor/analysis
instance's rate and meters may differ from its live processing instance.

## Build and test

The project uses C++17 and a vendored, independently implemented VST2 interface
header. See [BUILD.md](docs/BUILD.md) for MSVC, MinGW-w64, CMake, and sanitizer
instructions.

On Linux, run the DSP suites and actual-plugin host checks with:

```sh
bash tools/test-linux.sh
```

For a Windows x64 build with MinGW-w64:

```sh
bash tools/build-mingw.sh
```

Outputs are build/SteadyVoice.dll and build/ValidatePlugin.exe. The Linux shared
object is a validation build; the Windows DLL is the user plugin.

Seven automated suites, sanitizers, and Windows DLL/editor checks under Wine
passed for 1.5.0. See the [validation record](docs/VALIDATION.md) for measured
audio behavior, CPU costs, and limits. Those checks do not replace testing on a
physical Windows microphone with representative speech.

## License

SteadyVoice is [MIT licensed](LICENSE). The vendored interface header uses a
separate BSD-style license; see [third-party notices](THIRD_PARTY_NOTICES.txt)
and its [license](third_party/vst2sdk/LICENSE).
