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
#include "../dos_date.hpp"
#include "../dos_errors.hpp"
#include "../dos_pattern.hpp"
#include "../m68k_cpu.hpp"
#include "../m68k_jit.hpp"
#include "../ram_fs.hpp"
#include "../types.hpp"
#include "exec.hpp"
#include "native.hpp"
#include "utility.hpp"
#include "version.hpp"

// dos.library written in C++ for code run by the 68000 translator, over one
// read-only volume.
//
// dos keeps its structures in guest memory with the AmigaOS layout: the
// DosLibrary with its RootNode and DosInfo, a volume node, a Process with
// its CommandLineInterface for each command process, the file handles and
// the locks. Each Process is exec's task. run_command runs the shell's
// command on the shell Process, on a 4 KiB stack: it loads it, gives it its
// argument line and the console, and calls it. CreateProc, CreateNewProc,
// RunCommand, SystemTagList and Execute give a program Processes of its own,
// which run in the background unless the call is synchronous.
//
// There are four consoles, each with its own write sink and line reader;
// console 0 is the boot console. Every console stream handle belongs to one
// of them: what a program writes to it goes to that console's ConsoleWrite
// sink, and what it reads is the command's argument line, once, then that
// console's reader. Files on the volume can be opened, read and seeked in;
// anything that would write to the volume fails with
// ERROR_DISK_WRITE_PROTECTED. NIL: reads end of file and writes to it are
// discarded.
//
// Variables can be local (LocalVar nodes on pr_LocalVars, as SetVar
// creates by default) or global (files under ENV: on the RAM volume).
//
// Paths are AmigaDOS paths: DF0:, RAM:, T:, ENV:, C:, LIBS:, S:, SYS:,
// the volume name, ":" and any active assign; any other device is not
// mounted. A path without a device is relative to the current directory, a
// leading or doubled '/' goes up one level, and names compare without
// regard to case.
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
constexpr u32 DeleteFile = 72;
constexpr u32 Rename = 78;
constexpr u32 Lock = 84;
constexpr u32 UnLock = 90;
constexpr u32 DupLock = 96;
constexpr u32 Examine = 102;
constexpr u32 ExNext = 108;
constexpr u32 Info = 114;
constexpr u32 CreateDir = 120;
constexpr u32 CurrentDir = 126;
constexpr u32 IoErr = 132;
constexpr u32 CreateProc = 138;
constexpr u32 Exit = 144;
constexpr u32 LoadSeg = 150;
constexpr u32 UnLoadSeg = 156;
constexpr u32 SetComment = 180;
constexpr u32 SetProtection = 186;
constexpr u32 DateStamp = 192;
constexpr u32 Delay = 198;
constexpr u32 WaitForChar = 204;
constexpr u32 ParentDir = 210;
constexpr u32 IsInteractive = 216;
constexpr u32 Execute = 222;
constexpr u32 AllocDosObject = 228;
constexpr u32 FreeDosObject = 234;
constexpr u32 DoPkt = 240;
constexpr u32 SelectInput = 294;
constexpr u32 SelectOutput = 300;
constexpr u32 FGetC = 306;
constexpr u32 FPutC = 312;
constexpr u32 UnGetC = 318;
constexpr u32 FRead = 324;
constexpr u32 FWrite = 330;
constexpr u32 FGets = 336;
constexpr u32 FPuts = 342;
constexpr u32 VFWritef = 348;
constexpr u32 VFPrintf = 354;
constexpr u32 Flush = 360;
constexpr u32 SetVBuf = 366;
constexpr u32 DupLockFromFH = 372;
constexpr u32 OpenFromLock = 378;
constexpr u32 ParentOfFH = 384;
constexpr u32 ExamineFH = 390;
constexpr u32 SetFileDate = 396;
constexpr u32 NameFromLock = 402;
constexpr u32 NameFromFH = 408;
constexpr u32 SameLock = 420;
constexpr u32 SetMode = 426;
constexpr u32 ExAll = 432;
constexpr u32 SetFileSize = 456;
constexpr u32 SetIoErr = 462;
constexpr u32 Fault = 468;
constexpr u32 PrintFault = 474;
constexpr u32 ErrorReport = 480;
constexpr u32 Cli = 492;
constexpr u32 CreateNewProc = 498;
constexpr u32 RunCommand = 504;
constexpr u32 GetFileSysTask = 522;
constexpr u32 SetFileSysTask = 528;
constexpr u32 GetArgStr = 534;
constexpr u32 SetArgStr = 540;
constexpr u32 FindCliProc = 546;
constexpr u32 MaxCli = 552;
constexpr u32 SetCurrentDirName = 558;
constexpr u32 GetCurrentDirName = 564;
constexpr u32 SetProgramName = 570;
constexpr u32 GetProgramName = 576;
constexpr u32 SetPrompt = 582;
constexpr u32 GetPrompt = 588;
constexpr u32 SystemTagList = 606;
constexpr u32 AssignLock = 612;
constexpr u32 AssignLate = 618;
constexpr u32 AssignPath = 624;
constexpr u32 AssignAdd = 630;
constexpr u32 RemAssignList = 636;
constexpr u32 GetDeviceProc = 642;
constexpr u32 FreeDeviceProc = 648;
constexpr u32 LockDosList = 654;
constexpr u32 UnLockDosList = 660;
constexpr u32 AttemptLockDosList = 666;
constexpr u32 FindDosEntry = 684;
constexpr u32 NextDosEntry = 690;
constexpr u32 IsFileSystem = 708;
constexpr u32 CompareDates = 738;
constexpr u32 DateToStr = 744;
constexpr u32 StrToDate = 750;
constexpr u32 NewLoadSeg = 768;
constexpr u32 FindSegment = 780;
constexpr u32 CheckSignal = 792;
constexpr u32 ReadArgs = 798;
constexpr u32 FindArg = 804;
constexpr u32 ReadItem = 810;
constexpr u32 StrToLong = 816;
constexpr u32 MatchFirst = 822;
constexpr u32 MatchNext = 828;
constexpr u32 MatchEnd = 834;
constexpr u32 ParsePattern = 840;
constexpr u32 MatchPattern = 846;
constexpr u32 FreeArgs = 858;
constexpr u32 FilePart = 870;
constexpr u32 PathPart = 876;
constexpr u32 AddPart = 882;
constexpr u32 SetVar = 900;
constexpr u32 GetVar = 906;
constexpr u32 DeleteVar = 912;
constexpr u32 FindVar = 918;
constexpr u32 WriteChars = 942;
constexpr u32 PutStr = 948;
constexpr u32 VPrintf = 954;
constexpr u32 ParsePatternNoCase = 966;
constexpr u32 MatchPatternNoCase = 972;
constexpr u32 ExAllEnd = 990;
// The last vector of the 3.1 library; dos's table reaches down to it.
constexpr u32 SetOwner = 996;

}

