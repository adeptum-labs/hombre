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

#include "exec.hpp"

namespace hombre {

using m68k::CpuState;
using m68k::ExitReason;
using m68k::GuestMemory;
using m68k::Machine;
using native::Bus;
using native::LogLine;
using namespace ndk;

namespace {

constexpr u32 kLowestFree = 0x408;  // past the Jit's Halt and Returned stubs
// exec's vector table spans every negative d16(A6) displacement, so a call
// to any vector exec does not implement reaches the unimplemented native.
constexpr u32 kExecNegSize = 32768;
constexpr u32 kExecStrings = 96;
constexpr u32 kExecBlock = kExecNegSize + SYSBASESIZE + kExecStrings;
constexpr u32 kGuestCallBlocks = 1u << 24;
constexpr u32 kMaxCallDepth = 8;
constexpr u32 kMaxLoadDepth = 3;
constexpr u32 kMaxListNodes = 1u << 20;
constexpr u32 kMaxVectors = 0xFFFC / LIB_VECTSIZE;  // lib_NegSize is a UWORD
constexpr u32 kMaxName = 256;
constexpr u32 kMaxSegments = 64;
constexpr u32 kMaxInitCommands = 1u << 16;
constexpr u32 kMaxQueued = 4096;
constexpr char kExecName[] = "exec.library";
constexpr char kExecIdString[] = "exec 60.0 (Hombre)\r\n";
constexpr char kMemoryName[] = "memory";

constexpr u32 round8(u32 v) {
    return (v + 7) & ~7u;
}

// Ends the run unless it has ended already.
void end_run(Machine& m, ExitReason why) {
    if (m.cpu.exit == ExitReason::None) {
        m.cpu.exit = why;
    }
}

// Ends the run with BusError when a native is given a null guest pointer.
void fault_null(Bus& b) {
    b.bytes_at(0, b.machine().mem.size + 1);
}

// Reads the free chunk at addr and checks it: 8-aligned, at or above floor,
// at least 8 bytes, and ending at or below upper.
bool read_chunk(const GuestMemory& mem, u32 addr, u32 floor, u32 upper, u32& next, u32& bytes) {
    if ((addr & 7) != 0 || addr < floor || addr >= upper) {
        return false;
    }
    if (!mem.read32(addr + MC_NEXT, next) || !mem.read32(addr + MC_BYTES, bytes)) {
        return false;
    }
    return bytes >= MC_SIZE && (bytes & 7) == 0 && static_cast<u64>(addr) + bytes <= upper;
}

void log_corrupt_memory_list() {
    LogLine().text("exec: memory list is corrupt").send();
}

void put_string(Bus& b, u32 at, const char* s) {
    for (u32 i = 0;; ++i) {
        b.w8(at + i, static_cast<u8>(s[i]));
        if (s[i] == '\0') {
            return;
        }
    }
}

void new_list(Bus& b, u32 list, u8 type) {
    b.w32(list + LH_HEAD, list + LH_TAIL);
    b.w32(list + LH_TAIL, 0);
    b.w32(list + LH_TAILPRED, list + LH_HEAD);
    b.w8(list + LH_TYPE, type);
}

// --- lists ---------------------------------------------------------------

void add_head(Bus& b, u32 list, u32 node) {
    const u32 head = b.r32(list + LH_HEAD);
    b.w32(node + LN_SUCC, head);
    b.w32(node + LN_PRED, list + LH_HEAD);
    b.w32(head + LN_PRED, node);
    b.w32(list + LH_HEAD, node);
}

void add_tail(Bus& b, u32 list, u32 node) {
    const u32 last = b.r32(list + LH_TAILPRED);
    b.w32(node + LN_SUCC, list + LH_TAIL);
    b.w32(node + LN_PRED, last);
    b.w32(last + LN_SUCC, node);
    b.w32(list + LH_TAILPRED, node);
}

void remove(Bus& b, u32 node) {
    const u32 succ = b.r32(node + LN_SUCC);
    const u32 pred = b.r32(node + LN_PRED);
    b.w32(pred + LN_SUCC, succ);
    b.w32(succ + LN_PRED, pred);
}

u32 rem_head(Bus& b, u32 list) {
    const u32 head = b.r32(list + LH_HEAD);
    const u32 succ = b.r32(head + LN_SUCC);
    if (!b.ok() || succ == 0) {
        return 0;  // the head is the tail sentinel: empty
    }
    b.w32(list + LH_HEAD, succ);
    b.w32(succ + LN_PRED, list + LH_HEAD);
    return head;
}

u32 rem_tail(Bus& b, u32 list) {
    const u32 last = b.r32(list + LH_TAILPRED);
    const u32 pred = b.r32(last + LN_PRED);
    if (!b.ok() || pred == 0) {
        return 0;  // the last node is the head sentinel: empty
    }
    b.w32(list + LH_TAILPRED, pred);
    b.w32(pred + LN_SUCC, list + LH_TAIL);
    return last;
}

// Inserts node after pred; a null pred means at the head, the tail
// sentinel as pred (its ln_Succ is null) at the tail.
void insert(Bus& b, u32 list, u32 node, u32 pred) {
    if (pred == 0) {
        add_head(b, list, node);
        return;
    }
    const u32 succ = b.r32(pred + LN_SUCC);
    if (!b.ok()) {
        return;
    }
    if (succ == 0) {
        add_tail(b, list, node);
        return;
    }
    b.w32(node + LN_SUCC, succ);
    b.w32(node + LN_PRED, pred);
    b.w32(succ + LN_PRED, node);
    b.w32(pred + LN_SUCC, node);
}

void log_endless_list(u32 list) {
    LogLine().text("exec: list at ").hex(list).text(" has no end").send();
}

// Inserts node before the first node of lower priority.
void enqueue(Bus& b, u32 list, u32 node) {
    const u32 pri = b.r8(node + LN_PRI) ^ 0x80;  // signed order as unsigned
    u32 n = b.r32(list + LH_HEAD);
    for (u32 count = 0;; ++count) {
        if (count == kMaxListNodes) {
            log_endless_list(list);
            return;
        }
        const u32 succ = b.r32(n + LN_SUCC);
        if (!b.ok()) {
            return;
        }
        if (succ == 0 || (b.r8(n + LN_PRI) ^ 0x80) < pri) {
            break;
        }
        n = succ;
    }
    const u32 pred = b.r32(n + LN_PRED);
    b.w32(node + LN_SUCC, n);
    b.w32(node + LN_PRED, pred);
    b.w32(pred + LN_SUCC, node);
    b.w32(n + LN_PRED, node);
}

// The first node after start (a list header or a node) whose name matches.
template <typename Match>
u32 find_node(Bus& b, u32 start, Match match) {
    u32 n = b.r32(start + LN_SUCC);
    for (u32 count = 0; count < kMaxListNodes; ++count) {
        const u32 succ = b.r32(n + LN_SUCC);
        if (!b.ok() || succ == 0) {
            return 0;
        }
        const u32 name = b.r32(n + LN_NAME);
        if (name != 0 && match(name)) {
            return n;
        }
        if (!b.ok()) {
            return 0;
        }
        n = succ;
    }
    log_endless_list(start);
    return 0;
}

u32 find_name(Bus& b, u32 start, const char* name) {
    return find_node(b, start, [&](u32 node_name) { return b.equals(node_name, name); });
}

// --- 68000 calls ---------------------------------------------------------

// Calls 68000 code from a native. It saves the registers when made; the
// caller then sets the arguments in the CPU and calls run(), which on a
// return keeps D0 and A3 and puts every register back as it was saved.
class GuestCall {
public:
    GuestCall(u32& depth, Bus& bus) : depth_(depth), bus_(bus), cpu_(bus.machine().cpu) {
        for (u32 i = 0; i < 8; ++i) {
            d_[i] = cpu_.d[i];
            a_[i] = cpu_.a[i];
        }
    }

    // False if the run ended in the called code or could not start; the
    // native must then return at once and leave the CPU as it is.
    bool run(u32 entry) {
        if (!bus_.ok()) {
            return false;
        }
        if (depth_ >= kMaxCallDepth) {
            LogLine().text("exec: calls into 68000 code nested too deep").send();
            end_run(bus_.machine(), ExitReason::Unimplemented);
            bus_.stop();
            return false;
        }
        ++depth_;
        const ExitReason why = bus_.machine().jit->call_guest(entry, kGuestCallBlocks);
        --depth_;
        if (why != ExitReason::Returned) {
            bus_.stop();
            return false;
        }
        d0_ = cpu_.d[0];
        a3_ = cpu_.a[3];
        for (u32 i = 0; i < 8; ++i) {
            cpu_.d[i] = d_[i];
            cpu_.a[i] = a_[i];
        }
        return true;
    }

    u32 d0() const { return d0_; }
    u32 a3() const { return a3_; }

private:
    u32& depth_;
    Bus& bus_;
    CpuState& cpu_;
    u32 d_[8];
    u32 a_[8];
    u32 d0_ = 0;
    u32 a3_ = 0;
};

// Where RawDoFmt sends its characters: the buffer in A3, or the 68000
// routine in A2 called with the character in D0 and A3 as it left it.
struct FormatOutput {
    u32& depth;
    Bus& bus;
    u32 proc;
    u32 data;

    bool put(u32 c) {
        if (proc == 0) {
            bus.w8(data, c);
            ++data;
            return bus.ok();
        }
        GuestCall call(depth, bus);
        CpuState& cpu = bus.machine().cpu;
        cpu.d[0] = c & 0xFF;
        cpu.a[3] = data;
        if (!call.run(proc)) {
            return false;
        }
        data = call.a3();
        return true;
    }
};

}

// The exec natives. A member of no instance: each gets the Exec as its
// context (or a LibraryInfo for the unimplemented vectors).
struct ExecOps {
    static Exec& of(void* ctx) { return *static_cast<Exec*>(ctx); }

    // --- vectors exec itself fills ---------------------------------------

    static void unimplemented(void* ctx, Machine& m) {
        const Exec::LibraryInfo& info = *static_cast<const Exec::LibraryInfo*>(ctx);
        const u32 at = m.cpu.pc - 2;
        LogLine().stem(info.name).text(": unimplemented LVO -").dec(info.base - at).send();
        m.cpu.pc = at;
        m.cpu.trap_number = 10;
        end_run(m, ExitReason::Unimplemented);
    }

    static void lib_open(void*, Machine& m) {
        Bus b(m);
        const u32 lib = m.cpu.a[6];
        b.w16(lib + LIB_OPENCNT, b.r16(lib + LIB_OPENCNT) + 1);
        b.w8(lib + LIB_FLAGS, b.r8(lib + LIB_FLAGS) & ~LIBF_DELEXP);
        native::set_result(m, lib);
    }

    static void lib_close(void*, Machine& m) {
        Bus b(m);
        const u32 lib = m.cpu.a[6];
        const u32 count = b.r16(lib + LIB_OPENCNT);
        if (count != 0) {
            b.w16(lib + LIB_OPENCNT, count - 1);
        }
        native::set_result(m, 0);
    }

    static void lib_null(void*, Machine& m) { native::set_result(m, 0); }

    // --- memory ----------------------------------------------------------

