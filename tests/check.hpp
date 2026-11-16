/*
 * The Hombre Operating System.
 * Copyright © 2026 Adam Waldenberg and Adeptum AB.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Website: https://www.adeptum.se
 * Contact: info@adeptum.se
 */

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
