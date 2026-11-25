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

#include "m68k_cpu.hpp"
#include "types.hpp"

// Translates 68000 code to x86-64, one block at a time, and runs it.
//
// A block is straight-line guest code up to a branch, jump, return, A-line
// opcode or an instruction that ends the run, or up to the block limit (64
// instructions by default). Generated code keeps the guest registers in
// CpuState and loads them per instruction. While it runs, RBX holds the
// CpuState, R12 the guest memory base, R13 the guest memory size, R15 the
// address of the instruction being run and RBP the host stack pointer to
// unwind to when an access faults. R14 keeps an address across the calls
// to the C++ helpers (BCD, ROXL/ROXR, DIVU/DIVS, CHK), which generated
// code makes from block level with the stack 16-byte aligned.
//
// Every guest access is checked for 68000 alignment (AddressError) and,
// after the address mask, for addr + size <= mem.size (BusError); either
// ends the run with fault_addr set and pc at the instruction. The
// condition codes are computed after every instruction that sets them; X
// is bit 4 of sr.
//
// Exceptions are not delivered to the guest: where the 68000 would take
// one, the run ends with trap_number set to its vector.
//  - TRAP #n (Trap, vector 32 + n), TRAPV with V set (Trap, 7), CHK out
//    of bounds (Chk, 6) and division by zero (DivideByZero, 5) leave pc at
//    the next instruction, as the 68000 stacks it.
//  - Bus and address errors (2, 3), ILLEGAL and 68010+ opcodes (Illegal,
//    4), F-line opcodes (Illegal, 11), A-line opcodes without a native
//    (Unimplemented, 10) and privilege violations (Privilege, 8) leave pc
//    at the instruction.
//
// SR is plain data and there is one A7. The S bit only decides whether
// the privileged instructions run: with S clear MOVE to SR, ANDI/ORI/EORI
// to SR, MOVE USP, RESET, STOP and RTE end the run with Privilege. With S
// set the SR writes write SR (only the bits a 68000 has) and MOVE USP
// moves between An and cpu.usp, which A7 never sees. RESET, STOP and RTE
// mean nothing while exceptions are not delivered and end the run with
// Unimplemented. MOVE from SR is not privileged on the 68000.
//
// The code region is one set of pages seen through two views: generated
// code is written through `write` and run from `exec`. When it is full, or
// the block table is, everything is dropped and translation starts over.
//
// A-line opcodes 0xA000 | id dispatch to natives. The block ends at the
// opcode; run() calls the native from C++, so a native may call_guest() or
// flush(), and then goes on at the instruction after the opcode (an RTS in a
// library vector). Ids 0 and 1 are Halt and Returned.

namespace hombre::m68k {

struct CodeRegion {
    u8* write;
    const u8* exec;
    usize bytes;
};

class Jit;
struct Machine;

using NativeFn = void (*)(void* ctx, Machine& m);

struct NativeSlot {
    NativeFn fn;
    void* ctx;
    const char* name;
};

// Guest address holding A-line id 0 + RTS (Halt).
constexpr u32 kHaltStub = 0x00000400;
// A-line id 1 + RTS (Returned). Both sit just past the 256-vector table.
constexpr u32 kReturnStub = 0x00000404;

namespace detail {

// Offsets of the shared routines at the start of the code region.
struct Prelude {
    usize enter;
    usize leave;
    usize bus_error;
    usize address_error;
    usize helper_exit;
    usize read[3];   // byte, word, long
    usize write[3];
    usize ccr_nzvc;
    usize ccr_nzvcx;
    usize ccr_extend;
    usize ccr_shift;
    usize ccr_shift_x;
    usize end;
};

}

class Jit {
public:
    struct RunState {
        u64 budget_left = 0;
        u32 run_depth = 0;
        const char* native = nullptr;
        bool stop = false;
    };

    using PreemptHook = void (*)(void* ctx);

    // Takes the code region and the machine, writes the Halt and Returned
    // stubs into guest memory and drops all natives and translations. False
    // if the region is too small or guest memory cannot hold the stubs.
    bool init(const CodeRegion& region, Machine& machine);

