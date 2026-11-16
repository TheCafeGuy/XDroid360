#!/usr/bin/env bash
# MIRROR-1 PC viewer: builds build/xdroid-viewer.exe, static: it needs no DLL
# beyond Windows'. Needs MinGW-w64 g++ (x86_64) with C++20: g++ on PATH, or
# set CXX, e.g. CXX=/path/to/mingw64/bin/g++.exe bash build.sh.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
"${CXX:-g++}" \
  -std=c++20 -O2 -Wall -Wextra -municode -mwindows -static \
  -D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN \
  -I../xdroid-core/src/main/cpp \
  main.cc net.cc decode.cc render.cc adb.cc probe.cc selftest.cc audio.cc \
  -ld3d11 -ldxgi -ld3dcompiler_47 -ld2d1 -ldwrite -lmfplat -lmfuuid \
  -lwmcodecdspuuid -lole32 -luuid -lws2_32 -lbcrypt -lavrt -lksuser \
  -o build/xdroid-viewer.exe
