// Tiny test harness (no framework dependency): CHECK records failures, main returns non-zero.
#pragma once
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

inline int& failures() {
  static int n = 0;
  return n;
}

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures();                                                          \
    }                                                                        \
  } while (0)

#define CHECK_NEAR(a, b, tol) CHECK(std::fabs((a) - (b)) <= (tol))

// Poll until pred() is true or the timeout passes.
template <typename F>
bool wait_for(F pred, int timeout_ms) {
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}
