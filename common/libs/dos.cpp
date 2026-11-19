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

#include "dos.hpp"

#include "../hunk.hpp"

namespace hombre {

using m68k::CpuState;
using m68k::ExitReason;
using m68k::GuestMemory;
using m68k::Machine;
using native::Bus;
using native::LogLine;
using namespace ndk;

namespace {

constexpr u32 kHandles = 2 + Dos::kMaxFiles;
constexpr u32 kConsoleIn = 0;  // the handle slots of pr_CIS and pr_COS
constexpr u32 kConsoleOut = 1;
constexpr u32 kMaxName = 256;                // bytes of a guest name dos reads, NUL included
constexpr u32 kMaxLine = Dos::kMaxArgs + 1;  // a ReadArgs line, its newline included
constexpr u32 kMaxTemplate = 512;
constexpr u32 kMaxItems = 64;
constexpr u32 kMaxMulti = kMaxLine / 2 + 2;  // items of one line: a byte and a separator each
constexpr u32 kMaxPrompts = 4;
constexpr u32 kChunk = 128;
constexpr u32 kMaxComponent = 30;
constexpr u32 kFaultText = 256;
constexpr u32 kFailLevel = 10;
constexpr u32 kMaxVars = 1u << 16;  // nodes a walk of pr_LocalVars visits at most
constexpr u32 kMaxDosEntries = 256;
constexpr u32 kMaxAssignDepth = 8;
constexpr u8 SIGB_DOS = 8;
constexpr char kDosName[] = "dos.library";
constexpr char kIdString[] = "dos 60.0 (Hombre)\r\n";
constexpr char kProcessName[] = "Hombre Shell";
constexpr char kPrompt[] = "%N.%S> ";

constexpr u32 align4(u32 v) {
    return (v + 3) & ~3u;
}

// Where dos's structures sit in its block of guest memory.
constexpr u32 kRootAt = 0;
constexpr u32 kInfoAt = kRootAt + rn_SIZEOF;
constexpr u32 kTaskArrayAt = align4(kInfoAt + di_SIZEOF);
constexpr u32 kVolumeAt = kTaskArrayAt + 8;
constexpr u32 kVolumeNameAt = kVolumeAt + DosList_SIZEOF;  // a BSTR of up to 30 characters
constexpr u32 kRamVolumeAt = kVolumeNameAt + 32;
constexpr u32 kRamVolumeNameAt = kRamVolumeAt + DosList_SIZEOF;  // "Ram Disk" BSTR
constexpr u32 kDeviceAt = kRamVolumeNameAt + 12;
constexpr u32 kDeviceNameAt = kDeviceAt + DosList_SIZEOF;  // "DF0" BSTR
constexpr u32 kRamDeviceAt = kDeviceNameAt + 8;
constexpr u32 kRamDeviceNameAt = kRamDeviceAt + DosList_SIZEOF;  // "RAM" BSTR
constexpr u32 kPortAt = kRamDeviceNameAt + 8;
constexpr u32 kProcessAt = align4(kPortAt + MP_SIZE);
constexpr u32 kProcessNameAt = kProcessAt + pr_SIZEOF;
constexpr u32 kCliAt = align4(kProcessNameAt + sizeof kProcessName);
constexpr u32 kSetNameAt = kCliAt + cli_SIZEOF;            // up to 255 characters
constexpr u32 kPromptAt = kSetNameAt + 260;                // up to 255 characters
constexpr u32 kCommandFileAt = kPromptAt + 260;
constexpr u32 kCommandNameAt = kCommandFileAt + 4;         // up to 255 characters
constexpr u32 kIdStringAt = kCommandNameAt + 260;
constexpr u32 kHandlesAt = align4(kIdStringAt + sizeof kIdString);
constexpr u32 kLocksAt = kHandlesAt + kHandles * fh_SIZEOF;
constexpr u32 kAssignLocksAt = kLocksAt + Dos::kMaxLocks * fl_SIZEOF;
constexpr u32 kArgsAt = kAssignLocksAt + Dos::kMaxAssignLocks * fl_SIZEOF;
constexpr u32 kStackAt = (kArgsAt + Dos::kMaxArgs + 2 + 7) & ~7u;
constexpr u32 kBlockBytes = kStackAt + Dos::kStackBytes;

// The messages Fault and PrintFault give, in Hombre's own words.
struct FaultText {
    u32 code;
    const char* text;
};

constexpr FaultText kFaults[] = {
    {ERROR_NO_FREE_STORE, "out of memory"},
    {ERROR_TASK_TABLE_FULL, "no room for another process"},
    {ERROR_BAD_TEMPLATE, "malformed template"},
    {ERROR_BAD_NUMBER, "not a valid number"},
    {ERROR_REQUIRED_ARG_MISSING, "mandatory argument not given"},
    {ERROR_KEY_NEEDS_ARG, "keyword given without its value"},
    {ERROR_TOO_MANY_ARGS, "too many arguments"},
    {ERROR_UNMATCHED_QUOTES, "quote left open"},
    {ERROR_LINE_TOO_LONG, "command line too long or malformed"},
    {ERROR_FILE_NOT_OBJECT, "not a loadable program"},
    {ERROR_INVALID_RESIDENT_LIBRARY, "resident library is unusable"},
    {ERROR_NO_DEFAULT_DIR, "no current directory"},
    {ERROR_OBJECT_IN_USE, "busy, in use elsewhere"},
    {ERROR_OBJECT_EXISTS, "name already taken"},
    {ERROR_DIR_NOT_FOUND, "no such directory"},
    {ERROR_OBJECT_NOT_FOUND, "no such file or directory"},
    {ERROR_BAD_STREAM_NAME, "bad console window specification"},
    {ERROR_OBJECT_TOO_LARGE, "exceeds the size limit"},
    {ERROR_ACTION_NOT_KNOWN, "operation not supported here"},
    {ERROR_INVALID_COMPONENT_NAME, "bad name in path"},
    {ERROR_INVALID_LOCK, "not a valid lock or handle"},
    {ERROR_OBJECT_WRONG_TYPE, "file or directory of the wrong kind"},
    {ERROR_DISK_NOT_VALIDATED, "volume needs validating"},
    {ERROR_DISK_WRITE_PROTECTED, "volume is read-only"},
    {ERROR_RENAME_ACROSS_DEVICES, "cannot rename onto another volume"},
    {ERROR_DIRECTORY_NOT_EMPTY, "directory still has entries"},
    {ERROR_TOO_MANY_LEVELS, "path nests too deep"},
    {ERROR_DEVICE_NOT_MOUNTED, "no such device or volume"},
    {ERROR_SEEK_ERROR, "position outside the file"},
    {ERROR_COMMENT_TOO_BIG, "comment exceeds its limit"},
    {ERROR_DISK_FULL, "no space left on the volume"},
    {ERROR_DELETE_PROTECTED, "deleting not allowed"},
    {ERROR_WRITE_PROTECTED, "writing not allowed"},
    {ERROR_READ_PROTECTED, "reading not allowed"},
    {ERROR_NOT_A_DOS_DISK, "volume format not recognised"},
    {ERROR_NO_DISK, "drive is empty"},
    {ERROR_NO_MORE_ENTRIES, "end of the directory"},
    {ERROR_IS_SOFT_LINK, "name is a soft link"},
    {ERROR_OBJECT_LINKED, "name has hard links"},
    {ERROR_BAD_HUNK, "program file is corrupt"},
    {ERROR_NOT_IMPLEMENTED, "operation not available"},
    {ERROR_RECORD_NOT_LOCKED, "record was not locked"},
    {ERROR_LOCK_COLLISION, "record locked by another"},
    {ERROR_LOCK_TIMEOUT, "gave up waiting for a record lock"},
    {ERROR_UNLOCK_ERROR, "record unlock failed"},
    {ERROR_BUFFER_OVERFLOW, "data does not fit the buffer"},
    {ERROR_BREAK, "interrupted"},
    {ERROR_NOT_EXECUTABLE, "not marked executable"},
};

// Ends the run unless it has ended already.
void end_run(Machine& m, ExitReason why) {
    if (m.cpu.exit == ExitReason::None) {
        m.cpu.exit = why;
    }
}

char upper(char c) {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
}

// Compares count bytes of a with the NUL-terminated b, without regard to case.
bool same_name(const char* a, u32 count, const char* b) {
    for (u32 i = 0; i < count; ++i) {
        if (b[i] == '\0' || upper(a[i]) != upper(b[i])) {
            return false;
        }
    }
    return b[count] == '\0';
}

u32 length_of(const char* s) {
    u32 n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

void copy_cstr(char* dst, const char* src, u32 capacity) {
    u32 i = 0;
    for (; i + 1 < capacity && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

u32 copy_fault_message(u32 code, char* out, u32 size) {
    if (out == nullptr || size == 0) {
        return 0;
    }
    if (code == 0) {
        out[0] = '\0';
        return 0;
    }
    const char* message = nullptr;
    for (const FaultText& f : kFaults) {
        if (f.code == code) {
            message = f.text;
        }
    }
    u32 n = 0;
    auto add = [&](char c) {
        if (n + 1 < size) {
            out[n++] = c;
        }
    };
    if (message != nullptr) {
        for (const char* p = message; *p != '\0'; ++p) {
            add(*p);
        }
    } else {
        char digits[12];
        const bool negative = (code & 0x80000000) != 0;
        format_u64(digits, sizeof digits, negative ? 0u - code : code, 10);
        for (const char* p = "Error "; *p != '\0'; ++p) {
            add(*p);
        }
        if (negative) {
            add('-');
        }
        for (const char* p = digits; *p != '\0'; ++p) {
            add(*p);
        }
    }
    out[n] = '\0';
    return n;
}

bool format_volume_path(const char* vol_name, const char* path, char* out, u32 size) {
    if (out == nullptr || size == 0) {
        return false;
    }
    const u32 vol_len = length_of(vol_name);
    const u32 path_len = length_of(path);
    const u32 total = vol_len + 1 + path_len;
    u32 at = 0;
    for (u32 i = 0; i < vol_len && at + 1 < size; ++i) {
        out[at++] = vol_name[i];
    }
    if (at + 1 < size) {
        out[at++] = ':';
    }
    for (u32 i = 0; i < path_len && at + 1 < size; ++i) {
        out[at++] = path[i];
    }
    out[at] = '\0';
    return total + 1 <= size;
}

void put_string(Bus& b, u32 at, const char* s) {
    for (u32 i = 0;; ++i) {
        b.w8(at + i, static_cast<u8>(s[i]));
        if (s[i] == '\0') {
            return;
        }
    }
}

// A BSTR at a longword-aligned address: the length byte, at most max
// characters, and a NUL after them for C code that reads it as a string.
void put_bstr(Bus& b, u32 at, const char* s, u32 max) {
    u32 n = 0;
    while (n < max && s[n] != '\0') {
        b.w8(at + 1 + n, static_cast<u8>(s[n]));
        ++n;
    }
    b.w8(at, n);
    b.w8(at + 1 + n, 0);
}

void new_list(Bus& b, u32 list) {
    b.w32(list + LH_HEAD, list + LH_TAIL);
    b.w32(list + LH_TAIL, 0);
    b.w32(list + LH_TAILPRED, list + LH_HEAD);
}

void add_tail(Bus& b, u32 list, u32 node) {
    const u32 pred = b.r32(list + LH_TAILPRED);
    b.w32(node + LN_SUCC, list + LH_TAIL);
    b.w32(node + LN_PRED, pred);
    b.w32(pred + LN_SUCC, node);
    b.w32(list + LH_TAILPRED, node);
}

void remove(Bus& b, u32 node) {
    const u32 succ = b.r32(node + LN_SUCC);
    const u32 pred = b.r32(node + LN_PRED);
    b.w32(pred + LN_SUCC, succ);
    b.w32(succ + LN_PRED, pred);
}

void init_semaphore(Bus& b, u32 ss) {
    b.w8(ss + LN_TYPE, NT_SIGNALSEM);
    new_list(b, ss + SS_WAITQUEUE);
    b.w16(ss + SS_QUEUECOUNT, 0xFFFF);
}

u32 io_error_of(FsError error) {
    switch (error) {
    case FsError::NotFound: return ERROR_OBJECT_NOT_FOUND;
    case FsError::NotDir:
    case FsError::NotFile: return ERROR_OBJECT_WRONG_TYPE;
    case FsError::NameTooLong: return ERROR_INVALID_COMPONENT_NAME;
    default: return ERROR_NOT_A_DOS_DISK;
    }
}

// Parses a ReadArgs number: an optional sign, then decimal digits, or hex
// digits after '$' or "0x". Leading blanks are skipped. The value wraps
// modulo 2^32 as AmigaDOS's does.
bool parse_number(const char* s, u32& out) {
    while (*s == ' ' || *s == '\t') {
        ++s;
    }
    bool negative = false;
    if (*s == '+' || *s == '-') {
        negative = *s == '-';
        ++s;
    }
    u32 base = 10;
    if (*s == '$') {
        base = 16;
        ++s;
    } else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    u32 value = 0;
    u32 digits = 0;
    for (;; ++s, ++digits) {
        const char c = *s;
        u32 d = 0;
        if (c >= '0' && c <= '9') {
            d = static_cast<u32>(c - '0');
        } else if (base == 16 && upper(c) >= 'A' && upper(c) <= 'F') {
            d = static_cast<u32>(upper(c) - 'A' + 10);
        } else {
            break;
        }
        value = value * base + d;
    }
    if (digits == 0 || *s != '\0') {
        return false;
    }
    out = negative ? 0u - value : value;
    return true;
}

// --- ReadArgs parsing --------------------------------------------------------

// Template item flags.
constexpr u8 kRequired = 0x80;  // /A
constexpr u8 kKeyword = 0x40;   // /K
constexpr u8 kMultiple = 0x20;  // /M
constexpr u8 kTypeMask = 0x07;
constexpr u8 kNormal = 0;
constexpr u8 kSwitch = 1;       // /S, a keyword
constexpr u8 kToggle = 2;       // /T, a keyword
constexpr u8 kNumeric = 3;      // /N
constexpr u8 kRest = 4;         // /F

u8 modifier_flags(char c) {
    switch (upper(c)) {
    case 'A': return kRequired;
    case 'K': return kKeyword;
    case 'M': return kMultiple;
    case 'S': return kSwitch | kKeyword;
    case 'T': return kToggle | kKeyword;
    case 'N': return kNumeric;
    case 'F': return kRest;
    default: return 0;  // unknown modifiers are ignored, as AmigaDOS does
    }
}

// The item of the template a keyword names, or -1. Names are separated by
// '=' within an item and end at '/', ',' or the end.
i64 find_keyword(const char* tmpl, const char* word) {
    i64 item = 0;
    const char* t = tmpl;
    for (;;) {
        const char* w = word;
        while (*w != '\0' && upper(*w) == upper(*t)) {
            ++w;
            ++t;
        }
        if (*w == '\0' && (*t == '\0' || *t == '=' || *t == '/' || *t == ',')) {
            return item;
        }
        // On to the next name: after '=' or ','.
        for (;; ++t) {
            if (*t == '\0') {
                return -1;
            }
            if (*t == '=') {
                ++t;
                break;
            }
            if (*t == ',') {
                ++t;
                ++item;
                break;
            }
        }
    }
}

enum class Item { Nothing, Equal, Quoted, Unquoted, Error };

constexpr i64 kEnd = -1;

// One line being parsed: the text, always ending in '\n', and the strings
// taken from it, each NUL-terminated, in pool.
struct ArgLine {
    char text[kMaxLine + 1];
    u32 length;
    u32 pos;
    char pool[kMaxLine + 2];
    u32 used;
    u32 begin;  // where the last item read began, past its blanks

    i64 next() { return pos < length ? static_cast<u8>(text[pos++]) : kEnd; }

    void unget(i64 c) {
        if (c != kEnd) {
            --pos;
        }
    }

    bool store(i64 c) {
        if (used == sizeof pool) {
            return false;
        }
        pool[used++] = static_cast<char>(c);
        return true;
    }

    // Reads one item as AmigaDOS ReadItem does. Leading blanks are
    // skipped; a newline, NUL, ';' or the end gives Nothing and stays to
    // be read again; '=' alone is Equal. A quoted item takes *" for ",
    // *N for a newline, *E for escape and *x for x, and runs to its
    // closing quote; a line that ends first is an Error. An unquoted item
    // ends at a blank, '=', newline, NUL or the end; the blank or '=' is
    // read with it. The text goes into pool at start.
    Item read(u32& start) {
        start = used;
        i64 c = next();
        while (c == ' ' || c == '\t') {
            c = next();
        }
        begin = pos - (c == kEnd ? 0 : 1);
        if (c == kEnd || c == '\n' || c == 0 || c == ';') {
            unget(c);
            return Item::Nothing;
        }
        if (c == '=') {
            return Item::Equal;
        }
        if (c == '"') {
            for (;;) {
                c = next();
                if (c == '*') {
                    c = next();
                    if (c == kEnd || c == '\n' || c == 0) {
                        unget(c);
                        return Item::Error;
                    }
                    if (c == 'n' || c == 'N') {
                        c = '\n';
                    } else if (c == 'e' || c == 'E') {
                        c = 0x1B;
                    }
                } else if (c == kEnd || c == '\n' || c == 0) {
                    unget(c);
                    return Item::Error;
                } else if (c == '"') {
                    return store(0) ? Item::Quoted : Item::Error;
                }
                if (!store(c)) {
                    return Item::Error;
                }
            }
        }
        for (;;) {
            if (!store(c)) {
                return Item::Error;
            }
            c = next();
            if (c == kEnd || c == 0 || c == ' ' || c == '\t' || c == '\n' || c == '=') {
                if (c == kEnd || c == 0 || c == '\n') {
                    unget(c);
                }
                return store(0) ? Item::Unquoted : Item::Error;
            }
        }
    }

    // The rest of the line from position from, up to its newline,
    // appended to pool.
    bool store_rest(u32 from) {
        for (u32 i = from; i < length && text[i] != '\n' && text[i] != 0; ++i) {
            if (!store(text[i])) {
                return false;
            }
        }
        pos = length;
        return store(0);
    }
};

// The parse of a line against a template: for each item the pool offset of
// its text (or -1), and the strings of the /M item.
struct ArgResult {
    char tmpl[kMaxTemplate + 1];
    u8 flags[kMaxItems + 1];
    u32 items;
    i64 value[kMaxItems + 1];
    bool set[kMaxItems + 1];  // /S and /T given
    u32 multi[kMaxMulti];
    u32 multi_count;
    i64 multi_item;           // the template's /M item, or -1
};

// Fills result from the line; 0, or the IoErr code.
u32 parse_args(ArgLine& line, ArgResult& r) {
    for (u32 i = 0; i <= r.items; ++i) {
        r.value[i] = -1;
        r.set[i] = false;
    }
    r.multi_count = 0;
    r.flags[r.items] = kMultiple;  // takes what no item does
    for (u32 arg = 0; arg <= r.items;) {
        u32 next = arg + 1;
        if ((r.flags[arg] & kKeyword) != 0 || r.value[arg] >= 0 || r.set[arg]) {
            arg = next;
            continue;
        }
        u32 start = 0;
        Item kind = line.read(start);
        if (kind == Item::Unquoted) {
            const i64 key = find_keyword(r.tmpl, line.pool + start);
            if (key >= 0 && key < r.items && r.value[key] < 0 && !r.set[key]) {
                line.used = start;  // the keyword itself is not kept
                next = arg;
                arg = static_cast<u32>(key);
                const u8 type = r.flags[arg] & kTypeMask;
                if (type != kSwitch && type != kToggle) {
                    kind = line.read(start);
                    if (kind == Item::Equal) {
                        kind = line.read(start);
                    }
                    if (kind != Item::Quoted && kind != Item::Unquoted && kind != Item::Error) {
                        return ERROR_KEY_NEEDS_ARG;
                    }
                }
            }
        }
        if (kind == Item::Equal) {
            return ERROR_BAD_TEMPLATE;
        }
        if (kind == Item::Error) {
            return ERROR_LINE_TOO_LONG;
        }
        if (kind == Item::Nothing) {
            break;
        }
        const u8 type = r.flags[arg] & kTypeMask;
        if (type == kRest) {
            // An unquoted start is taken as it stands in the line, blanks
            // and '=' included; a quoted one keeps its unquoted text and
            // goes on with the line after its closing quote.
            bool stored = false;
            if (kind == Item::Unquoted) {
                line.used = start;
                stored = line.store_rest(line.begin);
            } else {
                --line.used;
                stored = line.store_rest(line.pos);
            }
            if (!stored) {
                return ERROR_LINE_TOO_LONG;
            }
            r.value[arg] = start;
            break;
        }
        if ((r.flags[arg] & kMultiple) != 0) {
            if (r.multi_count == kMaxMulti) {
                return ERROR_LINE_TOO_LONG;
            }
            r.multi[r.multi_count++] = start;
            next = arg;
        } else if (type == kSwitch || type == kToggle) {
            r.set[arg] = true;
        } else {
            r.value[arg] = start;
        }
        arg = next;
    }

    // Required items still empty take the last arguments of the /M item.
    for (u32 arg = r.items; arg-- > 0;) {
        if ((r.flags[arg] & kRequired) == 0 || (r.flags[arg] & kMultiple) != 0 ||
            r.value[arg] >= 0 || r.set[arg]) {
            continue;
        }
        if ((r.flags[arg] & kKeyword) != 0) {
            return ERROR_TOO_MANY_ARGS;  // AmigaDOS's "wrong number of arguments"
        }
        if (r.multi_count == 0) {
            return ERROR_REQUIRED_ARG_MISSING;
        }
        r.value[arg] = r.multi[--r.multi_count];
    }
    r.multi_item = -1;
    for (u32 arg = 0; arg < r.items; ++arg) {
        if ((r.flags[arg] & kMultiple) != 0) {
            r.multi_item = arg;
            if ((r.flags[arg] & kRequired) != 0 && r.multi_count == 0) {
                return ERROR_REQUIRED_ARG_MISSING;
            }
            break;
        }
    }
    if (r.multi_item < 0 && r.multi_count != 0) {
        return ERROR_TOO_MANY_ARGS;
    }
    return 0;
}

// What a hunk load allocated, so that a failed load can give it back.
struct SegmentTracker {
    Exec* exec;
    u32 addr[kMaxHunks];
    u32 bytes[kMaxHunks];
    u32 count;

    static u32 alloc(void* ctx, u32 bytes) {
        SegmentTracker& t = *static_cast<SegmentTracker*>(ctx);
        if (t.count == kMaxHunks) {
            return 0;
        }
        const u32 at = t.exec->alloc(bytes, MEMF_PUBLIC);
        if (at != 0) {
            t.addr[t.count] = at;
            t.bytes[t.count] = bytes;
            ++t.count;
        }
        return at;
    }

    void free_all() {
        while (count > 0) {
            --count;
            exec->free(addr[count], bytes[count]);
        }
    }
};

}

// The dos natives. A member of no instance: each gets the Dos as its
// context.
struct DosOps {
    static Dos& of(void* ctx) { return *static_cast<Dos*>(ctx); }

    // --- the Process ---------------------------------------------------------

    static void set_io_err(Dos& d, Bus& b, u32 code) { b.w32(d.process_ + pr_Result2, code); }

    // Sets IoErr and D0 together.
    static void fail(Dos& d, Bus& b, u32 code, u32 result) {
        set_io_err(d, b, code);
        if (b.ok()) {
            native::set_result(b.machine(), result);
        }
    }

    // --- handles and locks -------------------------------------------------

    // The slot of an open handle, by its BPTR alone; -1 if it is not one.
    static i64 handle_slot(const Dos& d, u32 bptr) {
        const u64 at = static_cast<u64>(bptr) << 2;
        if (at < d.handles_ || (at - d.handles_) % fh_SIZEOF != 0) {
            return -1;
        }
        const u64 slot = (at - d.handles_) / fh_SIZEOF;
        if (slot >= kHandles || d.files_[slot].stream == Dos::Stream::Closed) {
            return -1;
        }
        return static_cast<i64>(slot);
    }

    static i64 lock_slot(const Dos& d, u32 bptr) {
        const u64 at = static_cast<u64>(bptr) << 2;
        if (at < d.locks_ || (at - d.locks_) % fl_SIZEOF != 0) {
            return -1;
        }
        const u64 slot = (at - d.locks_) / fl_SIZEOF;
        if (slot >= Dos::kMaxLocks || !d.lock_slots_[slot].used) {
            return -1;
        }
        return static_cast<i64>(slot);
    }

    static i64 assign_lock_slot(const Dos& d, u32 bptr) {
        const u64 at = static_cast<u64>(bptr) << 2;
        if (at < d.assign_locks_ || (at - d.assign_locks_) % fl_SIZEOF != 0) {
            return -1;
        }
        const u64 slot = (at - d.assign_locks_) / fl_SIZEOF;
        if (slot >= Dos::kMaxAssignLocks || !d.assign_lock_slots_[slot].used) {
            return -1;
        }
        return static_cast<i64>(slot);
    }

    static Dos::LockSlot* find_lock_slot(Dos& d, u32 bptr) {
        const i64 user = lock_slot(d, bptr);
        if (user >= 0) {
            return &d.lock_slots_[user];
        }
        const i64 assign = assign_lock_slot(d, bptr);
        if (assign >= 0) {
            return &d.assign_lock_slots_[assign];
        }
        return nullptr;
    }

    static u32 handle_bptr(const Dos& d, u32 slot) { return (d.handles_ + slot * fh_SIZEOF) >> 2; }
    static u32 lock_bptr(const Dos& d, u32 slot) { return (d.locks_ + slot * fl_SIZEOF) >> 2; }
    static u32 assign_lock_bptr(const Dos& d, u32 slot) {
        return (d.assign_locks_ + slot * fl_SIZEOF) >> 2;
    }

    static void init_handle(Dos& d, Bus& b, u32 slot, bool interactive) {
        const u32 at = d.handles_ + slot * fh_SIZEOF;
        for (u32 i = 0; i < fh_SIZEOF; i += 4) {
            b.w32(at + i, 0);
        }
        b.w32(at + fh_Interactive, interactive ? DOSTRUE : 0);
        b.w32(at + fh_Type, d.port_);
        b.w32(at + fh_Args, slot);
    }

    // A free handle slot past the two console streams, or -1.
    static i64 free_handle(const Dos& d) {
        for (u32 slot = kConsoleOut + 1; slot < kHandles; ++slot) {
            if (d.files_[slot].stream == Dos::Stream::Closed) {
                return slot;
            }
        }
        return -1;
    }

    // Fails with ERROR_NO_FREE_STORE because a table of dos is full.
    static void table_full(Dos& d, Bus& b, const char* what, u32 entries) {
        LogLine().text("dos: ").text(what).text(" table full, ").dec(entries)
            .text(" entries in use").send();
        fail(d, b, ERROR_NO_FREE_STORE, 0);
    }

    static void log_bad_handle(const char* what, u32 bptr) {
        LogLine().text("dos: ").text(what).text(" a bad file handle ").hex(bptr).send();
    }

    // Hands bytes to the stream; false, with IoErr set, if it cannot take
    // them.
    static bool write_slot(Dos& d, Bus& b, u32 slot, const u8* data, u32 bytes) {
        Dos::FileSlot& f = d.files_[slot];
        if (f.stream == Dos::Stream::Console) {
            if (d.out_ != nullptr && bytes != 0) {
                d.out_(d.out_ctx_, data, bytes);
            }
            return true;
        }
        if (f.entry.volume != Dos::kVolRam) {
            set_io_err(d, b, ERROR_DISK_WRITE_PROTECTED);
            return false;
        }
        if (f.entry.header_block == 0) {
            set_io_err(d, b, ERROR_OBJECT_NOT_FOUND);
            return false;
        }
        f.has_unget = false;
        f.has_last = false;
        const u32 node = f.entry.header_block - 1;
        const u32 err = d.ram_.write(node, f.pos, data, bytes);
        if (err != 0) {
            set_io_err(d, b, err);
            return false;
        }
        f.pos += bytes;
        f.entry.size = d.ram_.node(node).size;
        return true;
    }

    // Characters bound for a stream, passed on in pieces.
    struct Sink {
        Dos& d;
        Bus& b;
        u32 slot;
        u8 buffer[kChunk];
        u32 held;
        u32 total;
        bool failed;

        Sink(Dos& dos, Bus& bus, u32 s) : d(dos), b(bus), slot(s), held(0), total(0), failed(false) {}

        bool put(u32 c) {
            if (failed) {
                return false;
            }
            buffer[held++] = static_cast<u8>(c);
            ++total;
            return held < kChunk || flush();
        }

        bool text(const char* s) {
            for (; *s != '\0'; ++s) {
                if (!put(static_cast<u8>(*s))) {
                    return false;
                }
            }
            return true;
        }

        bool flush() {
            if (held != 0 && !failed && !write_slot(d, b, slot, buffer, held)) {
                failed = true;
            }
            held = 0;
            return !failed;
        }
    };

    // Copies a guest string into the sink; false if the guest faulted or
    // the stream failed. The walk ends at the end of guest memory.
    static bool put_guest_string(Bus& b, Sink& out, u32 addr) {
        for (;; ++addr) {
            const u32 c = b.r8(addr);
            if (!b.ok()) {
                out.flush();
                return false;
            }
            if (c == 0) {
                return true;
            }
            if (!out.put(c)) {
                return false;
            }
        }
    }

    // --- volume dispatch ---------------------------------------------------

    static const char* volume_name(const Dos& d, u8 vol) {
        return vol == Dos::kVolRam ? "Ram Disk" : d.volume_.name;
    }

    static u32 volume_node(const Dos& d, u8 vol) {
        return vol == Dos::kVolRam ? d.ram_volume_node_ : d.volume_node_;
    }

    static DirEntry ram_entry(const Dos& d, u32 node) {
        const RamFs::Node& n = d.ram_.node(node);
        DirEntry e{};
        copy_cstr(e.name, n.name, sizeof e.name);
        e.is_dir = n.is_dir;
        e.size = n.is_dir ? 0 : n.size;
        e.header_block = node + 1;
        e.volume = Dos::kVolRam;
        return e;
    }

    static bool is_root_entry(const Dos& d, const DirEntry& entry) {
        return entry.volume == Dos::kVolRam ? entry.header_block == 1
                                            : entry.header_block == d.volume_.root_block;
    }

    static void retain_entry(Dos& d, const DirEntry& entry) {
        if (entry.volume == Dos::kVolRam && entry.header_block > 0) {
            d.ram_.retain(entry.header_block - 1);
        }
    }

    static void release_entry(Dos& d, const DirEntry& entry) {
        if (entry.volume == Dos::kVolRam && entry.header_block > 0) {
            d.ram_.release(entry.header_block - 1);
        }
    }

    static bool ram_node_busy(const Dos& d, u32 node, bool want_exclusive) {
        const u32 key = node + 1;
        for (const Dos::LockSlot& l : d.lock_slots_) {
            if (l.used && l.entry.volume == Dos::kVolRam && l.entry.header_block == key &&
                (want_exclusive || l.exclusive)) {
                return true;
            }
        }
        for (const Dos::LockSlot& l : d.assign_lock_slots_) {
            if (l.used && l.entry.volume == Dos::kVolRam && l.entry.header_block == key &&
                (want_exclusive || l.exclusive)) {
                return true;
            }
        }
        for (u32 i = kConsoleOut + 1; i < kHandles; ++i) {
            const Dos::FileSlot& f = d.files_[i];
            if (f.stream == Dos::Stream::File && f.entry.volume == Dos::kVolRam &&
                f.entry.header_block == key && (want_exclusive || f.exclusive)) {
                return true;
            }
        }
        return false;
    }

    static bool ram_is_at_or_below(const RamFs& ram, u32 node, u32 ancestor) {
        for (u32 cur = node; cur != RamFs::kNone && ram.valid(cur); cur = ram.parent(cur)) {
            if (cur == ancestor) {
                return true;
            }
        }
        return false;
    }

    static bool ram_node_path(const RamFs& ram, u32 node, char (&out)[Dos::kMaxPath]) {
        if (!ram.valid(node)) {
            return false;
        }
        if (node == 0) {
            out[0] = '\0';
            return true;
        }
        u32 chain[64];
        u32 depth = 0;
        for (u32 cur = node; cur != 0 && cur != RamFs::kNone; cur = ram.parent(cur)) {
            if (depth >= 64) {
                return false;
            }
            chain[depth++] = cur;
        }
        u32 pos = 0;
        for (u32 i = depth; i-- > 0;) {
            const char* name = ram.node(chain[i]).name;
            if (pos != 0) {
                if (pos + 1 >= Dos::kMaxPath) {
                    return false;
                }
                out[pos++] = '/';
            }
            for (u32 k = 0; name[k] != '\0'; ++k) {
                if (pos + 1 >= Dos::kMaxPath) {
                    return false;
                }
                out[pos++] = name[k];
            }
        }
        out[pos] = '\0';
        return true;
    }

    static void split_parent_and_leaf(const char* resolved, char (&parent)[Dos::kMaxPath],
                                      const char*& leaf) {
        u32 slash = 0;
        bool has_slash = false;
        for (u32 i = 0; resolved[i] != '\0'; ++i) {
            if (resolved[i] == '/') {
                slash = i;
                has_slash = true;
            }
        }
        if (has_slash) {
            for (u32 i = 0; i < slash && i + 1 < Dos::kMaxPath; ++i) {
                parent[i] = resolved[i];
            }
            parent[slash < Dos::kMaxPath ? slash : Dos::kMaxPath - 1] = '\0';
            leaf = resolved + slash + 1;
        } else {
            parent[0] = '\0';
            leaf = resolved;
        }
    }

    static u32 ram_alloc(void* ctx, u32 bytes) {
        return of(ctx).exec_->alloc(bytes, MEMF_PUBLIC);
    }

    static void ram_free(void* ctx, u32 addr, u32 bytes) {
        Dos& d = of(ctx);
        if (addr != 0 && d.m_ != nullptr) {
            d.exec_->free(native::guest_address(*d.m_, addr), bytes);
        }
    }

    static void ram_read(void* ctx, u32 addr, u8* dst, u32 n) {
        Dos& d = of(ctx);
        if (n == 0 || d.m_ == nullptr) {
            return;
        }
        Bus b(*d.m_);
        const u8* src = b.bytes_at(addr, n);
        if (src != nullptr) {
            for (u32 i = 0; i < n; ++i) {
                dst[i] = src[i];
            }
        }
    }

    static void ram_write(void* ctx, u32 addr, const u8* src, u32 n) {
        Dos& d = of(ctx);
        if (n == 0 || d.m_ == nullptr) {
            return;
        }
        Bus b(*d.m_);
        u8* dst = b.bytes_at(addr, n);
        if (dst != nullptr) {
            for (u32 i = 0; i < n; ++i) {
                dst[i] = src[i];
            }
        }
    }

    static void ram_now(void* ctx, u32& days, u32& minute, u32& tick) {
        const DosStamp s = current_stamp(of(ctx));
        days = s.days;
        minute = s.minutes;
        tick = s.ticks;
    }

    static u32 fs_find(const Dos& d, u8 vol, const char* path, DirEntry& out) {
        if (vol == Dos::kVolRam) {
            u32 node = RamFs::kNone;
            const u32 err = d.ram_.find_path(path, node);
            if (err != 0) {
                return err;
            }
            out = ram_entry(d, node);
            return 0;
        }
        const FsError err = adf_find(d.volume_, path, out);
        return err == FsError::Ok ? 0 : io_error_of(err);
    }

    static u32 fs_list(const Dos& d, u8 vol, const char* path, DirVisitor visitor, void* ctx) {
        if (vol == Dos::kVolRam) {
            u32 dir = RamFs::kNone;
            const u32 err = d.ram_.find_path(path, dir);
            if (err != 0) {
                return err;
            }
            if (!d.ram_.node(dir).is_dir) {
                return ERROR_OBJECT_WRONG_TYPE;
            }
            for (u32 child = d.ram_.next_child(dir, RamFs::kNone); child != RamFs::kNone;
                 child = d.ram_.next_child(dir, child)) {
                if (visitor != nullptr) {
                    visitor(ctx, ram_entry(d, child));
                }
            }
            return 0;
        }
        const FsError err = adf_list(d.volume_, path, visitor, ctx);
        return err == FsError::Ok ? 0 : io_error_of(err);
    }

    static u32 fs_list_from(const Dos& d, const DirEntry& dir, const char* path, u32 after_key,
                            bool from_start, DirVisitor visitor, void* ctx) {
        if (dir.volume == Dos::kVolRam) {
            if (dir.header_block == 0) {
                return ERROR_OBJECT_NOT_FOUND;
            }
            const u32 dir_node = dir.header_block - 1;
            if (!d.ram_.valid(dir_node)) {
                return ERROR_OBJECT_NOT_FOUND;
            }
            if (!d.ram_.node(dir_node).is_dir) {
                return ERROR_OBJECT_WRONG_TYPE;
            }
            const u32 start = (from_start || after_key == 0) ? RamFs::kNone : (after_key - 1);
            for (u32 child = d.ram_.next_child(dir_node, start); child != RamFs::kNone;
                 child = d.ram_.next_child(dir_node, child)) {
                if (visitor != nullptr) {
                    visitor(ctx, ram_entry(d, child));
                }
            }
            return 0;
        }
        const FsError err = adf_list(d.volume_, path, visitor, ctx);
        return err == FsError::Ok ? 0 : io_error_of(err);
    }

    static u32 fs_read(const Dos& d, const DirEntry& entry, u32 pos, u8* dst, u32 n, u32& got) {
        if (entry.volume == Dos::kVolRam) {
            if (entry.header_block == 0) {
                got = 0;
                return ERROR_OBJECT_NOT_FOUND;
            }
            return d.ram_.read(entry.header_block - 1, pos, dst, n, got);
        }
        const FsError err = adf_read(d.volume_, entry, pos, dst, n, got);
        return err == FsError::Ok ? 0 : io_error_of(err);
    }

    static u32 fs_size(const Dos& d, const DirEntry& entry) {
        if (entry.volume == Dos::kVolRam) {
            if (entry.header_block == 0) {
                return 0;
            }
            const u32 node = entry.header_block - 1;
            if (!d.ram_.valid(node) || d.ram_.node(node).is_dir) {
                return 0;
            }
            return d.ram_.node(node).size;
        }
        return entry.size;
    }

    static void fs_canonical(const Dos& d, u8 vol, const char* resolved,
                             char (&out)[Dos::kMaxPath]) {
        if (resolved[0] == '\0') {
            out[0] = '\0';
            return;
        }
        char probe[Dos::kMaxPath];
        u32 out_pos = 0;
        u32 i = 0;
        while (resolved[i] != '\0') {
            u32 comp_len = 0;
            while (resolved[i + comp_len] != '\0' && resolved[i + comp_len] != '/') {
                ++comp_len;
            }
            if (out_pos != 0 && out_pos + 1 < Dos::kMaxPath) {
                probe[out_pos] = '/';
                out[out_pos] = '/';
                ++out_pos;
            }
            u32 p_pos = out_pos;
            for (u32 k = 0; k < comp_len && p_pos + 1 < Dos::kMaxPath; ++k) {
                probe[p_pos++] = resolved[i + k];
            }
            probe[p_pos] = '\0';
            DirEntry e{};
            const char* name = probe + out_pos;
            if (fs_find(d, vol, probe, e) == 0 && e.name[0] != '\0') {
                name = e.name;
            }
            for (u32 k = 0; name[k] != '\0' && out_pos + 1 < Dos::kMaxPath; ++k) {
                out[out_pos] = name[k];
                probe[out_pos] = name[k];
                ++out_pos;
            }
            i += comp_len;
            if (resolved[i] == '/') {
                ++i;
            }
        }
        out[out_pos] = '\0';
    }

    struct FsMeta {
        u32 type;
        u32 size;
        u32 num_blocks;
        u32 protection;
        u32 days;
        u32 minute;
        u32 tick;
        const char* comment;
        u32 comment_len;
    };

    static u32 fs_meta(const Dos& d, const DirEntry& entry, FsMeta& out) {
        if (entry.volume == Dos::kVolRam) {
            if (entry.header_block == 0) {
                return ERROR_OBJECT_NOT_FOUND;
            }
            const u32 node = entry.header_block - 1;
            if (!d.ram_.valid(node)) {
                return ERROR_OBJECT_NOT_FOUND;
            }
            const RamFs::Node& n = d.ram_.node(node);
            out.type = node == 0 ? ST_ROOT : (n.is_dir ? ST_USERDIR : ST_FILE);
            out.size = n.is_dir ? 0 : n.size;
            out.num_blocks = n.is_dir || n.size == 0 ? 0 : (n.size - 1) / 512 + 1;
            out.protection = n.protection;
            out.days = n.date.days;
            out.minute = n.date.minute;
            out.tick = n.date.tick;
            out.comment = n.comment;
            const u32 clen = length_of(n.comment);
            out.comment_len = clen < 79 ? clen : 79;
            return 0;
        }
        using namespace amiga_fs_detail;
        FsError error = FsError::Ok;
        const u8* header = load_header(d.volume_, entry.header_block, error);
        if (header == nullptr) {
            return io_error_of(error);
        }
        const u32 payload = d.volume_.ffs ? block_bytes : block_bytes - ofs_header_bytes;
        out.type = long_at(header, l_sec_type);
        out.size = entry.size;
        out.num_blocks = entry.is_dir ? 0 : (entry.size + payload - 1) / payload;
        out.protection = long_at(header, block_longs - 48);
        out.days = long_at(header, block_longs - 23);
        out.minute = long_at(header, block_longs - 22);
        out.tick = long_at(header, block_longs - 21);
        const u8* comment = header + (block_longs - 46) * 4;
        out.comment_len = comment[0] < 79 ? comment[0] : 79;
        out.comment = reinterpret_cast<const char*>(comment + 1);
        return 0;
    }

    // --- names and paths ---------------------------------------------------

    // Reads a guest name; false if it faults (b.ok() is then false) or is
    // longer than dos takes, which sets IoErr.
    static bool read_name(Dos& d, Bus& b, u32 addr, char (&name)[kMaxName]) {
        if (b.string(addr, name, kMaxName)) {
            return true;
        }
        if (b.ok()) {
            set_io_err(d, b, ERROR_INVALID_COMPONENT_NAME);
        }
        return false;
    }

    static bool is_console(const char* name) {
        return same_name(name, length_of(name), "*") ||
               same_name(name, length_of(name), "CONSOLE:");
    }

    static bool is_volume_or_device(const Dos& d, const char* name, u32 len) {
        return same_name(name, len, d.volume_.name) || same_name(name, len, "DF0") ||
               same_name(name, len, "Ram Disk") || same_name(name, len, "RAM");
    }

    static bool is_mounted_device(const Dos& d, const char* name, u32 colon) {
        if (colon == 0 || is_volume_or_device(d, name, colon)) {
            return true;
        }
        return find_assign(d, name, colon) >= 0;
    }

    static i64 find_assign(const Dos& d, const char* name, u32 count) {
        for (u32 i = 0; i < Dos::kMaxAssigns; ++i) {
            if (d.assigns_[i].used && same_name(name, count, d.assigns_[i].name)) {
                return static_cast<i64>(i);
            }
        }
        return -1;
    }

    static i64 free_assign_slot(const Dos& d) {
        for (u32 i = 0; i < Dos::kMaxAssigns; ++i) {
            if (!d.assigns_[i].used) {
                return static_cast<i64>(i);
            }
        }
        return -1;
    }

    static void release_lock(Dos& d, u32 bptr) {
        if (bptr == 0) {
            return;
        }
        const i64 user = lock_slot(d, bptr);
        if (user >= 0) {
            release_entry(d, d.lock_slots_[user].entry);
            d.lock_slots_[user].used = false;
            d.lock_slots_[user].command_owned = false;
            d.lock_slots_[user].exclusive = false;
            return;
        }
        const i64 assign = assign_lock_slot(d, bptr);
        if (assign >= 0) {
            release_entry(d, d.assign_lock_slots_[assign].entry);
            d.assign_lock_slots_[assign].used = false;
            d.assign_lock_slots_[assign].command_owned = false;
            d.assign_lock_slots_[assign].exclusive = false;
        }
    }

    static bool alloc_assign_lock(Dos& d, Bus& b, const char* path, const DirEntry& entry,
                                  u32& out_bptr) {
        if (entry.volume == Dos::kVolRam && entry.header_block > 0 &&
            ram_node_busy(d, entry.header_block - 1, false)) {
            return false;
        }
        for (u32 i = 0; i < Dos::kMaxAssignLocks; ++i) {
            if (!d.assign_lock_slots_[i].used) {
                Dos::LockSlot& l = d.assign_lock_slots_[i];
                l.used = true;
                l.command_owned = false;
                l.exclusive = false;
                l.entry = entry;
                char canonical[Dos::kMaxPath];
                fs_canonical(d, entry.volume, path, canonical);
                copy_cstr(l.path, canonical, Dos::kMaxPath);
                retain_entry(d, entry);
                init_lock_at(d, b, d.assign_locks_ + i * fl_SIZEOF, entry.volume,
                             entry.header_block, SHARED_LOCK);
                out_bptr = assign_lock_bptr(d, i);
                return b.ok();
            }
        }
        return false;
    }

    static void relink_dos_list(Dos& d, Bus& b) {
        b.w32(d.volume_node_ + dol_Next, d.ram_volume_node_ >> 2);
        b.w32(d.ram_volume_node_ + dol_Next, d.device_node_ >> 2);
        b.w32(d.device_node_ + dol_Next, d.ram_device_node_ >> 2);
        u32 prev = d.ram_device_node_;
        for (u32 i = 0; i < Dos::kMaxAssigns; ++i) {
            if (d.assigns_[i].used && d.assigns_[i].node != 0) {
                b.w32(prev + dol_Next, d.assigns_[i].node >> 2);
                prev = d.assigns_[i].node;
            }
        }
        b.w32(prev + dol_Next, 0);
    }

    static bool build_assign_node(Dos& d, Bus& b, Dos::AssignEntry& e) {
        const u32 name_len = length_of(e.name);
        const u32 bstr_bytes = align4(1 + name_len + 1);
        u32 extra_bytes = 0;
        if (e.type == DLT_LATE || e.type == DLT_NONBINDING) {
            extra_bytes = align4(length_of(e.path) + 1);
        } else if (e.type == DLT_DIRECTORY && e.dir_count > 1) {
            extra_bytes = (e.dir_count - 1) * AssignList_SIZEOF;
        }
        const u32 total = DosList_SIZEOF + bstr_bytes + extra_bytes;
        const u32 node = d.exec_->alloc(total, MEMF_PUBLIC | MEMF_CLEAR);
        if (node == 0) {
            return false;
        }
        b.w32(node + dol_Next, 0);
        b.w32(node + dol_Type, e.type);
        b.w32(node + dol_Task, d.port_);
        const u32 bstr_at = node + DosList_SIZEOF;
        put_bstr(b, bstr_at, e.name, kMaxComponent);
        b.w32(node + dol_Name, bstr_at >> 2);
        if (e.type == DLT_LATE || e.type == DLT_NONBINDING) {
            const u32 path_at = bstr_at + bstr_bytes;
            put_string(b, path_at, e.path);
            b.w32(node + dol_AssignName, path_at);
        } else {
            b.w32(node + dol_Lock, e.locks[0]);
            if (e.dir_count > 1) {
                const u32 list_at = bstr_at + bstr_bytes;
                b.w32(node + dol_List, list_at);
                for (u32 i = 1; i < e.dir_count; ++i) {
                    const u32 al_at = list_at + (i - 1) * AssignList_SIZEOF;
                    const u32 next_al = (i + 1 < e.dir_count) ? (al_at + AssignList_SIZEOF) : 0;
                    b.w32(al_at + al_Next, next_al);
                    b.w32(al_at + al_Lock, e.locks[i]);
                }
            }
        }
        if (!b.ok()) {
            d.exec_->free(node, total);
            return false;
        }
        if (e.node != 0) {
            d.exec_->free(e.node, e.node_bytes);
        }
        e.node = node;
        e.node_bytes = total;
        return true;
    }

    static bool resolve_late_assign(Dos& d, Bus& b, Dos::AssignEntry& e, u32& error, u32 depth) {
        u8 vol = Dos::kVolAdf;
        char resolved[Dos::kMaxPath];
        if (!resolve(d, b, e.path, vol, resolved, error, depth)) {
            return false;
        }
        DirEntry entry{};
        const u32 err = fs_find(d, vol, resolved, entry);
        if (err != 0) {
            error = err;
            return false;
        }
        if (!entry.is_dir) {
            error = ERROR_OBJECT_WRONG_TYPE;
            return false;
        }
        u32 lock_bptr = 0;
        if (!alloc_assign_lock(d, b, resolved, entry, lock_bptr)) {
            error = (entry.volume == Dos::kVolRam && entry.header_block > 0 &&
                     ram_node_busy(d, entry.header_block - 1, false))
                        ? ERROR_OBJECT_IN_USE
                        : ERROR_NO_FREE_STORE;
            return false;
        }
        char saved_path[Dos::kMaxPath];
        copy_cstr(saved_path, e.path, Dos::kMaxPath);
        e.type = DLT_DIRECTORY;
        e.dir_count = 1;
        e.locks[0] = lock_bptr;
        e.path[0] = '\0';
        if (!build_assign_node(d, b, e)) {
            release_lock(d, lock_bptr);
            e.type = DLT_LATE;
            e.dir_count = 0;
            copy_cstr(e.path, saved_path, Dos::kMaxPath);
            error = ERROR_NO_FREE_STORE;
            return false;
        }
        relink_dos_list(d, b);
        return b.ok();
    }

    static void file_part(void*, Machine& m) {
        Bus b(m);
        const u32 path = m.cpu.d[1];
        if (path == 0) {
            native::set_result(m, 0);
            return;
        }
        u32 last = path;
        for (u32 i = 0; i < kMaxName * 4; ++i) {
            const u32 c = b.r8(path + i);
            if (!b.ok()) {
                return;
            }
            if (c == 0) {
                break;
            }
            if (c == '/' || c == ':') {
                last = path + i + 1;
            }
        }
        native::set_result(m, last);
    }

    static void path_part(void*, Machine& m) {
        Bus b(m);
        const u32 path = m.cpu.d[1];
        if (path == 0) {
            native::set_result(m, 0);
            return;
        }
        u32 last = path;
        for (u32 i = 0; i < kMaxName * 4; ++i) {
            const u32 c = b.r8(path + i);
            if (!b.ok()) {
                return;
            }
            if (c == 0) {
                break;
            }
            if (c == ':') {
                last = path + i + 1;
            } else if (c == '/') {
                last = path + i;
            }
        }
        native::set_result(m, last);
    }

    static void add_part(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 dirname = m.cpu.d[1];
        const u32 filename = m.cpu.d[2];
        const u32 size = m.cpu.d[3];
        if (dirname == 0 || filename == 0 || size == 0 || (size & 0x80000000) != 0) {
            fail(d, b, ERROR_LINE_TOO_LONG, DOSFALSE);
            return;
        }
        constexpr u32 kAddPartMax = 1024;
        const u32 limit = size < kAddPartMax ? size : kAddPartMax;
        char fn[kAddPartMax];
        u32 fn_len = 0;
        bool has_colon = false;
        for (;;) {
            if (fn_len >= limit) {
                fail(d, b, ERROR_LINE_TOO_LONG, DOSFALSE);
                return;
            }
            const u32 c = b.r8(filename + fn_len);
            if (!b.ok()) {
                return;
            }
            fn[fn_len] = static_cast<char>(c);
            if (c == 0) {
                break;
            }
            if (c == ':') {
                has_colon = true;
            }
            ++fn_len;
        }
        if (has_colon) {
            for (u32 i = 0; i <= fn_len; ++i) {
                b.w8(dirname + i, static_cast<u8>(fn[i]));
            }
            if (b.ok()) {
                native::set_result(m, DOSTRUE);
            }
            return;
        }
        u32 dir_len = 0;
        u32 last_ch = 0;
        for (;;) {
            if (dir_len >= limit) {
                fail(d, b, ERROR_LINE_TOO_LONG, DOSFALSE);
                return;
            }
            const u32 c = b.r8(dirname + dir_len);
            if (!b.ok()) {
                return;
            }
            if (c == 0) {
                break;
            }
            last_ch = c;
            ++dir_len;
        }
        if (fn_len == 0) {
            native::set_result(m, DOSTRUE);
            return;
        }
        const bool need_slash = dir_len > 0 && last_ch != ':' && last_ch != '/';
        const u32 total = dir_len + (need_slash ? 1 : 0) + fn_len;
        if (total + 1 > size) {
            fail(d, b, ERROR_LINE_TOO_LONG, DOSFALSE);
            return;
        }
        u32 at = dir_len;
        if (need_slash) {
            b.w8(dirname + at++, '/');
        }
        for (u32 i = 0; i <= fn_len; ++i) {
            b.w8(dirname + at + i, static_cast<u8>(fn[i]));
        }
        if (b.ok()) {
            native::set_result(m, DOSTRUE);
        }
    }

    static void is_file_system(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        if (m.cpu.d[1] == 0) {
            fail(d, b, ERROR_REQUIRED_ARG_MISSING, DOSFALSE);
            return;
        }
        char name[kMaxName];
        if (!read_name(d, b, m.cpu.d[1], name)) {
            native::set_result(m, DOSFALSE);
            return;
        }
        if (is_console(name)) {
            native::set_result(m, DOSFALSE);
            return;
        }
        u32 colon = 0;
        while (name[colon] != '\0' && name[colon] != ':') {
            ++colon;
        }
        if (name[colon] != ':') {
            native::set_result(m, DOSTRUE);
            return;
        }
        if (is_mounted_device(d, name, colon)) {
            native::set_result(m, DOSTRUE);
            return;
        }
        fail(d, b, ERROR_DEVICE_NOT_MOUNTED, DOSFALSE);
    }

    static bool apply_relative_path(const char* base, const char* rest,
                                    char (&out)[Dos::kMaxPath], u32& error) {
        u32 at = 0;
        for (; base[at] != '\0'; ++at) {
            if (at + 1 >= Dos::kMaxPath) {
                error = ERROR_INVALID_COMPONENT_NAME;
                return false;
            }
            out[at] = base[at];
        }
        // Each '/' that does not end a name goes up one level.
        u32 i = 0;
        while (rest[i] != '\0') {
            if (rest[i] == '/') {
                if (at == 0) {
                    error = ERROR_OBJECT_NOT_FOUND;  // nothing above the root
                    return false;
                }
                while (at > 0 && out[at - 1] != '/') {
                    --at;
                }
                if (at > 0) {
                    --at;  // the separator before the name dropped
                }
                ++i;
                continue;
            }
            u32 length = 0;
            while (rest[i + length] != '\0' && rest[i + length] != '/') {
                if (rest[i + length] == ':') {
                    error = ERROR_INVALID_COMPONENT_NAME;
                    return false;
                }
                ++length;
            }
            if (length > kMaxComponent || at + 1 + length + 1 > Dos::kMaxPath) {
                error = ERROR_INVALID_COMPONENT_NAME;
                return false;
            }
            if (at != 0) {
                out[at++] = '/';
            }
            for (u32 k = 0; k < length; ++k) {
                out[at++] = rest[i + k];
            }
            i += length;
            if (rest[i] == '/') {
                ++i;  // the separator after a name
            }
        }
        out[at] = '\0';
        return true;
    }

    // Turns an AmigaDOS name into a volume and a path from that volume's root,
    // its components separated by '/'. A name with a device starts where the
    // volume, device or assign points; one without starts at the current
    // directory.
    static bool resolve(Dos& d, Bus& b, const char* name, u8& vol, char (&out)[Dos::kMaxPath],
                        u32& error, u32 depth = 0) {
        if (depth >= kMaxAssignDepth) {
            error = ERROR_TOO_MANY_LEVELS;
            return false;
        }
        u32 colon = 0;
        while (name[colon] != '\0' && name[colon] != ':') {
            ++colon;
        }
        if (name[colon] == ':') {
            const char* rest = name + colon + 1;
            if (colon == 0) {
                const u32 current = b.r32(d.process_ + pr_CurrentDir);
                if (!b.ok()) {
                    return false;
                }
                vol = Dos::kVolAdf;
                if (current != 0) {
                    const Dos::LockSlot* slot = find_lock_slot(d, current);
                    if (slot == nullptr) {
                        error = ERROR_INVALID_LOCK;
                        return false;
                    }
                    vol = slot->entry.volume;
                }
                return apply_relative_path("", rest, out, error);
            }
            if (same_name(name, colon, d.volume_.name) || same_name(name, colon, "DF0")) {
                vol = Dos::kVolAdf;
                return apply_relative_path("", rest, out, error);
            }
            if (same_name(name, colon, "Ram Disk") || same_name(name, colon, "RAM")) {
                vol = Dos::kVolRam;
                return apply_relative_path("", rest, out, error);
            }
            const i64 idx = find_assign(d, name, colon);
            if (idx < 0) {
                error = ERROR_DEVICE_NOT_MOUNTED;
                return false;
            }
            Dos::AssignEntry& e = d.assigns_[idx];
            if (e.type == DLT_LATE) {
                if (!resolve_late_assign(d, b, e, error, depth + 1)) {
                    return false;
                }
            } else if (e.type == DLT_NONBINDING) {
                u8 base_vol = Dos::kVolAdf;
                char base[Dos::kMaxPath];
                if (!resolve(d, b, e.path, base_vol, base, error, depth + 1)) {
                    return false;
                }
                vol = base_vol;
                return apply_relative_path(base, rest, out, error);
            }
            if (e.dir_count == 0) {
                error = ERROR_DEVICE_NOT_MOUNTED;
                return false;
            }
            if (e.dir_count == 1) {
                const Dos::LockSlot* slot = find_lock_slot(d, e.locks[0]);
                if (slot == nullptr) {
                    error = ERROR_INVALID_LOCK;
                    return false;
                }
                vol = slot->entry.volume;
                return apply_relative_path(slot->path, rest, out, error);
            }
            for (u32 i = 0; i < e.dir_count; ++i) {
                const Dos::LockSlot* slot = find_lock_slot(d, e.locks[i]);
                if (slot == nullptr) {
                    continue;
                }
                char cand[Dos::kMaxPath];
                u32 cand_err = 0;
                if (!apply_relative_path(slot->path, rest, cand, cand_err)) {
                    if (cand_err == ERROR_INVALID_COMPONENT_NAME) {
                        error = cand_err;
                        return false;
                    }
                    continue;
                }
                DirEntry tmp{};
                if (fs_find(d, slot->entry.volume, cand, tmp) == 0) {
                    vol = slot->entry.volume;
                    copy_cstr(out, cand, Dos::kMaxPath);
                    return true;
                }
            }
            const Dos::LockSlot* slot0 = find_lock_slot(d, e.locks[0]);
            if (slot0 == nullptr) {
                error = ERROR_INVALID_LOCK;
                return false;
            }
            vol = slot0->entry.volume;
            return apply_relative_path(slot0->path, rest, out, error);
        }
        const u32 current = b.r32(d.process_ + pr_CurrentDir);
        if (!b.ok()) {
            return false;
        }
        vol = Dos::kVolAdf;
        const char* base = "";
        if (current != 0) {
            const Dos::LockSlot* slot = find_lock_slot(d, current);
            if (slot == nullptr) {
                error = ERROR_INVALID_LOCK;
                return false;
            }
            vol = slot->entry.volume;
            base = slot->path;
        }
        return apply_relative_path(base, name, out, error);
    }

    // Resolves a name and looks it up on the volume.
    static bool locate(Dos& d, Bus& b, const char* name, u8& vol, char (&path)[Dos::kMaxPath],
                       DirEntry& entry, u32& error) {
        if (!resolve(d, b, name, vol, path, error)) {
            return false;
        }
        const u32 err = fs_find(d, vol, path, entry);
        if (err != 0) {
            error = err;
            return false;
        }
        char canonical[Dos::kMaxPath];
        fs_canonical(d, vol, path, canonical);
        copy_cstr(path, canonical, Dos::kMaxPath);
        return true;
    }

    // --- console and files -------------------------------------------------

    static Dos::FileSlot make_file_slot(Dos::Stream stream, const DirEntry& entry,
                                          const char* path, bool command_owned = false,
                                          bool exclusive = false) {
        Dos::FileSlot s{};
        s.stream = stream;
        s.entry = entry;
        s.pos = 0;
        s.has_unget = false;
        s.unget = 0;
        s.has_last = false;
        s.last_char = 0;
        s.command_owned = command_owned;
        s.exclusive = exclusive;
        copy_cstr(s.path, path, Dos::kMaxPath);
        return s;
    }

    static bool refill_console(Dos& d) {
        if (d.input_pos_ < d.input_length_) {
            return true;
        }
        if (d.console_read_ == nullptr) {
            return false;
        }
        u32 n = d.console_read_(d.console_ctx_, reinterpret_cast<u8*>(d.input_),
                                Dos::kMaxArgs + 1);
        if (n > Dos::kMaxArgs + 1) {
            n = Dos::kMaxArgs + 1;
        }
        d.input_pos_ = 0;
        d.input_length_ = n;
        return n > 0;
    }

    static u32 available_bytes(const Dos& d, const Dos::FileSlot& f) {
        const u32 buffered = f.has_unget ? 1u : 0u;
        if (f.stream == Dos::Stream::Console) {
            const u32 left = d.input_length_ > d.input_pos_ ? d.input_length_ - d.input_pos_ : 0u;
            return buffered + left;
        }
        const u32 size = fs_size(d, f.entry);
        const u32 left = f.pos < size ? size - f.pos : 0u;
        return buffered + left;
    }

    // Reads up to length bytes from slot into to, consuming any pushed-back
    // byte first. False with IoErr set on a disk error.
    static bool read_from_slot(Dos& d, Bus& b, u32 slot, u8* to, u32 length, u32& got) {
        got = 0;
        if (length == 0) {
            return true;
        }
        Dos::FileSlot& f = d.files_[slot];
        if (f.has_unget) {
            to[0] = f.unget;
            f.has_unget = false;
            got = 1;
            if (length == 1) {
                f.last_char = to[0];
                f.has_last = true;
                return true;
            }
        }
        const u32 rem = length - got;
        if (f.stream == Dos::Stream::Console) {
            if (d.input_pos_ >= d.input_length_ && got == 0) {
                refill_console(d);
            }
            const u32 left = d.input_length_ > d.input_pos_ ? d.input_length_ - d.input_pos_ : 0u;
            const u32 n = rem < left ? rem : left;
            for (u32 i = 0; i < n; ++i) {
                to[got + i] = static_cast<u8>(d.input_[d.input_pos_ + i]);
            }
            d.input_pos_ += n;
            got += n;
        } else {
            const u32 size = fs_size(d, f.entry);
            const u32 left = f.pos < size ? size - f.pos : 0u;
            const u32 n = rem < left ? rem : left;
            if (n != 0) {
                u32 n_got = 0;
                const u32 err = fs_read(d, f.entry, f.pos, to + got, n, n_got);
                if (err != 0) {
                    set_io_err(d, b, f.entry.volume == Dos::kVolRam ? err : ERROR_NOT_A_DOS_DISK);
                    return false;
                }
                f.pos += n_got;
                got += n_got;
            }
        }
        if (got != 0) {
            f.last_char = to[got - 1];
            f.has_last = true;
        } else {
            f.has_last = false;
        }
        return true;
    }

    static void open(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 mode = m.cpu.d[2];
        char name[kMaxName];
        if (!read_name(d, b, m.cpu.d[1], name)) {
            native::set_result(m, 0);
            return;
        }
        const i64 slot = free_handle(d);
        if (is_console(name)) {
            if (slot < 0) {
                table_full(d, b, "file handle", Dos::kMaxFiles);
                return;
            }
            d.files_[slot] = make_file_slot(Dos::Stream::Console, DirEntry{}, "", d.running_);
            init_handle(d, b, static_cast<u32>(slot), true);
            native::set_result(m, handle_bptr(d, static_cast<u32>(slot)));
            return;
        }
        if (mode != MODE_OLDFILE && mode != MODE_NEWFILE && mode != MODE_READWRITE) {
            fail(d, b, ERROR_ACTION_NOT_KNOWN, 0);
            return;
        }
        u8 vol = Dos::kVolAdf;
        char resolved[Dos::kMaxPath];
        u32 error = 0;
        if (!resolve(d, b, name, vol, resolved, error)) {
            if (b.ok()) {
                fail(d, b, error, 0);
            }
            return;
        }
        if (vol != Dos::kVolRam) {
            if (mode == MODE_NEWFILE || mode == MODE_READWRITE) {
                fail(d, b, ERROR_DISK_WRITE_PROTECTED, 0);
                return;
            }
            DirEntry entry{};
            const u32 err = fs_find(d, vol, resolved, entry);
            if (err != 0) {
                fail(d, b, err, 0);
                return;
            }
            if (entry.is_dir) {
                fail(d, b, ERROR_OBJECT_WRONG_TYPE, 0);
                return;
            }
            if (slot < 0) {
                table_full(d, b, "file handle", Dos::kMaxFiles);
                return;
            }
            char canonical[Dos::kMaxPath];
            fs_canonical(d, vol, resolved, canonical);
            d.files_[slot] = make_file_slot(Dos::Stream::File, entry, canonical, d.running_);
            init_handle(d, b, static_cast<u32>(slot), false);
            native::set_result(m, handle_bptr(d, static_cast<u32>(slot)));
            return;
        }

        u32 node = RamFs::kNone;
        bool exclusive = false;
        auto create_ram_leaf = [&]() -> bool {
            if (resolved[0] == '\0') {
                fail(d, b, ERROR_OBJECT_WRONG_TYPE, 0);
                return false;
            }
            char parent[Dos::kMaxPath];
            const char* leaf = nullptr;
            split_parent_and_leaf(resolved, parent, leaf);
            u32 parent_node = RamFs::kNone;
            const u32 p_err = d.ram_.find_path(parent, parent_node);
            if (p_err != 0) {
                fail(d, b, p_err == ERROR_OBJECT_NOT_FOUND ? ERROR_DIR_NOT_FOUND : p_err, 0);
                return false;
            }
            if (!d.ram_.node(parent_node).is_dir) {
                fail(d, b, ERROR_DIR_NOT_FOUND, 0);
                return false;
            }
            if (slot < 0) {
                table_full(d, b, "file handle", Dos::kMaxFiles);
                return false;
            }
            const u32 c_err = d.ram_.create_file(parent_node, leaf, node);
            if (c_err != 0) {
                fail(d, b, c_err, 0);
                return false;
            }
            return true;
        };

        if (mode == MODE_NEWFILE) {
            if (!create_ram_leaf()) {
                return;
            }
            exclusive = true;
        } else {
            const u32 err = d.ram_.find_path(resolved, node);
            if (err == 0) {
                if (d.ram_.node(node).is_dir) {
                    fail(d, b, ERROR_OBJECT_WRONG_TYPE, 0);
                    return;
                }
                if (ram_node_busy(d, node, false)) {
                    fail(d, b, ERROR_OBJECT_IN_USE, 0);
                    return;
                }
                if (slot < 0) {
                    table_full(d, b, "file handle", Dos::kMaxFiles);
                    return;
                }
            } else if (mode == MODE_READWRITE && err == ERROR_OBJECT_NOT_FOUND) {
                if (!create_ram_leaf()) {
                    return;
                }
            } else {
                fail(d, b, err, 0);
                return;
            }
        }
        const DirEntry entry = ram_entry(d, node);
        char canonical[Dos::kMaxPath];
        ram_node_path(d.ram_, node, canonical);
        d.files_[slot] = make_file_slot(Dos::Stream::File, entry, canonical, d.running_, exclusive);
        retain_entry(d, entry);
        init_handle(d, b, static_cast<u32>(slot), false);
        native::set_result(m, handle_bptr(d, static_cast<u32>(slot)));
    }

    // Close(0) does nothing; the console streams stay open whatever is
    // closed.
    static void close(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        const u32 fh = m.cpu.d[1];
        if (fh == 0) {
            native::set_result(m, DOSTRUE);
            return;
        }
        const i64 slot = handle_slot(d, fh);
        if (slot < 0) {
            LogLine().text("dos: Close of a bad file handle ").hex(fh).send();
            native::set_result(m, DOSFALSE);
            return;
        }
        if (slot > kConsoleOut) {
            release_entry(d, d.files_[slot].entry);
            d.files_[slot] = make_file_slot(Dos::Stream::Closed, DirEntry{}, "");
        }
        native::set_result(m, DOSTRUE);
    }

    static void read(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 fh = m.cpu.d[1];
        const u32 buffer = m.cpu.d[2];
        const u32 length = m.cpu.d[3];
        const i64 slot = handle_slot(d, fh);
        if (slot < 0) {
            log_bad_handle("Read from", fh);
            fail(d, b, ERROR_INVALID_LOCK, 0xFFFFFFFF);
            return;
        }
        if ((length & 0x80000000) != 0) {
            fail(d, b, ERROR_BAD_NUMBER, 0xFFFFFFFF);
            return;
        }
        if (length != 0 && d.files_[slot].stream == Dos::Stream::Console &&
            available_bytes(d, d.files_[slot]) == 0) {
            refill_console(d);
        }
        const u32 avail = available_bytes(d, d.files_[slot]);
        const u32 n = length < avail ? length : avail;
        if (n == 0) {
            d.files_[slot].has_last = false;
            native::set_result(m, 0);
            return;
        }
        u8* to = b.bytes_at(buffer, n);
        if (to == nullptr) {
            return;
        }
        u32 got = 0;
        if (!read_from_slot(d, b, static_cast<u32>(slot), to, n, got)) {
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        native::set_result(m, got);
    }

    static void write_to_fh(Dos& d, Bus& b, u32 fh, u32 buffer, u32 length, const char* what) {
        const i64 slot = handle_slot(d, fh);
        if (slot < 0) {
            log_bad_handle(what, fh);
            fail(d, b, ERROR_INVALID_LOCK, 0xFFFFFFFF);
            return;
        }
        if ((length & 0x80000000) != 0) {
            fail(d, b, ERROR_BAD_NUMBER, 0xFFFFFFFF);
            return;
        }
        const u8* from = length != 0 ? b.bytes_at(buffer, length) : nullptr;
        if (length != 0 && from == nullptr) {
            return;
        }
        const bool ok = write_slot(d, b, static_cast<u32>(slot), from, length);
        native::set_result(b.machine(), ok ? length : 0xFFFFFFFF);
    }

    static void write(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        write_to_fh(d, b, m.cpu.d[1], m.cpu.d[2], m.cpu.d[3], "Write to");
    }

    static void write_chars(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 fh = b.r32(d.process_ + pr_COS);
        if (b.ok()) {
            write_to_fh(d, b, fh, m.cpu.d[1], m.cpu.d[2], "WriteChars to");
        }
    }

    // Seek returns the old position; a position outside the file is an
    // error that leaves it where it was.
    static void seek(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 fh = m.cpu.d[1];
        const i64 slot = handle_slot(d, fh);
        if (slot < 0) {
            log_bad_handle("Seek on", fh);
            fail(d, b, ERROR_INVALID_LOCK, 0xFFFFFFFF);
            return;
        }
        Dos::FileSlot& f = d.files_[slot];
        if (f.stream == Dos::Stream::Console) {
            fail(d, b, ERROR_ACTION_NOT_KNOWN, 0xFFFFFFFF);
            return;
        }
        const u32 size = fs_size(d, f.entry);
        const u32 mode = m.cpu.d[3];
        i64 from = 0;
        if (mode == OFFSET_BEGINNING) {
            from = 0;
        } else if (mode == OFFSET_CURRENT) {
            from = f.pos;
        } else if (mode == OFFSET_END) {
            from = size;
        } else {
            fail(d, b, ERROR_SEEK_ERROR, 0xFFFFFFFF);
            return;
        }
        const i64 to = from + static_cast<i64>(static_cast<__INT32_TYPE__>(m.cpu.d[2]));
        if (to < 0 || to > static_cast<i64>(size)) {
            fail(d, b, ERROR_SEEK_ERROR, 0xFFFFFFFF);
            return;
        }
        const u32 old = f.pos;
        f.pos = static_cast<u32>(to);
        f.has_unget = false;
        f.has_last = false;
        native::set_result(m, old);
    }

    static void set_file_size(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 fh = m.cpu.d[1];
        const i64 slot = handle_slot(d, fh);
        if (slot < 0) {
            log_bad_handle("SetFileSize on", fh);
            fail(d, b, ERROR_INVALID_LOCK, 0xFFFFFFFF);
            return;
        }
        Dos::FileSlot& f = d.files_[slot];
        if (f.stream == Dos::Stream::Console) {
            fail(d, b, ERROR_ACTION_NOT_KNOWN, 0xFFFFFFFF);
            return;
        }
        if (f.entry.volume != Dos::kVolRam) {
            fail(d, b, ERROR_DISK_WRITE_PROTECTED, 0xFFFFFFFF);
            return;
        }
        if (f.entry.header_block == 0) {
            fail(d, b, ERROR_OBJECT_NOT_FOUND, 0xFFFFFFFF);
            return;
        }
        const u32 node = f.entry.header_block - 1;
        const u32 cur_size = fs_size(d, f.entry);
        const u32 mode = m.cpu.d[3];
        i64 from = 0;
        if (mode == OFFSET_BEGINNING) {
            from = 0;
        } else if (mode == OFFSET_CURRENT) {
            from = f.pos;
        } else if (mode == OFFSET_END) {
            from = cur_size;
        } else {
            fail(d, b, ERROR_SEEK_ERROR, 0xFFFFFFFF);
            return;
        }
        const i64 new_size = from + static_cast<i64>(static_cast<__INT32_TYPE__>(m.cpu.d[2]));
        if (new_size < 0 || new_size > 0xFFFFFFFFll) {
            fail(d, b, ERROR_SEEK_ERROR, 0xFFFFFFFF);
            return;
        }
        const u32 target = static_cast<u32>(new_size);
        const u32 err = d.ram_.set_size(node, target);
        if (err != 0) {
            fail(d, b, err, 0xFFFFFFFF);
            return;
        }
        f.entry.size = target;
        if (f.pos > target) {
            f.pos = target;
            f.has_unget = false;
            f.has_last = false;
        }
        native::set_result(m, target);
    }

    static void input(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 fh = b.r32(d.process_ + pr_CIS);
        native::set_result(m, fh);
    }

    static void output(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 fh = b.r32(d.process_ + pr_COS);
        native::set_result(m, fh);
    }

    static void select(Machine& m, Dos& d, u32 field) {
        Bus b(m);
        const u32 old = b.r32(d.process_ + field);
        b.w32(d.process_ + field, m.cpu.d[1]);
        native::set_result(m, old);
    }

    static void select_input(void* ctx, Machine& m) { select(m, of(ctx), pr_CIS); }
    static void select_output(void* ctx, Machine& m) { select(m, of(ctx), pr_COS); }

    // The slot of the handle in D1 for an output call, or -1 with IoErr set
    // and the bad handle logged.
    static i64 output_slot(Dos& d, Bus& b, u32 fh, const char* what) {
        const i64 slot = handle_slot(d, fh);
        if (slot < 0) {
            log_bad_handle(what, fh);
            set_io_err(d, b, ERROR_INVALID_LOCK);
        }
        return slot;
    }

    // PutStr returns 0 or the IoErr code.
    static void put_str(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 fh = b.r32(d.process_ + pr_COS);
        if (!b.ok()) {
            return;
        }
        const i64 slot = output_slot(d, b, fh, "PutStr to");
        if (slot < 0) {
            native::set_result(m, ERROR_INVALID_LOCK);
            return;
        }
        Sink out(d, b, static_cast<u32>(slot));
        const bool ok = put_guest_string(b, out, m.cpu.d[1]) && out.flush();
        if (b.ok()) {
            native::set_result(m, ok ? 0 : b.r32(d.process_ + pr_Result2));
        }
    }

    static void fput_c(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const i64 slot = output_slot(d, b, m.cpu.d[1], "FPutC to");
        const u8 c = static_cast<u8>(m.cpu.d[2]);
        if (slot < 0 || !write_slot(d, b, static_cast<u32>(slot), &c, 1)) {
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        native::set_result(m, c);
    }

    static void fputs(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const i64 slot = output_slot(d, b, m.cpu.d[1], "FPuts to");
        if (slot < 0) {
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        Sink out(d, b, static_cast<u32>(slot));
        const bool ok = put_guest_string(b, out, m.cpu.d[2]) && out.flush();
        native::set_result(m, ok ? 0 : 0xFFFFFFFF);
    }

    // Formats as RawDoFmt does and returns the count of bytes written, -1
    // if the stream failed.
    static void printf_to(Machine& m, Dos& d, u32 fh, u32 format, u32 args, const char* what) {
        Bus b(m);
        const i64 slot = output_slot(d, b, fh, what);
        if (slot < 0) {
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        Sink out(d, b, static_cast<u32>(slot));
        u32 data = args;
        const bool ok = native::raw_format(b, format, data, out);
        out.flush();
        if (!b.ok()) {
            return;
        }
        native::set_result(m, ok && !out.failed ? out.total : 0xFFFFFFFF);
    }

    static void vfprintf(void* ctx, Machine& m) {
        printf_to(m, of(ctx), m.cpu.d[1], m.cpu.d[2], m.cpu.d[3], "VFPrintf to");
    }

    static void vprintf(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 fh = b.r32(d.process_ + pr_COS);
        if (b.ok()) {
            printf_to(m, d, fh, m.cpu.d[1], m.cpu.d[2], "VPrintf to");
        }
    }

    static void flush(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        if (handle_slot(d, m.cpu.d[1]) < 0) {
            fail(d, b, ERROR_INVALID_LOCK, DOSFALSE);
            return;
        }
        native::set_result(m, DOSTRUE);
    }

    // The console takes raw and cooked mode alike; a file has no mode.
    static void set_mode(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const i64 slot = output_slot(d, b, m.cpu.d[1], "SetMode on");
        if (slot < 0) {
            native::set_result(m, DOSFALSE);
        } else if (d.files_[slot].stream != Dos::Stream::Console) {
            fail(d, b, ERROR_ACTION_NOT_KNOWN, DOSFALSE);
        } else {
            native::set_result(m, DOSTRUE);
        }
    }

    static void fget_c(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 fh = m.cpu.d[1];
        const i64 slot = handle_slot(d, fh);
        if (slot < 0) {
            log_bad_handle("FGetC from", fh);
            fail(d, b, ERROR_INVALID_LOCK, 0xFFFFFFFF);
            return;
        }
        u8 c = 0;
        u32 got = 0;
        if (!read_from_slot(d, b, static_cast<u32>(slot), &c, 1, got)) {
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        if (got == 0) {
            set_io_err(d, b, 0);
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        native::set_result(m, c);
    }

    static void unget_c(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 fh = m.cpu.d[1];
        const u32 ch = m.cpu.d[2];
        const i64 slot = handle_slot(d, fh);
        if (slot < 0) {
            log_bad_handle("UnGetC on", fh);
            fail(d, b, ERROR_INVALID_LOCK, DOSFALSE);
            return;
        }
        Dos::FileSlot& f = d.files_[slot];
        if (f.has_unget) {
            native::set_result(m, DOSFALSE);
            return;
        }
        if (ch == 0xFFFFFFFF) {
            if (!f.has_last) {
                native::set_result(m, DOSFALSE);
                return;
            }
            f.unget = f.last_char;
        } else {
            f.unget = static_cast<u8>(ch);
        }
        f.has_unget = true;
        f.has_last = false;
        native::set_result(m, DOSTRUE);
    }

    static void fgets(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 fh = m.cpu.d[1];
        const u32 buf = m.cpu.d[2];
        const u32 len = m.cpu.d[3];
        const i64 slot = handle_slot(d, fh);
        if (slot < 0) {
            log_bad_handle("FGets from", fh);
            fail(d, b, ERROR_INVALID_LOCK, 0);
            return;
        }
        if (buf == 0 || len == 0 || (len & 0x80000000) != 0) {
            fail(d, b, ERROR_BAD_NUMBER, 0);
            return;
        }
        u32 i = 0;
        while (i < len - 1) {
            u8 c = 0;
            u32 got = 0;
            if (!read_from_slot(d, b, static_cast<u32>(slot), &c, 1, got)) {
                native::set_result(m, 0);
                return;
            }
            if (got == 0) {
                if (i == 0) {
                    set_io_err(d, b, 0);
                    native::set_result(m, 0);
                    return;
                }
                break;
            }
            b.w8(buf + i, c);
            if (!b.ok()) {
                return;
            }
            ++i;
            if (c == '\n') {
                break;
            }
        }
        b.w8(buf + i, 0);
        if (b.ok()) {
            native::set_result(m, buf);
        }
    }

    static void fread(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 fh = m.cpu.d[1];
        const u32 buf = m.cpu.d[2];
        const u32 blocklen = m.cpu.d[3];
        const u32 blocks = m.cpu.d[4];
        const i64 slot = handle_slot(d, fh);
        if (slot < 0) {
            log_bad_handle("FRead from", fh);
            fail(d, b, ERROR_INVALID_LOCK, 0);
            return;
        }
        if ((blocklen & 0x80000000) != 0 || (blocks & 0x80000000) != 0) {
            fail(d, b, ERROR_BAD_NUMBER, 0);
            return;
        }
        if (blocklen == 0 || blocks == 0) {
            native::set_result(m, 0);
            return;
        }
        if (d.files_[slot].stream == Dos::Stream::Console &&
            available_bytes(d, d.files_[slot]) == 0) {
            refill_console(d);
        }
        const u64 want = static_cast<u64>(blocklen) * blocks;
        const u32 avail = available_bytes(d, d.files_[slot]);
        const u32 n = want < avail ? static_cast<u32>(want) : avail;
        if (n == 0) {
            d.files_[slot].has_last = false;
            set_io_err(d, b, 0);
            native::set_result(m, 0);
            return;
        }
        u8* to = b.bytes_at(buf, n);
        if (to == nullptr) {
            return;
        }
        u32 got = 0;
        if (!read_from_slot(d, b, static_cast<u32>(slot), to, n, got)) {
            native::set_result(m, 0);
            return;
        }
        native::set_result(m, got / blocklen);
    }

    static void fwrite(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 fh = m.cpu.d[1];
        const u32 buf = m.cpu.d[2];
        const u32 blocklen = m.cpu.d[3];
        const u32 blocks = m.cpu.d[4];
        const i64 slot = handle_slot(d, fh);
        if (slot < 0) {
            log_bad_handle("FWrite to", fh);
            fail(d, b, ERROR_INVALID_LOCK, 0);
            return;
        }
        if ((blocklen & 0x80000000) != 0 || (blocks & 0x80000000) != 0) {
            fail(d, b, ERROR_BAD_NUMBER, 0);
            return;
        }
        if (blocklen == 0 || blocks == 0) {
            native::set_result(m, 0);
            return;
        }
        const u64 total = static_cast<u64>(blocklen) * blocks;
        const u32 bytes = total > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<u32>(total);
        const u8* from = b.bytes_at(buf, bytes);
        if (from == nullptr) {
            return;
        }
        const bool ok = write_slot(d, b, static_cast<u32>(slot), from, bytes);
        native::set_result(m, ok ? blocks : 0);
    }

    // BCPL-style formatting (%S, %T, %N, %I, %U, %X, %O, %C, %$, %%) from
    // an array of 32-bit values.
    static void vfwritef(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const i64 slot = output_slot(d, b, m.cpu.d[1], "VFWritef to");
        if (slot < 0) {
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        u32 fmt = m.cpu.d[2];
        u32 argv = m.cpu.d[3];
        Sink sink(d, b, static_cast<u32>(slot));
        native::format_detail::Capped<Sink> out{sink, native::format_detail::kMaxOutput};
        for (;;) {
            u32 c = b.r8(fmt++);
            if (!b.ok()) {
                sink.flush();
                return;
            }
            if (c == 0) {
                break;
            }
            if (c != '%') {
                if (!out.put(c)) {
                    sink.flush();
                    native::set_result(m, 0xFFFFFFFF);
                    return;
                }
                continue;
            }
            u32 width = 0;
            c = b.r8(fmt++);
            while (c >= '0' && c <= '9' && b.ok()) {
                if (width < native::format_detail::kMaxWidth) {
                    width = width * 10 + (c - '0');
                }
                c = b.r8(fmt++);
            }
            if (!b.ok()) {
                sink.flush();
                return;
            }
            bool ok = true;
            const char uc = upper(static_cast<char>(c));
            if (uc == '\0') {
                --fmt;
            } else if (uc == '%') {
                ok = out.put('%');
            } else if (uc == '$') {
                argv += 4;
            } else if (uc == 'C') {
                const u32 v = b.r32(argv);
                argv += 4;
                if (!b.ok()) {
                    sink.flush();
                    return;
                }
                const u32 pad = width > 1 ? width - 1 : 0;
                ok = out.put(v & 0xFF) && native::format_detail::repeat(out, ' ', pad);
            } else if (uc == 'S' || uc == 'T') {
                const u32 ptr = b.r32(argv);
                argv += 4;
                if (!b.ok()) {
                    sink.flush();
                    return;
                }
                u32 len = 0;
                if (ptr != 0) {
                    for (;; ++len) {
                        const u32 ch = b.r8(ptr + len);
                        if (!b.ok()) {
                            sink.flush();
                            return;
                        }
                        if (ch == 0) {
                            break;
                        }
                        if (!out.put(ch)) {
                            ok = false;
                            break;
                        }
                    }
                }
                if (ok && width > len) {
                    ok = native::format_detail::repeat(out, ' ', width - len);
                }
            } else if (uc == 'N' || uc == 'I' || uc == 'U' || uc == 'X') {
                u32 v = b.r32(argv);
                argv += 4;
                if (!b.ok()) {
                    sink.flush();
                    return;
                }
                bool negative = false;
                if ((uc == 'N' || uc == 'I') && (v & 0x80000000) != 0) {
                    negative = true;
                    v = 0u - v;
                }
                char digits[12];
                format_u64(digits, sizeof digits, v, uc == 'X' ? 16 : 10);
                if (uc == 'X') {
                    for (char* p = digits; *p != '\0'; ++p) {
                        *p = *p >= 'a' && *p <= 'f' ? static_cast<char>(*p - 'a' + 'A') : *p;
                    }
                }
                const native::format_detail::Field f{width, false, uc == 'X'};
                ok = native::format_detail::put_number(out, f, negative, digits);
            } else if (uc == 'O') {
                u32 v = b.r32(argv);
                argv += 4;
                if (!b.ok()) {
                    sink.flush();
                    return;
                }
                char buf[12];
                u32 n = 0;
                do {
                    buf[n++] = static_cast<char>('0' + (v & 7));
                    v >>= 3;
                } while (v != 0);
                char digits[12];
                for (u32 i = 0; i < n; ++i) {
                    digits[i] = buf[n - 1 - i];
                }
                digits[n] = '\0';
                const native::format_detail::Field f{width, false, false};
                ok = native::format_detail::put_number(out, f, false, digits);
            } else {
                ok = out.put(c);
            }
            if (!ok) {
                sink.flush();
                native::set_result(m, 0xFFFFFFFF);
                return;
            }
        }
        sink.flush();
        if (b.ok()) {
            native::set_result(m, sink.failed ? 0xFFFFFFFF : sink.total);
        }
    }

    static void is_interactive(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const i64 slot = handle_slot(d, m.cpu.d[1]);
        if (slot < 0) {
            fail(d, b, ERROR_INVALID_LOCK, DOSFALSE);
            return;
        }
        native::set_result(m, d.files_[slot].stream == Dos::Stream::Console ? DOSTRUE : DOSFALSE);
    }

    static void set_vbuf(void*, Machine& m) { native::set_result(m, 0); }

    // --- errors ------------------------------------------------------------

    static void io_err(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 code = b.r32(d.process_ + pr_Result2);
        native::set_result(m, code);
    }

    static void set_io_err_lvo(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 old = b.r32(d.process_ + pr_Result2);
        set_io_err(d, b, m.cpu.d[1]);
        native::set_result(m, old);
    }

    // "header: message", or the message alone, cut to capacity - 1
    // characters, into text. Returns its length; false in ok if the header
    // faulted.
    static u32 fault_text(Bus& b, u32 code, u32 header, char (&text)[kFaultText]) {
        u32 n = 0;
        auto add = [&](char c) {
            if (n + 1 < kFaultText) {
                text[n++] = c;
            }
        };
        if (header != 0) {
            for (u32 i = 0; n + 1 < kFaultText; ++i) {
                const u32 c = b.r8(header + i);
                if (!b.ok() || c == 0) {
                    break;
                }
                add(static_cast<char>(c));
            }
            add(':');
            add(' ');
        }
        n += copy_fault_message(code, text + n, kFaultText - n);
        text[n] = '\0';
        return n;
    }

    // Fault(code, header, buffer, length) puts at most length - 1
    // characters and a NUL into the buffer and returns their count; code 0
    // gives an empty string.
    static void fault(void*, Machine& m) {
        Bus b(m);
        const u32 code = m.cpu.d[1];
        const u32 header = m.cpu.d[2];
        const u32 buffer = m.cpu.d[3];
        const u32 length = m.cpu.d[4];
        if (length == 0 || (length & 0x80000000) != 0) {
            native::set_result(m, 0);
            return;
        }
        char text[kFaultText];
        u32 n = 0;
        if (code != 0) {
            n = fault_text(b, code, header, text);
        }
        if (!b.ok()) {
            return;
        }
        n = n < length - 1 ? n : length - 1;
        for (u32 i = 0; i < n; ++i) {
            b.w8(buffer + i, static_cast<u8>(text[i]));
        }
        b.w8(buffer + n, 0);
        native::set_result(m, n);
    }

    // PrintFault prints to Output() and sets IoErr to the code; code 0
    // prints nothing.
    static void print_fault(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 code = m.cpu.d[1];
        bool ok = true;
        if (code != 0) {
            char text[kFaultText];
            fault_text(b, code, m.cpu.d[2], text);
            const u32 fh = b.r32(d.process_ + pr_COS);
            if (!b.ok()) {
                return;
            }
            const i64 slot = output_slot(d, b, fh, "PrintFault to");
            if (slot < 0) {
                ok = false;
            } else {
                Sink out(d, b, static_cast<u32>(slot));
                ok = out.text(text) && out.put('\n') && out.flush();
            }
        }
        set_io_err(d, b, code);
        native::set_result(m, ok ? DOSTRUE : DOSFALSE);
    }

    static void error_report(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 code = m.cpu.d[1];
        if (code != 0) {
            char text[kFaultText];
            fault_text(b, code, 0, text);
            const u32 fh = b.r32(d.process_ + pr_COS);
            if (!b.ok()) {
                return;
            }
            const i64 slot = handle_slot(d, fh);
            const u32 out_slot = slot >= 0 && d.files_[slot].stream == Dos::Stream::Console
                                     ? static_cast<u32>(slot)
                                     : kConsoleOut;
            Sink out(d, b, out_slot);
            out.text(text);
            out.put('\n');
            out.flush();
        }
        if (b.ok()) {
            native::set_result(m, DOSTRUE);
        }
    }

    // --- variables -------------------------------------------------------------

    // The name of a variable, in D1, into name. False if it is null or too
    // long (IoErr and D0 are then set to code and result) or if reading it
    // faulted.
    static bool var_name(Dos& d, Bus& b, char (&name)[kMaxName], u32 result) {
        const u32 at = b.machine().cpu.d[1];
        if (at == 0) {
            fail(d, b, ERROR_REQUIRED_ARG_MISSING, result);
            return false;
        }
        if (!b.string(at, name, kMaxName)) {
            if (b.ok()) {
                fail(d, b, ERROR_INVALID_COMPONENT_NAME, result);
            }
            return false;
        }
        return true;
    }

    // The local variable of the type with the name, compared without regard
    // to case; 0 if there is none. A node marked LVF_IGNORE has another
    // type, so it never matches.
    static u32 find_var(Dos& d, Bus& b, const char* name, u32 type) {
        u32 node = b.r32(d.process_ + pr_LocalVars + LH_HEAD);
        for (u32 n = 0; n < kMaxVars; ++n) {
            const u32 succ = b.r32(node + LN_SUCC);
            if (!b.ok() || succ == 0) {
                return 0;
            }
            if (b.r8(node + LN_TYPE) == type) {
                char other[kMaxName];
                if (b.string(b.r32(node + LN_NAME), other, kMaxName) &&
                    same_name(other, length_of(other), name)) {
                    return node;
                }
                if (!b.ok()) {
                    return 0;
                }
            }
            node = succ;
        }
        LogLine().text("dos: the local variable list has no end").send();
        return 0;
    }

    // Frees what a variable's value takes.
    static void free_value(Dos& d, Bus& b, u32 node) {
        const u32 value = b.r32(node + lv_Value);
        const u32 length = b.r32(node + lv_Len);
        if (b.ok() && value != 0) {
            d.exec_->free(value, length);
        }
    }

    // Takes a variable off the list and frees it: the node, with its name
    // after it, and the value.
    static void delete_var(Dos& d, Bus& b, u32 node) {
        char name[kMaxName];
        if (!b.string(node + LocalVar_SIZEOF, name, kMaxName)) {
            return;
        }
        remove(b, node);
        free_value(d, b, node);
        if (b.ok()) {
            d.exec_->free(node, LocalVar_SIZEOF + length_of(name) + 1);
        }
    }

    // SetVar(name, buffer, size, flags): sets the local variable of the
    // type in the low byte of flags to size bytes from buffer (up to its
    // NUL for a size of -1), replacing one of the same name and type; a
    // null buffer deletes it. Local is the default; GVF_GLOBAL_ONLY fails.
    static void set_var(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 buffer = m.cpu.d[2];
        u32 size = m.cpu.d[3];
        const u32 flags = m.cpu.d[4];
        const u32 type = flags & 0xFF;
        char name[kMaxName];
        if (!var_name(d, b, name, DOSFALSE)) {
            return;
        }
        if ((flags & GVF_GLOBAL_ONLY) != 0) {
            fail(d, b, ERROR_DEVICE_NOT_MOUNTED, DOSFALSE);  // ENV:
            return;
        }
        u32 node = find_var(d, b, name, type);
        if (!b.ok()) {
            return;
        }
        if (buffer == 0) {
            if (node == 0) {
                fail(d, b, ERROR_OBJECT_NOT_FOUND, DOSFALSE);
                return;
            }
            delete_var(d, b, node);
            if (b.ok()) {
                native::set_result(m, DOSTRUE);
            }
            return;
        }
        if (size == 0xFFFFFFFF) {
            size = 0;
            while (b.r8(buffer + size) != 0 && b.ok()) {
                ++size;
            }
        }
        const u8* from = b.ok() && size != 0 ? b.bytes_at(buffer, size) : nullptr;
        if (!b.ok()) {
            return;
        }
        u32 value = 0;
        if (size != 0) {
            value = d.exec_->alloc(size, MEMF_PUBLIC);
            if (value == 0) {
                fail(d, b, ERROR_NO_FREE_STORE, DOSFALSE);
                return;
            }
            // exec handed the block out, so it is guest memory.
            for (u32 i = 0; i < size; ++i) {
                m.mem.base[value + i] = from[i];
            }
        }
        if (node == 0) {
            const u32 bytes = LocalVar_SIZEOF + length_of(name) + 1;
            node = d.exec_->alloc(bytes, MEMF_PUBLIC | MEMF_CLEAR);
            if (node == 0) {
                d.exec_->free(value, size);
                fail(d, b, ERROR_NO_FREE_STORE, DOSFALSE);
                return;
            }
            put_string(b, node + LocalVar_SIZEOF, name);
            b.w8(node + LN_TYPE, type);
            b.w32(node + LN_NAME, node + LocalVar_SIZEOF);
            add_tail(b, d.process_ + pr_LocalVars, node);
        } else {
            free_value(d, b, node);
        }
        b.w16(node + lv_Flags, flags & (GVF_BINARY_VAR | GVF_DONT_NULL_TERM));
        b.w32(node + lv_Value, value);
        b.w32(node + lv_Len, size);
        if (b.ok()) {
            native::set_result(m, DOSTRUE);
        }
    }

    // GetVar(name, buffer, size, flags): copies the local variable of the
    // type in the low byte of flags into buffer, up to its first newline
    // unless GVF_BINARY_VAR is set, and ends it with a NUL unless
    // GVF_DONT_NULL_TERM is set. D0 is the number of bytes copied, the NUL
    // left out, and IoErr the size of the whole variable. A variable that
    // does not exist gives -1 and an empty buffer; a null buffer or a size
    // that is not positive gives -1 and ERROR_BAD_NUMBER.
    static void get_var(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 buffer = m.cpu.d[2];
        const u32 size = m.cpu.d[3];
        const u32 flags = m.cpu.d[4];
        if (buffer == 0 || size == 0 || (size & 0x80000000) != 0) {
            fail(d, b, ERROR_BAD_NUMBER, 0xFFFFFFFF);
            return;
        }
        char name[kMaxName];
        const bool named = var_name(d, b, name, 0xFFFFFFFF);
        if (!b.ok()) {
            return;
        }
        const u32 node = named && (flags & GVF_GLOBAL_ONLY) == 0 ? find_var(d, b, name, flags & 0xFF)
                                                                 : 0;
        if (!b.ok()) {
            return;
        }
        if (node == 0) {
            b.w8(buffer, 0);
            if (named) {
                fail(d, b, ERROR_OBJECT_NOT_FOUND, 0xFFFFFFFF);
            }
            return;
        }
        const u32 value = b.r32(node + lv_Value);
        const u32 length = b.r32(node + lv_Len);
        const bool terminate = (flags & GVF_DONT_NULL_TERM) == 0;
        const u32 room = terminate ? size - 1 : size;
        u32 n = 0;
        for (; n < length && n < room; ++n) {
            const u32 c = b.r8(value + n);
            if (!b.ok() || ((flags & GVF_BINARY_VAR) == 0 && c == '\n')) {
                break;
            }
            b.w8(buffer + n, c);
        }
        if (terminate) {
            b.w8(buffer + n, 0);
        }
        set_io_err(d, b, length);
        if (b.ok()) {
            native::set_result(m, n);
        }
    }

    static void find_var_lvo(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        char name[kMaxName];
        if (!var_name(d, b, name, 0)) {
            return;
        }
        const u32 node = find_var(d, b, name, m.cpu.d[2]);
        if (!b.ok()) {
            return;
        }
        if (node == 0) {
            set_io_err(d, b, ERROR_OBJECT_NOT_FOUND);
        }
        if (b.ok()) {
            native::set_result(m, node);
        }
    }

    static void delete_var_lvo(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 flags = m.cpu.d[2];
        char name[kMaxName];
        if (!var_name(d, b, name, DOSFALSE)) {
            return;
        }
        if ((flags & GVF_GLOBAL_ONLY) != 0) {
            fail(d, b, ERROR_DEVICE_NOT_MOUNTED, DOSFALSE);
            return;
        }
        const u32 node = find_var(d, b, name, flags & 0xFF);
        if (!b.ok()) {
            return;
        }
        if (node == 0) {
            fail(d, b, ERROR_OBJECT_NOT_FOUND, DOSFALSE);
            return;
        }
        delete_var(d, b, node);
        if (b.ok()) {
            native::set_result(m, DOSTRUE);
        }
    }

    // StrToLong(string, value): the decimal number at the start of string,
    // after spaces and tabs and an optional sign, into the long at value.
    // D0 is the number of characters it took, the blanks included, or -1,
    // with 0 stored, if no digit follows them. The value wraps modulo 2^32.
    static void str_to_long(void*, Machine& m) {
        Bus b(m);
        const u32 s = m.cpu.d[1];
        const u32 out = m.cpu.d[2];
        u32 at = 0;
        u32 c = b.r8(s);
        while ((c == ' ' || c == '\t') && b.ok()) {
            c = b.r8(s + ++at);
        }
        const bool negative = c == '-';
        if (c == '-' || c == '+') {
            c = b.r8(s + ++at);
        }
        u32 value = 0;
        u32 digits = 0;
        while (c >= '0' && c <= '9' && b.ok()) {
            value = value * 10 + (c - '0');
            ++digits;
            c = b.r8(s + ++at);
        }
        if (!b.ok()) {
            return;
        }
        b.w32(out, digits == 0 ? 0 : negative ? 0u - value : value);
        if (b.ok()) {
            native::set_result(m, digits == 0 ? 0xFFFFFFFF : at);
        }
    }

    static void cli(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 bptr = b.r32(d.process_ + pr_CLI);
        native::set_result(m, bptr << 2);
    }

    static void get_file_sys_task(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 port = b.r32(d.process_ + pr_FileSystemTask);
        if (b.ok()) {
            native::set_result(m, port);
        }
    }

    static void set_file_sys_task(void* ctx, Machine& m) {
        select(m, of(ctx), pr_FileSystemTask);
    }

    static void max_cli(void*, Machine& m) { native::set_result(m, 1); }

    static void find_cli_proc(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        native::set_result(m, m.cpu.d[1] == 1 ? d.process_ : 0);
    }

    static void find_segment(void*, Machine& m) { native::set_result(m, 0); }

    static void get_arg_str(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 arg = b.r32(d.process_ + pr_Arguments);
        if (b.ok()) {
            native::set_result(m, arg);
        }
    }

    static void set_arg_str(void* ctx, Machine& m) { select(m, of(ctx), pr_Arguments); }

    // Reads a CLI BSTR field into a host buffer. False if out/size invalid,
    // no CLI exists, or the string does not fit.
    static bool read_cli_bstr_host(const Dos& d, Bus& b, u32 field, char* out, u32 size) {
        if (out == nullptr || size == 0) {
            return false;
        }
        out[0] = '\0';
        const u32 cli_bptr = b.r32(d.process_ + pr_CLI);
        if (!b.ok() || cli_bptr == 0) {
            return false;
        }
        const u32 bstr_bptr = b.r32((cli_bptr << 2) + field);
        if (!b.ok()) {
            return false;
        }
        if (bstr_bptr == 0) {
            return true;
        }
        const u32 bstr = bstr_bptr << 2;
        const u32 slen = b.r8(bstr);
        if (!b.ok()) {
            return false;
        }
        const u32 copy = slen < size - 1 ? slen : size - 1;
        for (u32 i = 0; i < copy; ++i) {
            out[i] = static_cast<char>(b.r8(bstr + 1 + i));
        }
        out[copy] = '\0';
        return b.ok() && slen + 1 <= size;
    }

    static void set_cli_bstr(Dos& d, Bus& b, u32 field, u32 dest, u32 str_addr) {
        if (str_addr == 0) {
            fail(d, b, ERROR_REQUIRED_ARG_MISSING, DOSFALSE);
            return;
        }
        char buf[kMaxName];
        if (!b.string(str_addr, buf, kMaxName)) {
            if (b.ok()) {
                fail(d, b, ERROR_LINE_TOO_LONG, DOSFALSE);
            }
            return;
        }
        const u32 cli_bptr = b.r32(d.process_ + pr_CLI);
        if (!b.ok()) {
            return;
        }
        if (cli_bptr == 0) {
            fail(d, b, ERROR_OBJECT_NOT_FOUND, DOSFALSE);
            return;
        }
        put_bstr(b, dest, buf, 255);
        b.w32((cli_bptr << 2) + field, dest >> 2);
        if (b.ok()) {
            native::set_result(b.machine(), DOSTRUE);
        }
    }

    static void get_cli_bstr(Dos& d, Bus& b, u32 field, u32 buf_addr, u32 len) {
        if (buf_addr == 0 || len == 0 || (len & 0x80000000) != 0) {
            fail(d, b, ERROR_LINE_TOO_LONG, DOSFALSE);
            return;
        }
        const u32 cli_bptr = b.r32(d.process_ + pr_CLI);
        if (!b.ok()) {
            return;
        }
        if (cli_bptr == 0) {
            fail(d, b, ERROR_OBJECT_NOT_FOUND, DOSFALSE);
            return;
        }
        const u32 bstr_bptr = b.r32((cli_bptr << 2) + field);
        if (!b.ok()) {
            return;
        }
        if (bstr_bptr == 0) {
            b.w8(buf_addr, 0);
            if (b.ok()) {
                native::set_result(b.machine(), DOSTRUE);
            }
            return;
        }
        const u32 bstr = bstr_bptr << 2;
        const u32 slen = b.r8(bstr);
        if (!b.ok()) {
            return;
        }
        if (slen + 1 > len) {
            fail(d, b, ERROR_LINE_TOO_LONG, DOSFALSE);
            return;
        }
        for (u32 i = 0; i < slen; ++i) {
            b.w8(buf_addr + i, b.r8(bstr + 1 + i));
        }
        b.w8(buf_addr + slen, 0);
        if (b.ok()) {
            native::set_result(b.machine(), DOSTRUE);
        }
    }

    static void set_current_dir_name(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        set_cli_bstr(d, b, cli_SetName, d.set_name_, m.cpu.d[1]);
    }

    static void get_current_dir_name(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        get_cli_bstr(d, b, cli_SetName, m.cpu.d[1], m.cpu.d[2]);
    }

    static void set_program_name(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        set_cli_bstr(d, b, cli_CommandName, d.command_name_, m.cpu.d[1]);
    }

    static void get_program_name(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        get_cli_bstr(d, b, cli_CommandName, m.cpu.d[1], m.cpu.d[2]);
    }

    static void set_prompt(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        set_cli_bstr(d, b, cli_Prompt, d.prompt_, m.cpu.d[1]);
    }

    static void get_prompt(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        get_cli_bstr(d, b, cli_Prompt, m.cpu.d[1], m.cpu.d[2]);
    }

    static void check_signal(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 mask = m.cpu.d[1];
        const u32 recvd = b.r32(d.process_ + TC_SIGRECVD);
        const u32 hit = recvd & mask;
        if (hit != 0) {
            b.w32(d.process_ + TC_SIGRECVD, recvd & ~hit);
        }
        if (b.ok()) {
            native::set_result(m, hit);
        }
    }

    static u32 dos_object_size(u32 type) {
        switch (type) {
        case DOS_FILEHANDLE: return fh_SIZEOF;
        case DOS_EXALLCONTROL: return eac_SIZEOF;
        case DOS_FIB: return fib_SIZEOF;
        case DOS_STDPKT: return sp_SIZEOF;
        case DOS_CLI: return cli_SIZEOF;
        case DOS_RDARGS: return RDA_SIZEOF;
        default: return 0;
        }
    }

    static void alloc_dos_object(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 type = m.cpu.d[1];
        const u32 bytes = dos_object_size(type);
        if (bytes == 0) {
            fail(d, b, ERROR_BAD_NUMBER, 0);
            return;
        }
        const u32 at = d.exec_->alloc(bytes, MEMF_PUBLIC | MEMF_CLEAR);
        if (at == 0) {
            fail(d, b, ERROR_NO_FREE_STORE, 0);
            return;
        }
        if (type == DOS_STDPKT) {
            b.w32(at + sp_Msg + LN_NAME, at + sp_Pkt);
            b.w16(at + sp_Msg + mn_Length, dp_SIZEOF);
            b.w32(at + sp_Pkt + dp_Link, at + sp_Msg);
        }
        if (b.ok()) {
            native::set_result(m, at);
        }
    }

    static void free_dos_object(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        const u32 ptr = m.cpu.d[2];
        if (ptr == 0) {
            return;
        }
        const u32 bytes = dos_object_size(m.cpu.d[1]);
        if (bytes == 0) {
            return;
        }
        d.exec_->free(native::guest_address(m, ptr), bytes);
    }

    // --- time ----------------------------------------------------------------

    static DosStamp current_stamp(const Dos& d) {
        if (d.now_ticks_ == nullptr) {
            return DosStamp{0u, 0u, 0u};
        }
        const u64 total = d.now_ticks_();
        constexpr u64 kTicksPerMin = detail::kTicksPerMinute;
        constexpr u64 kTicksPerDay = static_cast<u64>(detail::kMinutesPerDay) * kTicksPerMin;
        const u32 days = static_cast<u32>(total / kTicksPerDay);
        const u64 rem = total % kTicksPerDay;
        const u32 minutes = static_cast<u32>(rem / kTicksPerMin);
        const u32 ticks = static_cast<u32>(rem % kTicksPerMin);
        return DosStamp{days, minutes, ticks};
    }

    static void date_stamp(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 ds = m.cpu.d[1];
        const DosStamp s = current_stamp(d);
        b.w32(ds + ds_Days, s.days);
        b.w32(ds + ds_Minute, s.minutes);
        b.w32(ds + ds_Tick, s.ticks);
        native::set_result(m, ds);
    }

    // Returns < 0 if date1 is later than date2, 0 if equal, > 0 if earlier.
    static void compare_dates(void*, Machine& m) {
        Bus b(m);
        const u32 a1 = m.cpu.d[1];
        const u32 a2 = m.cpu.d[2];
        const i64 d1 = static_cast<i32>(b.r32(a1 + ds_Days));
        const i64 m1 = static_cast<i32>(b.r32(a1 + ds_Minute));
        const i64 t1 = static_cast<i32>(b.r32(a1 + ds_Tick));
        const i64 d2 = static_cast<i32>(b.r32(a2 + ds_Days));
        const i64 m2 = static_cast<i32>(b.r32(a2 + ds_Minute));
        const i64 t2 = static_cast<i32>(b.r32(a2 + ds_Tick));
        if (!b.ok()) {
            return;
        }
        i32 cmp = 0;
        if (d1 != d2) {
            cmp = d1 < d2 ? 1 : -1;
        } else if (m1 != m2) {
            cmp = m1 < m2 ? 1 : -1;
        } else if (t1 != t2) {
            cmp = t1 < t2 ? 1 : -1;
        }
        native::set_result(m, static_cast<u32>(cmp));
    }

    static void date_to_str(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 dt = m.cpu.d[1];
        if (dt == 0) {
            native::set_result(m, DOSFALSE);
            return;
        }
        const DosStamp stamp{
            b.r32(dt + dat_Stamp + ds_Days),
            b.r32(dt + dat_Stamp + ds_Minute),
            b.r32(dt + dat_Stamp + ds_Tick),
        };
        const u8 format = static_cast<u8>(b.r8(dt + dat_Format));
        const u8 flags = static_cast<u8>(b.r8(dt + dat_Flags));
        const u32 str_day = b.r32(dt + dat_StrDay);
        const u32 str_date = b.r32(dt + dat_StrDate);
        const u32 str_time = b.r32(dt + dat_StrTime);
        if (!b.ok()) {
            return;
        }
        char day_buf[LEN_DATSTRING]{};
        char date_buf[LEN_DATSTRING]{};
        char time_buf[LEN_DATSTRING]{};
        const u32 today = current_stamp(d).days;
        const u32 ok = dos_format_date(
            stamp, format, flags, today,
            str_date != 0 ? date_buf : nullptr, str_date != 0 ? LEN_DATSTRING : 0u,
            str_day != 0 ? day_buf : nullptr, str_day != 0 ? LEN_DATSTRING : 0u,
            str_time != 0 ? time_buf : nullptr, str_time != 0 ? LEN_DATSTRING : 0u);
        if (ok == 0u) {
            native::set_result(m, DOSFALSE);
            return;
        }
        if (str_day != 0) {
            put_string(b, str_day, day_buf);
        }
        if (str_date != 0) {
            put_string(b, str_date, date_buf);
        }
        if (str_time != 0) {
            put_string(b, str_time, time_buf);
        }
        if (b.ok()) {
            native::set_result(m, DOSTRUE);
        }
    }

    static void str_to_date(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 dt = m.cpu.d[1];
        if (dt == 0) {
            native::set_result(m, DOSFALSE);
            return;
        }
        DosStamp stamp{
            b.r32(dt + dat_Stamp + ds_Days),
            b.r32(dt + dat_Stamp + ds_Minute),
            b.r32(dt + dat_Stamp + ds_Tick),
        };
        const u8 format = static_cast<u8>(b.r8(dt + dat_Format));
        const u32 str_day = b.r32(dt + dat_StrDay);
        const u32 str_date = b.r32(dt + dat_StrDate);
        const u32 str_time = b.r32(dt + dat_StrTime);
        if (!b.ok()) {
            return;
        }
        char day_buf[LEN_DATSTRING]{};
        char date_buf[LEN_DATSTRING]{};
        char time_buf[LEN_DATSTRING]{};
        if (str_day != 0) {
            b.string(str_day, day_buf, LEN_DATSTRING);
            if (!b.ok()) {
                return;
            }
        }
        if (str_date != 0 && !b.string(str_date, date_buf, LEN_DATSTRING)) {
            if (b.ok()) {
                native::set_result(m, DOSFALSE);
            }
            return;
        }
        if (str_time != 0 && !b.string(str_time, time_buf, LEN_DATSTRING)) {
            if (b.ok()) {
                native::set_result(m, DOSFALSE);
            }
            return;
        }
        const u32 today = current_stamp(d).days;
        const u8 flags = static_cast<u8>(b.r8(dt + dat_Flags));
        if (!b.ok()) {
            return;
        }
        if (!dos_parse_date(str_date != 0 ? date_buf : nullptr,
                            str_time != 0 ? time_buf : nullptr, format, today, stamp, flags)) {
            native::set_result(m, DOSFALSE);
            return;
        }
        b.w32(dt + dat_Stamp + ds_Days, stamp.days);
        b.w32(dt + dat_Stamp + ds_Minute, stamp.minutes);
        b.w32(dt + dat_Stamp + ds_Tick, stamp.ticks);
        if (b.ok()) {
            native::set_result(m, DOSTRUE);
        }
    }

    // Nothing else runs, so a Delay is over at once.
    static void delay(void*, Machine& m) { native::set_result(m, 0); }

    // Reports whether the stream has a byte ready or becomes ready within the
    // timeout (microseconds).
    static void wait_for_char(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 fh = m.cpu.d[1];
        const u32 timeout = m.cpu.d[2];
        const i64 slot = handle_slot(d, fh);
        if (slot < 0) {
            fail(d, b, ERROR_INVALID_LOCK, DOSFALSE);
            return;
        }
        const Dos::FileSlot& f = d.files_[slot];
        if (f.stream == Dos::Stream::Console) {
            if (available_bytes(d, f) > 0) {
                native::set_result(m, DOSTRUE);
                return;
            }
            if (d.console_ready_ != nullptr && d.console_ready_(d.console_ctx_, timeout)) {
                native::set_result(m, DOSTRUE);
                return;
            }
        }
        native::set_result(m, DOSFALSE);
    }

    // --- the command -----------------------------------------------------------

    // Exit ends the command with D1 as its result. The run ends with Halt,
    // which every call into 68000 code passes on, and run_command takes it
    // as the command's end.
    static void exit(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        if (!d.running_) {
            LogLine().text("dos: Exit with no command running").send();
        }
        d.exited_ = d.running_;
        d.exit_code_ = m.cpu.d[1];
        if (m.cpu.exit == ExitReason::None) {
            m.cpu.d[0] = m.cpu.d[1];
        }
        end_run(m, ExitReason::Halt);
    }

    // --- locks -----------------------------------------------------------------

    static void init_lock_at(Dos& d, Bus& b, u32 at, u8 vol, u32 key, u32 access) {
        b.w32(at + fl_Link, 0);
        b.w32(at + fl_Key, key);
        b.w32(at + fl_Access, access == EXCLUSIVE_LOCK ? EXCLUSIVE_LOCK : SHARED_LOCK);
        b.w32(at + fl_Task, d.port_);
        b.w32(at + fl_Volume, volume_node(d, vol) >> 2);
    }

    static void init_lock(Dos& d, Bus& b, u32 slot, u8 vol, u32 key, u32 access) {
        init_lock_at(d, b, d.locks_ + slot * fl_SIZEOF, vol, key, access);
    }

    static i64 free_lock(const Dos& d) {
        for (u32 slot = 0; slot < Dos::kMaxLocks; ++slot) {
            if (!d.lock_slots_[slot].used) {
                return slot;
            }
        }
        return -1;
    }

    static bool alloc_user_lock(Dos& d, Bus& b, const char* path, const DirEntry& entry,
                                u32 access, u32& bptr_out, bool command_owned) {
        const bool exclusive = access == EXCLUSIVE_LOCK;
        if (entry.volume == Dos::kVolRam && entry.header_block > 0 &&
            ram_node_busy(d, entry.header_block - 1, exclusive)) {
            fail(d, b, ERROR_OBJECT_IN_USE, 0);
            return false;
        }
        const i64 slot = free_lock(d);
        if (slot < 0) {
            table_full(d, b, "lock", Dos::kMaxLocks);
            return false;
        }
        Dos::LockSlot& l = d.lock_slots_[slot];
        l.used = true;
        l.command_owned = command_owned;
        l.exclusive = exclusive;
        l.entry = entry;
        char canonical[Dos::kMaxPath];
        fs_canonical(d, entry.volume, path, canonical);
        copy_cstr(l.path, canonical, Dos::kMaxPath);
        retain_entry(d, entry);
        init_lock(d, b, static_cast<u32>(slot), entry.volume, l.entry.header_block, access);
        bptr_out = lock_bptr(d, static_cast<u32>(slot));
        return b.ok();
    }

    static bool alloc_user_lock(Dos& d, Bus& b, const char* path, const DirEntry& entry,
                                u32 access, u32& bptr_out) {
        return alloc_user_lock(d, b, path, entry, access, bptr_out, d.running_);
    }

    static bool free_user_lock(Dos& d, u32 bptr) {
        if (bptr == 0) {
            return true;
        }
        const i64 slot = lock_slot(d, bptr);
        if (slot < 0) {
            return false;
        }
        release_entry(d, d.lock_slots_[slot].entry);
        d.lock_slots_[slot].used = false;
        d.lock_slots_[slot].command_owned = false;
        d.lock_slots_[slot].exclusive = false;
        return true;
    }

    static void lock(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        char name[kMaxName];
        if (!read_name(d, b, m.cpu.d[1], name)) {
            native::set_result(m, 0);
            return;
        }
        u8 vol = Dos::kVolAdf;
        char path[Dos::kMaxPath];
        DirEntry entry{};
        u32 error = 0;
        if (!locate(d, b, name, vol, path, entry, error)) {
            if (b.ok()) {
                fail(d, b, error, 0);
            }
            return;
        }
        u32 bptr = 0;
        if (alloc_user_lock(d, b, path, entry, m.cpu.d[2], bptr)) {
            native::set_result(m, bptr);
        }
    }

    static void unlock(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        const u32 bptr = m.cpu.d[1];
        if (!free_user_lock(d, bptr)) {
            LogLine().text("dos: UnLock of a bad lock ").hex(bptr).send();
        }
    }

    static bool dup_lock_impl(Dos& d, Bus& b, u32 bptr) {
        if (bptr == 0) {
            native::set_result(b.machine(), 0);
            return true;
        }
        const Dos::LockSlot* from = find_lock_slot(d, bptr);
        if (from == nullptr) {
            fail(d, b, ERROR_INVALID_LOCK, 0);
            return false;
        }
        u32 copy_bptr = 0;
        if (alloc_user_lock(d, b, from->path, from->entry, SHARED_LOCK, copy_bptr)) {
            native::set_result(b.machine(), copy_bptr);
            return true;
        }
        return false;
    }

    static void dup_lock(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        dup_lock_impl(d, b, m.cpu.d[1]);
    }

    static void dup_lock_from_fh(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const i64 fslot = handle_slot(d, m.cpu.d[1]);
        if (fslot < 0) {
            fail(d, b, ERROR_INVALID_LOCK, 0);
            return;
        }
        const Dos::FileSlot& f = d.files_[fslot];
        if (f.stream != Dos::Stream::File) {
            fail(d, b, ERROR_ACTION_NOT_KNOWN, 0);
            return;
        }
        u32 bptr = 0;
        if (alloc_user_lock(d, b, f.path, f.entry, SHARED_LOCK, bptr)) {
            native::set_result(m, bptr);
        }
    }

    static void open_from_lock(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const i64 lslot = lock_slot(d, m.cpu.d[1]);
        if (lslot < 0) {
            fail(d, b, ERROR_INVALID_LOCK, 0);
            return;
        }
        Dos::LockSlot& l = d.lock_slots_[lslot];
        if (l.entry.is_dir) {
            fail(d, b, ERROR_OBJECT_WRONG_TYPE, 0);
            return;
        }
        const i64 fslot = free_handle(d);
        if (fslot < 0) {
            table_full(d, b, "file handle", Dos::kMaxFiles);
            return;
        }
        d.files_[fslot] = make_file_slot(Dos::Stream::File, l.entry, l.path, d.running_,
                                         l.exclusive);
        init_handle(d, b, static_cast<u32>(fslot), false);
        l.used = false;
        l.command_owned = false;
        l.exclusive = false;
        native::set_result(m, handle_bptr(d, static_cast<u32>(fslot)));
    }

    static void parent_path_of(const char* path, char (&out)[Dos::kMaxPath]) {
        u32 slash = 0;
        bool has_slash = false;
        for (u32 i = 0; path[i] != '\0'; ++i) {
            if (path[i] == '/') {
                slash = i;
                has_slash = true;
            }
        }
        if (has_slash) {
            for (u32 i = 0; i < slash && i + 1 < Dos::kMaxPath; ++i) {
                out[i] = path[i];
            }
            out[slash < Dos::kMaxPath ? slash : Dos::kMaxPath - 1] = '\0';
        } else {
            out[0] = '\0';
        }
    }

    static bool lock_parent_of_path(Dos& d, Bus& b, u8 vol, const char* path) {
        char parent[Dos::kMaxPath];
        parent_path_of(path, parent);
        DirEntry entry{};
        const u32 err = fs_find(d, vol, parent, entry);
        if (err != 0) {
            fail(d, b, err, 0);
            return false;
        }
        u32 bptr = 0;
        if (alloc_user_lock(d, b, parent, entry, SHARED_LOCK, bptr)) {
            native::set_result(b.machine(), bptr);
            return true;
        }
        return false;
    }

    static bool parent_dir_impl(Dos& d, Bus& b, u32 bptr) {
        if (bptr == 0) {
            set_io_err(d, b, 0);
            native::set_result(b.machine(), 0);
            return true;
        }
        const Dos::LockSlot* from = find_lock_slot(d, bptr);
        if (from == nullptr) {
            fail(d, b, ERROR_INVALID_LOCK, 0);
            return false;
        }
        if (from->path[0] == '\0') {
            set_io_err(d, b, 0);
            native::set_result(b.machine(), 0);
            return true;
        }
        return lock_parent_of_path(d, b, from->entry.volume, from->path);
    }

    static void parent_dir(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        parent_dir_impl(d, b, m.cpu.d[1]);
    }

    static void parent_of_fh(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const i64 fslot = handle_slot(d, m.cpu.d[1]);
        if (fslot < 0) {
            fail(d, b, ERROR_INVALID_LOCK, 0);
            return;
        }
        const Dos::FileSlot& f = d.files_[fslot];
        if (f.stream != Dos::Stream::File) {
            fail(d, b, ERROR_ACTION_NOT_KNOWN, 0);
            return;
        }
        if (f.path[0] == '\0') {
            set_io_err(d, b, 0);
            native::set_result(m, 0);
            return;
        }
        lock_parent_of_path(d, b, f.entry.volume, f.path);
    }

    static bool same_lock_impl(Dos& d, Bus& b, u32 lock1, u32 lock2) {
        DirEntry e1{};
        DirEntry e2{};
        const char* p1 = nullptr;
        const char* p2 = nullptr;
        if (!lock_entry(d, b, lock1, e1, p1) || !lock_entry(d, b, lock2, e2, p2)) {
            native::set_result(b.machine(), LOCK_DIFFERENT);
            return false;
        }
        u32 res = LOCK_DIFFERENT;
        if (e1.volume == e2.volume) {
            res = e1.header_block == e2.header_block ? LOCK_SAME : LOCK_SAME_VOLUME;
        }
        native::set_result(b.machine(), res);
        return true;
    }

    static void same_lock(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        same_lock_impl(d, b, m.cpu.d[1], m.cpu.d[2]);
    }

    static void write_volume_path(Dos& d, Bus& b, u8 vol, const char* path, u32 buffer, u32 len) {
        if (buffer == 0 || len == 0 || (len & 0x80000000) != 0) {
            fail(d, b, ERROR_LINE_TOO_LONG, DOSFALSE);
            return;
        }
        const char* vname = volume_name(d, vol);
        const u32 vol_len = length_of(vname);
        const u32 path_len = length_of(path);
        const u32 total = vol_len + 1 + path_len;
        if (total + 1 > len) {
            fail(d, b, ERROR_LINE_TOO_LONG, DOSFALSE);
            return;
        }
        u32 at = 0;
        for (u32 i = 0; i < vol_len; ++i) {
            b.w8(buffer + at++, static_cast<u8>(vname[i]));
        }
        b.w8(buffer + at++, ':');
        for (u32 i = 0; i < path_len; ++i) {
            b.w8(buffer + at++, static_cast<u8>(path[i]));
        }
        b.w8(buffer + at, 0);
        if (b.ok()) {
            native::set_result(b.machine(), DOSTRUE);
        }
    }

    static void name_from_lock(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        DirEntry entry{};
        const char* path = nullptr;
        if (!lock_entry(d, b, m.cpu.d[1], entry, path)) {
            native::set_result(m, DOSFALSE);
            return;
        }
        write_volume_path(d, b, entry.volume, path, m.cpu.d[2], m.cpu.d[3]);
    }

    static void name_from_fh(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const i64 slot = handle_slot(d, m.cpu.d[1]);
        if (slot < 0) {
            fail(d, b, ERROR_INVALID_LOCK, DOSFALSE);
            return;
        }
        const Dos::FileSlot& f = d.files_[slot];
        if (f.stream != Dos::Stream::File) {
            fail(d, b, ERROR_ACTION_NOT_KNOWN, DOSFALSE);
            return;
        }
        write_volume_path(d, b, f.entry.volume, f.path, m.cpu.d[2], m.cpu.d[3]);
    }

    static void current_dir(void* ctx, Machine& m) { select(m, of(ctx), pr_CurrentDir); }

    // The entry and path of a lock; 0 is the root. False with IoErr set if
    // it is not a lock.
    static bool lock_entry(Dos& d, Bus& b, u32 bptr, DirEntry& entry, const char*& path) {
        if (bptr == 0) {
            path = "";
            const u32 err = fs_find(d, Dos::kVolAdf, "", entry);
            if (err != 0) {
                set_io_err(d, b, err);
                return false;
            }
            return true;
        }
        const Dos::LockSlot* slot = find_lock_slot(d, bptr);
        if (slot == nullptr) {
            set_io_err(d, b, ERROR_INVALID_LOCK);
            return false;
        }
        entry = slot->entry;
        path = slot->path;
        return true;
    }

    // --- assigns and DosList -------------------------------------------------

    static bool read_assign_name(Dos& d, Bus& b, u32 addr, char (&name)[32]) {
        if (addr == 0) {
            fail(d, b, ERROR_REQUIRED_ARG_MISSING, DOSFALSE);
            return false;
        }
        char buf[kMaxName];
        if (!read_name(d, b, addr, buf)) {
            if (b.ok()) {
                native::set_result(b.machine(), DOSFALSE);
            }
            return false;
        }
        const u32 len = length_of(buf);
        if (len == 0 || len > kMaxComponent) {
            fail(d, b, ERROR_INVALID_COMPONENT_NAME, DOSFALSE);
            return false;
        }
        for (u32 i = 0; i < len; ++i) {
            if (buf[i] == ':' || buf[i] == '/') {
                fail(d, b, ERROR_INVALID_COMPONENT_NAME, DOSFALSE);
                return false;
            }
        }
        copy_cstr(name, buf, sizeof name);
        return true;
    }

    static void assign_lock(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        char name[32];
        if (!read_assign_name(d, b, m.cpu.d[1], name)) {
            return;
        }
        const u32 lock_bptr = m.cpu.d[2];
        const u32 name_len = length_of(name);
        const i64 existing = find_assign(d, name, name_len);
        if (lock_bptr == 0) {
            if (existing >= 0) {
                Dos::AssignEntry& e = d.assigns_[existing];
                for (u32 i = 0; i < e.dir_count; ++i) {
                    release_lock(d, e.locks[i]);
                }
                if (e.node != 0) {
                    d.exec_->free(e.node, e.node_bytes);
                }
                e = Dos::AssignEntry{};
                relink_dos_list(d, b);
            }
            if (b.ok()) {
                native::set_result(m, DOSTRUE);
            }
            return;
        }
        if (is_volume_or_device(d, name, name_len)) {
            fail(d, b, ERROR_OBJECT_EXISTS, DOSFALSE);
            return;
        }
        const Dos::LockSlot* slot = find_lock_slot(d, lock_bptr);
        if (slot == nullptr) {
            fail(d, b, ERROR_INVALID_LOCK, DOSFALSE);
            return;
        }
        if (!slot->entry.is_dir) {
            fail(d, b, ERROR_OBJECT_WRONG_TYPE, DOSFALSE);
            return;
        }
        const i64 target_idx = existing >= 0 ? existing : free_assign_slot(d);
        if (target_idx < 0) {
            table_full(d, b, "assign", Dos::kMaxAssigns);
            return;
        }
        Dos::AssignEntry& e = d.assigns_[target_idx];
        const Dos::AssignEntry saved = e;
        e.used = true;
        e.type = DLT_DIRECTORY;
        copy_cstr(e.name, name, sizeof e.name);
        e.dir_count = 1;
        e.locks[0] = lock_bptr;
        e.path[0] = '\0';
        if (!build_assign_node(d, b, e)) {
            e = saved;
            fail(d, b, ERROR_NO_FREE_STORE, DOSFALSE);
            return;
        }
        const i64 user_lock = lock_slot(d, lock_bptr);
        if (user_lock >= 0) {
            d.lock_slots_[user_lock].command_owned = false;
        }
        if (saved.used) {
            for (u32 i = 0; i < saved.dir_count; ++i) {
                if (saved.locks[i] != lock_bptr) {
                    release_lock(d, saved.locks[i]);
                }
            }
        }
        relink_dos_list(d, b);
        if (b.ok()) {
            native::set_result(m, DOSTRUE);
        }
    }

    static void assign_path_impl(Dos& d, Machine& m, u32 type) {
        Bus b(m);
        char name[32];
        if (!read_assign_name(d, b, m.cpu.d[1], name)) {
            return;
        }
        const u32 path_addr = m.cpu.d[2];
        if (path_addr == 0) {
            fail(d, b, ERROR_REQUIRED_ARG_MISSING, DOSFALSE);
            return;
        }
        char path[Dos::kMaxPath];
        if (!b.string(path_addr, path, sizeof path)) {
            if (b.ok()) {
                fail(d, b, ERROR_INVALID_COMPONENT_NAME, DOSFALSE);
            }
            return;
        }
        const u32 name_len = length_of(name);
        if (is_volume_or_device(d, name, name_len)) {
            fail(d, b, ERROR_OBJECT_EXISTS, DOSFALSE);
            return;
        }
        const i64 existing = find_assign(d, name, name_len);
        const i64 target_idx = existing >= 0 ? existing : free_assign_slot(d);
        if (target_idx < 0) {
            table_full(d, b, "assign", Dos::kMaxAssigns);
            return;
        }
        Dos::AssignEntry& e = d.assigns_[target_idx];
        const Dos::AssignEntry saved = e;
        e.used = true;
        e.type = type;
        copy_cstr(e.name, name, sizeof e.name);
        e.dir_count = 0;
        copy_cstr(e.path, path, sizeof e.path);
        if (!build_assign_node(d, b, e)) {
            e = saved;
            fail(d, b, ERROR_NO_FREE_STORE, DOSFALSE);
            return;
        }
        if (saved.used) {
            for (u32 i = 0; i < saved.dir_count; ++i) {
                release_lock(d, saved.locks[i]);
            }
        }
        relink_dos_list(d, b);
        if (b.ok()) {
            native::set_result(m, DOSTRUE);
        }
    }

    static void assign_late(void* ctx, Machine& m) {
        assign_path_impl(of(ctx), m, DLT_LATE);
    }

    static void assign_path(void* ctx, Machine& m) {
        assign_path_impl(of(ctx), m, DLT_NONBINDING);
    }

    static void assign_add(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        char name[32];
        if (!read_assign_name(d, b, m.cpu.d[1], name)) {
            return;
        }
        const u32 lock_bptr = m.cpu.d[2];
        const Dos::LockSlot* slot = find_lock_slot(d, lock_bptr);
        if (slot == nullptr) {
            fail(d, b, ERROR_INVALID_LOCK, DOSFALSE);
            return;
        }
        if (!slot->entry.is_dir) {
            fail(d, b, ERROR_OBJECT_WRONG_TYPE, DOSFALSE);
            return;
        }
        const i64 idx = find_assign(d, name, length_of(name));
        if (idx < 0) {
            fail(d, b, ERROR_OBJECT_NOT_FOUND, DOSFALSE);
            return;
        }
        Dos::AssignEntry& e = d.assigns_[idx];
        if (e.type == DLT_LATE) {
            u32 error = 0;
            if (!resolve_late_assign(d, b, e, error, 1)) {
                if (b.ok()) {
                    fail(d, b, error, DOSFALSE);
                }
                return;
            }
        }
        if (e.type != DLT_DIRECTORY) {
            fail(d, b, ERROR_OBJECT_WRONG_TYPE, DOSFALSE);
            return;
        }
        for (u32 i = 0; i < e.dir_count; ++i) {
            if (e.locks[i] == lock_bptr) {
                native::set_result(m, DOSTRUE);
                return;
            }
        }
        if (e.dir_count >= Dos::kMaxAssignDirs) {
            table_full(d, b, "assign directory", Dos::kMaxAssignDirs);
            return;
        }
        e.locks[e.dir_count++] = lock_bptr;
        if (!build_assign_node(d, b, e)) {
            --e.dir_count;
            fail(d, b, ERROR_NO_FREE_STORE, DOSFALSE);
            return;
        }
        const i64 user_lock = lock_slot(d, lock_bptr);
        if (user_lock >= 0) {
            d.lock_slots_[user_lock].command_owned = false;
        }
        relink_dos_list(d, b);
        if (b.ok()) {
            native::set_result(m, DOSTRUE);
        }
    }

    static void rem_assign_list(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        char name[32];
        if (!read_assign_name(d, b, m.cpu.d[1], name)) {
            return;
        }
        const Dos::LockSlot* target = find_lock_slot(d, m.cpu.d[2]);
        if (target == nullptr) {
            fail(d, b, ERROR_INVALID_LOCK, DOSFALSE);
            return;
        }
        const u32 target_key = target->entry.header_block;
        const u8 target_vol = target->entry.volume;
        const i64 idx = find_assign(d, name, length_of(name));
        if (idx < 0) {
            fail(d, b, ERROR_OBJECT_NOT_FOUND, DOSFALSE);
            return;
        }
        Dos::AssignEntry& e = d.assigns_[idx];
        if (e.type != DLT_DIRECTORY) {
            fail(d, b, ERROR_OBJECT_WRONG_TYPE, DOSFALSE);
            return;
        }
        i64 match = -1;
        for (u32 i = 0; i < e.dir_count; ++i) {
            const Dos::LockSlot* s = find_lock_slot(d, e.locks[i]);
            if (s != nullptr && s->entry.volume == target_vol &&
                s->entry.header_block == target_key) {
                match = static_cast<i64>(i);
                break;
            }
        }
        if (match < 0) {
            fail(d, b, ERROR_OBJECT_NOT_FOUND, DOSFALSE);
            return;
        }
        const u32 removed_lock = e.locks[match];
        if (e.dir_count == 1) {
            release_lock(d, removed_lock);
            if (e.node != 0) {
                d.exec_->free(e.node, e.node_bytes);
            }
            e = Dos::AssignEntry{};
            relink_dos_list(d, b);
            if (b.ok()) {
                native::set_result(m, DOSTRUE);
            }
            return;
        }
        const Dos::AssignEntry saved = e;
        for (u32 i = static_cast<u32>(match); i + 1 < e.dir_count; ++i) {
            e.locks[i] = e.locks[i + 1];
        }
        --e.dir_count;
        if (!build_assign_node(d, b, e)) {
            e = saved;
            fail(d, b, ERROR_NO_FREE_STORE, DOSFALSE);
            return;
        }
        release_lock(d, removed_lock);
        relink_dos_list(d, b);
        if (b.ok()) {
            native::set_result(m, DOSTRUE);
        }
    }

    static void free_dev_proc_ptr(Dos& d, Bus& b, u32 dvp) {
        if (dvp == 0) {
            return;
        }
        const u32 flags = b.r32(dvp + dvp_Flags);
        const u32 lock = b.r32(dvp + dvp_Lock);
        if (!b.ok()) {
            return;
        }
        if ((flags & DVPF_UNLOCK) != 0 && lock != 0) {
            release_lock(d, lock);
        }
        d.exec_->free(native::guest_address(b.machine(), dvp), DevProc_SIZEOF);
    }

    static void free_device_proc(void* ctx, Machine& m) {
        Bus b(m);
        free_dev_proc_ptr(of(ctx), b, m.cpu.d[1]);
    }

    static void get_device_proc(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 last = m.cpu.d[2];
        if (last != 0) {
            const u32 flags = b.r32(last + dvp_Flags);
            const u32 lock = b.r32(last + dvp_Lock);
            const u32 dev_node = b.r32(last + dvp_DevNode);
            if (!b.ok()) {
                return;
            }
            if ((flags & DVPF_ASSIGN) != 0) {
                for (u32 i = 0; i < Dos::kMaxAssigns; ++i) {
                    const Dos::AssignEntry& e = d.assigns_[i];
                    if (!e.used || e.node != dev_node || e.type != DLT_DIRECTORY) {
                        continue;
                    }
                    for (u32 k = 0; k + 1 < e.dir_count; ++k) {
                        if (e.locks[k] == lock) {
                            b.w32(last + dvp_Lock, e.locks[k + 1]);
                            if (b.ok()) {
                                native::set_result(m, last);
                            }
                            return;
                        }
                    }
                    break;
                }
            }
            free_dev_proc_ptr(d, b, last);
            if (b.ok()) {
                fail(d, b, ERROR_NO_MORE_ENTRIES, 0);
            }
            return;
        }
        if (m.cpu.d[1] == 0) {
            fail(d, b, ERROR_REQUIRED_ARG_MISSING, 0);
            return;
        }
        char name[kMaxName];
        if (!read_name(d, b, m.cpu.d[1], name)) {
            if (b.ok()) {
                native::set_result(m, 0);
            }
            return;
        }
        u32 colon = 0;
        while (name[colon] != '\0' && name[colon] != ':') {
            ++colon;
        }
        u32 dvp_lock = 0;
        u32 dvp_flags = 0;
        u32 dvp_node = 0;
        if (name[colon] != ':') {
            const u32 current = b.r32(d.process_ + pr_CurrentDir);
            if (!b.ok()) {
                return;
            }
            u8 cur_vol = Dos::kVolAdf;
            if (current != 0) {
                const Dos::LockSlot* slot = find_lock_slot(d, current);
                if (slot == nullptr) {
                    fail(d, b, ERROR_INVALID_LOCK, 0);
                    return;
                }
                cur_vol = slot->entry.volume;
            }
            dvp_lock = current;
            dvp_node = volume_node(d, cur_vol);
        } else if (is_console(name)) {
            fail(d, b, ERROR_DEVICE_NOT_MOUNTED, 0);
            return;
        } else if (colon == 0) {
            const u32 current = b.r32(d.process_ + pr_CurrentDir);
            if (!b.ok()) {
                return;
            }
            u8 cur_vol = Dos::kVolAdf;
            if (current != 0) {
                const Dos::LockSlot* slot = find_lock_slot(d, current);
                if (slot == nullptr) {
                    fail(d, b, ERROR_INVALID_LOCK, 0);
                    return;
                }
                cur_vol = slot->entry.volume;
            }
            dvp_node = volume_node(d, cur_vol);
        } else if (same_name(name, colon, d.volume_.name)) {
            dvp_node = d.volume_node_;
        } else if (same_name(name, colon, "DF0")) {
            dvp_node = d.device_node_;
        } else if (same_name(name, colon, "Ram Disk")) {
            dvp_node = d.ram_volume_node_;
        } else if (same_name(name, colon, "RAM")) {
            dvp_node = d.ram_device_node_;
        } else {
            const i64 idx = find_assign(d, name, colon);
            if (idx < 0) {
                fail(d, b, ERROR_DEVICE_NOT_MOUNTED, 0);
                return;
            }
            Dos::AssignEntry& e = d.assigns_[idx];
            if (e.type == DLT_LATE) {
                u32 error = 0;
                if (!resolve_late_assign(d, b, e, error, 1)) {
                    if (b.ok()) {
                        fail(d, b, error, 0);
                    }
                    return;
                }
            }
            if (e.type == DLT_NONBINDING) {
                u8 vol = Dos::kVolAdf;
                char resolved[Dos::kMaxPath];
                u32 error = 0;
                if (!resolve(d, b, e.path, vol, resolved, error, 1)) {
                    if (b.ok()) {
                        fail(d, b, error, 0);
                    }
                    return;
                }
                DirEntry entry{};
                const u32 err = fs_find(d, vol, resolved, entry);
                if (err != 0) {
                    fail(d, b, err, 0);
                    return;
                }
                if (!entry.is_dir) {
                    fail(d, b, ERROR_OBJECT_WRONG_TYPE, 0);
                    return;
                }
                if (!alloc_assign_lock(d, b, resolved, entry, dvp_lock)) {
                    const u32 lock_err = (entry.volume == Dos::kVolRam && entry.header_block > 0 &&
                                          ram_node_busy(d, entry.header_block - 1, false))
                                             ? ERROR_OBJECT_IN_USE
                                             : ERROR_NO_FREE_STORE;
                    fail(d, b, lock_err, 0);
                    return;
                }
                dvp_flags = DVPF_ASSIGN | DVPF_UNLOCK;
                dvp_node = e.node;
            } else {
                dvp_lock = e.locks[0];
                dvp_flags = DVPF_ASSIGN;
                dvp_node = e.node;
            }
        }
        const u32 dvp = d.exec_->alloc(DevProc_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
        if (dvp == 0) {
            if ((dvp_flags & DVPF_UNLOCK) != 0) {
                release_lock(d, dvp_lock);
            }
            fail(d, b, ERROR_NO_FREE_STORE, 0);
            return;
        }
        b.w32(dvp + dvp_Port, d.port_);
        b.w32(dvp + dvp_Lock, dvp_lock);
        b.w32(dvp + dvp_Flags, dvp_flags);
        b.w32(dvp + dvp_DevNode, dvp_node);
        if (!b.ok()) {
            if ((dvp_flags & DVPF_UNLOCK) != 0) {
                release_lock(d, dvp_lock);
            }
            d.exec_->free(dvp, DevProc_SIZEOF);
            return;
        }
        native::set_result(m, dvp);
    }

    static u32 dos_list_head(const Dos& d) { return d.block_ + kInfoAt + di_DevInfo; }

    static bool dos_type_matches(u32 type, u32 flags) {
        switch (type) {
        case DLT_DEVICE: return (flags & LDF_DEVICES) != 0;
        case DLT_VOLUME: return (flags & LDF_VOLUMES) != 0;
        case DLT_DIRECTORY:
        case DLT_LATE:
        case DLT_NONBINDING: return (flags & LDF_ASSIGNS) != 0;
        default: return false;
        }
    }

    static u32 step_dos_entry(Bus& b, u32 dlist, u32 flags) {
        if (dlist == 0) {
            return 0;
        }
        u32 cur = dlist;
        for (u32 n = 0; n < kMaxDosEntries; ++n) {
            const u32 next_bptr = b.r32(cur + dol_Next);
            if (!b.ok() || next_bptr == 0) {
                return 0;
            }
            cur = next_bptr << 2;
            const u32 type = b.r32(cur + dol_Type);
            if (!b.ok()) {
                return 0;
            }
            if (dos_type_matches(type, flags)) {
                return cur;
            }
        }
        LogLine().text("dos: DosList has no end").send();
        return 0;
    }

    static void lock_dos_list(void* ctx, Machine& m) {
        native::set_result(m, dos_list_head(of(ctx)));
    }

    static void unlock_dos_list(void*, Machine&) {}

    static void next_dos_entry(void*, Machine& m) {
        Bus b(m);
        const u32 next = step_dos_entry(b, m.cpu.d[1], m.cpu.d[2]);
        if (b.ok()) {
            native::set_result(m, next);
        }
    }

    static void find_dos_entry(void*, Machine& m) {
        Bus b(m);
        u32 cur = m.cpu.d[1];
        const u32 name_addr = m.cpu.d[2];
        const u32 flags = m.cpu.d[3];
        if (cur == 0 || name_addr == 0) {
            native::set_result(m, 0);
            return;
        }
        char target[kMaxName];
        if (!b.string(name_addr, target, sizeof target)) {
            if (b.ok()) {
                native::set_result(m, 0);
            }
            return;
        }
        for (u32 n = 0; n < kMaxDosEntries; ++n) {
            cur = step_dos_entry(b, cur, flags);
            if (!b.ok() || cur == 0) {
                break;
            }
            const u32 bstr_bptr = b.r32(cur + dol_Name);
            if (!b.ok()) {
                return;
            }
            if (bstr_bptr == 0) {
                continue;
            }
            const u32 bstr = bstr_bptr << 2;
            const u32 len = b.r8(bstr);
            if (!b.ok()) {
                return;
            }
            if (len >= kMaxName) {
                continue;
            }
            char entry_name[kMaxName];
            for (u32 i = 0; i < len; ++i) {
                entry_name[i] = static_cast<char>(b.r8(bstr + 1 + i));
            }
            if (!b.ok()) {
                return;
            }
            if (same_name(entry_name, len, target)) {
                native::set_result(m, cur);
                return;
            }
        }
        if (b.ok()) {
            native::set_result(m, 0);
        }
    }

    // Fills a FileInfoBlock from an entry and its metadata.
    static bool fill_fib(Dos& d, Bus& b, u32 fib, const DirEntry& entry) {
        FsMeta meta{};
        const u32 err = fs_meta(d, entry, meta);
        if (err != 0) {
            set_io_err(d, b, err);
            return false;
        }
        b.w32(fib + fib_DiskKey, entry.header_block);
        b.w32(fib + fib_DirEntryType, meta.type);
        u32 i = 0;
        for (; entry.name[i] != '\0' && i < 107; ++i) {
            b.w8(fib + fib_FileName + i, static_cast<u8>(entry.name[i]));
        }
        b.w8(fib + fib_FileName + i, 0);
        b.w32(fib + fib_Protection, meta.protection);
        b.w32(fib + fib_EntryType, meta.type);
        b.w32(fib + fib_Size, meta.size);
        b.w32(fib + fib_NumBlocks, meta.num_blocks);
        b.w32(fib + fib_DateStamp + ds_Days, meta.days);
        b.w32(fib + fib_DateStamp + ds_Minute, meta.minute);
        b.w32(fib + fib_DateStamp + ds_Tick, meta.tick);
        for (u32 k = 0; k < meta.comment_len; ++k) {
            b.w8(fib + fib_Comment + k, static_cast<u8>(meta.comment[k]));
        }
        b.w8(fib + fib_Comment + meta.comment_len, 0);
        b.w16(fib + fib_OwnerUID, 0);
        b.w16(fib + fib_OwnerGID, 0);
        return b.ok();
    }

    static bool examine_impl(Dos& d, Bus& b, u32 lock_bptr, u32 fib) {
        DirEntry entry{};
        const char* path = nullptr;
        if (!lock_entry(d, b, lock_bptr, entry, path)) {
            native::set_result(b.machine(), DOSFALSE);
            return false;
        }
        const bool ok = fill_fib(d, b, fib, entry);
        native::set_result(b.machine(), ok ? DOSTRUE : DOSFALSE);
        return ok;
    }

    static void examine(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        examine_impl(d, b, m.cpu.d[1], m.cpu.d[2]);
    }

    static void examine_fh(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const i64 slot = handle_slot(d, m.cpu.d[1]);
        if (slot < 0) {
            fail(d, b, ERROR_INVALID_LOCK, DOSFALSE);
            return;
        }
        const Dos::FileSlot& f = d.files_[slot];
        if (f.stream != Dos::Stream::File) {
            fail(d, b, ERROR_ACTION_NOT_KNOWN, DOSFALSE);
            return;
        }
        const bool ok = fill_fib(d, b, m.cpu.d[2], f.entry);
        native::set_result(m, ok ? DOSTRUE : DOSFALSE);
    }

    // Finds the entry after the one with header block `after` in listing
    // order, or the first when `after` is the directory itself.
    struct NextEntry {
        u32 after;
        bool first;
        bool seen;
        bool found;
        DirEntry entry;

        static void visit(void* ctx, const DirEntry& e) {
            NextEntry& n = *static_cast<NextEntry*>(ctx);
            if (n.found) {
                return;
            }
            if (n.first || n.seen) {
                n.entry = e;
                n.found = true;
                return;
            }
            n.seen = e.header_block == n.after;
        }
    };

    static bool ex_next_impl(Dos& d, Bus& b, u32 lock_bptr, u32 fib) {
        DirEntry dir{};
        const char* path = nullptr;
        if (!lock_entry(d, b, lock_bptr, dir, path)) {
            native::set_result(b.machine(), DOSFALSE);
            return false;
        }
        const u32 after = b.r32(fib + fib_DiskKey);
        if (!b.ok()) {
            return false;
        }
        if (!dir.is_dir) {
            fail(d, b, ERROR_OBJECT_WRONG_TYPE, DOSFALSE);
            return false;
        }
        NextEntry next{after, dir.volume == Dos::kVolRam || after == dir.header_block, false,
                       false, DirEntry{}};
        const u32 err = fs_list_from(d, dir, path, after,
                                     after == dir.header_block || after == 0,
                                     NextEntry::visit, &next);
        if (err != 0) {
            fail(d, b, err, DOSFALSE);
            return false;
        }
        if (!next.found) {
            fail(d, b, ERROR_NO_MORE_ENTRIES, DOSFALSE);
            return false;
        }
        const bool ok = fill_fib(d, b, fib, next.entry);
        native::set_result(b.machine(), ok ? DOSTRUE : DOSFALSE);
        return ok;
    }

    static void ex_next(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        ex_next_impl(d, b, m.cpu.d[1], m.cpu.d[2]);
    }

    static u32 ex_all_header_size(u32 type) {
        switch (type) {
        case ED_NAME: return 8;
        case ED_TYPE: return 12;
        case ED_SIZE: return 16;
        case ED_PROTECTION: return 20;
        case ED_DATE: return 32;
        case ED_COMMENT: return 36;
        default: return 0;
        }
    }

    // --- pattern matching and directory anchors --------------------------------

    // Reads a NUL-terminated tokenised pattern from guest memory. False if it
    // faults (b.ok() is false) or exceeds kDosPatMaxTokens bytes.
    static bool read_pattern_tokens(Bus& b, u32 addr,
                                    u8 (&tokens)[detail::kDosPatMaxTokens + 1]) {
        for (u32 i = 0; i <= detail::kDosPatMaxTokens; ++i) {
            const u32 c = b.r8(addr + i);
            if (!b.ok()) {
                return false;
            }
            tokens[i] = static_cast<u8>(c);
            if (c == 0) {
                return true;
            }
        }
        return false;
    }

    static void parse_pattern_impl(Dos& d, Machine& m, bool no_case) {
        Bus b(m);
        const u32 pat_addr = m.cpu.d[1];
        const u32 buf_addr = m.cpu.d[2];
        const u32 buflen = m.cpu.d[3];
        if (pat_addr == 0) {
            fail(d, b, ERROR_BAD_TEMPLATE, 0xFFFFFFFF);
            return;
        }
        if (buf_addr == 0 || buflen == 0 || (buflen & 0x80000000) != 0) {
            fail(d, b, ERROR_LINE_TOO_LONG, 0xFFFFFFFF);
            return;
        }
        char src[detail::kDosPatMaxPat + 1];
        if (!b.string(pat_addr, src, sizeof src)) {
            if (b.ok()) {
                fail(d, b, ERROR_LINE_TOO_LONG, 0xFFFFFFFF);
            }
            return;
        }
        u8 tokens[detail::kDosPatMaxTokens]{};
        const u32 src_len = length_of(src);
        const i32 wild = dos_parse_pattern(reinterpret_cast<const u8*>(src), src_len, tokens,
                                           sizeof tokens, no_case);
        if (wild < 0) {
            fail(d, b, ERROR_BAD_TEMPLATE, 0xFFFFFFFF);
            return;
        }
        u32 tok_len = 0;
        while (tok_len < sizeof tokens && tokens[tok_len] != '\0') {
            ++tok_len;
        }
        if (tok_len + 1 > buflen) {
            fail(d, b, ERROR_LINE_TOO_LONG, 0xFFFFFFFF);
            return;
        }
        for (u32 i = 0; i <= tok_len; ++i) {
            b.w8(buf_addr + i, tokens[i]);
        }
        if (b.ok()) {
            native::set_result(m, static_cast<u32>(wild));
        }
    }

    static void match_pattern_impl(Dos& d, Machine& m, bool no_case) {
        Bus b(m);
        const u32 pat_addr = m.cpu.d[1];
        const u32 str_addr = m.cpu.d[2];
        if (pat_addr == 0 || str_addr == 0) {
            native::set_result(m, DOSFALSE);
            return;
        }
        u8 tokens[detail::kDosPatMaxTokens + 1];
        if (!read_pattern_tokens(b, pat_addr, tokens)) {
            if (b.ok()) {
                fail(d, b, ERROR_LINE_TOO_LONG, DOSFALSE);
            }
            return;
        }
        char str[detail::kDosPatMaxStr + 1];
        if (!b.string(str_addr, str, sizeof str)) {
            if (b.ok()) {
                fail(d, b, ERROR_LINE_TOO_LONG, DOSFALSE);
            }
            return;
        }
        const bool matched =
            dos_match_pattern(tokens, reinterpret_cast<const u8*>(str), length_of(str), no_case);
        native::set_result(m, matched ? DOSTRUE : DOSFALSE);
    }

    static void parse_pattern(void* ctx, Machine& m) { parse_pattern_impl(of(ctx), m, false); }
    static void match_pattern(void* ctx, Machine& m) { match_pattern_impl(of(ctx), m, false); }
    static void parse_pattern_nc(void* ctx, Machine& m) { parse_pattern_impl(of(ctx), m, true); }
    static void match_pattern_nc(void* ctx, Machine& m) { match_pattern_impl(of(ctx), m, true); }

    struct MatchVisitor {
        u32 after;
        bool first;
        bool seen;
        bool found;
        const u8* tokens;
        DirEntry entry;

        static void visit(void* ctx, const DirEntry& e) {
            MatchVisitor& v = *static_cast<MatchVisitor*>(ctx);
            if (v.found) {
                return;
            }
            if (!v.first && !v.seen) {
                v.seen = e.header_block == v.after;
                return;
            }
            if (dos_match_pattern(v.tokens, reinterpret_cast<const u8*>(e.name),
                                  length_of(e.name), true)) {
                v.entry = e;
                v.found = true;
            }
        }
    };

    static i64 find_anchor_slot(const Dos& d, u32 key) {
        for (u32 i = 0; i < Dos::kMaxAnchors; ++i) {
            if (d.anchors_[i].used && d.anchors_[i].anchor_addr == key) {
                return static_cast<i64>(i);
            }
        }
        return -1;
    }

    static i64 free_anchor_slot(const Dos& d) {
        for (u32 i = 0; i < Dos::kMaxAnchors; ++i) {
            if (!d.anchors_[i].used) {
                return static_cast<i64>(i);
            }
        }
        return -1;
    }

    static bool check_anchor_break(Dos& d, Bus& b, u32 anchor) {
        const u32 break_bits = b.r32(anchor + ap_BreakBits);
        b.w32(anchor + ap_FoundBreak, 0);
        if (!b.ok() || break_bits == 0) {
            return false;
        }
        const u32 recvd = b.r32(d.process_ + TC_SIGRECVD);
        if (!b.ok()) {
            return false;
        }
        const u32 hit = recvd & break_bits;
        if (hit != 0) {
            b.w32(d.process_ + TC_SIGRECVD, recvd & ~hit);
            b.w32(anchor + ap_FoundBreak, hit);
            fail(d, b, ERROR_BREAK, ERROR_BREAK);
            return true;
        }
        return false;
    }

    static void join_path(const char* a, const char* b, char* out, u32 cap) {
        u32 pos = 0;
        for (u32 i = 0; a[i] != '\0' && pos + 1 < cap; ++i) {
            out[pos++] = a[i];
        }
        for (u32 i = 0; b[i] != '\0' && pos + 1 < cap; ++i) {
            out[pos++] = b[i];
        }
        out[pos] = '\0';
    }

    static bool join_volume_path(const char* parent, const char* child,
                                 char (&out)[Dos::kMaxPath]) {
        u32 pos = 0;
        for (u32 i = 0; parent[i] != '\0'; ++i) {
            if (pos + 1 >= Dos::kMaxPath) {
                return false;
            }
            out[pos++] = parent[i];
        }
        if (pos > 0 && child[0] != '\0') {
            if (pos + 1 >= Dos::kMaxPath) {
                return false;
            }
            out[pos++] = '/';
        }
        for (u32 i = 0; child[i] != '\0'; ++i) {
            if (pos + 1 >= Dos::kMaxPath) {
                return false;
            }
            out[pos++] = child[i];
        }
        out[pos] = '\0';
        return true;
    }

    static bool build_match_path(const Dos::AnchorSlot& s, u32 level_idx, const char* entry_name,
                                 char* out, u32 cap) {
        u32 pos = 0;
        auto push_char = [&](char c) -> bool {
            if (pos + 1 >= cap) {
                return false;
            }
            out[pos++] = c;
            return true;
        };
        auto push_part = [&](const char* part, bool need_sep) -> bool {
            if (part[0] == '\0') {
                return true;
            }
            if (need_sep && pos > 0 && out[pos - 1] != ':' && out[pos - 1] != '/') {
                if (!push_char('/')) {
                    return false;
                }
            }
            for (u32 i = 0; part[i] != '\0'; ++i) {
                if (!push_char(part[i])) {
                    return false;
                }
            }
            return true;
        };
        if (!push_part(s.prefix, false)) {
            return false;
        }
        for (u32 i = 1; i <= level_idx; ++i) {
            if (!push_part(s.levels[i].dir_label, true)) {
                return false;
            }
        }
        if (!push_part(entry_name, true)) {
            return false;
        }
        out[pos] = '\0';
        return true;
    }

    static bool write_anchor_match(Dos& d, Bus& b, u32 anchor, const char* full_path,
                                   const DirEntry& entry) {
        const u32 ap_len = b.r16(anchor + ap_Strlen);
        if (!b.ok()) {
            return false;
        }
        if ((ap_len & 0x8000u) == 0 && ap_len > 0) {
            const u32 len = length_of(full_path);
            if (len + 1 > ap_len) {
                fail(d, b, ERROR_BUFFER_OVERFLOW, ERROR_BUFFER_OVERFLOW);
                return false;
            }
            put_string(b, anchor + ap_Buf, full_path);
        }
        if (!fill_fib(d, b, anchor + ap_Info, entry)) {
            if (b.ok()) {
                const u32 err = b.r32(d.process_ + pr_Result2);
                native::set_result(b.machine(), err);
            }
            return false;
        }
        return b.ok();
    }

    static bool alloc_achain_node(Dos& d, Bus& b, u32 parent_chain, const char* dir_path,
                                  const DirEntry& dir_entry, u8 an_flags, const char* label,
                                  Dos::AnchorLevel& lvl, bool& oom) {
        oom = false;
        u32 lock_bptr = 0;
        if (!alloc_user_lock(d, b, dir_path, dir_entry, SHARED_LOCK, lock_bptr)) {
            oom = b.ok() && b.r32(d.process_ + pr_Result2) == ERROR_NO_FREE_STORE;
            return false;
        }
        const u32 label_len = length_of(label);
        const u32 bytes = an_String + label_len + 1;
        const u32 chain = d.exec_->alloc(bytes, MEMF_PUBLIC | MEMF_CLEAR);
        if (chain == 0) {
            free_user_lock(d, lock_bptr);
            oom = true;
            return false;
        }
        b.w32(chain + an_Child, 0);
        b.w32(chain + an_Parent, parent_chain);
        b.w32(chain + an_Lock, lock_bptr);
        if (!fill_fib(d, b, chain + an_Info, dir_entry)) {
            d.exec_->free(chain, bytes);
            free_user_lock(d, lock_bptr);
            return false;
        }
        b.w8(chain + an_Flags, an_flags);
        put_string(b, chain + an_String, label);
        if (!b.ok()) {
            d.exec_->free(chain, bytes);
            free_user_lock(d, lock_bptr);
            return false;
        }
        lvl.chain_addr = chain;
        lvl.chain_bytes = bytes;
        lvl.lock_bptr = lock_bptr;
        lvl.dir = dir_entry;
        copy_cstr(lvl.dir_path, dir_path, Dos::kMaxPath);
        copy_cstr(lvl.dir_label, label, sizeof lvl.dir_label);
        lvl.after = 0;
        lvl.first = true;
        lvl.exhausted = false;
        return true;
    }

    static void free_achain_level(Dos& d, Dos::AnchorLevel& lvl) {
        free_user_lock(d, lvl.lock_bptr);
        if (lvl.chain_addr != 0) {
            d.exec_->free(lvl.chain_addr, lvl.chain_bytes);
        }
        lvl.chain_addr = 0;
        lvl.chain_bytes = 0;
        lvl.lock_bptr = 0;
    }

    static void release_anchor_slot(Dos& d, u32 slot_idx) {
        Dos::AnchorSlot& s = d.anchors_[slot_idx];
        for (u32 i = 0; i < s.depth && i < Dos::kMaxAnchorDepth; ++i) {
            free_achain_level(d, s.levels[i]);
        }
        s.depth = 0;
        s.used = false;
        s.command_owned = false;
    }

    static void cleanup_anchor(Dos& d, Bus& b, u32 slot_idx, u32 anchor) {
        release_anchor_slot(d, slot_idx);
        if (anchor != 0) {
            b.w32(anchor + ap_Base, 0);
            b.w32(anchor + ap_Current, 0);
        }
    }

    static void fail_achain_alloc(Dos& d, Bus& b, u32 anchor, u8 flags, bool oom) {
        if (!b.ok()) {
            return;
        }
        if (oom) {
            b.w8(anchor + ap_Flags, static_cast<u8>(flags | APF_NOMEMERR));
            fail(d, b, ERROR_NO_FREE_STORE, ERROR_NO_FREE_STORE);
        } else {
            const u32 err = b.r32(d.process_ + pr_Result2);
            native::set_result(b.machine(), err);
        }
    }

    static void match_first(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 pat_addr = m.cpu.d[1];
        const u32 anchor = m.cpu.d[2];
        if (pat_addr == 0 || anchor == 0) {
            fail(d, b, ERROR_REQUIRED_ARG_MISSING, ERROR_REQUIRED_ARG_MISSING);
            return;
        }
        const u32 raw_flags = b.r8(anchor + ap_Flags);
        if (!b.ok()) {
            return;
        }
        const u32 key = native::guest_address(m, anchor);
        const i64 existing = find_anchor_slot(d, key);
        if (existing >= 0) {
            cleanup_anchor(d, b, static_cast<u32>(existing), anchor);
        }
        b.w32(anchor + ap_Base, 0);
        b.w32(anchor + ap_Current, 0);
        if (check_anchor_break(d, b, anchor) || !b.ok()) {
            return;
        }
        char pat_str[detail::kDosPatMaxPat + 1];
        if (!b.string(pat_addr, pat_str, sizeof pat_str)) {
            if (b.ok()) {
                fail(d, b, ERROR_LINE_TOO_LONG, ERROR_LINE_TOO_LONG);
            }
            return;
        }
        const u32 pat_len = length_of(pat_str);
        u32 split = 0;
        for (u32 i = 0; i < pat_len; ++i) {
            if (pat_str[i] == '/' || pat_str[i] == ':') {
                split = i + 1;
            }
        }
        char prefix[detail::kDosPatMaxPat + 1];
        for (u32 i = 0; i < split; ++i) {
            prefix[i] = pat_str[i];
        }
        prefix[split] = '\0';
        const char* leaf = pat_str + split;
        const u32 leaf_len = pat_len - split;

        u8 tokens[detail::kDosPatMaxTokens + 1]{};
        const i32 wild = dos_parse_pattern(reinterpret_cast<const u8*>(leaf), leaf_len, tokens,
                                           sizeof tokens, true);
        if (wild < 0) {
            fail(d, b, ERROR_BAD_TEMPLATE, ERROR_BAD_TEMPLATE);
            return;
        }
        u8 flags = static_cast<u8>(raw_flags & ~(APF_DODIR | APF_DIDDIR | APF_NOMEMERR |
                                                APF_DirChanged | APF_ITSWILD));
        if (wild > 0) {
            flags |= static_cast<u8>(APF_ITSWILD);
        }
        b.w8(anchor + ap_Flags, flags);
        if (!b.ok()) {
            return;
        }

        const i64 slot_idx = free_anchor_slot(d);
        if (slot_idx < 0) {
            LogLine().text("dos: anchor table full, ").dec(Dos::kMaxAnchors)
                .text(" entries in use").send();
            fail(d, b, ERROR_NO_FREE_STORE, ERROR_NO_FREE_STORE);
            return;
        }

        Dos::AnchorSlot& s = d.anchors_[slot_idx];
        s = Dos::AnchorSlot{};
        copy_cstr(s.prefix, prefix, sizeof s.prefix);

        if (wild == 0) {
            char lookup[detail::kDosPatMaxPat + 1];
            if (leaf_len == 0) {
                copy_cstr(lookup, pat_str, sizeof lookup);
            } else {
                join_path(prefix, reinterpret_cast<const char*>(tokens), lookup, sizeof lookup);
            }
            u8 vol = Dos::kVolAdf;
            char resolved[Dos::kMaxPath];
            DirEntry entry{};
            u32 error = 0;
            if (!locate(d, b, lookup, vol, resolved, entry, error)) {
                if (b.ok()) {
                    fail(d, b, error, error);
                }
                return;
            }
            char parent_path[Dos::kMaxPath];
            parent_path_of(resolved, parent_path);
            DirEntry parent_entry{};
            const u32 p_err = fs_find(d, vol, parent_path, parent_entry);
            if (p_err != 0) {
                fail(d, b, p_err, p_err);
                return;
            }
            bool oom = false;
            const u8 an_flags = static_cast<u8>(DDF_ExaminedBit | DDF_Single);
            if (!alloc_achain_node(d, b, 0, parent_path, parent_entry, an_flags, "",
                                   s.levels[0], oom)) {
                fail_achain_alloc(d, b, anchor, flags, oom);
                return;
            }
            s.depth = 1;
            s.is_wild = false;
            s.leaf_empty = leaf_len == 0;
            s.cur_entry = entry;
            s.levels[0].first = false;
            s.levels[0].exhausted = true;
            s.levels[0].after = entry.header_block;
            dos_parse_pattern(reinterpret_cast<const u8*>("#?"), 2, s.tokens, sizeof s.tokens,
                              true);
            b.w32(anchor + ap_Base, s.levels[0].chain_addr);
            b.w32(anchor + ap_Current, s.levels[0].chain_addr);
            char full[detail::kDosPatMaxPat + 32 + 1];
            if (leaf_len == 0) {
                copy_cstr(full, pat_str, sizeof full);
            } else {
                join_path(prefix, entry.name, full, sizeof full);
            }
            if (!write_anchor_match(d, b, anchor, full, entry)) {
                cleanup_anchor(d, b, static_cast<u32>(slot_idx), anchor);
                return;
            }
            s.used = true;
            s.command_owned = d.running_;
            s.anchor_addr = key;
            set_io_err(d, b, 0);
            native::set_result(m, 0);
            return;
        }

        u8 vol = Dos::kVolAdf;
        char dir_path[Dos::kMaxPath];
        DirEntry dir_entry{};
        u32 error = 0;
        if (!locate(d, b, prefix, vol, dir_path, dir_entry, error)) {
            if (b.ok()) {
                if (error == ERROR_OBJECT_NOT_FOUND) {
                    error = ERROR_DIR_NOT_FOUND;
                }
                fail(d, b, error, error);
            }
            return;
        }
        if (!dir_entry.is_dir) {
            fail(d, b, ERROR_DIR_NOT_FOUND, ERROR_DIR_NOT_FOUND);
            return;
        }
        MatchVisitor mv{0, true, false, false, tokens, DirEntry{}};
        const u32 list_err = fs_list(d, vol, dir_path, MatchVisitor::visit, &mv);
        if (list_err != 0) {
            fail(d, b, list_err, list_err);
            return;
        }
        if (!mv.found) {
            fail(d, b, ERROR_NO_MORE_ENTRIES, ERROR_NO_MORE_ENTRIES);
            return;
        }
        bool oom = false;
        const u8 an_flags = static_cast<u8>(DDF_PatternBit | DDF_ExaminedBit);
        if (!alloc_achain_node(d, b, 0, dir_path, dir_entry, an_flags, "", s.levels[0], oom)) {
            fail_achain_alloc(d, b, anchor, flags, oom);
            return;
        }
        s.depth = 1;
        s.is_wild = true;
        s.leaf_empty = false;
        s.cur_entry = mv.entry;
        s.levels[0].first = false;
        s.levels[0].after = mv.entry.header_block;
        s.levels[0].exhausted = false;
        for (u32 i = 0; i <= detail::kDosPatMaxTokens; ++i) {
            s.tokens[i] = tokens[i];
            if (tokens[i] == 0) {
                break;
            }
        }
        b.w32(anchor + ap_Base, s.levels[0].chain_addr);
        b.w32(anchor + ap_Current, s.levels[0].chain_addr);
        char full[detail::kDosPatMaxPat + 32 + 1];
        join_path(prefix, mv.entry.name, full, sizeof full);
        if (!write_anchor_match(d, b, anchor, full, mv.entry)) {
            cleanup_anchor(d, b, static_cast<u32>(slot_idx), anchor);
            return;
        }
        s.used = true;
        s.command_owned = d.running_;
        s.anchor_addr = key;
        set_io_err(d, b, 0);
        native::set_result(m, 0);
    }

    static bool write_level_match(Dos& d, Bus& b, u32 anchor, const Dos::AnchorSlot& s,
                                  u32 level_idx, const DirEntry& entry, const char* leaf_name) {
        char full[600];
        if (!build_match_path(s, level_idx, leaf_name, full, sizeof full)) {
            const u32 ap_len = b.r16(anchor + ap_Strlen);
            if (b.ok() && (ap_len & 0x8000u) == 0 && ap_len > 0) {
                fail(d, b, ERROR_BUFFER_OVERFLOW, ERROR_BUFFER_OVERFLOW);
                return false;
            }
            full[0] = '\0';
        }
        return write_anchor_match(d, b, anchor, full, entry);
    }

    static void match_next(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 anchor = m.cpu.d[1];
        if (anchor == 0) {
            fail(d, b, ERROR_REQUIRED_ARG_MISSING, ERROR_REQUIRED_ARG_MISSING);
            return;
        }
        if (check_anchor_break(d, b, anchor) || !b.ok()) {
            return;
        }
        const i64 slot_idx = find_anchor_slot(d, native::guest_address(m, anchor));
        if (slot_idx < 0) {
            fail(d, b, ERROR_NO_MORE_ENTRIES, ERROR_NO_MORE_ENTRIES);
            return;
        }
        Dos::AnchorSlot& s = d.anchors_[slot_idx];
        if (s.depth == 0 || s.depth > Dos::kMaxAnchorDepth) {
            fail(d, b, ERROR_NO_MORE_ENTRIES, ERROR_NO_MORE_ENTRIES);
            return;
        }
        const u32 raw_flags = b.r8(anchor + ap_Flags);
        if (!b.ok()) {
            return;
        }
        const bool want_dodir = (raw_flags & APF_DODIR) != 0;
        u8 flags = static_cast<u8>(raw_flags & ~(APF_DODIR | APF_DirChanged));

        if (want_dodir && s.cur_entry.is_dir) {
            if (s.depth >= Dos::kMaxAnchorDepth) {
                b.w8(anchor + ap_Flags, flags);
                fail(d, b, ERROR_TOO_MANY_LEVELS, ERROR_TOO_MANY_LEVELS);
                return;
            }
            Dos::AnchorLevel& parent = s.levels[s.depth - 1];
            char sub_path[Dos::kMaxPath];
            if (is_root_entry(d, s.cur_entry)) {
                sub_path[0] = '\0';
            } else if (!join_volume_path(parent.dir_path, s.cur_entry.name, sub_path)) {
                b.w8(anchor + ap_Flags, flags);
                fail(d, b, ERROR_INVALID_COMPONENT_NAME, ERROR_INVALID_COMPONENT_NAME);
                return;
            }
            const char* label =
                (s.depth == 1 && !s.is_wild && s.leaf_empty) ? "" : s.cur_entry.name;
            const u8 an_flags =
                static_cast<u8>(DDF_ExaminedBit | (s.is_wild ? DDF_PatternBit : DDF_AllBit));
            bool oom = false;
            Dos::AnchorLevel& child = s.levels[s.depth];
            if (!alloc_achain_node(d, b, parent.chain_addr, sub_path, s.cur_entry, an_flags, label,
                                   child, oom)) {
                if (!oom) {
                    b.w8(anchor + ap_Flags, flags);
                }
                fail_achain_alloc(d, b, anchor, flags, oom);
                return;
            }
            MatchVisitor mv{0, true, false, false, s.tokens, DirEntry{}};
            const u32 list_err = fs_list(d, child.dir.volume, child.dir_path, MatchVisitor::visit, &mv);
            if (list_err != 0) {
                free_achain_level(d, child);
                fail(d, b, list_err, list_err);
                return;
            }
            if (!mv.found) {
                free_achain_level(d, child);
                flags |= static_cast<u8>(APF_DIDDIR);
                b.w8(anchor + ap_Flags, flags);
                const char* leaf_name =
                    (s.depth == 1 && !s.is_wild && s.leaf_empty) ? "" : s.cur_entry.name;
                if (!write_level_match(d, b, anchor, s, s.depth - 1, s.cur_entry, leaf_name)) {
                    return;
                }
                set_io_err(d, b, 0);
                native::set_result(m, 0);
                return;
            }
            b.w32(parent.chain_addr + an_Child, child.chain_addr);
            b.w32(anchor + ap_Current, child.chain_addr);
            flags |= static_cast<u8>(APF_DirChanged);
            b.w8(anchor + ap_Flags, flags);
            child.first = false;
            child.after = mv.entry.header_block;
            ++s.depth;
            if (!write_level_match(d, b, anchor, s, s.depth - 1, mv.entry, mv.entry.name)) {
                return;
            }
            s.cur_entry = mv.entry;
            set_io_err(d, b, 0);
            native::set_result(m, 0);
            return;
        }

        const u32 cur_idx = s.depth - 1;
        Dos::AnchorLevel& cur = s.levels[cur_idx];
        if (cur_idx == 0 && (!s.is_wild || cur.exhausted)) {
            cur.exhausted = true;
            b.w8(anchor + ap_Flags, flags);
            fail(d, b, ERROR_NO_MORE_ENTRIES, ERROR_NO_MORE_ENTRIES);
            return;
        }
        MatchVisitor mv{cur.after, cur.dir.volume == Dos::kVolRam || cur.first, false, false,
                        s.tokens, DirEntry{}};
        const u32 list_err =
            fs_list_from(d, cur.dir, cur.dir_path, cur.after, cur.first, MatchVisitor::visit, &mv);
        if (list_err != 0) {
            fail(d, b, list_err, list_err);
            return;
        }
        if (mv.found) {
            b.w8(anchor + ap_Flags, flags);
            cur.first = false;
            cur.after = mv.entry.header_block;
            if (!write_level_match(d, b, anchor, s, cur_idx, mv.entry, mv.entry.name)) {
                return;
            }
            s.cur_entry = mv.entry;
            set_io_err(d, b, 0);
            native::set_result(m, 0);
            return;
        }
        if (cur_idx == 0) {
            cur.exhausted = true;
            b.w8(anchor + ap_Flags, flags);
            fail(d, b, ERROR_NO_MORE_ENTRIES, ERROR_NO_MORE_ENTRIES);
            return;
        }
        const DirEntry dir_entry = cur.dir;
        free_achain_level(d, cur);
        --s.depth;
        Dos::AnchorLevel& parent = s.levels[s.depth - 1];
        b.w32(parent.chain_addr + an_Child, 0);
        b.w32(anchor + ap_Current, parent.chain_addr);
        flags |= static_cast<u8>(APF_DIDDIR | APF_DirChanged);
        b.w8(anchor + ap_Flags, flags);
        s.cur_entry = dir_entry;
        const char* leaf_name =
            (s.depth == 1 && !s.is_wild && s.leaf_empty) ? "" : dir_entry.name;
        if (!write_level_match(d, b, anchor, s, s.depth - 1, dir_entry, leaf_name)) {
            return;
        }
        set_io_err(d, b, 0);
        native::set_result(m, 0);
    }

    static void match_end(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        const u32 anchor = m.cpu.d[1];
        if (anchor == 0) {
            return;
        }
        const i64 slot_idx = find_anchor_slot(d, native::guest_address(m, anchor));
        if (slot_idx >= 0) {
            Bus b(m);
            cleanup_anchor(d, b, static_cast<u32>(slot_idx), anchor);
        }
    }

    struct ExAllCollector {
        Dos& d;
        Bus& b;
        u32 buffer;
        u32 size;
        u32 type;
        u32 hdr_size;
        u32 after;
        const u8* match_pat;
        bool seen;
        u32 offset;
        u32 entries;
        u32 prev_entry;
        u32 last_key;
        bool full;
        bool more;
        bool error;

        static void visit(void* ctx, const DirEntry& e) {
            ExAllCollector& c = *static_cast<ExAllCollector*>(ctx);
            if (c.error || c.full) {
                if (!c.error) {
                    c.more = true;
                }
                return;
            }
            if (c.after != 0 && !c.seen) {
                c.seen = e.header_block == c.after;
                return;
            }
            const u32 name_len = length_of(e.name);
            if (c.match_pat != nullptr &&
                !dos_match_pattern(c.match_pat, reinterpret_cast<const u8*>(e.name), name_len,
                                   true)) {
                return;
            }
            FsMeta meta{};
            if (c.type >= ED_PROTECTION) {
                const u32 err = fs_meta(c.d, e, meta);
                if (err != 0) {
                    set_io_err(c.d, c.b, err);
                    c.error = true;
                    return;
                }
            }
            const u32 comment_len = c.type >= ED_COMMENT ? meta.comment_len : 0;
            const u32 name_at = c.offset + c.hdr_size;
            const u32 comment_at = name_at + name_len + 1;
            const u32 end = c.type >= ED_COMMENT ? comment_at + comment_len + 1 : comment_at;
            if (end < c.offset || end > c.size) {
                c.full = true;
                c.more = true;
                return;
            }
            const u32 at = c.buffer + c.offset;
            if (c.prev_entry != 0) {
                c.b.w32(c.prev_entry + ed_Next, at);
            }
            c.b.w32(at + ed_Next, 0);
            c.b.w32(at + ed_Name, c.buffer + name_at);
            for (u32 i = 0; i <= name_len; ++i) {
                c.b.w8(c.buffer + name_at + i, static_cast<u8>(e.name[i]));
            }
            if (c.type >= ED_TYPE) {
                c.b.w32(at + ed_Type, e.is_dir ? ST_USERDIR : ST_FILE);
            }
            if (c.type >= ED_SIZE) {
                c.b.w32(at + ed_Size, e.is_dir ? 0 : e.size);
            }
            if (c.type >= ED_PROTECTION) {
                c.b.w32(at + ed_Prot, meta.protection);
            }
            if (c.type >= ED_DATE) {
                c.b.w32(at + ed_Days, meta.days);
                c.b.w32(at + ed_Mins, meta.minute);
                c.b.w32(at + ed_Ticks, meta.tick);
            }
            if (c.type >= ED_COMMENT) {
                c.b.w32(at + ed_Comment, c.buffer + comment_at);
                for (u32 i = 0; i < comment_len; ++i) {
                    c.b.w8(c.buffer + comment_at + i, static_cast<u8>(meta.comment[i]));
                }
                c.b.w8(c.buffer + comment_at + comment_len, 0);
            }
            if (!c.b.ok()) {
                c.error = true;
                return;
            }
            c.prev_entry = at;
            c.offset = align4(end);
            c.entries += 1;
            c.last_key = e.header_block;
        }
    };

    // Lists directory entries into ExAllData records for ED_NAME..ED_COMMENT.
    // Honours eac_MatchString; eac_MatchFunc is ignored.
    static void ex_all(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 buffer = m.cpu.d[2];
        const u32 size = m.cpu.d[3];
        const u32 type = m.cpu.d[4];
        const u32 control = m.cpu.d[5];
        if (control == 0) {
            fail(d, b, ERROR_REQUIRED_ARG_MISSING, DOSFALSE);
            return;
        }
        const u32 hdr_size = ex_all_header_size(type);
        if (hdr_size == 0) {
            fail(d, b, ERROR_BAD_NUMBER, DOSFALSE);
            return;
        }
        if (buffer == 0 || size == 0 || (size & 0x80000000) != 0) {
            fail(d, b, ERROR_BUFFER_OVERFLOW, DOSFALSE);
            return;
        }
        DirEntry dir{};
        const char* path = nullptr;
        if (!lock_entry(d, b, m.cpu.d[1], dir, path)) {
            native::set_result(m, DOSFALSE);
            return;
        }
        if (!dir.is_dir) {
            fail(d, b, ERROR_OBJECT_WRONG_TYPE, DOSFALSE);
            return;
        }
        const u32 after = b.r32(control + eac_LastKey);
        const u32 match_str = b.r32(control + eac_MatchString);
        if (!b.ok()) {
            return;
        }
        u8 match_tokens[detail::kDosPatMaxTokens + 1];
        const u8* match_pat = nullptr;
        if (match_str != 0) {
            if (!read_pattern_tokens(b, match_str, match_tokens)) {
                if (b.ok()) {
                    fail(d, b, ERROR_BAD_TEMPLATE, DOSFALSE);
                }
                return;
            }
            match_pat = match_tokens;
        }
        ExAllCollector col{d, b, buffer, size, type, hdr_size, after, match_pat,
                           dir.volume == Dos::kVolRam, 0, 0, 0, after, false, false, false};
        const u32 err =
            fs_list_from(d, dir, path, after, after == 0, ExAllCollector::visit, &col);
        if (!b.ok()) {
            return;
        }
        if (col.error) {
            native::set_result(m, DOSFALSE);
            return;
        }
        if (err != 0) {
            fail(d, b, err, DOSFALSE);
            return;
        }
        b.w32(control + eac_Entries, col.entries);
        b.w32(control + eac_LastKey, col.last_key);
        if (!b.ok()) {
            return;
        }
        if (col.more) {
            if (col.entries == 0) {
                fail(d, b, ERROR_BUFFER_OVERFLOW, DOSFALSE);
                return;
            }
            native::set_result(m, DOSTRUE);
            return;
        }
        fail(d, b, ERROR_NO_MORE_ENTRIES, DOSFALSE);
    }

    static void ex_all_end(void*, Machine&) {}

    static bool fill_info_data(Dos& d, Bus& b, u32 lock_bptr, u32 id) {
        DirEntry entry{};
        const char* path = nullptr;
        if (!lock_entry(d, b, lock_bptr, entry, path)) {
            native::set_result(b.machine(), DOSFALSE);
            return false;
        }
        b.w32(id + id_NumSoftErrors, 0);
        b.w32(id + id_UnitNumber, 0);
        if (entry.volume == Dos::kVolRam) {
            const u32 blocks = (d.ram_.bytes_used() + 511u) / 512u + d.ram_.used_nodes();
            const u32 used = blocks > 1u ? blocks : 1u;
            b.w32(id + id_DiskState, ID_VALIDATED);
            b.w32(id + id_NumBlocks, used);
            b.w32(id + id_NumBlocksUsed, used);
            b.w32(id + id_BytesPerBlock, 512);
            b.w32(id + id_DiskType, ID_DOS_DISK);
            b.w32(id + id_VolumeNode, d.ram_volume_node_ >> 2);
        } else {
            const u32 blocks = d.volume_.block_count - 2;  // the bootblock is not counted
            b.w32(id + id_DiskState, ID_WRITE_PROTECTED);
            b.w32(id + id_NumBlocks, blocks);
            b.w32(id + id_NumBlocksUsed, blocks);
            b.w32(id + id_BytesPerBlock, d.volume_.ffs ? 512 : 488);
            b.w32(id + id_DiskType, ID_DOS_DISK | d.volume_.image[3]);
            b.w32(id + id_VolumeNode, d.volume_node_ >> 2);
        }
        b.w32(id + id_InUse, 0);
        if (b.ok()) {
            native::set_result(b.machine(), DOSTRUE);
            return true;
        }
        return false;
    }

    static void info(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        fill_info_data(d, b, m.cpu.d[1], m.cpu.d[2]);
    }

    static void log_unknown_pkt(Dos& d, u32 action) {
        for (u32 i = 0; i < d.unknown_pkt_count_; ++i) {
            if (d.unknown_pkt_actions_[i] == action) {
                return;
            }
        }
        if (d.unknown_pkt_count_ < Dos::kMaxLoggedActions) {
            d.unknown_pkt_actions_[d.unknown_pkt_count_++] = action;
        }
        LogLine().text("dos: DoPkt action ").dec(action).text(" not handled").send();
    }

    static bool read_bstr_arg(Dos& d, Bus& b, u32 bstr_bptr, char* out, u32 max_len,
                              u32 too_big_err, u32 fail_res, bool allow_null) {
        if (bstr_bptr == 0) {
            if (allow_null) {
                out[0] = '\0';
                return true;
            }
            fail(d, b, ERROR_REQUIRED_ARG_MISSING, fail_res);
            return false;
        }
        const u32 bstr = bstr_bptr << 2;
        const u32 len = b.r8(bstr);
        if (!b.ok()) {
            return false;
        }
        if (len > max_len) {
            fail(d, b, too_big_err, fail_res);
            return false;
        }
        for (u32 i = 0; i < len; ++i) {
            out[i] = static_cast<char>(b.r8(bstr + 1 + i));
        }
        out[len] = '\0';
        return b.ok();
    }

    static bool resolve_from_lock(Dos& d, Bus& b, u32 base_lock, const char* name, u8& vol,
                                  char (&resolved)[Dos::kMaxPath], u32& error) {
        DirEntry base_entry{};
        const char* base_path = nullptr;
        if (!lock_entry(d, b, base_lock, base_entry, base_path)) {
            error = b.ok() ? b.r32(d.process_ + pr_Result2) : 0;
            return false;
        }
        u32 colon = 0;
        while (name[colon] != '\0' && name[colon] != ':') {
            ++colon;
        }
        if (name[colon] == ':') {
            if (colon == 0) {
                vol = base_entry.volume;
                return apply_relative_path("", name + 1, resolved, error);
            }
            return resolve(d, b, name, vol, resolved, error);
        }
        vol = base_entry.volume;
        return apply_relative_path(base_path, name, resolved, error);
    }

    static void update_renamed_paths(Dos& d, Bus& b, u32 renamed_node) {
        auto update_lock_slot = [&](Dos::LockSlot& l) {
            if (l.used && l.entry.volume == Dos::kVolRam && l.entry.header_block > 0) {
                const u32 node = l.entry.header_block - 1;
                if (ram_is_at_or_below(d.ram_, node, renamed_node)) {
                    ram_node_path(d.ram_, node, l.path);
                    if (node == renamed_node) {
                        copy_cstr(l.entry.name, d.ram_.node(node).name, sizeof l.entry.name);
                    }
                }
            }
        };
        for (Dos::LockSlot& l : d.lock_slots_) {
            update_lock_slot(l);
        }
        for (Dos::LockSlot& l : d.assign_lock_slots_) {
            update_lock_slot(l);
        }
        for (u32 i = kConsoleOut + 1; i < kHandles; ++i) {
            Dos::FileSlot& f = d.files_[i];
            if (f.stream == Dos::Stream::File && f.entry.volume == Dos::kVolRam &&
                f.entry.header_block > 0) {
                const u32 node = f.entry.header_block - 1;
                if (ram_is_at_or_below(d.ram_, node, renamed_node)) {
                    ram_node_path(d.ram_, node, f.path);
                    if (node == renamed_node) {
                        copy_cstr(f.entry.name, d.ram_.node(node).name, sizeof f.entry.name);
                    }
                }
            }
        }
        for (Dos::AnchorSlot& a : d.anchors_) {
            if (!a.used) {
                continue;
            }
            for (u32 i = 0; i < a.depth && i < Dos::kMaxAnchorDepth; ++i) {
                Dos::AnchorLevel& lvl = a.levels[i];
                if (lvl.dir.volume == Dos::kVolRam && lvl.dir.header_block > 0) {
                    const u32 node = lvl.dir.header_block - 1;
                    if (ram_is_at_or_below(d.ram_, node, renamed_node)) {
                        ram_node_path(d.ram_, node, lvl.dir_path);
                        if (node == renamed_node) {
                            copy_cstr(lvl.dir.name, d.ram_.node(node).name, sizeof lvl.dir.name);
                            copy_cstr(lvl.dir_label, d.ram_.node(node).name, sizeof lvl.dir_label);
                        }
                    }
                }
            }
        }
        const u32 cur = b.r32(d.process_ + pr_CurrentDir);
        if (b.ok() && cur != 0 && d.cli_ != 0) {
            const Dos::LockSlot* cur_slot = find_lock_slot(d, cur);
            if (cur_slot != nullptr && cur_slot->entry.volume == Dos::kVolRam &&
                cur_slot->entry.header_block > 0 &&
                ram_is_at_or_below(d.ram_, cur_slot->entry.header_block - 1, renamed_node)) {
                char full_name[kMaxName];
                format_volume_path(volume_name(d, Dos::kVolRam), cur_slot->path, full_name,
                                   sizeof full_name);
                put_bstr(b, d.set_name_, full_name, 255);
                b.w32(d.cli_ + cli_SetName, d.set_name_ >> 2);
            }
        }
    }

    static bool delete_file_impl(Dos& d, Bus& b, u32 base_lock, const char* name) {
        u8 vol = Dos::kVolAdf;
        char resolved[Dos::kMaxPath];
        u32 error = 0;
        if (!resolve_from_lock(d, b, base_lock, name, vol, resolved, error)) {
            if (b.ok()) {
                fail(d, b, error, DOSFALSE);
            }
            return false;
        }
        if (vol != Dos::kVolRam) {
            fail(d, b, ERROR_DISK_WRITE_PROTECTED, DOSFALSE);
            return false;
        }
        if (resolved[0] == '\0') {
            fail(d, b, ERROR_OBJECT_WRONG_TYPE, DOSFALSE);
            return false;
        }
        u32 node = RamFs::kNone;
        const u32 find_err = d.ram_.find_path(resolved, node);
        if (find_err != 0) {
            fail(d, b, find_err, DOSFALSE);
            return false;
        }
        if (node == 0) {
            fail(d, b, ERROR_OBJECT_WRONG_TYPE, DOSFALSE);
            return false;
        }
        const u32 err = d.ram_.remove(node);
        if (err != 0) {
            fail(d, b, err, DOSFALSE);
            return false;
        }
        native::set_result(b.machine(), DOSTRUE);
        return true;
    }

    static void delete_file(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        if (m.cpu.d[1] == 0) {
            fail(d, b, ERROR_REQUIRED_ARG_MISSING, DOSFALSE);
            return;
        }
        char name[kMaxName];
        if (!read_name(d, b, m.cpu.d[1], name)) {
            if (b.ok()) {
                native::set_result(m, DOSFALSE);
            }
            return;
        }
        const u32 cur = b.r32(d.process_ + pr_CurrentDir);
        if (b.ok()) {
            delete_file_impl(d, b, cur, name);
        }
    }

    static bool rename_impl(Dos& d, Bus& b, u32 old_lock, const char* old_name, u32 new_lock,
                            const char* new_name) {
        u8 old_vol = Dos::kVolAdf;
        char old_resolved[Dos::kMaxPath];
        u32 error = 0;
        if (!resolve_from_lock(d, b, old_lock, old_name, old_vol, old_resolved, error)) {
            if (b.ok()) {
                fail(d, b, error, DOSFALSE);
            }
            return false;
        }
        u8 new_vol = Dos::kVolAdf;
        char new_resolved[Dos::kMaxPath];
        if (!resolve_from_lock(d, b, new_lock, new_name, new_vol, new_resolved, error)) {
            if (b.ok()) {
                fail(d, b, error, DOSFALSE);
            }
            return false;
        }
        if (old_vol != new_vol) {
            fail(d, b, ERROR_RENAME_ACROSS_DEVICES, DOSFALSE);
            return false;
        }
        if (old_vol != Dos::kVolRam) {
            fail(d, b, ERROR_DISK_WRITE_PROTECTED, DOSFALSE);
            return false;
        }
        u32 old_node = RamFs::kNone;
        const u32 find_err = d.ram_.find_path(old_resolved, old_node);
        if (find_err != 0) {
            fail(d, b, find_err, DOSFALSE);
            return false;
        }
        if (old_node == 0) {
            fail(d, b, ERROR_OBJECT_IN_USE, DOSFALSE);
            return false;
        }
        if (new_resolved[0] == '\0') {
            fail(d, b, ERROR_OBJECT_EXISTS, DOSFALSE);
            return false;
        }
        char new_parent_path[Dos::kMaxPath];
        const char* new_leaf = nullptr;
        split_parent_and_leaf(new_resolved, new_parent_path, new_leaf);
        u32 new_parent = RamFs::kNone;
        const u32 p_err = d.ram_.find_path(new_parent_path, new_parent);
        if (p_err != 0) {
            fail(d, b, p_err == ERROR_OBJECT_NOT_FOUND ? ERROR_DIR_NOT_FOUND : p_err, DOSFALSE);
            return false;
        }
        if (!d.ram_.node(new_parent).is_dir) {
            fail(d, b, ERROR_DIR_NOT_FOUND, DOSFALSE);
            return false;
        }
        const u32 err = d.ram_.rename(old_node, new_parent, new_leaf);
        if (err != 0) {
            fail(d, b, err, DOSFALSE);
            return false;
        }
        update_renamed_paths(d, b, old_node);
        if (b.ok()) {
            native::set_result(b.machine(), DOSTRUE);
            return true;
        }
        return false;
    }

    static void rename_lvo(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        if (m.cpu.d[1] == 0 || m.cpu.d[2] == 0) {
            fail(d, b, ERROR_REQUIRED_ARG_MISSING, DOSFALSE);
            return;
        }
        char old_name[kMaxName];
        if (!read_name(d, b, m.cpu.d[1], old_name)) {
            if (b.ok()) {
                native::set_result(m, DOSFALSE);
            }
            return;
        }
        char new_name[kMaxName];
        if (!read_name(d, b, m.cpu.d[2], new_name)) {
            if (b.ok()) {
                native::set_result(m, DOSFALSE);
            }
            return;
        }
        const u32 cur = b.r32(d.process_ + pr_CurrentDir);
        if (b.ok()) {
            rename_impl(d, b, cur, old_name, cur, new_name);
        }
    }

    static bool create_dir_impl(Dos& d, Bus& b, u32 base_lock, const char* name) {
        u8 vol = Dos::kVolAdf;
        char resolved[Dos::kMaxPath];
        u32 error = 0;
        if (!resolve_from_lock(d, b, base_lock, name, vol, resolved, error)) {
            if (b.ok()) {
                fail(d, b, error, 0);
            }
            return false;
        }
        if (vol != Dos::kVolRam) {
            fail(d, b, ERROR_DISK_WRITE_PROTECTED, 0);
            return false;
        }
        if (resolved[0] == '\0') {
            fail(d, b, ERROR_OBJECT_EXISTS, 0);
            return false;
        }
        char parent_path[Dos::kMaxPath];
        const char* leaf = nullptr;
        split_parent_and_leaf(resolved, parent_path, leaf);
        u32 parent_node = RamFs::kNone;
        const u32 p_err = d.ram_.find_path(parent_path, parent_node);
        if (p_err != 0) {
            fail(d, b, p_err == ERROR_OBJECT_NOT_FOUND ? ERROR_DIR_NOT_FOUND : p_err, 0);
            return false;
        }
        if (!d.ram_.node(parent_node).is_dir) {
            fail(d, b, ERROR_DIR_NOT_FOUND, 0);
            return false;
        }
        if (free_lock(d) < 0) {
            table_full(d, b, "lock", Dos::kMaxLocks);
            return false;
        }
        u32 new_node = RamFs::kNone;
        const u32 err = d.ram_.create_dir(parent_node, leaf, new_node);
        if (err != 0) {
            fail(d, b, err, 0);
            return false;
        }
        const DirEntry entry = ram_entry(d, new_node);
        char canonical[Dos::kMaxPath];
        ram_node_path(d.ram_, new_node, canonical);
        u32 bptr = 0;
        if (!alloc_user_lock(d, b, canonical, entry, EXCLUSIVE_LOCK, bptr)) {
            return false;
        }
        native::set_result(b.machine(), bptr);
        return true;
    }

    static void create_dir(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        if (m.cpu.d[1] == 0) {
            fail(d, b, ERROR_REQUIRED_ARG_MISSING, 0);
            return;
        }
        char name[kMaxName];
        if (!read_name(d, b, m.cpu.d[1], name)) {
            if (b.ok()) {
                native::set_result(m, 0);
            }
            return;
        }
        const u32 cur = b.r32(d.process_ + pr_CurrentDir);
        if (b.ok()) {
            create_dir_impl(d, b, cur, name);
        }
    }

    static bool set_protection_impl(Dos& d, Bus& b, u32 base_lock, const char* name, u32 mask) {
        u8 vol = Dos::kVolAdf;
        char resolved[Dos::kMaxPath];
        u32 error = 0;
        if (!resolve_from_lock(d, b, base_lock, name, vol, resolved, error)) {
            if (b.ok()) {
                fail(d, b, error, DOSFALSE);
            }
            return false;
        }
        if (vol != Dos::kVolRam) {
            fail(d, b, ERROR_DISK_WRITE_PROTECTED, DOSFALSE);
            return false;
        }
        u32 node = RamFs::kNone;
        const u32 find_err = d.ram_.find_path(resolved, node);
        if (find_err != 0) {
            fail(d, b, find_err, DOSFALSE);
            return false;
        }
        const u32 err = d.ram_.set_protection(node, mask);
        if (err != 0) {
            fail(d, b, err, DOSFALSE);
            return false;
        }
        native::set_result(b.machine(), DOSTRUE);
        return true;
    }

    static void set_protection(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        if (m.cpu.d[1] == 0) {
            fail(d, b, ERROR_REQUIRED_ARG_MISSING, DOSFALSE);
            return;
        }
        char name[kMaxName];
        if (!read_name(d, b, m.cpu.d[1], name)) {
            if (b.ok()) {
                native::set_result(m, DOSFALSE);
            }
            return;
        }
        const u32 cur = b.r32(d.process_ + pr_CurrentDir);
        if (b.ok()) {
            set_protection_impl(d, b, cur, name, m.cpu.d[2]);
        }
    }

    static bool set_comment_impl(Dos& d, Bus& b, u32 base_lock, const char* name,
                                 const char* comment) {
        u8 vol = Dos::kVolAdf;
        char resolved[Dos::kMaxPath];
        u32 error = 0;
        if (!resolve_from_lock(d, b, base_lock, name, vol, resolved, error)) {
            if (b.ok()) {
                fail(d, b, error, DOSFALSE);
            }
            return false;
        }
        if (vol != Dos::kVolRam) {
            fail(d, b, ERROR_DISK_WRITE_PROTECTED, DOSFALSE);
            return false;
        }
        u32 node = RamFs::kNone;
        const u32 find_err = d.ram_.find_path(resolved, node);
        if (find_err != 0) {
            fail(d, b, find_err, DOSFALSE);
            return false;
        }
        const u32 err = d.ram_.set_comment(node, comment);
        if (err != 0) {
            fail(d, b, err, DOSFALSE);
            return false;
        }
        native::set_result(b.machine(), DOSTRUE);
        return true;
    }

    static void set_comment(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        if (m.cpu.d[1] == 0) {
            fail(d, b, ERROR_REQUIRED_ARG_MISSING, DOSFALSE);
            return;
        }
        char name[kMaxName];
        if (!read_name(d, b, m.cpu.d[1], name)) {
            if (b.ok()) {
                native::set_result(m, DOSFALSE);
            }
            return;
        }
        char comment[RamFs::kMaxComment + 2]{};
        if (m.cpu.d[2] != 0 && !b.string(m.cpu.d[2], comment, sizeof comment)) {
            if (b.ok()) {
                fail(d, b, ERROR_COMMENT_TOO_BIG, DOSFALSE);
            }
            return;
        }
        const u32 cur = b.r32(d.process_ + pr_CurrentDir);
        if (b.ok()) {
            set_comment_impl(d, b, cur, name, comment);
        }
    }

    static bool set_file_date_impl(Dos& d, Bus& b, u32 base_lock, const char* name, u32 ds_addr) {
        if (ds_addr == 0) {
            fail(d, b, ERROR_REQUIRED_ARG_MISSING, DOSFALSE);
            return false;
        }
        const u32 days = b.r32(ds_addr + ds_Days);
        const u32 minute = b.r32(ds_addr + ds_Minute);
        const u32 tick = b.r32(ds_addr + ds_Tick);
        if (!b.ok()) {
            return false;
        }
        u8 vol = Dos::kVolAdf;
        char resolved[Dos::kMaxPath];
        u32 error = 0;
        if (!resolve_from_lock(d, b, base_lock, name, vol, resolved, error)) {
            if (b.ok()) {
                fail(d, b, error, DOSFALSE);
            }
            return false;
        }
        if (vol != Dos::kVolRam) {
            fail(d, b, ERROR_DISK_WRITE_PROTECTED, DOSFALSE);
            return false;
        }
        u32 node = RamFs::kNone;
        const u32 find_err = d.ram_.find_path(resolved, node);
        if (find_err != 0) {
            fail(d, b, find_err, DOSFALSE);
            return false;
        }
        const u32 err = d.ram_.set_date(node, days, minute, tick);
        if (err != 0) {
            fail(d, b, err, DOSFALSE);
            return false;
        }
        native::set_result(b.machine(), DOSTRUE);
        return true;
    }

    static void set_file_date(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        if (m.cpu.d[1] == 0) {
            fail(d, b, ERROR_REQUIRED_ARG_MISSING, DOSFALSE);
            return;
        }
        char name[kMaxName];
        if (!read_name(d, b, m.cpu.d[1], name)) {
            if (b.ok()) {
                native::set_result(m, DOSFALSE);
            }
            return;
        }
        const u32 cur = b.r32(d.process_ + pr_CurrentDir);
        if (b.ok()) {
            set_file_date_impl(d, b, cur, name, m.cpu.d[2]);
        }
    }

    static void pkt_locate_object(Dos& d, Bus& b, u32 lock_bptr, u32 name_bstr, u32 mode) {
        char name[kMaxName];
        if (!read_bstr_arg(d, b, name_bstr, name, kMaxName - 1, ERROR_INVALID_COMPONENT_NAME, 0,
                           false)) {
            return;
        }
        u8 vol = Dos::kVolAdf;
        char resolved[Dos::kMaxPath];
        u32 error = 0;
        if (!resolve_from_lock(d, b, lock_bptr, name, vol, resolved, error)) {
            if (b.ok()) {
                fail(d, b, error, 0);
            }
            return;
        }
        DirEntry entry{};
        const u32 err = fs_find(d, vol, resolved, entry);
        if (err != 0) {
            fail(d, b, err, 0);
            return;
        }
        u32 out_bptr = 0;
        if (alloc_user_lock(d, b, resolved, entry, mode, out_bptr)) {
            set_io_err(d, b, 0);
            native::set_result(b.machine(), out_bptr);
        }
    }

    static void do_pkt(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 port = m.cpu.d[1];
        const u32 action = m.cpu.d[2];
        const u32 arg1 = m.cpu.d[3];
        const u32 arg2 = m.cpu.d[4];
        const u32 arg3 = m.cpu.d[5];
        const u32 arg4 = m.cpu.d[6];
        if (port == 0 || native::guest_address(m, port) != d.port_) {
            fail(d, b, ERROR_ACTION_NOT_KNOWN, DOSFALSE);
            return;
        }
        switch (action) {
        case ACTION_DISK_INFO:
            if (fill_info_data(d, b, 0, arg1 << 2)) {
                set_io_err(d, b, 0);
            }
            return;
        case ACTION_INFO:
            if (fill_info_data(d, b, arg1, arg2 << 2)) {
                set_io_err(d, b, 0);
            }
            return;
        case ACTION_IS_FILESYSTEM:
            set_io_err(d, b, 0);
            native::set_result(m, DOSTRUE);
            return;
        case ACTION_CURRENT_VOLUME: {
            const u32 current = b.r32(d.process_ + pr_CurrentDir);
            if (!b.ok()) {
                return;
            }
            u8 vol = Dos::kVolAdf;
            if (current != 0) {
                const Dos::LockSlot* slot = find_lock_slot(d, current);
                if (slot != nullptr) {
                    vol = slot->entry.volume;
                }
            }
            set_io_err(d, b, 0);
            native::set_result(m, volume_node(d, vol) >> 2);
            return;
        }
        case ACTION_SAME_LOCK:
            if (same_lock_impl(d, b, arg1, arg2)) {
                set_io_err(d, b, 0);
            }
            return;
        case ACTION_LOCATE_OBJECT:
            pkt_locate_object(d, b, arg1, arg2, arg3);
            return;
        case ACTION_FREE_LOCK:
            if (free_user_lock(d, arg1)) {
                set_io_err(d, b, 0);
                native::set_result(m, DOSTRUE);
            } else {
                fail(d, b, ERROR_INVALID_LOCK, DOSFALSE);
            }
            return;
        case ACTION_DELETE_OBJECT: {
            char name[kMaxName];
            if (read_bstr_arg(d, b, arg2, name, kMaxName - 1, ERROR_INVALID_COMPONENT_NAME,
                              DOSFALSE, false) &&
                delete_file_impl(d, b, arg1, name)) {
                set_io_err(d, b, 0);
            }
            return;
        }
        case ACTION_RENAME_OBJECT: {
            char old_name[kMaxName];
            char new_name[kMaxName];
            if (read_bstr_arg(d, b, arg2, old_name, kMaxName - 1, ERROR_INVALID_COMPONENT_NAME,
                              DOSFALSE, false) &&
                read_bstr_arg(d, b, arg4, new_name, kMaxName - 1, ERROR_INVALID_COMPONENT_NAME,
                              DOSFALSE, false) &&
                rename_impl(d, b, arg1, old_name, arg3, new_name)) {
                set_io_err(d, b, 0);
            }
            return;
        }
        case ACTION_CREATE_DIR: {
            char name[kMaxName];
            if (read_bstr_arg(d, b, arg2, name, kMaxName - 1, ERROR_INVALID_COMPONENT_NAME, 0,
                              false) &&
                create_dir_impl(d, b, arg1, name)) {
                set_io_err(d, b, 0);
            }
            return;
        }
        case ACTION_SET_PROTECT: {
            char name[kMaxName];
            if (read_bstr_arg(d, b, arg3, name, kMaxName - 1, ERROR_INVALID_COMPONENT_NAME,
                              DOSFALSE, false) &&
                set_protection_impl(d, b, arg2, name, arg4)) {
                set_io_err(d, b, 0);
            }
            return;
        }
        case ACTION_SET_COMMENT: {
            char name[kMaxName];
            char comment[RamFs::kMaxComment + 1];
            if (read_bstr_arg(d, b, arg3, name, kMaxName - 1, ERROR_INVALID_COMPONENT_NAME,
                              DOSFALSE, false) &&
                read_bstr_arg(d, b, arg4, comment, RamFs::kMaxComment, ERROR_COMMENT_TOO_BIG,
                              DOSFALSE, true) &&
                set_comment_impl(d, b, arg2, name, comment)) {
                set_io_err(d, b, 0);
            }
            return;
        }
        case ACTION_SET_DATE: {
            char name[kMaxName];
            if (read_bstr_arg(d, b, arg3, name, kMaxName - 1, ERROR_INVALID_COMPONENT_NAME,
                              DOSFALSE, false) &&
                set_file_date_impl(d, b, arg2, name, arg4)) {
                set_io_err(d, b, 0);
            }
            return;
        }
        case ACTION_COPY_DIR:
            if (dup_lock_impl(d, b, arg1)) {
                set_io_err(d, b, 0);
            }
            return;
        case ACTION_PARENT:
            if (parent_dir_impl(d, b, arg1)) {
                set_io_err(d, b, 0);
            }
            return;
        case ACTION_EXAMINE_OBJECT:
            if (examine_impl(d, b, arg1, arg2 << 2)) {
                set_io_err(d, b, 0);
            }
            return;
        case ACTION_EXAMINE_NEXT:
            if (ex_next_impl(d, b, arg1, arg2 << 2)) {
                set_io_err(d, b, 0);
            }
            return;
        default:
            log_unknown_pkt(d, action);
            fail(d, b, ERROR_ACTION_NOT_KNOWN, DOSFALSE);
            return;
        }
    }

    // --- loading ---------------------------------------------------------------

    static void unload(Dos& d, u32 seglist) {
        const GuestMemory& mem = d.m_->mem;
        u32 bptr = seglist;
        for (u32 n = 0; bptr != 0; ++n) {
            if (n == kMaxHunks) {
                LogLine().text("dos: unloaded only the first ").dec(kMaxHunks)
                    .text(" segments of seglist ").hex(seglist).send();
                break;
            }
            // The seglist may come from the guest: the link goes through
            // the address mask as the 68000's would.
            const u64 link = native::guest_address(*d.m_, bptr << 2);
            u32 size = 0;
            u32 next = 0;
            if (link < 4 || link + 4 > mem.size || !mem.read32(static_cast<u32>(link - 4), size) ||
                !mem.read32(static_cast<u32>(link), next)) {
                break;
            }
            d.exec_->free(static_cast<u32>(link - 4), size);
            bptr = next;
        }
        d.m_->jit->flush();
    }

    static void load_seg_lvo(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        char name[kMaxName];
        if (!read_name(d, b, m.cpu.d[1], name)) {
            native::set_result(m, 0);
            return;
        }
        const u32 seglist = d.load_seg(name);
        native::set_result(m, seglist);
    }

    static void unload_seg(void* ctx, Machine& m) {
        unload(of(ctx), m.cpu.d[1]);
        native::set_result(m, DOSTRUE);
    }

    static bool load_segment(void* ctx, const char* path, u32& seglist) {
        seglist = of(ctx).load_seg(path);
        return seglist != 0;
    }

    // --- ReadArgs --------------------------------------------------------------

    // The next line of the console's input, its newline included; false if
    // it is longer than a line can be (it is read all the same).
    static bool console_line(Dos& d, ArgLine& line) {
        if (d.input_pos_ >= d.input_length_) {
            refill_console(d);
        }
        u32 n = 0;
        bool fits = true;
        while (d.input_pos_ < d.input_length_) {
            const char c = d.input_[d.input_pos_++];
            if (n < kMaxLine) {
                line.text[n++] = c;
            } else {
                fits = false;
            }
            if (c == '\n') {
                break;
            }
        }
        if (n == 0 || line.text[n - 1] != '\n') {
            if (n == kMaxLine) {
                fits = false;
            } else {
                line.text[n++] = '\n';
            }
        }
        line.length = n;
        return fits;
    }

    static bool is_question(const ArgLine& line) {
        u32 n = line.length;
        while (n > 0 && (line.text[n - 1] == '\n' || line.text[n - 1] == ' ' ||
                         line.text[n - 1] == '\t')) {
            --n;
        }
        u32 i = 0;
        while (i < n && (line.text[i] == ' ' || line.text[i] == '\t')) {
            ++i;
        }
        return n == i + 1 && line.text[i] == '?';
    }

    static i64 free_arg_block(const Dos& d) {
        for (u32 i = 0; i < Dos::kMaxArgBlocks; ++i) {
            if (d.arg_blocks_[i].block == 0) {
                return i;
            }
        }
        return -1;
    }

    // ReadArgs(template, array, rdargs). The line is the RDArgs' CSource
    // when it has a buffer, otherwise the next line of the console. The
    // strings, numbers and /M lists go into one block from exec that
    // FreeArgs gives back, with the RDArgs if ReadArgs made it.
    static void read_args(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 tmpl_addr = m.cpu.d[1];
        const u32 array = m.cpu.d[2];
        u32 rdargs = m.cpu.d[3];

        ArgResult r;
        r.items = 1;
        u32 tl = 0;
        for (;; ++tl) {
            if (tl == kMaxTemplate) {
                fail(d, b, ERROR_BAD_TEMPLATE, 0);
                return;
            }
            const u32 c = b.r8(tmpl_addr + tl);
            if (!b.ok()) {
                return;
            }
            r.tmpl[tl] = static_cast<char>(c);
            if (c == 0) {
                break;
            }
            r.items += c == ',' ? 1 : 0;
        }
        if (r.items > kMaxItems) {
            fail(d, b, ERROR_BAD_TEMPLATE, 0);
            return;
        }
        for (u8& f : r.flags) {
            f = 0;
        }
        for (u32 i = 0, item = 0; i < tl; ++i) {
            if (r.tmpl[i] == ',') {
                ++item;
            } else if (r.tmpl[i] == '/') {
                r.flags[item] |= modifier_flags(r.tmpl[i + 1]);  // tmpl[tl] is the NUL
            }
        }

        ArgLine line;
        line.pos = 0;
        line.used = 0;
        u32 source = 0;  // the CSource's buffer, 0 for the console
        u32 source_start = 0;
        u32 source_length = 0;
        if (rdargs != 0) {
            source = b.r32(rdargs + RDA_Source + CS_Buffer);
            const u32 length = b.r32(rdargs + RDA_Source + CS_Length);
            const u32 cur = b.r32(rdargs + RDA_Source + CS_CurChr);
            if (!b.ok()) {
                return;
            }
            if (source != 0) {
                source_start = cur;
                source_length = (length & 0x80000000) == 0 && cur < length ? length - cur : 0;
                if (source_length > kMaxLine) {
                    fail(d, b, ERROR_LINE_TOO_LONG, 0);
                    return;
                }
                for (u32 i = 0; i < source_length; ++i) {
                    line.text[i] = static_cast<char>(b.r8(source + cur + i));
                }
                if (!b.ok()) {
                    return;
                }
                line.text[source_length] = '\n';
                line.length = source_length + 1;
            }
        }
        if (source == 0) {
            bool fits = console_line(d, line);
            // A lone '?' asks for the template, and then for the line.
            for (u32 n = 0; fits && n < kMaxPrompts && is_question(line); ++n) {
                const u32 fh = b.r32(d.process_ + pr_COS);
                if (!b.ok()) {
                    return;
                }
                const i64 slot = handle_slot(d, fh);
                if (slot >= 0) {
                    Sink out(d, b, static_cast<u32>(slot));
                    out.text(r.tmpl);
                    out.text(": ");
                    out.flush();
                }
                fits = console_line(d, line);
            }
            if (!fits) {
                fail(d, b, ERROR_LINE_TOO_LONG, 0);
                return;
            }
        }

        const u32 error = parse_args(line, r);
        if (source != 0) {
            const u32 read = line.pos < source_length ? line.pos : source_length;
            b.w32(rdargs + RDA_Source + CS_CurChr, source_start + read);
        }
        if (!b.ok()) {
            return;
        }
        if (error != 0) {
            fail(d, b, error, 0);
            return;
        }

        // Numbers are checked before anything is allocated.
        u32 numbers = 0;
        for (u32 arg = 0; arg < r.items; ++arg) {
            if ((r.flags[arg] & kTypeMask) != kNumeric) {
                continue;
            }
            u32 v = 0;
            if (static_cast<i64>(arg) == r.multi_item) {
                for (u32 k = 0; k < r.multi_count; ++k) {
                    if (!parse_number(line.pool + r.multi[k], v)) {
                        fail(d, b, ERROR_BAD_NUMBER, 0);
                        return;
                    }
                }
                numbers += r.multi_count;
            } else if (r.value[arg] >= 0) {
                if (!parse_number(line.pool + r.value[arg], v)) {
                    fail(d, b, ERROR_BAD_NUMBER, 0);
                    return;
                }
                ++numbers;
            }
        }

        const i64 table = free_arg_block(d);
        if (table < 0) {
            table_full(d, b, "ReadArgs result", Dos::kMaxArgBlocks);
            return;
        }
        const bool make_rdargs = rdargs == 0;
        if (make_rdargs) {
            rdargs = d.exec_->alloc(RDA_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
            if (rdargs == 0) {
                fail(d, b, ERROR_NO_FREE_STORE, 0);
                return;
            }
        }
        const bool lists = r.multi_item >= 0 && r.multi_count != 0;
        const u32 strings = align4(line.used);
        const u32 list_bytes = lists ? (r.multi_count + 1) * 4 : 0;
        u32 bytes = strings + numbers * 4 + list_bytes;
        bytes = bytes < 8 ? 8 : bytes;
        const u32 block = d.exec_->alloc(bytes, MEMF_PUBLIC | MEMF_CLEAR);
        if (block == 0) {
            if (make_rdargs) {
                d.exec_->free(rdargs, RDA_SIZEOF);
            }
            fail(d, b, ERROR_NO_FREE_STORE, 0);
            return;
        }
        // exec handed the block out, so it is guest memory.
        for (u32 i = 0; i < line.used; ++i) {
            m.mem.base[block + i] = static_cast<u8>(line.pool[i]);
        }
        u32 number_at = block + strings;
        const u32 list_at = block + strings + numbers * 4;

        for (u32 arg = 0; arg < r.items && b.ok(); ++arg) {
            const u8 type = r.flags[arg] & kTypeMask;
            if (static_cast<i64>(arg) == r.multi_item) {
                if (!lists) {
                    b.w32(array + arg * 4, 0);
                    continue;
                }
                for (u32 k = 0; k < r.multi_count; ++k) {
                    u32 item = block + r.multi[k];
                    if (type == kNumeric) {
                        u32 v = 0;
                        parse_number(line.pool + r.multi[k], v);
                        b.w32(number_at, v);
                        item = number_at;
                        number_at += 4;
                    }
                    b.w32(list_at + k * 4, item);
                }
                b.w32(list_at + r.multi_count * 4, 0);
                b.w32(array + arg * 4, list_at);
            } else if (type == kSwitch && r.set[arg]) {
                b.w32(array + arg * 4, DOSTRUE);
            } else if (type == kToggle && r.set[arg]) {
                b.w32(array + arg * 4, b.r32(array + arg * 4) != 0 ? 0 : DOSTRUE);
            } else if (type == kNumeric && r.value[arg] >= 0) {
                u32 v = 0;
                parse_number(line.pool + r.value[arg], v);
                b.w32(number_at, v);
                b.w32(array + arg * 4, number_at);
                number_at += 4;
            } else if (r.value[arg] >= 0) {
                b.w32(array + arg * 4, block + static_cast<u32>(r.value[arg]));
            }
        }
        if (!b.ok()) {
            d.exec_->free(block, bytes);
            if (make_rdargs) {
                d.exec_->free(rdargs, RDA_SIZEOF);
            }
            return;
        }
        d.arg_blocks_[table] = Dos::ArgBlock{rdargs, block, bytes, make_rdargs};
        native::set_result(m, rdargs);
    }

    static void free_args(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        const u32 rdargs = m.cpu.d[1];
        if (rdargs == 0) {
            return;
        }
        bool found = false;
        bool owned = false;
        for (Dos::ArgBlock& a : d.arg_blocks_) {
            if (a.block != 0 && a.rdargs == rdargs) {
                d.exec_->free(a.block, a.bytes);
                owned = owned || a.owns_rdargs;
                a = Dos::ArgBlock{0, 0, 0, false};
                found = true;
            }
        }
        if (!found) {
            LogLine().text("dos: FreeArgs of ").hex(rdargs)
                .text(", which ReadArgs did not return").send();
            return;
        }
        if (owned) {
            d.exec_->free(rdargs, RDA_SIZEOF);
        }
    }

    static void find_arg(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 tmpl_addr = m.cpu.d[1];
        const u32 word_addr = m.cpu.d[2];
        if (tmpl_addr == 0 || word_addr == 0) {
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        char tmpl[kMaxTemplate + 1];
        if (!b.string(tmpl_addr, tmpl, sizeof tmpl)) {
            if (b.ok()) {
                fail(d, b, ERROR_BAD_TEMPLATE, 0xFFFFFFFF);
            }
            return;
        }
        char word[kMaxName];
        if (!b.string(word_addr, word, sizeof word)) {
            if (b.ok()) {
                native::set_result(m, 0xFFFFFFFF);
            }
            return;
        }
        if (word[0] == '\0') {
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        native::set_result(m, static_cast<u32>(find_keyword(tmpl, word)));
    }

    struct ItemSource {
        Dos& d;
        Bus& b;
        bool use_cs;
        u32 cs_addr;
        u32 cs_buf;
        u32 cs_len;
        u32 cs_cur;
        u32 slot;

        i64 next() {
            if (use_cs) {
                if ((cs_len & 0x80000000) != 0 || cs_cur >= cs_len) {
                    return kEnd;
                }
                const u32 c = b.r8(cs_buf + cs_cur);
                if (!b.ok()) {
                    return kEnd;
                }
                ++cs_cur;
                return static_cast<i64>(c);
            }
            u8 c = 0;
            u32 got = 0;
            if (!read_from_slot(d, b, slot, &c, 1, got) || got == 0) {
                return kEnd;
            }
            return static_cast<i64>(c);
        }

        void unget(i64 c) {
            if (c == kEnd) {
                return;
            }
            if (use_cs) {
                if (cs_cur > 0) {
                    --cs_cur;
                }
                return;
            }
            Dos::FileSlot& f = d.files_[slot];
            f.unget = static_cast<u8>(c);
            f.has_unget = true;
            f.has_last = false;
        }

        void commit() {
            if (use_cs) {
                b.w32(cs_addr + CS_CurChr, cs_cur);
            }
        }
    };

    static void read_item(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 buf = m.cpu.d[1];
        const u32 maxchars = m.cpu.d[2];
        const u32 csource = m.cpu.d[3];
        if (buf == 0 || maxchars == 0 || (maxchars & 0x80000000) != 0) {
            fail(d, b, ERROR_LINE_TOO_LONG, ITEM_ERROR);
            return;
        }
        ItemSource src{d, b, false, csource, 0, 0, 0, 0};
        if (csource != 0) {
            src.cs_buf = b.r32(csource + CS_Buffer);
            src.cs_len = b.r32(csource + CS_Length);
            src.cs_cur = b.r32(csource + CS_CurChr);
            if (!b.ok()) {
                return;
            }
            src.use_cs = src.cs_buf != 0;
        }
        if (!src.use_cs) {
            const u32 fh = b.r32(d.process_ + pr_CIS);
            if (!b.ok()) {
                return;
            }
            const i64 slot = handle_slot(d, fh);
            if (slot < 0) {
                fail(d, b, ERROR_INVALID_LOCK, ITEM_ERROR);
                return;
            }
            src.slot = static_cast<u32>(slot);
        }
        i64 c = src.next();
        while (b.ok() && (c == ' ' || c == '\t')) {
            c = src.next();
        }
        if (!b.ok()) {
            return;
        }
        if (c == kEnd || c == '\n' || c == 0 || c == ';') {
            src.unget(c);
            b.w8(buf, 0);
            src.commit();
            if (b.ok()) {
                native::set_result(m, ITEM_NOTHING);
            }
            return;
        }
        if (c == '=') {
            b.w8(buf, 0);
            src.commit();
            if (b.ok()) {
                native::set_result(m, ITEM_EQUAL);
            }
            return;
        }
        u32 out = 0;
        if (c == '"') {
            for (;;) {
                c = src.next();
                if (!b.ok()) {
                    return;
                }
                if (c == '*') {
                    c = src.next();
                    if (!b.ok()) {
                        return;
                    }
                    if (c == kEnd || c == '\n' || c == 0) {
                        src.unget(c);
                        b.w8(buf + out, 0);
                        src.commit();
                        fail(d, b, ERROR_UNMATCHED_QUOTES, ITEM_ERROR);
                        return;
                    }
                    if (c == 'n' || c == 'N') {
                        c = '\n';
                    } else if (c == 'e' || c == 'E') {
                        c = 0x1B;
                    }
                } else if (c == kEnd || c == '\n' || c == 0) {
                    src.unget(c);
                    b.w8(buf + out, 0);
                    src.commit();
                    fail(d, b, ERROR_UNMATCHED_QUOTES, ITEM_ERROR);
                    return;
                } else if (c == '"') {
                    b.w8(buf + out, 0);
                    src.commit();
                    if (b.ok()) {
                        native::set_result(m, ITEM_QUOTED);
                    }
                    return;
                }
                if (out + 1 >= maxchars) {
                    b.w8(buf + out, 0);
                    src.commit();
                    fail(d, b, ERROR_LINE_TOO_LONG, ITEM_ERROR);
                    return;
                }
                b.w8(buf + out++, static_cast<u8>(c));
                if (!b.ok()) {
                    return;
                }
            }
        }
        for (;;) {
            if (out + 1 >= maxchars) {
                b.w8(buf + out, 0);
                src.commit();
                fail(d, b, ERROR_LINE_TOO_LONG, ITEM_ERROR);
                return;
            }
            b.w8(buf + out++, static_cast<u8>(c));
            if (!b.ok()) {
                return;
            }
            c = src.next();
            if (!b.ok()) {
                return;
            }
            if (c == kEnd || c == 0 || c == ' ' || c == '\t' || c == '\n' || c == '=') {
                if (c == 0 || c == '\n' || c == '=') {
                    src.unget(c);
                }
                b.w8(buf + out, 0);
                src.commit();
                if (b.ok()) {
                    native::set_result(m, ITEM_UNQUOTED);
                }
                return;
            }
        }
    }

    static void cleanup_command(Dos& d, u32 saved_current_dir, u32 saved_cis, u32 saved_cos) {
        d.m_->mem.write32(d.process_ + pr_CurrentDir, saved_current_dir);
        u32 cur_cis = 0;
        if (d.m_->mem.read32(d.process_ + pr_CIS, cur_cis)) {
            const i64 slot = handle_slot(d, cur_cis);
            if (slot > static_cast<i64>(kConsoleOut) && d.files_[slot].command_owned) {
                d.m_->mem.write32(d.process_ + pr_CIS, saved_cis);
            }
        }
        u32 cur_cos = 0;
        if (d.m_->mem.read32(d.process_ + pr_COS, cur_cos)) {
            const i64 slot = handle_slot(d, cur_cos);
            if (slot > static_cast<i64>(kConsoleOut) && d.files_[slot].command_owned) {
                d.m_->mem.write32(d.process_ + pr_COS, saved_cos);
            }
        }
        for (u32 i = 0; i < Dos::kMaxAnchors; ++i) {
            if (d.anchors_[i].used && d.anchors_[i].command_owned) {
                release_anchor_slot(d, i);
            }
        }
        for (u32 i = kConsoleOut + 1; i < kHandles; ++i) {
            if (d.files_[i].stream != Dos::Stream::Closed && d.files_[i].command_owned) {
                release_entry(d, d.files_[i].entry);
                d.files_[i] = make_file_slot(Dos::Stream::Closed, DirEntry{}, "");
            }
        }
        for (u32 i = 0; i < Dos::kMaxLocks; ++i) {
            if (d.lock_slots_[i].used && d.lock_slots_[i].command_owned) {
                release_entry(d, d.lock_slots_[i].entry);
                d.lock_slots_[i].used = false;
                d.lock_slots_[i].command_owned = false;
                d.lock_slots_[i].exclusive = false;
            }
        }
    }
};

namespace {

struct DosFunction {
    u32 lvo;
    m68k::NativeFn fn;
    const char* name;
};

constexpr DosFunction kDosFunctions[] = {
    {dos_lvo::Open, DosOps::open, "Open"},
    {dos_lvo::Close, DosOps::close, "Close"},
    {dos_lvo::Read, DosOps::read, "Read"},
    {dos_lvo::Write, DosOps::write, "Write"},
    {dos_lvo::Input, DosOps::input, "Input"},
    {dos_lvo::Output, DosOps::output, "Output"},
    {dos_lvo::Seek, DosOps::seek, "Seek"},
    {dos_lvo::DeleteFile, DosOps::delete_file, "DeleteFile"},
    {dos_lvo::Rename, DosOps::rename_lvo, "Rename"},
    {dos_lvo::Lock, DosOps::lock, "Lock"},
    {dos_lvo::UnLock, DosOps::unlock, "UnLock"},
    {dos_lvo::DupLock, DosOps::dup_lock, "DupLock"},
    {dos_lvo::Examine, DosOps::examine, "Examine"},
    {dos_lvo::ExNext, DosOps::ex_next, "ExNext"},
    {dos_lvo::Info, DosOps::info, "Info"},
    {dos_lvo::CreateDir, DosOps::create_dir, "CreateDir"},
    {dos_lvo::CurrentDir, DosOps::current_dir, "CurrentDir"},
    {dos_lvo::IoErr, DosOps::io_err, "IoErr"},
    {dos_lvo::Exit, DosOps::exit, "Exit"},
    {dos_lvo::LoadSeg, DosOps::load_seg_lvo, "LoadSeg"},
    {dos_lvo::UnLoadSeg, DosOps::unload_seg, "UnLoadSeg"},
    {dos_lvo::SetComment, DosOps::set_comment, "SetComment"},
    {dos_lvo::SetProtection, DosOps::set_protection, "SetProtection"},
    {dos_lvo::DateStamp, DosOps::date_stamp, "DateStamp"},
    {dos_lvo::Delay, DosOps::delay, "Delay"},
    {dos_lvo::WaitForChar, DosOps::wait_for_char, "WaitForChar"},
    {dos_lvo::ParentDir, DosOps::parent_dir, "ParentDir"},
    {dos_lvo::IsInteractive, DosOps::is_interactive, "IsInteractive"},
    {dos_lvo::AllocDosObject, DosOps::alloc_dos_object, "AllocDosObject"},
    {dos_lvo::FreeDosObject, DosOps::free_dos_object, "FreeDosObject"},
    {dos_lvo::DoPkt, DosOps::do_pkt, "DoPkt"},
    {dos_lvo::SelectInput, DosOps::select_input, "SelectInput"},
    {dos_lvo::SelectOutput, DosOps::select_output, "SelectOutput"},
    {dos_lvo::FGetC, DosOps::fget_c, "FGetC"},
    {dos_lvo::FPutC, DosOps::fput_c, "FPutC"},
    {dos_lvo::UnGetC, DosOps::unget_c, "UnGetC"},
    {dos_lvo::FRead, DosOps::fread, "FRead"},
    {dos_lvo::FWrite, DosOps::fwrite, "FWrite"},
    {dos_lvo::FGets, DosOps::fgets, "FGets"},
    {dos_lvo::FPuts, DosOps::fputs, "FPuts"},
    {dos_lvo::VFWritef, DosOps::vfwritef, "VFWritef"},
    {dos_lvo::VFPrintf, DosOps::vfprintf, "VFPrintf"},
    {dos_lvo::Flush, DosOps::flush, "Flush"},
    {dos_lvo::SetVBuf, DosOps::set_vbuf, "SetVBuf"},
    {dos_lvo::DupLockFromFH, DosOps::dup_lock_from_fh, "DupLockFromFH"},
    {dos_lvo::OpenFromLock, DosOps::open_from_lock, "OpenFromLock"},
    {dos_lvo::ParentOfFH, DosOps::parent_of_fh, "ParentOfFH"},
    {dos_lvo::ExamineFH, DosOps::examine_fh, "ExamineFH"},
    {dos_lvo::SetFileDate, DosOps::set_file_date, "SetFileDate"},
    {dos_lvo::NameFromLock, DosOps::name_from_lock, "NameFromLock"},
    {dos_lvo::NameFromFH, DosOps::name_from_fh, "NameFromFH"},
    {dos_lvo::SameLock, DosOps::same_lock, "SameLock"},
    {dos_lvo::SetMode, DosOps::set_mode, "SetMode"},
    {dos_lvo::ExAll, DosOps::ex_all, "ExAll"},
    {dos_lvo::SetFileSize, DosOps::set_file_size, "SetFileSize"},
    {dos_lvo::SetIoErr, DosOps::set_io_err_lvo, "SetIoErr"},
    {dos_lvo::Fault, DosOps::fault, "Fault"},
    {dos_lvo::PrintFault, DosOps::print_fault, "PrintFault"},
    {dos_lvo::ErrorReport, DosOps::error_report, "ErrorReport"},
    {dos_lvo::Cli, DosOps::cli, "Cli"},
    {dos_lvo::GetFileSysTask, DosOps::get_file_sys_task, "GetFileSysTask"},
    {dos_lvo::SetFileSysTask, DosOps::set_file_sys_task, "SetFileSysTask"},
    {dos_lvo::GetArgStr, DosOps::get_arg_str, "GetArgStr"},
    {dos_lvo::SetArgStr, DosOps::set_arg_str, "SetArgStr"},
    {dos_lvo::FindCliProc, DosOps::find_cli_proc, "FindCliProc"},
    {dos_lvo::MaxCli, DosOps::max_cli, "MaxCli"},
    {dos_lvo::SetCurrentDirName, DosOps::set_current_dir_name, "SetCurrentDirName"},
    {dos_lvo::GetCurrentDirName, DosOps::get_current_dir_name, "GetCurrentDirName"},
    {dos_lvo::SetProgramName, DosOps::set_program_name, "SetProgramName"},
    {dos_lvo::GetProgramName, DosOps::get_program_name, "GetProgramName"},
    {dos_lvo::SetPrompt, DosOps::set_prompt, "SetPrompt"},
    {dos_lvo::GetPrompt, DosOps::get_prompt, "GetPrompt"},
    {dos_lvo::AssignLock, DosOps::assign_lock, "AssignLock"},
    {dos_lvo::AssignLate, DosOps::assign_late, "AssignLate"},
    {dos_lvo::AssignPath, DosOps::assign_path, "AssignPath"},
    {dos_lvo::AssignAdd, DosOps::assign_add, "AssignAdd"},
    {dos_lvo::RemAssignList, DosOps::rem_assign_list, "RemAssignList"},
    {dos_lvo::GetDeviceProc, DosOps::get_device_proc, "GetDeviceProc"},
    {dos_lvo::FreeDeviceProc, DosOps::free_device_proc, "FreeDeviceProc"},
    {dos_lvo::LockDosList, DosOps::lock_dos_list, "LockDosList"},
    {dos_lvo::UnLockDosList, DosOps::unlock_dos_list, "UnLockDosList"},
    {dos_lvo::AttemptLockDosList, DosOps::lock_dos_list, "AttemptLockDosList"},
    {dos_lvo::FindDosEntry, DosOps::find_dos_entry, "FindDosEntry"},
    {dos_lvo::NextDosEntry, DosOps::next_dos_entry, "NextDosEntry"},
    {dos_lvo::IsFileSystem, DosOps::is_file_system, "IsFileSystem"},
    {dos_lvo::CompareDates, DosOps::compare_dates, "CompareDates"},
    {dos_lvo::DateToStr, DosOps::date_to_str, "DateToStr"},
    {dos_lvo::StrToDate, DosOps::str_to_date, "StrToDate"},
    {dos_lvo::NewLoadSeg, DosOps::load_seg_lvo, "NewLoadSeg"},
    {dos_lvo::FindSegment, DosOps::find_segment, "FindSegment"},
    {dos_lvo::CheckSignal, DosOps::check_signal, "CheckSignal"},
    {dos_lvo::ReadArgs, DosOps::read_args, "ReadArgs"},
    {dos_lvo::FindArg, DosOps::find_arg, "FindArg"},
    {dos_lvo::ReadItem, DosOps::read_item, "ReadItem"},
    {dos_lvo::StrToLong, DosOps::str_to_long, "StrToLong"},
    {dos_lvo::MatchFirst, DosOps::match_first, "MatchFirst"},
    {dos_lvo::MatchNext, DosOps::match_next, "MatchNext"},
    {dos_lvo::MatchEnd, DosOps::match_end, "MatchEnd"},
    {dos_lvo::ParsePattern, DosOps::parse_pattern, "ParsePattern"},
    {dos_lvo::MatchPattern, DosOps::match_pattern, "MatchPattern"},
    {dos_lvo::FreeArgs, DosOps::free_args, "FreeArgs"},
    {dos_lvo::FilePart, DosOps::file_part, "FilePart"},
    {dos_lvo::PathPart, DosOps::path_part, "PathPart"},
    {dos_lvo::AddPart, DosOps::add_part, "AddPart"},
    {dos_lvo::SetVar, DosOps::set_var, "SetVar"},
    {dos_lvo::GetVar, DosOps::get_var, "GetVar"},
    {dos_lvo::DeleteVar, DosOps::delete_var_lvo, "DeleteVar"},
    {dos_lvo::FindVar, DosOps::find_var_lvo, "FindVar"},
    {dos_lvo::WriteChars, DosOps::write_chars, "WriteChars"},
    {dos_lvo::PutStr, DosOps::put_str, "PutStr"},
    {dos_lvo::VPrintf, DosOps::vprintf, "VPrintf"},
    {dos_lvo::ParsePatternNoCase, DosOps::parse_pattern_nc, "ParsePatternNoCase"},
    {dos_lvo::MatchPatternNoCase, DosOps::match_pattern_nc, "MatchPatternNoCase"},
    {dos_lvo::ExAllEnd, DosOps::ex_all_end, "ExAllEnd"},
};

}

bool Dos::init(Machine& m, Exec& exec, const AdfVolume& volume, ConsoleWrite out, void* ctx) {
    m_ = nullptr;
    exec_ = &exec;
    volume_ = volume;
    ram_ = RamFs{};
    out_ = out;
    out_ctx_ = ctx;
    base_ = 0;
    process_ = 0;
    for (FileSlot& f : files_) {
        f = DosOps::make_file_slot(Stream::Closed, DirEntry{}, "");
    }
    for (LockSlot& l : lock_slots_) {
        l.used = false;
        l.command_owned = false;
    }
    for (LockSlot& l : assign_lock_slots_) {
        l.used = false;
        l.command_owned = false;
    }
    for (AssignEntry& a : assigns_) {
        a = AssignEntry{};
    }
    for (ArgBlock& a : arg_blocks_) {
        a = ArgBlock{0, 0, 0, false};
    }
    for (AnchorSlot& a : anchors_) {
        a = AnchorSlot{};
    }
    for (u32& act : unknown_pkt_actions_) {
        act = 0;
    }
    unknown_pkt_count_ = 0;
    now_ticks_ = nullptr;
    console_read_ = nullptr;
    console_ready_ = nullptr;
    console_ctx_ = nullptr;
    for (u32 i = 0; i < kMaxPathDirs; ++i) {
        path_locks_[i] = 0;
        path_nodes_[i] = 0;
    }
    path_count_ = 0;
    input_length_ = 0;
    input_pos_ = 0;
    running_ = false;
    exited_ = false;
    exit_code_ = 0;
    if (m.jit == nullptr || m.mem.base == nullptr || volume.image == nullptr) {
        return false;
    }

    block_ = exec.alloc(kBlockBytes, MEMF_PUBLIC | MEMF_CLEAR);
    if (block_ == 0) {
        return false;
    }
    m_ = &m;
    root_ = block_ + kRootAt;
    volume_node_ = block_ + kVolumeAt;
    ram_volume_node_ = block_ + kRamVolumeAt;
    device_node_ = block_ + kDeviceAt;
    ram_device_node_ = block_ + kRamDeviceAt;
    port_ = block_ + kPortAt;
    process_ = block_ + kProcessAt;
    cli_ = block_ + kCliAt;
    set_name_ = block_ + kSetNameAt;
    prompt_ = block_ + kPromptAt;
    command_name_ = block_ + kCommandNameAt;
    handles_ = block_ + kHandlesAt;
    locks_ = block_ + kLocksAt;
    assign_locks_ = block_ + kAssignLocksAt;
    args_ = block_ + kArgsAt;
    stack_ = block_ + kStackAt;

    for (NativeVector& v : vectors_) {
        v = NativeVector{nullptr, nullptr};
    }
    for (const DosFunction& f : kDosFunctions) {
        vectors_[f.lvo / LIB_VECTSIZE - 1] = NativeVector{f.fn, f.name};
    }
    base_ = exec.make_native_library(kDosName, kHombreLibVersion, vectors_, kVectors, this,
                                     dl_SIZEOF);
    if (base_ == 0) {
        exec.free(block_, kBlockBytes);
        m_ = nullptr;
        return false;
    }

    ram_.init(RamFs::Store{
        this,
        DosOps::ram_alloc,
        DosOps::ram_free,
        DosOps::ram_read,
        DosOps::ram_write,
        DosOps::ram_now,
    });

    Bus b(m);
    const u32 info = block_ + kInfoAt;
    const u32 tasks = block_ + kTaskArrayAt;
    put_string(b, block_ + kIdStringAt, kIdString);
    b.w16(base_ + LIB_REVISION, kHombreLibRevision);
    b.w32(base_ + LIB_IDSTRING, block_ + kIdStringAt);
    b.w32(base_ + dl_Root, root_);

    // RootNode, DosInfo and the volume's node.
    b.w32(root_ + rn_TaskArray, tasks >> 2);
    b.w32(root_ + rn_Info, info >> 2);
    new_list(b, root_ + rn_CliList);
    b.w32(tasks, 1);
    b.w32(tasks + 4, process_ + pr_MsgPort);
    b.w32(info + di_DevInfo, volume_node_ >> 2);
    init_semaphore(b, info + di_DevLock);
    init_semaphore(b, info + di_EntryLock);
    init_semaphore(b, info + di_DeleteLock);
    const u8* root_block = volume.image + static_cast<usize>(volume.root_block) * 512;
    b.w32(volume_node_ + dol_Type, DLT_VOLUME);
    b.w32(volume_node_ + dol_Task, port_);
    for (u32 i = 0; i < 3; ++i) {
        // The volume's creation date, longs 121 to 123 of the root block.
        b.w32(volume_node_ + dol_VolumeDate + i * 4, amiga_fs_detail::long_at(root_block, 121 + i));
    }
    b.w32(volume_node_ + dol_DiskType, ID_DOS_DISK | volume.image[3]);
    put_bstr(b, block_ + kVolumeNameAt, volume.name, 30);
    b.w32(volume_node_ + dol_Name, (block_ + kVolumeNameAt) >> 2);
    b.w32(ram_volume_node_ + dol_Type, DLT_VOLUME);
    b.w32(ram_volume_node_ + dol_Task, port_);
    const RamFs::Date& ram_date = ram_.node(0).date;
    b.w32(ram_volume_node_ + dol_VolumeDate + ds_Days, ram_date.days);
    b.w32(ram_volume_node_ + dol_VolumeDate + ds_Minute, ram_date.minute);
    b.w32(ram_volume_node_ + dol_VolumeDate + ds_Tick, ram_date.tick);
    b.w32(ram_volume_node_ + dol_DiskType, ID_DOS_DISK);
    put_bstr(b, block_ + kRamVolumeNameAt, "Ram Disk", 30);
    b.w32(ram_volume_node_ + dol_Name, (block_ + kRamVolumeNameAt) >> 2);
    b.w32(device_node_ + dol_Type, DLT_DEVICE);
    b.w32(device_node_ + dol_Task, port_);
    put_bstr(b, block_ + kDeviceNameAt, "DF0", 30);
    b.w32(device_node_ + dol_Name, (block_ + kDeviceNameAt) >> 2);
    b.w32(ram_device_node_ + dol_Type, DLT_DEVICE);
    b.w32(ram_device_node_ + dol_Task, port_);
    put_bstr(b, block_ + kRamDeviceNameAt, "RAM", 30);
    b.w32(ram_device_node_ + dol_Name, (block_ + kRamDeviceNameAt) >> 2);

    // The file system's port, which nothing sends to.
    b.w8(port_ + LN_TYPE, NT_MSGPORT);
    new_list(b, port_ + MP_MSGLIST);

    // The shell Process and its CLI.
    put_string(b, block_ + kProcessNameAt, kProcessName);
    b.w8(process_ + LN_TYPE, NT_PROCESS);
    b.w32(process_ + LN_NAME, block_ + kProcessNameAt);
    b.w8(process_ + TC_STATE, TS_RUN);
    b.w32(process_ + TC_SIGALLOC, 0xFFFF);
    new_list(b, process_ + TC_MEMENTRY);
    const u32 port = process_ + pr_MsgPort;
    b.w8(port + LN_TYPE, NT_MSGPORT);
    b.w8(port + MP_SIGBIT, SIGB_DOS);
    b.w32(port + MP_SIGTASK, process_);
    new_list(b, port + MP_MSGLIST);
    b.w32(process_ + pr_TaskNum, 1);
    b.w32(process_ + pr_StackSize, kStackBytes);
    b.w32(process_ + pr_StackBase, (stack_ + kStackBytes) >> 2);
    b.w32(process_ + TC_SPLOWER, stack_);
    b.w32(process_ + TC_SPUPPER, stack_ + kStackBytes);
    b.w32(process_ + pr_CIS, (handles_ + kConsoleIn * fh_SIZEOF) >> 2);
    b.w32(process_ + pr_COS, (handles_ + kConsoleOut * fh_SIZEOF) >> 2);
    b.w32(process_ + pr_FileSystemTask, port_);
    b.w32(process_ + pr_CLI, cli_ >> 2);
    new_list(b, process_ + pr_LocalVars);

    char set_name[32];
    u32 n = 0;
    for (; volume.name[n] != '\0' && n < 30; ++n) {
        set_name[n] = volume.name[n];
    }
    set_name[n++] = ':';
    set_name[n] = '\0';
    put_bstr(b, set_name_, set_name, 31);
    put_bstr(b, prompt_, kPrompt, sizeof kPrompt - 1);
    put_bstr(b, block_ + kCommandFileAt, "", 0);
    put_bstr(b, command_name_, "", 0);
    b.w32(cli_ + cli_SetName, set_name_ >> 2);
    b.w32(cli_ + cli_CommandName, command_name_ >> 2);
    b.w32(cli_ + cli_FailLevel, kFailLevel);
    b.w32(cli_ + cli_Prompt, prompt_ >> 2);
    b.w32(cli_ + cli_StandardInput, b.r32(process_ + pr_CIS));
    b.w32(cli_ + cli_CurrentInput, b.r32(process_ + pr_CIS));
    b.w32(cli_ + cli_CommandFile, (block_ + kCommandFileAt) >> 2);
    b.w32(cli_ + cli_Interactive, DOSTRUE);
    b.w32(cli_ + cli_CurrentOutput, b.r32(process_ + pr_COS));
    b.w32(cli_ + cli_DefaultStack, kStackBytes / 4);
    b.w32(cli_ + cli_StandardOutput, b.r32(process_ + pr_COS));

    // The console streams.
    files_[kConsoleIn] = DosOps::make_file_slot(Stream::Console, DirEntry{}, "");
    files_[kConsoleOut] = DosOps::make_file_slot(Stream::Console, DirEntry{}, "");
    DosOps::init_handle(*this, b, kConsoleIn, true);
    DosOps::init_handle(*this, b, kConsoleOut, true);

    // Default assigns for the standard directories present on the volume.
    struct DefaultAssign {
        const char* name;
        const char* path;
    };
    constexpr DefaultAssign kDefaultAssigns[] = {
        {"SYS", ""},           {"C", "C"},               {"S", "S"},
        {"L", "L"},            {"LIBS", "LIBS"},         {"DEVS", "DEVS"},
        {"FONTS", "FONTS"},    {"LOCALE", "LOCALE"},     {"KEYMAPS", "KEYMAPS"},
        {"PRINTERS", "PRINTERS"}, {"ENVARC", "Prefs/Env-Archive"},
    };
    for (const DefaultAssign& def : kDefaultAssigns) {
        DirEntry entry{};
        if (DosOps::fs_find(*this, kVolAdf, def.path, entry) != 0 || !entry.is_dir) {
            continue;
        }
        char lock_path[kMaxPath];
        if (def.path[0] == '\0') {
            lock_path[0] = '\0';
        } else if (same_name(def.name, length_of(def.name), "ENVARC")) {
            DirEntry prefs{};
            if (DosOps::fs_find(*this, kVolAdf, "Prefs", prefs) == 0 && prefs.is_dir) {
                u32 p = 0;
                for (u32 i = 0; prefs.name[i] != '\0' && p + 2 < kMaxPath; ++i) {
                    lock_path[p++] = prefs.name[i];
                }
                lock_path[p++] = '/';
                for (u32 i = 0; entry.name[i] != '\0' && p + 1 < kMaxPath; ++i) {
                    lock_path[p++] = entry.name[i];
                }
                lock_path[p] = '\0';
            } else {
                copy_cstr(lock_path, def.path, kMaxPath);
            }
        } else {
            copy_cstr(lock_path, entry.name, kMaxPath);
        }
        const i64 idx = DosOps::free_assign_slot(*this);
        if (idx < 0) {
            break;
        }
        u32 lock_bptr = 0;
        if (!DosOps::alloc_assign_lock(*this, b, lock_path, entry, lock_bptr)) {
            m_ = nullptr;
            return false;
        }
        AssignEntry& e = assigns_[idx];
        e.used = true;
        e.type = DLT_DIRECTORY;
        copy_cstr(e.name, def.name, sizeof e.name);
        e.dir_count = 1;
        e.locks[0] = lock_bptr;
        if (!DosOps::build_assign_node(*this, b, e)) {
            m_ = nullptr;
            return false;
        }
    }
    DosOps::relink_dos_list(*this, b);

    if (!b.ok()) {
        m_ = nullptr;
        return false;
    }
    exec.set_this_task(process_);
    exec.set_segment_loader(SegmentLoader{DosOps::load_segment, this});
    return true;
}

u32 Dos::load_seg(const char* path) {
    if (m_ == nullptr || path == nullptr) {
        return 0;
    }
    Bus b(*m_);
    char name[kMaxName];
    u32 length = 0;
    while (path[length] != '\0') {
        if (++length == kMaxName) {
            DosOps::set_io_err(*this, b, ERROR_INVALID_COMPONENT_NAME);
            return 0;
        }
    }
    for (u32 i = 0; i <= length; ++i) {
        name[i] = path[i];
    }
    u8 vol = kVolAdf;
    char resolved[kMaxPath];
    DirEntry entry{};
    u32 error = 0;
    if (!DosOps::locate(*this, b, name, vol, resolved, entry, error)) {
        if (b.ok()) {
            DosOps::set_io_err(*this, b, error);
        }
        return 0;
    }
    if (entry.is_dir) {
        DosOps::set_io_err(*this, b, ERROR_OBJECT_WRONG_TYPE);
        return 0;
    }
    const u32 file_size = DosOps::fs_size(*this, entry);
    if (file_size == 0) {
        DosOps::set_io_err(*this, b, ERROR_FILE_NOT_OBJECT);
        return 0;
    }
    // The file is read into guest memory for the loader and dropped after.
    const u32 file = exec_->alloc(file_size, MEMF_PUBLIC);
    if (file == 0) {
        DosOps::set_io_err(*this, b, ERROR_NO_FREE_STORE);
        return 0;
    }
    u32 got = 0;
    if (DosOps::fs_read(*this, entry, 0, m_->mem.base + file, file_size, got) != 0) {
        exec_->free(file, file_size);
        DosOps::set_io_err(*this, b, ERROR_NOT_A_DOS_DISK);
        return 0;
    }
    // A file that does not start with HUNK_HEADER is no executable; one
    // that does but fails to load is a bad one.
    u32 magic = 0;
    if (got < 4 || !m_->mem.read32(file, magic) || magic != hunk_detail::kHunkHeader) {
        exec_->free(file, file_size);
        DosOps::set_io_err(*this, b, ERROR_FILE_NOT_OBJECT);
        return 0;
    }
    SegmentTracker tracker{exec_, {}, {}, 0};
    u32 seglist = 0;
    const HunkError loaded = hunk_load(m_->mem.base + file, got, m_->mem,
                                       HunkAllocator{SegmentTracker::alloc, &tracker}, seglist);
    exec_->free(file, file_size);
    if (loaded != HunkError::Ok) {
        tracker.free_all();
        DosOps::set_io_err(*this, b,
                           loaded == HunkError::OutOfMemory ? ERROR_NO_FREE_STORE : ERROR_BAD_HUNK);
        return 0;
    }
    m_->jit->flush();
    return seglist;
}

bool Dos::run_command(const char* path, const char* args, u32& result, u64 max_blocks) {
    if (m_ == nullptr || path == nullptr || args == nullptr) {
        return false;
    }
    if (running_) {
        LogLine().text("dos: ").text(path).text(" started while a command runs").send();
        return false;
    }
    u32 length = 0;
    while (args[length] != '\0') {
        if (++length > kMaxArgs) {
            LogLine().text("dos: the argument line of ").text(path).text(" is longer than ")
                .dec(kMaxArgs).text(" bytes").send();
            return false;
        }
    }
    const u32 seglist = load_seg(path);
    if (seglist == 0) {
        LogLine().text("dos: cannot load ").text(path).send();
        return false;
    }

    // The argument line ends in a newline, as the shell passes it.
    for (u32 i = 0; i < length; ++i) {
        input_[i] = args[i];
    }
    if (length == 0 || input_[length - 1] != '\n') {
        input_[length++] = '\n';
    }
    input_length_ = length;
    input_pos_ = 0;

    Bus b(*m_);
    for (u32 i = 0; i < length; ++i) {
        b.w8(args_ + i, static_cast<u8>(input_[i]));
    }
    b.w8(args_ + length, 0);
    put_bstr(b, command_name_, path, 255);
    b.w32(cli_ + cli_CommandName, command_name_ >> 2);
    b.w32(cli_ + cli_Module, seglist);
    b.w32(cli_ + cli_ReturnCode, 0);
    b.w32(process_ + pr_Result2, 0);
    b.w32(process_ + pr_Arguments, args_);

    // The stack: its size at 4(SP), which pr_ReturnAddr points at, and the
    // return address call_guest pushes at (SP).
    const u32 top = stack_ + kStackBytes;
    b.w32(top - 4, kStackBytes);
    b.w32(process_ + pr_ReturnAddr, top - 4);
    b.w32(process_ + TC_SPREG, top - 8);
    const u32 saved_current_dir = b.r32(process_ + pr_CurrentDir);
    const u32 saved_cis = b.r32(process_ + pr_CIS);
    const u32 saved_cos = b.r32(process_ + pr_COS);
    if (!b.ok()) {
        DosOps::unload(*this, seglist);
        return false;
    }
    CpuState& cpu = m_->cpu;
    for (u32 i = 0; i < 8; ++i) {
        cpu.d[i] = 0;
        cpu.a[i] = 0;
    }
    cpu.d[0] = length;
    cpu.a[0] = args_;
    cpu.a[7] = top - 4;
    exec_->set_this_task(process_);

    running_ = true;
    exited_ = false;
    const ExitReason why = m_->jit->call_guest((seglist << 2) + 4, max_blocks);
    running_ = false;
    DosOps::cleanup_command(*this, saved_current_dir, saved_cis, saved_cos);
    bool ok = false;
    if (why == ExitReason::Returned) {
        result = cpu.d[0];
        ok = true;
    } else if (why == ExitReason::Halt && exited_) {
        result = exit_code_;
        cpu.exit = ExitReason::None;
        ok = true;
    } else {
        LogLine().text("dos: ").text(path).text(" ended with ").text(exit_reason_name(why)).send();
    }
    exited_ = false;
    input_length_ = 0;
    input_pos_ = 0;
    m_->mem.write32(cli_ + cli_Module, 0);
    if (ok) {
        m_->mem.write32(cli_ + cli_ReturnCode, result);
        u32 io_err = 0;
        m_->mem.read32(process_ + pr_Result2, io_err);
        m_->mem.write32(cli_ + cli_Result2, io_err);
    }
    DosOps::unload(*this, seglist);
    return ok;
}

bool Dos::change_dir(const char* path, u32& error) {
    if (m_ == nullptr || path == nullptr) {
        error = ERROR_INVALID_COMPONENT_NAME;
        return false;
    }
    if (length_of(path) >= kMaxName) {
        error = ERROR_INVALID_COMPONENT_NAME;
        set_last_error(error);
        return false;
    }
    Bus b(*m_);
    u8 vol = kVolAdf;
    char resolved[kMaxPath];
    DirEntry entry{};
    if (!DosOps::locate(*this, b, path, vol, resolved, entry, error)) {
        if (b.ok()) {
            set_last_error(error);
        }
        return false;
    }
    if (!entry.is_dir) {
        error = ERROR_OBJECT_WRONG_TYPE;
        set_last_error(error);
        return false;
    }
    char canonical[kMaxPath];
    DosOps::fs_canonical(*this, vol, resolved, canonical);
    u32 new_lock = 0;
    if (!DosOps::alloc_user_lock(*this, b, canonical, entry, SHARED_LOCK, new_lock, false)) {
        error = b.r32(process_ + pr_Result2);
        return false;
    }
    const u32 old_lock = b.r32(process_ + pr_CurrentDir);
    b.w32(process_ + pr_CurrentDir, new_lock);
    if (old_lock != 0) {
        DosOps::free_user_lock(*this, old_lock);
    }
    char full_name[kMaxName];
    format_volume_path(DosOps::volume_name(*this, vol), canonical, full_name, sizeof full_name);
    put_bstr(b, set_name_, full_name, 255);
    b.w32(cli_ + cli_SetName, set_name_ >> 2);
    error = 0;
    return b.ok();
}

bool Dos::current_dir_name(char* out, u32 size) const {
    if (m_ == nullptr || out == nullptr || size == 0) {
        return false;
    }
    Bus b(*m_);
    return DosOps::read_cli_bstr_host(*this, b, cli_SetName, out, size);
}

bool Dos::prompt(char* out, u32 size) const {
    if (m_ == nullptr || out == nullptr || size == 0) {
        return false;
    }
    Bus b(*m_);
    return DosOps::read_cli_bstr_host(*this, b, cli_Prompt, out, size);
}

void Dos::set_prompt(const char* text) {
    if (m_ == nullptr || text == nullptr || cli_ == 0) {
        return;
    }
    Bus b(*m_);
    put_bstr(b, prompt_, text, 255);
    b.w32(cli_ + cli_Prompt, prompt_ >> 2);
}

u32 Dos::last_error() const {
    if (m_ == nullptr) {
        return 0;
    }
    Bus b(*m_);
    return b.r32(process_ + pr_Result2);
}

void Dos::set_last_error(u32 code) {
    if (m_ == nullptr) {
        return;
    }
    Bus b(*m_);
    b.w32(process_ + pr_Result2, code);
    if (cli_ != 0) {
        b.w32(cli_ + cli_Result2, code);
    }
}

u32 Dos::last_return_code() const {
    if (m_ == nullptr || cli_ == 0) {
        return 0;
    }
    Bus b(*m_);
    return b.r32(cli_ + cli_ReturnCode);
}

void Dos::set_return_code(u32 code) {
    if (m_ == nullptr || cli_ == 0) {
        return;
    }
    Bus b(*m_);
    b.w32(cli_ + cli_ReturnCode, code);
}

u32 Dos::fail_level() const {
    if (m_ == nullptr || cli_ == 0) {
        return kFailLevel;
    }
    Bus b(*m_);
    return b.r32(cli_ + cli_FailLevel);
}

void Dos::set_fail_level(u32 level) {
    if (m_ == nullptr || cli_ == 0) {
        return;
    }
    Bus b(*m_);
    b.w32(cli_ + cli_FailLevel, level);
}

u32 Dos::fault(u32 code, char* out, u32 size) const {
    return copy_fault_message(code, out, size);
}

bool Dos::path_add(const char* dir, u32& error) {
    if (m_ == nullptr || dir == nullptr) {
        error = ERROR_INVALID_COMPONENT_NAME;
        return false;
    }
    if (length_of(dir) >= kMaxName) {
        error = ERROR_INVALID_COMPONENT_NAME;
        set_last_error(error);
        return false;
    }
    if (path_count_ >= kMaxPathDirs) {
        error = ERROR_NO_FREE_STORE;
        set_last_error(error);
        return false;
    }
    Bus b(*m_);
    u8 vol = kVolAdf;
    char resolved[kMaxPath];
    DirEntry entry{};
    if (!DosOps::locate(*this, b, dir, vol, resolved, entry, error)) {
        if (b.ok()) {
            set_last_error(error);
        }
        return false;
    }
    if (!entry.is_dir) {
        error = ERROR_OBJECT_WRONG_TYPE;
        set_last_error(error);
        return false;
    }
    char canonical[kMaxPath];
    DosOps::fs_canonical(*this, vol, resolved, canonical);
    if (path_index_of(vol, canonical) < path_count_) {
        error = 0;
        return true;  // already in the path
    }
    u32 lock_bptr = 0;
    if (!DosOps::alloc_user_lock(*this, b, canonical, entry, SHARED_LOCK, lock_bptr, false)) {
        error = b.r32(process_ + pr_Result2);
        return false;
    }
    constexpr u32 kPathNodeBytes = 8;
    const u32 node = exec_->alloc(kPathNodeBytes, MEMF_PUBLIC | MEMF_CLEAR);
    if (node == 0) {
        DosOps::free_user_lock(*this, lock_bptr);
        error = ERROR_NO_FREE_STORE;
        set_last_error(error);
        return false;
    }
    b.w32(node, 0);
    b.w32(node + 4, lock_bptr);
    if (path_count_ == 0) {
        b.w32(cli_ + cli_CommandDir, node >> 2);
    } else {
        b.w32(path_nodes_[path_count_ - 1], node >> 2);
    }
    if (!b.ok()) {
        exec_->free(node, kPathNodeBytes);
        DosOps::free_user_lock(*this, lock_bptr);
        error = ERROR_INVALID_LOCK;
        return false;
    }
    path_locks_[path_count_] = lock_bptr;
    path_nodes_[path_count_] = node;
    ++path_count_;
    error = 0;
    return true;
}

u32 Dos::path_index_of(u8 vol, const char* canonical) const {
    for (u32 i = 0; i < path_count_ && i < kMaxPathDirs; ++i) {
        const i64 slot = DosOps::lock_slot(*this, path_locks_[i]);
        if (slot < 0 || lock_slots_[slot].entry.volume != vol) {
            continue;
        }
        const char* held = lock_slots_[slot].path;
        if (same_name(held, length_of(held), canonical)) {
            return i;
        }
    }
    return path_count_;
}

bool Dos::path_remove(const char* dir, u32& error) {
    if (m_ == nullptr || dir == nullptr || length_of(dir) >= kMaxName) {
        error = ERROR_INVALID_COMPONENT_NAME;
        return false;
    }
    Bus b(*m_);
    u8 vol = kVolAdf;
    char resolved[kMaxPath];
    DirEntry entry{};
    if (!DosOps::locate(*this, b, dir, vol, resolved, entry, error)) {
        if (b.ok()) {
            set_last_error(error);
        }
        return false;
    }
    char canonical[kMaxPath];
    DosOps::fs_canonical(*this, vol, resolved, canonical);
    const u32 index = path_index_of(vol, canonical);
    if (index >= path_count_) {
        error = ERROR_OBJECT_NOT_FOUND;
        set_last_error(error);
        return false;
    }
    constexpr u32 kPathNodeBytes = 8;
    DosOps::free_user_lock(*this, path_locks_[index]);
    exec_->free(path_nodes_[index], kPathNodeBytes);
    for (u32 i = index; i + 1 < path_count_; ++i) {
        path_locks_[i] = path_locks_[i + 1];
        path_nodes_[i] = path_nodes_[i + 1];
    }
    --path_count_;
    path_locks_[path_count_] = 0;
    path_nodes_[path_count_] = 0;
    // Relink the guest list in order.
    b.w32(cli_ + cli_CommandDir, path_count_ == 0 ? 0 : path_nodes_[0] >> 2);
    for (u32 i = 0; i < path_count_; ++i) {
        b.w32(path_nodes_[i], i + 1 < path_count_ ? path_nodes_[i + 1] >> 2 : 0);
    }
    error = 0;
    return true;
}

void Dos::path_reset() {
    if (m_ == nullptr) {
        return;
    }
    constexpr u32 kPathNodeBytes = 8;
    for (u32 i = 0; i < path_count_ && i < kMaxPathDirs; ++i) {
        if (path_locks_[i] != 0) {
            DosOps::free_user_lock(*this, path_locks_[i]);
            path_locks_[i] = 0;
        }
        if (path_nodes_[i] != 0) {
            exec_->free(path_nodes_[i], kPathNodeBytes);
            path_nodes_[i] = 0;
        }
    }
    path_count_ = 0;
    if (cli_ != 0) {
        Bus b(*m_);
        b.w32(cli_ + cli_CommandDir, 0);
    }
}

bool Dos::path_name(u32 i, char* out, u32 size) const {
    if (m_ == nullptr || out == nullptr || size == 0 || i >= path_count_) {
        return false;
    }
    const i64 slot = DosOps::lock_slot(*this, path_locks_[i]);
    if (slot < 0) {
        out[0] = '\0';
        return false;
    }
    return format_volume_path(DosOps::volume_name(*this, lock_slots_[slot].entry.volume),
                              lock_slots_[slot].path, out, size);
}

bool Dos::find_command(const char* word, char* out, u32 size) {
    if (m_ == nullptr || word == nullptr || word[0] == '\0' || out == nullptr || size == 0) {
        return false;
    }
    const u32 word_len = length_of(word);
    if (word_len >= kMaxName) {
        return false;
    }
    bool has_sep = false;
    for (u32 i = 0; i < word_len; ++i) {
        if (word[i] == ':' || word[i] == '/') {
            has_sep = true;
            break;
        }
    }
    Bus b(*m_);
    auto exists_file = [&](const char* candidate) -> bool {
        u8 vol = kVolAdf;
        char resolved[kMaxPath];
        DirEntry entry{};
        u32 error = 0;
        return DosOps::locate(*this, b, candidate, vol, resolved, entry, error) && !entry.is_dir;
    };
    auto copy_out = [&](const char* candidate) -> bool {
        const u32 len = length_of(candidate);
        if (len + 1 > size) {
            return false;
        }
        copy_cstr(out, candidate, size);
        return true;
    };

    if (has_sep) {
        return exists_file(word) && copy_out(word);
    }

    if (exists_file(word)) {
        return copy_out(word);
    }

    char cand[kMaxName + 32];
    for (u32 i = 0; i < path_count_ && i < kMaxPathDirs; ++i) {
        char dir[kMaxName];
        if (!path_name(i, dir, sizeof dir)) {
            continue;
        }
        const u32 dir_len = length_of(dir);
        const bool need_slash = dir_len > 0 && dir[dir_len - 1] != ':' && dir[dir_len - 1] != '/';
        if (dir_len + (need_slash ? 1u : 0u) + word_len >= sizeof cand) {
            continue;
        }
        copy_cstr(cand, dir, sizeof cand);
        u32 pos = dir_len;
        if (need_slash) {
            cand[pos++] = '/';
        }
        copy_cstr(cand + pos, word, sizeof cand - pos);
        if (exists_file(cand)) {
            return copy_out(cand);
        }
    }

    if (2 + word_len < sizeof cand) {
        cand[0] = 'C';
        cand[1] = ':';
        copy_cstr(cand + 2, word, sizeof cand - 2);
        if (exists_file(cand)) {
            return copy_out(cand);
        }
    }
    return false;
}

void Dos::set_console_input(ConsoleRead read, ConsoleReady ready, void* ctx) {
    console_read_ = read;
    console_ready_ = ready;
    console_ctx_ = ctx;
}

bool Dos::read_console_line(char* out, u32 size) {
    if (out == nullptr || size == 0) {
        return false;
    }
    out[0] = '\0';
    if (console_read_ == nullptr) {
        return false;
    }
    u8 raw[kMaxArgs + 2];
    u32 n = console_read_(console_ctx_, raw, kMaxArgs + 1);
    if (n == 0) {
        return false;
    }
    if (n > kMaxArgs + 1) {
        n = kMaxArgs + 1;
    }
    while (n > 0 && (raw[n - 1] == '\n' || raw[n - 1] == '\r')) {
        --n;
    }
    const u32 copy = n < size - 1 ? n : size - 1;
    for (u32 i = 0; i < copy; ++i) {
        out[i] = static_cast<char>(raw[i]);
    }
    out[copy] = '\0';
    return true;
}

}
