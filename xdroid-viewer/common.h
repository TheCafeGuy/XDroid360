// MIRROR-1 PC viewer: helpers shared by every module.
#pragma once

#include <windows.h>

#include <cstdint>

namespace xv {

// QueryPerformanceCounter ticks in microseconds.
inline int64_t QpcToUs(int64_t ticks) {
  static const int64_t frequency = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return int64_t(f.QuadPart);
  }();
  return ticks / frequency * 1000000 + ticks % frequency * 1000000 / frequency;
}

// The viewer's only clock.
inline int64_t NowUs() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return QpcToUs(t.QuadPart);
}

}  // namespace xv
