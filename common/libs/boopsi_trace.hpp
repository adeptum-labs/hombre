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

#include "../format.hpp"
#include "../log.hpp"
#include "../m68k_jit.hpp"
#include "../types.hpp"
#include "boopsi.hpp"
#include "tags.hpp"

#include <algorithm>

// The BOOPSI trace of a kernel built with -DHOMBRE_BOOPSI_TRACE: a line on
// the serial console for each method, as the reference for classes written
// to behave as the 3.1 ones do.
//
// Every line starts "boopsi: $<task> <depth>": the task the method runs in
// and how many traced calls that task has open around it.
//   host <dispatcher> obj $<o> <method> ...  C++ sends the method
//   guest <class> obj ...                    68000 code reached a class
//                                            written in C++ through h_Entry
//   68k <class> obj ...                      a class whose dispatcher is
//                                            68000 code is entered by 68000
//                                            code, as DoSuperMethod does
//   = $<result> [stored $<value>]            a host or guest call returns;
//                                            for OM_GET what it stored
//   no class <name> / made class <c> over <super> inst <bytes>
// A dispatcher that is a Hook and no class shows as "hook $<address>". The
// method is followed by its tag list (OM_NEW, OM_SET, OM_UPDATE,
// OM_NOTIFY: the GadgetInfo and flags, then up to kTags items on lines of
// their own), the attribute and storage of OM_GET, the arguments of the
// other methods it knows, or, for any other, "raw" and the next three longs,
// which may lie past a shorter message. " ..." marks a tag list cut short
// and " [cut]" a line too long for the console.
//
// Guest memory is only peeked at: a bad pointer shows as "?" and never ends
// the run or changes the CPU. Every function is inline and is called only
// where kBoopsiTrace keeps the call, so a kernel without the flag holds none
// of them, while the compiler still checks them and test_boopsi_trace runs
// them.

namespace hombre {

#if defined(HOMBRE_BOOPSI_TRACE)
inline constexpr bool kBoopsiTrace = true;
#else
inline constexpr bool kBoopsiTrace = false;
#endif

namespace boopsi_trace {

inline constexpr u32 kTags = 16;
inline constexpr u32 kTagsPerLine = 4;
inline constexpr u32 kTagSteps = 256;
inline constexpr u32 kChainSteps = 16;
inline constexpr u32 kMaxName = 64;
inline constexpr u32 kRawArgs = 3;

// Reads guest memory as the translated code sees it, through the address
// mask, without the fault a native::Bus raises.
class Peek {
public:
    explicit Peek(const m68k::Machine& m)
        : mem_(m.mem), mask_(m.jit != nullptr ? m.jit->address_mask() : 0xFFFFFFFF) {}

    bool r8(u32 addr, u32& v) const { return mem_.read8(addr & mask_, v); }
    bool r16(u32 addr, u32& v) const { return (addr & 1) == 0 && mem_.read16(addr & mask_, v); }
    bool r32(u32 addr, u32& v) const { return (addr & 1) == 0 && mem_.read32(addr & mask_, v); }

private:
    const m68k::GuestMemory& mem_;
    u32 mask_;
};

// One console line, marked when it is too long to keep whole.
class Line {
public:
    static constexpr usize kRoom = 150;

    Line& text(const char* s) {
        while (*s != '\0') {
            put(*s++);
        }
        return *this;
    }
    Line& hex(u32 v) {
        put('$');
        return number(v, 16);
    }
    Line& dec(u32 v) { return number(v, 10); }
    // The long at addr, or "?" where guest memory has none.
    Line& peeked(const Peek& p, u32 addr) {
        u32 v = 0;
        return p.r32(addr, v) ? hex(v) : text("?");
    }
    bool empty() const { return length_ == 0; }
    void send() {
        buffer_[length_] = '\0';
        LogLine().text(buffer_).text(cut_ ? " [cut]" : "").send();
        length_ = 0;
        cut_ = false;
    }

private:
    void put(char c) {
        if (length_ < kRoom) {
            buffer_[length_++] = c;
        } else {
            cut_ = true;
        }
    }
    Line& number(u32 v, unsigned base) {
        char digits[12];
        if (format_u64(digits, sizeof digits, v, base) != 0) {
            text(digits);
        }
        return *this;
    }