// Offsets, sizes and values of the AmigaOS structures dos works with,
// named as the NDK assembler includes name them.
namespace ndk {

constexpr u8 NT_PROCESS = 13;

// struct Task, the field dos sets beyond exec's Task
constexpr u32 TC_MEMENTRY = 74;

// struct MsgPort
constexpr u32 MP_FLAGS = 14;
constexpr u32 MP_SIGBIT = 15;
constexpr u32 MP_SIGTASK = 16;
constexpr u32 MP_MSGLIST = 20;

// struct DateStamp
constexpr u32 ds_Days = 0;
constexpr u32 ds_Minute = 4;
constexpr u32 ds_Tick = 8;
constexpr u32 ds_SIZEOF = 12;

// struct DateTime
constexpr u32 dat_Stamp = 0;
constexpr u32 dat_Format = 12;
constexpr u32 dat_Flags = 13;
constexpr u32 dat_StrDay = 14;
constexpr u32 dat_StrDate = 18;
constexpr u32 dat_StrTime = 22;
constexpr u32 dat_SIZEOF = 26;

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

// CreateNewProc tags (dos/dostags.h), as their full 32-bit values.
constexpr u32 NP_Dummy = 0x800003E8;
constexpr u32 NP_Seglist = NP_Dummy + 1;
constexpr u32 NP_FreeSeglist = NP_Dummy + 2;
constexpr u32 NP_Entry = NP_Dummy + 3;
constexpr u32 NP_Input = NP_Dummy + 4;
constexpr u32 NP_Output = NP_Dummy + 5;
constexpr u32 NP_CloseInput = NP_Dummy + 6;
constexpr u32 NP_CloseOutput = NP_Dummy + 7;
constexpr u32 NP_Error = NP_Dummy + 8;
constexpr u32 NP_CloseError = NP_Dummy + 9;
constexpr u32 NP_CurrentDir = NP_Dummy + 10;
constexpr u32 NP_StackSize = NP_Dummy + 11;
constexpr u32 NP_Name = NP_Dummy + 12;
constexpr u32 NP_Priority = NP_Dummy + 13;
constexpr u32 NP_ConsoleTask = NP_Dummy + 14;
constexpr u32 NP_WindowPtr = NP_Dummy + 15;
constexpr u32 NP_HomeDir = NP_Dummy + 16;
constexpr u32 NP_CopyVars = NP_Dummy + 17;
constexpr u32 NP_Cli = NP_Dummy + 18;
constexpr u32 NP_Path = NP_Dummy + 19;
constexpr u32 NP_CommandName = NP_Dummy + 20;
constexpr u32 NP_Arguments = NP_Dummy + 21;

// SystemTagList and Execute tags (dos/dostags.h).
constexpr u32 SYS_Dummy = 0x80000020;
constexpr u32 SYS_Input = SYS_Dummy + 1;
constexpr u32 SYS_Output = SYS_Dummy + 2;
constexpr u32 SYS_Asynch = SYS_Dummy + 3;
constexpr u32 SYS_UserShell = SYS_Dummy + 4;
constexpr u32 SYS_CustomShell = SYS_Dummy + 5;

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

// struct DosList, for devices, volumes and assigns
constexpr u32 dol_Next = 0;
constexpr u32 dol_Type = 4;
constexpr u32 dol_Task = 8;
constexpr u32 dol_Lock = 12;
constexpr u32 dol_Handler = 16;
constexpr u32 dol_StackSize = 20;
constexpr u32 dol_Priority = 24;
constexpr u32 dol_Startup = 28;
constexpr u32 dol_SegList = 32;
constexpr u32 dol_GlobVec = 36;
constexpr u32 dol_VolumeDate = 16;
constexpr u32 dol_LockList = 28;
constexpr u32 dol_DiskType = 32;
constexpr u32 dol_AssignName = 16;
constexpr u32 dol_List = 20;
constexpr u32 dol_Name = 40;
constexpr u32 DosList_SIZEOF = 44;
constexpr u32 DLT_DEVICE = 0;
constexpr u32 DLT_DIRECTORY = 1;
constexpr u32 DLT_VOLUME = 2;
constexpr u32 DLT_LATE = 3;
constexpr u32 DLT_NONBINDING = 4;

// struct AssignList
constexpr u32 al_Next = 0;
constexpr u32 al_Lock = 4;
constexpr u32 AssignList_SIZEOF = 8;

// LockDosList, NextDosEntry and FindDosEntry flags
constexpr u32 LDF_READ = 1u << 0;
constexpr u32 LDF_WRITE = 1u << 1;
constexpr u32 LDF_DEVICES = 1u << 2;
constexpr u32 LDF_VOLUMES = 1u << 3;
constexpr u32 LDF_ASSIGNS = 1u << 4;
constexpr u32 LDF_ENTRY = 1u << 5;
constexpr u32 LDF_DELETE = 1u << 6;
constexpr u32 LDF_ALL = LDF_DEVICES | LDF_VOLUMES | LDF_ASSIGNS;

// struct DevProc and dvp_Flags
constexpr u32 dvp_Port = 0;
constexpr u32 dvp_Lock = 4;
constexpr u32 dvp_Flags = 8;
constexpr u32 dvp_DevNode = 12;
constexpr u32 DevProc_SIZEOF = 16;
constexpr u32 DVPF_UNLOCK = 1u << 0;
constexpr u32 DVPF_ASSIGN = 1u << 1;

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
constexpr u32 ID_VALIDATED = 82;
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

constexpr u32 LOCK_SAME = 0;
constexpr u32 LOCK_SAME_VOLUME = 1;
constexpr u32 LOCK_DIFFERENT = 0xFFFFFFFF;  // -1

constexpr u32 ITEM_EQUAL = 0xFFFFFFFE;      // -2
constexpr u32 ITEM_ERROR = 0xFFFFFFFF;      // -1
constexpr u32 ITEM_NOTHING = 0;
constexpr u32 ITEM_UNQUOTED = 1;
constexpr u32 ITEM_QUOTED = 2;

constexpr u32 DOS_FILEHANDLE = 0;
constexpr u32 DOS_EXALLCONTROL = 1;
constexpr u32 DOS_FIB = 2;
constexpr u32 DOS_STDPKT = 3;
constexpr u32 DOS_CLI = 4;
constexpr u32 DOS_RDARGS = 5;

constexpr u32 ED_NAME = 1;
constexpr u32 ED_TYPE = 2;
constexpr u32 ED_SIZE = 3;
constexpr u32 ED_PROTECTION = 4;
constexpr u32 ED_DATE = 5;
constexpr u32 ED_COMMENT = 6;

// struct ExAllData and struct ExAllControl
constexpr u32 ed_Next = 0;
constexpr u32 ed_Name = 4;
constexpr u32 ed_Type = 8;
constexpr u32 ed_Size = 12;
constexpr u32 ed_Prot = 16;
constexpr u32 ed_Days = 20;
constexpr u32 ed_Mins = 24;
constexpr u32 ed_Ticks = 28;
constexpr u32 ed_Comment = 32;
constexpr u32 eac_Entries = 0;
constexpr u32 eac_LastKey = 4;
constexpr u32 eac_MatchString = 8;
constexpr u32 eac_MatchFunc = 12;
constexpr u32 eac_SIZEOF = 16;

// struct DosPacket, struct StandardPacket and packet action numbers
constexpr u32 dp_Link = 0;
constexpr u32 dp_Port = 4;
constexpr u32 dp_Type = 8;
constexpr u32 dp_Res1 = 12;
constexpr u32 dp_Res2 = 16;
constexpr u32 dp_SIZEOF = 48;
constexpr u32 sp_Msg = 0;
constexpr u32 sp_Pkt = MN_SIZE;
constexpr u32 sp_SIZEOF = MN_SIZE + dp_SIZEOF;

constexpr u32 ACTION_CURRENT_VOLUME = 7;
constexpr u32 ACTION_LOCATE_OBJECT = 8;
constexpr u32 ACTION_FREE_LOCK = 15;
constexpr u32 ACTION_DELETE_OBJECT = 16;
constexpr u32 ACTION_RENAME_OBJECT = 17;
constexpr u32 ACTION_COPY_DIR = 19;
constexpr u32 ACTION_SET_PROTECT = 21;
constexpr u32 ACTION_CREATE_DIR = 22;
constexpr u32 ACTION_EXAMINE_OBJECT = 23;
constexpr u32 ACTION_EXAMINE_NEXT = 24;
constexpr u32 ACTION_DISK_INFO = 25;
constexpr u32 ACTION_INFO = 26;
constexpr u32 ACTION_SET_COMMENT = 28;
constexpr u32 ACTION_PARENT = 29;
constexpr u32 ACTION_SET_DATE = 34;
constexpr u32 ACTION_SAME_LOCK = 40;
constexpr u32 ACTION_IS_FILESYSTEM = 1027;

// struct AnchorPath, struct AChain and their flag bits (dos/dosasl.h)
constexpr u32 ap_Base = 0;
constexpr u32 ap_Last = 4;
constexpr u32 ap_Current = 4;
constexpr u32 ap_BreakBits = 8;
constexpr u32 ap_FoundBreak = 12;
constexpr u32 ap_Flags = 16;
constexpr u32 ap_Reserved = 17;
constexpr u32 ap_Strlen = 18;
constexpr u32 ap_Info = 20;
constexpr u32 ap_Buf = 280;
constexpr u32 APF_DOWILD = 1;
constexpr u32 APF_ITSWILD = 2;
constexpr u32 APF_DODIR = 4;
constexpr u32 APF_DIDDIR = 8;
constexpr u32 APF_NOMEMERR = 16;
constexpr u32 APF_DODOT = 32;
constexpr u32 APF_DirChanged = 64;
constexpr u32 APF_FollowHLinks = 128;

constexpr u32 an_Child = 0;
constexpr u32 an_Parent = 4;
constexpr u32 an_Lock = 8;
constexpr u32 an_Info = 12;
constexpr u32 an_Flags = 272;
constexpr u32 an_String = 273;
constexpr u32 AChain_SIZEOF = 274;
constexpr u32 DDF_PatternBit = 1;
constexpr u32 DDF_ExaminedBit = 2;
constexpr u32 DDF_Completed = 4;
constexpr u32 DDF_AllBit = 8;
constexpr u32 DDF_Single = 16;

// IoErr() codes
using hombre::ERROR_NO_FREE_STORE;
using hombre::ERROR_TASK_TABLE_FULL;
using hombre::ERROR_BAD_TEMPLATE;
using hombre::ERROR_BAD_NUMBER;
using hombre::ERROR_REQUIRED_ARG_MISSING;
using hombre::ERROR_KEY_NEEDS_ARG;
using hombre::ERROR_TOO_MANY_ARGS;
using hombre::ERROR_UNMATCHED_QUOTES;
using hombre::ERROR_LINE_TOO_LONG;
using hombre::ERROR_FILE_NOT_OBJECT;
using hombre::ERROR_INVALID_RESIDENT_LIBRARY;
using hombre::ERROR_NO_DEFAULT_DIR;
using hombre::ERROR_OBJECT_IN_USE;
using hombre::ERROR_OBJECT_EXISTS;
using hombre::ERROR_DIR_NOT_FOUND;
using hombre::ERROR_OBJECT_NOT_FOUND;
using hombre::ERROR_BAD_STREAM_NAME;
using hombre::ERROR_OBJECT_TOO_LARGE;
using hombre::ERROR_ACTION_NOT_KNOWN;
using hombre::ERROR_INVALID_COMPONENT_NAME;
using hombre::ERROR_INVALID_LOCK;
using hombre::ERROR_OBJECT_WRONG_TYPE;
using hombre::ERROR_DISK_NOT_VALIDATED;
using hombre::ERROR_DISK_WRITE_PROTECTED;
using hombre::ERROR_RENAME_ACROSS_DEVICES;
using hombre::ERROR_DIRECTORY_NOT_EMPTY;
using hombre::ERROR_TOO_MANY_LEVELS;
using hombre::ERROR_DEVICE_NOT_MOUNTED;
using hombre::ERROR_SEEK_ERROR;
using hombre::ERROR_COMMENT_TOO_BIG;
using hombre::ERROR_DISK_FULL;
using hombre::ERROR_DELETE_PROTECTED;
using hombre::ERROR_WRITE_PROTECTED;
using hombre::ERROR_READ_PROTECTED;
using hombre::ERROR_NOT_A_DOS_DISK;
using hombre::ERROR_NO_DISK;
using hombre::ERROR_NO_MORE_ENTRIES;
using hombre::ERROR_IS_SOFT_LINK;
using hombre::ERROR_OBJECT_LINKED;
using hombre::ERROR_BAD_HUNK;
using hombre::ERROR_NOT_IMPLEMENTED;
using hombre::ERROR_RECORD_NOT_LOCKED;
using hombre::ERROR_LOCK_COLLISION;
using hombre::ERROR_LOCK_TIMEOUT;
using hombre::ERROR_UNLOCK_ERROR;
using hombre::ERROR_BUFFER_OVERFLOW;
using hombre::ERROR_BREAK;
using hombre::ERROR_NOT_EXECUTABLE;

}