    static void alloc_mem(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).alloc(m.cpu.d[0], m.cpu.d[1]));
    }

    static void free_mem(void* ctx, Machine& m) { of(ctx).free(m.cpu.a[1], m.cpu.d[0]); }

    static void avail_mem(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        const u32 flags = m.cpu.d[1];
        if ((flags & MEMF_TOTAL) != 0) {
            native::set_result(m, e.upper_ - e.lower_);
            return;
        }
        if ((flags & MEMF_LARGEST) == 0) {
            native::set_result(m, e.free_bytes_);
            return;
        }
        u32 largest = 0;
        u32 chunk = 0;
        u32 floor = e.lower_;
        m.mem.read32(e.header_ + MH_FIRST, chunk);
        while (chunk != 0) {
            u32 next = 0;
            u32 bytes = 0;
            if (!read_chunk(m.mem, chunk, floor, e.upper_, next, bytes)) {
                log_corrupt_memory_list();
                break;
            }
            largest = bytes > largest ? bytes : largest;
            floor = chunk + bytes;
            chunk = next;
        }
        native::set_result(m, largest);
    }

    // AllocVec keeps the size of the whole block, its 8-byte header
    // included, in the longword just below the pointer it returns.
    static void alloc_vec(void* ctx, Machine& m) {
        const u32 bytes = m.cpu.d[0];
        if (bytes == 0 || bytes > 0xFFFFFFFFu - 8) {
            native::set_result(m, 0);
            return;
        }
        const u32 block = of(ctx).alloc(bytes + 8, m.cpu.d[1]);
        if (block == 0) {
            native::set_result(m, 0);
            return;
        }
        m.mem.write32(block + 4, bytes + 8);
        native::set_result(m, block + 8);
    }

    static void free_vec(void* ctx, Machine& m) {
        const u32 addr = native::guest_address(m, m.cpu.a[1]);
        if (addr == 0) {
            return;
        }
        u32 total = 0;
        if (addr < 8 || !m.mem.read32(addr - 4, total) || total <= 8) {
            LogLine().text("exec: FreeMem of a bad block at ").hex(addr).send();
            return;
        }
        of(ctx).free(addr - 8, total);
    }

    static void copy_mem(void*, Machine& m) {
        Bus b(m);
        const u32 src = m.cpu.a[0];
        const u32 dst = m.cpu.a[1];
        const u32 bytes = m.cpu.d[0];
        if (bytes == 0) {
            return;
        }
        const u8* from = b.bytes_at(src, bytes);
        u8* to = from != nullptr ? b.bytes_at(dst, bytes) : nullptr;
        if (to == nullptr) {
            return;
        }
        if (to < from) {
            for (u32 i = 0; i < bytes; ++i) {
                to[i] = from[i];
            }
        } else {
            for (u32 i = bytes; i > 0; --i) {
                to[i - 1] = from[i - 1];
            }
        }
    }

    static void cache_clear(void*, Machine& m) { m.jit->flush(); }

    // --- lists -----------------------------------------------------------

    static void insert_node(void*, Machine& m) {
        Bus b(m);
        insert(b, m.cpu.a[0], m.cpu.a[1], m.cpu.a[2]);
    }

    static void add_head_node(void*, Machine& m) {
        Bus b(m);
        add_head(b, m.cpu.a[0], m.cpu.a[1]);
    }

    static void add_tail_node(void*, Machine& m) {
        Bus b(m);
        add_tail(b, m.cpu.a[0], m.cpu.a[1]);
    }

    static void remove_node(void*, Machine& m) {
        Bus b(m);
        remove(b, m.cpu.a[1]);
    }

    static void rem_head_node(void*, Machine& m) {
        Bus b(m);
        native::set_result(m, rem_head(b, m.cpu.a[0]));
    }

    static void rem_tail_node(void*, Machine& m) {
        Bus b(m);
        native::set_result(m, rem_tail(b, m.cpu.a[0]));
    }

    static void enqueue_node(void*, Machine& m) {
        Bus b(m);
        enqueue(b, m.cpu.a[0], m.cpu.a[1]);
    }

    static void find_name_node(void*, Machine& m) {
        Bus b(m);
        const u32 name = m.cpu.a[1];
        const u32 found =
            name == 0 ? 0 : find_node(b, m.cpu.a[0], [&](u32 n) { return b.equals(n, name); });
        native::set_result(m, found);
    }

    // --- tasks and signals -----------------------------------------------

    static void preempt_hook(void* ctx) { of(ctx).maybe_preempt(); }

    static bool poll_idle(Exec& e) {
        if (e.task_host_.idle != nullptr) {
            return e.task_host_.idle(e.task_host_.ctx);
        }
        if (e.idle_.poll != nullptr) {
            return e.idle_.poll(e.idle_.ctx);
        }
        return false;
    }

    static u32 sig_alloc_of(Exec& e, Bus& b, u32 addr) {
        return addr != 0 ? b.r32(addr + TC_SIGALLOC) : e.sig_alloc_;
    }

    static void store_sig_alloc_of(Exec& e, Bus& b, u32 addr, u32 value) {
        if (addr != 0) {
            b.w32(addr + TC_SIGALLOC, value);
        } else {
            e.sig_alloc_ = value;
        }
    }

    static u32 signals_of(Exec& e, Bus& b, u32 addr) {
        return addr != 0 ? b.r32(addr + TC_SIGRECVD) : e.signals_;
    }

    static void store_signals_of(Exec& e, Bus& b, u32 addr, u32 value) {
        if (addr != 0) {
            b.w32(addr + TC_SIGRECVD, value);
        } else {
            e.signals_ = value;
        }
    }

    static u32 sig_alloc(Exec& e, Bus& b) { return sig_alloc_of(e, b, e.task_); }
    static void store_sig_alloc(Exec& e, Bus& b, u32 value) {
        store_sig_alloc_of(e, b, e.task_, value);
    }
    static u32 signals(Exec& e, Bus& b) { return signals_of(e, b, e.task_); }
    static void store_signals(Exec& e, Bus& b, u32 value) {
        store_signals_of(e, b, e.task_, value);
    }

    // Signals `task_addr` (any guest Task, known or unknown). Wakes a waiting
    // task whose awaited mask matches and flags an immediate switch if a higher
    // priority task became ready.
    static void signal_task(Exec& e, Bus& b, u32 task_addr, u32 mask) {
        const u32 addr = native::guest_address(*e.m_, task_addr);
        if (addr == 0) {
            return;
        }
        const u32 old = b.r32(addr + TC_SIGRECVD);
        const u32 recvd = old | mask;
        b.w32(addr + TC_SIGRECVD, recvd);
        if (!b.ok()) {
            return;
        }
        const i32 id = e.sched_.find(addr);
        if (id < 0) {
            return;
        }
        if (e.sched_.slot(static_cast<u32>(id)).state == TaskState::Waiting) {
            const u32 wanted = b.r32(addr + TC_SIGWAIT);
            if (!b.ok()) {
                return;
            }
            if ((recvd & wanted) != 0) {
                remove(b, addr);
                b.w8(addr + TC_STATE, TS_READY);
                e.sched_.wake(id);
                enqueue(b, e.base_ + TaskReady, addr);
                if (e.sched_.should_switch_now()) {
                    e.pending_switch_ = true;
                }
            }
        }
    }

    static void forbid(void* ctx, Machine& m) { nest(of(ctx), m, TDNestCnt, 1); }

    static void permit(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        nest(e, m, TDNestCnt, 0xFF);
        if (!e.forbidden() && e.sched_.should_switch_now()) {
            e.pending_switch_ = true;
        }
    }

    static void disable(void* ctx, Machine& m) { nest(of(ctx), m, IDNestCnt, 1); }

    static void enable(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        nest(e, m, IDNestCnt, 0xFF);
        if (!e.forbidden() && e.sched_.should_switch_now()) {
            e.pending_switch_ = true;
        }
    }

    static void nest(Exec& e, Machine& m, u32 field, u32 step) {
        Bus b(m);
        b.w8(e.base_ + field, b.r8(e.base_ + field) + step);
    }

    static void add_task_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 task_raw = m.cpu.a[1];
        const u32 task = native::guest_address(m, task_raw);
        if (task == 0) {
            fault_null(b);
            return;
        }
        const u32 init_pc = m.cpu.a[2];
        const u32 final_pc = m.cpu.a[3];
        const i8 pri = static_cast<i8>(b.r8(task + LN_PRI));
        const u32 sp_reg = b.r32(task + TC_SPREG);
        const u32 existing_alloc = b.r32(task + TC_SIGALLOC);
        if (!b.ok()) {
            return;
        }
        const u32 child_sp = sp_reg - 4;
        b.w32(child_sp, final_pc != 0 ? final_pc : e.task_exit_stub_);
        if (!b.ok()) {
            return;
        }
        const i32 id = e.sched_.add(task, pri);
        if (id < 0) {
            native::set_result(m, 0);
            return;
        }
        if (e.task_host_.create != nullptr &&
            !e.task_host_.create(e.task_host_.ctx, static_cast<u32>(id))) {
            e.sched_.remove(id);
            e.sched_.release(id);
            native::set_result(m, 0);
            return;
        }
        if (b.r8(task + LN_TYPE) == 0) {
            b.w8(task + LN_TYPE, NT_TASK);
        }
        b.w8(task + TC_STATE, TS_READY);
        b.w8(task + TC_IDNESTCNT, 0xFF);
        b.w8(task + TC_TDNESTCNT, 0xFF);
        if (existing_alloc == 0) {
            b.w32(task + TC_SIGALLOC, 0x0000FFFF);
        }
        b.w32(task + TC_SIGWAIT, 0);
        b.w32(task + TC_SIGRECVD, 0);
        b.w32(task + TC_SPREG, child_sp);
        enqueue(b, e.base_ + TaskReady, task);
        if (!b.ok()) {
            e.sched_.remove(id);
            e.reap_removed();
            return;
        }
        Exec::TaskContext& tc = e.tasks_[id];
        tc = Exec::TaskContext{};
        tc.cpu.pc = init_pc;
        tc.cpu.a[7] = child_sp;
        tc.run_state = m68k::Jit::RunState{~0ull, 0};
        tc.call_depth = 0;
        tc.task_addr = task;
        tc.budget = ~0ull;
        tc.td_nest = 0xFF;
        tc.id_nest = 0xFF;
        native::set_result(m, task_raw);
        if (e.sched_.should_switch_now()) {
            e.pending_switch_ = true;
        }
    }

    static void switch_away_after_block(Exec& e, Bus& b, i32 cur, u32 cur_addr, bool is_remove) {
        Machine& m = b.machine();
        i32 next = e.sched_.pick_next();
        while (next < 0) {
            const bool can_wake = poll_idle(e);
            if (m.cpu.exit != ExitReason::None) {
                if (!is_remove && e.sched_.slot(static_cast<u32>(cur)).state == TaskState::Waiting) {
                    if (cur_addr != 0) {
                        remove(b, cur_addr);
                        b.w8(cur_addr + TC_STATE, TS_RUN);
                    }
                    e.sched_.wake(cur);
                    e.sched_.switch_to(cur);
                }
                b.stop();
                return;
            }
            next = e.sched_.pick_next();
            if (next < 0 && !can_wake) {
                if (!e.every_task_waits_logged_) {
                    e.every_task_waits_logged_ = true;
                    LogLine().text("exec: every task waits").send();
                }
                if (cur == 0) {
                    if (!is_remove && cur_addr != 0) {
                        remove(b, cur_addr);
                        b.w8(cur_addr + TC_STATE, TS_RUN);
                    }
                    if (!is_remove) {
                        e.sched_.wake(0);
                        e.sched_.switch_to(0);
                    }
                    end_run(m, ExitReason::Unimplemented);
                    b.stop();
                    return;
                }
                e.abort_to_boot_task_ = true;
                if (e.sched_.slot(0).state == TaskState::Waiting) {
                    if (e.tasks_[0].task_addr != 0) {
                        remove(b, e.tasks_[0].task_addr);
                        b.w8(e.tasks_[0].task_addr + TC_STATE, TS_READY);
                    }
                    e.sched_.wake(0);
                    if (e.tasks_[0].task_addr != 0) {
                        enqueue(b, e.base_ + TaskReady, e.tasks_[0].task_addr);
                    }
                }
                next = 0;
                break;
            }
        }
        if (e.task_host_.switch_to == nullptr) {
            if (is_remove) {
                end_run(m, ExitReason::Returned);
                b.stop();
            }
            return;
        }
        e.switch_to_task(next);
    }

    static void rem_task_id(Exec& e, Bus& b, i32 target_id) {
        Machine& m = b.machine();
        const i32 cur = e.sched_.current();
        if (target_id < 0) {
            return;
        }
        if (target_id == 0) {
            LogLine().text("exec: refusing to RemTask the boot task").send();
            return;
        }
        const Scheduler::Slot slot = e.sched_.slot(static_cast<u32>(target_id));
        const u32 target_addr = slot.task_addr;
        if ((slot.state == TaskState::Ready || slot.state == TaskState::Waiting) &&
            target_addr != 0) {
            remove(b, target_addr);
        }
        e.remove_sem_waiter(static_cast<u32>(target_id));
        if (target_addr != 0) {
            b.w8(target_addr + TC_STATE, TS_REMOVED);
        }
        e.sched_.remove(target_id);
        if (target_id != cur) {
            e.reap_removed();
            return;
        }
        if (e.task_host_.switch_to == nullptr) {
            end_run(m, ExitReason::Returned);
            b.stop();
            return;
        }
        switch_away_after_block(e, b, cur, target_addr, true);
    }

    static void rem_task_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 arg = m.cpu.a[1];
        const i32 target = arg == 0 ? e.sched_.current()
                                    : e.sched_.find(native::guest_address(m, arg));
        rem_task_id(e, b, target);
    }

    static void task_exit_stub(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        rem_task_id(e, b, e.sched_.current());
    }

    static void set_task_pri_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 addr = native::guest_address(m, m.cpu.a[1]);
        if (addr == 0) {
            native::set_result(m, 0);
            return;
        }
        const i8 new_pri = static_cast<i8>(m.cpu.d[0] & 0xFF);
        const i8 old_pri = static_cast<i8>(b.r8(addr + LN_PRI));
        if (!b.ok()) {
            return;
        }
        b.w8(addr + LN_PRI, static_cast<u8>(new_pri));
        const i32 id = e.sched_.find(addr);
        if (id >= 0) {
            const TaskState st = e.sched_.slot(static_cast<u32>(id)).state;
            if (st == TaskState::Ready) {
                remove(b, addr);
                e.sched_.set_priority(id, new_pri);
                enqueue(b, e.base_ + TaskReady, addr);
            } else if (st == TaskState::Waiting) {
                remove(b, addr);
                e.sched_.set_priority(id, new_pri);
                enqueue(b, e.base_ + TaskWait, addr);
            } else {
                e.sched_.set_priority(id, new_pri);
            }
            if (e.sched_.should_switch_now()) {
                e.pending_switch_ = true;
            }
        }
        if (b.ok()) {
            native::set_result(m, static_cast<u32>(static_cast<i32>(old_pri)));
        }
    }

    static void find_task(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 name = m.cpu.a[1];
        if (name == 0) {
            native::set_result(m, e.task_);
            return;
        }
        b.r8(name);
        if (!b.ok()) {
            return;
        }
        u32 found = 0;
        if (e.task_ != 0) {
            const u32 task_name = b.r32(e.task_ + LN_NAME);
            if (task_name != 0 && b.equals(task_name, name)) {
                found = e.task_;
            }
        }
        if (found == 0 && b.ok()) {
            found = find_node(b, e.base_ + TaskReady, [&](u32 n) { return b.equals(n, name); });
        }
        if (found == 0 && b.ok()) {
            found = find_node(b, e.base_ + TaskWait, [&](u32 n) { return b.equals(n, name); });
        }
        if (b.ok()) {
            native::set_result(m, found);
        }
    }

    static void set_signal(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 old = signals(e, b);
        const u32 mask = m.cpu.d[1];
        store_signals(e, b, (old & ~mask) | (m.cpu.d[0] & mask));
        native::set_result(m, old);
    }

    static void signal_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        signal_task(e, b, m.cpu.a[1], m.cpu.d[0]);
    }

    static u32 alloc_signal(Exec& e, Bus& b, u32 signal_num) {
        const u32 alloc = sig_alloc(e, b);
        if (!b.ok()) {
            return 0xFFFFFFFF;
        }
        u32 chosen = 0xFFFFFFFF;
        if (signal_num == 0xFFFFFFFF || signal_num == 0xFF) {
            for (u32 bit = 16; bit < 32; ++bit) {
                if ((alloc & (1u << bit)) == 0) {
                    chosen = bit;
                    break;
                }
            }
        } else if (signal_num < 32 && (alloc & (1u << signal_num)) == 0) {
            chosen = signal_num;
        }
        if (chosen >= 32) {
            return 0xFFFFFFFF;
        }
        const u32 mask = 1u << chosen;
        store_sig_alloc(e, b, alloc | mask);
        store_signals(e, b, signals(e, b) & ~mask);
        return b.ok() ? chosen : 0xFFFFFFFF;
    }

    static void alloc_signal_lvo(void* ctx, Machine& m) {
        Bus b(m);
        native::set_result(m, alloc_signal(of(ctx), b, m.cpu.d[0]));
    }

    static void free_signal(Exec& e, Bus& b, u32 signal_num) {
        if (signal_num < 32) {
            store_sig_alloc(e, b, sig_alloc(e, b) & ~(1u << signal_num));
        }
    }

    static void free_signal_lvo(void* ctx, Machine& m) {
        Bus b(m);
        free_signal(of(ctx), b, m.cpu.d[0]);
    }

    // Waits for any bit in wanted. While none is pending, polls idle or
    // switches to the next ready task; ends the run with Break on an unwanted
    // Ctrl+C for the boot task, or Unimplemented when nothing can wake.
    static u32 wait_signals(Exec& e, Bus& b, u32 wanted) {
        Machine& m = b.machine();
        for (;;) {
            const u32 pending = signals(e, b);
            const u32 got = pending & wanted;
            if (!b.ok()) {
                return 0;
            }
            if (got != 0) {
                store_signals(e, b, pending & ~got);
                return got;
            }
            const i32 cur = e.sched_.current();
            const u32 cur_addr = e.task_;
            const u32 raisable = e.port_signals_ | SIGBREAKF_CTRL_C;
            if (cur == 0 && e.sched_.active_count() == 1 && (wanted & raisable) == 0) {
                LogLine().text("exec: Wait with no signals pending").send();
                end_run(m, ExitReason::Unimplemented);
                b.stop();
                return 0;
            }
            if (cur == 0 && e.sched_.active_count() == 1 && e.task_host_.switch_to == nullptr) {
                if (!poll_idle(e)) {
                    if (m.cpu.exit == ExitReason::None) {
                        LogLine().text("exec: Wait with no signals pending").send();
                        end_run(m, ExitReason::Unimplemented);
                    }
                    b.stop();
                    return 0;
                }
                if (m.cpu.exit != ExitReason::None) {
                    b.stop();
                    return 0;
                }
                if ((wanted & SIGBREAKF_CTRL_C) == 0) {
                    const u32 after = signals(e, b);
                    if (!b.ok()) {
                        return 0;
                    }
                    if ((after & SIGBREAKF_CTRL_C) != 0) {
                        store_signals(e, b, after & ~SIGBREAKF_CTRL_C);
                        LogLine().text("exec: Wait interrupted by Ctrl+C").send();
                        end_run(m, ExitReason::Break);
                        b.stop();
                        return 0;
                    }
                }
                continue;
            }

            if (cur_addr != 0) {
                b.w32(cur_addr + TC_SIGWAIT, wanted);
                b.w8(cur_addr + TC_STATE, TS_WAIT);
                enqueue(b, e.base_ + TaskWait, cur_addr);
                if (!b.ok()) {
                    return 0;
                }
            }
            e.sched_.wait(cur);

            i32 next = e.sched_.pick_next();
            while (next < 0) {
                const bool can_wake = poll_idle(e);
                if (m.cpu.exit != ExitReason::None) {
                    if (e.sched_.slot(static_cast<u32>(cur)).state == TaskState::Waiting) {
                        if (cur_addr != 0) {
                            remove(b, cur_addr);
                            b.w8(cur_addr + TC_STATE, TS_RUN);
                        }
                        e.sched_.wake(cur);
                        e.sched_.switch_to(cur);
                    }
                    b.stop();
                    return 0;
                }
                if (cur == 0 && (wanted & SIGBREAKF_CTRL_C) == 0) {
                    const u32 after = signals(e, b);
                    if ((after & SIGBREAKF_CTRL_C) != 0) {
                        store_signals(e, b, after & ~SIGBREAKF_CTRL_C);
                        if (cur_addr != 0) {
                            b.w32(cur_addr + TC_SIGWAIT, 0);
                            if (e.sched_.slot(0).state == TaskState::Waiting) {
                                remove(b, cur_addr);
                                b.w8(cur_addr + TC_STATE, TS_RUN);
                                e.sched_.wake(0);
                                e.sched_.switch_to(0);
                            }
                        }
                        LogLine().text("exec: Wait interrupted by Ctrl+C").send();
                        end_run(m, ExitReason::Break);
                        b.stop();
                        return 0;
                    }
                }
                next = e.sched_.pick_next();
                if (next < 0 && !can_wake) {
                    if (cur == 0 && e.sched_.active_count() == 1) {
                        if (cur_addr != 0) {
                            b.w32(cur_addr + TC_SIGWAIT, 0);
                            remove(b, cur_addr);
                            b.w8(cur_addr + TC_STATE, TS_RUN);
                        }
                        e.sched_.wake(0);
                        e.sched_.switch_to(0);
                        LogLine().text("exec: Wait with no signals pending").send();
                        end_run(m, ExitReason::Unimplemented);
                        b.stop();
                        return 0;
                    }
                    if (!e.every_task_waits_logged_) {
                        e.every_task_waits_logged_ = true;
                        LogLine().text("exec: every task waits").send();
                    }
                    if (cur == 0) {
                        if (cur_addr != 0) {
                            b.w32(cur_addr + TC_SIGWAIT, 0);
                            remove(b, cur_addr);
                            b.w8(cur_addr + TC_STATE, TS_RUN);
                        }
                        e.sched_.wake(0);
                        e.sched_.switch_to(0);
                        end_run(m, ExitReason::Unimplemented);
                        b.stop();
                        return 0;
                    }
                    e.abort_to_boot_task_ = true;
                    if (e.sched_.slot(0).state == TaskState::Waiting) {
                        if (e.tasks_[0].task_addr != 0) {
                            remove(b, e.tasks_[0].task_addr);
                            b.w8(e.tasks_[0].task_addr + TC_STATE, TS_READY);
                        }
                        e.sched_.wake(0);
                        if (e.tasks_[0].task_addr != 0) {
                            enqueue(b, e.base_ + TaskReady, e.tasks_[0].task_addr);
                        }
                    }
                    next = 0;
                    break;
                }
            }

            if (next == cur) {
                if (cur_addr != 0) {
                    remove(b, cur_addr);
                    b.w8(cur_addr + TC_STATE, TS_RUN);
                    b.w32(cur_addr + TC_SIGWAIT, 0);
                }
                e.sched_.switch_to(cur);
                continue;
            }
            if (e.task_host_.switch_to == nullptr) {
                if (cur_addr != 0) {
                    remove(b, cur_addr);
                    b.w8(cur_addr + TC_STATE, TS_RUN);
                    b.w32(cur_addr + TC_SIGWAIT, 0);
                }
                e.sched_.wake(cur);
                e.sched_.switch_to(cur);
                LogLine().text("exec: Wait with no signals pending").send();
                end_run(m, ExitReason::Unimplemented);
                b.stop();
                return 0;
            }
            e.switch_to_task(next);
            if (e.sched_.current() != cur) {
                return 0;
            }
            if (cur_addr != 0) {
                b.w32(cur_addr + TC_SIGWAIT, 0);
            }
            if (e.abort_to_boot_task_ && cur == 0) {
                end_run(m, ExitReason::Unimplemented);
                b.stop();
                return 0;
            }
            if (cur == 0 && (wanted & SIGBREAKF_CTRL_C) == 0) {
                const u32 after = signals(e, b);
                if (b.ok() && (after & SIGBREAKF_CTRL_C) != 0) {
                    store_signals(e, b, after & ~SIGBREAKF_CTRL_C);
                    LogLine().text("exec: Wait interrupted by Ctrl+C").send();
                    end_run(m, ExitReason::Break);
                    b.stop();
                    return 0;
                }
            }
        }
    }

    static void wait(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 got = wait_signals(e, b, m.cpu.d[0]);
        if (b.ok()) {
            native::set_result(m, got);
        }
    }

    // --- message ports ---------------------------------------------------

    static u32 peek_head(Bus& b, u32 port) {
        const u32 list = port + mp_MsgList;
        const u32 head = b.r32(list + LH_HEAD);
        if (!b.ok()) {
            return 0;
        }
        if (head == 0) {
            fault_null(b);
            return 0;
        }
        const u32 succ = b.r32(head + LN_SUCC);
        if (!b.ok() || succ == 0) {
            return 0;
        }
        return head;
    }

    static u32 get_msg(Bus& b, u32 port) {
        const u32 head = peek_head(b, port);
        if (head == 0 || !b.ok()) {
            return 0;
        }
        const u32 list = port + mp_MsgList;
        const u32 succ = b.r32(head + LN_SUCC);
        b.w32(list + LH_HEAD, succ);
        b.w32(succ + LN_PRED, list + LH_HEAD);
        return b.ok() ? head : 0;
    }

    static bool queue_message(Exec& e, Bus& b, u32 port, u32 message, u8 type) {
        const u32 list = port + mp_MsgList;
        u32 n = b.r32(list + LH_HEAD);
        if (!b.ok()) {
            return false;
        }
        if (n == 0) {
            fault_null(b);
            return false;
        }
        for (u32 count = 0;; ++count) {
            const u32 succ = b.r32(n + LN_SUCC);
            if (!b.ok()) {
                return false;
            }
            if (succ == 0) {
                break;
            }
            if (count + 1 >= kMaxQueued) {
                LogLine().text("exec: message port at ").hex(port).text(" is full").send();
                return false;
            }
            n = succ;
        }
        const u32 flags = b.r8(port + mp_Flags);
        const u32 sig_bit = b.r8(port + mp_SigBit);
        const u32 sig_task = b.r32(port + mp_SigTask);
        b.w8(message + LN_TYPE, type);
        add_tail(b, list, message);
        if (!b.ok()) {
            return false;
        }
        const u32 pa = flags & 3;
        if (pa == PA_SIGNAL) {
            if (sig_bit < 32 && sig_task != 0) {
                signal_task(e, b, sig_task, 1u << sig_bit);
            }
        } else if (pa == PA_SOFTINT) {
            if (!e.softint_logged_) {
                e.softint_logged_ = true;
                LogLine().text("exec: PA_SOFTINT not handled").send();
            }
        }
        return b.ok();
    }

    static void create_msg_port_lvo(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).create_port());
    }

    static void delete_msg_port_lvo(void* ctx, Machine& m) {
        of(ctx).delete_port(m.cpu.a[0]);
    }

    static void add_port_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 port = m.cpu.a[1];
        if (native::guest_address(m, port) == 0) {
            fault_null(b);
            return;
        }
        add_tail(b, e.base_ + PortList, port);
    }

    static void rem_port_lvo(void*, Machine& m) {
        Bus b(m);
        const u32 port = m.cpu.a[1];
        if (native::guest_address(m, port) == 0) {
            fault_null(b);
            return;
        }
        remove(b, port);
    }

    static void find_port_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 name = m.cpu.a[1];
        if (native::guest_address(m, name) == 0) {
            fault_null(b);
            return;
        }
        b.r8(name);
        if (!b.ok()) {
            return;
        }
        const u32 found =
            find_node(b, e.base_ + PortList, [&](u32 n) { return b.equals(n, name); });
        native::set_result(m, found);
    }

    static void put_msg_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 port = m.cpu.a[0];
        const u32 message = m.cpu.a[1];
        if (native::guest_address(m, port) == 0 || native::guest_address(m, message) == 0) {
            fault_null(b);
            return;
        }
        queue_message(e, b, port, message, NT_MESSAGE);
    }

    static void get_msg_lvo(void*, Machine& m) {
        Bus b(m);
        const u32 port = m.cpu.a[0];
        if (native::guest_address(m, port) == 0) {
            fault_null(b);
            return;
        }
        const u32 msg = get_msg(b, port);
        if (b.ok()) {
            native::set_result(m, msg);
        }
    }

    static void reply_msg_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 message = m.cpu.a[1];
        if (native::guest_address(m, message) == 0) {
            fault_null(b);
            return;
        }
        const u32 reply_port = b.r32(message + mn_ReplyPort);
        if (!b.ok()) {
            return;
        }
        if (native::guest_address(m, reply_port) == 0) {
            b.w8(message + LN_TYPE, NT_FREEMSG);
            return;
        }
        if (e.reply_handler_ != nullptr && e.reply_port_ != 0 &&
            native::guest_address(m, reply_port) == native::guest_address(m, e.reply_port_)) {
            e.reply_handler_(e.reply_ctx_, message);
            return;
        }
        queue_message(e, b, reply_port, message, NT_REPLYMSG);
    }

    static void wait_port_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 port = m.cpu.a[0];
        if (native::guest_address(m, port) == 0) {
            fault_null(b);
            return;
        }
        for (;;) {
            const u32 msg = peek_head(b, port);
            if (!b.ok()) {
                return;
            }
            if (msg != 0) {
                native::set_result(m, msg);
                return;
            }
            const u32 sig_bit = b.r8(port + mp_SigBit);
            if (!b.ok()) {
                return;
            }
            const u32 mask = sig_bit < 32 ? (1u << sig_bit) : 0;
            if (wait_signals(e, b, mask) == 0 || !b.ok()) {
                return;
            }
        }
    }

    // --- libraries -------------------------------------------------------

    static void sum_library(Bus& b, u32 lib) {
        const u32 flags = b.r8(lib + LIB_FLAGS);
        if (!b.ok() || (flags & LIBF_SUMUSED) == 0 || (flags & LIBF_SUMMING) != 0) {
            return;
        }
        b.w8(lib + LIB_FLAGS, flags & ~LIBF_CHANGED);
        u32 sum = 0;
        for (u32 off = b.r16(lib + LIB_NEGSIZE) & ~3u; off >= 4 && b.ok(); off -= 4) {
            sum += b.r32(lib - off);
        }
        if (!b.ok()) {
            return;
        }
        if ((flags & LIBF_CHANGED) == 0 && b.r32(lib + LIB_SUM) != sum) {
            LogLine().text("exec: checksum of the library at ").hex(lib).text(" changed").send();
        }
        b.w32(lib + LIB_SUM, sum);
    }

    static void add_library(Exec& e, Bus& b, u32 lib) {
        b.w8(lib + LN_TYPE, NT_LIBRARY);
        b.w8(lib + LIB_FLAGS, b.r8(lib + LIB_FLAGS) | LIBF_CHANGED);
        sum_library(b, lib);
        enqueue(b, e.base_ + LibList, lib);
    }

    // Writes JMP vectors below target from a table of addresses ended by
    // -1, or, with disp_base, of word offsets from it ended by -1 (an offset
    // of 0 leaves the vector's address alone). Returns the table size.
    static u32 make_functions(Exec& e, Bus& b, u32 target, u32 array, u32 disp_base) {
        u32 n = 0;
        for (u32 p = array;;) {
            u32 addr = 0;
            if (disp_base != 0) {
                const u32 v = b.r16(p);
                if (!b.ok() || v == 0xFFFF) {
                    break;
                }
                addr = v == 0 ? 0 : disp_base + v - ((v & 0x8000) != 0 ? 0x10000 : 0);
                p += 2;
            } else {
                addr = b.r32(p);
                if (!b.ok() || addr == 0xFFFFFFFF) {
                    break;
                }
                p += 4;
            }
            if (++n > kMaxVectors) {
                LogLine().text("exec: function table at ").hex(array).text(" has no end").send();
                end_run(*e.m_, ExitReason::Unimplemented);
                b.stop();
                return 0;
            }
            b.w16(target - LIB_VECTSIZE * n, native::kJmpAbsL);
            if (addr != 0) {
                b.w32(target - LIB_VECTSIZE * n + 2, addr);
            }
        }
        e.m_->jit->flush();
        return b.ok() ? LIB_VECTSIZE * n : 0;
    }

    // Runs the InitStruct commands at table on memory, after clearing its
    // first size bytes. A command byte is ddssnnnn: dd copies the next
    // n + 1 items (0), repeats the next one n + 1 times (1), or first moves
    // the destination to a byte (2) or 24-bit (3) offset; ss is the item
    // size, long, word or byte (3 has no meaning on the 68000).
    static void init_struct(Bus& b, u32 table, u32 memory, u32 size) {
        size &= 0xFFFF;
        if (size != 0) {
            u8* clear = b.bytes_at(memory, size);
            if (clear == nullptr) {
                return;
            }
            for (u32 i = 0; i < size; ++i) {
                clear[i] = 0;
            }
        }
        u32 it = table;
        u32 dst = memory;
        for (u32 n = 0; n < kMaxInitCommands; ++n) {
            const u32 cmd = b.r8(it);
            if (!b.ok() || cmd == 0) {
                return;
            }
            const u32 action = (cmd >> 6) & 3;
            const u32 kind = (cmd >> 4) & 3;
            const u32 count = (cmd & 15) + 1;
            if (kind == 3) {
                LogLine().text("exec: InitStruct command ").hex(cmd).text(" has no size").send();
                end_run(b.machine(), ExitReason::Unimplemented);
                b.stop();
                return;
            }
            u32 offset = 0;
            if (action == 2) {
                offset = b.r8(it + 1);
                it += 2;
            } else if (action == 3) {
                offset = b.r32(it) & 0xFFFFFF;
                it += 4;
            } else {
                it += 1;
            }
            const u32 unit = kind == 0 ? 4 : kind == 1 ? 2 : 1;
            if (unit > 1) {
                it = (it + 1) & ~1u;
                dst = (dst + 1) & ~1u;
            }
            if (action >= 2) {
                dst = memory + offset;
            }
            if (action == 1) {
                for (u32 c = 0; c < count; ++c) {
                    for (u32 i = 0; i < unit; ++i) {
                        b.w8(dst++, b.r8(it + i));
                    }
                }
                it += unit;
            } else {
                for (u32 i = 0; i < unit * count; ++i) {
                    b.w8(dst++, b.r8(it++));
                }
            }
            it = (it + 1) & ~1u;
        }
    }

    static u32 make_library(Exec& e, Bus& b, u32 vectors, u32 structure, u32 init, u32 data_size,
                            u32 seglist) {
        const bool words = b.r16(vectors) == 0xFFFF;
        const u32 array = words ? vectors + 2 : vectors;
        u32 count = 0;
        for (u32 p = array;; p += words ? 2 : 4) {
            const u32 v = words ? b.r16(p) : b.r32(p);
            if (!b.ok()) {
                return 0;
            }
            if (v == (words ? 0xFFFFu : 0xFFFFFFFFu)) {
                break;
            }
            if (++count > kMaxVectors) {
                LogLine().text("exec: function table at ").hex(vectors).text(" has no end").send();
                end_run(*e.m_, ExitReason::Unimplemented);
                b.stop();
                return 0;
            }
        }
        if (data_size > 0xFFFF) {
            return 0;  // lib_PosSize is a UWORD
        }
        const u32 neg = (count * LIB_VECTSIZE + 3) & ~3u;
        const u32 pos = data_size < LIB_SIZE ? LIB_SIZE : data_size;
        const u32 block = e.alloc(neg + pos, MEMF_PUBLIC | MEMF_CLEAR);
        if (block == 0) {
            return 0;
        }
        u32 lib = block + neg;
        make_functions(e, b, lib, array, words ? vectors : 0);
        b.w16(lib + LIB_NEGSIZE, neg);
        b.w16(lib + LIB_POSSIZE, pos);
        if (structure != 0) {
            init_struct(b, structure, lib, 0);
        }
        if (!b.ok()) {
            return 0;
        }
        if (init != 0) {
            GuestCall call(e.call_depth_, b);
            e.m_->cpu.d[0] = lib;
            e.m_->cpu.a[0] = seglist;
            e.m_->cpu.a[6] = e.base_;
            if (!call.run(init)) {
                return 0;
            }
            lib = call.d0();
        }
        return lib;
    }

    // An RTF_AUTOINIT resident points rt_Init at {data size, vectors, init
    // struct, init routine}: the library is made, given the tag's type,
    // name, version and id string, then the init routine runs with D0 = the
    // library, A0 = seglist and A6 = ExecBase, and what it returns in D0 is
    // added to the system. Any other resident's rt_Init is called the same
    // way with D0 = 0.
    static u32 init_resident(Exec& e, Bus& b, u32 res, u32 seglist) {
        if (b.r16(res + RT_MATCHWORD) != RTC_MATCHWORD || b.r32(res + RT_MATCHTAG) != res ||
            !b.ok()) {
            return 0;
        }
        const u32 init = b.r32(res + RT_INIT);
        if ((b.r8(res + RT_FLAGS) & RTF_AUTOINIT) == 0) {
            if (init == 0 || !b.ok()) {
                return 0;
            }
            GuestCall call(e.call_depth_, b);
            e.m_->cpu.d[0] = 0;
            e.m_->cpu.a[0] = seglist;
            e.m_->cpu.a[6] = e.base_;
            return call.run(init) ? call.d0() : 0;
        }
        const u32 data_size = b.r32(init);
        const u32 vectors = b.r32(init + 4);
        const u32 structure = b.r32(init + 8);
        const u32 routine = b.r32(init + 12);
        if (!b.ok()) {
            return 0;
        }
        u32 lib = make_library(e, b, vectors, structure, 0, data_size, seglist);
        if (lib == 0 || !b.ok()) {
            return 0;
        }
        const u32 type = b.r8(res + RT_TYPE);
        b.w8(lib + LN_TYPE, type);
        b.w32(lib + LN_NAME, b.r32(res + RT_NAME));
        b.w16(lib + LIB_VERSION, b.r8(res + RT_VERSION));
        b.w32(lib + LIB_IDSTRING, b.r32(res + RT_IDSTRING));
        b.w8(lib + LIB_FLAGS, LIBF_SUMUSED | LIBF_CHANGED);
        if (routine != 0) {
            GuestCall call(e.call_depth_, b);
            e.m_->cpu.d[0] = lib;
            e.m_->cpu.a[0] = seglist;
            e.m_->cpu.a[6] = e.base_;
            if (!call.run(routine)) {
                return 0;
            }
            lib = call.d0();
        }
        if (lib == 0 || !b.ok()) {
            return 0;
        }
        if (type == NT_LIBRARY) {
            add_library(e, b, lib);
        } else if (type == NT_DEVICE || type == NT_RESOURCE) {
            b.w8(lib + LIB_FLAGS, b.r8(lib + LIB_FLAGS) | LIBF_CHANGED);
            sum_library(b, lib);
            enqueue(b, e.base_ + (type == NT_DEVICE ? DeviceList : ResourceList), lib);
        }
        return b.ok() ? lib : 0;
    }

    // The first Resident in the seglist's segments: an RTC_MATCHWORD whose
    // rt_MatchTag points at itself. Plain reads: a seglist that leaves
    // guest memory just ends the search.
    static u32 find_resident(const GuestMemory& mem, u32 seglist) {
        u32 bptr = seglist;
        for (u32 n = 0; bptr != 0; ++n) {
            if (n == kMaxSegments) {
                LogLine().text("exec: seglist ").hex(seglist).text(" has more than ")
                    .dec(kMaxSegments).text(" segments").send();
                return 0;
            }
            const u64 link = static_cast<u64>(bptr) << 2;
            u32 size = 0;
            u32 next = 0;
            if (link < 4 || link + 4 > mem.size || !mem.read32(static_cast<u32>(link - 4), size) ||
                !mem.read32(static_cast<u32>(link), next)) {
                return 0;
            }
            u64 end = link - 4 + size;
            end = end > mem.size ? mem.size : end;
            for (u64 p = link + 4; p + RT_SIZE <= end; p += 2) {
                u32 word = 0;
                u32 tag = 0;
                if (mem.read16(static_cast<u32>(p), word) && word == RTC_MATCHWORD &&
                    mem.read32(static_cast<u32>(p + 2), tag) && tag == p) {
                    return static_cast<u32>(p);
                }
            }
            bptr = next;
        }
        return 0;
    }

    // Frees the segments of a seglist made by the segment loader.
    static void unload(Exec& e, u32 seglist) {
        const GuestMemory& mem = e.m_->mem;
        u32 bptr = seglist;
        for (u32 n = 0; bptr != 0; ++n) {
            if (n == kMaxSegments) {
                LogLine().text("exec: unloaded only the first ").dec(kMaxSegments)
                    .text(" segments of seglist ").hex(seglist).send();
                break;
            }
            const u64 link = static_cast<u64>(bptr) << 2;
            u32 size = 0;
            u32 next = 0;
            if (link < 4 || link + 4 > mem.size || !mem.read32(static_cast<u32>(link - 4), size) ||
                !mem.read32(static_cast<u32>(link), next)) {
                break;
            }
            e.free(static_cast<u32>(link - 4), size);
            bptr = next;
        }
        e.m_->jit->flush();
    }

    static void load_library(Exec& e, Bus& b, const char* path) {
        u32 seglist = 0;
        if (!e.loader_.load(e.loader_.ctx, path, seglist) || seglist == 0) {
            LogLine().text("exec: cannot load ").text(path).send();
            return;
        }
        e.m_->jit->flush();
        const u32 res = find_resident(e.m_->mem, seglist);
        if (res == 0) {
            LogLine().text("exec: no resident in ").text(path).send();
            unload(e, seglist);
            return;
        }
        const u32 lib = init_resident(e, b, res, seglist);
        if (!b.ok()) {
            return;
        }
        if (lib == 0) {
            LogLine().text("exec: the resident in ").text(path).text(" failed").send();
            unload(e, seglist);
            return;
        }
        for (u32& slot : e.loaded_) {
            if (slot == 0) {
                slot = seglist;
                return;
            }
        }
        LogLine().text("exec: loaded library table full, ").text(path)
            .text(" will not be unloaded").send();
    }

    static u32 open_library(Exec& e, Bus& b, u32 name_addr, u32 version) {
        char name[kMaxName];
        if (!b.string(name_addr, name, sizeof name)) {
            return 0;
        }
        const char* leaf = name;
        bool has_device = false;
        for (const char* p = name; *p != '\0'; ++p) {
            if (*p == ':' || *p == '/') {
                has_device = has_device || *p == ':';
                leaf = p + 1;
            }
        }
        const u32 list = e.base_ + LibList;
        u32 lib = find_name(b, list, leaf);
        if (lib == 0 && b.ok() && e.loader_.load != nullptr && e.load_depth_ >= kMaxLoadDepth) {
            LogLine().text("exec: libraries nested too deep to load ").text(name).send();
        } else if (lib == 0 && b.ok() && e.loader_.load != nullptr) {
            char path[kMaxName + 8];
            u32 at = 0;
            if (!has_device) {
                for (const char* p = "LIBS:"; *p != '\0'; ++p) {
                    path[at++] = *p;
                }
            }
            for (const char* p = name;; ++p) {
                path[at++] = *p;
                if (*p == '\0') {
                    break;
                }
            }
            ++e.load_depth_;
            load_library(e, b, path);
            --e.load_depth_;
            if (b.ok()) {
                lib = find_name(b, list, leaf);
            }
        }
        if (lib == 0 || !b.ok() || b.r16(lib + LIB_VERSION) < version) {
            return 0;
        }
        GuestCall call(e.call_depth_, b);
        e.m_->cpu.d[0] = version;
        e.m_->cpu.a[6] = lib;
        if (!call.run(lib - lib_lvo::Open)) {
            return 0;
        }
        return call.d0();
    }

    static void open_library_lvo(void* ctx, Machine& m) {
        Bus b(m);
        native::set_result(m, open_library(of(ctx), b, m.cpu.a[1], m.cpu.d[0] & 0xFFFF));
    }

    static void old_open_library(void* ctx, Machine& m) {
        Bus b(m);
        native::set_result(m, open_library(of(ctx), b, m.cpu.a[1], 0));
    }

    // Calls the library's Close vector. What it returns is the seglist of a
    // library that expunged itself; exec unloads it if it loaded it.
    static void close_library(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 lib = m.cpu.a[1];
        if (lib == 0) {
            native::set_result(m, 0);
            return;
        }
        GuestCall call(e.call_depth_, b);
        m.cpu.a[6] = lib;
        if (!call.run(lib - lib_lvo::Close)) {
            return;
        }
        const u32 seglist = call.d0();
        for (u32& slot : e.loaded_) {
            if (seglist != 0 && slot == seglist) {
                slot = 0;
                unload(e, seglist);
                break;
            }
        }
        native::set_result(m, seglist);
    }

    static void add_library_lvo(void* ctx, Machine& m) {
        Bus b(m);
        add_library(of(ctx), b, m.cpu.a[1]);
    }

    static void sum_library_lvo(void*, Machine& m) {
        Bus b(m);
        sum_library(b, m.cpu.a[1]);
    }

    static void make_library_lvo(void* ctx, Machine& m) {
        Bus b(m);
        const CpuState& c = m.cpu;
        native::set_result(m, make_library(of(ctx), b, c.a[0], c.a[1], c.a[2], c.d[0], c.d[1]));
    }

    static void make_functions_lvo(void* ctx, Machine& m) {
        Bus b(m);
        native::set_result(m, make_functions(of(ctx), b, m.cpu.a[0], m.cpu.a[1], m.cpu.a[2]));
    }

    static void init_struct_lvo(void*, Machine& m) {
        Bus b(m);
        init_struct(b, m.cpu.a[1], m.cpu.a[2], m.cpu.d[0]);
    }

    static void init_resident_lvo(void* ctx, Machine& m) {
        Bus b(m);
        native::set_result(m, init_resident(of(ctx), b, m.cpu.a[1], m.cpu.d[1]));
    }

    // --- semaphores ------------------------------------------------------

    static void new_min_list(Bus& b, u32 list) {
        b.w32(list + MLH_HEAD, list + MLH_TAIL);
        b.w32(list + MLH_TAIL, 0);
        b.w32(list + MLH_TAILPRED, list + MLH_HEAD);
    }

    static void init_semaphore(Exec& e, Bus& b, u32 sem_raw) {
        const u32 sem = native::guest_address(*e.m_, sem_raw);
        if (sem == 0) {
            fault_null(b);
            return;
        }
        b.w8(sem + LN_TYPE, NT_SIGNALSEM);
        b.w16(sem + SS_NESTCOUNT, 0);
        new_min_list(b, sem + SS_WAITQUEUE);
        new_min_list(b, sem + SS_MULTIPLELINK);
        b.w32(sem + SS_OWNER, 0);
        b.w16(sem + SS_QUEUECOUNT, 0xFFFF);
        if (!b.ok()) {
            return;
        }
        if (Exec::SemWaitQueue* q = e.sem_queue(sem, false)) {
            q->count = 0;
            q->addr = 0;
        }
    }

    static void obtain_semaphore(Exec& e, Bus& b, u32 sem_raw, bool shared) {
        const u32 sem = native::guest_address(*e.m_, sem_raw);
        if (sem == 0) {
            fault_null(b);
            return;
        }
        const i16 nest = static_cast<i16>(b.r16(sem + SS_NESTCOUNT));
        const u32 owner = native::guest_address(*e.m_, b.r32(sem + SS_OWNER));
        const i16 qcount = static_cast<i16>(b.r16(sem + SS_QUEUECOUNT));
        if (!b.ok()) {
            return;
        }
        const i32 cur = e.sched_.current();
        const u32 cur_addr = native::guest_address(*e.m_, e.task_);
        Exec::SemWaitQueue* existing_q = e.sem_queue(sem, false);

        if (nest == 0) {
            b.w32(sem + SS_OWNER, shared ? 0 : e.task_);
            b.w16(sem + SS_NESTCOUNT, 1);
            b.w16(sem + SS_QUEUECOUNT, static_cast<u16>(qcount + 1));
            return;
        }
        if (owner != 0 && owner == cur_addr) {
            b.w16(sem + SS_NESTCOUNT, static_cast<u16>(nest + 1));
            b.w16(sem + SS_QUEUECOUNT, static_cast<u16>(qcount + 1));
            return;
        }
        if (shared && owner == 0 && (existing_q == nullptr || existing_q->count == 0)) {
            b.w16(sem + SS_NESTCOUNT, static_cast<u16>(nest + 1));
            b.w16(sem + SS_QUEUECOUNT, static_cast<u16>(qcount + 1));
            return;
        }

        Exec::SemWaitQueue* q = e.sem_queue(sem, true);
        if (q == nullptr || q->count >= Scheduler::kMaxTasks) {
            return;
        }
        q->waiters[q->count++] = Exec::SemWaitQueue::Waiter{static_cast<u32>(cur), shared};
        b.w16(sem + SS_QUEUECOUNT, static_cast<u16>(qcount + 1));
        if (cur_addr != 0) {
            b.w8(cur_addr + TC_STATE, TS_WAIT);
            enqueue(b, e.base_ + TaskWait, cur_addr);
        }
        e.sched_.wait(cur);
        switch_away_after_block(e, b, cur, cur_addr, false);
    }

    static u32 attempt_semaphore(Exec& e, Bus& b, u32 sem_raw, bool shared) {
        const u32 sem = native::guest_address(*e.m_, sem_raw);
        if (sem == 0) {
            fault_null(b);
            return 0;
        }
        const i16 nest = static_cast<i16>(b.r16(sem + SS_NESTCOUNT));
        const u32 owner = native::guest_address(*e.m_, b.r32(sem + SS_OWNER));
        const i16 qcount = static_cast<i16>(b.r16(sem + SS_QUEUECOUNT));
        if (!b.ok()) {
            return 0;
        }
        const u32 cur_addr = native::guest_address(*e.m_, e.task_);
        Exec::SemWaitQueue* q = e.sem_queue(sem, false);

        if (nest == 0) {
            b.w32(sem + SS_OWNER, shared ? 0 : e.task_);
            b.w16(sem + SS_NESTCOUNT, 1);
            b.w16(sem + SS_QUEUECOUNT, static_cast<u16>(qcount + 1));
            return b.ok() ? 1 : 0;
        }
        if (owner != 0 && owner == cur_addr) {
            b.w16(sem + SS_NESTCOUNT, static_cast<u16>(nest + 1));
            b.w16(sem + SS_QUEUECOUNT, static_cast<u16>(qcount + 1));
            return b.ok() ? 1 : 0;
        }
        if (shared && owner == 0 && (q == nullptr || q->count == 0)) {
            b.w16(sem + SS_NESTCOUNT, static_cast<u16>(nest + 1));
            b.w16(sem + SS_QUEUECOUNT, static_cast<u16>(qcount + 1));
            return b.ok() ? 1 : 0;
        }
        return 0;
    }

    static void pop_waiter(Exec::SemWaitQueue& q) {
        for (u32 i = 1; i < q.count; ++i) {
            q.waiters[i - 1] = q.waiters[i];
        }
        --q.count;
    }

    static void wake_sem_waiter(Exec& e, Bus& b, u32 wid) {
        const u32 waddr = e.tasks_[wid].task_addr;
        if (waddr != 0) {
            remove(b, waddr);
            b.w8(waddr + TC_STATE, TS_READY);
        }
        e.sched_.wake(static_cast<i32>(wid));
        if (waddr != 0) {
            enqueue(b, e.base_ + TaskReady, waddr);
        }
    }

    static void release_semaphore(Exec& e, Bus& b, u32 sem_raw) {
        const u32 sem = native::guest_address(*e.m_, sem_raw);
        if (sem == 0) {
            fault_null(b);
            return;
        }
        i16 nest = static_cast<i16>(b.r16(sem + SS_NESTCOUNT));
        const i16 qcount = static_cast<i16>(b.r16(sem + SS_QUEUECOUNT));
        if (!b.ok() || nest <= 0) {
            return;
        }
        --nest;
        b.w16(sem + SS_NESTCOUNT, static_cast<u16>(nest));
        b.w16(sem + SS_QUEUECOUNT, static_cast<u16>(qcount - 1));
        if (nest > 0 || !b.ok()) {
            return;
        }

        Exec::SemWaitQueue* q = e.sem_queue(sem, false);
        if (q == nullptr || q->count == 0) {
            b.w32(sem + SS_OWNER, 0);
            if (q != nullptr) {
                q->addr = 0;
            }
            return;
        }

        if (!q->waiters[0].shared) {
            const u32 wid = q->waiters[0].id;
            pop_waiter(*q);
            if (q->count == 0) {
                q->addr = 0;
            }
            b.w32(sem + SS_OWNER, e.tasks_[wid].task_addr);
            b.w16(sem + SS_NESTCOUNT, 1);
            wake_sem_waiter(e, b, wid);
        } else {
            u32 woken = 0;
            b.w32(sem + SS_OWNER, 0);
            while (q->count > 0 && q->waiters[0].shared) {
                const u32 wid = q->waiters[0].id;
                pop_waiter(*q);
                ++woken;
                wake_sem_waiter(e, b, wid);
            }
            b.w16(sem + SS_NESTCOUNT, static_cast<u16>(woken));
            if (q->count == 0) {
                q->addr = 0;
            }
        }
        if (e.sched_.should_switch_now()) {
            e.pending_switch_ = true;
        }
    }

    static void init_semaphore_lvo(void* ctx, Machine& m) {
        Bus b(m);
        init_semaphore(of(ctx), b, m.cpu.a[0]);
    }

    static void obtain_semaphore_lvo(void* ctx, Machine& m) {
        Bus b(m);
        obtain_semaphore(of(ctx), b, m.cpu.a[0], false);
    }

    static void release_semaphore_lvo(void* ctx, Machine& m) {
        Bus b(m);
        release_semaphore(of(ctx), b, m.cpu.a[0]);
    }

    static void attempt_semaphore_lvo(void* ctx, Machine& m) {
        Bus b(m);
        const u32 got = attempt_semaphore(of(ctx), b, m.cpu.a[0], false);
        if (b.ok()) {
            native::set_result(m, got);
        }
    }

    static void obtain_semaphore_shared_lvo(void* ctx, Machine& m) {
        Bus b(m);
        obtain_semaphore(of(ctx), b, m.cpu.a[0], true);
    }

    static void attempt_semaphore_shared_lvo(void* ctx, Machine& m) {
        Bus b(m);
        const u32 got = attempt_semaphore(of(ctx), b, m.cpu.a[0], true);
        if (b.ok()) {
            native::set_result(m, got);
        }
    }

    static void obtain_semaphore_list_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 list = native::guest_address(m, m.cpu.a[0]);
        if (list == 0) {
            fault_null(b);
            return;
        }
        for (u32 attempt = 0; attempt < kMaxListNodes; ++attempt) {
            u32 acquired[64]{};
            u32 acq_count = 0;
            u32 failed_sem = 0;
            u32 n = b.r32(list + LH_HEAD);
            for (u32 count = 0;; ++count) {
                if (count == kMaxListNodes) {
                    log_endless_list(list);
                    break;
                }
                const u32 succ = b.r32(n + LN_SUCC);
                if (!b.ok() || succ == 0) {
                    break;
                }
                if (attempt_semaphore(e, b, n, false) != 0) {
                    if (acq_count < 64) {
                        acquired[acq_count++] = n;
                    }
                } else {
                    failed_sem = n;
                    break;
                }
                n = succ;
            }
            if (!b.ok()) {
                return;
            }
            if (failed_sem == 0) {
                return;
            }
            for (u32 i = acq_count; i > 0; --i) {
                release_semaphore(e, b, acquired[i - 1]);
            }
            obtain_semaphore(e, b, failed_sem, false);
            if (!b.ok()) {
                return;
            }
            release_semaphore(e, b, failed_sem);
            if (!b.ok()) {
                return;
            }
        }
    }

    static void release_semaphore_list_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 list = native::guest_address(m, m.cpu.a[0]);
        if (list == 0) {
            fault_null(b);
            return;
        }
        u32 n = b.r32(list + LH_HEAD);
        for (u32 count = 0;; ++count) {
            if (count == kMaxListNodes) {
                log_endless_list(list);
                return;
            }
            const u32 succ = b.r32(n + LN_SUCC);
            if (!b.ok() || succ == 0) {
                return;
            }
            release_semaphore(e, b, n);
            n = succ;
        }
    }

    static void find_semaphore_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 name = m.cpu.a[1];
        if (native::guest_address(m, name) == 0) {
            fault_null(b);
            return;
        }
        b.r8(name);
        if (!b.ok()) {
            return;
        }
        const u32 found =
            find_node(b, e.base_ + SemaphoreList, [&](u32 n) { return b.equals(n, name); });
        if (b.ok()) {
            native::set_result(m, found);
        }
    }

    static void add_semaphore_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 sem = m.cpu.a[1];
        if (native::guest_address(m, sem) == 0) {
            fault_null(b);
            return;
        }
        init_semaphore(e, b, sem);
        if (b.ok()) {
            enqueue(b, e.base_ + SemaphoreList, sem);
        }
    }

    static void rem_semaphore_lvo(void*, Machine& m) {
        Bus b(m);
        const u32 sem = m.cpu.a[1];
        if (native::guest_address(m, sem) == 0) {
            fault_null(b);
            return;
        }
        remove(b, sem);
    }

    // --- RawDoFmt --------------------------------------------------------

    // Formats as native::raw_format describes. Each character goes to the
    // routine in A2, or into the buffer in A3 when A2 is 0, the closing NUL
    // too. D0 returns the data stream pointer past the last argument used.
    static void raw_do_fmt(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        u32 data = m.cpu.a[1];
        FormatOutput out{e.call_depth_, b, m.cpu.a[2], m.cpu.a[3]};
        if (native::raw_format(b, m.cpu.a[0], data, out) && out.put(0)) {
            native::set_result(m, data);
        }
    }
};

