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

#include "../m68k_cpu.hpp"
#include "../m68k_jit.hpp"
#include "../sched.hpp"
#include "../slot_list.hpp"
#include "../types.hpp"
#include "native.hpp"
#include "version.hpp"

#include <vector>

// exec.library written in C++ for code run by the 68000 translator.
//
// Every structure exec keeps lives in guest memory with the AmigaOS layout:
// ExecBase with its lists, the libraries, the memory header and its free
// chunks. A vector of a native library is the A-line opcode of its native
// followed by RTS (see native.hpp); libraries made from 68000 code get
// JMP abs.l vectors.
//
// A native given a pointer the guest memory does not hold ends the run with
// BusError, or AddressError for a word or long at an odd address. Walks of
// guest lists and tables are bounded, so a corrupt list cannot hang the
// host. Calls back into 68000 code (library init, Open and Close vectors,
// the RawDoFmt character routine) go through Jit::call_guest, nest at most
// eight deep, and leave every register but the result as it was.

namespace hombre {

// Library vector offsets of exec.library, n of LVO -n, as the 3.1 function
// description numbers them.
namespace exec_lvo {

constexpr u32 InitStruct = 78;
constexpr u32 MakeLibrary = 84;
constexpr u32 MakeFunctions = 90;
constexpr u32 InitResident = 102;
constexpr u32 Alert = 108;
constexpr u32 Disable = 120;
constexpr u32 Enable = 126;
constexpr u32 Forbid = 132;
constexpr u32 Permit = 138;
constexpr u32 AllocMem = 198;
constexpr u32 FreeMem = 210;
constexpr u32 AvailMem = 216;
constexpr u32 AllocEntry = 222;
constexpr u32 FreeEntry = 228;
constexpr u32 Insert = 234;
constexpr u32 AddHead = 240;
constexpr u32 AddTail = 246;
constexpr u32 Remove = 252;
constexpr u32 RemHead = 258;
constexpr u32 RemTail = 264;
constexpr u32 Enqueue = 270;
constexpr u32 FindName = 276;
constexpr u32 AddTask = 282;
constexpr u32 RemTask = 288;
constexpr u32 FindTask = 294;
constexpr u32 SetTaskPri = 300;
constexpr u32 SetSignal = 306;
constexpr u32 Wait = 318;
constexpr u32 Signal = 324;
constexpr u32 AllocSignal = 330;
constexpr u32 FreeSignal = 336;
constexpr u32 AddPort = 354;
constexpr u32 RemPort = 360;
constexpr u32 PutMsg = 366;
constexpr u32 GetMsg = 372;
constexpr u32 ReplyMsg = 378;
constexpr u32 WaitPort = 384;
constexpr u32 FindPort = 390;
constexpr u32 AddLibrary = 396;
constexpr u32 OldOpenLibrary = 408;
constexpr u32 CloseLibrary = 414;
constexpr u32 SetFunction = 420;
constexpr u32 SumLibrary = 426;
constexpr u32 OpenDevice = 444;
constexpr u32 CloseDevice = 450;
constexpr u32 DoIO = 456;
constexpr u32 SendIO = 462;
constexpr u32 CheckIO = 468;
constexpr u32 WaitIO = 474;
constexpr u32 AbortIO = 480;
constexpr u32 AddResource = 486;
constexpr u32 RemResource = 492;
constexpr u32 OpenResource = 498;
constexpr u32 RawDoFmt = 522;
constexpr u32 TypeOfMem = 534;
constexpr u32 OpenLibrary = 552;
constexpr u32 InitSemaphore = 558;
constexpr u32 ObtainSemaphore = 564;
constexpr u32 ReleaseSemaphore = 570;
constexpr u32 AttemptSemaphore = 576;
constexpr u32 ObtainSemaphoreList = 582;
constexpr u32 ReleaseSemaphoreList = 588;
constexpr u32 FindSemaphore = 594;
constexpr u32 AddSemaphore = 600;
constexpr u32 RemSemaphore = 606;
constexpr u32 CopyMem = 624;
constexpr u32 CopyMemQuick = 630;
constexpr u32 CacheClearU = 636;
constexpr u32 CacheClearE = 642;
constexpr u32 CacheControl = 648;
constexpr u32 CreateIORequest = 654;
constexpr u32 DeleteIORequest = 660;
constexpr u32 CreateMsgPort = 666;
constexpr u32 DeleteMsgPort = 672;
constexpr u32 ObtainSemaphoreShared = 678;
constexpr u32 AllocVec = 684;
constexpr u32 FreeVec = 690;
constexpr u32 AttemptSemaphoreShared = 720;

}

// The standard vectors every library starts with.
namespace lib_lvo {

constexpr u32 Open = 6;
constexpr u32 Close = 12;
constexpr u32 Expunge = 18;
constexpr u32 Reserved = 24;

}

// The standard vectors every device starts with.
namespace dev_lvo {

constexpr u32 Open = 6;
constexpr u32 Close = 12;
constexpr u32 Expunge = 18;
constexpr u32 Reserved = 24;
constexpr u32 BeginIO = 30;
constexpr u32 AbortIO = 36;

}

// Offsets and values of the AmigaOS structures exec works with, named as
// the NDK assembler includes name them.
namespace ndk {

// struct Node
constexpr u32 LN_SUCC = 0;
constexpr u32 LN_PRED = 4;
constexpr u32 LN_TYPE = 8;
constexpr u32 LN_PRI = 9;
constexpr u32 LN_NAME = 10;
constexpr u32 LN_SIZE = 14;

// struct List
constexpr u32 LH_HEAD = 0;
constexpr u32 LH_TAIL = 4;
constexpr u32 LH_TAILPRED = 8;
constexpr u32 LH_TYPE = 12;
constexpr u32 LH_SIZE = 14;

// struct MinList
constexpr u32 MLH_HEAD = 0;
constexpr u32 MLH_TAIL = 4;
constexpr u32 MLH_TAILPRED = 8;
constexpr u32 MLH_SIZE = 12;

// struct Library
constexpr u32 LIB_FLAGS = 14;
constexpr u32 LIB_NEGSIZE = 16;
constexpr u32 LIB_POSSIZE = 18;
constexpr u32 LIB_VERSION = 20;
constexpr u32 LIB_REVISION = 22;
constexpr u32 LIB_IDSTRING = 24;
constexpr u32 LIB_SUM = 28;
constexpr u32 LIB_OPENCNT = 32;
constexpr u32 LIB_SIZE = 34;
constexpr u32 LIB_VECTSIZE = 6;

constexpr u8 LIBF_SUMMING = 0x01;
constexpr u8 LIBF_CHANGED = 0x02;
constexpr u8 LIBF_SUMUSED = 0x04;
constexpr u8 LIBF_DELEXP = 0x08;

// struct ExecBase
constexpr u32 SoftVer = 34;
constexpr u32 MaxLocMem = 62;
constexpr u32 ThisTask = 276;
constexpr u32 IDNestCnt = 294;
constexpr u32 TDNestCnt = 295;
constexpr u32 AttnFlags = 296;
constexpr u32 MemList = 322;
constexpr u32 ResourceList = 336;
constexpr u32 DeviceList = 350;
constexpr u32 IntrList = 364;
constexpr u32 LibList = 378;
constexpr u32 PortList = 392;
constexpr u32 TaskReady = 406;
constexpr u32 TaskWait = 420;
constexpr u32 SoftInts = 434;
constexpr u32 VBlankFrequency = 530;
constexpr u32 PowerSupplyFrequency = 531;
constexpr u32 SemaphoreList = 532;
constexpr u32 ex_EClockFrequency = 568;
constexpr u32 SYSBASESIZE = 632;

// struct MemHeader and struct MemChunk
constexpr u32 MH_ATTRIBUTES = 14;
constexpr u32 MH_FIRST = 16;
constexpr u32 MH_LOWER = 20;
constexpr u32 MH_UPPER = 24;
constexpr u32 MH_FREE = 28;
constexpr u32 MH_SIZE = 32;
constexpr u32 MC_NEXT = 0;
constexpr u32 MC_BYTES = 4;
constexpr u32 MC_SIZE = 8;

// struct MemList and struct MemEntry
constexpr u32 ML_NUMENTRIES = 14;
constexpr u32 ML_ME = 16;
constexpr u32 ME_ADDR = 0;  // ME_REQS before the entry is allocated
constexpr u32 ME_LENGTH = 4;
constexpr u32 ME_SIZE = 8;

constexpr u32 MEMF_ANY = 0;
constexpr u32 MEMF_PUBLIC = 1u << 0;
constexpr u32 MEMF_CHIP = 1u << 1;
constexpr u32 MEMF_FAST = 1u << 2;
constexpr u32 MEMF_CLEAR = 1u << 16;
constexpr u32 MEMF_LARGEST = 1u << 17;
constexpr u32 MEMF_TOTAL = 1u << 19;

// struct Resident
constexpr u32 RT_MATCHWORD = 0;
constexpr u32 RT_MATCHTAG = 2;
constexpr u32 RT_ENDSKIP = 6;
constexpr u32 RT_FLAGS = 10;
constexpr u32 RT_VERSION = 11;
constexpr u32 RT_TYPE = 12;
constexpr u32 RT_PRI = 13;
constexpr u32 RT_NAME = 14;
constexpr u32 RT_IDSTRING = 18;
constexpr u32 RT_INIT = 22;
constexpr u32 RT_SIZE = 26;
constexpr u16 RTC_MATCHWORD = 0x4AFC;
constexpr u8 RTF_AUTOINIT = 0x80;

// struct Task
constexpr u32 TC_STATE = 15;
constexpr u32 TC_IDNESTCNT = 16;
constexpr u32 TC_TDNESTCNT = 17;
constexpr u32 TC_SIGALLOC = 18;
constexpr u32 TC_SIGWAIT = 22;
constexpr u32 TC_SIGRECVD = 26;
constexpr u32 TC_SPREG = 54;
constexpr u32 TC_SPLOWER = 58;
constexpr u32 TC_SPUPPER = 62;
constexpr u32 TC_SIZE = 92;

constexpr u8 TS_RUN = 2;
constexpr u8 TS_READY = 3;
constexpr u8 TS_WAIT = 4;
constexpr u8 TS_REMOVED = 6;

constexpr u32 SIGBREAKF_CTRL_C = 1u << 12;

// struct SignalSemaphore
constexpr u32 SS_NESTCOUNT = 14;
constexpr u32 SS_WAITQUEUE = 16;
constexpr u32 SS_MULTIPLELINK = 28;
constexpr u32 SS_OWNER = 40;
constexpr u32 SS_QUEUECOUNT = 44;
constexpr u32 SS_SIZE = 46;

// struct MsgPort and struct Message
constexpr u32 mp_Flags = 14;
constexpr u32 mp_SigBit = 15;
constexpr u32 mp_SigTask = 16;
constexpr u32 mp_MsgList = 20;
constexpr u32 MP_SIZE = 34;

constexpr u8 PA_SIGNAL = 0;
constexpr u8 PA_SOFTINT = 1;
constexpr u8 PA_IGNORE = 2;

constexpr u32 mn_ReplyPort = 14;
constexpr u32 mn_Length = 18;
constexpr u32 MN_SIZE = 20;

// struct IORequest and struct IOStdReq
constexpr u32 io_Device = 20;
constexpr u32 io_Unit = 24;
constexpr u32 io_Command = 28;
constexpr u32 io_Flags = 30;
constexpr u32 io_Error = 31;
constexpr u32 IO_SIZE = 32;

constexpr u32 io_Actual = 32;
constexpr u32 io_Length = 36;
constexpr u32 io_Data = 40;
constexpr u32 io_Offset = 44;
constexpr u32 IOSTD_SIZE = 48;

constexpr u8 IOF_QUICK = 1;

constexpr u16 CMD_INVALID = 0;
constexpr u16 CMD_RESET = 1;
constexpr u16 CMD_READ = 2;
constexpr u16 CMD_WRITE = 3;
constexpr u16 CMD_UPDATE = 4;
constexpr u16 CMD_CLEAR = 5;
constexpr u16 CMD_STOP = 6;
constexpr u16 CMD_START = 7;
constexpr u16 CMD_FLUSH = 8;
constexpr u16 CMD_NONSTD = 9;

constexpr i8 IOERR_OPENFAIL = -1;
constexpr i8 IOERR_ABORTED = -2;
constexpr i8 IOERR_NOCMD = -3;
constexpr i8 IOERR_BADLENGTH = -4;

// Node types
constexpr u8 NT_TASK = 1;
constexpr u8 NT_DEVICE = 3;
constexpr u8 NT_MSGPORT = 4;
constexpr u8 NT_MESSAGE = 5;
constexpr u8 NT_FREEMSG = 6;
constexpr u8 NT_REPLYMSG = 7;
constexpr u8 NT_RESOURCE = 8;
constexpr u8 NT_LIBRARY = 9;
constexpr u8 NT_MEMORY = 10;
constexpr u8 NT_SIGNALSEM = 15;

}

// Host context-switch and idle interface supplied by the kernel (or a fake
// in unit tests).
struct TaskHost {
    // Creates a host context that will call Exec::task_main(id) when first
    // switched to. False when no stack can be had.
    bool (*create)(void* ctx, u32 id) = nullptr;
    // Saves the running host context of `from` and resumes `to`.
    void (*switch_to)(void* ctx, u32 from, u32 to) = nullptr;
    // Frees the context of a removed task that is not running.
    void (*destroy)(void* ctx, u32 id) = nullptr;
    // True once per time slice (take_need_resched).
    bool (*slice_over)(void* ctx) = nullptr;
    // Polls input and flushes the display without halting; called at slice end.
    void (*poll)(void* ctx) = nullptr;
    // Polls input and halts until the next interrupt; called while no
    // task can run. False when nothing can ever wake a waiter (no
    // keyboard).
    bool (*idle)(void* ctx) = nullptr;
    // Monotonic microseconds since boot, or null when no clock is wired.
    u64 (*now_us)(void* ctx) = nullptr;
    void* ctx = nullptr;
};

// Called at slice end, yield points, and before idling in Wait/Delay to
// complete due timer requests.
struct TimerHook {
    void (*expire)(void* ctx, u64 now_us) = nullptr;
    bool (*has_pending)(void* ctx) = nullptr;
    void* ctx = nullptr;
};

// Loads a file the way dos LoadSeg does and hands back its seglist BPTR.
// The segments must come from Exec::alloc, so that exec can free them.
struct SegmentLoader {
    bool (*load)(void* ctx, const char* path, u32& seglist_bptr);
    void* ctx;
};

// A built-in library OpenLibrary offers under leaf. A disk library of a
// higher version is loaded instead; otherwise make builds the library, adds
// it to LibList under leaf and returns its base, or returns 0.
struct LibraryProvider {
    const char* leaf;
    u16 version;
    u32 (*make)(void* ctx);
    void* ctx;
};

// Runs while Wait or WaitPort blocks with no wanted signal pending. False
// stops the wait.
struct IdleHook {
    bool (*poll)(void* ctx);
    void* ctx;
};

// Called by PutMsg before it queues a message. True means the host took
// the message, as a file system handler takes a DOS packet.
struct MessageHook {
    bool (*take)(void* ctx, u32 port, u32 message);
    void* ctx;
};

// What the system monitor may ask of a task.
enum class Rescue : u8 { None, Suspend, Resume, End };

// A task the scheduler holds, as the system monitor reports it.
struct TaskInfo {
    u32 task_addr = 0;
    char name[32]{};
    TaskState state = TaskState::Free;
    i8 priority = 0;
    bool forbid = false;
    bool disable = false;
    bool suspended = false;
};

class Exec {
public:
    // Builds ExecBase and exec's memory list in guest memory, stores the
    // ExecBase address at guest address 4 and registers exec's natives with
    // the machine's Jit, which must be initialised. The free memory is
    // [free_start, fast_end) in two regions: chip memory up to chip_end, and
    // from there fast memory, which is left out when fast_end equals
    // chip_end. It must lie above the Jit stubs (0x408) and inside guest
    // memory. False if it does not, or if it is too small.
    bool init(m68k::Machine& m, u32 free_start, u32 chip_end, u32 fast_end);