// Where a console's output goes: the bytes a program writes to it, as it
// writes them.
using ConsoleWrite = void (*)(void* ctx, const u8* data, u32 bytes);

// A console's line input: blocks until a line is typed, returns the byte
// count written into buf (final '\n' included), or 0 at end of input.
using ConsoleRead = u32 (*)(void* ctx, u8* buf, u32 size);

// A console's readiness check: returns true if a byte is ready or arrives
// within timeout_us microseconds.
using ConsoleReady = bool (*)(void* ctx, u32 timeout_us);

class Dos {
public:
    using ConsoleWrite = hombre::ConsoleWrite;
    using ConsoleRead = hombre::ConsoleRead;
    using ConsoleReady = hombre::ConsoleReady;

    static constexpr u8 kVolAdf = 0;
    static constexpr u8 kVolRam = 1;
    static constexpr u32 kMaxConsoles = 4;
    static constexpr u32 kStackBytes = 4096;
    static constexpr u32 kMaxArgs = 512;    // bytes of a command's argument line
    static constexpr u32 kMaxPath = 256;    // bytes of a resolved path, NUL included
    static constexpr u32 kMaxFiles = 16;    // files and console streams a program opens
    static constexpr u32 kMaxLocks = 128;
    static constexpr u32 kMaxPathDirs = 8;  // directories in the CLI command search path
    static constexpr u32 kMaxAssigns = 32;
    static constexpr u32 kMaxAssignDirs = 4;
    static constexpr u32 kMaxAssignLocks = 64;
    static constexpr u32 kMaxArgBlocks = 16;  // ReadArgs results not yet freed
    static constexpr u32 kMaxAnchors = 8;     // active MatchFirst/MatchNext anchors
    static constexpr u32 kMaxAnchorDepth = 8;
    static constexpr u32 kMaxLoggedActions = 32;
    static constexpr u32 kMaxProcs = 8;  // processes and their CLI numbers

