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

// The C++ runtime symbols that libstdc++'s header-only code needs. The kernel
// has no exceptions, so every place the library would throw halts instead.

#include "../common/serial.hpp"

#include <bits/functexcept.h>

namespace {

[[noreturn]] void die(const char* message) {
    hombre::serial::write("kernel: ");
    hombre::serial::write(message);
    hombre::serial::write("\n");
    for (;;) {
        asm volatile("cli; hlt");
    }
}

}

namespace std {

void __throw_bad_alloc() { die("bad alloc"); }
void __throw_bad_array_new_length() { die("bad array new length"); }
void __throw_length_error(const char* message) { die(message); }
void __throw_logic_error(const char* message) { die(message); }
void __throw_out_of_range(const char* message) { die(message); }
void __throw_out_of_range_fmt(const char* format, ...) { die(format); }
void __throw_invalid_argument(const char* message) { die(message); }
void __throw_bad_function_call() { die("bad function call"); }

}

extern "C" {

// Registrations come from global constructors, which never run, and the
// kernel never exits, so there is nothing to remember.
int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }

void* __dso_handle = nullptr;

void __cxa_pure_virtual() { die("pure virtual call"); }

}
