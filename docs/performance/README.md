# Performance measurement

The final-release 8× measurements are in results-15-release-smoke-mono.csv and
results-15-release-smoke-stereo.csv. The off-mode compatibility repeat is in
results-15-release-off.csv. Earlier baseline and pre-optimization matrix data
are retained separately; they do not represent final 8× performance.

The harness calls the complete native Linux Plugin::render<float> callback.
It includes parameter synchronization, denormal protection, DSP, and meter
publication, but excludes GUI, APO, driver/device, OBS, and other filters.
It pins one available CPU, warms up, and measures three calibrated repetitions
using thread CPU time. Probe columns also record callback p99/max CPU and max
wall time. Output hashing is independent of timing repetition calibration.

From the source directory on Linux:

```sh
mkdir -p build
g++ -std=c++17 -O3 -DNDEBUG -fno-math-errno -Wall -Wextra \
  -DSV_BENCH_V15 -Isrc docs/performance/bench.cpp src/plugin.cpp src/editor.cpp \
  -o build/performance_bench
build/performance_bench --smoke-focus > build/performance-mono.csv
build/performance_bench --smoke-focus --stereo-focus > build/performance-stereo.csv
```

--focus selects 48/192 kHz, 5 ms blocks, both fixtures, 5/10 ms lookahead, and all
profiles. The complete matrix includes 96 kHz and 2/10 ms blocks. --smoke-focus
selects final 8×/5 ms cases only; --off-focus checks legacy output compatibility.
To compare 1.4, compile the same harness against unchanged 1.4 sources without
SV_BENCH_V15. No fast-math, LTO, or CPU-specific flags were used.

Results refer to one core in a Linux KVM guest on an AMD EPYC 9V74. The heavy
fixture uses −8 dBFS target, 24 dB boost, 4 ms lift/reduce, +12 dB trim, −1 dBFS
ceiling, bass, and noise. Silent-right and two independently active channels are
separate routes; a real unused ADC input may contain noise. VM outliers include
a final 8.53 ms CPU callback in a 5 ms block. Average cost does not guarantee
real-time deadlines or describe Ryzen 5800X3D/Windows APO performance.
See ../VALIDATION.md for the final-source summary and test limits.
