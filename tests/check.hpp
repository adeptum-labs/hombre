#pragma once

#include <cstdio>
#include <cstdlib>
#include <type_traits>

namespace check {

inline unsigned long long checks_run = 0;

template <typename T>
unsigned long long as_number(const T& value) {
    if constexpr (std::is_enum_v<T>) {
        return static_cast<unsigned long long>(static_cast<std::underlying_type_t<T>>(value));
    } else {
        return static_cast<unsigned long long>(value);
    }
}

[[noreturn]] inline void fail(const char* file, int line, const char* message) {
    std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, message);
    std::abort();
}

template <typename A, typename B>
void expect_equal(const char* file, int line, const char* a_text, const char* b_text,
                  const A& a, const B& b) {
    ++checks_run;
    if (a == b) {
        return;
    }
    std::fprintf(stderr, "%s:%d: check failed: %s == %s (%llu vs %llu)\n", file, line, a_text,
                 b_text, as_number(a), as_number(b));
    std::abort();
}

inline int finish(const char* name) {
    std::printf("%s: %llu checks passed\n", name, checks_run);
    return 0;
}

}

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++check::checks_run;                                                   \
        if (!(expr)) {                                                         \
            check::fail(__FILE__, __LINE__, #expr);                            \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b) check::expect_equal(__FILE__, __LINE__, #a, #b, (a), (b))