    u32 exec_base() const { return base_; }

    // The bytes no allocation holds in the regions whose attributes contain
    // every MEMF_PUBLIC, MEMF_CHIP and MEMF_FAST bit of attributes; all free
    // memory for MEMF_ANY, as a title bar reports it.
    u32 free_bytes(u32 attributes = ndk::MEMF_ANY) const;

    // The bytes of the regions whose attributes contain every memory-type
    // bit of attributes, headers included, as free_bytes selects them.
    u32 total_bytes(u32 attributes = ndk::MEMF_ANY) const;

    // Allocates from the free list of the first region, fast before chip,
    // whose attributes contain every MEMF_PUBLIC, MEMF_CHIP and MEMF_FAST
    // bit of flags. First fit, 8-byte aligned; MEMF_CLEAR zeroes the block,
    // other options are ignored. 0 on failure.
    u32 alloc(u32 bytes, u32 flags);

    // Returns a block to the free list of the region that holds it and
    // merges it with its neighbours. A block exec did not hand out, or one
    // already free, is logged and left alone.
    void free(u32 addr, u32 bytes);

    // The attributes of the region that holds addr, 0 if none does.
    u32 type_of_mem(u32 addr) const;

    // AddResource: puts the resource node at node on ResourceList by its
    // priority, as AddResource does.
    void add_resource(u32 node);

