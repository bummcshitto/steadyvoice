# Build SteadyVoice 1.5.0

The project uses C++17 and the vendored VST2 interface header. No SDK download,
submodule, framework, or package-manager dependency is required. Build for
x64; compile-time checks enforce the 64-bit ABI.

## Microsoft Visual C++

Install Visual Studio Build Tools with the C++ workload and a Windows SDK.
From an **x64 Native Tools Command Prompt**, run:

```bat
tools\build-windows.bat
```

Outputs are `build-msvc\SteadyVoice.dll` and
`build-msvc\ValidatePlugin.exe`. The script uses the static runtime,
optimization, precise floating-point math, and the version resource.
This is a rebuilding recipe; see `VALIDATION.md` for the release toolchain.

## MinGW-w64 on Linux or Windows/MSYS2

Provide an x64 MinGW-w64 compiler and resource compiler on PATH, then run:

```sh
bash tools/build-mingw.sh
```

Outputs are `build/SteadyVoice.dll` and `build/ValidatePlugin.exe`.
Alternate toolchain names can be supplied explicitly:

```sh
SV_CXX=x86_64-w64-mingw32-g++-posix \
SV_WINDRES=x86_64-w64-mingw32-windres bash tools/build-mingw.sh
```

The script uses baseline x86-64/SSE2, no machine-specific CPU flags, and static
compiler runtimes. Do not add `-ffast-math`: input validation and limiting
depend on IEEE floating-point behavior.

## CMake and automated validation

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake --config Release
ctest --test-dir build-cmake -C Release --output-on-failure
```

Linux builds include the DSP suites and a VST2 `.so` without the native Windows
editor. That shared object is for automated validation; the Windows DLL is the
user build. CMake cross-compilation uses a normal MinGW toolchain file.

The direct Linux recipe is:

```sh
bash tools/test-linux.sh
```

The suites cover original DSP behavior, loud-response regressions against a
test-only 1.2.0 reference, adaptive RMS and lookahead, adaptive guard distortion/recovery, independent
oversampled detector/output reconstruction, and a host loading the
actual plugin. Host checks include original parameter compatibility, actual
68-byte version-1–3 chunk migration, version-4 migration, version-5 restoration, reported latency,
and concurrent parameter changes. The Linux host interposes C++ allocation
operators to verify that audio callbacks allocate nothing.

The adaptive suite can also be built independently:

```sh
mkdir -p build
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic \
  tests/adaptive_tests.cpp -o build/adaptive_tests
build/adaptive_tests
```

It measures retained soft-speech gain and protected quiet-to-loud peaks, and
checks classifier anchors/continuity, exact delayed bypass, sub-ceiling
transparency, float/double, in-place, stereo, sample-rate/block independence,
live parameter changes, pause recovery, and invalid input. These synthetic
checks do not replace listening tests on representative speech.

The Windows host harness exercises plugin loading and the native editor. Its
coverage includes controls on both tabs, numeric entry, keyboard behavior,
presets, reopening, and GDI cleanup. Actual OBS/Equalizer APO validation is a
separate application-level step; do not infer it from host-harness success.

## Sanitizers and optional encoding probes

Compile DSP suites with
`-fsanitize=address,undefined,float-divide-by-zero -fno-omit-frame-pointer`.
For an instrumented plugin-host run, build both the shared plugin and host
with those flags; retain `-shared -fPIC` for the plugin and `-rdynamic` for
the host. LeakSanitizer may need process-inspection permissions unavailable in
a managed environment. Fixed lookahead buffers increase `Leveler` object size;
account for stack limits when a test keeps many processors in one scope.

The test-only baseline in `tests/fixtures/leveler_1_2.hpp` is never linked into
the plugin. The loud-response test can export float32 mono 48 kHz WAVs with
−3 and 0 dBFS sample ceilings:

```sh
build/loudness_tests --export build/encoding-probes
```

FFmpeg encoding/resampling experiments are optional and not normal build or
plugin dependencies. Record exact codec settings and commands alongside
results. Sample-ceiling success does not guarantee inter-sample, resampled,
mixed, or encoded peak limits. Refer to `VALIDATION.md` for the measured build
and test results, including any pending checks.
