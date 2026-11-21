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

#include "libs/console_device.hpp"
#include "check.hpp"

#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace hombre;
using namespace hombre::m68k;
using namespace hombre::ndk;
using namespace hombre::keymap_ndk;
using namespace hombre::console_ndk;

namespace {

constexpr u32 kCode = 0x1000;
constexpr u32 kStack = 0x8000;
constexpr u32 kRequest = 0x9000;
constexpr u32 kOtherRequest = 0x9100;
constexpr u32 kThirdRequest = 0x9200;
constexpr u32 kWindow = 0x9300;  // only its address matters
constexpr u32 kEvent = 0x9400;
constexpr u32 kCustom = 0x9500;
constexpr u32 kBuffer = 0x9600;
constexpr u32 kOut = 0x9700;
constexpr u32 kName = 0x9800;
constexpr u32 kText = 0x9900;
constexpr u32 kFreeStart = 0x10000;
constexpr u32 kFreeEnd = 0x400000;
constexpr usize kRegion = 1 << 20;

// Static storage, as the kernel keeps them.
Jit g_jit;
Exec g_exec;
KeymapLibrary g_keymap;
ConsoleDevice g_console;

class Rig {
public:
    Rig() : memory_(kGuestBytes) {
        fd_ = memfd_create("console_device", 0);
        CHECK(fd_ >= 0);
        CHECK(ftruncate(fd_, static_cast<off_t>(kRegion)) == 0);
        void* rw = mmap(nullptr, kRegion, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        void* rx = mmap(nullptr, kRegion, PROT_READ | PROT_EXEC, MAP_SHARED, fd_, 0);
        CHECK(rw != MAP_FAILED);
        CHECK(rx != MAP_FAILED);
        rw_ = static_cast<u8*>(rw);
        rx_ = static_cast<const u8*>(rx);
        m.cpu = CpuState{};
        m.mem = GuestMemory{memory_.data(), kGuestBytes};
        m.jit = nullptr;
        CHECK(g_jit.init(CodeRegion{rw_, rx_, kRegion}, m));
        g_log = nullptr;
        CHECK(g_exec.init(m, kFreeStart, kFreeEnd));
        CHECK(g_keymap.init(m, g_exec));
        CHECK(g_console.init(m, g_exec, g_keymap));
        str(kName, "console.device");
    }

    ~Rig() {
        munmap(rw_, kRegion);
        munmap(const_cast<u8*>(rx_), kRegion);
        close(fd_);
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    Machine m;

    void put8(u32 at, u32 v) { memory_[at] = static_cast<u8>(v); }
    void put16(u32 at, u32 v) {
        put8(at, v >> 8);
        put8(at + 1, v);
    }
    void put32(u32 at, u32 v) {
        put16(at, v >> 16);
        put16(at + 2, v);
    }
    u32 get8(u32 at) const { return memory_[at]; }
    u32 get16(u32 at) const { return (get8(at) << 8) | get8(at + 1); }
    u32 get32(u32 at) const { return (get16(at) << 16) | get16(at + 2); }

    void str(u32 at, const char* s) {
        std::memcpy(&memory_[at], s, std::strlen(s) + 1);
    }

    bool same_keymap(u32 a, u32 b) const {
        return std::memcmp(&memory_[a], &memory_[b], KEYMAP_SIZE) == 0;
    }

    // A KeyMap at kCustom that no built-in one equals.
    void make_custom_keymap() {
        for (u32 i = 0; i < KEYMAP_SIZE; i += 4) {
            put32(kCustom + i, 0x01010101u * (i / 4 + 1));
        }
    }

    // OpenDevice("console.device") of unit with the window in io_Data; the
    // error it returns.
    u32 open(u32 unit, u32 window, u32 request = kRequest) {
        for (u32 i = 0; i < IOSTD_SIZE; ++i) {
            put8(request + i, 0);
        }
        put32(request + io_Data, window);
        m.cpu.a[0] = kName;
        m.cpu.d[0] = unit;
        m.cpu.a[1] = request;
        m.cpu.d[1] = 0;
        CHECK_EQ(call(g_exec.exec_base(), exec_lvo::OpenDevice), ExitReason::Returned);
        return m.cpu.d[0];
    }

    void close_device(u32 request = kRequest) {
        m.cpu.a[1] = request;
        CHECK_EQ(call(g_exec.exec_base(), exec_lvo::CloseDevice), ExitReason::Returned);
    }

    // DoIO of the command; the error it returns, as a signed number.
    i32 command(u32 request, u16 cmd, u32 data, u32 length) {
        put16(request + io_Command, cmd);
        put32(request + io_Data, data);
        put32(request + io_Length, length);
        m.cpu.a[1] = request;
        CHECK_EQ(call(g_exec.exec_base(), exec_lvo::DoIO), ExitReason::Returned);
        return static_cast<i32>(m.cpu.d[0]);
    }

    // The characters of the raw key code with qualifier into buffer, by the
    // vector lvo of the library at base.
    u32 convert(u32 base, u32 lvo, u32 code, u32 qualifier, u32 buffer) {
        put16(kEvent + ie_Code, code);
        put16(kEvent + ie_Qualifier, qualifier);
        m.cpu.a[0] = kEvent;
        m.cpu.a[1] = buffer;
        m.cpu.d[1] = 8;
        m.cpu.a[2] = 0;
        CHECK_EQ(call(base, lvo), ExitReason::Returned);
        return m.cpu.d[0];
    }

    // JSR -lvo(A6) with A6 = base, from hand-written 68000 code.
    ExitReason call(u32 base, u32 lvo) {
        put16(kCode, 0x4EAE);
        put16(kCode + 2, 0x10000 - lvo);
        put16(kCode + 4, 0x4E75);
        g_jit.flush();
        m.cpu.a[6] = base;
        m.cpu.pc = kCode;
        put32(kStack - 4, kReturnStub);
        m.cpu.a[7] = kStack - 4;
        return g_jit.run(100000);
    }

private:
    std::vector<u8> memory_;
    int fd_ = -1;
    u8* rw_ = nullptr;
    const u8* rx_ = nullptr;
};

void window_units_open_and_library_unit_needs_no_window() {
    Rig r;
    CHECK_EQ(r.open(CONU_STANDARD, kWindow), 0u);
    CHECK_EQ(r.get32(kRequest + io_Device), g_console.device_base());
    CHECK(r.get32(kRequest + io_Unit) != 0);
    CHECK_EQ(r.get16(g_console.device_base() + LIB_OPENCNT), 1u);
    CHECK_EQ(r.open(CONU_CHARMAP, kWindow, kOtherRequest), 0u);
    CHECK_EQ(r.open(CONU_SNIPMAP, kWindow, kThirdRequest), 0u);
    CHECK(r.get32(kOtherRequest + io_Unit) != r.get32(kRequest + io_Unit));
    CHECK_EQ(r.get16(g_console.device_base() + LIB_OPENCNT), 3u);

    CHECK_EQ(r.open(CONU_LIBRARY, 0), 0u);
    CHECK_EQ(r.get32(kRequest + io_Unit), 0u);
    CHECK_EQ(r.get32(kRequest + io_Device), g_console.device_base());
}

void opening_without_a_window_or_with_a_bad_unit_fails() {
    Rig r;
    CHECK_EQ(r.open(CONU_STANDARD, 0), 0xFFFFFFFFu);
    CHECK_EQ(r.get8(kRequest + io_Error), static_cast<u8>(IOERR_OPENFAIL));
    CHECK_EQ(r.get32(kRequest + io_Unit), 0u);
    CHECK_EQ(r.get32(kRequest + io_Device), 0u);
    CHECK_EQ(r.open(2, kWindow), 0xFFFFFFFFu);
    CHECK_EQ(r.open(CONU_SNIPMAP + 1, kWindow), 0xFFFFFFFFu);
    CHECK_EQ(r.get16(g_console.device_base() + LIB_OPENCNT), 0u);
}

void ask_keymap_returns_the_default_keymap() {
    Rig r;
    CHECK_EQ(r.open(CONU_STANDARD, kWindow), 0u);
    CHECK_EQ(r.command(kRequest, CD_ASKKEYMAP, kBuffer, KEYMAP_SIZE), 0);
    CHECK_EQ(r.get32(kRequest + io_Actual), KEYMAP_SIZE);
    CHECK(r.same_keymap(kBuffer, g_keymap.default_keymap()));
    CHECK(r.same_keymap(kBuffer, g_keymap.keymap()));
    CHECK_EQ(r.command(kRequest, CD_ASKKEYMAP, kBuffer, KEYMAP_SIZE - 1), IOERR_BADLENGTH);
}

void set_keymap_changes_only_that_unit() {
    Rig r;
    CHECK_EQ(r.open(CONU_STANDARD, kWindow), 0u);
    CHECK_EQ(r.open(CONU_STANDARD, kWindow, kOtherRequest), 0u);
    r.make_custom_keymap();
    CHECK_EQ(r.command(kRequest, CD_SETKEYMAP, kCustom, KEYMAP_SIZE), 0);
    CHECK_EQ(r.command(kRequest, CD_ASKKEYMAP, kBuffer, KEYMAP_SIZE), 0);
    CHECK(r.same_keymap(kBuffer, kCustom));
    CHECK_EQ(r.command(kOtherRequest, CD_ASKKEYMAP, kBuffer, KEYMAP_SIZE), 0);
    CHECK(r.same_keymap(kBuffer, g_keymap.keymap()));
    CHECK_EQ(r.command(kRequest, CD_ASKDEFAULTKEYMAP, kBuffer, KEYMAP_SIZE), 0);
    CHECK(r.same_keymap(kBuffer, g_keymap.keymap()));
    CHECK_EQ(r.command(kRequest, CD_SETKEYMAP, kCustom, KEYMAP_SIZE - 1), IOERR_BADLENGTH);
}

void default_keymap_commands_use_the_keymap_library_default() {
    Rig r;
    CHECK_EQ(r.open(CONU_STANDARD, kWindow), 0u);
    r.make_custom_keymap();
    CHECK_EQ(r.command(kRequest, CD_SETDEFAULTKEYMAP, kCustom, KEYMAP_SIZE), 0);
    CHECK_EQ(g_keymap.default_keymap(), kCustom);
    CHECK_EQ(r.command(kRequest, CD_ASKDEFAULTKEYMAP, kBuffer, KEYMAP_SIZE), 0);
    CHECK(r.same_keymap(kBuffer, kCustom));
    CHECK_EQ(r.open(CONU_STANDARD, kWindow, kOtherRequest), 0u);
    CHECK_EQ(r.command(kOtherRequest, CD_ASKKEYMAP, kBuffer, KEYMAP_SIZE), 0);
    CHECK(r.same_keymap(kBuffer, kCustom));
}

void library_unit_converts_raw_keys_like_keymap_library() {
    Rig r;
    CHECK_EQ(r.open(CONU_LIBRARY, 0), 0u);
    const u32 base = g_console.device_base();
    const u32 codes[] = {0x20, 0x01, 0x44, 0x4C, 0xA0};
    const u32 qualifiers[] = {0, IEQUALIFIER_LSHIFT, IEQUALIFIER_CAPSLOCK};
    for (const u32 code : codes) {
        for (const u32 qualifier : qualifiers) {
            const u32 expected =
                r.convert(g_keymap.base(), keymap_lvo::MapRawKey, code, qualifier, kBuffer);
            CHECK_EQ(r.convert(base, console_lvo::RawKeyConvert, code, qualifier, kOut), expected);
            CHECK_EQ(r.get16(kOut), r.get16(kBuffer));
        }
    }
    CHECK_EQ(r.convert(base, console_lvo::RawKeyConvert, 0x20, IEQUALIFIER_LSHIFT, kOut), 1u);
    CHECK_EQ(r.get8(kOut), u32{'A'});

    r.m.cpu.a[0] = kEvent;
    CHECK_EQ(r.call(base, console_lvo::CDInputHandler), ExitReason::Returned);
    CHECK_EQ(r.m.cpu.d[0], kEvent);

    CHECK_EQ(r.command(kRequest, CD_ASKKEYMAP, kBuffer, KEYMAP_SIZE), IOERR_NOCMD);
    CHECK_EQ(r.command(kRequest, CD_ASKDEFAULTKEYMAP, kBuffer, KEYMAP_SIZE), 0);
    CHECK(r.same_keymap(kBuffer, g_keymap.keymap()));
}

void write_clear_read_and_unknown_commands() {
    Rig r;
    CHECK_EQ(r.open(CONU_STANDARD, kWindow), 0u);
    r.str(kText, "hello");
    CHECK_EQ(r.command(kRequest, CMD_WRITE, kText, 5), 0);
    CHECK_EQ(r.get32(kRequest + io_Actual), 5u);
    CHECK_EQ(r.command(kRequest, CMD_WRITE, kText, 0xFFFFFFFF), 0);
    CHECK_EQ(r.get32(kRequest + io_Actual), 5u);
    CHECK_EQ(r.command(kRequest, CMD_CLEAR, 0, 0), 0);
    CHECK_EQ(r.command(kRequest, CMD_READ, kBuffer, 1), IOERR_NOCMD);
    CHECK_EQ(r.command(kRequest, 99, 0, 0), IOERR_NOCMD);
}

void close_frees_the_unit() {
    Rig r;
    const u32 free_before = g_exec.free_bytes();
    CHECK_EQ(r.open(CONU_STANDARD, kWindow), 0u);
    CHECK(g_exec.free_bytes() < free_before);
    r.close_device();
    CHECK_EQ(g_exec.free_bytes(), free_before);
    CHECK_EQ(r.get32(kRequest + io_Unit), 0u);
    CHECK_EQ(r.get32(kRequest + io_Device), 0u);
    CHECK_EQ(r.get16(g_console.device_base() + LIB_OPENCNT), 0u);

    CHECK_EQ(r.open(CONU_LIBRARY, 0), 0u);
    r.close_device();
    CHECK_EQ(g_exec.free_bytes(), free_before);
}

}

int main() {
    window_units_open_and_library_unit_needs_no_window();
    opening_without_a_window_or_with_a_bad_unit_fails();
    ask_keymap_returns_the_default_keymap();
    set_keymap_changes_only_that_unit();
    default_keymap_commands_use_the_keymap_library_default();
    library_unit_converts_raw_keys_like_keymap_library();
    write_clear_read_and_unknown_commands();
    close_frees_the_unit();
    return check::finish("test_console_device");
}
