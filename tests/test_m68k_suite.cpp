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
// Without files it skips, so make test can run it like the other tests.
//
// A case gives the registers, SR, the pc and the bytes of memory before and
// after one instruction. pc is the address of the opcode, the first word of
// prefetch; the second word sits at pc + 2 and any further words of the
// instruction are among the memory bytes. The final pc is the address of
// the next instruction. Cycle counts and bus transactions are not checked,
// and the suite assumes a 24-bit address bus, so the translator runs with
// the address mask 0x00FFFFFF and the harness masks every address it loads.
//
// The translator keeps SR as plain data and knows a single A7: the harness
// loads A7 from ssp when S is set and from usp otherwise, compares the
// final A7 with the stack pointer of the final mode and expects the other
// one unchanged. Memory is compared on the bytes the final state lists.
//
// A case gates when the instruction runs to its end on the real CPU. It
// does not gate when:
//  - the expected state shows an exception: the final SR has S set, ssp
//    dropped (a frame was pushed) and the final pc is the long the final
//    memory holds in one of the vector slots 2-63 (counted by vector);
//  - the opcode is MOVE to or from SR, MOVE USP, ANDI, ORI or EORI to SR,
//    RTE, STOP or RESET ("privileged");
//  - the opcode is an A-line or F-line opcode ("line a/f");
//  - the case is one of two kinds of shift where the suite's expected state
//    cannot be what the CPU does ("quirk"). A byte or word shift or rotate
//    of a data register whose expected state changes register bits above
//    the operand (two ASL.b cases). And ASR by a register count larger than
//    the operand of a negative operand: the suite clears C and X, where
//    the sign bit, the last bit shifted out, sets them; a count equal to
//    the size gives the sign bit in the suite as well.
// The translator still runs those cases; they are only counted, except
// that an exception other than trace must stop the translated code by the
// end of the next block (a jump to an odd address faults at the fetch
// there), with the matching exit for a bus or address error: a case where
// it did not fails. A gating case that ends with
// Unimplemented is counted as pending: the instruction is not translated
// yet. Any other gating case that ends in a different state is a failure,
// and any failure makes the exit status non-zero.

#include "m68k_jit.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace hombre;
using namespace hombre::m68k;