    // Creates a native library and adds it to the library list. Vector n
    // (at base - 6 * n) is [0xA000 | id][0x4E75] and calls fns[n - 1].fn
    // with ctx. Where fns[n - 1].fn is null, the Open, Close, Expunge and
    // reserved vectors (1 to 4) get exec's own, which count the opens in
    // lib_OpenCnt, and any other vector ends the run as unimplemented.
    // The name is copied into guest memory, but it and the vector names
    // are also kept as pointers for the log, so they must stay valid. The
    // positive part of the base is pos_size bytes (at least LIB_SIZE, at
    // most 0xFFFF), cleared, for a library with fields past the Library
    // node; the name follows it. Returns the library base, 0 on failure.
    u32 make_native_library(const char* name, u16 version, const NativeVector* fns, u32 count,
                            void* ctx = nullptr, u32 pos_size = ndk::LIB_SIZE);

    // Like make_native_library, but sets LN_TYPE = NT_DEVICE and enqueues on
    // ExecBase->DeviceList.
    u32 make_native_device(const char* name, u16 version, const NativeVector* fns, u32 count,
                           void* ctx = nullptr, u32 pos_size = ndk::LIB_SIZE);

    // OpenLibrary of a library not in the list asks this for LIBS:<name>.
    // It may be set before init and is kept across it.
    void set_segment_loader(const SegmentLoader& loader) { loader_ = loader; }

