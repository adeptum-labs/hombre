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

// Runs the 68000 single-step tests of github.com/SingleStepTests/680x0,
// commit e0d5ece9670205cc84a0101081837deb446f86a3, files 68000/v1/*.json
// (unpacked), through the translator, one instruction per case.
//
//   test_m68k_suite <file.json>...
//
// Without files it checks itself on a few cases made up to pass and to
// fail, so make test runs it like the other tests.
//
// A case gives the registers, SR, the pc and the bytes of memory before and
// after one instruction. pc is the address of the opcode, the first word of
// prefetch; the second word sits at pc + 2 and any further words of the
// instruction are among the memory bytes. The final pc is the address of
// the next instruction. Cycle counts and bus transactions are not checked,
// and the suite assumes a 24-bit address bus, so the translator runs with
// the address mask 0x00FFFFFF and the harness masks every address it loads.
//
// The translator keeps SR as plain data and knows a single A7, with usp
// apart for MOVE USP: the harness loads A7 from ssp when S is set and from
// usp otherwise, and usp from usp. It compares the final A7 with the stack
// pointer of the final mode, and usp too when S is set.
//
// Memory is zero before every case except for the bytes the initial state
// gives. Afterwards each byte the final state lists must hold its value,
// every other byte the initial state gives must be unchanged, and every
// other byte near them, near A7 and after the pc must still be zero. A
// store further away shows as memory that is not zero after the file; the
// file then runs again with all of memory checked after every case.
//
// A case gates when the instruction runs to its end on the real CPU. It
// does not gate when:
//  - the expected state shows an exception: the final SR has S set, ssp
//    dropped (a frame was pushed) and the final pc is the long the final
//    memory holds in one of the vector slots 2-63 (counted by vector);
//  - the opcode is RTE, STOP or RESET ("privileged"): with S set the
//    translator ends the run at them, as they have no meaning while
//    exceptions are not delivered to the guest; this holds also where the
//    expected state shows an exception;
//  - S differs before and after ("stack switch"): A7 is not banked;
//  - the opcode is an A-line or F-line opcode ("line a/f").
// The translator still runs those cases; they are only counted, except
// that an exception other than trace must stop the translated code by the
// end of the next block (a jump to an odd address faults at the fetch
// there), with the matching exit for a bus or address error: a case where
// it did not fails, Unimplemented included.
//
// In two kinds of data register shift the suite's expected state cannot be
// what the CPU does. These cases gate, are counted as quirks and leave out
// of the comparison only what the suite gets wrong:
//  - a byte or word shift or rotate whose expected state changes register
//    bits above the operand (two ASL.b cases, whose whole result and flags
//    are wrong): the destination register and the CCR;
//  - ASR by a register count larger than the operand, of a negative
//    operand: the suite clears C and X, where the sign bit, the last bit
//    shifted out, sets them (a count equal to the size gives the sign bit
//    in the suite as well): C and X.
//
// Every 68000 instruction is translated, so a gating case that ends with
// Unimplemented is a failure like any gating case that ends in a different
// state or leaves a stray store; any failure makes the exit status
// non-zero. RTE, STOP and RESET, which end the run with Unimplemented by
// design, do not gate.

#include "m68k_jit.hpp"
#include "test_guest.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace hombre;
using namespace hombre::m68k;

namespace {

constexpr u32 kBusMask = 0x00FFFFFF;
constexpr u32 kSupervisor = 0x2000;
constexpr u32 kCcr = 0x00FF;
constexpr u32 kCcrX = 0x10;
constexpr u32 kCcrC = 0x01;
// Around every address a case touches, the bytes checked for stray stores.
constexpr u32 kStrayReach = 16;
constexpr u32 kStackReach = 64;
constexpr usize kRegionBytes = 1 << 20;
constexpr u64 kShownFailures = 5;

struct State {
    u32 d[8]{};
    u32 a[7]{};
    u32 usp = 0;
    u32 ssp = 0;
    u32 sr = 0;
    u32 pc = 0;
    u32 prefetch[2]{};
    std::vector<std::pair<u32, u32>> ram;

