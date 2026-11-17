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
constexpr u8 SIGB_DOS = 8;
constexpr char kDosName[] = "dos.library";
constexpr char kIdString[] = "dos 60.0 (Hombre)\r\n";
constexpr char kProcessName[] = "Hombre Shell";
constexpr char kPrompt[] = "%N> ";

constexpr u32 align4(u32 v) {
    return (v + 3) & ~3u;
}

// Where dos's structures sit in its block of guest memory.
constexpr u32 kRootAt = 0;
constexpr u32 kInfoAt = kRootAt + rn_SIZEOF;
constexpr u32 kTaskArrayAt = align4(kInfoAt + di_SIZEOF);
constexpr u32 kVolumeAt = kTaskArrayAt + 8;
constexpr u32 kVolumeNameAt = kVolumeAt + DosList_SIZEOF;  // a BSTR of up to 30 characters
constexpr u32 kPortAt = kVolumeNameAt + 32;
constexpr u32 kProcessAt = align4(kPortAt + MP_SIZE);
constexpr u32 kProcessNameAt = kProcessAt + pr_SIZEOF;
constexpr u32 kCliAt = align4(kProcessNameAt + sizeof kProcessName);
constexpr u32 kSetNameAt = kCliAt + cli_SIZEOF;  // the volume name and ':'
constexpr u32 kPromptAt = kSetNameAt + 36;
constexpr u32 kCommandFileAt = kPromptAt + 8;
constexpr u32 kCommandNameAt = kCommandFileAt + 4;  // up to 255 characters
constexpr u32 kIdStringAt = kCommandNameAt + 260;
constexpr u32 kHandlesAt = align4(kIdStringAt + sizeof kIdString);
constexpr u32 kLocksAt = kHandlesAt + kHandles * fh_SIZEOF;
constexpr u32 kArgsAt = kLocksAt + Dos::kMaxLocks * fl_SIZEOF;
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

    static u32 handle_bptr(const Dos& d, u32 slot) { return (d.handles_ + slot * fh_SIZEOF) >> 2; }
    static u32 lock_bptr(const Dos& d, u32 slot) { return (d.locks_ + slot * fl_SIZEOF) >> 2; }

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
        if (d.files_[slot].stream == Dos::Stream::Console) {
            if (d.out_ != nullptr && bytes != 0) {
                d.out_(d.out_ctx_, data, bytes);
            }
            return true;
        }
        set_io_err(d, b, ERROR_DISK_WRITE_PROTECTED);
        return false;
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

    // Turns an AmigaDOS name into a path from the volume's root, its
    // components separated by '/'. A name with a device starts where the
    // device points; one without starts at the current directory.
    static bool resolve(Dos& d, Bus& b, const char* name, char (&out)[Dos::kMaxPath], u32& error) {
        u32 at = 0;
        const char* rest = name;
        u32 colon = 0;
        while (name[colon] != '\0' && name[colon] != ':') {
            ++colon;
        }
        if (name[colon] == ':') {
            const char* start = "";
            if (colon == 0 || same_name(name, colon, "SYS") ||
                same_name(name, colon, d.volume_.name)) {
                start = "";
            } else if (same_name(name, colon, "C")) {
                start = "C";
            } else if (same_name(name, colon, "LIBS")) {
                start = "LIBS";
            } else if (same_name(name, colon, "S")) {
                start = "S";
            } else {
                error = ERROR_DEVICE_NOT_MOUNTED;
                return false;
            }
            for (; start[at] != '\0'; ++at) {
                out[at] = start[at];
            }
            rest = name + colon + 1;
        } else {
            const u32 current = b.r32(d.process_ + pr_CurrentDir);
            if (!b.ok()) {
                return false;
            }
            if (current != 0) {
                const i64 slot = lock_slot(d, current);
                if (slot < 0) {
                    error = ERROR_INVALID_LOCK;
                    return false;
                }
                const char* base = d.lock_slots_[slot].path;
                for (; base[at] != '\0'; ++at) {
                    out[at] = base[at];
                }
            }
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

    // Resolves a name and looks it up on the volume.
    static bool locate(Dos& d, Bus& b, const char* name, char (&path)[Dos::kMaxPath],
                       DirEntry& entry, u32& error) {
        if (!resolve(d, b, name, path, error)) {
            return false;
        }
        const FsError found = adf_find(d.volume_, path, entry);
        if (found != FsError::Ok) {
            error = io_error_of(found);
            return false;
        }
        return true;
    }

    // --- console and files -------------------------------------------------

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
            d.files_[slot] = Dos::FileSlot{Dos::Stream::Console, DirEntry{}, 0};
            init_handle(d, b, static_cast<u32>(slot), true);
            native::set_result(m, handle_bptr(d, static_cast<u32>(slot)));
            return;
        }
        if (mode == MODE_NEWFILE || mode == MODE_READWRITE) {
            fail(d, b, ERROR_DISK_WRITE_PROTECTED, 0);
            return;
        }
        if (mode != MODE_OLDFILE) {
            fail(d, b, ERROR_ACTION_NOT_KNOWN, 0);
            return;
        }
        char path[Dos::kMaxPath];
        DirEntry entry{};
        u32 error = 0;
        if (!locate(d, b, name, path, entry, error)) {
            if (b.ok()) {
                fail(d, b, error, 0);
            }
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
        d.files_[slot] = Dos::FileSlot{Dos::Stream::File, entry, 0};
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
            d.files_[slot].stream = Dos::Stream::Closed;
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
        Dos::FileSlot& f = d.files_[slot];
        if (f.stream == Dos::Stream::Console) {
            const u32 left = d.input_length_ - d.input_pos_;
            const u32 n = length < left ? length : left;
            u8* to = n != 0 ? b.bytes_at(buffer, n) : nullptr;
            if (n != 0 && to == nullptr) {
                return;
            }
            for (u32 i = 0; i < n; ++i) {
                to[i] = static_cast<u8>(d.input_[d.input_pos_ + i]);
            }
            d.input_pos_ += n;
            native::set_result(m, n);
            return;
        }
        const u32 left = f.pos < f.entry.size ? f.entry.size - f.pos : 0;
        const u32 n = length < left ? length : left;
        if (n == 0) {
            native::set_result(m, 0);
            return;
        }
        u8* to = b.bytes_at(buffer, n);
        if (to == nullptr) {
            return;
        }
        u32 got = 0;
        if (adf_read(d.volume_, f.entry, f.pos, to, n, got) != FsError::Ok) {
            fail(d, b, ERROR_NOT_A_DOS_DISK, 0xFFFFFFFF);
            return;
        }
        f.pos += got;
        native::set_result(m, got);
    }

    static void write(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 fh = m.cpu.d[1];
        const u32 buffer = m.cpu.d[2];
        const u32 length = m.cpu.d[3];
        const i64 slot = handle_slot(d, fh);
        if (slot < 0) {
            log_bad_handle("Write to", fh);
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
        native::set_result(m, ok ? length : 0xFFFFFFFF);
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
        const u32 mode = m.cpu.d[3];
        i64 from = 0;
        if (mode == OFFSET_BEGINNING) {
            from = 0;
        } else if (mode == OFFSET_CURRENT) {
            from = f.pos;
        } else if (mode == OFFSET_END) {
            from = f.entry.size;
        } else {
            fail(d, b, ERROR_SEEK_ERROR, 0xFFFFFFFF);
            return;
        }
        const i64 to = from + static_cast<i64>(static_cast<__INT32_TYPE__>(m.cpu.d[2]));
        if (to < 0 || to > static_cast<i64>(f.entry.size)) {
            fail(d, b, ERROR_SEEK_ERROR, 0xFFFFFFFF);
            return;
        }
        const u32 old = f.pos;
        f.pos = static_cast<u32>(to);
        native::set_result(m, old);
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

    // Nothing is buffered.
    static void flush(void*, Machine& m) { native::set_result(m, DOSTRUE); }

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
        const char* message = nullptr;
        for (const FaultText& f : kFaults) {
            if (f.code == code) {
                message = f.text;
            }
        }
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

    // --- time ----------------------------------------------------------------

    // There is no clock: the time is the start of 1978.
    static void date_stamp(void*, Machine& m) {
        Bus b(m);
        const u32 ds = m.cpu.d[1];
        b.w32(ds + ds_Days, 0);
        b.w32(ds + ds_Minute, 0);
        b.w32(ds + ds_Tick, 0);
        native::set_result(m, ds);
    }

    // Nothing else runs, so a Delay is over at once.
    static void delay(void*, Machine& m) { native::set_result(m, 0); }

    // The console never has a character waiting.
    static void wait_for_char(void*, Machine& m) { native::set_result(m, DOSFALSE); }

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

    static void init_lock(Dos& d, Bus& b, u32 slot, u32 key, u32 access) {
        const u32 at = d.locks_ + slot * fl_SIZEOF;
        b.w32(at + fl_Link, 0);
        b.w32(at + fl_Key, key);
        b.w32(at + fl_Access, access == EXCLUSIVE_LOCK ? EXCLUSIVE_LOCK : SHARED_LOCK);
        b.w32(at + fl_Task, d.port_);
        b.w32(at + fl_Volume, d.volume_node_ >> 2);
    }

    static i64 free_lock(const Dos& d) {
        for (u32 slot = 0; slot < Dos::kMaxLocks; ++slot) {
            if (!d.lock_slots_[slot].used) {
                return slot;
            }
        }
        return -1;
    }

    static void lock(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        char name[kMaxName];
        if (!read_name(d, b, m.cpu.d[1], name)) {
            native::set_result(m, 0);
            return;
        }
        const i64 slot = free_lock(d);
        if (slot < 0) {
            table_full(d, b, "lock", Dos::kMaxLocks);
            return;
        }
        Dos::LockSlot& l = d.lock_slots_[slot];
        u32 error = 0;
        if (!locate(d, b, name, l.path, l.entry, error)) {
            if (b.ok()) {
                fail(d, b, error, 0);
            }
            return;
        }
        l.used = true;
        init_lock(d, b, static_cast<u32>(slot), l.entry.header_block, m.cpu.d[2]);
        native::set_result(m, lock_bptr(d, static_cast<u32>(slot)));
    }

    static void unlock(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        const u32 bptr = m.cpu.d[1];
        if (bptr == 0) {
            return;
        }
        const i64 slot = lock_slot(d, bptr);
        if (slot < 0) {
            LogLine().text("dos: UnLock of a bad lock ").hex(bptr).send();
            return;
        }
        d.lock_slots_[slot].used = false;
    }

    static void dup_lock(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 bptr = m.cpu.d[1];
        if (bptr == 0) {
            native::set_result(m, 0);
            return;
        }
        const i64 from = lock_slot(d, bptr);
        if (from < 0) {
            fail(d, b, ERROR_INVALID_LOCK, 0);
            return;
        }
        const i64 slot = free_lock(d);
        if (slot < 0) {
            table_full(d, b, "lock", Dos::kMaxLocks);
            return;
        }
        d.lock_slots_[slot] = d.lock_slots_[from];
        init_lock(d, b, static_cast<u32>(slot), d.lock_slots_[slot].entry.header_block,
                  SHARED_LOCK);
        native::set_result(m, lock_bptr(d, static_cast<u32>(slot)));
    }

    static void current_dir(void* ctx, Machine& m) { select(m, of(ctx), pr_CurrentDir); }

    // The entry and path of a lock; 0 is the root. False with IoErr set if
    // it is not a lock.
    static bool lock_entry(Dos& d, Bus& b, u32 bptr, DirEntry& entry, const char*& path) {
        if (bptr == 0) {
            path = "";
            const FsError error = adf_find(d.volume_, "", entry);
            if (error != FsError::Ok) {
                set_io_err(d, b, io_error_of(error));
                return false;
            }
            return true;
        }
        const i64 slot = lock_slot(d, bptr);
        if (slot < 0) {
            set_io_err(d, b, ERROR_INVALID_LOCK);
            return false;
        }
        entry = d.lock_slots_[slot].entry;
        path = d.lock_slots_[slot].path;
        return true;
    }

    // Fills a FileInfoBlock from an entry and its header block.
    static bool fill_fib(Dos& d, Bus& b, u32 fib, const DirEntry& entry) {
        using namespace amiga_fs_detail;
        FsError error = FsError::Ok;
        const u8* header = load_header(d.volume_, entry.header_block, error);
        if (header == nullptr) {
            set_io_err(d, b, io_error_of(error));
            return false;
        }
        const u32 type = long_at(header, l_sec_type);
        const u32 payload = d.volume_.ffs ? block_bytes : block_bytes - ofs_header_bytes;
        b.w32(fib + fib_DiskKey, entry.header_block);
        b.w32(fib + fib_DirEntryType, type);
        u32 i = 0;
        for (; entry.name[i] != '\0' && i < 107; ++i) {
            b.w8(fib + fib_FileName + i, static_cast<u8>(entry.name[i]));
        }
        b.w8(fib + fib_FileName + i, 0);
        b.w32(fib + fib_Protection, long_at(header, block_longs - 48));
        b.w32(fib + fib_EntryType, type);
        b.w32(fib + fib_Size, entry.size);
        b.w32(fib + fib_NumBlocks, entry.is_dir ? 0 : (entry.size + payload - 1) / payload);
        b.w32(fib + fib_DateStamp + ds_Days, long_at(header, block_longs - 23));
        b.w32(fib + fib_DateStamp + ds_Minute, long_at(header, block_longs - 22));
        b.w32(fib + fib_DateStamp + ds_Tick, long_at(header, block_longs - 21));
        // The comment is a BSTR of at most 79 characters in the header.
        const u8* comment = header + (block_longs - 46) * 4;
        const u32 length = comment[0] < 79 ? comment[0] : 79;
        for (u32 k = 0; k < length; ++k) {
            b.w8(fib + fib_Comment + k, comment[1 + k]);
        }
        b.w8(fib + fib_Comment + length, 0);
        b.w16(fib + fib_OwnerUID, 0);
        b.w16(fib + fib_OwnerGID, 0);
        return b.ok();
    }

    static void examine(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        DirEntry entry{};
        const char* path = nullptr;
        if (!lock_entry(d, b, m.cpu.d[1], entry, path)) {
            native::set_result(m, DOSFALSE);
            return;
        }
        const bool ok = fill_fib(d, b, m.cpu.d[2], entry);
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

    static void ex_next(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 fib = m.cpu.d[2];
        DirEntry dir{};
        const char* path = nullptr;
        if (!lock_entry(d, b, m.cpu.d[1], dir, path)) {
            native::set_result(m, DOSFALSE);
            return;
        }
        const u32 after = b.r32(fib + fib_DiskKey);
        if (!b.ok()) {
            return;
        }
        if (!dir.is_dir) {
            fail(d, b, ERROR_OBJECT_WRONG_TYPE, DOSFALSE);
            return;
        }
        NextEntry next{after, after == dir.header_block, false, false, DirEntry{}};
        const FsError error = adf_list(d.volume_, path, NextEntry::visit, &next);
        if (error != FsError::Ok) {
            fail(d, b, io_error_of(error), DOSFALSE);
            return;
        }
        if (!next.found) {
            fail(d, b, ERROR_NO_MORE_ENTRIES, DOSFALSE);
            return;
        }
        const bool ok = fill_fib(d, b, fib, next.entry);
        native::set_result(m, ok ? DOSTRUE : DOSFALSE);
    }

    static void info(void* ctx, Machine& m) {
        Dos& d = of(ctx);
        Bus b(m);
        const u32 id = m.cpu.d[2];
        DirEntry entry{};
        const char* path = nullptr;
        if (!lock_entry(d, b, m.cpu.d[1], entry, path)) {
            native::set_result(m, DOSFALSE);
            return;
        }
        const u32 blocks = d.volume_.block_count - 2;  // the bootblock is not counted
        b.w32(id + id_NumSoftErrors, 0);
        b.w32(id + id_UnitNumber, 0);
        b.w32(id + id_DiskState, ID_WRITE_PROTECTED);
        b.w32(id + id_NumBlocks, blocks);
        b.w32(id + id_NumBlocksUsed, blocks);
        b.w32(id + id_BytesPerBlock, d.volume_.ffs ? 512 : 488);
        b.w32(id + id_DiskType, ID_DOS_DISK | d.volume_.image[3]);
        b.w32(id + id_VolumeNode, d.volume_node_ >> 2);
        b.w32(id + id_InUse, 0);
        native::set_result(m, DOSTRUE);
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
    {dos_lvo::Lock, DosOps::lock, "Lock"},
    {dos_lvo::UnLock, DosOps::unlock, "UnLock"},
    {dos_lvo::DupLock, DosOps::dup_lock, "DupLock"},
    {dos_lvo::Examine, DosOps::examine, "Examine"},
    {dos_lvo::ExNext, DosOps::ex_next, "ExNext"},
    {dos_lvo::Info, DosOps::info, "Info"},
    {dos_lvo::CurrentDir, DosOps::current_dir, "CurrentDir"},
    {dos_lvo::IoErr, DosOps::io_err, "IoErr"},
    {dos_lvo::Exit, DosOps::exit, "Exit"},
    {dos_lvo::LoadSeg, DosOps::load_seg_lvo, "LoadSeg"},
    {dos_lvo::UnLoadSeg, DosOps::unload_seg, "UnLoadSeg"},
    {dos_lvo::DateStamp, DosOps::date_stamp, "DateStamp"},
    {dos_lvo::Delay, DosOps::delay, "Delay"},
    {dos_lvo::WaitForChar, DosOps::wait_for_char, "WaitForChar"},
    {dos_lvo::SelectInput, DosOps::select_input, "SelectInput"},
    {dos_lvo::SelectOutput, DosOps::select_output, "SelectOutput"},
    {dos_lvo::FPutC, DosOps::fput_c, "FPutC"},
    {dos_lvo::FPuts, DosOps::fputs, "FPuts"},
    {dos_lvo::VFPrintf, DosOps::vfprintf, "VFPrintf"},
    {dos_lvo::Flush, DosOps::flush, "Flush"},
    {dos_lvo::SetMode, DosOps::set_mode, "SetMode"},
    {dos_lvo::SetIoErr, DosOps::set_io_err_lvo, "SetIoErr"},
    {dos_lvo::Fault, DosOps::fault, "Fault"},
    {dos_lvo::PrintFault, DosOps::print_fault, "PrintFault"},
    {dos_lvo::Cli, DosOps::cli, "Cli"},
    {dos_lvo::NewLoadSeg, DosOps::load_seg_lvo, "NewLoadSeg"},
    {dos_lvo::ReadArgs, DosOps::read_args, "ReadArgs"},
    {dos_lvo::StrToLong, DosOps::str_to_long, "StrToLong"},
    {dos_lvo::FreeArgs, DosOps::free_args, "FreeArgs"},
    {dos_lvo::SetVar, DosOps::set_var, "SetVar"},
    {dos_lvo::GetVar, DosOps::get_var, "GetVar"},
    {dos_lvo::PutStr, DosOps::put_str, "PutStr"},
    {dos_lvo::VPrintf, DosOps::vprintf, "VPrintf"},
};

}

bool Dos::init(Machine& m, Exec& exec, const AdfVolume& volume, ConsoleWrite out, void* ctx) {
    m_ = nullptr;
    exec_ = &exec;
    volume_ = volume;
    out_ = out;
    out_ctx_ = ctx;
    base_ = 0;
    process_ = 0;
    for (FileSlot& f : files_) {
        f = FileSlot{Stream::Closed, DirEntry{}, 0};
    }
    for (LockSlot& l : lock_slots_) {
        l.used = false;
    }
    for (ArgBlock& a : arg_blocks_) {
        a = ArgBlock{0, 0, 0, false};
    }
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
    port_ = block_ + kPortAt;
    process_ = block_ + kProcessAt;
    cli_ = block_ + kCliAt;
    command_name_ = block_ + kCommandNameAt;
    handles_ = block_ + kHandlesAt;
    locks_ = block_ + kLocksAt;
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
    put_bstr(b, block_ + kSetNameAt, set_name, 31);
    put_bstr(b, block_ + kPromptAt, kPrompt, 4);
    put_bstr(b, block_ + kCommandFileAt, "", 0);
    put_bstr(b, command_name_, "", 0);
    b.w32(cli_ + cli_SetName, (block_ + kSetNameAt) >> 2);
    b.w32(cli_ + cli_CommandName, command_name_ >> 2);
    b.w32(cli_ + cli_FailLevel, kFailLevel);
    b.w32(cli_ + cli_Prompt, (block_ + kPromptAt) >> 2);
    b.w32(cli_ + cli_StandardInput, b.r32(process_ + pr_CIS));
    b.w32(cli_ + cli_CurrentInput, b.r32(process_ + pr_CIS));
    b.w32(cli_ + cli_CommandFile, (block_ + kCommandFileAt) >> 2);
    b.w32(cli_ + cli_Interactive, DOSTRUE);
    b.w32(cli_ + cli_CurrentOutput, b.r32(process_ + pr_COS));
    b.w32(cli_ + cli_DefaultStack, kStackBytes / 4);
    b.w32(cli_ + cli_StandardOutput, b.r32(process_ + pr_COS));

    // The console streams.
    files_[kConsoleIn] = FileSlot{Stream::Console, DirEntry{}, 0};
    files_[kConsoleOut] = FileSlot{Stream::Console, DirEntry{}, 0};
    DosOps::init_handle(*this, b, kConsoleIn, true);
    DosOps::init_handle(*this, b, kConsoleOut, true);
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
    char resolved[kMaxPath];
    DirEntry entry{};
    u32 error = 0;
    if (!DosOps::locate(*this, b, name, resolved, entry, error)) {
        if (b.ok()) {
            DosOps::set_io_err(*this, b, error);
        }
        return 0;
    }
    if (entry.is_dir) {
        DosOps::set_io_err(*this, b, ERROR_OBJECT_WRONG_TYPE);
        return 0;
    }
    if (entry.size == 0) {
        DosOps::set_io_err(*this, b, ERROR_FILE_NOT_OBJECT);
        return 0;
    }
    // The file is read into guest memory for the loader and dropped after.
    const u32 file = exec_->alloc(entry.size, MEMF_PUBLIC);
    if (file == 0) {
        DosOps::set_io_err(*this, b, ERROR_NO_FREE_STORE);
        return 0;
    }
    u32 got = 0;
    if (adf_read(volume_, entry, 0, m_->mem.base + file, entry.size, got) != FsError::Ok) {
        exec_->free(file, entry.size);
        DosOps::set_io_err(*this, b, ERROR_NOT_A_DOS_DISK);
        return 0;
    }
    // A file that does not start with HUNK_HEADER is no executable; one
    // that does but fails to load is a bad one.
    u32 magic = 0;
    if (got < 4 || !m_->mem.read32(file, magic) || magic != hunk_detail::kHunkHeader) {
        exec_->free(file, entry.size);
        DosOps::set_io_err(*this, b, ERROR_FILE_NOT_OBJECT);
        return 0;
    }
    SegmentTracker tracker{exec_, {}, {}, 0};
    u32 seglist = 0;
    const HunkError loaded = hunk_load(m_->mem.base + file, got, m_->mem,
                                       HunkAllocator{SegmentTracker::alloc, &tracker}, seglist);
    exec_->free(file, entry.size);
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

}