namespace {

constexpr u32 kBusMask = 0x00FFFFFF;
constexpr u32 kSupervisor = 0x2000;
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
    u32 other_stack() const { return supervisor() ? usp : ssp; }
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

bool privileged(u32 op) {
    return (op & 0xFFC0) == 0x46C0     // MOVE to SR
           || (op & 0xFFC0) == 0x40C0  // MOVE from SR
           || (op & 0xFFF0) == 0x4E60  // MOVE USP
           || op == 0x027C || op == 0x007C || op == 0x0A7C  // ANDI, ORI, EORI to SR
           || op == 0x4E73 || op == 0x4E72 || op == 0x4E70;  // RTE, STOP, RESET
}

// Shifts and rotates of a data register, by an immediate or by a register
// count: 1110 ccc d ss i tt rrr with ss other than 11.
bool register_shift(u32 op) {
    return (op & 0xF000) == 0xE000 && (op & 0x00C0) != 0x00C0;
}

u32 register_shift_bits(u32 op) {
    return 8u << ((op >> 6) & 3);
}

// Two places where the expected state of the suite cannot be what the CPU
// does; see the file comment.
const char* suite_quirk(const Case& c) {
    const u32 op = c.initial.prefetch[0];
    if (!register_shift(op)) {
        return nullptr;
    }
    const u32 bits = register_shift_bits(op);
    const u32 size_mask = bits == 32 ? 0xFFFFFFFF : (1u << bits) - 1;
    const u32 r = op & 7;
    if (((c.initial.d[r] ^ c.final.d[r]) & ~size_mask) != 0) {
        return "quirk: bits beyond size";
    }
    const bool asr_by_register = (op & 0x0138) == 0x0020;
    const u32 count = c.initial.d[(op >> 9) & 7] & 63;
    const bool negative = (c.initial.d[r] >> (bits - 1) & 1) != 0;
    if (asr_by_register && count > bits && negative) {
        return "quirk: asr past size";
    }
    return nullptr;
}

// Empty for a gating case, else why it does not gate.
std::string non_gating_reason(const Case& c) {
    if (const u32 v = taken_vector(c); v != 0) {
        return vector_name(v);
    }
    const u32 op = c.initial.prefetch[0];
    if (privileged(op)) {
        return "privileged";
    }
    if ((op & 0xF000) == 0xA000 || (op & 0xF000) == 0xF000) {
        return "line a/f";
    }
    if (const char* quirk = suite_quirk(c)) {
        return quirk;
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

const char* exit_name(ExitReason why) {
    static const char* const names[] = {"None",    "Halt",         "Returned", "Budget",
                                        "Illegal", "Trap",         "DivideByZero", "Chk",
                                        "BusError", "AddressError", "Privilege", "Unimplemented"};
    return names[static_cast<u32>(why)];
}

class Harness {
public:
    Harness() : memory_(kGuestBytes) {
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
        m_.mem = GuestMemory{memory_.data(), kGuestBytes};
        if (!jit_.init(CodeRegion{static_cast<u8*>(rw), static_cast<const u8*>(rx), kRegionBytes},
                       m_)) {
            std::fprintf(stderr, "jit init failed\n");
            std::exit(2);
        }
        jit_.set_block_limit(1);
        jit_.set_address_mask(kBusMask);
    }

    // Runs the instruction from the initial state: the exit, and the
    // differences from the final state when the exit is the end of the
    // instruction.
    ExitReason run(const Case& c, std::vector<std::string>& diffs) {
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
        cpu.sr = static_cast<u16>(s.sr);
        cpu.pc = s.pc;
        for (const auto& [addr, value] : s.ram) {
            poke(addr, value);
        }
        for (u32 i = 0; i < 2; ++i) {
            poke(s.pc + 2 * i, s.prefetch[i] >> 8);
            poke(s.pc + 2 * i + 1, s.prefetch[i]);
        }
        jit_.flush();
        const ExitReason why = jit_.run(1);
        if (why != ExitReason::Budget && why != ExitReason::Unimplemented) {
            diffs.push_back(std::string("exit ") + exit_name(why) + " fault_addr " +
                            hex(cpu.fault_addr));
        }
        if (why == ExitReason::Budget) {
            compare(c, diffs);
        }
        for (const auto& [addr, value] : c.final.ram) {
            touched_.push_back(addr & kBusMask);
        }
        return why;
    }

    ExitReason next_block() { return jit_.run(1); }

    // Zeroes the memory the last case touched, or all of it after a case
    // that failed: a wrong store may have landed anywhere.
    void clear(bool all) {
        if (all) {
            std::memset(memory_.data(), 0, memory_.size());
        } else {
            for (u32 at : touched_) {
                memory_[at] = 0;
            }
        }
        touched_.clear();
    }

private:
    static std::string hex(u32 v) {
        char text[16];
        std::snprintf(text, sizeof text, "0x%08x", v);
        return text;
    }

    void poke(u32 addr, u32 value) {
        memory_[addr & kBusMask] = static_cast<u8>(value);
        touched_.push_back(addr & kBusMask);
    }

    void compare(const Case& c, std::vector<std::string>& diffs) const {
        const CpuState& cpu = m_.cpu;
        const State& f = c.final;
        const auto check = [&](const std::string& what, u32 want, u32 got) {
            if (want != got) {
                diffs.push_back(what + " want " + hex(want) + " got " + hex(got));
            }
        };
        for (u32 i = 0; i < 8; ++i) {
            check("d" + std::to_string(i), f.d[i], cpu.d[i]);
        }
        for (u32 i = 0; i < 7; ++i) {
            check("a" + std::to_string(i), f.a[i], cpu.a[i]);
        }
        check(f.supervisor() ? "a7/ssp" : "a7/usp", f.stack(), cpu.a[7]);
        check(f.supervisor() ? "usp" : "ssp", f.other_stack(), c.initial.other_stack());
        check("sr", f.sr, cpu.sr);
        check("pc", f.pc, cpu.pc);
        for (const auto& [addr, value] : f.ram) {
            check("byte " + hex(addr), value, memory_[addr & kBusMask]);
        }
    }

    std::vector<u8> memory_;
    std::vector<u32> touched_;
    Machine m_{};
    Jit jit_;
};

struct Tally {
    u64 cases = 0;
    u64 passed = 0;
    u64 failed = 0;
    u64 pending = 0;
    std::map<std::string, u64> non_gating;

    u64 non_gating_total() const {
        u64 n = 0;
        for (const auto& [why, count] : non_gating) {
            n += count;
        }
        return n;
    }

    void add(const Tally& t) {
        cases += t.cases;
        passed += t.passed;
        failed += t.failed;
        pending += t.pending;
        for (const auto& [why, count] : t.non_gating) {
            non_gating[why] += count;
        }
    }

    void print(const std::string& name) const {
        std::printf("%-20s cases %6llu passed %6llu failed %4llu pending %6llu non-gating %5llu",
                    name.c_str(), static_cast<unsigned long long>(cases),
                    static_cast<unsigned long long>(passed), static_cast<unsigned long long>(failed),
                    static_cast<unsigned long long>(pending),
                    static_cast<unsigned long long>(non_gating_total()));
        const char* sep = " (";
        for (const auto& [why, count] : non_gating) {
            std::printf("%s%s %llu", sep, why.c_str(), static_cast<unsigned long long>(count));
            sep = ", ";
        }
        std::printf("%s\n", non_gating.empty() ? "" : ")");
    }
};

// The Harness holds a 16 MiB guest memory and a whole Jit, so it lives in
// static storage, as the kernel keeps its Jit.
Harness* g_harness = nullptr;

// Runs one case and counts it under the file's tally.
void run_case(const Case& c, const std::string& file, Tally& t) {
    ++t.cases;
    std::vector<std::string> diffs;
    ExitReason why = g_harness->run(c, diffs);
    if (why == ExitReason::Budget && stops_the_code(c)) {
        // A jump or return to an odd address faults when the translator
        // fetches there, at the start of the next block.
        why = g_harness->next_block();
    }
    g_harness->clear(false);
    const std::string reason = non_gating_reason(c);
    const bool missed = stops_the_code(c) && !stopped_as_expected(c, why);
    if (missed) {
        diffs.assign(1, reason + " expected, exit " + exit_name(why));
    }
    if (!reason.empty() && !missed) {
        ++t.non_gating[reason];
    } else if (why == ExitReason::Unimplemented) {
        ++t.pending;
    } else if (diffs.empty()) {
        ++t.passed;
    } else {
        g_harness->clear(true);
        if (t.failed++ < kShownFailures) {
            std::printf("  FAIL %s: %s\n", file.c_str(), c.name.c_str());
            for (const std::string& d : diffs) {
                std::printf("       %s\n", d.c_str());
            }
        }
    }
}

Tally run_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "%s: cannot open\n", path.c_str());
        std::exit(2);
    }
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const std::string name = path.substr(path.find_last_of('/') + 1);
    Reader in(path, text);
    Tally t;
    in.array([&] { run_case(read_case(in), name, t); });
    if (!in.done()) {
        in.fail("trailing data");
    }
    t.print(name);
    return t;
}

}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("test_m68k_suite: skipping, no suite files given\n");
        return 0;
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // a line per file as it ends
    static Harness harness;
    g_harness = &harness;
    Tally total;
    for (int i = 1; i < argc; ++i) {
        total.add(run_file(argv[i]));
    }
    total.print("total");
    return total.failed == 0 ? 0 : 1;
}
