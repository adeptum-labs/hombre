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

#include "libs/exec.hpp"
#include "libs/keymap_library.hpp"
#include "check.hpp"

#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace hombre;
using namespace hombre::m68k;
using namespace hombre::ndk;
using namespace hombre::keymap_ndk;

namespace {

constexpr u32 kCall = 0x1800;
constexpr u32 kStack = 0x8000;
constexpr u32 kData = 0x9000;  // test structures up to 0xFFFF
constexpr u32 kEvent = kData;
constexpr u32 kText = kData + 0x100;
constexpr u32 kOther = kData + 0x400;
constexpr u32 kOut = 0x9800;
constexpr u32 kFreeStart = 0x10000;
constexpr u32 kFreeEnd = 0x400000;

// Static storage, as the kernel keeps them.
Jit g_jit;
Exec g_exec;
KeymapLibrary g_keymap;

// 16 MiB of guest memory, the Jit, exec and keymap.library over them.
class Rig {
public:
    Rig() : memory_(kGuestBytes) {
        fd_ = memfd_create("keymap", 0);
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
        m.cpu.a[7] = kStack;
    }

    ~Rig() {
        munmap(rw_, kRegion);
        munmap(const_cast<u8*>(rx_), kRegion);
        close(fd_);
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    Machine m;

    u32 put8(u32 at, u32 v) {
        memory_[at] = static_cast<u8>(v);
        return at;
    }

    u32 put16(u32 at, u32 v) {
        put8(at, v >> 8);
        put8(at + 1, v);
        return at;
    }

    u32 put32(u32 at, u32 v) {
        put16(at, v >> 16);
        put16(at + 2, v);
        return at;
    }

    u32 get8(u32 at) const { return memory_[at]; }
    u32 get16(u32 at) const { return (get8(at) << 8) | get8(at + 1); }
    u32 get32(u32 at) const { return (get16(at) << 16) | get16(at + 2); }

    u32 str(u32 at, const std::string& s) {
        for (usize i = 0; i < s.size(); ++i) {
            put8(at + static_cast<u32>(i), static_cast<u8>(s[i]));
        }
        put8(at + static_cast<u32>(s.size()), 0);
        return at;
    }

    std::string read_str(u32 at) const {
        std::string s;
        while (memory_[at] != 0) {
            s += static_cast<char>(memory_[at]);
            ++at;
        }
        return s;
    }

    // JSR -lvo(A6) with A6 = base, from hand-written 68000 code.
    ExitReason call(u32 base, u32 lvo) {
        put16(kCall, 0x4EAE);
        put16(kCall + 2, static_cast<u32>(0x10000 - lvo));
        put16(kCall + 4, 0x4EF8);  // JMP $400.w
        put16(kCall + 6, 0x0400);
        g_jit.flush();
        m.cpu.a[6] = base;
        m.cpu.a[7] = kStack;
        m.cpu.pc = kCall;
        return g_jit.run(1000000);
    }

    // Calls the vector, which must return; D0 is what it gave.
    u32 result(u32 base, u32 lvo) {
        CHECK_EQ(call(base, lvo), ExitReason::Halt);
        return m.cpu.d[0];
    }

    // MapRawKey of the raw key with the qualifier, into kOut. The previous key
    // down is the one the InputEvent carries, none by default.
    u32 map_raw(u32 code, u32 qualifier, u32 length = 8, u32 prev_code = 0xFF,
                u32 prev_qualifier = 0) {
        put16(kEvent + ie_Code, code);
        put16(kEvent + ie_Qualifier, qualifier);
        put8(kEvent + ie_Prev1DownCode, prev_code);
        put8(kEvent + ie_Prev1DownQual, prev_qualifier);
        m.cpu.a[0] = kEvent;
        m.cpu.a[1] = kOut;
        m.cpu.d[1] = length;
        m.cpu.a[2] = 0;
        return result(g_keymap.base(), keymap_lvo::MapRawKey);
    }

    // MapANSI of text into kOut.
    u32 map_ansi(const std::string& text, u32 length = 16) {
        for (usize i = 0; i < text.size(); ++i) {
            put8(kText + static_cast<u32>(i), static_cast<u8>(text[i]));
        }
        m.cpu.a[0] = kText;
        m.cpu.d[0] = static_cast<u32>(text.size());
        m.cpu.a[1] = kOut;
        m.cpu.d[1] = length;
        m.cpu.a[2] = 0;
        return result(g_keymap.base(), keymap_lvo::MapANSI);
    }

private:
    static constexpr usize kRegion = 1 << 20;

    std::vector<u8> memory_;
    int fd_ = -1;
    u8* rw_ = nullptr;
    const u8* rx_ = nullptr;
};

void resource_holds_the_usa_keymap() {
    Rig r;
    r.str(kData, "keymap.resource");
    r.m.cpu.a[1] = kData;
    const u32 resource = r.result(g_exec.exec_base(), exec_lvo::OpenResource);
    CHECK(resource != 0);
    CHECK_EQ(r.get8(resource + LN_TYPE), NT_RESOURCE);
    CHECK(r.read_str(r.get32(resource + LN_NAME)) == "keymap.resource");

    const u32 list = resource + kr_List;
    const u32 node = r.get32(list + LH_HEAD);
    CHECK(node != 0);
    CHECK_EQ(r.get8(node + LN_TYPE), NT_KEYMAP);
    CHECK(r.read_str(r.get32(node + LN_NAME)) == "usa");
    // "usa1", as Input prefs names the built-in keymap, has the same tables.
    const u32 usa1 = r.get32(node + LN_SUCC);
    CHECK_EQ(r.get32(usa1 + LN_SUCC), list + LH_TAIL);
    CHECK_EQ(r.get8(usa1 + LN_TYPE), NT_KEYMAP);
    CHECK(r.read_str(r.get32(usa1 + LN_NAME)) == "usa1");
    for (u32 i = 0; i < 32; i += 4) {
        CHECK_EQ(r.get32(usa1 + 14 + i), r.get32(node + 14 + i));
    }
    CHECK_EQ(r.get8(list + LH_TYPE), NT_KEYMAP);

    const u32 keymap = node + kn_KeyMap;
    CHECK(r.get32(keymap + km_LoKeyMapTypes) != 0);
    CHECK(r.get32(keymap + km_LoKeyMap) != 0);
    CHECK(r.get32(keymap + km_HiKeyMapTypes) != 0);
    CHECK(r.get32(keymap + km_HiKeyMap) != 0);
    CHECK_EQ(g_keymap.keymap(), keymap);
}

void keymap_library_opens_and_answers_its_default() {
    Rig r;
    r.str(kData, "keymap.library");
    r.m.cpu.a[1] = kData;
    r.m.cpu.d[0] = 0;
    CHECK_EQ(r.result(g_exec.exec_base(), exec_lvo::OpenLibrary), g_keymap.base());
    CHECK_EQ(r.result(g_keymap.base(), keymap_lvo::AskKeyMapDefault), g_keymap.keymap());
}

void raw_keys_type_their_characters() {
    Rig r;
    CHECK_EQ(r.map_raw(0x20, 0), 1u);
    CHECK_EQ(r.get8(kOut), u32{'a'});
    CHECK_EQ(r.map_raw(0x20, IEQUALIFIER_LSHIFT), 1u);
    CHECK_EQ(r.get8(kOut), u32{'A'});
    CHECK_EQ(r.map_raw(0x01, IEQUALIFIER_LSHIFT), 1u);
    CHECK_EQ(r.get8(kOut), u32{'!'});
    CHECK_EQ(r.map_raw(0x44, 0), 1u);
    CHECK_EQ(r.get8(kOut), 0x0Du);
    CHECK_EQ(r.map_raw(0x4C, 0), 2u);
    CHECK_EQ(r.get8(kOut), 0x9Bu);
    CHECK_EQ(r.get8(kOut + 1), u32{'A'});
    CHECK_EQ(r.map_raw(0xA0, 0), 0u);
    CHECK_EQ(r.map_raw(0x20, IEQUALIFIER_CAPSLOCK), 1u);
    CHECK_EQ(r.get8(kOut), u32{'A'});
    CHECK_EQ(r.map_raw(0x20, IEQUALIFIER_CAPSLOCK | IEQUALIFIER_LSHIFT), 1u);
    CHECK_EQ(r.get8(kOut), u32{'a'});
    CHECK_EQ(r.map_raw(0x4C, 0, 1), 0xFFFFFFFFu);
}

void alt_characters_match_the_rom_map() {
    Rig r;
    const struct {
        u32 code;
        u32 qualifier;
        u32 expected;
    } cases[] = {
        {0x01, IEQUALIFIER_LALT, 0xB9},
        {0x20, IEQUALIFIER_LALT, 0xE6},
        {0x20, IEQUALIFIER_LALT | IEQUALIFIER_LSHIFT, 0xC6},
        {0x21, IEQUALIFIER_LALT, 0xDF},
        {0x12, IEQUALIFIER_CONTROL | IEQUALIFIER_LALT, 0x85},
        {0x12, IEQUALIFIER_RALT, 0xA9},
        {0x20, IEQUALIFIER_CONTROL, 0x01},
        {0x44, IEQUALIFIER_CONTROL, 0x0A},
        {0x45, 0, 0x1B},
        {0x45, IEQUALIFIER_LALT, 0x9B},
        {0x40, IEQUALIFIER_LALT, 0xA0},
    };
    for (const auto& c : cases) {
        CHECK_EQ(r.map_raw(c.code, c.qualifier), 1u);
        CHECK_EQ(r.get8(kOut), c.expected);
    }
}

void dead_keys_make_nothing_and_accent_the_next_key() {
    Rig r;
    const u32 acute = 0x23;
    const u32 circumflex = 0x25;
    const u32 diaeresis = 0x27;
    CHECK_EQ(r.map_raw(acute, IEQUALIFIER_LALT), 0u);
    CHECK_EQ(r.map_raw(0x12, 0, 8, acute, IEQUALIFIER_LALT), 1u);
    CHECK_EQ(r.get8(kOut), 0xE9u);
    CHECK_EQ(r.map_raw(0x12, IEQUALIFIER_LSHIFT, 8, acute, IEQUALIFIER_LALT), 1u);
    CHECK_EQ(r.get8(kOut), 0xC9u);
    CHECK_EQ(r.map_raw(0x12, IEQUALIFIER_CAPSLOCK, 8, 0x24, IEQUALIFIER_LALT), 1u);
    CHECK_EQ(r.get8(kOut), 0xC8u);  // grave, capital by Caps Lock
    CHECK_EQ(r.map_raw(0x16, 0, 8, diaeresis, IEQUALIFIER_LALT), 1u);
    CHECK_EQ(r.get8(kOut), 0xFCu);
    CHECK_EQ(r.map_raw(0x40, 0, 8, circumflex, IEQUALIFIER_LALT), 1u);
    CHECK_EQ(r.get8(kOut), u32{'^'});
    CHECK_EQ(r.map_raw(0x36, 0, 8, 0x26, IEQUALIFIER_LALT), 1u);
    CHECK_EQ(r.get8(kOut), 0xF1u);  // tilde
    CHECK_EQ(r.map_raw(0x12, 0, 1, acute, IEQUALIFIER_LALT), 1u);
    CHECK_EQ(r.map_raw(0x12, 0, 0, acute, IEQUALIFIER_LALT), 0xFFFFFFFFu);
}

void keys_without_a_dead_key_before_them_type_themselves() {
    Rig r;
    CHECK_EQ(r.map_raw(0x12, 0), 1u);
    CHECK_EQ(r.get8(kOut), u32{'e'});
    CHECK_EQ(r.map_raw(0x12, 0, 8, 0x20, 0), 1u);  // after a plain a
    CHECK_EQ(r.get8(kOut), u32{'e'});
    CHECK_EQ(r.map_raw(0x12, 0, 8, 0x23, 0), 1u);  // f without Alt is no dead key
    CHECK_EQ(r.get8(kOut), u32{'e'});
    CHECK_EQ(r.map_raw(0x40, 0), 1u);
    CHECK_EQ(r.get8(kOut), u32{' '});
    CHECK_EQ(r.map_raw(0x23, 0), 1u);
    CHECK_EQ(r.get8(kOut), u32{'f'});
}

void usa_capsable_and_repeatable_bits_match_the_rom() {
    Rig r;
    const u8 lo_caps[] = {0x00, 0x00, 0xFF, 0x03, 0xFF, 0x01, 0xFE, 0x00};
    const u8 lo_repeat[] = {0xFF, 0xBF, 0xFF, 0xEF, 0xFF, 0xEF, 0xFF, 0xF7};
    const u8 hi_repeat[] = {0x47, 0xFF, 0xFF, 0x7F, 0x00, 0x80, 0x00};
    const u32 keymap = g_keymap.keymap();
    for (u32 i = 0; i < sizeof lo_caps; ++i) {
        CHECK_EQ(r.get8(r.get32(keymap + km_LoCapsable) + i), lo_caps[i]);
        CHECK_EQ(r.get8(r.get32(keymap + km_LoRepeatable) + i), lo_repeat[i]);
    }
    for (u32 i = 0; i < sizeof hi_repeat; ++i) {
        CHECK_EQ(r.get8(r.get32(keymap + km_HiCapsable) + i), 0u);
        CHECK_EQ(r.get8(r.get32(keymap + km_HiRepeatable) + i), hi_repeat[i]);
    }
}

void vanilla_mapping_copies_the_characters_out() {
    Rig r;
    u8 out[4] = {};
    CHECK_EQ(g_keymap.map_vanilla(0x20, 0, out, 1), 1u);
    CHECK_EQ(out[0], 'a');
    CHECK_EQ(g_keymap.map_vanilla(0x20, IEQUALIFIER_LSHIFT, out, 1), 1u);
    CHECK_EQ(out[0], 'A');
    CHECK_EQ(g_keymap.map_vanilla(0x4C, 0, out, 4), 2u);
    CHECK_EQ(out[0], 0x9B);
    CHECK_EQ(out[1], 'A');
    CHECK_EQ(g_keymap.map_vanilla(0x4C, 0, out, 1), 0u);     // too small
    CHECK_EQ(g_keymap.map_vanilla(0xA0, 0, out, 1), 0u);     // a release
    CHECK_EQ(g_keymap.map_vanilla(0x60, 0, out, 1), 0u);     // a qualifier key
    CHECK_EQ(g_keymap.map_vanilla(0x20, 0, out, 99), 0u);    // beyond the scratch buffer
}

void vanilla_mapping_remembers_the_dead_key() {
    Rig r;
    u8 out[4] = {};
    CHECK_EQ(g_keymap.map_vanilla(0x23, IEQUALIFIER_LALT, out, 1), 0u);  // acute
    CHECK_EQ(g_keymap.map_vanilla(0x12, 0, out, 1), 1u);
    CHECK_EQ(out[0], 0xE9);
    CHECK_EQ(g_keymap.map_vanilla(0x12, 0, out, 1), 1u);  // the pair is used up
    CHECK_EQ(out[0], 'e');
    CHECK_EQ(g_keymap.map_vanilla(0x27, IEQUALIFIER_LALT, out, 1), 0u);  // diaeresis
    CHECK_EQ(g_keymap.map_vanilla(0x64, 0, out, 1), 0u);  // Alt going down in between
    CHECK_EQ(g_keymap.map_vanilla(0x16, 0, out, 1), 1u);
    CHECK_EQ(out[0], 0xFC);
}

void ansi_maps_back_to_raw_keys() {
    Rig r;
    CHECK_EQ(r.map_ansi("A"), 1u);
    CHECK_EQ(r.get8(kOut), 0x20u);
    CHECK_EQ(r.get8(kOut + 1), IEQUALIFIER_LSHIFT);
    CHECK_EQ(r.map_ansi("a"), 1u);
    CHECK_EQ(r.get8(kOut), 0x20u);
    CHECK_EQ(r.get8(kOut + 1), 0u);
    CHECK_EQ(r.map_ansi("e"), 1u);  // a dead-capable key types its plain letter
    CHECK_EQ(r.get8(kOut), 0x12u);
    CHECK_EQ(r.get8(kOut + 1), 0u);
    CHECK_EQ(r.map_ansi("E"), 1u);
    CHECK_EQ(r.get8(kOut), 0x12u);
    CHECK_EQ(r.get8(kOut + 1), IEQUALIFIER_LSHIFT);
}

void set_default_changes_the_asked_keymap() {
    Rig r;
    r.m.cpu.a[0] = kOther;
    r.result(g_keymap.base(), keymap_lvo::SetKeyMapDefault);
    CHECK_EQ(r.result(g_keymap.base(), keymap_lvo::AskKeyMapDefault), kOther);
    r.m.cpu.a[0] = g_keymap.keymap();
    r.result(g_keymap.base(), keymap_lvo::SetKeyMapDefault);
    CHECK_EQ(r.result(g_keymap.base(), keymap_lvo::AskKeyMapDefault), g_keymap.keymap());
}

}

int main() {
    resource_holds_the_usa_keymap();
    keymap_library_opens_and_answers_its_default();
    raw_keys_type_their_characters();
    alt_characters_match_the_rom_map();
    dead_keys_make_nothing_and_accent_the_next_key();
    keys_without_a_dead_key_before_them_type_themselves();
    usa_capsable_and_repeatable_bits_match_the_rom();
    vanilla_mapping_copies_the_characters_out();
    vanilla_mapping_remembers_the_dead_key();
    ansi_maps_back_to_raw_keys();
    set_default_changes_the_asked_keymap();
    return check::finish("test_keymap");
}