    // Registers a built-in library offered whenever OpenLibrary is asked for
    // its leaf and the library is not yet in the list. Up to
    // kMaxLibraryProviders are kept; reset on init.
    bool add_library_provider(const LibraryProvider& provider);

    // Opens and closes a library as the OpenLibrary and CloseLibrary vectors
    // do, for C++ code running in a native of a task.
    u32 open_library(const char* name, u16 version);
    void close_library(u32 base);

    // Wait and WaitPort call this while no wanted signal is pending. It may
    // be set before init and is kept across it.
    void set_idle_hook(const IdleHook& hook) { idle_ = hook; }

    // True when a function SetFunction put on a library vector, and has not
    // taken off again, lies in [lo, hi).
    bool function_set_within(u32 lo, u32 hi) const;

    // PutMsg offers each message to this hook first. Kept across init.
    void set_message_hook(const MessageHook& hook) { message_hook_ = hook; }

    // Supplies the host context-switch and timer callbacks. Kept across init.
    void set_task_host(const TaskHost& host) { task_host_ = host; }

    static constexpr u32 kMaxTimerHooks = 4;

    // Registers a timer hook, called at slice end, yield points, and before
    // idling. Up to kMaxTimerHooks devices can have one; a hook with the same
    // context as a registered one replaces it. False when the list is full.
    // Reset on init.
    bool add_timer_hook(const TimerHook& hook);