namespace {

struct ExecFunction {
    u32 lvo;
    m68k::NativeFn fn;
    const char* name;
};

constexpr ExecFunction kExecFunctions[] = {
    {exec_lvo::InitStruct, ExecOps::init_struct_lvo, "InitStruct"},
    {exec_lvo::MakeLibrary, ExecOps::make_library_lvo, "MakeLibrary"},
    {exec_lvo::MakeFunctions, ExecOps::make_functions_lvo, "MakeFunctions"},
    {exec_lvo::InitResident, ExecOps::init_resident_lvo, "InitResident"},
    {exec_lvo::Disable, ExecOps::disable, "Disable"},
    {exec_lvo::Enable, ExecOps::enable, "Enable"},
    {exec_lvo::Forbid, ExecOps::forbid, "Forbid"},
    {exec_lvo::Permit, ExecOps::permit, "Permit"},
    {exec_lvo::AllocMem, ExecOps::alloc_mem, "AllocMem"},
    {exec_lvo::FreeMem, ExecOps::free_mem, "FreeMem"},
    {exec_lvo::AvailMem, ExecOps::avail_mem, "AvailMem"},
    {exec_lvo::Insert, ExecOps::insert_node, "Insert"},
    {exec_lvo::AddHead, ExecOps::add_head_node, "AddHead"},
    {exec_lvo::AddTail, ExecOps::add_tail_node, "AddTail"},
    {exec_lvo::Remove, ExecOps::remove_node, "Remove"},
    {exec_lvo::RemHead, ExecOps::rem_head_node, "RemHead"},
    {exec_lvo::RemTail, ExecOps::rem_tail_node, "RemTail"},
    {exec_lvo::Enqueue, ExecOps::enqueue_node, "Enqueue"},
    {exec_lvo::FindName, ExecOps::find_name_node, "FindName"},
    {exec_lvo::AddTask, ExecOps::add_task_lvo, "AddTask"},
    {exec_lvo::RemTask, ExecOps::rem_task_lvo, "RemTask"},
    {exec_lvo::FindTask, ExecOps::find_task, "FindTask"},
    {exec_lvo::SetTaskPri, ExecOps::set_task_pri_lvo, "SetTaskPri"},
    {exec_lvo::SetSignal, ExecOps::set_signal, "SetSignal"},
    {exec_lvo::Wait, ExecOps::wait, "Wait"},
    {exec_lvo::Signal, ExecOps::signal_lvo, "Signal"},
    {exec_lvo::AllocSignal, ExecOps::alloc_signal_lvo, "AllocSignal"},
    {exec_lvo::FreeSignal, ExecOps::free_signal_lvo, "FreeSignal"},
    {exec_lvo::AddPort, ExecOps::add_port_lvo, "AddPort"},
    {exec_lvo::RemPort, ExecOps::rem_port_lvo, "RemPort"},
    {exec_lvo::PutMsg, ExecOps::put_msg_lvo, "PutMsg"},
    {exec_lvo::GetMsg, ExecOps::get_msg_lvo, "GetMsg"},
    {exec_lvo::ReplyMsg, ExecOps::reply_msg_lvo, "ReplyMsg"},
    {exec_lvo::WaitPort, ExecOps::wait_port_lvo, "WaitPort"},
    {exec_lvo::FindPort, ExecOps::find_port_lvo, "FindPort"},
    {exec_lvo::AddLibrary, ExecOps::add_library_lvo, "AddLibrary"},
    {exec_lvo::OldOpenLibrary, ExecOps::old_open_library, "OldOpenLibrary"},
    {exec_lvo::CloseLibrary, ExecOps::close_library, "CloseLibrary"},
    {exec_lvo::SumLibrary, ExecOps::sum_library_lvo, "SumLibrary"},
    {exec_lvo::RawDoFmt, ExecOps::raw_do_fmt, "RawDoFmt"},
    {exec_lvo::OpenLibrary, ExecOps::open_library_lvo, "OpenLibrary"},
    {exec_lvo::InitSemaphore, ExecOps::init_semaphore_lvo, "InitSemaphore"},
    {exec_lvo::ObtainSemaphore, ExecOps::obtain_semaphore_lvo, "ObtainSemaphore"},
    {exec_lvo::ReleaseSemaphore, ExecOps::release_semaphore_lvo, "ReleaseSemaphore"},
    {exec_lvo::AttemptSemaphore, ExecOps::attempt_semaphore_lvo, "AttemptSemaphore"},
    {exec_lvo::ObtainSemaphoreList, ExecOps::obtain_semaphore_list_lvo, "ObtainSemaphoreList"},
    {exec_lvo::ReleaseSemaphoreList, ExecOps::release_semaphore_list_lvo, "ReleaseSemaphoreList"},
    {exec_lvo::FindSemaphore, ExecOps::find_semaphore_lvo, "FindSemaphore"},
    {exec_lvo::AddSemaphore, ExecOps::add_semaphore_lvo, "AddSemaphore"},
    {exec_lvo::RemSemaphore, ExecOps::rem_semaphore_lvo, "RemSemaphore"},
    {exec_lvo::CopyMem, ExecOps::copy_mem, "CopyMem"},
    {exec_lvo::CopyMemQuick, ExecOps::copy_mem, "CopyMemQuick"},
    {exec_lvo::CacheClearU, ExecOps::cache_clear, "CacheClearU"},
    {exec_lvo::CacheClearE, ExecOps::cache_clear, "CacheClearE"},
    {exec_lvo::CreateMsgPort, ExecOps::create_msg_port_lvo, "CreateMsgPort"},
    {exec_lvo::DeleteMsgPort, ExecOps::delete_msg_port_lvo, "DeleteMsgPort"},
    {exec_lvo::ObtainSemaphoreShared, ExecOps::obtain_semaphore_shared_lvo,
     "ObtainSemaphoreShared"},
    {exec_lvo::AllocVec, ExecOps::alloc_vec, "AllocVec"},
    {exec_lvo::FreeVec, ExecOps::free_vec, "FreeVec"},
    {exec_lvo::AttemptSemaphoreShared, ExecOps::attempt_semaphore_shared_lvo,
     "AttemptSemaphoreShared"},
};

}