    bool supervisor() const { return (sr & kSupervisor) != 0; }
    u32 stack() const { return supervisor() ? ssp : usp; }
};

struct Case {
    std::string name;
    State initial;
    State final;
};

// Just enough JSON for the suite: objects, arrays, strings without escapes
// that matter here, and non-negative integers.
class Reader {
public:
    Reader(const std::string& file, const std::string& text)
        : file_(file), begin_(text.data()), p_(begin_), end_(begin_ + text.size()) {}

    bool done() {
        space();
        return p_ == end_;
    }

    void expect(char c) {
        space();
        if (p_ == end_ || *p_ != c) {
            fail(std::string("expected '") + c + "'");
        }
        ++p_;
    }

    // Calls each() at every element of an array.
    template <typename F>
    void array(F each) {
        expect('[');
        elements(']', each);
    }

    // Calls each(key) at every member of an object, positioned at its value.
    template <typename F>
    void object(F each) {
        expect('{');
        elements('}', [&] {
            const std::string key = string();
            expect(':');
            each(key);
        });
    }

    std::string string() {
        expect('"');
        std::string out;
        while (p_ != end_ && *p_ != '"') {
            if (*p_ == '\\' && p_ + 1 != end_) {
                ++p_;
            }
            out += *p_++;
        }
        expect('"');
        return out;
    }

    u32 number() {
        space();
        u64 v = 0;
        const char* start = p_;
        while (p_ != end_ && *p_ >= '0' && *p_ <= '9' && v <= 0xFFFFFFFFu) {
            v = v * 10 + static_cast<u64>(*p_++ - '0');
        }
        if (p_ == start || v > 0xFFFFFFFFu) {
            fail("expected a 32-bit number");
        }
        return static_cast<u32>(v);
    }

    void skip() {
        space();
        if (p_ == end_) {
            fail("expected a value");
        } else if (*p_ == '"') {
            string();
        } else if (*p_ == '[') {
            array([&] { skip(); });
        } else if (*p_ == '{') {
            object([&](const std::string&) { skip(); });
        } else {
            while (p_ != end_ && *p_ != ',' && *p_ != ']' && *p_ != '}' && *p_ > ' ') {
                ++p_;
            }
        }
    }

    [[noreturn]] void fail(const std::string& why) const {
        std::fprintf(stderr, "%s: offset %zu: %s\n", file_.c_str(), static_cast<usize>(p_ - begin_),
                     why.c_str());
        std::exit(2);
    }

private:
    template <typename F>
    void elements(char close, F each) {
        space();
        if (p_ != end_ && *p_ == close) {
            ++p_;
            return;
        }
        for (;;) {
            each();
            space();
            if (p_ == end_ || *p_ != ',') {
                break;
            }
            ++p_;
        }
        expect(close);
    }

    void space() {
        while (p_ != end_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) {
            ++p_;
        }
    }

