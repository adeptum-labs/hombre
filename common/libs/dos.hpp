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

#include "../amiga_fs.hpp"
#include "../m68k_cpu.hpp"
#include "../m68k_jit.hpp"
#include "../types.hpp"
#include "exec.hpp"
#include "native.hpp"
#include "version.hpp"

// dos.library written in C++ for code run by the 68000 translator, over one
// read-only volume.
//
// dos keeps its structures in guest memory with the AmigaOS layout: the
// DosLibrary with its RootNode and DosInfo, a volume node, one Process with
// its CommandLineInterface, the file handles and the locks. The Process is
// exec's task. Commands run as the CLI's command, one at a time, on a
// 4 KiB stack: run_command loads one, gives it its argument line and the
// console, and calls it.
//
// The console is the one interactive stream: what a program writes to it
// goes to the ConsoleWrite sink, and what it reads is the command's
// argument line, once, then end of file. Files on the volume can be
// opened, read and seeked in; anything that would write to the volume
// fails with ERROR_DISK_WRITE_PROTECTED.
//
// Variables are the Process's local ones, LocalVar nodes on pr_LocalVars
// as SetVar makes them. There are no global variables: ENV: is not
// mounted.
//
// Paths are AmigaDOS paths: C:, LIBS:, S:, SYS:, the volume name and ":"
// name the volume's C, LIBS and S directories and its root; any other
// device is not mounted. A path without a device is relative to the
// current directory, a leading or doubled '/' goes up one level, and names
// compare without regard to case.
//
// Every guest pointer a native takes goes through native::Bus, so a bad
// one ends the run with BusError or AddressError; walks over guest strings
// end at the end of guest memory at the latest. A file handle or lock is
// only ever looked up in dos's own tables, by its BPTR, never by what the
// guest structure says. Vectors dos does not implement log "dos:
// unimplemented LVO -<n>" and end the run with Unimplemented.

