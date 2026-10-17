// A tiny test harness (no dependency, so CI needs only a compiler and CMake).
#pragma once

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "anv/schema.hpp"

namespace t {

struct Case {
  const char* name;
  void (*fn)();
};
inline std::vector<Case>& cases() {
  static std::vector<Case> c;
  return c;
}
inline int& failures() {
  static int f = 0;
  return f;
}
struct Reg {
  Reg(const char* n, void (*f)()) { cases().push_back({n, f}); }
};

inline int run_all() {
  int bad = 0;
  for (const Case& c : cases()) {
    const int before = failures();
    c.fn();
    const bool ok = failures() == before;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", c.name);
    bad += ok ? 0 : 1;
  }
  std::printf("%d of %zu failed\n", bad, cases().size());
  return bad == 0 ? 0 : 1;
}

inline const anv::Schema& schema() {
  static std::shared_ptr<anv::Schema> s = [] {
    std::string err;
    auto loaded = anv::Schema::load_file(anv::Schema::default_path(), &err);
    if (!loaded) {
      std::printf("cannot load schema: %s\n", err.c_str());
      std::exit(2);
    }
    return loaded;
  }();
  return *s;
}

}  // namespace t

#define TEST(name)                         \
  static void name();                      \
  static t::Reg reg_##name(#name, &name);  \
  static void name()

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      ++t::failures();                                                         \
      std::printf("  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);   \
    }                                                                          \
  } while (0)

#define TEST_MAIN() \
  int main() { return t::run_all(); }