    std::string file_;
    const char* begin_;
    const char* p_;
    const char* end_;
};

State read_state(Reader& in) {
    State s;
    in.object([&](const std::string& key) {
        if (key.size() == 2 && (key[0] == 'd' || key[0] == 'a') && key[1] >= '0' && key[1] <= '7' &&
            key != "a7") {
            (key[0] == 'd' ? s.d : s.a)[key[1] - '0'] = in.number();
        } else if (key == "usp") {
            s.usp = in.number();
        } else if (key == "ssp") {
            s.ssp = in.number();
        } else if (key == "sr") {
            s.sr = in.number();
        } else if (key == "pc") {
            s.pc = in.number();
        } else if (key == "prefetch") {
            u32 n = 0;
            in.array([&] { s.prefetch[n++ & 1] = in.number(); });
        } else if (key == "ram") {
            in.array([&] {
                u32 pair[2]{};
                u32 n = 0;
                in.array([&] { pair[n++ & 1] = in.number(); });
                s.ram.emplace_back(pair[0], pair[1]);
            });
        } else {
            in.skip();
        }
    });
    return s;
}

Case read_case(Reader& in) {
    Case c;
    in.object([&](const std::string& key) {
        if (key == "name") {
            c.name = in.string();
        } else if (key == "initial") {
            c.initial = read_state(in);
        } else if (key == "final") {
            c.final = read_state(in);
        } else {
            in.skip();
        }
    });
    return c;
}

// --- which cases gate ---------------------------------------------------

const char* vector_name(u32 v) {
    switch (v) {
    case 2: return "bus error";
    case 3: return "address error";
    case 4: return "illegal";
    case 5: return "zero divide";
    case 6: return "chk";
    case 7: return "trapv";
    case 8: return "privilege";
    case 9: return "trace";
    case 10: return "line a";
    case 11: return "line f";
    default: return v >= 32 && v < 48 ? "trap" : "other vector";
    }
}

// The vector the expected state shows the instruction took, or 0.
u32 taken_vector(const Case& c) {
    if (!c.final.supervisor() || c.final.ssp >= c.initial.ssp) {
        return 0;
    }
    std::map<u32, u32> mem;
    for (const auto& [addr, value] : c.final.ram) {
        mem[addr & kBusMask] = value;
    }
    for (u32 v = 2; v < 64; ++v) {
        u32 slot = 0;
        bool whole = true;
        for (u32 i = 0; i < 4 && whole; ++i) {
            const auto it = mem.find(4 * v + i);
            whole = it != mem.end();
            slot = whole ? slot << 8 | it->second : 0;
        }
        if (whole && slot == c.final.pc) {
            return v;
        }
    }
    return 0;
}

// RTE, STOP and RESET: the privileged instructions the translator ends
// the run at, with S set as well.
bool untranslated(u32 op) {
    return op == 0x4E73 || op == 0x4E72 || op == 0x4E70;
}

// Shifts and rotates of a data register, by an immediate or by a register
// count: 1110 ccc d ss i tt rrr with ss other than 11.
bool register_shift(u32 op) {
    return (op & 0xF000) == 0xE000 && (op & 0x00C0) != 0x00C0;
}

u32 register_shift_bits(u32 op) {
    return 8u << ((op >> 6) & 3);
}

// What the comparison leaves out of a case whose expected state the suite
// gets wrong (see the file comment). A case without a quirk has no name.
struct Quirk {
    const char* name = nullptr;
    u32 skip_dreg = 8;  // none
    u32 sr_ignored = 0;
};

Quirk suite_quirk(const Case& c) {
    const u32 op = c.initial.prefetch[0];
    if (!register_shift(op)) {
        return {};
    }
    const u32 bits = register_shift_bits(op);
    const u32 size_mask = bits == 32 ? 0xFFFFFFFF : (1u << bits) - 1;
    const u32 r = op & 7;
    if (((c.initial.d[r] ^ c.final.d[r]) & ~size_mask) != 0) {
        return {"bits beyond size", r, kCcr};
    }
    const bool asr_by_register = (op & 0x0138) == 0x0020;
    const u32 count = c.initial.d[(op >> 9) & 7] & 63;
    const bool negative = (c.initial.d[r] >> (bits - 1) & 1) != 0;
    if (asr_by_register && count > bits && negative) {
        return {"asr past size", 8, kCcrX | kCcrC};
    }
    return {};
}

// Empty for a gating case, else why it does not gate.
std::string non_gating_reason(const Case& c) {
    if (const u32 v = taken_vector(c); v != 0) {
        return vector_name(v);
    }
    const u32 op = c.initial.prefetch[0];
    if (untranslated(op)) {
        return "privileged";
    }
    if (c.initial.supervisor() != c.final.supervisor()) {
        return "stack switch";
    }
    if ((op & 0xF000) == 0xA000 || (op & 0xF000) == 0xF000) {
        return "line a/f";
    }
    return {};
}

// Whether the expected state shows an exception the translated code must
// stop at. A trace exception comes after the instruction, so it is the one
// the translator does not see.
bool stops_the_code(const Case& c) {
    const u32 v = taken_vector(c);
    return v != 0 && v != 9;
}

// Whether the translator stopped as the expected exception demands: with
// the same exit for a bus or address error, with any exit for the others,
// which may still be waiting for their helpers.
bool stopped_as_expected(const Case& c, ExitReason why) {
    switch (taken_vector(c)) {
    case 2: return why == ExitReason::BusError;
    case 3: return why == ExitReason::AddressError;
    default: return why != ExitReason::Budget;
    }
}

// --- running a case -------------------------------------------------------

class Harness {
public:
    Harness() : memory_(kTestGuestBytes) {
        const int fd = memfd_create("m68k-suite", 0);
        if (fd < 0 || ftruncate(fd, kRegionBytes) != 0) {
            std::perror("memfd");
            std::exit(2);
        }
        void* rw = mmap(nullptr, kRegionBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        void* rx = mmap(nullptr, kRegionBytes, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
        if (rw == MAP_FAILED || rx == MAP_FAILED) {
            std::perror("mmap");
            std::exit(2);
        }
        m_.mem = GuestMemory{memory_.data(), kTestGuestBytes};
        if (!jit_.init(CodeRegion{static_cast<u8*>(rw), static_cast<const u8*>(rx), kRegionBytes},
                       m_)) {
            std::fprintf(stderr, "jit init failed\n");
            std::exit(2);
        }
        jit_.set_block_limit(1);
        jit_.set_address_mask(kBusMask);
        // No case runs the stubs init wrote; every case starts on zeroes.
        clear_all();
    }

    // Runs the instruction from the initial state. Returns the exit, and
    // the differences from the final state, memory included, when the exit
    // is the end of the instruction. Stray stores, bytes that the case does
    // not give and are no longer zero, are listed apart whatever the exit.
    ExitReason run(const Case& c, const Quirk& quirk, std::vector<std::string>& diffs,
                   std::vector<std::string>& strays) {
        const State& s = c.initial;
        CpuState& cpu = m_.cpu;
        cpu = CpuState{};
        for (u32 i = 0; i < 8; ++i) {
            cpu.d[i] = s.d[i];
        }
        for (u32 i = 0; i < 7; ++i) {
            cpu.a[i] = s.a[i];
        }
        cpu.a[7] = s.stack();
        cpu.usp = s.usp;
        cpu.sr = static_cast<u16>(s.sr);
        cpu.pc = s.pc;
        expected_.clear();
        for (const auto& [addr, value] : s.ram) {
            poke(addr, value);
        }
        for (u32 i = 0; i < 2; ++i) {
            poke(s.pc + 2 * i, s.prefetch[i] >> 8);
            poke(s.pc + 2 * i + 1, s.prefetch[i]);
        }
        for (const auto& [addr, value] : c.final.ram) {
            touched_.push_back(addr & kBusMask);
            expected_[addr & kBusMask] = value & 0xFF;
        }
        jit_.flush();
        ExitReason why = jit_.run(1);
        if (why == ExitReason::Budget && stops_the_code(c)) {
            // A jump or return to an odd address faults when the translator
            // fetches there, at the start of the next block.
            why = jit_.run(1);
        }
        if (why != ExitReason::Budget) {
            diffs.push_back(std::string("exit ") + exit_reason_name(why) + " fault_addr " +
                            hex(cpu.fault_addr));
        }
        if (why == ExitReason::Budget) {
            compare(c, quirk, diffs);
        }
        find_strays(c, strays);
        return why;
    }

    // Zeroes the memory the last case touched, or all of it after a case
    // that failed: a wrong store may have landed anywhere.
    void clear(bool all) {
        if (all) {
            clear_all();
        } else {
            for (u32 at : touched_) {
                memory_[at] = 0;
            }
            touched_.clear();
        }
    }

    // The first byte of guest memory that is not zero, or kTestGuestBytes. All
    // of it is zero between cases unless a store went where no case
    // expects one.
    u32 dirty_byte() const {
        static const u8 zeroes[1 << 16]{};
        for (u32 at = 0; at < kTestGuestBytes; at += sizeof zeroes) {
            if (std::memcmp(memory_.data() + at, zeroes, sizeof zeroes) != 0) {
                while (memory_[at] == 0) {
                    ++at;
                }
                return at;
            }
        }
        return kTestGuestBytes;
    }

    bool clean() const { return dirty_byte() == kTestGuestBytes; }

    static std::string hex(u32 v) {
        char text[16];
        std::snprintf(text, sizeof text, "0x%08x", v);
        return text;
    }

private:

    void clear_all() {
        std::memset(memory_.data(), 0, memory_.size());
        touched_.clear();
    }

    // Initial bytes stay as they are unless the final state lists them.
    void poke(u32 addr, u32 value) {
        memory_[addr & kBusMask] = static_cast<u8>(value);
        touched_.push_back(addr & kBusMask);
        expected_[addr & kBusMask] = value & 0xFF;
    }

    void compare(const Case& c, const Quirk& quirk, std::vector<std::string>& diffs) const {
        const CpuState& cpu = m_.cpu;
        const State& f = c.final;
        const auto check = [&](const std::string& what, u32 want, u32 got) {
            if (want != got) {
                diffs.push_back(what + " want " + hex(want) + " got " + hex(got));
            }
        };
        for (u32 i = 0; i < 8; ++i) {
            if (i != quirk.skip_dreg) {
                check("d" + std::to_string(i), f.d[i], cpu.d[i]);
            }
        }
        for (u32 i = 0; i < 7; ++i) {
            check("a" + std::to_string(i), f.a[i], cpu.a[i]);
        }
        check(f.supervisor() ? "a7/ssp" : "a7/usp", f.stack(), cpu.a[7]);
        if (f.supervisor()) {
            check("usp", f.usp, cpu.usp);
        }
        check("sr", f.sr & ~quirk.sr_ignored, cpu.sr & ~quirk.sr_ignored);
        check("pc", f.pc, cpu.pc);
        for (const auto& [addr, want] : expected_) {
            check("byte " + hex(addr), want, memory_[addr]);
        }
    }

    // Every byte the case does not give, near the addresses it touches,
    // near A7 before and after and after the pc, must still be zero. A
    // store further away is left to clean().
    void find_strays(const Case& c, std::vector<std::string>& strays) {
        spans_.clear();
        const auto near = [&](u32 addr, u32 reach) {
            addr &= kBusMask;
            spans_.emplace_back(addr < reach ? 0 : addr - reach,
                                std::min<u32>(addr + reach + 1, kTestGuestBytes));
        };
        for (const auto& [addr, want] : expected_) {
            near(addr, kStrayReach);
        }
        near(c.initial.stack(), kStackReach);
        near(m_.cpu.a[7], kStackReach);
        near(c.initial.pc + kStrayReach, kStrayReach);
        std::sort(spans_.begin(), spans_.end());
        u32 from = 0;
        for (const auto& [lo, hi] : spans_) {
            for (u32 at = std::max(lo, from); at < hi; ++at) {
                if (memory_[at] != 0 && expected_.find(at) == expected_.end()) {
                    strays.push_back("stray byte " + hex(at) + " = " + hex(memory_[at]));
                    touched_.push_back(at);
                }
            }
            from = std::max(from, hi);
        }
    }

    std::vector<u8> memory_;
    std::vector<u32> touched_;
    std::unordered_map<u32, u32> expected_;
    std::vector<std::pair<u32, u32>> spans_;
    Machine m_{};
    Jit jit_;
};

struct Tally {
    u64 cases = 0;
    u64 passed = 0;
    u64 failed = 0;
    std::map<std::string, u64> non_gating;
    std::map<std::string, u64> quirks;
    u64 non_gating_strays = 0;
    std::vector<std::string> shown;  // the first failures in detail

    static u64 sum(const std::map<std::string, u64>& counts) {
        u64 n = 0;
        for (const auto& [why, count] : counts) {
            n += count;
        }
        return n;
    }

    void add(const Tally& t) {
        cases += t.cases;
        passed += t.passed;
        failed += t.failed;
        non_gating_strays += t.non_gating_strays;
        for (const auto& [why, count] : t.non_gating) {
            non_gating[why] += count;
        }
        for (const auto& [why, count] : t.quirks) {
            quirks[why] += count;
        }
    }

    static void print_counts(const char* what, const std::map<std::string, u64>& counts) {
        std::printf(" %s %llu", what, static_cast<unsigned long long>(sum(counts)));
        const char* sep = " (";
        for (const auto& [why, count] : counts) {
            std::printf("%s%s %llu", sep, why.c_str(), static_cast<unsigned long long>(count));
            sep = ", ";
        }
        std::printf("%s", counts.empty() ? "" : ")");
    }

    void print(const std::string& name) const {
        for (const std::string& line : shown) {
            std::printf("%s\n", line.c_str());
        }
        std::printf("%-20s cases %6llu passed %6llu failed %4llu", name.c_str(),
                    static_cast<unsigned long long>(cases), static_cast<unsigned long long>(passed),
                    static_cast<unsigned long long>(failed));
        print_counts("non-gating", non_gating);
        if (!quirks.empty()) {
            print_counts("quirks", quirks);
        }
        if (non_gating_strays != 0) {
            std::printf(" non-gating strays %llu", static_cast<unsigned long long>(non_gating_strays));
        }
        std::printf("\n");
    }
};

enum class Verdict { Pass, Fail, NonGating };

struct Outcome {
    Verdict verdict;
    std::string reason;  // why a case does not gate
    const char* quirk;
    std::vector<std::string> diffs;
    bool stray;
};

// Runs one case and judges it. With full_scan, all of guest memory is
// checked after the case, to find the case behind a stray store that no
// nearby check saw.
Outcome judge(Harness& h, const Case& c, bool full_scan) {
    const Quirk quirk = suite_quirk(c);
    std::vector<std::string> diffs;
    std::vector<std::string> strays;
    const ExitReason why = h.run(c, quirk, diffs, strays);
    h.clear(false);
    if (const u32 at = full_scan ? h.dirty_byte() : kTestGuestBytes; at != kTestGuestBytes) {
        strays.push_back("stray byte " + Harness::hex(at) + ", away from the case's bytes");
    }
    const std::string reason = non_gating_reason(c);
    const bool missed = stops_the_code(c) && !stopped_as_expected(c, why) &&
                        !untranslated(c.initial.prefetch[0]);
    if (missed) {
        diffs.assign(1, reason + " expected, exit " + exit_reason_name(why));
    }
    const bool gating = reason.empty() || missed;
    if (gating) {
        diffs.insert(diffs.end(), strays.begin(), strays.end());
    }
    if (!strays.empty() || (gating && !diffs.empty())) {
        h.clear(true);
    }
    Outcome out{Verdict::Pass, reason, quirk.name, std::move(diffs), !strays.empty()};
    if (!gating) {
        out.verdict = Verdict::NonGating;
    } else if (!out.diffs.empty()) {
        out.verdict = Verdict::Fail;
    }
    return out;
}

// The Harness holds a 16 MiB guest memory and a whole Jit, so it lives in
// static storage, as the kernel keeps its Jit.
Harness* g_harness = nullptr;

void count(const Case& c, const Outcome& out, const std::string& file, Tally& t) {
    ++t.cases;
    switch (out.verdict) {
    case Verdict::NonGating:
        ++t.non_gating[out.reason];
        t.non_gating_strays += out.stray ? 1 : 0;
        return;
    case Verdict::Pass:
        ++t.passed;
        break;
    case Verdict::Fail:
        if (t.failed++ < kShownFailures) {
            t.shown.push_back("  FAIL " + file + ": " + c.name);
            for (const std::string& d : out.diffs) {
                t.shown.push_back("       " + d);
            }
        }
        break;
    }
    if (out.quirk != nullptr) {
        ++t.quirks[out.quirk];
    }
}

Tally run_cases(const std::string& path, const std::string& text, bool full_scan) {
    const std::string name = path.substr(path.find_last_of('/') + 1);
    Reader in(path, text);
    Tally t;
    in.array([&] {
        const Case c = read_case(in);
        count(c, judge(*g_harness, c, full_scan), name, t);
    });
    if (!in.done()) {
        in.fail("trailing data");
    }
    return t;
}

// A store far from every byte its case touches shows only as memory that
// is not zero after the file; the file then runs again, checking all of
// memory after every case, to name the case.
Tally run_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "%s: cannot open\n", path.c_str());
        std::exit(2);
    }
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    Tally t = run_cases(path, text, false);
    if (!g_harness->clean()) {
        std::printf("  stray store in %s, running it again to find the case\n", path.c_str());
        g_harness->clear(true);
        t = run_cases(path, text, true);
    }
    t.print(path.substr(path.find_last_of('/') + 1));
    return t;
}

