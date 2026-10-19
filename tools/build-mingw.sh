#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
sv_cxx="${SV_CXX:-x86_64-w64-mingw32-g++}"
sv_windres="${SV_WINDRES:-x86_64-w64-mingw32-windres}"
mkdir -p build
"$sv_windres" src/SteadyVoice.rc -o build/resource.o
"$sv_cxx" -std=c++17 -O3 -Wall -Wextra -Wpedantic -fno-math-errno \
  -shared -static -static-libgcc -static-libstdc++ \
  src/plugin.cpp src/editor.cpp build/resource.o \
  -o build/SteadyVoice.dll -luser32 -lgdi32 -Wl,--no-insert-timestamp
"$sv_cxx" -std=c++17 -O3 -Wall -Wextra -Wpedantic \
  -static -static-libgcc -static-libstdc++ tests/host_tests.cpp \
  -o build/ValidatePlugin.exe -luser32 -lgdi32