bool Exec::init(Machine& m, u32 free_start, u32 free_end) {
    // The task, the segment loader, the idle hook and the task host may be
    // set before init; they stay.
    const u32 task = task_;
    const SegmentLoader loader = loader_;
    const IdleHook idle = idle_;
    const TaskHost task_host = task_host_;
    *this = Exec{};
    task_ = task;
    loader_ = loader;
    idle_ = idle;
    task_host_ = task_host;
    if (m.jit == nullptr || m.mem.base == nullptr || free_start < kLowestFree ||
        free_end <= free_start || free_end > m.mem.size) {
        return false;
    }
    const u64 start = (static_cast<u64>(free_start) + 7) & ~static_cast<u64>(7);
    const u32 end = free_end & ~7u;
    if (start + MH_SIZE + kExecBlock + MC_SIZE > end) {
        return false;
    }
    m_ = &m;
    header_ = static_cast<u32>(start);
    lower_ = header_ + MH_SIZE;
    upper_ = end;
    free_bytes_ = upper_ - lower_;

    Bus b(m);
    for (u32 i = 0; i < MH_SIZE; ++i) {
        b.w8(header_ + i, 0);
    }
    b.w8(header_ + LN_TYPE, NT_MEMORY);
    b.w16(header_ + MH_ATTRIBUTES, MEMF_PUBLIC | MEMF_CHIP | MEMF_FAST);
    b.w32(header_ + MH_FIRST, lower_);
    b.w32(header_ + MH_LOWER, lower_);
    b.w32(header_ + MH_UPPER, upper_);
    b.w32(header_ + MH_FREE, free_bytes_);
    b.w32(lower_ + MC_NEXT, 0);
    b.w32(lower_ + MC_BYTES, free_bytes_);

    const u32 block = alloc(kExecBlock, MEMF_PUBLIC | MEMF_CLEAR);
    if (block == 0 || !b.ok()) {
        m_ = nullptr;
        return false;
    }
    base_ = block + kExecNegSize;
    libraries_[0] = LibraryInfo{this, base_, kExecName};
    library_count_ = 1;

    m68k::Jit& jit = *m.jit;
    const u16 trap = jit.add_native(ExecOps::unimplemented, &libraries_[0], "exec unimplemented");
    open_id_ = jit.add_native(ExecOps::lib_open, this, "Open");
    close_id_ = jit.add_native(ExecOps::lib_close, this, "Close");
    expunge_id_ = jit.add_native(ExecOps::lib_null, this, "Expunge");
    reserved_id_ = jit.add_native(ExecOps::lib_null, this, "Reserved");
    if (trap == 0 || open_id_ == 0 || close_id_ == 0 || expunge_id_ == 0 || reserved_id_ == 0) {
        LogLine().text("exec: no native ids left for ").text(kExecName).send();
        m_ = nullptr;
        return false;
    }
    native::fill_with_native(b, base_, kExecNegSize, trap);
    native::put_native_vector(b, base_, lib_lvo::Open, open_id_);
    native::put_native_vector(b, base_, lib_lvo::Close, close_id_);
    native::put_native_vector(b, base_, lib_lvo::Expunge, expunge_id_);
    native::put_native_vector(b, base_, lib_lvo::Reserved, reserved_id_);
    for (const ExecFunction& f : kExecFunctions) {
        const u16 id = jit.add_native(f.fn, this, f.name);
        if (id == 0) {
            LogLine().text("exec: no native ids left for ").text(kExecName).send();
            m_ = nullptr;
            return false;
        }
        native::put_native_vector(b, base_, f.lvo, id);
    }

    const u32 name = base_ + SYSBASESIZE;
    const u32 id_string = name + sizeof kExecName;
    const u32 memory_name = id_string + sizeof kExecIdString;
    task_exit_stub_ = (memory_name + sizeof kMemoryName + 1) & ~1u;
    put_string(b, name, kExecName);
    put_string(b, id_string, kExecIdString);
    put_string(b, memory_name, kMemoryName);
    const u16 exit_id = jit.add_native(ExecOps::task_exit_stub, this, "TaskExit");
    if (exit_id == 0) {
        LogLine().text("exec: no native ids left for ").text(kExecName).send();
        m_ = nullptr;
        return false;
    }
    b.w16(task_exit_stub_, native::kALine | exit_id);
    b.w16(task_exit_stub_ + 2, native::kRts);

    b.w8(base_ + LN_TYPE, NT_LIBRARY);
    b.w32(base_ + LN_NAME, name);
    b.w8(base_ + LIB_FLAGS, LIBF_SUMUSED);
    b.w16(base_ + LIB_NEGSIZE, kExecNegSize);
    b.w16(base_ + LIB_POSSIZE, SYSBASESIZE);
    b.w16(base_ + LIB_VERSION, kHombreLibVersion);
    b.w16(base_ + LIB_REVISION, kHombreLibRevision);
    b.w32(base_ + LIB_IDSTRING, id_string);
    b.w16(base_ + SoftVer, kHombreLibRevision);  // SoftVer is the revision
    b.w8(base_ + IDNestCnt, 0xFF);
    b.w8(base_ + TDNestCnt, 0xFF);
    b.w16(base_ + AttnFlags, 0);  // a plain 68000
    b.w8(base_ + VBlankFrequency, 50);
    b.w8(base_ + PowerSupplyFrequency, 50);
    b.w32(base_ + ex_EClockFrequency, 709379);
    new_list(b, base_ + MemList, NT_MEMORY);
    new_list(b, base_ + ResourceList, NT_RESOURCE);
    new_list(b, base_ + DeviceList, NT_DEVICE);
    new_list(b, base_ + IntrList, 0);
    new_list(b, base_ + LibList, NT_LIBRARY);
    new_list(b, base_ + PortList, NT_MSGPORT);
    new_list(b, base_ + TaskReady, NT_TASK);
    new_list(b, base_ + TaskWait, NT_TASK);
    new_list(b, base_ + SemaphoreList, 0);

    b.w32(header_ + LN_NAME, memory_name);
    add_tail(b, base_ + MemList, header_);
    ExecOps::add_library(*this, b, base_);
    b.w32(base_ + ThisTask, task_);
    b.w32(4, base_);
    sched_.init(task_);
    tasks_[0].task_addr = task_;
    if (task_ != 0) {
        b.w8(task_ + TC_STATE, TS_RUN);
        b.w8(task_ + TC_TDNESTCNT, 0xFF);
        b.w8(task_ + TC_IDNESTCNT, 0xFF);
    }
    jit.set_preempt_hook(&ExecOps::preempt_hook, this);
    jit.flush();
    if (!b.ok()) {
        m_ = nullptr;
        return false;
    }
    return true;
}