namespace hombre {

// Library vector offsets of dos.library, n of LVO -n, as the 3.1 function
// description numbers them.
namespace dos_lvo {

constexpr u32 Open = 30;
constexpr u32 Close = 36;
constexpr u32 Read = 42;
constexpr u32 Write = 48;
constexpr u32 Input = 54;
constexpr u32 Output = 60;
constexpr u32 Seek = 66;
constexpr u32 Lock = 84;
constexpr u32 UnLock = 90;
constexpr u32 DupLock = 96;
constexpr u32 Examine = 102;
constexpr u32 ExNext = 108;
constexpr u32 Info = 114;
constexpr u32 CurrentDir = 126;
constexpr u32 IoErr = 132;
constexpr u32 Exit = 144;
constexpr u32 LoadSeg = 150;
constexpr u32 UnLoadSeg = 156;
constexpr u32 DateStamp = 192;
constexpr u32 Delay = 198;
constexpr u32 WaitForChar = 204;
constexpr u32 SelectInput = 294;
constexpr u32 SelectOutput = 300;
constexpr u32 FPutC = 312;
constexpr u32 FPuts = 342;
constexpr u32 VFPrintf = 354;
constexpr u32 Flush = 360;
constexpr u32 SetMode = 426;
constexpr u32 SetIoErr = 462;
constexpr u32 Fault = 468;
constexpr u32 PrintFault = 474;
constexpr u32 Cli = 492;
constexpr u32 NewLoadSeg = 768;
constexpr u32 ReadArgs = 798;
constexpr u32 StrToLong = 816;
constexpr u32 FreeArgs = 858;
constexpr u32 SetVar = 900;
constexpr u32 GetVar = 906;
constexpr u32 PutStr = 948;
constexpr u32 VPrintf = 954;
// The last vector of the 3.1 library; dos's table reaches down to it.
constexpr u32 SetOwner = 996;

}

// Offsets, sizes and values of the AmigaOS structures dos works with,
// named as the NDK assembler includes name them.
namespace ndk {

constexpr u8 NT_MSGPORT = 4;
constexpr u8 NT_PROCESS = 13;
constexpr u8 NT_SIGNALSEM = 15;
constexpr u8 TS_RUN = 2;

// struct Task, the fields dos sets
constexpr u32 TC_STATE = 15;
constexpr u32 TC_SPREG = 54;
constexpr u32 TC_SPLOWER = 58;
constexpr u32 TC_SPUPPER = 62;
constexpr u32 TC_MEMENTRY = 74;

// struct MsgPort
constexpr u32 MP_FLAGS = 14;
constexpr u32 MP_SIGBIT = 15;
constexpr u32 MP_SIGTASK = 16;
constexpr u32 MP_MSGLIST = 20;
constexpr u32 MP_SIZE = 34;

// struct MinList
constexpr u32 MLH_HEAD = 0;
constexpr u32 MLH_TAIL = 4;
constexpr u32 MLH_TAILPRED = 8;
constexpr u32 MLH_SIZE = 12;

// struct SignalSemaphore
constexpr u32 SS_NESTCOUNT = 14;
constexpr u32 SS_WAITQUEUE = 16;
constexpr u32 SS_MULTIPLELINK = 28;
constexpr u32 SS_OWNER = 40;
constexpr u32 SS_QUEUECOUNT = 44;
constexpr u32 SS_SIZE = 46;

// struct DateStamp
constexpr u32 ds_Days = 0;
constexpr u32 ds_Minute = 4;
constexpr u32 ds_Tick = 8;
constexpr u32 ds_SIZEOF = 12;

// struct Process
constexpr u32 pr_Task = 0;
constexpr u32 pr_MsgPort = 92;
constexpr u32 pr_Pad = 126;
constexpr u32 pr_SegList = 128;
constexpr u32 pr_StackSize = 132;
constexpr u32 pr_GlobVec = 136;
constexpr u32 pr_TaskNum = 140;
constexpr u32 pr_StackBase = 144;
constexpr u32 pr_Result2 = 148;
constexpr u32 pr_CurrentDir = 152;
constexpr u32 pr_CIS = 156;
constexpr u32 pr_COS = 160;
constexpr u32 pr_ConsoleTask = 164;
constexpr u32 pr_FileSystemTask = 168;
constexpr u32 pr_CLI = 172;
constexpr u32 pr_ReturnAddr = 176;
constexpr u32 pr_PktWait = 180;
constexpr u32 pr_WindowPtr = 184;
constexpr u32 pr_HomeDir = 188;
constexpr u32 pr_Flags = 192;
constexpr u32 pr_ExitCode = 196;
constexpr u32 pr_ExitData = 200;
constexpr u32 pr_Arguments = 204;
constexpr u32 pr_LocalVars = 208;
constexpr u32 pr_ShellPrivate = 220;
constexpr u32 pr_CES = 224;
constexpr u32 pr_SIZEOF = 228;

// struct CommandLineInterface
constexpr u32 cli_Result2 = 0;
constexpr u32 cli_SetName = 4;
constexpr u32 cli_CommandDir = 8;
constexpr u32 cli_ReturnCode = 12;
constexpr u32 cli_CommandName = 16;
constexpr u32 cli_FailLevel = 20;
constexpr u32 cli_Prompt = 24;
constexpr u32 cli_StandardInput = 28;
constexpr u32 cli_CurrentInput = 32;
constexpr u32 cli_CommandFile = 36;
constexpr u32 cli_Interactive = 40;
constexpr u32 cli_Background = 44;
constexpr u32 cli_CurrentOutput = 48;
constexpr u32 cli_DefaultStack = 52;  // in longwords
constexpr u32 cli_StandardOutput = 56;
constexpr u32 cli_Module = 60;
constexpr u32 cli_SIZEOF = 64;

// struct FileHandle
constexpr u32 fh_Link = 0;
constexpr u32 fh_Interactive = 4;
constexpr u32 fh_Type = 8;
constexpr u32 fh_Buf = 12;
constexpr u32 fh_Pos = 16;
constexpr u32 fh_End = 20;
constexpr u32 fh_Funcs = 24;
constexpr u32 fh_Func2 = 28;
constexpr u32 fh_Func3 = 32;
constexpr u32 fh_Args = 36;
constexpr u32 fh_Arg2 = 40;
constexpr u32 fh_SIZEOF = 44;

// struct FileLock
constexpr u32 fl_Link = 0;
constexpr u32 fl_Key = 4;
constexpr u32 fl_Access = 8;
constexpr u32 fl_Task = 12;
constexpr u32 fl_Volume = 16;
constexpr u32 fl_SIZEOF = 20;

// struct DosLibrary
constexpr u32 dl_Root = 34;
constexpr u32 dl_GV = 38;
constexpr u32 dl_A2 = 42;
constexpr u32 dl_A5 = 46;
constexpr u32 dl_A6 = 50;
constexpr u32 dl_Errors = 54;
constexpr u32 dl_TimeReq = 58;
constexpr u32 dl_UtilityBase = 62;
constexpr u32 dl_IntuitionBase = 66;
constexpr u32 dl_SIZEOF = 70;

// struct RootNode
constexpr u32 rn_TaskArray = 0;
constexpr u32 rn_ConsoleSegment = 4;
constexpr u32 rn_Time = 8;
constexpr u32 rn_RestartSeg = 20;
constexpr u32 rn_Info = 24;
constexpr u32 rn_FileHandlerSegment = 28;
constexpr u32 rn_CliList = 32;
constexpr u32 rn_BootProc = 44;
constexpr u32 rn_ShellSegment = 48;
constexpr u32 rn_Flags = 52;
constexpr u32 rn_SIZEOF = 56;

// struct DosInfo
constexpr u32 di_McName = 0;
constexpr u32 di_DevInfo = 4;
constexpr u32 di_Devices = 8;
constexpr u32 di_Handlers = 12;
constexpr u32 di_NetHand = 16;
constexpr u32 di_DevLock = 20;
constexpr u32 di_EntryLock = 66;
constexpr u32 di_DeleteLock = 112;
constexpr u32 di_SIZEOF = 158;

// struct DosList, for a volume
constexpr u32 dol_Next = 0;
constexpr u32 dol_Type = 4;
constexpr u32 dol_Task = 8;
constexpr u32 dol_Lock = 12;
constexpr u32 dol_VolumeDate = 16;
constexpr u32 dol_LockList = 28;
constexpr u32 dol_DiskType = 32;
constexpr u32 dol_Name = 40;
constexpr u32 DosList_SIZEOF = 44;
constexpr u32 DLT_VOLUME = 2;

// struct FileInfoBlock
constexpr u32 fib_DiskKey = 0;
constexpr u32 fib_DirEntryType = 4;
constexpr u32 fib_FileName = 8;
constexpr u32 fib_Protection = 116;
constexpr u32 fib_EntryType = 120;
constexpr u32 fib_Size = 124;
constexpr u32 fib_NumBlocks = 128;
constexpr u32 fib_DateStamp = 132;
constexpr u32 fib_Comment = 144;
constexpr u32 fib_OwnerUID = 224;
constexpr u32 fib_OwnerGID = 226;
constexpr u32 fib_Reserved = 228;
constexpr u32 fib_SIZEOF = 260;

// struct InfoData
constexpr u32 id_NumSoftErrors = 0;
constexpr u32 id_UnitNumber = 4;
constexpr u32 id_DiskState = 8;
constexpr u32 id_NumBlocks = 12;
constexpr u32 id_NumBlocksUsed = 16;
constexpr u32 id_BytesPerBlock = 20;
constexpr u32 id_DiskType = 24;
constexpr u32 id_VolumeNode = 28;
constexpr u32 id_InUse = 32;
constexpr u32 id_SIZEOF = 36;
constexpr u32 ID_WRITE_PROTECTED = 80;
constexpr u32 ID_DOS_DISK = 0x444F5300;  // 'DOS\0'

// struct CSource and struct RDArgs
constexpr u32 CS_Buffer = 0;
constexpr u32 CS_Length = 4;
constexpr u32 CS_CurChr = 8;
constexpr u32 RDA_Source = 0;
constexpr u32 RDA_DAList = 12;
constexpr u32 RDA_Buffer = 16;
constexpr u32 RDA_BufSiz = 20;
constexpr u32 RDA_ExtHelp = 24;
constexpr u32 RDA_Flags = 28;
constexpr u32 RDA_SIZEOF = 32;

// struct LocalVar and the variable types and flags
constexpr u32 lv_Node = 0;
constexpr u32 lv_Flags = 14;
constexpr u32 lv_Value = 16;
constexpr u32 lv_Len = 20;
constexpr u32 LocalVar_SIZEOF = 24;
constexpr u32 LV_VAR = 0;
constexpr u32 LV_ALIAS = 1;
constexpr u32 LVF_IGNORE = 0x80;
constexpr u32 GVF_GLOBAL_ONLY = 0x100;
constexpr u32 GVF_LOCAL_ONLY = 0x200;
constexpr u32 GVF_BINARY_VAR = 0x400;
constexpr u32 GVF_DONT_NULL_TERM = 0x800;
constexpr u32 GVF_SAVE_VAR = 0x1000;

constexpr u32 DOSTRUE = 0xFFFFFFFF;
constexpr u32 DOSFALSE = 0;
constexpr u32 MODE_OLDFILE = 1005;
constexpr u32 MODE_NEWFILE = 1006;
constexpr u32 MODE_READWRITE = 1004;
constexpr u32 OFFSET_BEGINNING = 0xFFFFFFFF;  // -1
constexpr u32 OFFSET_CURRENT = 0;
constexpr u32 OFFSET_END = 1;
constexpr u32 SHARED_LOCK = 0xFFFFFFFE;     // -2, ACCESS_READ
constexpr u32 EXCLUSIVE_LOCK = 0xFFFFFFFF;  // -1, ACCESS_WRITE
constexpr u32 ST_ROOT = 1;
constexpr u32 ST_USERDIR = 2;
constexpr u32 ST_FILE = 0xFFFFFFFD;  // -3

// IoErr() codes
constexpr u32 ERROR_NO_FREE_STORE = 103;
constexpr u32 ERROR_TASK_TABLE_FULL = 105;
constexpr u32 ERROR_BAD_TEMPLATE = 114;
constexpr u32 ERROR_BAD_NUMBER = 115;
constexpr u32 ERROR_REQUIRED_ARG_MISSING = 116;
constexpr u32 ERROR_KEY_NEEDS_ARG = 117;
constexpr u32 ERROR_TOO_MANY_ARGS = 118;
constexpr u32 ERROR_UNMATCHED_QUOTES = 119;
constexpr u32 ERROR_LINE_TOO_LONG = 120;
constexpr u32 ERROR_FILE_NOT_OBJECT = 121;
constexpr u32 ERROR_INVALID_RESIDENT_LIBRARY = 122;
constexpr u32 ERROR_NO_DEFAULT_DIR = 201;
constexpr u32 ERROR_OBJECT_IN_USE = 202;
constexpr u32 ERROR_OBJECT_EXISTS = 203;
constexpr u32 ERROR_DIR_NOT_FOUND = 204;
constexpr u32 ERROR_OBJECT_NOT_FOUND = 205;
constexpr u32 ERROR_BAD_STREAM_NAME = 206;
constexpr u32 ERROR_OBJECT_TOO_LARGE = 207;
constexpr u32 ERROR_ACTION_NOT_KNOWN = 209;
constexpr u32 ERROR_INVALID_COMPONENT_NAME = 210;
constexpr u32 ERROR_INVALID_LOCK = 211;
constexpr u32 ERROR_OBJECT_WRONG_TYPE = 212;
constexpr u32 ERROR_DISK_NOT_VALIDATED = 213;
constexpr u32 ERROR_DISK_WRITE_PROTECTED = 214;
constexpr u32 ERROR_RENAME_ACROSS_DEVICES = 215;
constexpr u32 ERROR_DIRECTORY_NOT_EMPTY = 216;
constexpr u32 ERROR_TOO_MANY_LEVELS = 217;
constexpr u32 ERROR_DEVICE_NOT_MOUNTED = 218;
constexpr u32 ERROR_SEEK_ERROR = 219;
constexpr u32 ERROR_COMMENT_TOO_BIG = 220;
constexpr u32 ERROR_DISK_FULL = 221;
constexpr u32 ERROR_DELETE_PROTECTED = 222;
constexpr u32 ERROR_WRITE_PROTECTED = 223;
constexpr u32 ERROR_READ_PROTECTED = 224;
constexpr u32 ERROR_NOT_A_DOS_DISK = 225;
constexpr u32 ERROR_NO_DISK = 226;
constexpr u32 ERROR_NO_MORE_ENTRIES = 232;
constexpr u32 ERROR_IS_SOFT_LINK = 233;
constexpr u32 ERROR_OBJECT_LINKED = 234;
constexpr u32 ERROR_BAD_HUNK = 235;
constexpr u32 ERROR_NOT_IMPLEMENTED = 236;
constexpr u32 ERROR_RECORD_NOT_LOCKED = 240;
constexpr u32 ERROR_LOCK_COLLISION = 241;
constexpr u32 ERROR_LOCK_TIMEOUT = 242;
constexpr u32 ERROR_UNLOCK_ERROR = 243;
constexpr u32 ERROR_BUFFER_OVERFLOW = 303;
constexpr u32 ERROR_BREAK = 304;
constexpr u32 ERROR_NOT_EXECUTABLE = 305;

}

// Where the console output goes: the bytes a program writes to it, as it
// writes them.
using ConsoleWrite = void (*)(void* ctx, const u8* data, u32 bytes);

class Dos {
public:
    static constexpr u32 kStackBytes = 4096;
    static constexpr u32 kMaxArgs = 512;    // bytes of a command's argument line
    static constexpr u32 kMaxPath = 256;    // bytes of a resolved path, NUL included
    static constexpr u32 kMaxFiles = 16;    // files and console streams a program opens
    static constexpr u32 kMaxLocks = 32;
    static constexpr u32 kMaxArgBlocks = 16;  // ReadArgs results not yet freed