    // Ids 0 (Halt) and 1 (Returned) are reserved; user ids start at 2, at
    // most 4094 of them. Returns the A-line id, 0 if full.
    u16 add_native(NativeFn fn, void* ctx, const char* name);

    // Calls registered native id directly. False if id is out of range or
    // not registered.
    bool call_native(u16 id);

    // Drops every translated block.
    void flush();

    // Runs from cpu.pc until cpu.exit != None, or until max_blocks blocks
    // have run (Budget). Clears cpu.exit first. The outermost run sets the
    // budget; a run inside a native (through call_guest) draws on what is
    // left of it as well as on its own max_blocks, so that calls nested
    // in natives cannot multiply it. A Budget there is logged.
    ExitReason run(u64 max_blocks);

    // Pushes kReturnStub, runs from entry until the code returns there and
    // comes back with Returned, cpu.exit cleared and cpu.pc as it was. Any
    // other exit is left in cpu.exit and returned.
    ExitReason call_guest(u32 entry, u64 max_blocks);

    // The most instructions in one block (at least 1). Drops every block.
    void set_block_limit(u32 instructions);

    // ANDed into every guest address before it reaches guest memory, the
    // instruction fetches included; the address registers and the pc keep
    // all 32 bits. 0x00FFFFFF gives the 68000's 24-bit address bus. Call
    // after init, which resets it to 0xFFFFFFFF. Drops every block.
    void set_address_mask(u32 mask);

    // The mask set_address_mask set. Natives apply it to the guest
    // addresses they are given, so that they see what translated code sees.
    u32 address_mask() const { return address_mask_; }

    // Bytes of the code region that hold translated code, and its size.
    usize code_used() const { return used_; }
    usize code_total() const { return region_.bytes; }

    // Called at the top of the run loop when run_depth_ == 1.
    void set_preempt_hook(PreemptHook fn, void* ctx) {
        preempt_hook_ = fn;
        preempt_ctx_ = ctx;
    }

    RunState save_run_state() const {
        return RunState{budget_left_, run_depth_, native_, stop_};
    }

    void restore_run_state(const RunState& state) {
        budget_left_ = state.budget_left;
        run_depth_ = state.run_depth;
        native_ = state.native;
        stop_ = state.stop;
    }

    // The name of the native the running task is in, null outside one.
    const char* current_native() const { return native_; }

    // How many runs are inside one another; 0 outside 68000 code.
    u32 run_depth() const { return run_depth_; }

    // Ends the running task's 68000 code, at every depth, with Stopped,
    // before its next block.
    void request_stop() { stop_ = true; }

    // True once request_stop was called and the running task's 68000 code
    // has not ended yet.
    bool stop_requested() const { return stop_; }

    // Called once, before the next block at any depth, after
    // raise_attention, which an interrupt may call.
    void set_attention_hook(PreemptHook fn, void* ctx) {
        attention_hook_ = fn;
        attention_ctx_ = ctx;
    }
    void raise_attention() { attention_ = true; }

private:
    static constexpr u32 kSlotBits = 13;
    static constexpr u32 kSlots = 1u << kSlotBits;
    static constexpr u32 kNatives = 4096;

    struct Block {
        u32 pc;
        u32 offset;  // in the code region; 0 marks an empty slot
    };

    const u8* lookup(u32 pc) const;
    const u8* translate(u32 pc);
    void dispatch(u32 opcode);
    void run_native(const NativeSlot& slot);

    Machine* machine_ = nullptr;
    CodeRegion region_{};
    detail::Prelude prelude_{};
    usize used_ = 0;
    u32 block_limit_ = 64;
    u32 address_mask_ = 0xFFFFFFFF;
    u64 budget_left_ = 0;  // blocks the outermost run has left
    u32 run_depth_ = 0;
    const char* native_ = nullptr;
    bool stop_ = false;
    volatile bool attention_ = false;
    PreemptHook attention_hook_ = nullptr;
    void* attention_ctx_ = nullptr;
    PreemptHook preempt_hook_ = nullptr;
    void* preempt_ctx_ = nullptr;
    u32 blocks_ = 0;
    u32 next_native_ = 2;
    Block cache_[kSlots]{};
    NativeSlot natives_[kNatives]{};
};

struct Machine {
    CpuState cpu;
    GuestMemory mem;
    Jit* jit;
};

}