u32 Exec::alloc(u32 bytes, u32 flags) {
    if (m_ == nullptr || bytes == 0 || bytes > upper_ - lower_) {
        return 0;
    }
    const u32 size = round8(bytes);
    GuestMemory& mem = m_->mem;
    u32 link = header_ + MH_FIRST;
    u32 chunk = 0;
    u32 floor = lower_;
    mem.read32(link, chunk);
    while (chunk != 0) {
        u32 next = 0;
        u32 avail = 0;
        if (!read_chunk(mem, chunk, floor, upper_, next, avail)) {
            log_corrupt_memory_list();
            return 0;
        }
        if (avail >= size) {
            if (avail == size) {
                mem.write32(link, next);
            } else {
                const u32 rest = chunk + size;
                mem.write32(rest + MC_NEXT, next);
                mem.write32(rest + MC_BYTES, avail - size);
                mem.write32(link, rest);
            }
            free_bytes_ = free_bytes_ >= size ? free_bytes_ - size : 0;
            mem.write32(header_ + MH_FREE, free_bytes_);
            if ((flags & MEMF_CLEAR) != 0) {
                for (u32 i = 0; i < size; ++i) {
                    mem.base[chunk + i] = 0;
                }
            }
            return chunk;
        }
        floor = chunk + avail;
        link = chunk + MC_NEXT;
        chunk = next;
    }
    return 0;
}