    // Same as add_timer_hook, for callers that register a single hook.
    bool set_timer_hook(const TimerHook& hook) { return add_timer_hook(hook); }

    // Registers a hook called with each task exec removes, so that a device
    // can drop the requests that task issued. Up to kMaxTimerHooks hooks are
    // kept; one with the same function and context as a registered one is not
    // added twice. False when the list is full. Reset on init.
    bool add_timer_drop_hook(void (*hook)(void* ctx, u32 task), void* ctx);

    // Same as add_timer_drop_hook, for callers that register a single hook.
    bool set_timer_drop_hook(void (*hook)(void* ctx, u32 task), void* ctx) {
        return add_timer_drop_hook(hook, ctx);
    }

    // Entry point for newly created host task contexts.
    void task_main(u32 id);

    // Yields from the boot task when another task is Ready and scheduling is
    // permitted.
    void yield_if_ready();

    // Yields from inside a native call when another task is Ready and
    // scheduling is permitted, returning true after switching back or false
    // when no switch took place.
    bool yield_in_native();

    // Removes and reaps every non-detached task the boot task created, and
    // their non-detached descendants, and clears their semaphore wait queue
    // entries.
    void end_child_tasks();

    // Removes and reaps every non-detached task whose parent is the task at
    // parent, and their non-detached descendants, logging each as ended.
    void end_tasks_of(u32 parent);

    // Marks every live task whose parent is the task at parent detached, so
    // that it outlives its parent: end_tasks_of and end_child_tasks leave it.
    void detach_tasks_of(u32 parent);

    // Puts the task's nest counts back to -1 (0xFF), clears its received
    // signals and gives it all signals back. ExecBase gets the nest counts
    // when the task is the running one.
    void reset_task_state(u32 task);

    // Resets TDNestCnt and IDNestCnt to -1 (0xFF) in ExecBase, the host
    // context, and the boot task's Task structure.
    void reset_nest_counts();

    // Counts one nested call into 68000 code against the same limit as the
    // translator's own calls. False, logged, when the limit is reached.
    bool enter_nested_run();

    // Gives back a count enter_nested_run took.
    void leave_nested_run();

    const Scheduler& scheduler() const { return sched_; }

    // Fills out with the task in scheduler slot when it is Ready, Running or
    // Waiting: its address, name, state, priority and whether it holds Forbid
    // and Disable. Reads only, for the system monitor. False otherwise.
    bool task_info(u32 slot, TaskInfo& out) const;

    // True while the running task holds Forbid or Disable, so code that must
    // not run in the middle of a file system operation waits.
    bool forbidden() const;

    // The 68000 registers of the task in slot, live for the running one and
    // as saved for another, and the native it is in, null outside one. Reads
    // only; false for a slot without a task.
    bool task_context(u32 slot, m68k::CpuState& cpu, const char*& native) const;

    // Asks that the task in slot, if it is still task_addr, be suspended,
    // resumed or ended. Only remembers the request, so an interrupt may call
    // it; run_rescue carries it out before the next block of 68000 code, and
    // whatever polls in normal context calls it too. Suspending the running
    // task takes effect at its next block outside any native, under Forbid
    // too; ending it ends its 68000 code with Stopped. Ending a shell or the
    // boot task stops only the command it runs; one at its prompt is ended
    // through set_end_hook. Any other task is removed, a damaged one too.
    void request_rescue(Rescue op, u32 slot, u32 task_addr);
    void run_rescue();

    // Asked by an End of the boot task, a shell or the Workbench when it
    // runs no 68000 code: true when the task's owner takes it as a request
    // to end, as a shell's EndCLI. Kept across init.
    void set_end_hook(bool (*hook)(void* ctx, u32 task), void* ctx) {
        end_hook_ = hook;
        end_hook_ctx_ = ctx;
    }

    // The task FindTask(0) and ExecBase->ThisTask report, and whose
    // tc_SigRecvd SetSignal and Wait use. It may be set before init and is
    // kept across it.
    void set_this_task(u32 task_addr);

    // The running task's guest address.
    u32 this_task() const { return task_; }

    // Adds task to the scheduler, starting at init_pc; final_pc is written
    // where its final RTS returns, the exit stub when it is 0. A detached
    // task outlives the command that started it: end_child_tasks leaves it
    // running. Returns task, or 0 if it could not be started.
    u32 spawn(u32 task, u32 init_pc, u32 final_pc, bool detached);

    // Like spawn, but task_main calls body(arg) with the task's exec state
    // (ThisTask, nest counts) set as for 68000 code, then ends the task as a
    // returned one. The task must be initialised by the caller. Returns task,
    // or 0 if it could not be started.
    u32 spawn_native(u32 task, void (*body)(void* arg), void* arg, i8 pri, bool detached);

    // Sets the bits of mask in the task's tc_SigRecvd and wakes it when it
    // waits for them; Ctrl+C also cuts a non-boot task's Delay short. An
    // address that is not a scheduled task is logged and left alone.
    void signal_task(u32 task, u32 mask);