    char buffer_[kRoom + 1];
    usize length_ = 0;
    bool cut_ = false;
};

struct Method {
    u32 id;
    const char* name;
    u32 args;  // longs after the MethodID shown
};

inline constexpr Method kMethods[] = {
    {ndk::OM_NEW, "OM_NEW", 0},           {ndk::OM_DISPOSE, "OM_DISPOSE", 0},
    {ndk::OM_SET, "OM_SET", 0},           {ndk::OM_GET, "OM_GET", 0},
    {ndk::OM_ADDTAIL, "OM_ADDTAIL", 1},   {ndk::OM_REMOVE, "OM_REMOVE", 0},
    {ndk::OM_NOTIFY, "OM_NOTIFY", 0},     {ndk::OM_UPDATE, "OM_UPDATE", 0},
    {ndk::OM_ADDMEMBER, "OM_ADDMEMBER", 1}, {ndk::OM_REMMEMBER, "OM_REMMEMBER", 1},
    {ndk::GM_HITTEST, "GM_HITTEST", 2},   {ndk::GM_RENDER, "GM_RENDER", 3},
    {ndk::GM_GOACTIVE, "GM_GOACTIVE", 3}, {ndk::GM_HANDLEINPUT, "GM_HANDLEINPUT", 3},
    {ndk::GM_GOINACTIVE, "GM_GOINACTIVE", 2}, {ndk::GM_HELPTEST, "GM_HELPTEST", 2},
    {ndk::GM_LAYOUT, "GM_LAYOUT", 2},     {ndk::IM_DRAW, "IM_DRAW", 4},
    {ndk::IM_HITTEST, "IM_HITTEST", 1},   {ndk::IM_ERASE, "IM_ERASE", 3},
    {ndk::IM_MOVE, "IM_MOVE", 4},         {ndk::IM_DRAWFRAME, "IM_DRAWFRAME", 5},
    {ndk::IM_FRAMEBOX, "IM_FRAMEBOX", 4}, {ndk::IM_HITFRAME, "IM_HITFRAME", 2},
    {ndk::IM_ERASEFRAME, "IM_ERASEFRAME", 3}, {0x402, "ICM_SETLOOP", 0},
    {0x403, "ICM_CLEARLOOP", 0},          {0x404, "ICM_CHECKLOOP", 0},
};

inline const Method* method_of(u32 id) {
    for (const Method& m : kMethods) {
        if (m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

inline bool has_tag_list(u32 method) {
    return method == ndk::OM_NEW || method == ndk::OM_SET || method == ndk::OM_UPDATE ||
           method == ndk::OM_NOTIFY;
}

// The NUL-terminated name at addr, if it is one of printable characters
// shorter than kMaxName.
inline bool name_at(const Peek& p, u32 addr, char (&out)[kMaxName]) {
    for (u32 i = 0; i < kMaxName; ++i) {
        u32 c = 0;
        if (addr == 0 || !p.r8(addr + i, c) || (c != 0 && (c < 0x20 || c > 0x7E))) {
            return false;
        }
        out[i] = static_cast<char>(c);
        if (c == 0) {
            return true;
        }
    }
    return false;
}

inline bool public_name(const Peek& p, u32 cl, char (&out)[kMaxName]) {
    u32 id = 0;
    return p.r32(cl + ndk::cl_ID, id) && name_at(p, id, out);
}

// Whether cl is on the chain of superclasses from first on.
inline bool on_chain(const Peek& p, u32 first, u32 cl) {
    u32 at = first;
    for (u32 step = 0; at != 0 && step < kChainSteps; ++step) {
        if (at == cl) {
            return true;
        }
        if (!p.r32(at + ndk::cl_Super, at)) {
            return false;
        }
    }
    return false;
}

// Whether the dispatcher is a class of the object rather than a gadget's
// Hook: one of the superclasses from the object's class on, or, for
// OM_NEW, from the class being made, which is the object then.
inline bool is_class_of(const Peek& p, u32 cl, u32 object, u32 method) {
    if (method == ndk::OM_NEW) {
        return on_chain(p, object, cl);
    }
    u32 own = 0;
    return object >= ndk::Object_SIZEOF &&
           p.r32(object - ndk::Object_SIZEOF + ndk::o_Class, own) && on_chain(p, own, cl);
}

// The class by its public name, or, for a private class, by its address
// and the name of the nearest public class above it.
inline void class_text(const Peek& p, Line& line, u32 cl) {
    char name[kMaxName];
    if (public_name(p, cl, name)) {
        line.text(name);
        return;
    }
    line.hex(cl);
    u32 super = 0;
    for (u32 step = 0; p.r32(cl + ndk::cl_Super, super) && super != 0 && step < kChainSteps;
         ++step) {
        if (public_name(p, super, name)) {
            line.text("<").text(name);
            return;
        }
        cl = super;
    }
}

// The items of the tag list, kTagsPerLine to a line, the first kTags,
// TAG_IGNORE, TAG_MORE and TAG_SKIP followed as utility.library does.
inline void tag_lines(const Peek& p, u32 list) {
    Line line;
    u32 shown = 0;
    u32 item = list;
    bool more = false;
    bool bad = false;
    u32 step = 0;
    for (; item != 0 && step < kTagSteps; ++step) {
        u32 tag = 0;
        u32 data = 0;
        if (!p.r32(item, tag) || !p.r32(item + 4, data)) {
            bad = true;
            break;
        }
        if (tag == tags::kTagDone) {
            break;
        }
        if (tag == tags::kTagIgnore || tag == tags::kTagMore || tag == tags::kTagSkip) {
            item = tag == tags::kTagMore ? data
                                         : item + tags::kItemBytes *
                                                      (tag == tags::kTagSkip ? data + 1 : 1);
            continue;
        }
        if (shown == kTags) {
            more = true;
            break;
        }
        if (line.empty()) {
            line.text("boopsi:   ");
        }
        line.text(" ").hex(tag).text("=").hex(data);
        if (++shown % kTagsPerLine == 0) {
            line.send();
        }
        item += tags::kItemBytes;
    }
    more = more || step == kTagSteps;
    if ((bad || more) && line.empty()) {
        line.text("boopsi:   ");
    }
    if (bad) {
        line.text(" ? at ").hex(item);
    }
    if (more) {
        line.text(" ...");
    }
    if (!line.empty()) {
        line.send();
    }
}

// The count of traced calls open in the task, taken from the spare entry
// when every entry is held by another task.
inline BoopsiTraceState::Depth& depth_of(BoopsiTraceState& s, u32 task) {
    BoopsiTraceState::Depth* free_entry = nullptr;
    for (BoopsiTraceState::Depth& d : s.depths) {
        if (d.used && d.task == task) {
            return d;
        }
        if (!d.used && free_entry == nullptr) {
            free_entry = &d;
        }
    }
    if (free_entry == nullptr) {
        return s.spare;
    }
    *free_entry = BoopsiTraceState::Depth{true, task, 0};
    return *free_entry;
}

inline u32 depth(BoopsiTraceState& s, u32 task) { return depth_of(s, task).depth; }

// Forgets a task exec removed, with whatever calls it left open.
inline void drop_task(BoopsiTraceState& s, u32 task) {
    for (BoopsiTraceState::Depth& d : s.depths) {
        if (d.used && d.task == task) {
            d = BoopsiTraceState::Depth{};
        }
    }
}

inline void reset(BoopsiTraceState& s) {
    for (BoopsiTraceState::Depth& d : s.depths) {
        d = BoopsiTraceState::Depth{};
    }
    s.spare = BoopsiTraceState::Depth{};
    s.expected_entry = 0;
    s.classes.clear();
}

// What a call's return line needs.
struct Frame {
    u32 task;
    u32 method;
    u32 storage;  // OM_GET's
};

// The line for a method and the lines of its tag list; via names where it
// came from.
inline Frame message_lines(const Peek& p, u32 task, u32 depth, const char* via, u32 cl,
                           u32 object, u32 msg) {
    Frame frame{task, 0xFFFFFFFF, 0};
    Line line;
    line.text("boopsi: ").hex(task).text(" ").dec(depth).text(" ").text(via).text(" ");
    u32 method = 0;
    const bool readable = p.r32(msg, method);
    if (cl != 0 && readable && !is_class_of(p, cl, object, method)) {
        line.text("hook ").hex(cl);
    } else if (cl != 0) {
        class_text(p, line, cl);
    }
    line.text(" obj ").hex(object).text(" ");
    if (!readable) {
        line.text("msg ").hex(msg).text(" ?");
        line.send();
        return frame;
    }
    frame.method = method;
    const Method* known = method_of(method);
    if (known != nullptr) {
        line.text(known->name);
    } else {
        line.hex(method).text(" raw");
    }
    if (has_tag_list(method)) {
        line.text(" ginfo ").peeked(p, msg + ndk::opu_GInfo);
        if (method == ndk::OM_UPDATE || method == ndk::OM_NOTIFY) {
            line.text(" flags ").peeked(p, msg + ndk::opu_Flags);
        }
    } else if (method == ndk::OM_GET) {
        line.text(" attr ").peeked(p, msg + 4).text(" storage ").peeked(p, msg + 8);
        p.r32(msg + 8, frame.storage);
    } else {
        const u32 args = known != nullptr ? known->args : kRawArgs;
        for (u32 i = 1; i <= args; ++i) {
            line.text(" ").peeked(p, msg + i * 4);
        }
    }
    line.send();
    u32 list = 0;
    if (has_tag_list(method) && p.r32(msg + 4, list)) {
        tag_lines(p, list);
    }
    return frame;
}

// A call from C++ (via "host") or from 68000 code into a class written in
// C++ (via "guest"), before the dispatcher runs.
inline Frame call(BoopsiTraceState& s, const m68k::Machine& m, u32 task, const char* via, u32 cl,
                  u32 object, u32 msg) {
    BoopsiTraceState::Depth& d = depth_of(s, task);
    return message_lines(Peek(m), task, d.depth++, via, cl, object, msg);
}

inline void ret(BoopsiTraceState& s, const m68k::Machine& m, const Frame& frame, u32 result) {
    BoopsiTraceState::Depth& d = depth_of(s, frame.task);
    if (d.depth != 0) {
        --d.depth;
    }
    Line line;
    line.text("boopsi: ").hex(frame.task).text(" ").dec(d.depth).text(" = ").hex(result);
    if (frame.method == ndk::OM_GET && result != 0) {
        line.text(" stored ").peeked(Peek(m), frame.storage);
    }
    line.send();
    if (d.depth == 0 && &d != &s.spare) {
        d = BoopsiTraceState::Depth{};
    }
}

// Called before each block: a block at the h_Entry of a class MakeClass
// made is 68000 code dispatching a method, unless it is the dispatch C++
// itself starts and logs. The Hook registers hold the class in A0, the
// object in A2 and the message in A1.
inline void watch(BoopsiTraceState& s, const m68k::Machine& m, u32 task) {
    const u32 pc = m.cpu.pc;
    if (pc == 0) {
        return;
    }
    if (pc == s.expected_entry) {
        s.expected_entry = 0;
        return;
    }
    const Peek p(m);
    for (const u32 cl : s.classes) {
        u32 entry = 0;
        if (p.r32(cl + ndk::h_Entry, entry) && entry == pc) {
            message_lines(p, task, depth(s, task), "68k", m.cpu.a[0], m.cpu.a[2], m.cpu.a[1]);
            return;
        }
    }
}

inline void missing_class(const m68k::Machine& m, u32 id) {
    const Peek p(m);
    char name[kMaxName];
    Line line;
    line.text("boopsi: no class ");
    if (name_at(p, id, name)) {
        line.text(name);
    } else {
        line.hex(id);
    }
    line.send();
}

inline void made_class(BoopsiTraceState& s, const m68k::Machine& m, u32 cl) {
    s.classes.push_back(cl);
    const Peek p(m);
    Line line;
    line.text("boopsi: made class ");
    class_text(p, line, cl);
    line.text(" over ");
    u32 super = 0;
    if (p.r32(cl + ndk::cl_Super, super)) {
        class_text(p, line, super);
    } else {
        line.text("?");
    }
    u32 inst = 0;
    line.text(" inst ");
    if (p.r16(cl + ndk::cl_InstSize, inst)) {
        line.dec(inst);
    } else {
        line.text("?");
    }
    line.send();
}

inline void freed_class(BoopsiTraceState& s, u32 cl) { std::erase(s.classes, cl); }

}

}