    // Makes dos.library over the volume, which must stay mounted, with its
    // structures and the shell Process, registers the Process as exec's
    // task and dos as exec's segment loader. exec must be initialised. The
    // console output goes to out with ctx; out may be null. False if exec
    // runs out of memory or native ids.
    bool init(m68k::Machine& m, Exec& exec, const AdfVolume& volume, ConsoleWrite out, void* ctx);

    // Runs the command at path as the CLI's command: loads it, sets
    // cli_CommandName and cli_Module, gives it args followed by a newline
    // in A0 with its length in D0 (and as what the console reads), the
    // stack size at 4(A7) and the return address at (A7), and calls it for
    // at most max_blocks blocks. The program's D0, or what it gave Exit, is
    // stored in result. False, logged, if it cannot be loaded or the run
    // ends any other way (cpu.exit then says how). The command is unloaded
    // afterwards.
    bool run_command(const char* path, const char* args, u32& result, u64 max_blocks);

    // LoadSeg of a path, relative to the current directory: the seglist
    // BPTR, or 0 with the Process's IoErr set. Flushes the translations.
    u32 load_seg(const char* path);

    u32 dos_base() const { return base_; }
    u32 process() const { return process_; }

private:
    friend struct DosOps;

    static constexpr u32 kVectors = dos_lvo::SetOwner / 6;
    static constexpr u32 kHandles = 2 + kMaxFiles;  // the console in and out first

