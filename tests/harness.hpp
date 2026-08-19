#pragma once

// A ~90-line test harness. There is no GTest here, and that is deliberate:
// `make test` must work on a clean machine with nothing but g++ and make. A
// reviewer should never have to install a package to check that the claims in
// the README are true.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace pt_test {

struct TestCase {
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}
inline int& failures_in_current() {
  static int f = 0;
  return f;
}

struct Registrar {
  Registrar(const char* n, void (*f)()) { registry().push_back({n, f}); }
};

inline void report_failure(const char* file, int line, const std::string& msg) {
  std::fprintf(stderr, "    FAIL %s:%d\n      %s\n", file, line, msg.c_str());
  ++failures_in_current();
}

inline std::string to_debug(long long v) { return std::to_string(v); }
inline std::string to_debug(unsigned long long v) { return std::to_string(v); }
inline std::string to_debug(int v) { return std::to_string(v); }
inline std::string to_debug(unsigned v) { return std::to_string(v); }
inline std::string to_debug(std::size_t v) { return std::to_string(v); }
inline std::string to_debug(bool v) { return v ? "true" : "false"; }
inline std::string to_debug(const std::string& v) { return "\"" + v + "\""; }
inline std::string to_debug(const char* v) { return std::string("\"") + v + "\""; }

inline int run_all() {
  int failed_tests = 0;
  std::printf("running %zu tests\n\n", registry().size());
  for (const auto& tc : registry()) {
    failures_in_current() = 0;
    std::printf("  %-52s", tc.name);
    std::fflush(stdout);
    tc.fn();
    if (failures_in_current() == 0) {
      std::printf(" ok\n");
    } else {
      std::printf(" FAILED (%d)\n", failures_in_current());
      ++failed_tests;
    }
  }
  std::printf("\n%s: %d/%zu passed\n",
              failed_tests == 0 ? "PASS" : "FAIL",
              static_cast<int>(registry().size()) - failed_tests,
              registry().size());
  return failed_tests == 0 ? 0 : 1;
}

}  // namespace pt_test

#define TEST(name)                                                    \
  static void name();                                                 \
  static ::pt_test::Registrar pt_reg_##name(#name, &name);            \
  static void name()

#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) ::pt_test::report_failure(__FILE__, __LINE__,        \
                                           "CHECK(" #cond ") is false"); \
  } while (0)

#define CHECK_EQ(a, b)                                                \
  do {                                                                \
    const auto pt_a_ = (a);                                           \
    const auto pt_b_ = (b);                                           \
    if (!(pt_a_ == pt_b_))                                            \
      ::pt_test::report_failure(                                      \
          __FILE__, __LINE__,                                         \
          std::string(#a " == " #b) + "\n        lhs = " +            \
              ::pt_test::to_debug(static_cast<long long>(pt_a_)) +    \
              "\n        rhs = " +                                    \
              ::pt_test::to_debug(static_cast<long long>(pt_b_)));    \
  } while (0)
