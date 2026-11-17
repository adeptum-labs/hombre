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
#include "../types.hpp"
#include "native.hpp"
#include "version.hpp"

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
constexpr u32 Disable = 120;
constexpr u32 Enable = 126;
constexpr u32 Forbid = 132;
constexpr u32 Permit = 138;
constexpr u32 AllocMem = 198;
constexpr u32 FreeMem = 210;
constexpr u32 AvailMem = 216;
constexpr u32 Insert = 234;
constexpr u32 AddHead = 240;
constexpr u32 AddTail = 246;
constexpr u32 Remove = 252;
constexpr u32 RemHead = 258;
constexpr u32 RemTail = 264;
constexpr u32 Enqueue = 270;
constexpr u32 FindName = 276;
constexpr u32 FindTask = 294;
constexpr u32 SetSignal = 306;
constexpr u32 Wait = 318;
constexpr u32 AddLibrary = 396;
constexpr u32 OldOpenLibrary = 408;
constexpr u32 CloseLibrary = 414;
constexpr u32 SumLibrary = 426;
constexpr u32 RawDoFmt = 522;
constexpr u32 OpenLibrary = 552;
constexpr u32 CopyMem = 624;
constexpr u32 CopyMemQuick = 630;
constexpr u32 CacheClearU = 636;
constexpr u32 CacheClearE = 642;
constexpr u32 AllocVec = 684;
constexpr u32 FreeVec = 690;

}

// The standard vectors every library starts with.
namespace lib_lvo {

constexpr u32 Open = 6;
constexpr u32 Close = 12;
constexpr u32 Expunge = 18;
constexpr u32 Reserved = 24;

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
constexpr u32 TC_SIGALLOC = 18;
constexpr u32 TC_SIGWAIT = 22;
constexpr u32 TC_SIGRECVD = 26;
constexpr u32 TC_SIZE = 92;

// Node types
constexpr u8 NT_TASK = 1;
constexpr u8 NT_DEVICE = 3;
constexpr u8 NT_RESOURCE = 8;
constexpr u8 NT_LIBRARY = 9;
constexpr u8 NT_MEMORY = 10;

}

// Loads a file the way dos LoadSeg does and hands back its seglist BPTR.
// The segments must come from Exec::alloc, so that exec can free them.
struct SegmentLoader {
    bool (*load)(void* ctx, const char* path, u32& seglist_bptr);
    void* ctx;
};

class Exec {
public:
    // Builds ExecBase and exec's memory list in guest memory, stores the
    // ExecBase address at guest address 4 and registers exec's natives with
    // the machine's Jit, which must be initialised. The free memory is
    // [free_start, free_end); it must lie above the Jit stubs (0x408) and
    // inside guest memory. False if it does not, or if it is too small.
    bool init(m68k::Machine& m, u32 free_start, u32 free_end);

    u32 exec_base() const { return base_; }

    // Allocates from the free list, first fit, 8-byte aligned. Every MEMF_
    // attribute is accepted; MEMF_CLEAR zeroes the block. 0 on failure.
    u32 alloc(u32 bytes, u32 flags);

    // Returns a block to the free list and merges it with its neighbours.
    // A block exec did not hand out, or one already free, is logged and
    // left alone.
    void free(u32 addr, u32 bytes);

    // Creates a native library and adds it to the library list. Vector n
    // (at base - 6 * n) is [0xA000 | id][0x4E75] and calls fns[n - 1].fn
    // with ctx. Where fns[n - 1].fn is null, the Open, Close, Expunge and
    // reserved vectors (1 to 4) get exec's own, which count the opens in
    // lib_OpenCnt, and any other vector ends the run as unimplemented.
    // The name is copied into guest memory, but it and the vector names
    // are also kept as pointers for the log, so they must stay valid.
    // Returns the library base, 0 on failure.
    u32 make_native_library(const char* name, u16 version, const NativeVector* fns, u32 count,
                            void* ctx = nullptr);

    // OpenLibrary of a library not in the list asks this for LIBS:<name>.
    // It may be set before init and is kept across it.
    void set_segment_loader(const SegmentLoader& loader) { loader_ = loader; }

    // The task FindTask(0) and ExecBase->ThisTask report, and whose
    // tc_SigRecvd SetSignal and Wait use. It may be set before init and is
    // kept across it.
    void set_this_task(u32 task_addr);

private:
    friend struct ExecOps;

    static constexpr u32 kMaxLibraries = 32;
    static constexpr u32 kMaxLoaded = 32;

    // Context of the native every unimplemented vector of a library runs.
    struct LibraryInfo {
        Exec* exec;
        u32 base;
        const char* name;
    };

    m68k::Machine* m_ = nullptr;
    u32 base_ = 0;
    u32 header_ = 0;  // the MemHeader
    u32 lower_ = 0;
    u32 upper_ = 0;
    u32 free_bytes_ = 0;
    u32 task_ = 0;
    u32 signals_ = 0;  // while no task is set
    u32 call_depth_ = 0;
    u32 load_depth_ = 0;
    u16 open_id_ = 0;
    u16 close_id_ = 0;
    u16 expunge_id_ = 0;
    u16 reserved_id_ = 0;
    SegmentLoader loader_{nullptr, nullptr};
    LibraryInfo libraries_[kMaxLibraries]{};
    u32 library_count_ = 0;
    u32 loaded_[kMaxLoaded]{};  // seglists of libraries exec loaded
};

}