    // Makes dos.library over the volume, which must stay mounted, with its
    // structures and the shell Process, registers the Process as exec's
    // task and dos as exec's segment loader. exec must be initialised. The
    // boot console's output goes to out with ctx; out may be null. False if
    // exec runs out of memory or native ids.
    bool init(m68k::Machine& m, Exec& exec, const AdfVolume& volume, ConsoleWrite out, void* ctx);

    // Runs the command at path in the boot shell's process as the CLI's
    // command: loads it, sets cli_CommandName and cli_Module, gives it args
    // followed by a newline in A0 with its length in D0 (and as what the
    // console reads), the stack size at 4(A7) and the return address at
    // (A7), and calls it for at most max_blocks blocks. The program's D0,
    // or what it gave Exit, is stored in result. False, logged, if it cannot
    // be loaded or the run ends any other way (cpu.exit then says how). The
    // command is unloaded afterwards.
    bool run_command(const char* path, const char* args, u32& result, u64 max_blocks);

    // run_command for the context whose Process is process; normally called
    // from that process's own task.
    bool run_command_in(u32 process, const char* path, const char* args, u32& result,
                        u64 max_blocks);

    // Builds, but does not start, a CLI process for console: named
    // "HombreShell", with the console's new handle pair as pr_CIS and
    // pr_COS, the boot shell's current directory and prompt, an interactive
    // CLI, a guest stack and a task array entry. process and cli_number
    // give the Process and its CLI number. False when console does not
    // exist, no context or CLI number is free, or memory runs out.
    bool create_shell_process(u8 console, u32& process, u32& cli_number);