    // Called for every task exec removes, before the task's slot is
    // released. self is true when the task removed itself, in which case D0
    // holds its return value. Each ctx has one hook, which a later call
    // replaces and a null hook removes; kMaxTaskEndHooks contexts at most.
    static constexpr u32 kMaxTaskEndHooks = 4;
    void set_task_end_hook(void (*hook)(void* ctx, u32 task, bool self), void* ctx);

    // Allocates and initialises a PA_SIGNAL MsgPort for the current task, or
    // returns 0 if memory or signals are exhausted.
    u32 create_port();

    // Frees a port made by create_port or CreateMsgPort; ignores 0.
    void delete_port(u32 port);

    // Queues message at the tail of port and signals mp_SigTask when
    // mp_Flags is PA_SIGNAL. False if port or message is 0 or faults.
    bool post_message(u32 port, u32 message);

    // Removes and returns the head message of port, or 0 if it is empty.
    u32 get_message(u32 port);

    // Replies message to its mn_ReplyPort (or marks it NT_FREEMSG when null).
    bool reply_message(u32 message);

    // Replies a timer's request. A fault on the request or on its reply port
    // is logged and dropped, and the current task's run state is left alone.
    // False when the reply was not delivered.
    bool reply_timer_message(u32 message);

    // Expires any due sleeping tasks and invokes the registered timer hook.
    void expire_timers();

    // True when a task is sleeping in sleep_us or the timer hook reports
    // pending requests.
    bool has_pending_timers() const;

    // Blocks the current task in the scheduler for at least us microseconds
    // (returns at once when us == 0 or no clock is wired).
    void sleep_us(u64 us);

    // Blocks the current task until one of the signals in mask is set or us
    // microseconds have passed, whichever comes first, as a Wait with a
    // time limit. Returns the signals of mask that were set, cleared from
    // tc_SigRecvd, or 0 when the time ran out. With us == 0 or no clock
    // wired it only takes the signals already set.
    u32 wait_for(u32 mask, u64 us);

    // Current monotonic microseconds from TaskHost::now_us, or 0 if unset.
    u64 now_us() const;

    // Sets the bits of mask in the current task's tc_SigRecvd.
    void signal_current_task(u32 mask);

    // Restores the boot task's tc_SigAlloc to 0x0000FFFF, clears its
    // tc_SigRecvd and nest counts, and clears the tracked raisable-port mask.
    void reset_task_signals();

    // Registers a reply port whose replies go to handler instead of being
    // queued on the port. A second call for the same port replaces its
    // handler; up to kMaxReplyHandlers ports have one.
    static constexpr u32 kMaxReplyHandlers = 4;
    void set_reply_handler(u32 port, void (*handler)(void* ctx, u32 message), void* ctx);

    // --- the memory of a program --------------------------------------------
    //
    // A program is the code dos or the Workbench starts: a command on a
    // shell's task, or a process with a seglist of its own. The tasks it
    // starts belong to it too. Exec records the blocks its code asks for,
    // through AllocMem, AllocVec, CreateMsgPort and CreateIORequest and
    // through the library calls that hand it an object of its own, as long
    // as the call comes straight from one of its seglists and no library is
    // being loaded. A program that ends by itself keeps what it left behind,
    // as on an Amiga. One the system monitor ends gets its blocks freed once
    // its last task is gone, except a block that holds a task still alive,
    // its stack, a node on a system list or a message on a public port.
    // Freeing a recorded block in any other way drops its record. When the
    // record is full, further blocks are not recorded.
    static constexpr u32 kMaxProgramBlocks = 2048;
    static constexpr u32 kMaxProgramCode = 4;
    static constexpr u32 kMaxReleasers = 15;
    static constexpr u32 kMaxKeptLists = 4;

    // Starts a program on task, whose code is the seglist (a BPTR), and
    // returns its id. A task that has a program already, the one that
    // started it or the one it took over from its parent, keeps it and only
    // adds the seglist to its code, unless fresh is set: then the program
    // the task started is ended as end_program ends it and a new one starts.
    // 0 when the task is not scheduled or no program can be started.
    u32 begin_program(u32 task, u32 seglist, bool fresh);

    // Ends the program task started, once its last task is gone: its blocks
    // are freed when the system monitor ended it and are forgotten, left
    // allocated, otherwise. For a program the monitor ended, the device
    // requests task issued are dropped first, as at a task's end, since the
    // task outlives its command.
    void end_program(u32 task);

    // Records the block [addr, addr + bytes) as the running program's when
    // the native that runs was called straight from its code. release is 0
    // for a block exec frees, or an id add_program_releaser handed out for a
    // library's own object.
    void claim_for_program(u32 addr, u32 bytes, u8 release = 0);

    // Registers how a library frees an object of its own that it handed a
    // program, so that ending the program releases it as the library's free
    // call does. Returns the id to give claim_for_program, 0 when the table
    // is full (the objects are then not recorded). Reset on init.
    u8 add_program_releaser(void (*release)(void* ctx, u32 addr), void* ctx);

    // A list outside ExecBase whose nodes a program's blocks may hold and
    // must keep: an exec List at head, or with bptr_chain a chain of nodes
    // whose first longword is the BPTR of the next, starting at the BPTR at
    // head. False when kMaxKeptLists are registered. Reset on init.
    bool add_kept_list(u32 head, bool bptr_chain);

