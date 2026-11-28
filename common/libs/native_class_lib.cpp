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

#include "native_class_lib.hpp"

#include "boopsi.hpp"
#include "version.hpp"

namespace hombre {

using m68k::Machine;
using native::Bus;
using native::LogLine;

u32 NativeClassLibrary::make(Machine& m, Exec& exec, const char* name, const char* id,
                             MakeClass make_class, void* ctx) {
    base_ = 0;
    class_ = 0;
    making_ = false;
    maker_ = 0;
    exec_ = &exec;
    make_class_ = make_class;
    ctx_ = ctx;
    for (NativeVector& v : vectors_) {
        v = NativeVector{nullptr, nullptr};
    }
    vectors_[0] = NativeVector{open, "Open"};
    vectors_[kVectors - 1] = NativeVector{obtain_engine, "ObtainEngine"};
    u32 length = 0;
    while (id[length] != '\0') {
        ++length;
    }
    const u32 id_string = exec.alloc(length + 1, ndk::MEMF_PUBLIC);
    if (id_string == 0) {
        return 0;
    }
    const u32 base = exec.make_native_library(name, kHombreLibVersion, vectors_, kVectors, this);
    if (base == 0) {
        exec.free(id_string, length + 1);
        return 0;
    }
    Bus b(m);
    for (u32 i = 0; i <= length; ++i) {
        b.w8(id_string + i, static_cast<u8>(id[i]));
    }
    b.w16(base + ndk::LIB_REVISION, kHombreLibRevision);
    b.w32(base + ndk::LIB_IDSTRING, id_string);
    if (!b.ok()) {
        return 0;
    }
    base_ = base;
    return base;
}

bool NativeClassLibrary::ensure_class(Machine& m, u32 base) {
    const u32 me = exec_->this_task();
    if (making_ && maker_ == me) {
        LogLine().text("exec: a class library opened while its class is made").send();
        return false;
    }
    // Making the class may open other libraries and let other tasks run;
    // one that opens the library then waits for the class.
    const u64 start = exec_->now_us();
    while (making_ && class_ == 0) {
        if (!exec_->yield_in_native()) {
            const u64 now = exec_->now_us();
            if (now == 0 || now - start > kMakeWaitUs) {
                return false;
            }
            exec_->sleep_us(kMakePollUs);
        }
    }
    if (class_ != 0) {
        return true;
    }
    making_ = true;
    maker_ = me;
    const u32 cl = make_class_(ctx_, base);
    making_ = false;
    if (cl == 0 || native::run_ended(m)) {
        return false;
    }
    Bus b(m);
    b.w32(cl + ndk::cl_UserData, base);
    if (!b.ok()) {
        return false;
    }
    class_ = cl;
    return true;
}

// Open: the class is made the first time, and an open it fails fails.
void NativeClassLibrary::open(void* ctx, Machine& m) {
    NativeClassLibrary& lib = *static_cast<NativeClassLibrary*>(ctx);
    const u32 base = m.cpu.a[6];
    native::set_result(m, 0);
    if (!lib.ensure_class(m, base)) {
        return;
    }
    Bus b(m);
    b.w16(base + ndk::LIB_OPENCNT, b.r16(base + ndk::LIB_OPENCNT) + 1);
    b.w8(base + ndk::LIB_FLAGS, b.r8(base + ndk::LIB_FLAGS) & ~ndk::LIBF_DELEXP);
    if (b.ok()) {
        native::set_result(m, base);
    }
}

void NativeClassLibrary::obtain_engine(void* ctx, Machine& m) {
    native::set_result(m, static_cast<NativeClassLibrary*>(ctx)->class_);
}

}