    enum class Stream : u8 { Closed, Console, File };

    struct FileSlot {
        Stream stream;
        DirEntry entry;
        u32 pos;
    };

    struct LockSlot {
        bool used;
        DirEntry entry;
        char path[kMaxPath];  // from the root, components separated by '/'
    };

    struct ArgBlock {
        u32 rdargs;
        u32 block;  // 0 marks a free slot
        u32 bytes;
        bool owns_rdargs;
    };

    m68k::Machine* m_ = nullptr;
    Exec* exec_ = nullptr;
    AdfVolume volume_{};
    ConsoleWrite out_ = nullptr;
    void* out_ctx_ = nullptr;

    // Guest addresses.
    u32 base_ = 0;
    u32 block_ = 0;
    u32 root_ = 0;
    u32 volume_node_ = 0;
    u32 port_ = 0;  // the file system's MsgPort
    u32 process_ = 0;
    u32 cli_ = 0;
    u32 command_name_ = 0;
    u32 handles_ = 0;
    u32 locks_ = 0;
    u32 args_ = 0;
    u32 stack_ = 0;

    FileSlot files_[kHandles]{};
    LockSlot lock_slots_[kMaxLocks]{};
    ArgBlock arg_blocks_[kMaxArgBlocks]{};
    NativeVector vectors_[kVectors]{};

    // The console's input: the argument line, read once.
    char input_[kMaxArgs + 2]{};
    u32 input_length_ = 0;
    u32 input_pos_ = 0;

    bool running_ = false;
    bool exited_ = false;
    u32 exit_code_ = 0;
};

}
