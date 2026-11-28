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

#include <algorithm>

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
constexpr u32 kTaskMemEntry = 74;  // tc_MemEntry, which dos.hpp names
constexpr u32 kMaxMemLists = 4096;
// The MemEntries RemTask takes from a task's tc_MemEntry, at most.
constexpr u32 kMaxTaskMemEntries = 4096;
constexpr char kExecName[] = "exec.library";
constexpr char kCommodities[] = "commodities.library";
constexpr char kExecIdString[] = "exec 60.0 (Hombre)\r\n";
constexpr char kChipName[] = "chip memory";
constexpr char kFastName[] = "expansion memory";
constexpr u32 kMemoryTypes = MEMF_PUBLIC | MEMF_CHIP | MEMF_FAST;
constexpr u8 kChipPriority = static_cast<u8>(-10);
constexpr u8 kFastPriority = 0;

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

void read_task_name(Machine& m, u32 task_addr, char (&out)[kMaxName]) {
    if (task_addr != 0) {
        Bus b(m);
        const u32 ptr = b.r32(task_addr + LN_NAME);
        if (b.ok() && ptr != 0 && b.string(ptr, out, sizeof out) && out[0] != '\0') {
            return;
        }
    }
    static constexpr char kUnnamed[] = "(unnamed)";
    for (u32 i = 0; i < sizeof kUnnamed; ++i) {
        out[i] = kUnnamed[i];
    }
}

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
        Exec& e = of(ctx);
        const u32 block = e.alloc(m.cpu.d[0], m.cpu.d[1]);
        e.claim_for_program(block, m.cpu.d[0]);
        native::set_result(m, block);
    }

    static void free_mem(void* ctx, Machine& m) { of(ctx).free(m.cpu.a[1], m.cpu.d[0]); }

    // AllocEntry(memList A0): the memory each MemEntry asks for, in a
    // MemList of this call's own. A failed allocation frees what it got and
    // answers the requirements it failed on, with bit 31 set.
    static void alloc_entry(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 list = m.cpu.a[0];
        if (list == 0) {
            native::set_result(m, 0);
            return;
        }
        const u32 count = b.r16(list + ML_NUMENTRIES);
        if (!b.ok()) {
            return;
        }
        const u32 list_bytes = ML_ME + count * ME_SIZE;
        const u32 table = e.alloc(list_bytes, MEMF_PUBLIC | MEMF_CLEAR);
        u32 failed = count != 0 ? b.r32(list + ML_ME) : 0;
        u32 made = 0;
        for (; table != 0 && made < count; ++made) {
            const u32 at = list + ML_ME + made * ME_SIZE;
            const u32 reqs = b.r32(at);
            const u32 length = b.r32(at + ME_LENGTH);
            if (!b.ok()) {
                break;
            }
            const u32 block = e.alloc(length, reqs);
            if (block == 0) {
                failed = reqs;
                break;
            }
            b.w32(table + ML_ME + made * ME_SIZE, block);
            b.w32(table + ML_ME + made * ME_SIZE + ME_LENGTH, length);
        }
        if (table == 0 || !b.ok() || made < count) {
            for (u32 i = 0; i < made; ++i) {
                const u32 at = table + ML_ME + i * ME_SIZE;
                e.free(b.r32(at), b.r32(at + ME_LENGTH));
            }
            e.free(table, list_bytes);
            native::set_result(m, failed | 0x80000000u);
            return;
        }
        b.w16(table + ML_NUMENTRIES, count);
        b.w8(table + LN_TYPE, NT_MEMORY);
        native::set_result(m, table);
    }

    // FreeEntry(memList A0): every entry of an AllocEntry list, then the
    // MemList itself.
    static void free_entry(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        const u32 list = m.cpu.a[0];
        if (list == 0) {
            return;
        }
        Bus b(m);
        const u32 count = b.r16(list + ML_NUMENTRIES);
        if (!b.ok()) {
            return;
        }
        for (u32 i = 0; i < count; ++i) {
            const u32 at = list + ML_ME + i * ME_SIZE;
            e.free(b.r32(at), b.r32(at + ME_LENGTH));
        }
        e.free(list, ML_ME + count * ME_SIZE);
    }

    static void avail_mem(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        const u32 flags = m.cpu.d[1];
        u32 result = 0;
        for (u32 i = 0; i < e.region_count_; ++i) {
            const Exec::Region& region = e.regions_[i];
            if (!Exec::accepts(region, flags)) {
                continue;
            }
            if ((flags & MEMF_TOTAL) != 0) {
                result += region.upper - region.lower;
            } else if ((flags & MEMF_LARGEST) != 0) {
                const u32 largest = e.largest_free(region);
                result = largest > result ? largest : result;
            } else {
                result += region.free_bytes;
            }
        }
        native::set_result(m, result);
    }

    static void type_of_mem(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).type_of_mem(m.cpu.a[1]));
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
        of(ctx).claim_for_program(block, bytes + 8);
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

    // CacheControl(bits D0, mask D1): the translations are the only cache,
    // so the bits are only kept: the ones in mask become bits, and the old
    // ones come back.
    static void cache_control(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        const u32 old = e.cache_bits_;
        e.cache_bits_ = (old & ~m.cpu.d[1]) | (m.cpu.d[0] & m.cpu.d[1]);
        native::set_result(m, old);
    }

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

    // --- resources -------------------------------------------------------

    // AddResource: the node at A1 goes on ResourceList by its priority.
    static void add_resource_lvo(void* ctx, Machine& m) {
        Bus b(m);
        enqueue(b, of(ctx).base_ + ResourceList, m.cpu.a[1]);
    }

    // RemResource: takes the node at A1 off ResourceList.
    static void rem_resource_lvo(void*, Machine& m) {
        Bus b(m);
        remove(b, m.cpu.a[1]);
    }

    // OpenResource: the resource named at A1 on ResourceList, or 0.
    static void open_resource_lvo(void* ctx, Machine& m) {
        Bus b(m);
        const u32 name = m.cpu.a[1];
        const u32 found =
            name == 0 ? 0
                      : find_node(b, of(ctx).base_ + ResourceList,
                                  [&](u32 n) { return b.equals(n, name); });
        native::set_result(m, found);
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
    static void attention_hook(void* ctx) { of(ctx).run_rescue(); }

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

    // True for a task the scheduler holds or the boot task.
    static bool known_task(const Exec& e, u32 addr) {
        return addr != 0 && (e.sched_.find(addr) >= 0 || addr == e.tasks_[0].task_addr);
    }

    // Signals a task exec knows. An address that is not a scheduled task and
    // not the boot task may be freed memory and is only logged.
    static void signal_task(Exec& e, Bus& b, u32 task_addr, u32 mask) {
        const u32 addr = native::guest_address(*e.m_, task_addr);
        if (addr == 0) {
            return;
        }
        if (!known_task(e, addr)) {
            if (e.unknown_signal_addr_ != addr) {
                e.unknown_signal_addr_ = addr;
                LogLine().text("exec: Signal to unknown task ").hex(addr).send();
            }
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
            const bool ctrl_c = (recvd & SIGBREAKF_CTRL_C) != 0;
            // Ctrl+C also cuts a non-boot task's Delay short.
            const bool wake = e.tasks_[id].sleeping
                                  ? ((ctrl_c && id != 0) ||
                                     (recvd & e.tasks_[id].sleep_signals) != 0)
                                  : ((recvd & wanted) != 0 || (id == 0 && ctrl_c));
            if (wake) {
                const bool linked = e.unlink_task(addr);
                b.w8(addr + TC_STATE, TS_READY);
                e.sched_.wake(id);
                if (linked) {
                    enqueue(b, e.base_ + TaskReady, addr);
                } else {
                    e.rebuild_task_lists();
                }
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
        const u32 task_raw = m.cpu.a[1];
        const u32 task = native::guest_address(m, task_raw);
        const u32 spawned = e.spawn(task, m.cpu.a[2], m.cpu.a[3], false);
        native::set_result(m, spawned != 0 ? task_raw : 0);
    }

    static bool restore_task_from_wait(Exec& e, Bus& b, i32 cur, u32 cur_addr) {
        if (cur < 0 || static_cast<u32>(cur) >= Scheduler::kMaxTasks) {
            b.stop();
            return false;
        }
        if (e.sched_.slot(static_cast<u32>(cur)).suspended) {
            e.hold_while_suspended();
        }
        const TaskState st = e.sched_.slot(static_cast<u32>(cur)).state;
        if (st == TaskState::Running) {
            if (cur_addr != 0) {
                b.w32(cur_addr + TC_SIGWAIT, 0);
            }
            return true;
        }
        if (st != TaskState::Waiting && st != TaskState::Ready) {
            b.stop();
            return false;
        }
        const bool linked = cur_addr == 0 || e.unlink_task(cur_addr);
        if (cur_addr != 0) {
            b.w8(cur_addr + TC_STATE, TS_RUN);
            b.w32(cur_addr + TC_SIGWAIT, 0);
        }
        if (st == TaskState::Waiting) {
            e.sched_.wake(cur);
        }
        if (!e.sched_.switch_to(cur)) {
            b.stop();
            return false;
        }
        if (!linked) {
            e.rebuild_task_lists();
        }
        return true;
    }

    static void switch_away_after_block(Exec& e, Bus& b, i32 cur, u32 cur_addr, bool is_remove) {
        Machine& m = b.machine();
        for (;;) {
            i32 next = e.sched_.pick_next();
            while (next < 0) {
                e.expire_timers();
                if (!is_remove && m.cpu.exit != ExitReason::None) {
                    restore_task_from_wait(e, b, cur, cur_addr);
                    b.stop();
                    return;
                }
                if (is_remove && m.cpu.exit != ExitReason::None) {
                    m.cpu.exit = ExitReason::None;
                }
                next = e.sched_.pick_next();
                if (next >= 0) {
                    break;
                }
                const bool can_wake = poll_idle(e);
                if (!is_remove && m.cpu.exit != ExitReason::None) {
                    restore_task_from_wait(e, b, cur, cur_addr);
                    b.stop();
                    return;
                }
                if (is_remove && m.cpu.exit != ExitReason::None) {
                    m.cpu.exit = ExitReason::None;
                }
                e.expire_timers();
                if (!is_remove && m.cpu.exit != ExitReason::None) {
                    restore_task_from_wait(e, b, cur, cur_addr);
                    b.stop();
                    return;
                }
                if (is_remove && m.cpu.exit != ExitReason::None) {
                    m.cpu.exit = ExitReason::None;
                }
                if (!is_remove && cur == 0 && (signals(e, b) & SIGBREAKF_CTRL_C) != 0 &&
                    e.sched_.slot(0).state == TaskState::Waiting && !e.tasks_[0].sleeping) {
                    if (cur_addr != 0) {
                        e.unlink_task(cur_addr);
                        b.w8(cur_addr + TC_STATE, TS_READY);
                    }
                    e.sched_.wake(0);
                    if (cur_addr != 0) {
                        enqueue(b, e.base_ + TaskReady, cur_addr);
                    }
                }
                next = e.sched_.pick_next();
                const bool timer_pending = e.task_host_.now_us != nullptr && e.has_pending_timers();
                if (next < 0 && !can_wake && !timer_pending) {
                    if (!is_remove && cur == 0 && e.sched_.active_count() == 1) {
                        if (restore_task_from_wait(e, b, 0, cur_addr)) {
                            LogLine().text("exec: Wait with no signals pending").send();
                            end_run(m, ExitReason::Unimplemented);
                        }
                        b.stop();
                        return;
                    }
                    if (!e.every_task_waits_logged_) {
                        e.every_task_waits_logged_ = true;
                        LogLine().text("exec: every task waits").send();
                    }
                    if (cur == 0) {
                        if (restore_task_from_wait(e, b, 0, cur_addr)) {
                            end_run(m, ExitReason::Unimplemented);
                        }
                        b.stop();
                        return;
                    }
                    e.abort_to_boot_task_ = true;
                    if (e.sched_.slot(0).state == TaskState::Waiting) {
                        const u32 boot_addr = e.tasks_[0].task_addr;
                        if (boot_addr != 0) {
                            e.unlink_task(boot_addr);
                            b.w8(boot_addr + TC_STATE, TS_READY);
                        }
                        e.sched_.wake(0);
                        if (boot_addr != 0) {
                            enqueue(b, e.base_ + TaskReady, boot_addr);
                        }
                    }
                    next = 0;
                    break;
                }
            }
            if (e.task_host_.switch_to == nullptr) {
                if (is_remove) {
                    end_run(m, ExitReason::Returned);
                } else if (restore_task_from_wait(e, b, cur, cur_addr)) {
                    end_run(m, ExitReason::Unimplemented);
                }
                b.stop();
                return;
            }
            if (!is_remove && next == cur) {
                if (!restore_task_from_wait(e, b, cur, cur_addr)) {
                    b.stop();
                }
                return;
            }
            if (e.switch_to_task(next)) {
                return;
            }
            if (!is_remove) {
                restore_task_from_wait(e, b, cur, cur_addr);
                end_run(m, ExitReason::Unimplemented);
                b.stop();
                return;
            }
        }
    }

    // Notes the MemLists on a removed task's tc_MemEntry, which RemTask
    // frees; they may hold the task and its stack, so the memory goes only
    // when the slot is released. A task Hombre made starts with that list
    // zeroed rather than empty, and one whose tail pred is 0 has none. A
    // list that comes back to a node it passed, or holds more than
    // kMaxTaskMemEntries entries in all, is taken only that far.
    static void take_task_mem_entries(Exec& e, Bus& b, u32 slot, u32 task) {
        const u32 list = task + kTaskMemEntry;
        u32 node = b.r32(list + LH_HEAD);
        if (!b.ok() || node == 0 || b.r32(list + LH_TAILPRED) == 0) {
            return;
        }
        std::vector<Exec::TaskContext::Owned>& owned = e.tasks_[slot].owned;
        std::vector<u32> seen;
        u32 entries = 0;
        for (u32 n = 0; node != list + LH_TAIL && n < kMaxMemLists; ++n) {
            if (node == 0 || std::find(seen.begin(), seen.end(), node) != seen.end()) {
                LogLine().text("exec: tc_MemEntry of task ").hex(task).text(" loops").send();
                return;
            }
            seen.push_back(node);
            const u32 next = b.r32(node + LN_SUCC);
            const u32 count = b.r16(node + ML_NUMENTRIES);
            if (!b.ok()) {
                return;
            }
            if (entries + count > kMaxTaskMemEntries) {
                LogLine().text("exec: tc_MemEntry of task ").hex(task).text(" too long").send();
                return;
            }
            entries += count;
            for (u32 i = 0; i < count; ++i) {
                const u32 at = node + ML_ME + i * ME_SIZE;
                owned.push_back({b.r32(at), b.r32(at + ME_LENGTH)});
            }
            owned.push_back({node, ML_ME + count * ME_SIZE});
            node = next;
        }
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
        if (target_addr != 0) {
            e.drop_timer_requests_of(target_addr);
        }
        bool relink = false;
        if ((slot.state == TaskState::Ready || slot.state == TaskState::Waiting) &&
            target_addr != 0) {
            relink = !e.unlink_task(target_addr);
        }
        e.remove_sem_waiter(static_cast<u32>(target_id));
        if (target_addr != 0) {
            b.w8(target_addr + TC_STATE, TS_REMOVED);
        }
        e.sched_.remove(target_id);
        if (relink) {
            e.rebuild_task_lists();
        }
        if (target_addr != 0) {
            take_task_mem_entries(e, b, static_cast<u32>(target_id), target_addr);
        }
        e.task_ended(target_addr, target_id == cur);
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
                e.unlink_task(addr);
                e.sched_.set_priority(id, new_pri);
                enqueue(b, e.base_ + TaskReady, addr);
            } else if (st == TaskState::Waiting) {
                e.unlink_task(addr);
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
            e.expire_timers();
            if (m.cpu.exit != ExitReason::None) {
                b.stop();
                return 0;
            }
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
            const bool timer_can_wake = e.task_host_.now_us != nullptr && e.has_pending_timers();
            if (cur == 0 && e.sched_.active_count() == 1 && (wanted & raisable) == 0 &&
                !(wanted != 0 && timer_can_wake)) {
                LogLine().text("exec: Wait with no signals pending").send();
                end_run(m, ExitReason::Unimplemented);
                b.stop();
                return 0;
            }
            if (cur == 0 && e.sched_.active_count() == 1 && e.task_host_.switch_to == nullptr) {
                const bool can_wake = poll_idle(e);
                if (m.cpu.exit != ExitReason::None) {
                    b.stop();
                    return 0;
                }
                e.expire_timers();
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
                const bool still_timer = e.task_host_.now_us != nullptr && e.has_pending_timers();
                if (!can_wake && !still_timer && (signals(e, b) & wanted) == 0) {
                    if (m.cpu.exit == ExitReason::None) {
                        LogLine().text("exec: Wait with no signals pending").send();
                        end_run(m, ExitReason::Unimplemented);
                    }
                    b.stop();
                    return 0;
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
                e.expire_timers();
                if (m.cpu.exit != ExitReason::None) {
                    restore_task_from_wait(e, b, cur, cur_addr);
                    b.stop();
                    return 0;
                }
                next = e.sched_.pick_next();
                if (next >= 0) {
                    break;
                }
                const bool can_wake = poll_idle(e);
                if (m.cpu.exit != ExitReason::None) {
                    restore_task_from_wait(e, b, cur, cur_addr);
                    b.stop();
                    return 0;
                }
                e.expire_timers();
                if (m.cpu.exit != ExitReason::None) {
                    restore_task_from_wait(e, b, cur, cur_addr);
                    b.stop();
                    return 0;
                }
                if (cur == 0 && (wanted & SIGBREAKF_CTRL_C) == 0) {
                    const u32 after = signals(e, b);
                    if ((after & SIGBREAKF_CTRL_C) != 0) {
                        store_signals(e, b, after & ~SIGBREAKF_CTRL_C);
                        restore_task_from_wait(e, b, 0, cur_addr);
                        LogLine().text("exec: Wait interrupted by Ctrl+C").send();
                        end_run(m, ExitReason::Break);
                        b.stop();
                        return 0;
                    }
                }
                next = e.sched_.pick_next();
                const bool still_timer = e.task_host_.now_us != nullptr && e.has_pending_timers();
                if (next < 0 && !can_wake && !still_timer) {
                    if (cur == 0 && e.sched_.active_count() == 1) {
                        if (restore_task_from_wait(e, b, 0, cur_addr)) {
                            LogLine().text("exec: Wait with no signals pending").send();
                            end_run(m, ExitReason::Unimplemented);
                        }
                        b.stop();
                        return 0;
                    }
                    if (!e.every_task_waits_logged_) {
                        e.every_task_waits_logged_ = true;
                        LogLine().text("exec: every task waits").send();
                    }
                    if (cur == 0) {
                        if (restore_task_from_wait(e, b, 0, cur_addr)) {
                            end_run(m, ExitReason::Unimplemented);
                        }
                        b.stop();
                        return 0;
                    }
                    e.abort_to_boot_task_ = true;
                    if (e.sched_.slot(0).state == TaskState::Waiting) {
                        if (e.tasks_[0].task_addr != 0) {
                            e.unlink_task(e.tasks_[0].task_addr);
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
                if (!restore_task_from_wait(e, b, cur, cur_addr)) {
                    return 0;
                }
                continue;
            }
            if (e.task_host_.switch_to == nullptr || !e.switch_to_task(next)) {
                if (restore_task_from_wait(e, b, cur, cur_addr)) {
                    LogLine().text("exec: Wait with no signals pending").send();
                    end_run(m, ExitReason::Unimplemented);
                }
                b.stop();
                return 0;
            }
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
        Exec& e = of(ctx);
        const u32 port = e.create_port();
        e.claim_for_program(port, MP_SIZE);
        native::set_result(m, port);
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
        if (e.message_hook_.take != nullptr && e.message_hook_.take(e.message_hook_.ctx, port, message)) {
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

    static bool reply_msg(Exec& e, Bus& b, u32 message) {
        Machine& m = b.machine();
        if (native::guest_address(m, message) == 0) {
            fault_null(b);
            return false;
        }
        const u32 reply_port = b.r32(message + mn_ReplyPort);
        if (!b.ok()) {
            return false;
        }
        if (native::guest_address(m, reply_port) == 0) {
            b.w8(message + LN_TYPE, NT_FREEMSG);
            return b.ok();
        }
        for (const auto& h : e.reply_handlers_) {
            if (h.handler != nullptr && h.port != 0 &&
                native::guest_address(m, reply_port) == native::guest_address(m, h.port)) {
                h.handler(h.ctx, message);
                return true;
            }
        }
        return queue_message(e, b, reply_port, message, NT_REPLYMSG);
    }

    static void reply_msg_lvo(void* ctx, Machine& m) {
        Bus b(m);
        (void)reply_msg(of(ctx), b, m.cpu.a[1]);
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
        if (e.loaded_.size() < Exec::kMaxLoaded) {
            e.loaded_.push_back(seglist);
            return;
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
        // commodities.library keeps the ports of the programs that use it, in
        // a list exec cannot see.
        if (b.equals(name_addr + static_cast<u32>(leaf - name), kCommodities)) {
            if (Exec::Program* program = e.find_program(e.calling_program())) {
                program->uses_commodities = true;
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
        if (seglist != 0 && std::erase(e.loaded_, seglist) != 0) {
            unload(e, seglist);
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

    // Alert(alertNum): logged. A recoverable alert returns to its caller; a
    // dead-end one (bit 31 set) ends the task that raised it.
    static void alert(void*, Machine& m) {
        const u32 number = m.cpu.d[7];
        const bool dead_end = (number & 0x80000000u) != 0;
        LogLine().text("exec: Alert ").hex(number).text(dead_end ? " (dead end)" : "").send();
        if (dead_end) {
            end_run(m, ExitReason::Halt);
        }
    }

    // SetFunction(library, offset, function): the vector at offset becomes a
    // JMP to function, and the old function comes back in D0. A native
    // vector is an A-line call and an RTS; the call moves to a stub of its
    // own, so the old function stays callable.
    static void set_function(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 lib = m.cpu.a[1];
        const i32 offset = static_cast<i32>(m.cpu.a[0]);
        const u32 function = m.cpu.d[0];
        const u32 vec = lib + static_cast<u32>(offset);
        const u32 w0 = b.r16(vec);
        if (!b.ok() || lib == 0 || offset >= 0) {
            native::set_result(m, 0);
            return;
        }
        u32 old = 0;
        if (w0 == native::kJmpAbsL) {
            old = b.r32(vec + 2);
        } else if ((w0 & 0xF000u) == native::kALine) {
            // A vector with no native of its own is A-line words without
            // the RTS; its stub gets one.
            old = e.alloc(4, MEMF_PUBLIC);
            if (old == 0) {
                native::set_result(m, 0);
                return;
            }
            b.w16(old, static_cast<u16>(w0));
            b.w16(old + 2, native::kRts);
        } else {
            LogLine().text("exec: SetFunction of an unknown vector at ").hex(vec).send();
            native::set_result(m, 0);
            return;
        }
        b.w16(vec, native::kJmpAbsL);
        b.w32(vec + 2, function);
        // A vector's patches stack up. Setting it back to what the last one
        // replaced takes that one off; any other function goes on top, and
        // those beneath stay, as the new one may call them.
        u32 top = e.patch_count_;
        while (top > 0 && e.patches_[top - 1].vector != vec) {
            --top;
        }
        if (top > 0 && e.patches_[top - 1].replaced == function) {
            for (u32 i = top; i < e.patch_count_; ++i) {
                e.patches_[i - 1] = e.patches_[i];
            }
            --e.patch_count_;
        } else if (e.patch_count_ < Exec::kMaxPatches) {
            e.patches_[e.patch_count_++] = {vec, function, old};
        }
        b.w8(lib + LIB_FLAGS, static_cast<u8>(b.r8(lib + LIB_FLAGS) | LIBF_CHANGED));
        sum_library(b, lib);
        m.jit->flush();
        native::set_result(m, old);
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

    // --- devices ---------------------------------------------------------

    static constexpr u32 sign_extend_byte(u32 v) {
        return static_cast<u32>(static_cast<i32>(static_cast<i8>(v & 0xFF)));
    }

    static bool call_device_vector(Exec& e, Bus& b, u32 dev, u32 lvo, u32 d0, u32 d1, u32 a1,
                                   u32& d0_out) {
        const u32 vec = dev - lvo;
        const u32 w0 = b.r16(vec);
        const u32 w1 = b.r16(vec + 2);
        if (!b.ok()) {
            return false;
        }
        CpuState& cpu = e.m_->cpu;
        u32 saved_d[8];
        u32 saved_a[8];
        for (u32 i = 0; i < 8; ++i) {
            saved_d[i] = cpu.d[i];
            saved_a[i] = cpu.a[i];
        }
        if ((w0 & 0xF000u) == native::kALine && w1 == native::kRts) {
            const u16 id = static_cast<u16>(w0 & 0x0FFFu);
            cpu.d[0] = d0;
            cpu.d[1] = d1;
            cpu.a[1] = a1;
            cpu.a[6] = dev;
            // A device vector may name any native, OpenDevice itself
            // included, so direct calls nest under the same limit as calls
            // into 68000 code.
            if (e.call_depth_ >= kMaxCallDepth) {
                LogLine().text("exec: device calls nested too deep").send();
                end_run(*e.m_, ExitReason::Unimplemented);
                b.stop();
                return false;
            }
            ++e.call_depth_;
            const bool called = e.m_->jit->call_native(id);
            --e.call_depth_;
            if (called) {
                if (cpu.exit != ExitReason::None) {
                    b.stop();
                    return false;
                }
                d0_out = cpu.d[0];
                for (u32 i = 0; i < 8; ++i) {
                    cpu.d[i] = saved_d[i];
                    cpu.a[i] = saved_a[i];
                }
                return true;
            }
            for (u32 i = 0; i < 8; ++i) {
                cpu.d[i] = saved_d[i];
                cpu.a[i] = saved_a[i];
            }
        }
        GuestCall call(e.call_depth_, b);
        cpu.d[0] = d0;
        cpu.d[1] = d1;
        cpu.a[1] = a1;
        cpu.a[6] = dev;
        if (!call.run(vec)) {
            return false;
        }
        d0_out = call.d0();
        return true;
    }

    static bool is_queued_on_port(Bus& b, u32 port, u32 target) {
        Machine& m = b.machine();
        const u32 target_addr = native::guest_address(m, target);
        const u32 list = port + mp_MsgList;
        u32 n = b.r32(list + LH_HEAD);
        for (u32 count = 0; count < kMaxQueued; ++count) {
            const u32 succ = b.r32(n + LN_SUCC);
            if (!b.ok() || succ == 0) {
                return false;
            }
            if (native::guest_address(m, n) == target_addr) {
                return true;
            }
            n = succ;
        }
        return false;
    }

    static u32 wait_io(Exec& e, Bus& b, u32 io_raw) {
        Machine& m = b.machine();
        if (native::guest_address(m, io_raw) == 0) {
            fault_null(b);
            return 0;
        }
        const u32 flags = b.r8(io_raw + io_Flags);
        if (!b.ok()) {
            return 0;
        }
        if ((flags & IOF_QUICK) != 0) {
            return sign_extend_byte(b.r8(io_raw + io_Error));
        }
        const u32 port = b.r32(io_raw + mn_ReplyPort);
        if (!b.ok()) {
            return 0;
        }
        for (;;) {
            e.expire_timers();
            if (m.cpu.exit != ExitReason::None) {
                b.stop();
                return 0;
            }
            const u32 type = b.r8(io_raw + LN_TYPE);
            if (!b.ok()) {
                return 0;
            }
            if (type == NT_REPLYMSG) {
                break;
            }
            if (native::guest_address(m, port) == 0) {
                fault_null(b);
                return 0;
            }
            const u32 sig_bit = b.r8(port + mp_SigBit);
            if (!b.ok()) {
                return 0;
            }
            const u32 mask = sig_bit < 32 ? (1u << sig_bit) : 0;
            if (wait_signals(e, b, mask) == 0 || !b.ok()) {
                return 0;
            }
        }
        if (native::guest_address(m, port) != 0 && is_queued_on_port(b, port, io_raw)) {
            remove(b, io_raw);
        }
        return sign_extend_byte(b.r8(io_raw + io_Error));
    }

    static void open_device_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 name_raw = m.cpu.a[0];
        const u32 unit = m.cpu.d[0];
        const u32 io_raw = m.cpu.a[1];
        const u32 flags = m.cpu.d[1];
        if (native::guest_address(m, name_raw) == 0 || native::guest_address(m, io_raw) == 0) {
            fault_null(b);
            return;
        }
        char name[kMaxName];
        if (!b.string(name_raw, name, sizeof name)) {
            if (b.ok()) {
                LogLine().text("exec: device name too long").send();
                b.w32(io_raw + io_Device, 0);
                b.w8(io_raw + io_Error, static_cast<u8>(IOERR_OPENFAIL));
                if (b.ok()) {
                    native::set_result(m, static_cast<u32>(-1));
                }
            }
            return;
        }
        const u32 dev = find_name(b, e.base_ + DeviceList, name);
        if (!b.ok()) {
            return;
        }
        if (dev == 0) {
            LogLine().text("exec: no device ").text(name).send();
            b.w32(io_raw + io_Device, 0);
            b.w8(io_raw + io_Error, static_cast<u8>(IOERR_OPENFAIL));
            if (b.ok()) {
                native::set_result(m, static_cast<u32>(-1));
            }
            return;
        }
        b.w32(io_raw + io_Device, dev);
        b.w8(io_raw + io_Error, 0);
        if (!b.ok()) {
            return;
        }
        u32 d0_out = 0;
        if (!call_device_vector(e, b, dev, dev_lvo::Open, unit, flags, io_raw, d0_out)) {
            return;
        }
        const u32 err = sign_extend_byte(b.r8(io_raw + io_Error));
        if (b.ok()) {
            native::set_result(m, err);
        }
    }

    static void close_device_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 io_raw = m.cpu.a[1];
        if (native::guest_address(m, io_raw) == 0) {
            return;
        }
        const u32 dev = b.r32(io_raw + io_Device);
        if (!b.ok()) {
            return;
        }
        if (native::guest_address(m, dev) != 0) {
            u32 d0_out = 0;
            if (!call_device_vector(e, b, dev, dev_lvo::Close, 0, 0, io_raw, d0_out)) {
                return;
            }
        }
        b.w32(io_raw + io_Device, 0);
    }

    static void do_io_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 io_raw = m.cpu.a[1];
        if (native::guest_address(m, io_raw) == 0) {
            fault_null(b);
            return;
        }
        const u32 dev = b.r32(io_raw + io_Device);
        if (!b.ok()) {
            return;
        }
        if (native::guest_address(m, dev) == 0) {
            fault_null(b);
            return;
        }
        b.w8(io_raw + io_Flags, IOF_QUICK);
        b.w8(io_raw + LN_TYPE, NT_MESSAGE);
        if (!b.ok()) {
            return;
        }
        u32 d0_out = 0;
        if (!call_device_vector(e, b, dev, dev_lvo::BeginIO, 0, 0, io_raw, d0_out)) {
            return;
        }
        const u32 flags = b.r8(io_raw + io_Flags);
        if (!b.ok()) {
            return;
        }
        if ((flags & IOF_QUICK) != 0) {
            const u32 err = sign_extend_byte(b.r8(io_raw + io_Error));
            if (b.ok()) {
                native::set_result(m, err);
            }
            return;
        }
        const u32 err = wait_io(e, b, io_raw);
        if (b.ok()) {
            native::set_result(m, err);
        }
    }

    static void send_io_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 io_raw = m.cpu.a[1];
        if (native::guest_address(m, io_raw) == 0) {
            fault_null(b);
            return;
        }
        const u32 dev = b.r32(io_raw + io_Device);
        if (!b.ok()) {
            return;
        }
        if (native::guest_address(m, dev) == 0) {
            fault_null(b);
            return;
        }
        b.w8(io_raw + io_Flags, 0);
        b.w8(io_raw + LN_TYPE, NT_MESSAGE);
        if (!b.ok()) {
            return;
        }
        u32 d0_out = 0;
        (void)call_device_vector(e, b, dev, dev_lvo::BeginIO, 0, 0, io_raw, d0_out);
    }

    static void check_io_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 io_raw = m.cpu.a[1];
        if (native::guest_address(m, io_raw) == 0) {
            fault_null(b);
            return;
        }
        e.expire_timers();
        if (m.cpu.exit != ExitReason::None) {
            return;
        }
        const u32 flags = b.r8(io_raw + io_Flags);
        const u32 type = b.r8(io_raw + LN_TYPE);
        if (!b.ok()) {
            return;
        }
        const bool done = (flags & IOF_QUICK) != 0 || type == NT_REPLYMSG;
        native::set_result(m, done ? io_raw : 0);
    }

    static void wait_io_lvo(void* ctx, Machine& m) {
        Bus b(m);
        const u32 err = wait_io(of(ctx), b, m.cpu.a[1]);
        if (b.ok()) {
            native::set_result(m, err);
        }
    }

    static void abort_io_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 io_raw = m.cpu.a[1];
        if (native::guest_address(m, io_raw) == 0) {
            fault_null(b);
            return;
        }
        const u32 dev = b.r32(io_raw + io_Device);
        if (!b.ok()) {
            return;
        }
        if (native::guest_address(m, dev) == 0) {
            fault_null(b);
            return;
        }
        u32 d0_out = 0;
        if (!call_device_vector(e, b, dev, dev_lvo::AbortIO, 0, 0, io_raw, d0_out)) {
            return;
        }
        native::set_result(m, d0_out);
    }

    static void create_io_request_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 port = m.cpu.a[0];
        const u32 size = m.cpu.d[0];
        if (native::guest_address(m, port) == 0 || size < MN_SIZE || size > 0xFFFF) {
            native::set_result(m, 0);
            return;
        }
        const u32 req = e.alloc(size, MEMF_PUBLIC | MEMF_CLEAR);
        if (req == 0) {
            native::set_result(m, 0);
            return;
        }
        b.w8(req + LN_TYPE, NT_REPLYMSG);
        b.w32(req + mn_ReplyPort, port);
        b.w16(req + mn_Length, size);
        if (!b.ok()) {
            e.free(req, size);
            return;
        }
        e.claim_for_program(req, size);
        native::set_result(m, req);
    }

    static void delete_io_request_lvo(void* ctx, Machine& m) {
        Exec& e = of(ctx);
        Bus b(m);
        const u32 io_raw = m.cpu.a[0];
        if (native::guest_address(m, io_raw) == 0) {
            return;
        }
        const u32 len = b.r16(io_raw + mn_Length);
        if (!b.ok() || len == 0) {
            return;
        }
        e.free(io_raw, len);
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
            e.sem_queues_.remove(q);
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
        if (shared && owner == 0) {
            b.w16(sem + SS_NESTCOUNT, static_cast<u16>(nest + 1));
            b.w16(sem + SS_QUEUECOUNT, static_cast<u16>(qcount + 1));
            return;
        }

        Exec::SemWaitQueue* q = e.sem_queue(sem, true);
        if (q == nullptr || q->count >= Scheduler::kMaxTasks) {
            LogLine().text("exec: semaphore wait queue full").send();
            end_run(b.machine(), ExitReason::Unimplemented);
            b.stop();
            return;
        }
        if (cur_addr != 0) {
            b.w32(cur_addr + TC_SIGWAIT, 0);
            b.w8(cur_addr + TC_STATE, TS_WAIT);
            enqueue(b, e.base_ + TaskWait, cur_addr);
            if (!b.ok()) {
                return;
            }
        }
        q->waiters[q->count++] = Exec::SemWaitQueue::Waiter{static_cast<u32>(cur), shared};
        b.w16(sem + SS_QUEUECOUNT, static_cast<u16>(qcount + 1));
        if (!b.ok()) {
            e.remove_sem_waiter(static_cast<u32>(cur));
            return;
        }
        e.sched_.wait(cur);
        switch_away_after_block(e, b, cur, cur_addr, false);

        Machine& m = b.machine();
        if (e.sched_.current() != cur && m.cpu.exit == ExitReason::None) {
            return;
        }
        if (e.abort_to_boot_task_ && cur == 0) {
            e.remove_sem_waiter(static_cast<u32>(cur));
            end_run(m, ExitReason::Unimplemented);
            b.stop();
            return;
        }
        if (m.cpu.exit != ExitReason::None) {
            e.remove_sem_waiter(static_cast<u32>(cur));
            b.stop();
            return;
        }
        if (cur == 0) {
            const u32 after = signals(e, b);
            if (b.ok() && (after & SIGBREAKF_CTRL_C) != 0) {
                store_signals(e, b, after & ~SIGBREAKF_CTRL_C);
                e.remove_sem_waiter(0);
                LogLine().text("exec: ObtainSemaphore interrupted by Ctrl+C").send();
                end_run(m, ExitReason::Break);
                b.stop();
                return;
            }
        }
        if (e.has_sem_waiter(sem, static_cast<u32>(cur))) {
            e.remove_sem_waiter(static_cast<u32>(cur));
            restore_task_from_wait(e, b, cur, cur_addr);
            end_run(m, ExitReason::Unimplemented);
            b.stop();
        }
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
        if (shared && owner == 0) {
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
        if (wid >= Scheduler::kMaxTasks || e.sched_.slot(wid).state != TaskState::Waiting) {
            return;
        }
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
        while (q != nullptr && q->count > 0) {
            if (e.sched_.slot(q->waiters[0].id).state != TaskState::Waiting) {
                pop_waiter(*q);
                const u16 qc = static_cast<u16>(b.r16(sem + SS_QUEUECOUNT) - 1);
                b.w16(sem + SS_QUEUECOUNT, qc);
                continue;
            }
            if (!q->waiters[0].shared) {
                const u32 wid = q->waiters[0].id;
                pop_waiter(*q);
                if (q->count == 0) {
                    e.sem_queues_.remove(q);
                }
                b.w32(sem + SS_OWNER, e.tasks_[wid].task_addr);
                b.w16(sem + SS_NESTCOUNT, 1);
                wake_sem_waiter(e, b, wid);
                if (e.sched_.should_switch_now()) {
                    e.pending_switch_ = true;
                }
                return;
            }
            u32 woken = 0;
            b.w32(sem + SS_OWNER, 0);
            while (q->count > 0 && q->waiters[0].shared) {
                const u32 wid = q->waiters[0].id;
                pop_waiter(*q);
                if (e.sched_.slot(wid).state != TaskState::Waiting) {
                    const u16 qc = static_cast<u16>(b.r16(sem + SS_QUEUECOUNT) - 1);
                    b.w16(sem + SS_QUEUECOUNT, qc);
                    continue;
                }
                ++woken;
                wake_sem_waiter(e, b, wid);
            }
            if (woken > 0) {
                b.w16(sem + SS_NESTCOUNT, static_cast<u16>(woken));
                if (q->count == 0) {
                    e.sem_queues_.remove(q);
                }
                if (e.sched_.should_switch_now()) {
                    e.pending_switch_ = true;
                }
                return;
            }
        }
        b.w32(sem + SS_OWNER, 0);
        if (q != nullptr) {
            e.sem_queues_.remove(q);
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
    {exec_lvo::AllocEntry, ExecOps::alloc_entry, "AllocEntry"},
    {exec_lvo::FreeEntry, ExecOps::free_entry, "FreeEntry"},
    {exec_lvo::TypeOfMem, ExecOps::type_of_mem, "TypeOfMem"},
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
    {exec_lvo::SetFunction, ExecOps::set_function, "SetFunction"},
    {exec_lvo::Alert, ExecOps::alert, "Alert"},
    {exec_lvo::OpenDevice, ExecOps::open_device_lvo, "OpenDevice"},
    {exec_lvo::CloseDevice, ExecOps::close_device_lvo, "CloseDevice"},
    {exec_lvo::DoIO, ExecOps::do_io_lvo, "DoIO"},
    {exec_lvo::SendIO, ExecOps::send_io_lvo, "SendIO"},
    {exec_lvo::CheckIO, ExecOps::check_io_lvo, "CheckIO"},
    {exec_lvo::WaitIO, ExecOps::wait_io_lvo, "WaitIO"},
    {exec_lvo::AbortIO, ExecOps::abort_io_lvo, "AbortIO"},
    {exec_lvo::AddResource, ExecOps::add_resource_lvo, "AddResource"},
    {exec_lvo::RemResource, ExecOps::rem_resource_lvo, "RemResource"},
    {exec_lvo::OpenResource, ExecOps::open_resource_lvo, "OpenResource"},
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
    {exec_lvo::CacheControl, ExecOps::cache_control, "CacheControl"},
    {exec_lvo::CreateIORequest, ExecOps::create_io_request_lvo, "CreateIORequest"},
    {exec_lvo::DeleteIORequest, ExecOps::delete_io_request_lvo, "DeleteIORequest"},
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

bool Exec::init(Machine& m, u32 free_start, u32 chip_end, u32 fast_end) {
    // The task, the segment loader, the idle hook and the task host may be
    // set before init; reset the remaining state in place so the kernel
    // stack never holds a full Exec temporary.
    m_ = nullptr;
    base_ = 0;
    for (Region& region : regions_) {
        region = Region{};
    }
    region_count_ = 0;
    patch_count_ = 0;
    cache_bits_ = 0;
    sig_alloc_ = 0x0000FFFF;
    signals_ = 0;
    port_signals_ = 0;
    call_depth_ = 0;
    load_depth_ = 0;
    open_id_ = 0;
    close_id_ = 0;
    expunge_id_ = 0;
    reserved_id_ = 0;
    task_exit_stub_ = 0;
    sched_.init(0);
    for (TaskContext& tc : tasks_) {
        tc = TaskContext{};
    }
    sem_queues_.clear();
    pending_switch_ = false;
    softint_logged_ = false;
    unknown_signal_addr_ = 0;
    every_task_waits_logged_ = false;
    abort_to_boot_task_ = false;
    for (ReplyHandler& h : reply_handlers_) {
        h = ReplyHandler{};
    }
    for (TaskEndHook& h : task_end_hooks_) {
        h = TaskEndHook{};
    }
    for (TimerHook& h : timer_hooks_) {
        h = TimerHook{};
    }
    for (DropHook& h : timer_drop_hooks_) {
        h = DropHook{};
    }
    for (LibraryInfo& info : libraries_) {
        info = LibraryInfo{};
    }
    library_count_ = 0;
    loaded_.clear();
    for (Program& p : programs_) {
        p = Program{};
    }
    next_program_ = 1;
    program_block_count_ = 0;
    for (Releaser& r : releasers_) {
        r = Releaser{};
    }
    for (KeptList& k : kept_lists_) {
        k = KeptList{};
    }
    kept_list_count_ = 0;
    for (Keeper& k : keepers_) {
        k = Keeper{};
    }
    for (EndHook& h : end_hooks_) {
        h = EndHook{};
    }
    if (m.jit == nullptr || m.mem.base == nullptr || free_start < kLowestFree ||
        chip_end <= free_start || fast_end < chip_end || fast_end > m.mem.size) {
        return false;
    }
    m_ = &m;
    Bus b(m);
    if ((fast_end > chip_end &&
         !make_region(b, chip_end, fast_end, MEMF_FAST | MEMF_PUBLIC, kFastPriority)) ||
        !make_region(b, free_start, chip_end, MEMF_CHIP | MEMF_PUBLIC, kChipPriority)) {
        m_ = nullptr;
        return false;
    }

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
    const u32 chip_name = id_string + sizeof kExecIdString;
    const u32 fast_name = chip_name + sizeof kChipName;
    task_exit_stub_ = (fast_name + sizeof kFastName + 1) & ~1u;
    put_string(b, name, kExecName);
    put_string(b, id_string, kExecIdString);
    put_string(b, chip_name, kChipName);
    put_string(b, fast_name, kFastName);
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

    for (u32 i = 0; i < region_count_; ++i) {
        const u32 header = regions_[i].header;
        b.w32(header + LN_NAME, (regions_[i].attributes & MEMF_CHIP) != 0 ? chip_name : fast_name);
        enqueue(b, base_ + MemList, header);
    }
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
    jit.set_attention_hook(&ExecOps::attention_hook, this);
    rescue_op_ = Rescue::None;
    jit.flush();
    if (!b.ok()) {
        m_ = nullptr;
        return false;
    }
    return true;
}

bool Exec::accepts(const Region& region, u32 flags) {
    return (region.attributes & flags & kMemoryTypes) == (flags & kMemoryTypes);
}

bool Exec::make_region(Bus& b, u32 from, u32 until, u32 attributes, u8 priority) {
    const u64 start = (static_cast<u64>(from) + 7) & ~static_cast<u64>(7);
    const u32 end = until & ~7u;
    if (start + MH_SIZE + MC_SIZE > end) {
        return false;
    }
    Region& region = regions_[region_count_++];
    region.header = static_cast<u32>(start);
    region.lower = region.header + MH_SIZE;
    region.upper = end;
    region.free_bytes = end - region.lower;
    region.attributes = attributes;
    for (u32 i = 0; i < MH_SIZE; ++i) {
        b.w8(region.header + i, 0);
    }
    b.w8(region.header + LN_TYPE, NT_MEMORY);
    b.w8(region.header + LN_PRI, priority);
    b.w16(region.header + MH_ATTRIBUTES, static_cast<u16>(attributes));
    b.w32(region.header + MH_FIRST, region.lower);
    b.w32(region.header + MH_LOWER, region.lower);
    b.w32(region.header + MH_UPPER, region.upper);
    b.w32(region.header + MH_FREE, region.free_bytes);
    b.w32(region.lower + MC_NEXT, 0);
    b.w32(region.lower + MC_BYTES, region.free_bytes);
    return b.ok();
}

u32 Exec::region_holding(u32 addr, u32 bytes) const {
    for (u32 i = 0; i < region_count_; ++i) {
        const Region& region = regions_[i];
        if (bytes <= region.upper - region.lower && addr >= region.lower &&
            static_cast<u64>(addr) + round8(bytes) <= region.upper) {
            return i;
        }
    }
    return region_count_;
}

u32 Exec::free_bytes(u32 attributes) const {
    u32 total = 0;
    for (u32 i = 0; i < region_count_; ++i) {
        total += accepts(regions_[i], attributes) ? regions_[i].free_bytes : 0;
    }
    return total;
}

u32 Exec::total_bytes(u32 attributes) const {
    u32 total = 0;
    for (u32 i = 0; i < region_count_; ++i) {
        total += accepts(regions_[i], attributes) ? regions_[i].upper - regions_[i].lower : 0;
    }
    return total;
}

u32 Exec::type_of_mem(u32 addr) const {
    if (m_ == nullptr) {
        return 0;
    }
    const u32 i = region_holding(native::guest_address(*m_, addr), 1);
    return i < region_count_ ? regions_[i].attributes : 0;
}

u32 Exec::largest_free(const Region& region) const {
    u32 largest = 0;
    u32 chunk = 0;
    u32 floor = region.lower;
    m_->mem.read32(region.header + MH_FIRST, chunk);
    while (chunk != 0) {
        u32 next = 0;
        u32 bytes = 0;
        if (!read_chunk(m_->mem, chunk, floor, region.upper, next, bytes)) {
            log_corrupt_memory_list();
            break;
        }
        largest = bytes > largest ? bytes : largest;
        floor = chunk + bytes;
        chunk = next;
    }
    return largest;
}

u32 Exec::alloc(u32 bytes, u32 flags) {
    if (m_ == nullptr || bytes == 0) {
        return 0;
    }
    for (u32 i = 0; i < region_count_; ++i) {
        if (!accepts(regions_[i], flags) || bytes > regions_[i].upper - regions_[i].lower) {
            continue;
        }
        const u32 size = round8(bytes);
        const u32 block = alloc_from(regions_[i], size);
        if (block == 0) {
            continue;
        }
        if ((flags & MEMF_CLEAR) != 0) {
            for (u32 j = 0; j < size; ++j) {
                m_->mem.base[block + j] = 0;
            }
        }
        return block;
    }
    return 0;
}

u32 Exec::alloc_from(Region& region, u32 size) {
    GuestMemory& mem = m_->mem;
    u32 link = region.header + MH_FIRST;
    u32 chunk = 0;
    u32 floor = region.lower;
    mem.read32(link, chunk);
    while (chunk != 0) {
        u32 next = 0;
        u32 avail = 0;
        if (!read_chunk(mem, chunk, floor, region.upper, next, avail)) {
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
            region.free_bytes = region.free_bytes >= size ? region.free_bytes - size : 0;
            mem.write32(region.header + MH_FREE, region.free_bytes);
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
    // Whatever frees a recorded block, or part of one, ends its record: a
    // program's end never frees it a second time.
    drop_program_blocks(addr, bytes);
    const u32 held_by = region_holding(addr, bytes);
    if ((addr & 7) != 0 || held_by == region_count_) {
        LogLine().text("exec: FreeMem of a bad block at ").hex(addr).text(", ").dec(bytes)
            .text(" bytes").send();
        return;
    }
    Region& region = regions_[held_by];
    const u32 size = round8(bytes);
    GuestMemory& mem = m_->mem;

    // The last free chunk below the block (prev) and the first at or above it.
    u32 link = region.header + MH_FIRST;
    u32 prev = 0;
    u32 prev_bytes = 0;
    u32 chunk = 0;
    u32 chunk_next = 0;
    u32 chunk_bytes = 0;
    u32 floor = region.lower;
    mem.read32(link, chunk);
    while (chunk != 0) {
        if (!read_chunk(mem, chunk, floor, region.upper, chunk_next, chunk_bytes)) {
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
    region.free_bytes += size;
    mem.write32(region.header + MH_FREE, region.free_bytes);
}

void Exec::add_resource(u32 node) {
    if (m_ == nullptr || node == 0) {
        return;
    }
    Bus b(*m_);
    enqueue(b, base_ + ResourceList, node);
}

u32 Exec::make_native_node(const char* name, u16 version, u8 node_type, u32 list_offset,
                           const NativeVector* fns, u32 count, void* ctx, u32 pos_size) {
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
    b.w8(lib + LN_TYPE, node_type);
    b.w8(lib + LIB_FLAGS, LIBF_SUMUSED | LIBF_CHANGED);
    b.w16(lib + LIB_NEGSIZE, neg);
    b.w16(lib + LIB_POSSIZE, pos);
    b.w16(lib + LIB_VERSION, version);
    ExecOps::sum_library(b, lib);
    enqueue(b, base_ + list_offset, lib);
    jit.flush();
    if (!b.ok()) {
        return 0;
    }
    ++library_count_;
    return lib;
}

u32 Exec::make_native_library(const char* name, u16 version, const NativeVector* fns, u32 count,
                              void* ctx, u32 pos_size) {
    return make_native_node(name, version, NT_LIBRARY, LibList, fns, count, ctx, pos_size);
}

u32 Exec::make_native_device(const char* name, u16 version, const NativeVector* fns, u32 count,
                             void* ctx, u32 pos_size) {
    return make_native_node(name, version, NT_DEVICE, DeviceList, fns, count, ctx, pos_size);
}

u32 Exec::spawn(u32 task, u32 init_pc, u32 final_pc, bool detached) {
    if (m_ == nullptr) {
        return 0;
    }
    Bus b(*m_);
    if (task == 0) {
        fault_null(b);
        return 0;
    }
    if (sched_.find(task) >= 0) {
        LogLine().text("exec: AddTask of already scheduled task at ").hex(task).send();
        return 0;
    }
    const i8 pri = static_cast<i8>(b.r8(task + LN_PRI));
    const u32 sp_reg = b.r32(task + TC_SPREG);
    const u32 existing_alloc = b.r32(task + TC_SIGALLOC);
    if (!b.ok()) {
        return 0;
    }
    const u32 child_sp = sp_reg - 4;
    b.w32(child_sp, final_pc != 0 ? final_pc : task_exit_stub_);
    if (!b.ok()) {
        return 0;
    }
    const i32 id = sched_.add(task, pri);
    if (id < 0) {
        return 0;
    }
    if (task_host_.create != nullptr &&
        !task_host_.create(task_host_.ctx, static_cast<u32>(id))) {
        sched_.remove(id);
        sched_.release(id);
        return 0;
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
    enqueue(b, base_ + TaskReady, task);
    if (!b.ok()) {
        sched_.remove(id);
        reap_removed();
        return 0;
    }
    TaskContext& tc = tasks_[id];
    tc = TaskContext{};
    tc.cpu.pc = init_pc;
    tc.cpu.a[7] = child_sp;
    tc.run_state = m68k::Jit::RunState{~0ull, 0};
    tc.call_depth = 0;
    tc.task_addr = task;
    tc.parent = task_;
    tc.budget = ~0ull;
    tc.td_nest = 0xFF;
    tc.id_nest = 0xFF;
    tc.detached = detached;
    // A task a program starts is part of it, unless a library being loaded
    // starts it for itself.
    const i32 creator = sched_.current();
    if (creator >= 0 && creator != id && load_depth_ == 0) {
        tc.program = tasks_[creator].program;
    }
    // A detached task starts with the caller's D0 and A0, which carry a
    // command's argument line.
    if (detached) {
        tc.cpu.d[0] = m_->cpu.d[0];
        tc.cpu.a[0] = m_->cpu.a[0];
    }
    if (sched_.should_switch_now()) {
        pending_switch_ = true;
    }
    return task;
}

u32 Exec::spawn_native(u32 task, void (*body)(void* arg), void* arg, i8 pri, bool detached) {
    if (m_ == nullptr || body == nullptr) {
        return 0;
    }
    Bus b(*m_);
    if (task == 0) {
        fault_null(b);
        return 0;
    }
    if (sched_.find(task) >= 0) {
        LogLine().text("exec: AddTask of already scheduled task at ").hex(task).send();
        return 0;
    }
    const u32 sp_reg = b.r32(task + TC_SPREG);
    const u32 existing_alloc = b.r32(task + TC_SIGALLOC);
    if (!b.ok()) {
        return 0;
    }
    const u32 child_sp = sp_reg - 4;
    b.w32(child_sp, task_exit_stub_);
    if (!b.ok()) {
        return 0;
    }
    const i32 id = sched_.add(task, pri);
    if (id < 0) {
        return 0;
    }
    if (task_host_.create != nullptr &&
        !task_host_.create(task_host_.ctx, static_cast<u32>(id))) {
        sched_.remove(id);
        sched_.release(id);
        return 0;
    }
    if (b.r8(task + LN_TYPE) == 0) {
        b.w8(task + LN_TYPE, NT_TASK);
    }
    b.w8(task + LN_PRI, static_cast<u8>(pri));
    b.w8(task + TC_STATE, TS_READY);
    b.w8(task + TC_IDNESTCNT, 0xFF);
    b.w8(task + TC_TDNESTCNT, 0xFF);
    if (existing_alloc == 0) {
        b.w32(task + TC_SIGALLOC, 0x0000FFFF);
    }
    b.w32(task + TC_SIGWAIT, 0);
    b.w32(task + TC_SIGRECVD, 0);
    b.w32(task + TC_SPREG, child_sp);
    enqueue(b, base_ + TaskReady, task);
    if (!b.ok()) {
        sched_.remove(id);
        reap_removed();
        return 0;
    }
    TaskContext& tc = tasks_[id];
    tc = TaskContext{};
    tc.cpu.a[7] = child_sp;
    tc.run_state = m68k::Jit::RunState{~0ull, 0};
    tc.task_addr = task;
    tc.parent = task_;
    tc.budget = ~0ull;
    tc.detached = detached;
    tc.native_body = body;
    tc.native_arg = arg;
    tc.native_task = true;
    if (sched_.should_switch_now()) {
        pending_switch_ = true;
    }
    return task;
}

void Exec::set_task_end_hook(void (*hook)(void* ctx, u32 task, bool self), void* ctx) {
    TaskEndHook* free_slot = nullptr;
    for (TaskEndHook& h : task_end_hooks_) {
        if (h.hook != nullptr && h.ctx == ctx) {
            h = hook != nullptr ? TaskEndHook{hook, ctx} : TaskEndHook{};
            return;
        }
        if (h.hook == nullptr && free_slot == nullptr) {
            free_slot = &h;
        }
    }
    if (hook == nullptr) {
        return;
    }
    if (free_slot == nullptr) {
        LogLine().text("exec: task end hooks full").send();
        return;
    }
    *free_slot = TaskEndHook{hook, ctx};
}

void Exec::task_ended(u32 task, bool self) {
    for (const TaskEndHook& h : task_end_hooks_) {
        if (h.hook != nullptr) {
            h.hook(h.ctx, task, self);
        }
    }
}

bool Exec::add_timer_hook(const TimerHook& hook) {
    for (TimerHook& h : timer_hooks_) {
        if (h.expire == nullptr || h.ctx == hook.ctx) {
            h = hook;
            return true;
        }
    }
    return false;
}

bool Exec::add_timer_drop_hook(void (*hook)(void* ctx, u32 task), void* ctx) {
    for (DropHook& h : timer_drop_hooks_) {
        if (h.fn == nullptr || (h.fn == hook && h.ctx == ctx)) {
            h = DropHook{hook, ctx};
            return true;
        }
    }
    return false;
}

void Exec::drop_timer_requests_of(u32 task) {
    for (const DropHook& h : timer_drop_hooks_) {
        if (h.fn != nullptr) {
            h.fn(h.ctx, task);
        }
    }
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

bool Exec::task_info(u32 slot, TaskInfo& out) const {
    if (m_ == nullptr || slot >= Scheduler::kMaxTasks) {
        return false;
    }
    const Scheduler::Slot& s = sched_.slot(slot);
    if (s.state != TaskState::Ready && s.state != TaskState::Running &&
        s.state != TaskState::Waiting) {
        return false;
    }
    out = TaskInfo{};
    out.task_addr = s.task_addr;
    out.state = s.state;
    out.priority = s.priority;
    out.suspended = s.suspended;

    // The monitor reads from the interrupt: a fault must not reach the CPU
    // of the code it interrupted, so it lands in a copy.
    m68k::Machine probe = *m_;
    u32 td = 0xFF;
    u32 id = 0xFF;
    if (s.state == TaskState::Running) {
        probe.mem.read8(base_ + TDNestCnt, td);
        probe.mem.read8(base_ + IDNestCnt, id);
    } else {
        td = tasks_[slot].td_nest;
        id = tasks_[slot].id_nest;
    }
    out.forbid = td != 0xFF;
    out.disable = id != 0xFF;

    native::Bus b(probe);
    const u32 name = s.task_addr != 0 ? b.r32(s.task_addr + LN_NAME) : 0;
    if (name != 0 && !b.string(name, out.name, sizeof out.name - 1) && !b.ok()) {
        out.name[0] = '\0';
    }
    return true;
}

bool Exec::task_context(u32 slot, m68k::CpuState& cpu, const char*& native) const {
    TaskInfo info;
    if (!task_info(slot, info)) {
        return false;
    }
    const bool running = info.state == TaskState::Running;
    cpu = running ? m_->cpu : tasks_[slot].cpu;
    native = running ? m_->jit->current_native() : tasks_[slot].run_state.native;
    return true;
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
    // The bit belongs to the task the port signals, which need not be the
    // one deleting it: IPrefs closes the windows the Workbench opened. A
    // task that has ended keeps nothing to free.
    const u32 owner = b.r32(port + mp_SigTask);
    if (owner == task_) {
        ExecOps::free_signal(*this, b, sig);
    } else if (sig < 32 && ExecOps::known_task(*this, owner)) {
        ExecOps::store_sig_alloc_of(*this, b, owner,
                                    ExecOps::sig_alloc_of(*this, b, owner) & ~(1u << sig));
    }
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

bool Exec::reply_message(u32 message) {
    if (m_ == nullptr) {
        return false;
    }
    Bus b(*m_);
    return ExecOps::reply_msg(*this, b, message);
}

bool Exec::reply_timer_message(u32 message) {
    if (m_ == nullptr) {
        return false;
    }
    const ExitReason saved_exit = m_->cpu.exit;
    const u32 saved_fault = m_->cpu.fault_addr;
    const u32 saved_trap = m_->cpu.trap_number;
    m_->cpu.exit = ExitReason::None;
    Bus b(*m_);
    const u32 port = b.r32(message + mn_ReplyPort);
    bool replyable = port == 0;
    if (b.ok() && port != 0) {
        replyable = ExecOps::known_task(*this, b.r32(port + mp_SigTask));
    }
    const bool replied = b.ok() && replyable && ExecOps::reply_msg(*this, b, message);
    if (!b.ok()) {
        LogLine().text("exec: timer reply faulted at ").hex(m_->cpu.fault_addr).send();
    }
    m_->cpu.exit = saved_exit;
    m_->cpu.fault_addr = saved_fault;
    m_->cpu.trap_number = saved_trap;
    return replied;
}

u64 Exec::now_us() const {
    return task_host_.now_us != nullptr ? task_host_.now_us(task_host_.ctx) : 0;
}

bool Exec::has_pending_timers() const {
    for (u32 i = 0; i < Scheduler::kMaxTasks; ++i) {
        if (tasks_[i].sleeping) {
            return true;
        }
    }
    for (const TimerHook& h : timer_hooks_) {
        if (h.has_pending != nullptr && h.has_pending(h.ctx)) {
            return true;
        }
    }
    return false;
}

void Exec::expire_timers() {
    if (m_ == nullptr || base_ == 0 || task_host_.now_us == nullptr) {
        return;
    }
    const u64 now = task_host_.now_us(task_host_.ctx);
    // Timers belong to every task, not to the one that happens to be running:
    // a fault while completing one is dropped instead of ending that run.
    const ExitReason saved_exit = m_->cpu.exit;
    const u32 saved_fault = m_->cpu.fault_addr;
    const u32 saved_trap = m_->cpu.trap_number;
    m_->cpu.exit = ExitReason::None;
    Bus b(*m_);
    for (u32 i = 0; i < Scheduler::kMaxTasks; ++i) {
        if (!tasks_[i].sleeping || now < tasks_[i].sleep_deadline_us) {
            continue;
        }
        tasks_[i].sleeping = false;
        if (sched_.slot(i).state == TaskState::Waiting) {
            const u32 addr = tasks_[i].task_addr;
            if (addr != 0) {
                unlink_task(addr);
                b.w8(addr + TC_STATE, TS_READY);
            }
            sched_.wake(static_cast<i32>(i));
            if (addr != 0) {
                enqueue(b, base_ + TaskReady, addr);
            }
            if (sched_.should_switch_now()) {
                pending_switch_ = true;
            }
        }
    }
    bool faulted = !b.ok();
    for (const TimerHook& h : timer_hooks_) {
        if (h.expire != nullptr) {
            h.expire(h.ctx, now);
            faulted = faulted || m_->cpu.exit != ExitReason::None;
        }
    }
    if (faulted) {
        LogLine().text("exec: timer reply faulted at ").hex(m_->cpu.fault_addr).send();
    }
    m_->cpu.exit = saved_exit;
    m_->cpu.fault_addr = saved_fault;
    m_->cpu.trap_number = saved_trap;
}

void Exec::sleep_us(u64 us) {
    if (m_ == nullptr || us == 0 || task_host_.now_us == nullptr) {
        return;
    }
    const i32 cur = sched_.current();
    if (cur < 0 || static_cast<u32>(cur) >= Scheduler::kMaxTasks) {
        return;
    }
    const u64 now = task_host_.now_us(task_host_.ctx);
    const u64 deadline = (~u64(0) - now < us) ? ~u64(0) : now + us;
    tasks_[cur].sleeping = true;
    tasks_[cur].sleep_deadline_us = deadline;

    const u32 cur_addr = tasks_[cur].task_addr;
    Bus b(*m_);
    if (cur != 0 && cur_addr != 0) {
        const u32 recvd = b.r32(cur_addr + TC_SIGRECVD);
        if (b.ok() && (recvd & SIGBREAKF_CTRL_C) != 0) {
            b.w32(cur_addr + TC_SIGRECVD, recvd & ~SIGBREAKF_CTRL_C);
            tasks_[cur].sleeping = false;
            end_run(*m_, ExitReason::Break);
            return;
        }
    }
    if (cur_addr != 0) {
        b.w32(cur_addr + TC_SIGWAIT, 0);
        b.w8(cur_addr + TC_STATE, TS_WAIT);
        enqueue(b, base_ + TaskWait, cur_addr);
        if (!b.ok()) {
            tasks_[cur].sleeping = false;
            return;
        }
    }
    sched_.wait(cur);

    // Ctrl+C wakes a non-boot task out of its sleep so that Break ends its
    // command; the boot task keeps its old end-of-delay check below.
    auto ctrl_c_break = [&]() -> bool {
        if (cur == 0 || cur_addr == 0) {
            return false;
        }
        const u32 recvd = b.r32(cur_addr + TC_SIGRECVD);
        if (!b.ok() || (recvd & SIGBREAKF_CTRL_C) == 0) {
            return false;
        }
        b.w32(cur_addr + TC_SIGRECVD, recvd & ~SIGBREAKF_CTRL_C);
        tasks_[cur].sleeping = false;
        end_run(*m_, ExitReason::Break);
        return true;
    };

    // A signal wait_for waits on ends the sleep before its time.
    auto signalled = [&]() -> bool {
        const u32 wanted = tasks_[cur].sleep_signals;
        return wanted != 0 && (ExecOps::signals(*this, b) & wanted) != 0 && b.ok();
    };

    while (tasks_[cur].sleeping && m_->cpu.exit == ExitReason::None) {
        expire_timers();
        if (tasks_[cur].sleeping && signalled()) {
            tasks_[cur].sleeping = false;
        }
        if (!tasks_[cur].sleeping) {
            if (sched_.slot(static_cast<u32>(cur)).state != TaskState::Running) {
                ExecOps::restore_task_from_wait(*this, b, cur, cur_addr);
            }
            break;
        }
        i32 next = sched_.pick_next();
        while (next < 0) {
            expire_timers();
            if (m_->cpu.exit != ExitReason::None) {
                tasks_[cur].sleeping = false;
                ExecOps::restore_task_from_wait(*this, b, cur, cur_addr);
                return;
            }
            next = sched_.pick_next();
            if (next >= 0) {
                break;
            }
            const bool can_wake = ExecOps::poll_idle(*this);
            if (m_->cpu.exit != ExitReason::None) {
                tasks_[cur].sleeping = false;
                ExecOps::restore_task_from_wait(*this, b, cur, cur_addr);
                return;
            }
            expire_timers();
            if (m_->cpu.exit != ExitReason::None) {
                tasks_[cur].sleeping = false;
                ExecOps::restore_task_from_wait(*this, b, cur, cur_addr);
                return;
            }
            next = sched_.pick_next();
            const bool still_timer = task_host_.now_us != nullptr && has_pending_timers();
            if (next < 0 && !can_wake && !still_timer) {
                tasks_[cur].sleeping = false;
                ExecOps::restore_task_from_wait(*this, b, cur, cur_addr);
                return;
            }
        }
        if (!tasks_[cur].sleeping) {
            if (sched_.slot(static_cast<u32>(cur)).state != TaskState::Running) {
                ExecOps::restore_task_from_wait(*this, b, cur, cur_addr);
            }
            break;
        }
        if (next == cur) {
            ExecOps::restore_task_from_wait(*this, b, cur, cur_addr);
            if (ctrl_c_break()) {
                return;
            }
            continue;
        }
        if (task_host_.switch_to == nullptr || !switch_to_task(next)) {
            tasks_[cur].sleeping = false;
            ExecOps::restore_task_from_wait(*this, b, cur, cur_addr);
            return;
        }
        if (sched_.current() != cur) {
            return;
        }
        if (ctrl_c_break()) {
            return;
        }
        if (abort_to_boot_task_ && cur == 0) {
            tasks_[cur].sleeping = false;
            end_run(*m_, ExitReason::Unimplemented);
            return;
        }
        if (m_->cpu.exit != ExitReason::None) {
            tasks_[cur].sleeping = false;
            return;
        }
    }

    if (cur == 0 && m_->cpu.exit == ExitReason::None) {
        const u32 after = ExecOps::signals(*this, b);
        if (b.ok() && (after & SIGBREAKF_CTRL_C) != 0) {
            ExecOps::store_signals(*this, b, after & ~SIGBREAKF_CTRL_C);
            LogLine().text("exec: Delay interrupted by Ctrl+C").send();
            end_run(*m_, ExitReason::Break);
        }
    }
}

u32 Exec::wait_for(u32 mask, u64 us) {
    if (m_ == nullptr) {
        return 0;
    }
    const i32 cur = sched_.current();
    if (cur < 0 || static_cast<u32>(cur) >= Scheduler::kMaxTasks) {
        return 0;
    }
    Bus b(*m_);
    auto take = [&]() -> u32 {
        const u32 pending = ExecOps::signals(*this, b);
        const u32 got = b.ok() ? pending & mask : 0;
        if (got != 0) {
            ExecOps::store_signals(*this, b, pending & ~got);
        }
        return got;
    };
    const u32 got = take();
    if (got != 0 || us == 0 || !b.ok()) {
        return got;
    }
    tasks_[cur].sleep_signals = mask;
    sleep_us(us);
    tasks_[cur].sleep_signals = 0;
    if (m_->cpu.exit != ExitReason::None || sched_.current() != cur) {
        return 0;
    }
    return take();
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
        if (sched_.slot(0).state == TaskState::Waiting && (mask & SIGBREAKF_CTRL_C) != 0) {
            sched_.wake(0);
        }
    }
}

void Exec::signal_task(u32 task, u32 mask) {
    if (m_ == nullptr || task == 0) {
        return;
    }
    Bus b(*m_);
    ExecOps::signal_task(*this, b, task, mask);
}

void Exec::reset_task_signals() {
    sig_alloc_ = 0x0000FFFF;
    signals_ = 0;
    port_signals_ = 0;
    every_task_waits_logged_ = false;
    abort_to_boot_task_ = false;
    tasks_[0].sleeping = false;
    tasks_[0].sleep_deadline_us = 0;
    reset_task_state(tasks_[0].task_addr != 0 ? tasks_[0].task_addr : task_);
}

void Exec::set_reply_handler(u32 port, void (*handler)(void* ctx, u32 message), void* ctx) {
    ReplyHandler* free_slot = nullptr;
    for (ReplyHandler& h : reply_handlers_) {
        if (h.port == port) {
            h = ReplyHandler{port, handler, ctx};
            return;
        }
        if (free_slot == nullptr && h.port == 0) {
            free_slot = &h;
        }
    }
    if (free_slot != nullptr) {
        *free_slot = ReplyHandler{port, handler, ctx};
    } else {
        LogLine().text("exec: no room for the reply handler of port ").hex(port).send();
    }
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

void Exec::request_rescue(Rescue op, u32 slot, u32 task_addr) {
    rescue_slot_ = slot;
    rescue_task_ = task_addr;
    rescue_op_ = op;
    if (m_ != nullptr) {
        m_->jit->raise_attention();
    }
}

void Exec::run_rescue() {
    const Rescue op = rescue_op_;
    if (op == Rescue::None || m_ == nullptr) {
        return;
    }
    // The rescue runs on whatever task is running: a task whose Task or
    // lists are damaged must not fault that one. A fault on the way stays
    // the rescue's; only the Stopped of the running task's own end counts.
    CpuState& cpu = m_->cpu;
    const ExitReason exit = cpu.exit;
    const u32 trap_number = cpu.trap_number;
    const u32 fault_addr = cpu.fault_addr;
    run_rescue_now();
    if (cpu.exit != exit && cpu.exit != ExitReason::Stopped) {
        LogLine().text("exec: a fault while ending a task, at ").hex(cpu.fault_addr)
            .text(", was ignored").send();
        cpu.exit = exit;
        cpu.trap_number = trap_number;
        cpu.fault_addr = fault_addr;
    }
}

void Exec::run_rescue_now() {
    const Rescue op = rescue_op_;
    if (op == Rescue::None) {
        return;
    }
    const u32 slot = rescue_slot_;
    const u32 task = rescue_task_;
    rescue_op_ = Rescue::None;
    TaskInfo info;
    if (!task_info(slot, info) || info.task_addr != task) {
        return;
    }
    const i32 id = static_cast<i32>(slot);
    if (op == Rescue::End) {
        sched_.resume(id);
        end_for_rescue(slot, info);
        return;
    }
    if (op == Rescue::Suspend) {
        sched_.suspend(id);
    } else {
        sched_.resume(id);
        pending_switch_ = true;
    }
    LogLine()
        .text(op == Rescue::Suspend ? "exec: suspended " : "exec: resumed ")
        .text(info.name)
        .send();
}

void Exec::end_for_rescue(u32 slot, const TaskInfo& info) {
    const i32 id = static_cast<i32>(slot);
    const bool current = id == sched_.current();
    const TaskContext& tc = tasks_[slot];
    // The running task, the boot task, which is never removed, and a shell
    // or the Workbench, whose task outlives the command it runs, only have
    // their 68000 code ended. A task that runs none is left alone.
    if (current && m_->jit->run_depth() != 0) {
        doom_program_of(slot);
        m_->jit->request_stop();
        if (info.state == TaskState::Waiting) {
            end_run(*m_, ExitReason::Stopped);
        }
    } else if (!current && slot == 0 && tc.run_state.run_depth != 0) {
        doom_program_of(slot);
        tasks_[0].run_state.stop = true;
        signal_task(info.task_addr, SIGBREAKF_CTRL_C);
    } else if (!current && slot != 0 && tc.native_task && tc.run_state.run_depth != 0) {
        doom_program_of(slot);
        stop_command_of(slot);
    } else if (!current && slot != 0 && !tc.native_task) {
        LogLine().text("exec: removing ").text(info.name).send();
        doom_program_of(slot);
        Bus b(*m_);
        ExecOps::rem_task_id(*this, b, id);
        return;
    } else if ((slot == 0 || tc.native_task) && end_hook_ != nullptr &&
               end_hook_(end_hook_ctx_, info.task_addr)) {
        LogLine().text("exec: closing ").text(info.name).send();
        return;
    } else {
        LogLine().text("exec: cannot end ").text(info.name).send();
        return;
    }
    LogLine().text("exec: ending ").text(info.name).send();
}

// Ends the 68000 code a task that is not running is in, as request_stop
// ends the running task's: the task's next block, or the wait it is in,
// ends with Stopped. A waiting task is woken for it.
void Exec::stop_command_of(u32 slot) {
    TaskContext& tc = tasks_[slot];
    tc.run_state.stop = true;
    tc.cpu.exit = ExitReason::Stopped;
    const i32 id = static_cast<i32>(slot);
    if (sched_.slot(slot).state != TaskState::Waiting) {
        return;
    }
    Bus b(*m_);
    const bool linked = tc.task_addr != 0 && unlink_task(tc.task_addr);
    if (tc.task_addr != 0) {
        b.w8(tc.task_addr + TC_STATE, TS_READY);
    }
    sched_.wake(id);
    if (!linked) {
        rebuild_task_lists();
    } else {
        enqueue(b, base_ + TaskReady, tc.task_addr);
    }
}

// Takes a task's node off TaskReady or TaskWait. When its links do not
// lead back to it, as when a program wrote over its Task, nothing is
// written through them: the lists are built again without the task, and
// the answer is false.
bool Exec::unlink_task(u32 addr) {
    const GuestMemory& mem = m_->mem;
    u32 succ = 0;
    u32 pred = 0;
    u32 back = 0;
    u32 forth = 0;
    if (!mem.read32(addr + LN_SUCC, succ) || !mem.read32(addr + LN_PRED, pred) || succ == 0 ||
        pred == 0 || (succ & 1) != 0 || (pred & 1) != 0 || !mem.read32(succ + LN_PRED, back) ||
        !mem.read32(pred + LN_SUCC, forth) || back != addr || forth != addr) {
        LogLine().text("exec: task at ").hex(addr).text(" is off its list").send();
        rebuild_task_lists(addr);
        return false;
    }
    Bus b(*m_);
    remove(b, addr);
    return b.ok();
}

// Builds TaskReady and TaskWait again from the scheduler, each task by its
// priority, as they would be had nothing written over them; the task at
// leave_out, whose node the caller takes care of, is left off.
void Exec::rebuild_task_lists(u32 leave_out) {
    Bus b(*m_);
    new_list(b, base_ + TaskReady, NT_TASK);
    new_list(b, base_ + TaskWait, NT_TASK);
    for (u32 i = 0; i < Scheduler::kMaxTasks; ++i) {
        const Scheduler::Slot s = sched_.slot(i);
        const u32 addr = tasks_[i].task_addr != 0 ? tasks_[i].task_addr : s.task_addr;
        // The scheduler's state alone decides: the running task may be
        // waiting, when the rescue runs from its idle poll.
        if (addr == 0 || addr == leave_out) {
            continue;
        }
        if (s.state == TaskState::Ready) {
            enqueue(b, base_ + TaskReady, addr);
        } else if (s.state == TaskState::Waiting) {
            enqueue(b, base_ + TaskWait, addr);
        }
    }
}

// The program the task in slot started, marked to have its blocks freed
// once its last task is gone. A task that only took its program over from
// its parent is not the program: ending it frees nothing.
void Exec::doom_program_of(u32 slot) {
    const TaskContext& tc = tasks_[slot];
    if (!tc.program_root) {
        return;
    }
    if (Program* p = find_program(tc.program)) {
        p->doomed = true;
        tell_program_ended(p->id);
    }
}

void Exec::tell_program_ended(u32 id) {
    for (const EndHook& h : end_hooks_) {
        if (h.fn != nullptr) {
            h.fn(h.ctx, id);
        }
    }
}

bool Exec::add_program_end_hook(void (*hook)(void* ctx, u32 program), void* ctx) {
    if (hook == nullptr) {
        return false;
    }
    for (EndHook& h : end_hooks_) {
        if (h.fn == hook && h.ctx == ctx) {
            return true;
        }
        if (h.fn == nullptr) {
            h = EndHook{hook, ctx};
            return true;
        }
    }
    return false;
}

bool Exec::program_holds(u32 program, u32 addr) const {
    const Program* p = nullptr;
    for (const Program& candidate : programs_) {
        if (program != 0 && candidate.id == program) {
            p = &candidate;
        }
    }
    if (p == nullptr || m_ == nullptr) {
        return false;
    }
    if (in_program_code(*p, addr)) {
        return true;
    }
    u32 lo = 0;
    u32 hi = program_block_count_;
    while (lo < hi) {
        const u32 mid = (lo + hi) / 2;
        if (program_blocks_[mid].addr <= addr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return false;
    }
    const ProgramBlock& block = program_blocks_[lo - 1];
    return block.program == program && static_cast<u64>(block.addr) + block.bytes > addr;
}

void Exec::stop_code_of(u32 task) {
    if (m_ == nullptr) {
        return;
    }
    const i32 id = slot_of(native::guest_address(*m_, task));
    if (id < 0) {
        return;
    }
    if (id == sched_.current()) {
        if (m_->jit->run_depth() != 0) {
            m_->jit->request_stop();
        }
        return;
    }
    if (tasks_[id].run_state.run_depth != 0) {
        stop_command_of(static_cast<u32>(id));
    }
}

bool Exec::let_run(u32 task) {
    if (m_ == nullptr || task_host_.switch_to == nullptr) {
        return false;
    }
    const i32 id = slot_of(native::guest_address(*m_, task));
    if (id < 0 || id == sched_.current() ||
        sched_.slot(static_cast<u32>(id)).state != TaskState::Ready) {
        return false;
    }
    return switch_to_task(id);
}

i32 Exec::slot_of(u32 task) const {
    if (task == 0) {
        return -1;
    }
    const i32 id = sched_.find(task);
    if (id >= 0) {
        return id;
    }
    return task == tasks_[0].task_addr ? 0 : -1;
}

Exec::Program* Exec::find_program(u32 id) {
    if (id == 0) {
        return nullptr;
    }
    for (Program& p : programs_) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

// A task that removed itself still runs until another takes over: its
// program is not over before then.
bool Exec::program_alive(u32 id) const {
    for (u32 i = 0; i < Scheduler::kMaxTasks; ++i) {
        const TaskState st = sched_.slot(i).state;
        const bool alive = st == TaskState::Ready || st == TaskState::Running ||
                           st == TaskState::Waiting ||
                           (st == TaskState::Removed && static_cast<i32>(i) == sched_.current());
        if (alive && tasks_[i].program == id) {
            return true;
        }
    }
    return false;
}

bool Exec::in_program_code(const Program& p, u32 pc) const {
    const GuestMemory& mem = m_->mem;
    for (u32 seglist : p.code) {
        u32 bptr = seglist;
        for (u32 n = 0; bptr != 0 && n < kMaxSegments; ++n) {
            const u64 link = static_cast<u64>(bptr) << 2;
            u32 size = 0;
            u32 next = 0;
            if (link < 4 || link + 4 > mem.size || !mem.read32(static_cast<u32>(link - 4), size) ||
                !mem.read32(static_cast<u32>(link), next)) {
                break;
            }
            if (pc >= link + 4 && pc < link - 4 + static_cast<u64>(size)) {
                return true;
            }
            bptr = next;
        }
    }
    return false;
}

// The program whose code called the native that runs: the running task's,
// when the return address on its stack lies in the program's seglists and
// no library is being loaded. 0 otherwise.
u32 Exec::calling_program() const {
    if (load_depth_ != 0) {
        return 0;
    }
    const i32 cur = sched_.current();
    if (cur < 0) {
        return 0;
    }
    const u32 id = tasks_[cur].program;
    if (id == 0) {
        return 0;
    }
    const Program* program = nullptr;
    for (const Program& p : programs_) {
        if (p.id == id) {
            program = &p;
        }
    }
    const u32 mask = m_->jit->address_mask();
    u32 ret = 0;
    if (program == nullptr || !m_->mem.read32(m_->cpu.a[7] & mask, ret)) {
        return 0;
    }
    return in_program_code(*program, ret & mask) ? id : 0;
}

u32 Exec::begin_program(u32 task, u32 seglist, bool fresh) {
    if (m_ == nullptr) {
        return 0;
    }
    const i32 id = slot_of(native::guest_address(*m_, task));
    if (id < 0) {
        return 0;
    }
    TaskContext& tc = tasks_[id];
    if (tc.program != 0 && fresh) {
        if (tc.program_root) {
            end_program(task);
        } else {
            tc.program = 0;
        }
    }
    if (tc.program != 0) {
        if (Program* p = find_program(tc.program)) {
            for (u32& code : p->code) {
                if (code == seglist) {
                    break;
                }
                if (code == 0) {
                    code = seglist;
                    break;
                }
            }
        }
        return 0;
    }
    for (Program& p : programs_) {
        if (p.id != 0) {
            continue;
        }
        p = Program{};
        p.id = next_program_++;
        if (next_program_ == 0) {
            next_program_ = 1;
        }
        p.code[0] = seglist;
        tc.program = p.id;
        tc.program_root = true;
        return p.id;
    }
    return 0;
}

void Exec::end_program(u32 task) {
    if (m_ == nullptr) {
        return;
    }
    const i32 id = slot_of(native::guest_address(*m_, task));
    if (id < 0 || !tasks_[id].program_root) {
        return;
    }
    const u32 program = tasks_[id].program;
    tasks_[id].program = 0;
    tasks_[id].program_root = false;
    // The task outlives a command the system monitor ended, so what a
    // task's end drops goes now: a device would otherwise reply a request
    // into the command's memory after it is freed. A command that ends by
    // itself leaves its requests as before.
    const Program* p = find_program(program);
    if (p != nullptr && p->doomed) {
        drop_timer_requests_of(tasks_[id].task_addr);
    }
    settle_program(program);
}

void Exec::claim_for_program(u32 addr, u32 bytes, u8 release) {
    if (m_ == nullptr || addr == 0 || bytes == 0 || bytes > 0xFFFFFFF0u ||
        release > kMaxReleasers || (release != 0 && releasers_[release].fn == nullptr)) {
        return;
    }
    const u32 program = calling_program();
    if (program != 0) {
        record_program_block(addr, round8(bytes), program, release);
    }
}

u8 Exec::add_program_releaser(void (*release)(void* ctx, u32 addr), void* ctx) {
    if (release == nullptr) {
        return 0;
    }
    for (u32 i = 1; i <= kMaxReleasers; ++i) {
        if (releasers_[i].fn == release && releasers_[i].ctx == ctx) {
            return static_cast<u8>(i);
        }
        if (releasers_[i].fn == nullptr) {
            releasers_[i] = Releaser{release, ctx};
            return static_cast<u8>(i);
        }
    }
    return 0;
}

bool Exec::add_kept_list(u32 head, bool bptr_chain) {
    if (head == 0 || kept_list_count_ == kMaxKeptLists) {
        return false;
    }
    kept_lists_[kept_list_count_++] = KeptList{head, bptr_chain};
    return true;
}

bool Exec::add_program_keeper(void (*keep)(void* ctx), void* ctx) {
    if (keep == nullptr) {
        return false;
    }
    for (Keeper& k : keepers_) {
        if (k.keep == keep && k.ctx == ctx) {
            return true;
        }
        if (k.keep == nullptr) {
            k = Keeper{keep, ctx};
            return true;
        }
    }
    return false;
}

// The index of the first block at or above addr, program_block_count_ if
// none is.
u32 Exec::first_block_at_or_after(u32 addr) const {
    u32 lo = 0;
    u32 hi = program_block_count_;
    while (lo < hi) {
        const u32 mid = (lo + hi) / 2;
        if (program_blocks_[mid].addr < addr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

void Exec::erase_program_block(u32 index) {
    for (u32 i = index + 1; i < program_block_count_; ++i) {
        program_blocks_[i - 1] = program_blocks_[i];
    }
    --program_block_count_;
}

// A block that overlaps one already recorded means the record is wrong
// somewhere: neither stays, so that neither can be freed.
void Exec::record_program_block(u32 addr, u32 bytes, u32 program, u8 release) {
    const u64 end = static_cast<u64>(addr) + bytes;
    u32 at = first_block_at_or_after(addr);
    bool overlapped = false;
    if (at < program_block_count_ && program_blocks_[at].addr < end) {
        erase_program_block(at);
        overlapped = true;
    }
    if (at > 0 && static_cast<u64>(program_blocks_[at - 1].addr) + program_blocks_[at - 1].bytes >
                      addr) {
        erase_program_block(at - 1);
        --at;
        overlapped = true;
    }
    if (overlapped || program_block_count_ == kMaxProgramBlocks) {
        return;
    }
    for (u32 i = program_block_count_; i > at; --i) {
        program_blocks_[i] = program_blocks_[i - 1];
    }
    program_blocks_[at] = ProgramBlock{addr, bytes, program, release, false};
    ++program_block_count_;
}

void Exec::drop_program_blocks(u32 addr, u32 bytes) {
    if (program_block_count_ == 0) {
        return;
    }
    const u64 end = static_cast<u64>(addr) + ((static_cast<u64>(bytes) + 7) & ~u64(7));
    u32 i = end > 0xFFFFFFFFull ? program_block_count_
                                : first_block_at_or_after(static_cast<u32>(end));
    while (i > 0) {
        const ProgramBlock& block = program_blocks_[i - 1];
        if (static_cast<u64>(block.addr) + block.bytes <= addr) {
            break;
        }
        erase_program_block(i - 1);
        --i;
    }
}

// Frees or forgets the blocks of a program that has no task left.
void Exec::settle_program(u32 id) {
    Program* p = find_program(id);
    if (p == nullptr || program_alive(id)) {
        return;
    }
    const bool doomed = p->doomed;
    const bool keep_ports = p->uses_commodities;
    if (doomed) {
        // What a device took from the program after the End goes too,
        // while its code and blocks are still known.
        tell_program_ended(id);
        p = find_program(id);
        if (p == nullptr) {
            return;
        }
    }
    *p = Program{};
    if (doomed) {
        free_program_blocks(id, keep_ports);
        return;
    }
    for (u32 i = 0; i < program_block_count_;) {
        if (program_blocks_[i].program == id) {
            erase_program_block(i);
        } else {
            ++i;
        }
    }
}

void Exec::keep_block_holding(u32 addr) {
    const u32 at = first_block_at_or_after(addr == 0xFFFFFFFFu ? addr : addr + 1);
    if (at == 0) {
        return;
    }
    ProgramBlock& block = program_blocks_[at - 1];
    if (static_cast<u64>(block.addr) + block.bytes > addr) {
        block.keep = true;
    }
}

void Exec::keep_blocks_within(u32 lo, u32 hi) {
    if (hi <= lo) {
        return;
    }
    u32 at = first_block_at_or_after(hi);
    while (at > 0) {
        ProgramBlock& block = program_blocks_[at - 1];
        if (static_cast<u64>(block.addr) + block.bytes <= lo) {
            break;
        }
        block.keep = true;
        --at;
    }
}

// Keeps the blocks that hold a node of the exec List at list, and with
// with_messages those that hold a message queued on a port of it.
void Exec::keep_list_nodes(u32 list, bool with_messages) {
    constexpr u32 kMaxNodes = 4096;
    const GuestMemory& mem = m_->mem;
    u32 node = 0;
    if (!mem.read32(list + LH_HEAD, node)) {
        return;
    }
    for (u32 n = 0; n < kMaxNodes; ++n) {
        u32 next = 0;
        if (node == 0 || !mem.read32(node + LN_SUCC, next) || next == 0) {
            return;
        }
        keep_block_holding(node);
        if (with_messages) {
            u32 message = 0;
            mem.read32(node + mp_MsgList + LH_HEAD, message);
            for (u32 k = 0; k < kMaxNodes; ++k) {
                u32 after = 0;
                if (message == 0 || !mem.read32(message + LN_SUCC, after) || after == 0) {
                    break;
                }
                keep_block_holding(message);
                message = after;
            }
        }
        node = next;
    }
}

// What may not go with a program's blocks: a task still alive and its
// stack, a node on a system list, a message on a public port, a semaphore
// another task waits for, a port with a reply handler, code a library
// vector was set to and whatever a library's keeper names.
void Exec::mark_kept_blocks() {
    for (u32 i = 0; i < program_block_count_; ++i) {
        program_blocks_[i].keep = false;
    }
    const GuestMemory& mem = m_->mem;
    for (u32 i = 0; i < Scheduler::kMaxTasks; ++i) {
        const Scheduler::Slot s = sched_.slot(i);
        if (s.state == TaskState::Free ||
            (s.state == TaskState::Removed && static_cast<i32>(i) != sched_.current())) {
            continue;
        }
        const u32 task = tasks_[i].task_addr != 0 ? tasks_[i].task_addr : s.task_addr;
        if (task == 0) {
            continue;
        }
        keep_block_holding(task);
        u32 lower = 0;
        u32 upper = 0;
        if (mem.read32(task + TC_SPLOWER, lower) && mem.read32(task + TC_SPUPPER, upper)) {
            keep_blocks_within(lower, upper);
        }
    }
    for (u32 list : {LibList, DeviceList, ResourceList, SemaphoreList, IntrList, TaskReady,
                     TaskWait}) {
        keep_list_nodes(base_ + list, false);
    }
    keep_list_nodes(base_ + PortList, true);
    for (u32 k = 0; k < kept_list_count_; ++k) {
        const KeptList& kept = kept_lists_[k];
        if (!kept.bptr_chain) {
            keep_list_nodes(kept.head, false);
            continue;
        }
        u32 bptr = 0;
        mem.read32(kept.head, bptr);
        for (u32 n = 0; bptr != 0 && n < 4096; ++n) {
            const u64 node = static_cast<u64>(bptr) << 2;
            if (node + 4 > mem.size) {
                break;
            }
            keep_block_holding(static_cast<u32>(node));
            if (!mem.read32(static_cast<u32>(node), bptr)) {
                break;
            }
        }
    }
    for (const SemWaitQueue& q : sem_queues_) {
        keep_block_holding(q.addr);
    }
    for (const ReplyHandler& h : reply_handlers_) {
        if (h.port != 0) {
            keep_block_holding(h.port);
        }
    }
    for (u32 i = 0; i < patch_count_; ++i) {
        keep_block_holding(patches_[i].function);
    }
    for (const Keeper& k : keepers_) {
        if (k.keep != nullptr) {
            k.keep(k.ctx);
        }
    }
}

// The message ports among the program's blocks stay, deaf: what still sends
// to one queues on it and signals no task.
void Exec::keep_deaf_ports(u32 id) {
    Bus b(*m_);
    for (u32 i = 0; i < program_block_count_; ++i) {
        ProgramBlock& block = program_blocks_[i];
        if (block.program == id && block.bytes == round8(MP_SIZE) &&
            b.r8(block.addr + LN_TYPE) == NT_MSGPORT) {
            b.w8(block.addr + mp_Flags, PA_IGNORE);
            b.w32(block.addr + mp_SigTask, 0);
            block.keep = true;
        }
    }
}

void Exec::free_program_blocks(u32 id, bool keep_ports) {
    mark_kept_blocks();
    if (keep_ports) {
        keep_deaf_ports(id);
    }
    u32 kept = 0;
    for (u32 i = 0; i < program_block_count_;) {
        if (program_blocks_[i].program == id && program_blocks_[i].keep) {
            erase_program_block(i);
            ++kept;
        } else {
            ++i;
        }
    }
    u32 freed = 0;
    u64 freed_bytes = 0;
    // A library's release may free other blocks, and so drop their records:
    // each round looks the next block up again.
    for (;;) {
        u32 i = 0;
        while (i < program_block_count_ && program_blocks_[i].program != id) {
            ++i;
        }
        if (i == program_block_count_) {
            break;
        }
        const ProgramBlock block = program_blocks_[i];
        erase_program_block(i);
        ++freed;
        freed_bytes += block.bytes;
        if (block.release == 0) {
            free(block.addr, block.bytes);
        } else {
            const Releaser& r = releasers_[block.release];
            r.fn(r.ctx, block.addr);
        }
    }
    if (freed != 0 || kept != 0) {
        LogLine().text("exec: freed ").dec(freed).text(" blocks, ").dec(static_cast<u32>(freed_bytes))
            .text(" bytes, of an ended program; kept ").dec(kept).send();
    }
}

// Runs whatever else can, or waits, until the running slot is resumed.
// Returns with it still suspended only when something ends its run, nothing
// can resume it, or the host switched away for good.
void Exec::hold_while_suspended() {
    const i32 cur = sched_.current();
    if (cur < 0) {
        return;
    }
    while (sched_.slot(static_cast<u32>(cur)).suspended && m_->cpu.exit == ExitReason::None) {
        const i32 next = sched_.pick_next();
        if (next >= 0 && task_host_.switch_to != nullptr) {
            if (!switch_to_task(next) || sched_.current() != cur) {
                return;
            }
            continue;
        }
        if (!ExecOps::poll_idle(*this)) {
            return;
        }
        expire_timers();
        run_rescue();
    }
}

void Exec::maybe_preempt() {
    run_rescue();
    const i32 cur = sched_.current();
    if (cur >= 0 && sched_.slot(static_cast<u32>(cur)).suspended) {
        hold_while_suspended();
    }
    if (forbidden()) {
        return;
    }
    const bool has_timers = has_pending_timers();
    if (sched_.active_count() <= 1 && !has_timers && !pending_switch_) {
        return;
    }
    const bool slice_ended =
        task_host_.slice_over != nullptr && task_host_.slice_over(task_host_.ctx);
    if (slice_ended) {
        if (task_host_.poll != nullptr) {
            task_host_.poll(task_host_.ctx);
        }
        expire_timers();
    }
    if (task_host_.switch_to == nullptr || sched_.active_count() <= 1) {
        pending_switch_ = false;
        return;
    }
    bool do_switch = false;
    if (pending_switch_) {
        pending_switch_ = false;
        if (sched_.should_switch_now()) {
            do_switch = true;
        }
    }
    if (!do_switch && slice_ended && sched_.should_preempt()) {
        do_switch = true;
    }
    if (do_switch) {
        const i32 next = sched_.pick_next();
        if (next >= 0) {
            switch_to_task(next);
        }
    }
}

bool Exec::switch_to_task(i32 next_id) {
    const i32 from = sched_.current();
    if (from < 0 || next_id < 0 || from == next_id ||
        static_cast<u32>(from) >= Scheduler::kMaxTasks ||
        static_cast<u32>(next_id) >= Scheduler::kMaxTasks ||
        sched_.slot(static_cast<u32>(next_id)).state != TaskState::Ready) {
        return false;
    }
    Bus b(*m_);
    const u32 from_addr = tasks_[from].task_addr;
    const u32 to_addr = tasks_[next_id].task_addr;
    // A task that was woken while it could not run, as a suspended one is,
    // is Ready and on TaskReady already.
    const bool was_running = sched_.slot(static_cast<u32>(from)).state == TaskState::Running;

    if (!sched_.switch_to(next_id)) {
        return false;
    }

    tasks_[from].cpu = m_->cpu;
    tasks_[from].run_state = m_->jit->save_run_state();
    tasks_[from].call_depth = call_depth_;
    tasks_[from].load_depth = load_depth_;
    const u8 td_nest = static_cast<u8>(b.r8(base_ + TDNestCnt));
    const u8 id_nest = static_cast<u8>(b.r8(base_ + IDNestCnt));
    tasks_[from].td_nest = td_nest;
    tasks_[from].id_nest = id_nest;
    if (from_addr != 0) {
        b.w8(from_addr + TC_TDNESTCNT, td_nest);
        b.w8(from_addr + TC_IDNESTCNT, id_nest);
    }

    if (was_running && from_addr != 0) {
        b.w8(from_addr + TC_STATE, TS_READY);
        enqueue(b, base_ + TaskReady, from_addr);
    }
    if (to_addr != 0) {
        unlink_task(to_addr);
        b.w8(to_addr + TC_STATE, TS_RUN);
    }

    task_ = to_addr;
    b.w32(base_ + ThisTask, to_addr);
    b.w8(base_ + TDNestCnt, tasks_[next_id].td_nest);
    b.w8(base_ + IDNestCnt, tasks_[next_id].id_nest);

    if (task_host_.slice_over != nullptr) {
        (void)task_host_.slice_over(task_host_.ctx);
    }
    if (task_host_.switch_to != nullptr) {
        task_host_.switch_to(task_host_.ctx, static_cast<u32>(from), static_cast<u32>(next_id));
    }

    if (sched_.current() == from) {
        reap_removed();
        m_->cpu = tasks_[from].cpu;
        m_->jit->restore_run_state(tasks_[from].run_state);
        call_depth_ = tasks_[from].call_depth;
        load_depth_ = tasks_[from].load_depth;
        task_ = tasks_[from].task_addr;
        Bus rb(*m_);
        rb.w32(base_ + ThisTask, task_);
        rb.w8(base_ + TDNestCnt, tasks_[from].td_nest);
        rb.w8(base_ + IDNestCnt, tasks_[from].id_nest);
    }
    return true;
}

void Exec::reap_removed() {
    u32 ended[Scheduler::kMaxTasks];
    u32 ended_count = 0;
    for (u32 i = 0; i < Scheduler::kMaxTasks; ++i) {
        if (sched_.slot(i).state == TaskState::Removed &&
            static_cast<i32>(i) != sched_.current()) {
            if (task_host_.destroy != nullptr) {
                task_host_.destroy(task_host_.ctx, i);
            }
            sched_.release(static_cast<i32>(i));
            for (const TaskContext::Owned& block : tasks_[i].owned) {
                free(block.address, block.bytes);
            }
            tasks_[i].owned.clear();
            if (tasks_[i].program != 0) {
                ended[ended_count++] = tasks_[i].program;
            }
            tasks_[i].program = 0;
            tasks_[i].program_root = false;
        }
    }
    // A program whose last task is gone is over.
    for (u32 i = 0; i < ended_count; ++i) {
        settle_program(ended[i]);
    }
}

void Exec::remove_sem_waiter(u32 task_id) {
    for (usize i = 0; i < sem_queues_.size();) {
        SemWaitQueue& q = sem_queues_[i];
        for (u32 j = 0; j < q.count;) {
            if (q.waiters[j].id == task_id) {
                for (u32 k = j + 1; k < q.count; ++k) {
                    q.waiters[k - 1] = q.waiters[k];
                }
                --q.count;
                if (m_ != nullptr) {
                    u32 qc = 0;
                    if (m_->mem.read16(q.addr + SS_QUEUECOUNT, qc)) {
                        m_->mem.write16(q.addr + SS_QUEUECOUNT, static_cast<u16>(qc - 1));
                    }
                }
            } else {
                ++j;
            }
        }
        if (q.count == 0) {
            sem_queues_.remove(&q);
        } else {
            ++i;
        }
    }
}

bool Exec::has_sem_waiter(u32 sem_addr, u32 task_id) const {
    for (const SemWaitQueue& q : sem_queues_) {
        if (q.addr != sem_addr) {
            continue;
        }
        for (u32 i = 0; i < q.count; ++i) {
            if (q.waiters[i].id == task_id) {
                return true;
            }
        }
    }
    return false;
}

Exec::SemWaitQueue* Exec::sem_queue(u32 sem_addr, bool create) {
    SemWaitQueue* q = sem_queues_.find_if(
        [sem_addr](const SemWaitQueue& queue) { return queue.addr == sem_addr; });
    if (q != nullptr || !create) {
        return q;
    }
    q = sem_queues_.add();
    if (q == nullptr) {
        return nullptr;
    }
    q->addr = sem_addr;
    return q;
}

void Exec::task_main(u32 id) {
    if (m_ == nullptr || id >= Scheduler::kMaxTasks) {
        return;
    }
    reap_removed();
    m_->cpu = tasks_[id].cpu;
    m_->jit->restore_run_state(tasks_[id].run_state);
    call_depth_ = tasks_[id].call_depth;
    load_depth_ = tasks_[id].load_depth;
    task_ = tasks_[id].task_addr;
    Bus b(*m_);
    b.w32(base_ + ThisTask, task_);
    b.w8(base_ + TDNestCnt, tasks_[id].td_nest);
    b.w8(base_ + IDNestCnt, tasks_[id].id_nest);

    char name[kMaxName];
    if (tasks_[id].native_body != nullptr) {
        void (*body)(void*) = tasks_[id].native_body;
        void* arg = tasks_[id].native_arg;
        tasks_[id].native_body = nullptr;
        tasks_[id].native_arg = nullptr;
        body(arg);
        read_task_name(*m_, task_, name);
        LogLine().text("exec: task ").text(name).text(" ended").send();
    } else {
        const ExitReason why = m_->jit->run(tasks_[id].budget);
        read_task_name(*m_, task_, name);
        LogLine().text("exec: task ").text(name).text(" ended with ")
            .text(m68k::exit_reason_name(why)).send();
        // A task that crashed cannot answer the boot task's command that
        // started it, which may wait for it for good: Ctrl+C, which ends the
        // boot task's Wait, tells that command to give up. Other tasks, the
        // Workbench's among them, see crashed children for themselves.
        const bool crashed = why != ExitReason::None && why != ExitReason::Halt &&
                             why != ExitReason::Returned && why != ExitReason::Break &&
                             why != ExitReason::Stopped;
        if (crashed) {
            LogLine().text("exec: task ").text(name).text(" stopped at pc=").hex(m_->cpu.pc).send();
        }
        const u32 boot_task = tasks_[0].task_addr;
        if (crashed && boot_task != 0 && tasks_[id].parent == boot_task) {
            signal_task(tasks_[id].parent, SIGBREAKF_CTRL_C);
        }
    }
    m_->cpu.exit = ExitReason::None;
    Bus eb(*m_);
    ExecOps::rem_task_id(*this, eb, static_cast<i32>(id));
}

bool Exec::yield_in_native() {
    if (m_ == nullptr || forbidden()) {
        return false;
    }
    expire_timers();
    if (task_host_.switch_to == nullptr) {
        return false;
    }
    const i32 next = sched_.pick_next();
    if (next < 0) {
        return false;
    }
    return switch_to_task(next);
}

void Exec::yield_if_ready() {
    (void)yield_in_native();
}

void Exec::end_child_tasks() {
    if (m_ == nullptr) {
        return;
    }
    pending_switch_ = false;
    tasks_[0].sleeping = false;
    tasks_[0].sleep_deadline_us = 0;
    if (sched_.current() != 0) {
        if (sched_.slot(0).state == TaskState::Waiting) {
            if (tasks_[0].task_addr != 0) {
                Bus b(*m_);
                unlink_task(tasks_[0].task_addr);
            }
            sched_.wake(0);
        }
        if (sched_.slot(0).state == TaskState::Ready) {
            if (tasks_[0].task_addr != 0) {
                Bus b(*m_);
                unlink_task(tasks_[0].task_addr);
            }
            sched_.switch_to(0);
        }
        if (tasks_[0].task_addr != 0) {
            Bus b(*m_);
            b.w8(tasks_[0].task_addr + TC_STATE, TS_RUN);
            b.w32(tasks_[0].task_addr + TC_SIGWAIT, 0);
        }
        task_ = tasks_[0].task_addr;
        m_->mem.write32(base_ + ThisTask, task_);
    }
    end_tasks_of(tasks_[0].task_addr != 0 ? tasks_[0].task_addr : task_);
}

bool Exec::function_set_within(u32 lo, u32 hi) const {
    for (u32 i = 0; i < patch_count_; ++i) {
        if (patches_[i].function >= lo && patches_[i].function < hi) {
            return true;
        }
    }
    return false;
}

void Exec::detach_tasks_of(u32 parent) {
    if (parent == 0) {
        return;
    }
    for (u32 i = 1; i < Scheduler::kMaxTasks; ++i) {
        const Scheduler::Slot s = sched_.slot(i);
        if (s.state != TaskState::Free && s.state != TaskState::Removed &&
            tasks_[i].parent == parent) {
            tasks_[i].detached = true;
        }
    }
}

void Exec::end_tasks_of(u32 parent) {
    if (m_ == nullptr) {
        return;
    }
    u32 ended[Scheduler::kMaxTasks];
    u32 ended_count = 0;
    ended[ended_count++] = parent;
    auto is_ended = [&](u32 addr) {
        for (u32 i = 0; i < ended_count; ++i) {
            if (ended[i] == addr) {
                return true;
            }
        }
        return false;
    };
    for (bool found = true; found;) {
        found = false;
        for (u32 i = 1; i < Scheduler::kMaxTasks; ++i) {
            const Scheduler::Slot s = sched_.slot(i);
            if (s.state == TaskState::Free || s.state == TaskState::Removed ||
                tasks_[i].detached || !is_ended(tasks_[i].parent)) {
                continue;
            }
            tasks_[i].sleeping = false;
            tasks_[i].sleep_deadline_us = 0;
            char name[kMaxName];
            read_task_name(*m_, s.task_addr, name);
            LogLine().text("exec: ended task ").text(name).send();
            bool relink = false;
            if (s.task_addr != 0) {
                Bus b(*m_);
                if (s.state == TaskState::Ready || s.state == TaskState::Waiting) {
                    relink = !unlink_task(s.task_addr);
                }
                b.w8(s.task_addr + TC_STATE, TS_REMOVED);
            }
            remove_sem_waiter(i);
            if (s.task_addr != 0) {
                drop_timer_requests_of(s.task_addr);
            }
            sched_.remove(static_cast<i32>(i));
            if (relink) {
                rebuild_task_lists();
            }
            task_ended(s.task_addr, false);
            if (s.task_addr != 0 && ended_count < Scheduler::kMaxTasks) {
                ended[ended_count++] = s.task_addr;
            }
            found = true;
        }
    }
    reap_removed();
}

void Exec::reset_task_state(u32 task) {
    i32 id = sched_.find(task);
    if (id < 0 && task == tasks_[0].task_addr) {
        id = 0;
    }
    if (id >= 0) {
        tasks_[id].td_nest = 0xFF;
        tasks_[id].id_nest = 0xFF;
    }
    if (m_ == nullptr || base_ == 0 || task == 0) {
        return;
    }
    Bus b(*m_);
    b.w8(task + TC_TDNESTCNT, 0xFF);
    b.w8(task + TC_IDNESTCNT, 0xFF);
    b.w32(task + TC_SIGRECVD, 0);
    b.w32(task + TC_SIGALLOC, 0x0000FFFF);
    if (id >= 0 && id == sched_.current()) {
        b.w8(base_ + TDNestCnt, 0xFF);
        b.w8(base_ + IDNestCnt, 0xFF);
    }
}

void Exec::reset_nest_counts() {
    reset_task_state(tasks_[0].task_addr != 0 ? tasks_[0].task_addr : task_);
}

bool Exec::enter_nested_run() {
    // Two levels stay free, so the innermost program can still make calls
    // that go into 68000 code, such as opening a library.
    if (call_depth_ + 2 >= kMaxCallDepth) {
        LogLine().text("exec: calls into 68000 code nested too deep").send();
        return false;
    }
    ++call_depth_;
    return true;
}

void Exec::leave_nested_run() {
    if (call_depth_ > 0) {
        --call_depth_;
    }
}

}