    // Makes a new pair of console stream handles bound to console, owned by
    // nobody, and returns their BPTRs as Open would. False, with the
    // outputs cleared, if console does not exist or no two handle slots are
    // free.
    bool open_console_handles(u8 console, u32& in_fh, u32& out_fh);

    // Sets the write, read and readiness callbacks and their context of
    // console. Console 0 is the boot console; other consoles have none
    // until set. Out-of-range consoles are ignored.
    void set_console(u8 console, ConsoleWrite write, ConsoleRead read, ConsoleReady ready,
                     void* ctx);

    // Starts path as a background CLI process with args, reading NIL: and
    // writing to the shell's output. Returns its CLI number, 0 on failure.
    u32 start_background(const char* path, const char* args);

    // LoadSeg of a path, relative to the current directory: the seglist
    // BPTR, or 0 with the Process's IoErr set. Flushes the translations.
    u32 load_seg(const char* path);

    // What a name resolves to, for callers that pick behaviour by object
    // type, such as GetDiskObjectNew's choice of default icon.
    enum class TargetKind : u8 { Missing, File, Dir, VolumeRoot };

    struct Target {
        TargetKind kind = TargetKind::Missing;
        u32 size = 0;
    };

    // Looks the name up as dos resolves it, setting IoErr when the path
    // names no mounted volume. A name that is not there reports Missing,
    // with IoErr ERROR_OBJECT_NOT_FOUND.
    Target examine(const char* name);