// --- checks of the harness itself ---------------------------------------

// MOVE.L D0,(A0) at 0x1000 with A0 = 0x2000: the expected state is right;
// the checks below break it on purpose.
Case move_case() {
    Case c;
    c.name = "MOVE.L D0,(A0)";
    c.initial.d[0] = 0x11223344;
    c.initial.a[0] = 0x2000;
    c.initial.ssp = 0x800;
    c.initial.sr = 0x2700;
    c.initial.pc = 0x1000;
    c.initial.prefetch[0] = 0x2080;
    c.initial.prefetch[1] = 0x4E71;
    c.final = c.initial;
    c.final.pc = 0x1002;
    c.final.ram = {{0x2000, 0x11}, {0x2001, 0x22}, {0x2002, 0x33}, {0x2003, 0x44}};
    return c;
}

// The suite's case "e067 [ASR.w D0, D7] 5": C and X cleared by a count of
// 38 on a negative word.
Case asr_case() {
    Case c;
    c.name = "ASR.W D0,D7";
    c.initial.d[0] = 0x9D5417A6;
    c.initial.d[7] = 0xF67BAFC3;
    c.initial.ssp = 0x800;
    c.initial.sr = 0x2718;
    c.initial.pc = 0xC00;
    c.initial.prefetch[0] = 0xE067;
    c.initial.prefetch[1] = 0xB9CD;
    c.final = c.initial;
    c.final.d[7] = 0xF67BFFFF;
    c.final.sr = 0x2708;
    c.final.pc = 0xC02;
    return c;
}