void Exec::free(u32 addr, u32 bytes) {
    if (m_ == nullptr) {
        return;
    }
    addr = native::guest_address(*m_, addr);
    if (addr == 0 || bytes == 0) {
        return;
    }
    if ((addr & 7) != 0 || bytes > upper_ - lower_ || addr < lower_ ||
        static_cast<u64>(addr) + round8(bytes) > upper_) {
        LogLine().text("exec: FreeMem of a bad block at ").hex(addr).text(", ").dec(bytes)
            .text(" bytes").send();
        return;
    }
    const u32 size = round8(bytes);
    GuestMemory& mem = m_->mem;

    // The last free chunk below the block (prev) and the first at or above it.
    u32 link = header_ + MH_FIRST;
    u32 prev = 0;
    u32 prev_bytes = 0;
    u32 chunk = 0;
    u32 chunk_next = 0;
    u32 chunk_bytes = 0;
    u32 floor = lower_;
    mem.read32(link, chunk);
    while (chunk != 0) {
        if (!read_chunk(mem, chunk, floor, upper_, chunk_next, chunk_bytes)) {
            log_corrupt_memory_list();
            return;
        }
        if (chunk >= addr) {
            break;
        }
        prev = chunk;
        prev_bytes = chunk_bytes;
        floor = chunk + chunk_bytes;
        link = chunk + MC_NEXT;
        chunk = chunk_next;
    }
    if ((prev != 0 && prev + prev_bytes > addr) || (chunk != 0 && addr + size > chunk)) {
        LogLine().text("exec: FreeMem of a free block at ").hex(addr).text(", ").dec(bytes)
            .text(" bytes").send();
        return;
    }
    u32 merged = size;
    u32 after = chunk;
    if (chunk != 0 && addr + size == chunk) {
        merged += chunk_bytes;
        after = chunk_next;
    }
    if (prev != 0 && prev + prev_bytes == addr) {
        mem.write32(prev + MC_BYTES, prev_bytes + merged);
        mem.write32(prev + MC_NEXT, after);
    } else {
        mem.write32(addr + MC_NEXT, after);
        mem.write32(addr + MC_BYTES, merged);
        mem.write32(link, addr);
    }
    free_bytes_ += size;
    mem.write32(header_ + MH_FREE, free_bytes_);
}