    // Reads the whole of the file name into data, at most capacity bytes,
    // setting got, error and IoErr. False unless the whole file was read;
    // a file on RAM: protected from reading is not.
    bool read_file(const char* name, u8* data, u32 capacity, u32& got, u32& error);

    // Replaces the file name with the bytes; RAM: is the only writable
    // volume, and a file there protected from writing or open elsewhere
    // stays as it is. False with error and IoErr set otherwise.
    bool write_file(const char* name, const u8* data, u32 bytes, u32& error);

    // Deletes the file name, on RAM: only.
    bool delete_file(const char* name, u32& error);

    // Sets the clock source returning 50 Hz AmigaDOS ticks since 1978-01-01,
    // or null to report 0/0/0. Stamps any RAM: nodes whose date is still zero.
    void set_clock(u64 (*now_ticks)());

    // Changes the process's current directory to path ("/" parent, ":"
    // volume root, relative, or volume/assign-qualified). Must be a
    // directory (ERROR_OBJECT_WRONG_TYPE otherwise). Updates pr_CurrentDir
    // (unlocking the previous lock) and cli_SetName. On failure sets error
    // and IoErr. process 0 is the boot shell's process.
    bool change_dir(const char* path, u32& error, u32 process = 0);

    // Copies the current directory name (cli_SetName) into out. False if out
    // is null, size is 0, or the name does not fit. process 0 is the boot
    // shell's process.
    bool current_dir_name(char* out, u32 size, u32 process = 0) const;

    // Copies the CLI prompt template (cli_Prompt) into out. False if out is
    // null, size is 0, or the template does not fit. process 0 is the boot
    // shell's process.
    bool prompt(char* out, u32 size, u32 process = 0) const;

    // Stores text as the CLI prompt template (cli_Prompt, bounded to 255
    // bytes). process 0 is the boot shell's process.
    void set_prompt(const char* text, u32 process = 0);

    // Gets or sets the last IoErr code (pr_Result2 / cli_Result2).
    u32 last_error(u32 process = 0) const;
    void set_last_error(u32 code);

    // Gets or sets the last CLI command return code (cli_ReturnCode).
    u32 last_return_code(u32 process = 0) const;
    void set_return_code(u32 code);

    // Gets or sets the CLI fail limit (cli_FailLevel).
    u32 fail_level(u32 process = 0) const;
    void set_fail_level(u32 level);

    // Formats the fault text for code without a header into out (at most
    // size - 1 bytes plus NUL) and returns the length written (0 for code 0).
    u32 fault(u32 code, char* out, u32 size) const;

    // Appends dir to the running process's command search path (up to
    // kMaxPathDirs), kept as locks and mirrored in its cli_CommandDir. On
    // failure sets error and IoErr.
    bool path_add(const char* dir, u32& error);

    // Clears the running process's command search path, unlocking all path
    // locks and freeing its cli_CommandDir list.
    void path_reset();

    // Removes dir from the running process's command search path. False with
    // error set when it cannot be found or is not in the path.
    bool path_remove(const char* dir, u32& error);

    // Number of directories currently in the running process's command
    // search path.
    u32 path_count() const { return procs_[cur_index()].path_count; }

    // Copies the NameFromLock text of path entry i into out. False if i is
    // out of range or the name does not fit.
    bool path_name(u32 i, char* out, u32 size) const;