// The suite's case "e502 [ASL.b Q, D2] 1761": all of D2 replaced.
Case asl_case() {
    Case c;
    c.name = "ASL.B #2,D2";
    c.initial.d[2] = 0x417C7E7D;
    c.initial.ssp = 0x800;
    c.initial.sr = 0x271D;
    c.initial.pc = 0xC00;
    c.initial.prefetch[0] = 0xE502;
    c.initial.prefetch[1] = 0xD958;
    c.final = c.initial;
    c.final.d[2] = 0x6461D390;
    c.final.sr = 0x271B;
    c.final.pc = 0xC02;
    return c;
}

int self_test(Harness& h) {
    int failures = 0;
    const auto expect = [&](const char* what, bool ok) {
        if (!ok) {
            std::printf("test_m68k_suite: self-test failed: %s\n", what);
            ++failures;
        }
    };
    const auto verdict = [&](const Case& c) { return judge(h, c, false); };

    expect("a right case passes", verdict(move_case()).verdict == Verdict::Pass);

    Case shifted = move_case();
    for (auto& [addr, value] : shifted.final.ram) {
        addr += 8;
    }
    const Outcome near = verdict(shifted);
    expect("a store next to the expected bytes fails", near.verdict == Verdict::Fail && near.stray);

    Case far = move_case();
    far.initial.a[0] = far.final.a[0] = 0x9000;
    far.final.ram.clear();
    expect("a far store passes the nearby checks", verdict(far).verdict == Verdict::Pass);
    expect("a far store leaves memory dirty", !h.clean());
    h.clear(true);
    const Outcome scanned = judge(h, far, true);
    expect("a far store fails the full scan", scanned.verdict == Verdict::Fail && scanned.stray);
    expect("memory is clean after a failure", h.clean());

    Case lost = move_case();
    lost.initial.ram = {{0x2008, 0x5A}};
    const Outcome kept = verdict(lost);
    expect("an initial byte the final state does not list counts as unchanged",
           kept.verdict == Verdict::Pass);

    const Outcome asr = verdict(asr_case());
    expect("the ASR quirk passes", asr.verdict == Verdict::Pass && asr.quirk != nullptr);
    Case asr_wrong = asr_case();
    asr_wrong.final.d[7] ^= 1;
    expect("the ASR quirk still compares the result", verdict(asr_wrong).verdict == Verdict::Fail);
    asr_wrong = asr_case();
    asr_wrong.final.sr ^= 0x08;
    expect("the ASR quirk still compares N", verdict(asr_wrong).verdict == Verdict::Fail);

    const Outcome asl = verdict(asl_case());
    expect("the ASL quirk passes", asl.verdict == Verdict::Pass && asl.quirk != nullptr);
    Case asl_wrong = asl_case();
    asl_wrong.final.d[3] = 1;
    expect("the ASL quirk still compares the other registers",
           verdict(asl_wrong).verdict == Verdict::Fail);
    asl_wrong = asl_case();
    asl_wrong.final.sr ^= 0x0100;
    expect("the ASL quirk still compares the system byte",
           verdict(asl_wrong).verdict == Verdict::Fail);
    // MOVE A0,USP gates, with usp compared.
    Case usp;
    usp.name = "MOVE A0,USP";
    usp.initial.a[0] = 0x1234;
    usp.initial.usp = 0x800;
    usp.initial.ssp = 0x900;
    usp.initial.sr = 0x2700;
    usp.initial.pc = 0xC00;
    usp.initial.prefetch[0] = 0x4E60;
    usp.initial.prefetch[1] = 0x4E71;
    usp.final = usp.initial;
    usp.final.usp = 0x1234;
    usp.final.pc = 0xC02;
    expect("MOVE USP gates and passes", verdict(usp).verdict == Verdict::Pass);
    Case usp_wrong = usp;
    usp_wrong.final.usp = 0x800;
    expect("usp is compared", verdict(usp_wrong).verdict == Verdict::Fail);

    // ANDI #$DFFF,SR leaves supervisor mode: A7 would switch stacks.
    Case leave = usp;
    leave.name = "ANDI #$DFFF,SR";
    leave.initial.prefetch[0] = 0x027C;
    leave.initial.prefetch[1] = 0xDFFF;
    leave.final = leave.initial;
    leave.final.sr = 0x0700;
    leave.final.pc = 0xC04;
    const Outcome left = verdict(leave);
    expect("a switch of stacks does not gate",
           left.verdict == Verdict::NonGating && left.reason == "stack switch");

    // RTE ends the run; where an address error is expected that is not a
    // failure either.
    Case rte = usp;
    rte.name = "RTE";
    rte.initial.prefetch[0] = 0x4E73;
    rte.final = rte.initial;
    rte.final.ssp = 0x8FA;
    rte.final.pc = 0x1000;
    rte.final.ram = {{0x0C, 0x00}, {0x0D, 0x00}, {0x0E, 0x10}, {0x0F, 0x00}};
    const Outcome rte_out = verdict(rte);
    expect("RTE with an address error does not gate", rte_out.verdict == Verdict::NonGating);
    h.clear(true);

    // A case that expects a bus error, run on an opcode the translator
    // stops at with Unimplemented, gates and fails.
    Case stopped = rte;
    stopped.name = "A-line expecting a bus error";
    stopped.initial.prefetch[0] = 0xA123;
    stopped.final = stopped.initial;
    stopped.final.ssp = 0x8F2;
    stopped.final.pc = 0x1000;
    stopped.final.ram = {{0x08, 0x00}, {0x09, 0x00}, {0x0A, 0x10}, {0x0B, 0x00}};
    const Outcome stopped_out = verdict(stopped);
    expect("an Unimplemented stop where a bus error is expected fails",
           stopped_out.verdict == Verdict::Fail);
    h.clear(true);

    expect("memory is clean at the end", h.clean());
    return failures;
}

}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // a line per file as it ends
    static Harness harness;
    g_harness = &harness;
    if (argc < 2) {
        const int failures = self_test(harness);
        std::printf("test_m68k_suite: self-test %s; no suite files given\n",
                    failures == 0 ? "passed" : "FAILED");
        return failures == 0 ? 0 : 1;
    }
    Tally total;
    for (int i = 1; i < argc; ++i) {
        total.add(run_file(argv[i]));
    }
    total.print("total");
    return total.failed == 0 ? 0 : 1;
}