u32 Exec::make_native_library(const char* name, u16 version, const NativeVector* fns, u32 count,
                              void* ctx, u32 pos_size) {
    if (m_ == nullptr || name == nullptr || (count != 0 && fns == nullptr) || pos_size > 0xFFFF) {
        return 0;
    }
    const u32 pos = pos_size < LIB_SIZE ? LIB_SIZE : pos_size;
    if (library_count_ == kMaxLibraries) {
        LogLine().text("exec: native library table full, ").text(name).text(" not made").send();
        return 0;
    }
    if (count > kMaxVectors) {
        LogLine().text("exec: ").text(name).text(" has more than ").dec(kMaxVectors)
            .text(" vectors").send();
        return 0;
    }
    u32 name_bytes = 0;
    while (name[name_bytes] != '\0') {
        if (++name_bytes == kMaxName) {
            LogLine().text("exec: library name longer than ").dec(kMaxName - 1)
                .text(" bytes").send();
            return 0;
        }
    }
    ++name_bytes;
    const u32 vectors = count < 4 ? 4 : count;
    const u32 neg = (vectors * LIB_VECTSIZE + 3) & ~3u;
    const u32 block = alloc(neg + pos + name_bytes, MEMF_PUBLIC | MEMF_CLEAR);
    if (block == 0) {
        return 0;
    }
    const u32 lib = block + neg;
    LibraryInfo& info = libraries_[library_count_];
    info = LibraryInfo{this, lib, name};
    m68k::Jit& jit = *m_->jit;
    Bus b(*m_);
    const u16 trap = jit.add_native(ExecOps::unimplemented, &info, name);
    if (trap == 0) {
        LogLine().text("exec: no native ids left for ").text(name).send();
        free(block, neg + pos + name_bytes);
        return 0;
    }
    native::fill_with_native(b, lib, neg, trap);
    const u16 standard[] = {open_id_, close_id_, expunge_id_, reserved_id_};
    for (u32 n = 1; n <= vectors; ++n) {
        const NativeVector* v = n <= count ? &fns[n - 1] : nullptr;
        if (v != nullptr && v->fn != nullptr) {
            const u16 id = jit.add_native(v->fn, ctx, v->name != nullptr ? v->name : name);
            if (id == 0) {
                LogLine().text("exec: no native ids left for ").text(name).send();
                free(block, neg + pos + name_bytes);
                return 0;
            }
            native::put_native_vector(b, lib, LIB_VECTSIZE * n, id);
        } else if (n <= 4) {
            native::put_native_vector(b, lib, LIB_VECTSIZE * n, standard[n - 1]);
        }
    }
    put_string(b, lib + pos, name);
    b.w32(lib + LN_NAME, lib + pos);
    b.w8(lib + LIB_FLAGS, LIBF_SUMUSED);
    b.w16(lib + LIB_NEGSIZE, neg);
    b.w16(lib + LIB_POSSIZE, pos);
    b.w16(lib + LIB_VERSION, version);
    ExecOps::add_library(*this, b, lib);
    jit.flush();
    if (!b.ok()) {
        return 0;
    }
    ++library_count_;
    return lib;
}