    // Resolves command word: if it contains ':' or '/', returns it as is
    // when it exists as a file; otherwise checks the process's current
    // directory (the bare name relative to it), then each directory of its
    // command path in order, then "C:<word>". Returns false if none exists
    // as a file or out is too small. process 0 is the boot shell's process.
    bool find_command(const char* word, char* out, u32 size, u32 process = 0);

    // Registers console 0's interactive input callbacks, used after the
    // command's argument line is exhausted and by read_console_line.
    // WaitForChar uses the readiness callback of the handle's console. Pass
    // null to restore immediate EOF.
    void set_console_input(ConsoleRead read, ConsoleReady ready, void* ctx);

    // Reads one line from the interactive console reader into out (trailing
    // newline stripped). False when no reader is set, at EOF, or invalid
    // buffer.
    bool read_console_line(char* out, u32 size);

    // Sets a shell variable (local by default, or global ENV: with
    // GVF_GLOBAL_ONLY); a null value deletes it. False with IoErr set on error.
    bool set_var(const char* name, const char* value, u32 flags = ndk::LV_VAR);

    // Copies the variable into out (up to the first newline unless
    // GVF_BINARY_VAR). Returns bytes copied (excluding NUL), or -1 with IoErr
    // set if missing or invalid.
    i32 get_var(const char* name, char* out, u32 size, u32 flags = ndk::LV_VAR);

    // Deletes a variable (local, global, or both per flags). False with IoErr
    // set if missing or on error.
    bool delete_var(const char* name, u32 flags = ndk::LV_VAR);

    // Visits every local variable of type (e.g. LV_VAR) in the process's
    // pr_LocalVars, passing its name and first line of value. process 0 is
    // the boot shell's process.
    void list_local_vars(u32 type,
                         void (*visit)(void* ctx, const char* name, const char* value),
                         void* ctx, u32 process = 0) const;

    // Visits every top-level entry in ENV:, passing its name. False on error.
    bool list_env_vars(void (*visit)(void* ctx, const char* name), void* ctx);

    u32 dos_base() const { return base_; }
    u32 process() const { return procs_[0].process; }
    RamFs& ram() { return ram_; }
    const RamFs& ram() const { return ram_; }

private:
    friend struct DosOps;

    static constexpr u32 kVectors = dos_lvo::SetOwner / 6;
    static constexpr u32 kHandles = 2 + kMaxFiles;  // the console in and out first
    static constexpr u8 kNoConsole = 0xFF;

    enum class Stream : u8 { Closed, Console, File, Nil };

    // The callbacks of one console. Writes and reads keep their own context
    // so that init and set_console_input can each set their half.
    struct ConsoleBinding {
        ConsoleWrite write = nullptr;
        ConsoleRead read = nullptr;
        ConsoleReady ready = nullptr;
        void* write_ctx = nullptr;
        void* read_ctx = nullptr;
    };

    struct ProcContext {
        bool used;
        u32 process;       // guest Process
        u32 cli;           // guest CommandLineInterface, 0 when none
        u32 args;          // guest argument line buffer
        u32 stack;         // guest stack, kStackBytes
        u32 command_name;  // BSTR buffer of cli_CommandName
        u32 prompt;        // BSTR buffer of cli_Prompt
        u32 set_name;      // BSTR buffer of cli_SetName
        // The console's input: the argument line, read once, then the
        // console's reader.
        char input[kMaxArgs + 2];
        u32 input_length;
        u32 input_pos;
        bool running;
        bool exited;
        u32 exit_code;
        u32 seglist;        // the running seglist, when the context owns one
        bool free_seglist;
        u32 cli_number;     // CLI number 1..kMaxProcs, 0 when the process has no CLI
        u32 process_bytes;  // bytes of the Process block, 0 when not owned
        u32 cli_bytes;      // bytes of the CLI block, 0 when not owned
        u32 stack_bytes;    // bytes of the stack, 0 when not owned
        u32 in_handle;      // pr_CIS
        u32 out_handle;     // pr_COS
        bool close_input;
        bool close_output;
        bool shell_console;  // owns the console handle pair create_shell_process opened
        u8 console;          // the console its streams read from and write to
        u32 path_locks[kMaxPathDirs];
        u32 path_nodes[kMaxPathDirs];
        u32 path_count;
    };
    ProcContext procs_[kMaxProcs]{};
    // The context of the running task: the one whose process is exec's
    // this_task(), else context 0 (the boot shell's process).
    u32 cur_index() const;
    ProcContext& cur();
    // The context whose process is process, or context 0 when process is 0
    // or not a known process.
    ProcContext& context_for(u32 process);
    const ProcContext& context_for(u32 process) const;
    // The console the process's streams read from and write to.
    u8 console_for_process(u32 process) const;
    // The owner value of the current context, 0 when it has no command running.
    u8 cur_owner() const;
    void set_context_error(const ProcContext& p, u32 code);