    // A library that keeps addresses a program gave it, outside any list,
    // names them through keep, which exec calls while it picks what an
    // ended program's end frees; keep calls keep_block_at for each. False
    // when kMaxKeepers are registered. Reset on init.
    static constexpr u32 kMaxKeepers = 4;
    bool add_program_keeper(void (*keep)(void* ctx), void* ctx);

    // Keeps the program block that holds addr, if one does: for a keeper.
    void keep_block_at(u32 addr) { keep_block_holding(addr); }

    // A library or device that keeps addresses a program gave it and calls
    // them, as input.device its handlers, hears through hook of each
    // program the system monitor ends: once when the End is asked for, and
    // again just before the program's blocks are freed. program_holds tells
    // it which addresses it must let go. False when kMaxKeepers hooks are
    // registered. Reset on init.
    bool add_program_end_hook(void (*hook)(void* ctx, u32 program), void* ctx);

    // True when addr lies in a segment of the program's code or in a block
    // exec recorded as the program's.
    bool program_holds(u32 program, u32 addr) const;

    // Ends the 68000 code the task at task is in, as an End from the system
    // monitor ends a shell's command: its run ends with Stopped, at its next
    // block or out of the wait it is in. A task in no 68000 code is left
    // alone.
    void stop_code_of(u32 task);

    // Lets the Ready task at task run now, from a native of the running
    // task that waits for it, as Wait lets other tasks run under Forbid;
    // back once the caller is picked again. False when that task is not
    // Ready or is the caller, or no switch could be made.
    bool let_run(u32 task);

private:
    friend struct ExecOps;
    friend struct ExecTest;

    static constexpr u32 kMaxLibraries = 32;
    static constexpr u32 kMaxLoaded = 1024;
    static constexpr u32 kMaxSemaphores = 4096;
    static constexpr u32 kMaxLibraryProviders = 8;

    // Context of the native every unimplemented vector of a library runs.
    struct LibraryInfo {
        Exec* exec;
        u32 base;
        const char* name;
    };

    struct TaskContext {
        m68k::CpuState cpu{};
        m68k::Jit::RunState run_state{};
        u32 call_depth = 0;
        u32 load_depth = 0;
        u32 task_addr = 0;
        u32 parent = 0;
        u64 budget = 0;
        u8 td_nest = 0xFF;
        u8 id_nest = 0xFF;
        bool sleeping = false;
        // Signals that end the task's sleep early, for wait_for.
        u32 sleep_signals = 0;
        bool detached = false;
        u64 sleep_deadline_us = 0;
        void (*native_body)(void*) = nullptr;
        void* native_arg = nullptr;
        // Started by spawn_native: a shell or the Workbench, whose own code
        // is no program's.
        bool native_task = false;
        // The program the task runs, 0 for none, and whether the task
        // started it rather than taking it over from its parent.
        u32 program = 0;
        bool program_root = false;
        // The blocks RemTask found on tc_MemEntry, freed once the slot is
        // released and nothing reads the task any more.
        struct Owned {
            u32 address;
            u32 bytes;
        };
        std::vector<Owned> owned;
    };

    struct Program {
        u32 id = 0;  // 0 for a free entry
        u32 code[kMaxProgramCode]{};  // seglist BPTRs
        bool doomed = false;  // ended by the system monitor
        bool uses_commodities = false;  // opened commodities.library
    };

    struct ProgramBlock {
        u32 addr = 0;
        u32 bytes = 0;
        u32 program = 0;
        u8 release = 0;
        bool keep = false;
    };

    struct Releaser {
        void (*fn)(void* ctx, u32 addr) = nullptr;
        void* ctx = nullptr;
    };

    struct KeptList {
        u32 head = 0;
        bool bptr_chain = false;
    };

    struct SemWaitQueue {
        struct Waiter {
            u32 id = 0;
            bool shared = false;
        };
        u32 addr = 0;
        Waiter waiters[Scheduler::kMaxTasks]{};
        u32 count = 0;
    };

    u32 make_native_node(const char* name, u16 version, u8 node_type, u32 list_offset,
                         const NativeVector* fns, u32 count, void* ctx, u32 pos_size);
    void maybe_preempt();
    void hold_while_suspended();
    void end_for_rescue(u32 slot, const TaskInfo& info);
    void run_rescue_now();
    void stop_command_of(u32 slot);
    bool unlink_task(u32 addr);
    void rebuild_task_lists(u32 leave_out = 0);
    void doom_program_of(u32 slot);
    i32 slot_of(u32 task) const;
    // Runs every task end hook for the task exec is removing.
    void task_ended(u32 task, bool self);
    Program* find_program(u32 id);
    bool program_alive(u32 id) const;
    bool in_program_code(const Program& p, u32 pc) const;
    u32 calling_program() const;
    void record_program_block(u32 addr, u32 bytes, u32 program, u8 release);
    u32 first_block_at_or_after(u32 addr) const;
    void erase_program_block(u32 index);
    void drop_program_blocks(u32 addr, u32 bytes);
    void settle_program(u32 id);
    void mark_kept_blocks();
    void keep_block_holding(u32 addr);
    void keep_blocks_within(u32 lo, u32 hi);
    void keep_list_nodes(u32 list, bool with_messages);
    void keep_deaf_ports(u32 id);
    void free_program_blocks(u32 id, bool keep_ports);
    bool switch_to_task(i32 next_id);
    void reap_removed();
    void remove_sem_waiter(u32 task_id);
    bool has_sem_waiter(u32 sem_addr, u32 task_id) const;
    SemWaitQueue* sem_queue(u32 sem_addr, bool create);