void Exec::set_this_task(u32 task_addr) {
    task_ = task_addr;
    if (m_ != nullptr) {
        m_->mem.write32(base_ + ThisTask, task_addr);
        tasks_[0].task_addr = task_addr;
        sched_.set_task_addr(0, task_addr);
        if (task_addr != 0) {
            Bus b(*m_);
            b.w8(task_addr + TC_STATE, TS_RUN);
            b.w8(task_addr + TC_TDNESTCNT, b.r8(base_ + TDNestCnt));
            b.w8(task_addr + TC_IDNESTCNT, b.r8(base_ + IDNestCnt));
        }
    }
}

u32 Exec::create_port() {
    if (m_ == nullptr) {
        return 0;
    }
    const u32 port = alloc(MP_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if (port == 0) {
        return 0;
    }
    Bus b(*m_);
    const u32 sig = ExecOps::alloc_signal(*this, b, 0xFFFFFFFF);
    if (sig >= 32 || !b.ok()) {
        free(port, MP_SIZE);
        return 0;
    }
    b.w8(port + LN_TYPE, NT_MSGPORT);
    b.w8(port + mp_Flags, PA_SIGNAL);
    b.w8(port + mp_SigBit, sig);
    b.w32(port + mp_SigTask, task_);
    new_list(b, port + mp_MsgList, NT_MESSAGE);
    if (!b.ok()) {
        ExecOps::free_signal(*this, b, sig);
        free(port, MP_SIZE);
        return 0;
    }
    port_signals_ |= 1u << sig;
    return port;
}

void Exec::delete_port(u32 port) {
    if (m_ == nullptr || native::guest_address(*m_, port) == 0) {
        return;
    }
    Bus b(*m_);
    const u32 sig = b.r8(port + mp_SigBit);
    if (!b.ok()) {
        return;
    }
    if (sig < 32) {
        port_signals_ &= ~(1u << sig);
    }
    ExecOps::free_signal(*this, b, sig);
    free(port, MP_SIZE);
}

bool Exec::post_message(u32 port, u32 message) {
    if (m_ == nullptr || native::guest_address(*m_, port) == 0 ||
        native::guest_address(*m_, message) == 0) {
        return false;
    }
    Bus b(*m_);
    return ExecOps::queue_message(*this, b, port, message, NT_MESSAGE);
}

u32 Exec::get_message(u32 port) {
    if (m_ == nullptr || native::guest_address(*m_, port) == 0) {
        return 0;
    }
    Bus b(*m_);
    return ExecOps::get_msg(b, port);
}

void Exec::signal_current_task(u32 mask) {
    if (m_ == nullptr) {
        return;
    }
    Bus b(*m_);
    if (tasks_[0].task_addr != 0) {
        ExecOps::signal_task(*this, b, tasks_[0].task_addr, mask);
    } else {
        signals_ |= mask;
    }
}

void Exec::reset_task_signals() {
    sig_alloc_ = 0x0000FFFF;
    signals_ = 0;
    port_signals_ = 0;
    every_task_waits_logged_ = false;
    abort_to_boot_task_ = false;
    const u32 boot_addr = tasks_[0].task_addr != 0 ? tasks_[0].task_addr : task_;
    if (m_ != nullptr && boot_addr != 0) {
        Bus b(*m_);
        b.w32(boot_addr + TC_SIGALLOC, 0x0000FFFF);
        b.w32(boot_addr + TC_SIGRECVD, 0);
    }
}

void Exec::set_reply_handler(u32 port, void (*handler)(void* ctx, u32 message), void* ctx) {
    reply_port_ = port;
    reply_handler_ = handler;
    reply_ctx_ = ctx;
}

bool Exec::forbidden() const {
    if (m_ == nullptr || base_ == 0) {
        return true;
    }
    u32 td = 0;
    u32 id = 0;
    if (!m_->mem.read8(base_ + TDNestCnt, td) || !m_->mem.read8(base_ + IDNestCnt, id)) {
        return true;
    }
    return td != 0xFF || id != 0xFF;
}

void Exec::maybe_preempt() {
    if (task_host_.switch_to == nullptr || forbidden()) {
        return;
    }
    bool do_switch = false;
    if (pending_switch_) {
        pending_switch_ = false;
        if (sched_.should_switch_now()) {
            do_switch = true;
        }
    }
    if (!do_switch && task_host_.slice_over != nullptr && task_host_.slice_over(task_host_.ctx)) {
        if (sched_.should_preempt()) {
            do_switch = true;
        }
    }
    if (do_switch) {
        const i32 next = sched_.pick_next();
        if (next >= 0) {
            switch_to_task(next);
        }
    }
}

void Exec::switch_to_task(i32 next_id) {
    const i32 from = sched_.current();
    if (from < 0 || next_id < 0 || from == next_id ||
        static_cast<u32>(from) >= Scheduler::kMaxTasks ||
        static_cast<u32>(next_id) >= Scheduler::kMaxTasks) {
        return;
    }
    Bus b(*m_);
    const u32 from_addr = tasks_[from].task_addr;
    const u32 to_addr = tasks_[next_id].task_addr;

    tasks_[from].cpu = m_->cpu;
    tasks_[from].run_state = m_->jit->save_run_state();
    tasks_[from].call_depth = call_depth_;
    const u8 td_nest = static_cast<u8>(b.r8(base_ + TDNestCnt));
    const u8 id_nest = static_cast<u8>(b.r8(base_ + IDNestCnt));
    tasks_[from].td_nest = td_nest;
    tasks_[from].id_nest = id_nest;
    if (from_addr != 0) {
        b.w8(from_addr + TC_TDNESTCNT, td_nest);
        b.w8(from_addr + TC_IDNESTCNT, id_nest);
    }

    sched_.switch_to(next_id);
    if (sched_.slot(static_cast<u32>(from)).state == TaskState::Ready && from_addr != 0) {
        b.w8(from_addr + TC_STATE, TS_READY);
        enqueue(b, base_ + TaskReady, from_addr);
    }
    if (to_addr != 0) {
        remove(b, to_addr);
        b.w8(to_addr + TC_STATE, TS_RUN);
    }

    task_ = to_addr;
    b.w32(base_ + ThisTask, to_addr);
    if (to_addr != 0) {
        b.w8(base_ + TDNestCnt, b.r8(to_addr + TC_TDNESTCNT));
        b.w8(base_ + IDNestCnt, b.r8(to_addr + TC_IDNESTCNT));
    } else {
        b.w8(base_ + TDNestCnt, tasks_[next_id].td_nest);
        b.w8(base_ + IDNestCnt, tasks_[next_id].id_nest);
    }

    if (task_host_.switch_to != nullptr) {
        task_host_.switch_to(task_host_.ctx, static_cast<u32>(from), static_cast<u32>(next_id));
    }

    if (sched_.current() == from) {
        reap_removed();
        m_->cpu = tasks_[from].cpu;
        m_->jit->restore_run_state(tasks_[from].run_state);
        call_depth_ = tasks_[from].call_depth;
        task_ = tasks_[from].task_addr;
        Bus rb(*m_);
        rb.w32(base_ + ThisTask, task_);
        if (task_ != 0) {
            rb.w8(base_ + TDNestCnt, rb.r8(task_ + TC_TDNESTCNT));
            rb.w8(base_ + IDNestCnt, rb.r8(task_ + TC_IDNESTCNT));
        } else {
            rb.w8(base_ + TDNestCnt, tasks_[from].td_nest);
            rb.w8(base_ + IDNestCnt, tasks_[from].id_nest);
        }
    }
}

void Exec::reap_removed() {
    for (u32 i = 0; i < Scheduler::kMaxTasks; ++i) {
        if (sched_.slot(i).state == TaskState::Removed &&
            static_cast<i32>(i) != sched_.current()) {
            if (task_host_.destroy != nullptr) {
                task_host_.destroy(task_host_.ctx, i);
            }
            sched_.release(static_cast<i32>(i));
        }
    }
}

void Exec::remove_sem_waiter(u32 task_id) {
    for (SemWaitQueue& q : sem_queues_) {
        if (q.addr == 0 || q.count == 0) {
            continue;
        }
        for (u32 i = 0; i < q.count;) {
            if (q.waiters[i].id == task_id) {
                for (u32 j = i + 1; j < q.count; ++j) {
                    q.waiters[j - 1] = q.waiters[j];
                }
                --q.count;
                if (m_ != nullptr) {
                    u32 qc = 0;
                    if (m_->mem.read16(q.addr + SS_QUEUECOUNT, qc)) {
                        m_->mem.write16(q.addr + SS_QUEUECOUNT, static_cast<u16>(qc - 1));
                    }
                }
            } else {
                ++i;
            }
        }
        if (q.count == 0) {
            q.addr = 0;
        }
    }
}

Exec::SemWaitQueue* Exec::sem_queue(u32 sem_addr, bool create) {
    SemWaitQueue* free_slot = nullptr;
    for (SemWaitQueue& q : sem_queues_) {
        if (q.addr == sem_addr) {
            return &q;
        }
        if (q.addr == 0 && free_slot == nullptr) {
            free_slot = &q;
        }
    }
    if (!create || free_slot == nullptr) {
        return nullptr;
    }
    free_slot->addr = sem_addr;
    free_slot->count = 0;
    return free_slot;
}

void Exec::task_main(u32 id) {
    if (m_ == nullptr || id >= Scheduler::kMaxTasks) {
        return;
    }
    reap_removed();
    m_->cpu = tasks_[id].cpu;
    m_->jit->restore_run_state(tasks_[id].run_state);
    call_depth_ = tasks_[id].call_depth;
    task_ = tasks_[id].task_addr;
    Bus b(*m_);
    b.w32(base_ + ThisTask, task_);
    if (task_ != 0) {
        b.w8(base_ + TDNestCnt, b.r8(task_ + TC_TDNESTCNT));
        b.w8(base_ + IDNestCnt, b.r8(task_ + TC_IDNESTCNT));
    } else {
        b.w8(base_ + TDNestCnt, tasks_[id].td_nest);
        b.w8(base_ + IDNestCnt, tasks_[id].id_nest);
    }

    const ExitReason why = m_->jit->run(tasks_[id].budget);
    LogLine().text("exec: task ended with ").text(m68k::exit_reason_name(why)).send();
    Bus eb(*m_);
    ExecOps::rem_task_id(*this, eb, static_cast<i32>(id));
}

void Exec::yield_if_ready() {
    if (m_ == nullptr || task_host_.switch_to == nullptr || forbidden()) {
        return;
    }
    const i32 next = sched_.pick_next();
    if (next >= 0) {
        switch_to_task(next);
    }
}

}