    // What spawn_process and create_shell_process build a Process and its
    // context from. Pointers stay valid only for the call.
    struct NewProcess {
        const char* name;
        u32 seglist;
        bool free_seglist;
        u32 entry;
        u32 stack_bytes;
        u32 input;   // 0 = NIL:
        u32 output;  // 0 = the parent's, shared and left open
        bool close_input;
        bool close_output;
        u32 current_dir;  // 0 = the parent's
        const char* args;
        const char* command_name;
        bool cli;
        u32 priority;
        u8 console = kNoConsole;  // bind pr_CIS and pr_COS to this console's new handles
        bool start = true;        // start a task for the process
        bool interactive = false; // cli_Interactive
        bool background = true;   // cli_Background
        u32 parent = 0;           // the context to inherit from, 0 = the current
    };
    static constexpr u32 kMaxDeferred = kMaxProcs * 2;

    u32 free_proc_slot() const;
    u32 free_cli_number() const;
    bool build_process(const NewProcess& spec, u32& process, u32& cli_number);
    bool spawn_process(const NewProcess& spec, u32& process, u32& cli_number);
    u32 open_nil(u8 owner);
    void process_ended(u32 task, bool self);
    void defer_free(u32 at, u32 bytes);
    void drain_deferred();
    u32 task_array_at(u32 cli) const;
    u32 stack_size_of(const ProcContext& p) const;

    struct FileSlot {
        Stream stream;
        DirEntry entry;
        u32 pos;
        bool has_unget;
        u8 unget;
        bool has_last;
        u8 last_char;
        u8 owner;
        bool exclusive;
        u8 console;       // the console of a Console stream
        u8 generation;    // changes each time the slot is opened
        char path[kMaxPath];
    };

    struct LockSlot {
        bool used;
        u8 owner;
        bool exclusive;
        DirEntry entry;
        char path[kMaxPath];  // from the root, components separated by '/'
    };

    struct ArgBlock {
        u32 rdargs;
        u32 block;  // 0 marks a free slot
        u32 bytes;
        bool owns_rdargs;
    };

    struct AnchorLevel {
        u32 chain_addr;
        u32 chain_bytes;
        u32 lock_bptr;
        DirEntry dir;
        char dir_path[kMaxPath];
        char dir_label[32];
        u32 after;
        bool first;
        bool exhausted;
    };

    struct AnchorSlot {
        bool used;
        u8 owner;
        u32 anchor_addr;  // masked guest AnchorPath address
        bool is_wild;
        bool leaf_empty;
        u32 depth;
        DirEntry cur_entry;
        char prefix[detail::kDosPatMaxPat + 1];
        u8 tokens[detail::kDosPatMaxTokens + 1];
        AnchorLevel levels[kMaxAnchorDepth];
    };

    struct AssignEntry {
        bool used;
        u32 type;
        char name[32];
        u32 locks[kMaxAssignDirs];
        u32 dir_count;
        char path[kMaxPath];
        u32 node;
        u32 node_bytes;
    };

    m68k::Machine* m_ = nullptr;
    Exec* exec_ = nullptr;
    AdfVolume volume_{};
    RamFs ram_{};
    ConsoleBinding consoles_[kMaxConsoles]{};

    // Guest addresses.
    u32 base_ = 0;
    u32 block_ = 0;
    u32 root_ = 0;
    u32 volume_node_ = 0;
    u32 ram_volume_node_ = 0;
    u32 device_node_ = 0;
    u32 ram_device_node_ = 0;
    u32 port_ = 0;      // the ADF file system's MsgPort
    u32 ram_port_ = 0;  // the RAM: file system's MsgPort
    u32 handles_ = 0;
    u32 locks_ = 0;
    u32 assign_locks_ = 0;

    FileSlot files_[kHandles]{};
    LockSlot lock_slots_[kMaxLocks]{};
    LockSlot assign_lock_slots_[kMaxAssignLocks]{};
    AssignEntry assigns_[kMaxAssigns]{};
    ArgBlock arg_blocks_[kMaxArgBlocks]{};
    AnchorSlot anchors_[kMaxAnchors]{};
    u32 unknown_pkt_actions_[kMaxLoggedActions]{};
    u32 unknown_pkt_count_ = 0;
    NativeVector vectors_[kVectors]{};
    u64 (*now_ticks_)() = nullptr;
    // The index of the path directory whose lock holds (vol, canonical), or
    // the path's count when none does.
    static u32 path_index_of(const Dos& d, const ProcContext& p, u8 vol, const char* canonical);

    // Memory of processes that ended themselves, freed once nothing runs on
    // it any more.
    u32 deferred_addr_[kMaxDeferred]{};
    u32 deferred_bytes_[kMaxDeferred]{};
    u32 deferred_count_ = 0;
};

}
