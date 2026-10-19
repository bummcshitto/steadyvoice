#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
sv_cxx="${SV_CXX:-g++}"
mkdir -p build
for sv_suite in dsp adaptive loudness guard_quality true_peak peak_guard; do
  "$sv_cxx" -std=c++17 -O3 -Wall -Wextra -Wpedantic \
    "tests/${sv_suite}_tests.cpp" -o "build/${sv_suite}_tests"
done
"$sv_cxx" -std=c++17 -O3 -Wall -Wextra -Wpedantic -fno-math-errno \
  -shared -fPIC -fvisibility=hidden src/plugin.cpp src/editor.cpp \
  -o build/SteadyVoice.so
"$sv_cxx" -std=c++17 -O3 -Wall -Wextra -Wpedantic -rdynamic \
  tests/host_tests.cpp -o build/host_tests -ldl -pthread
for sv_suite in dsp adaptive loudness guard_quality true_peak peak_guard; do
  "build/${sv_suite}_tests"
done
build/host_tests "$PWD/build/SteadyVoice.so"