    // One MemHeader and the free chunks within [lower, upper).
    struct Region {
        u32 header = 0;
        u32 lower = 0;
        u32 upper = 0;
        u32 free_bytes = 0;
        u32 attributes = 0;
    };

    static bool accepts(const Region& region, u32 flags);
    bool make_region(native::Bus& b, u32 from, u32 until, u32 attributes, u8 priority);
    u32 alloc_from(Region& region, u32 size);
    u32 largest_free(const Region& region) const;
    // The index of the region that holds [addr, addr + bytes), region_count_
    // if none does.
    u32 region_holding(u32 addr, u32 bytes) const;

    m68k::Machine* m_ = nullptr;
    u32 base_ = 0;
    Region regions_[2]{};  // in MemList order
    u32 region_count_ = 0;
    u32 task_ = 0;
    u32 sig_alloc_ = 0x0000FFFF;  // while no task is set
    u32 signals_ = 0;             // while no task is set
    u32 port_signals_ = 0;        // mp_SigBit mask of live exec-created ports
    u32 call_depth_ = 0;
    u32 load_depth_ = 0;
    u16 open_id_ = 0;
    u16 close_id_ = 0;
    u16 expunge_id_ = 0;
    u16 reserved_id_ = 0;
    u32 task_exit_stub_ = 0;
    SegmentLoader loader_{nullptr, nullptr};
    IdleHook idle_{nullptr, nullptr};
    MessageHook message_hook_{nullptr, nullptr};
    // A function SetFunction put on a vector and the one it replaced there.
    struct Patch {
        u32 vector = 0;
        u32 function = 0;
        u32 replaced = 0;
    };
    static constexpr u32 kMaxPatches = 256;
    Patch patches_[kMaxPatches]{};
    u32 patch_count_ = 0;
    u32 cache_bits_ = 0;  // what CacheControl was last given
    // The request waiting for run_rescue.
    volatile Rescue rescue_op_ = Rescue::None;
    volatile u32 rescue_slot_ = 0;
    volatile u32 rescue_task_ = 0;
    TaskHost task_host_{};
    struct DropHook {
        void (*fn)(void* ctx, u32 task) = nullptr;
        void* ctx = nullptr;
    };
    TimerHook timer_hooks_[kMaxTimerHooks]{};
    DropHook timer_drop_hooks_[kMaxTimerHooks]{};
    void drop_timer_requests_of(u32 task);
    Scheduler sched_{};
    TaskContext tasks_[Scheduler::kMaxTasks]{};
    SlotList<SemWaitQueue> sem_queues_{kMaxSemaphores};
    bool pending_switch_ = false;
    bool softint_logged_ = false;
    u32 unknown_signal_addr_ = 0;  // the last logged unknown Signal target
    bool every_task_waits_logged_ = false;
    bool abort_to_boot_task_ = false;
    struct ReplyHandler {
        u32 port = 0;
        void (*handler)(void* ctx, u32 message) = nullptr;
        void* ctx = nullptr;
    };
    ReplyHandler reply_handlers_[kMaxReplyHandlers]{};
    struct TaskEndHook {
        void (*hook)(void* ctx, u32 task, bool self) = nullptr;
        void* ctx = nullptr;
    };
    TaskEndHook task_end_hooks_[kMaxTaskEndHooks]{};
    bool (*end_hook_)(void* ctx, u32 task) = nullptr;
    void* end_hook_ctx_ = nullptr;
    LibraryInfo libraries_[kMaxLibraries]{};
    u32 library_count_ = 0;
    LibraryProvider providers_[kMaxLibraryProviders]{};
    u32 provider_count_ = 0;
    Program programs_[Scheduler::kMaxTasks]{};
    u32 next_program_ = 1;
    ProgramBlock program_blocks_[kMaxProgramBlocks]{};  // by address
    u32 program_block_count_ = 0;
    Releaser releasers_[kMaxReleasers + 1]{};  // 0 is exec's own free
    KeptList kept_lists_[kMaxKeptLists]{};
    u32 kept_list_count_ = 0;
    struct Keeper {
        void (*keep)(void* ctx) = nullptr;
        void* ctx = nullptr;
    };
    Keeper keepers_[kMaxKeepers]{};
    struct EndHook {
        void (*fn)(void* ctx, u32 program) = nullptr;
        void* ctx = nullptr;
    };
    EndHook end_hooks_[kMaxKeepers]{};
    void tell_program_ended(u32 id);
    std::vector<u32> loaded_;  // seglists of libraries exec loaded
};

}
