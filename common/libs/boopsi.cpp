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

#include "boopsi.hpp"

#include "boopsi_trace.hpp"

#include "gfx.hpp"
#include "tags.hpp"

#include <algorithm>

namespace hombre {

using m68k::Machine;
using native::Bus;
using native::LogLine;
using namespace ndk;

namespace {

constexpr u32 kMaxClassName = 64;

// struct Image, the instance data of imageclass.
constexpr u32 ig_LeftEdge = 0;
constexpr u32 ig_TopEdge = 2;
constexpr u32 ig_Width = 4;
constexpr u32 ig_Height = 6;
constexpr u32 ig_Depth = 8;
constexpr u32 ig_ImageData = 10;
constexpr u32 ig_PlanePick = 14;
constexpr u32 ig_PlaneOnOff = 15;
constexpr u32 Image_SIZEOF = 20;

// struct sysiClass's instance data after imageclass's Image.
constexpr u32 si_Which = Image_SIZEOF;
constexpr u32 si_Size = Image_SIZEOF + 2;
constexpr u32 si_DrawInfo = Image_SIZEOF + 4;
constexpr u32 SysImage_SIZEOF = 8;

// struct TextFont
constexpr u32 tf_YSize = 20;

struct SysImageDimensions {
    u32 which;
    u16 width;
    u16 height;
};

// The MEDRES size of every sysiclass image; HIRES uses it too.
constexpr SysImageDimensions kSysImageDimensions[] = {
    {DEPTHIMAGE, 23, 11}, {ZOOMIMAGE, 23, 11},  {SDEPTHIMAGE, 23, 11},
    {CLOSEIMAGE, 20, 11}, {SIZEIMAGE, 18, 10},  {LEFTIMAGE, 16, 10},
    {RIGHTIMAGE, 16, 10}, {UPIMAGE, 18, 11},    {DOWNIMAGE, 18, 11},
    {CHECKIMAGE, 26, 11}, {MXIMAGE, 17, 9},     {MENUCHECK, 15, 8},
    {AMIGAKEY, 23, 8},
};

// The size SYSIA_Which and SYSIA_Size give an image, with a reference font
// setting the height of those that follow one; false for an unknown Which.
bool sys_image_dimensions(Bus& b, u32 which, u32 size, u32 reference_font, u16& width,
                          u16& height) {
    const SysImageDimensions* found = nullptr;
    for (const SysImageDimensions& dimensions : kSysImageDimensions) {
        if (dimensions.which == which) {
            found = &dimensions;
            break;
        }
    }
    if (found == nullptr) {
        return false;
    }
    width = found->width;
    height = found->height;
    if (size == SYSISIZE_LOWRES && which != CHECKIMAGE && which != MXIMAGE) {
        width = static_cast<u16>((width + 1) / 2);
    }
    // The size gadget and the arrows fit a window's borders, which no font
    // changes.
    const bool border_sized = which == SIZEIMAGE || which == LEFTIMAGE || which == RIGHTIMAGE ||
                              which == UPIMAGE || which == DOWNIMAGE;
    if (reference_font != 0 && !border_sized) {
        const u16 y_size = b.r16(reference_font + tf_YSize);
        if (!b.ok()) {
            return false;
        }
        height = static_cast<u16>(which == MENUCHECK || which == AMIGAKEY ? y_size : y_size + 3);
    }
    return true;
}

constexpr u32 GTYP_GZZGADGET = 0x2000;

// icclass's instance data: the target, the map and the loop counter.
constexpr u32 ic_Target = 0;
constexpr u32 ic_Map = 4;
constexpr u32 ic_Loop = 8;
constexpr u32 kIcBytes = 12;

// modelclass's: the MinList of its members, linked through their object
// headers as rootclass's OM_ADDTAIL links them.
constexpr u32 model_Members = 0;
constexpr u32 kModelBytes = 12;
constexpr u32 kMaxModelMembers = 1024;

// pointerclass's: the POINTERA_ attributes, a long each.
constexpr u32 kPointerAttrs = 6;
constexpr u32 kPointerBytes = kPointerAttrs * 4;

// The block a class lives in: the IClass and its name. cl_Reserved holds
// the block's size for FreeClass.
constexpr u32 cl_Reserved = 20;

// Copies the guest string at name, at most kMaxClassName - 1 bytes, into
// out. False on a bad address or an overlong name.
bool read_name(Bus& b, u32 name, char (&out)[kMaxClassName]) {
    for (u32 i = 0; i < kMaxClassName; ++i) {
        out[i] = static_cast<char>(b.r8(name + i));
        if (!b.ok()) {
            return false;
        }
        if (out[i] == '\0') {
            return true;
        }
    }
    return false;
}

bool same(const char* a, const char* b) {
    for (;; ++a, ++b) {
        if (*a != *b) {
            return false;
        }
        if (*a == '\0') {
            return true;
        }
    }
}

}

struct Boopsi::Natives {
    static Boopsi& of(void* ctx) { return *static_cast<Boopsi*>(ctx); }

    // A registered class as a Hook calls it: the class in A0, the object
    // in A2 and the message in A1.
    static void entry(void* ctx, Machine& m) {
        const NativeClass& native = *static_cast<const NativeClass*>(ctx);
        Boopsi& boopsi = *native.boopsi;
        const u32 cl = m.cpu.a[0];
        const u32 object = m.cpu.a[2];
        const u32 msg = m.cpu.a[1];
        if constexpr (kBoopsiTrace) {
            const boopsi_trace::Frame frame = boopsi_trace::call(
                boopsi.trace_, m, boopsi.exec_->this_task(), "guest", cl, object, msg);
            const u32 result = native.dispatcher(native.ctx, cl, object, msg);
            boopsi_trace::ret(boopsi.trace_, m, frame, result);
            native::set_result(m, result);
        } else {
            native::set_result(m, native.dispatcher(native.ctx, cl, object, msg));
        }
    }
    static u32 root(void* ctx, u32 cl, u32 object, u32 msg) {
        return of(ctx).root_dispatch(cl, object, msg);
    }
    static u32 gadget(void* ctx, u32 cl, u32 object, u32 msg) {
        return of(ctx).gadget_dispatch(cl, object, msg);
    }
    static u32 image(void* ctx, u32 cl, u32 object, u32 msg) {
        return of(ctx).image_dispatch(cl, object, msg);
    }
    static u32 sysi(void* ctx, u32 cl, u32 object, u32 msg) {
        return of(ctx).sysi_dispatch(cl, object, msg);
    }
    static u32 ic(void* ctx, u32 cl, u32 object, u32 msg) {
        return of(ctx).ic_dispatch(cl, object, msg);
    }
    static u32 model(void* ctx, u32 cl, u32 object, u32 msg) {
        return of(ctx).model_dispatch(cl, object, msg);
    }
    static u32 pointer(void* ctx, u32 cl, u32 object, u32 msg) {
        return of(ctx).pointer_dispatch(cl, object, msg);
    }

    static void task_ended(void* ctx, u32 task, bool) {
        of(ctx).task_ended(task);
        if constexpr (kBoopsiTrace) {
            boopsi_trace::drop_task(of(ctx).trace_, task);
        }
    }
    static void trace_block(void* ctx, Machine& m) {
        Boopsi& boopsi = of(ctx);
        boopsi_trace::watch(boopsi.trace_, m, boopsi.exec_->this_task());
    }
};

bool Boopsi::init(Machine& m, Exec& exec) {
    machine_ = &m;
    exec_ = &exec;
    if constexpr (kBoopsiTrace) {
        boopsi_trace::reset(trace_);
        if (m.jit != nullptr) {
            m.jit->set_block_watch(Natives::trace_block, this);
        }
    }
    public_.clear();
    ica_.clear();
    notify_depths_.clear();
    loop_holds_.clear();
    exec.set_task_end_hook(Natives::task_ended, this);
    native_count_ = 0;
    root_ = make_native_class("rootclass", 0, 0, Natives::root, this);
    gadget_ = root_ != 0
                  ? make_native_class("gadgetclass", root_, ExtGadget_SIZEOF, Natives::gadget, this)
                  : 0;
    image_ = root_ != 0
                 ? make_native_class("imageclass", root_, Image_SIZEOF, Natives::image, this)
                 : 0;
    sysi_ = image_ != 0
                ? make_native_class("sysiclass", image_, SysImage_SIZEOF, Natives::sysi, this)
                : 0;
    ic_ = root_ != 0 ? make_native_class("icclass", root_, kIcBytes, Natives::ic, this) : 0;
    model_ = ic_ != 0 ? make_native_class("modelclass", ic_, kModelBytes, Natives::model, this)
                      : 0;
    pointer_ = root_ != 0
                   ? make_native_class("pointerclass", root_, kPointerBytes, Natives::pointer, this)
                   : 0;
    return root_ != 0 && gadget_ != 0 && image_ != 0 && sysi_ != 0 && ic_ != 0 && model_ != 0 &&
           pointer_ != 0;
}

// A public class whose dispatcher is C++, behind an A-line stub that 68000
// code can call through h_Entry, and the registry entry that finds the C++
// by that stub.
u32 Boopsi::make_native_class(const char* name, u32 super, u32 inst_size,
                              NativeDispatcher dispatcher, void* ctx) {
    if (native_count_ >= kMaxNativeClasses) {
        LogLine().text("intuition: native class registry full for ").text(name).send();
        return 0;
    }
    NativeClass& native = natives_[native_count_];
    const u16 id = machine_->jit->add_native(Natives::entry, &native, name);
    if (id == 0) {
        LogLine().text("intuition: no native ids left for ").text(name).send();
        return 0;
    }
    const u32 stub = exec_->alloc(4, MEMF_PUBLIC);
    if (stub == 0) {
        return 0;
    }
    native = NativeClass{this, stub, dispatcher, ctx};
    Bus b(*machine_);
    b.w16(stub, native::kALine | id);
    b.w16(stub + 2, native::kRts);
    u32 length = 0;
    while (name[length] != '\0') {
        ++length;
    }
    const u32 bytes = IClass_SIZEOF + length + 1;
    const u32 cl = exec_->alloc(bytes, MEMF_PUBLIC | MEMF_CLEAR);
    if (cl == 0) {
        return 0;
    }
    for (u32 i = 0; i <= length; ++i) {
        b.w8(cl + IClass_SIZEOF + i, static_cast<u8>(name[i]));
    }
    b.w32(cl + h_Entry, stub);
    b.w32(cl + cl_Reserved, bytes);
    b.w32(cl + cl_Super, super);
    b.w32(cl + cl_ID, cl + IClass_SIZEOF);
    const u32 offset = super != 0 ? b.r16(super + cl_InstOffset) + b.r16(super + cl_InstSize) : 0;
    b.w16(cl + cl_InstOffset, offset);
    b.w16(cl + cl_InstSize, inst_size);
    if (super != 0) {
        b.w32(super + cl_SubclassCount, b.r32(super + cl_SubclassCount) + 1);
    }
    machine_->jit->flush();
    if (!b.ok()) {
        return 0;
    }
    ++native_count_;
    add_class(cl);
    return cl;
}

// MakeClass: a new class over the superclass, by pointer or by public
// name. Its dispatcher is the caller's to fill in.
u32 Boopsi::make_class(u32 id, u32 super_id, u32 super_class, u32 inst_size, u32 flags) {
    (void)flags;
    Bus b(*machine_);
    const u32 super = super_class != 0 ? super_class : (super_id != 0 ? find_class(super_id) : 0);
    if (super == 0) {
        if constexpr (kBoopsiTrace) {
            boopsi_trace::missing_class(*machine_, super_id);
        }
        return 0;
    }
    char name[kMaxClassName];
    u32 length = 0;
    if (id != 0) {
        if (!read_name(b, id, name)) {
            return 0;
        }
        while (name[length] != '\0') {
            ++length;
        }
    }
    const u32 bytes = IClass_SIZEOF + length + 1;
    const u32 cl = exec_->alloc(bytes, MEMF_PUBLIC | MEMF_CLEAR);
    if (cl == 0) {
        return 0;
    }
    for (u32 i = 0; i < length; ++i) {
        b.w8(cl + IClass_SIZEOF + i, static_cast<u8>(name[i]));
    }
    b.w32(cl + cl_Reserved, bytes);
    b.w32(cl + cl_Super, super);
    b.w32(cl + cl_ID, id != 0 ? cl + IClass_SIZEOF : 0);
    b.w16(cl + cl_InstOffset, b.r16(super + cl_InstOffset) + b.r16(super + cl_InstSize));
    b.w16(cl + cl_InstSize, inst_size & 0xFFFF);
    b.w32(super + cl_SubclassCount, b.r32(super + cl_SubclassCount) + 1);
    if (!b.ok()) {
        exec_->free(cl, bytes);
        return 0;
    }
    if constexpr (kBoopsiTrace) {
        boopsi_trace::made_class(trace_, *machine_, cl);
    }
    return cl;
}

void Boopsi::add_class(u32 cl) {
    if (cl == 0) {
        return;
    }
    Bus b(*machine_);
    if ((b.r32(cl + cl_Flags) & CLF_INLIST) != 0) {
        return;
    }
    if (public_.size() >= kMaxPublicClasses) {
        LogLine().text("intuition: public class table full").send();
        return;
    }
    public_.push_back(cl);
    b.w32(cl + cl_Flags, b.r32(cl + cl_Flags) | CLF_INLIST);
}

void Boopsi::remove_class(u32 cl) {
    if (cl == 0) {
        return;
    }
    std::erase(public_, cl);
    Bus b(*machine_);
    b.w32(cl + cl_Flags, b.r32(cl + cl_Flags) & ~CLF_INLIST);
}

// FreeClass: false while the class has objects or subclasses. The system
// classes stay.
bool Boopsi::free_class(u32 cl) {
    if (cl == 0) {
        return false;
    }
    Bus b(*machine_);
    const u32 entry = b.r32(cl + h_Entry);
    for (u32 i = 0; i < native_count_; ++i) {
        if (natives_[i].entry == entry) {
            return false;
        }
    }
    if (b.r32(cl + cl_ObjectCount) != 0 || b.r32(cl + cl_SubclassCount) != 0 || !b.ok()) {
        return false;
    }
    remove_class(cl);
    const u32 super = b.r32(cl + cl_Super);
    if (super != 0) {
        const u32 count = b.r32(super + cl_SubclassCount);
        b.w32(super + cl_SubclassCount, count != 0 ? count - 1 : 0);
    }
    const u32 bytes = b.r32(cl + cl_Reserved);
    if (!b.ok()) {
        return false;
    }
    if constexpr (kBoopsiTrace) {
        boopsi_trace::freed_class(trace_, cl);
    }
    exec_->free(cl, bytes);
    return true;
}

u32 Boopsi::find_class(u32 id) const {
    Bus b(*machine_);
    char name[kMaxClassName];
    return id != 0 && read_name(b, id, name) ? find_class_named(name) : 0;
}

u32 Boopsi::find_class_named(const char* name) const {
    Bus b(*machine_);
    for (const u32 cl : public_) {
        char other[kMaxClassName];
        if (cl != 0 && read_name(b, b.r32(cl + cl_ID), other) && same(name, other)) {
            return cl;
        }
    }
    return 0;
}

u32 Boopsi::class_of(u32 object) const {
    if (object < Object_SIZEOF) {
        return 0;
    }
    Bus b(*machine_);
    const u32 cl = b.r32(object - Object_SIZEOF + o_Class);
    return b.ok() ? cl : 0;
}

// An object of one of the classes: its header names a class whose
// dispatcher is set.
bool Boopsi::is_object(u32 object) const {
    if (object < Object_SIZEOF || (object & 1) != 0) {
        return false;
    }
    const u32 cl = class_of(object);
    // A header read from something that is not an object may be anything:
    // only an even address in memory can be a class.
    if (cl == 0 || (cl & 1) != 0 || native::guest_address(*machine_, cl + h_Entry) == 0) {
        return false;
    }
    Bus b(*machine_);
    const u32 entry = b.r32(cl + h_Entry);
    return b.ok() && entry != 0;
}

u32 Boopsi::custom_dispatcher(u32 gadget) const {
    if (is_object(gadget)) {
        return class_of(gadget);
    }
    if (gadget == 0 || (gadget & 1) != 0 ||
        native::guest_address(*machine_, gadget + ndk::gg_MutualExclude + 4) == 0) {
        return 0;
    }
    Bus b(*machine_);
    const u32 hook = b.r32(gadget + ndk::gg_MutualExclude);
    if (!b.ok() || hook == 0 || (hook & 1) != 0 ||
        native::guest_address(*machine_, hook + h_Entry + 4) == 0) {
        return 0;
    }
    return b.r32(hook + h_Entry) != 0 ? hook : 0;
}

u32 Boopsi::object_bytes(u32 cl) const {
    Bus b(*machine_);
    return Object_SIZEOF + b.r16(cl + cl_InstOffset) + b.r16(cl + cl_InstSize);
}

u32 Boopsi::do_method(u32 object, u32 msg) {
    return object != 0 ? coerce_method(class_of(object), object, msg) : 0;
}

u32 Boopsi::coerce_method(u32 cl, u32 object, u32 msg) {
    if constexpr (kBoopsiTrace) {
        const boopsi_trace::Frame frame =
            boopsi_trace::call(trace_, *machine_, exec_->this_task(), "host", cl, object, msg);
        const u32 result = dispatch(cl, object, msg);
        boopsi_trace::ret(trace_, *machine_, frame, result);
        return result;
    }
    return dispatch(cl, object, msg);
}

// The class's dispatcher on the message: in C++ for a class of the
// registry, as a nested guest call for one written in 68000 code.
u32 Boopsi::dispatch(u32 cl, u32 object, u32 msg) {
    if (cl == 0) {
        return 0;
    }
    Bus b(*machine_);
    const u32 entry = b.r32(cl + h_Entry);
    if (!b.ok() || entry == 0) {
        return 0;
    }
    for (u32 i = 0; i < native_count_; ++i) {
        const NativeClass& native = natives_[i];
        if (native.entry == entry) {
            return native.dispatcher(native.ctx, cl, object, msg);
        }
    }
    Machine& m = *machine_;
    if (!exec_->enter_nested_run()) {
        return 0;
    }
    u32 d[8];
    u32 a[8];
    for (u32 i = 0; i < 8; ++i) {
        d[i] = m.cpu.d[i];
        a[i] = m.cpu.a[i];
    }
    m.cpu.a[0] = cl;
    m.cpu.a[1] = msg;
    m.cpu.a[2] = object;
    if constexpr (kBoopsiTrace) {
        trace_.expected_entry = entry;
    }
    const m68k::ExitReason why = m.jit->call_guest(entry, kDispatchBlocks);
    if constexpr (kBoopsiTrace) {
        trace_.expected_entry = 0;
    }
    exec_->leave_nested_run();
    if (why != m68k::ExitReason::Returned) {
        return 0;  // the run ended inside the dispatcher
    }
    const u32 result = m.cpu.d[0];
    for (u32 i = 0; i < 8; ++i) {
        m.cpu.d[i] = d[i];
        m.cpu.a[i] = a[i];
    }
    return result;
}

u32 Boopsi::call_super(u32 cl, u32 object, u32 msg) {
    Bus b(*machine_);
    const u32 super = b.r32(cl + cl_Super);
    return b.ok() ? coerce_method(super, object, msg) : 0;
}

u32 Boopsi::send(u32 cl, u32 object, const u32* longs, u32 count) {
    if (count == 0 || count > kMaxMessageLongs) {
        return 0;
    }
    const u32 msg = exec_->alloc(count * 4, MEMF_PUBLIC);
    if (msg == 0) {
        return 0;
    }
    Bus b(*machine_);
    for (u32 i = 0; i < count; ++i) {
        b.w32(msg + i * 4, longs[i]);
    }
    const u32 result = b.ok() ? coerce_method(cl, object, msg) : 0;
    exec_->free(msg, count * 4);
    return result;
}

// NewObjectA: OM_NEW to the class, given or found by public name, with
// the class as the object, as rootclass expects of the true class.
u32 Boopsi::new_object(u32 cl, u32 id, u32 tags) {
    const u32 true_class = cl != 0 ? cl : find_class(id);
    if (true_class == 0) {
        if constexpr (kBoopsiTrace) {
            boopsi_trace::missing_class(*machine_, id);
        }
        return 0;
    }
    const u32 msg[] = {OM_NEW, tags, 0};
    return send(true_class, true_class, msg, 3);
}

void Boopsi::dispose_object(u32 object) {
    if (object == 0) {
        return;
    }
    const u32 msg[] = {OM_DISPOSE};
    send(class_of(object), object, msg, 1);
}

u32 Boopsi::set_attrs(u32 object, u32 tags, u32 ginfo) {
    if (object == 0) {
        return 0;
    }
    const u32 msg[] = {OM_SET, tags, ginfo};
    return send(class_of(object), object, msg, 3);
}

u32 Boopsi::get_attr(u32 attr, u32 object, u32 storage) {
    if (object == 0) {
        return 0;
    }
    const u32 msg[] = {OM_GET, attr, storage};
    return send(class_of(object), object, msg, 3);
}

// NextObject: state holds the node the walk is at, an object's MinNode
// in a list rootclass's OM_ADDTAIL made; the walk moves on to the next.
u32 Boopsi::next_object(u32 state) {
    Bus b(*machine_);
    const u32 node = b.r32(state);
    const u32 next = node != 0 ? b.r32(node) : 0;
    if (!b.ok() || next == 0) {
        return 0;
    }
    b.w32(state, next);
    return node + Object_SIZEOF;
}

// rootclass: makes and frees objects and lists them.
u32 Boopsi::root_dispatch(u32 cl, u32 object, u32 msg) {
    (void)cl;
    Bus b(*machine_);
    const u32 method = b.r32(msg);
    if (!b.ok()) {
        return 0;
    }
    switch (method) {
    case OM_NEW: {
        // The object of OM_NEW is the class the object is made from.
        const u32 true_class = object;
        const u32 bytes = object_bytes(true_class);
        const u32 block = exec_->alloc(bytes, MEMF_PUBLIC | MEMF_CLEAR);
        if (block == 0) {
            return 0;
        }
        b.w32(block + o_Class, true_class);
        b.w32(true_class + cl_ObjectCount, b.r32(true_class + cl_ObjectCount) + 1);
        return b.ok() ? block + Object_SIZEOF : 0;
    }
    case OM_DISPOSE: {
        const u32 true_class = class_of(object);
        if (true_class == 0) {
            return 0;
        }
        const u32 count = b.r32(true_class + cl_ObjectCount);
        b.w32(true_class + cl_ObjectCount, count != 0 ? count - 1 : 0);
        exec_->free(object - Object_SIZEOF, object_bytes(true_class));
        return 0;
    }
    case OM_ADDTAIL: {
        const u32 list = b.r32(msg + 4);
        const u32 node = object - Object_SIZEOF;
        const u32 last = b.r32(list + 8);
        b.w32(node, list + 4);
        b.w32(node + 4, last);
        b.w32(last, node);
        b.w32(list + 8, node);
        return 0;
    }
    case OM_REMOVE: {
        const u32 node = object - Object_SIZEOF;
        const u32 next = b.r32(node);
        const u32 previous = b.r32(node + 4);
        if (next != 0 && previous != 0) {
            b.w32(previous, next);
            b.w32(next + 4, previous);
        }
        // A node taken out twice leaves the list as it is; its successor
        // stays for whoever walks on from it, as after Remove.
        b.w32(node + 4, 0);
        return 0;
    }
    default:
        return 0;
    }
}

// gadgetclass: a custom gadget whose Gadget is the object itself. It
// takes the GA_ attributes; drawing and input are its subclasses' to do.
u32 Boopsi::gadget_dispatch(u32 cl, u32 object, u32 msg) {
    Bus b(*machine_);
    const u32 method = b.r32(msg);
    if (!b.ok()) {
        return 0;
    }
    switch (method) {
    case OM_NEW: {
        const u32 gadget = call_super(cl, object, msg);
        if (gadget == 0) {
            return 0;
        }
        b.w16(gadget + gg_GadgetType, GTYP_CUSTOMGADGET);
        b.w16(gadget + gg_Flags, GFLG_EXTENDED);
        set_gadget_attrs(gadget, b.r32(msg + 4), true);
        return gadget;
    }
    case OM_DISPOSE:
        drop_ica(object);
        return call_super(cl, object, msg);
    case OM_SET:
        return call_super(cl, object, msg) + set_gadget_attrs(object, b.r32(msg + 4), false);
    case OM_UPDATE: {
        // OM_UPDATE is OM_SET that also passes the change on to the target.
        const u32 result = call_super(cl, object, msg) +
                           set_gadget_attrs(object, b.r32(msg + 4), false);
        notify(object, msg);
        return result;
    }
    case OM_NOTIFY:
        return notify(object, msg);
    case OM_GET:
        return get_gadget_attr(object, b.r32(msg + 4), b.r32(msg + 8))
                   ? 1
                   : call_super(cl, object, msg);
    case GM_HITTEST:
        return GMR_GADGETHIT;
    case GM_RENDER:
    case GM_GOINACTIVE:
    case GM_LAYOUT:
    case GM_HELPTEST:
        return 0;
    case GM_GOACTIVE:
    case GM_HANDLEINPUT:
        return GMR_NOREUSE;
    default:
        return call_super(cl, object, msg);
    }
}

// The GA_ attributes of a tag list on the gadget: how many change its
// look, so that OM_SET can say it needs drawing again.
u32 Boopsi::set_gadget_attrs(u32 gadget, u32 tags, bool initial) {
    Bus b(*machine_);
    u32 flags = b.r16(gadget + gg_Flags);
    u32 activation = b.r16(gadget + gg_Activation);
    u32 type = b.r16(gadget + gg_GadgetType);
    u32 visual = 0;
    auto bit = [](u32& word, u32 mask, u32 on) { word = on != 0 ? word | mask : word & ~mask; };
    u32 budget = tags::kMaxTagItems;
    u32 at = tags;
    for (;;) {
        const u32 item = tags::next(b, at, budget);
        if (item == 0) {
            break;
        }
        const u32 tag = b.r32(item);
        const u32 data = b.r32(item + 4);
        switch (tag) {
        case ICA_TARGET:
        case ICA_MAP: {
            IcaEntry* entry = ica_slot(gadget);
            if (entry != nullptr) {
                if (tag == ICA_TARGET) {
                    entry->target = data;
                } else {
                    entry->map = data;
                }
                if (entry->target == 0 && entry->map == 0) {
                    drop_ica(gadget);
                }
            }
            break;
        }
        case GA_Left:
        case GA_RelRight:
            b.w16(gadget + gg_LeftEdge, data);
            bit(flags, GFLG_RELRIGHT, tag == GA_RelRight);
            ++visual;
            break;
        case GA_Top:
        case GA_RelBottom:
            b.w16(gadget + gg_TopEdge, data);
            bit(flags, GFLG_RELBOTTOM, tag == GA_RelBottom);
            ++visual;
            break;
        case GA_Width:
        case GA_RelWidth:
            b.w16(gadget + gg_Width, data);
            bit(flags, GFLG_RELWIDTH, tag == GA_RelWidth);
            ++visual;
            break;
        case GA_Height:
        case GA_RelHeight:
            b.w16(gadget + gg_Height, data);
            bit(flags, GFLG_RELHEIGHT, tag == GA_RelHeight);
            ++visual;
            break;
        case GA_Text:
        case GA_IntuiText:
        case GA_LabelImage:
            b.w32(gadget + gg_GadgetText, data);
            flags &= ~(GFLG_LABELSTRING | GFLG_LABELIMAGE);
            flags |= tag == GA_Text ? GFLG_LABELSTRING
                                    : (tag == GA_LabelImage ? GFLG_LABELIMAGE : GFLG_LABELITEXT);
            ++visual;
            break;
        case GA_Image:
        case GA_Border:
            b.w32(gadget + gg_GadgetRender, data);
            bit(flags, GFLG_GADGIMAGE, tag == GA_Image);
            ++visual;
            break;
        case GA_SelectRender:
            b.w32(gadget + gg_SelectRender, data);
            ++visual;
            break;
        case GA_Highlight:
            flags = (flags & ~GFLG_GADGHIGHBITS) | (data & GFLG_GADGHIGHBITS);
            break;
        case GA_Disabled:
            bit(flags, GFLG_DISABLED, data);
            ++visual;
            break;
        case GA_Selected:
            bit(flags, GFLG_SELECTED, data);
            ++visual;
            break;
        case GA_ID:
            b.w16(gadget + gg_GadgetID, data);
            break;
        case GA_UserData:
            b.w32(gadget + gg_UserData, data);
            break;
        case GA_SpecialInfo:
            b.w32(gadget + gg_SpecialInfo, data);
            break;
        case GA_EndGadget:
            bit(activation, GACT_ENDGADGET, data);
            break;
        case GA_Immediate:
            bit(activation, GACT_IMMEDIATE, data);
            break;
        case GA_RelVerify:
            bit(activation, GACT_RELVERIFY, data);
            break;
        case GA_FollowMouse:
            bit(activation, GACT_FOLLOWMOUSE, data);
            break;
        case GA_RightBorder:
            bit(activation, GACT_RIGHTBORDER, data);
            break;
        case GA_LeftBorder:
            bit(activation, GACT_LEFTBORDER, data);
            break;
        case GA_TopBorder:
            bit(activation, GACT_TOPBORDER, data);
            break;
        case GA_BottomBorder:
            bit(activation, GACT_BOTTOMBORDER, data);
            break;
        case GA_ToggleSelect:
            bit(activation, GACT_TOGGLESELECT, data);
            break;
        case GA_GZZGadget:
            bit(type, GTYP_GZZGADGET, data);
            break;
        case GA_RelSpecial:
            bit(flags, GFLG_RELSPECIAL, data);
            break;
        case GA_Previous:
            // The new gadget follows the one given.
            if (initial && data != 0) {
                b.w32(data + gg_NextGadget, gadget);
            }
            break;
        default:
            break;
        }
        if (!b.ok()) {
            return visual;
        }
    }
    b.w16(gadget + gg_Flags, flags);
    b.w16(gadget + gg_Activation, activation);
    b.w16(gadget + gg_GadgetType, type);
    return visual;
}

bool Boopsi::get_gadget_attr(u32 gadget, u32 attr, u32 storage) {
    Bus b(*machine_);
    u32 value = 0;
    switch (attr) {
    case ICA_TARGET:
    case ICA_MAP: {
        const IcaEntry* entry = find_ica(gadget);
        value = entry == nullptr ? 0 : (attr == ICA_TARGET ? entry->target : entry->map);
        break;
    }
    case GA_Left:
    case GA_RelRight:
        value = static_cast<u32>(static_cast<i32>(static_cast<i16>(b.r16(gadget + gg_LeftEdge))));
        break;
    case GA_Top:
    case GA_RelBottom:
        value = static_cast<u32>(static_cast<i32>(static_cast<i16>(b.r16(gadget + gg_TopEdge))));
        break;
    case GA_Width:
    case GA_RelWidth:
        value = static_cast<u32>(static_cast<i32>(static_cast<i16>(b.r16(gadget + gg_Width))));
        break;
    case GA_Height:
    case GA_RelHeight:
        value = static_cast<u32>(static_cast<i32>(static_cast<i16>(b.r16(gadget + gg_Height))));
        break;
    case GA_ID:
        value = b.r16(gadget + gg_GadgetID);
        break;
    case GA_UserData:
        value = b.r32(gadget + gg_UserData);
        break;
    case GA_SpecialInfo:
        value = b.r32(gadget + gg_SpecialInfo);
        break;
    case GA_Selected:
        value = (b.r16(gadget + gg_Flags) & GFLG_SELECTED) != 0 ? 1 : 0;
        break;
    case GA_Disabled:
        value = (b.r16(gadget + gg_Flags) & GFLG_DISABLED) != 0 ? 1 : 0;
        break;
    default:
        return false;
    }
    b.w32(storage, value);
    return b.ok();
}

Boopsi::IcaEntry* Boopsi::find_ica(u32 object) {
    if (object == 0) {
        return nullptr;
    }
    for (IcaEntry& entry : ica_) {
        if (entry.object == object) {
            return &entry;
        }
    }
    return nullptr;
}

Boopsi::IcaEntry* Boopsi::ica_slot(u32 object) {
    if (object == 0) {
        return nullptr;
    }
    if (IcaEntry* entry = find_ica(object); entry != nullptr) {
        return entry;
    }
    IcaEntry* entry = ica_.add();
    if (entry == nullptr) {
        LogLine().text("intuition: notification table full").send();
        return nullptr;
    }
    entry->object = object;
    return entry;
}

void Boopsi::drop_ica(u32 object) {
    ica_.remove_if([object](const IcaEntry& entry) { return entry.object == object; });
}

// The tag a map list renames the tag to: its first occurrence as a source,
// TAG_IGNORE to drop it, or the tag itself when the list does not name it.
u32 Boopsi::map_tag(Bus& b, u32 map, u32 tag) {
    u32 budget = tags::kMaxTagItems;
    for (;;) {
        const u32 item = tags::next(b, map, budget);
        if (item == 0) {
            return tag;
        }
        if (b.r32(item) == tag) {
            return b.r32(item + 4);
        }
    }
}

// Passes the gadget's changed attributes on to its ICA_TARGET: renamed
// through ICA_MAP, with GA_ID naming the gadget when the list has none.
// The target either gets an IDCMP_IDCMPUPDATE for its window or, when it
// is another object, an OM_UPDATE carrying the same GadgetInfo and flags.
u32 Boopsi::notify(u32 gadget, u32 msg) {
    const IcaEntry* entry = find_ica(gadget);
    if (entry == nullptr || entry->target == 0) {
        return 0;
    }
    const u32 target = entry->target;
    const u32 map = entry->map;
    Bus b(*machine_);
    const u32 id = b.r16(gadget + gg_GadgetID);
    if (b.ok()) {
        forward(target, map, msg, true, id);
    }
    return 0;
}

void Boopsi::forward(u32 target, u32 map, u32 msg, bool with_id, u32 id) {
    if (target == 0) {
        return;
    }
    const u32 task = exec_->this_task();
    for (const TaskDepth& depth : notify_depths_) {
        if (depth.task == task && depth.depth >= kMaxNotifyDepth) {
            return;
        }
    }
    Bus b(*machine_);
    const u32 attr_list = b.r32(msg + opu_AttrList);
    const u32 ginfo = b.r32(msg + opu_GInfo);
    const u32 flags = b.r32(msg + opu_Flags);
    if (!b.ok()) {
        return;
    }
    u32 pairs[kMaxNotifyTags * 2];
    u32 count = 0;
    bool has_id = false;
    u32 budget = tags::kMaxTagItems;
    u32 at = attr_list;
    for (;;) {
        const u32 item = tags::next(b, at, budget);
        if (item == 0) {
            break;
        }
        const u32 mapped = map_tag(b, map, b.r32(item));
        const u32 data = b.r32(item + 4);
        if (!b.ok()) {
            return;
        }
        if (mapped == tags::kTagIgnore) {
            continue;
        }
        has_id = has_id || mapped == GA_ID;
        if (count < kMaxNotifyTags) {
            pairs[count * 2] = mapped;
            pairs[count * 2 + 1] = data;
            ++count;
        }
    }
    if (with_id && !has_id && count < kMaxNotifyTags) {
        pairs[count * 2] = GA_ID;
        pairs[count * 2 + 1] = id;
        ++count;
    }
    if (target == ICTARGET_IDCMP) {
        const u32 window = ginfo != 0 ? b.r32(ginfo + gi_Window) : 0;
        if (!b.ok() || window == 0 || idcmp_update_hook_ == nullptr) {
            return;
        }
        u16 code = 0;
        for (u32 i = 0; i < count; ++i) {
            if (pairs[i * 2] == ICSPECIAL_CODE) {
                code = static_cast<u16>(pairs[i * 2 + 1]);
            }
        }
        idcmp_update_hook_(idcmp_update_hook_ctx_, window, pairs, count, flags, code);
        return;
    }
    send_update(target, pairs, count, ginfo, flags);
}

namespace {

// A guest copy of the attribute pairs as a tag list, made for one call and
// freed after it.
class GuestTags {
public:
    GuestTags(Exec& exec, Machine& m, const u32* pairs, u32 count)
        : exec_(exec), bytes_((count + 1) * tags::kItemBytes) {
        list_ = exec.alloc(bytes_, MEMF_PUBLIC | MEMF_CLEAR);
        if (list_ == 0) {
            return;
        }
        Bus b(m);
        for (u32 i = 0; i < count; ++i) {
            b.w32(list_ + i * tags::kItemBytes, pairs[i * 2]);
            b.w32(list_ + i * tags::kItemBytes + 4, pairs[i * 2 + 1]);
        }
        b.w32(list_ + count * tags::kItemBytes, tags::kTagDone);
        if (!b.ok()) {
            exec.free(list_, bytes_);
            list_ = 0;
        }
    }
    ~GuestTags() {
        if (list_ != 0) {
            exec_.free(list_, bytes_);
        }
    }
    GuestTags(const GuestTags&) = delete;
    GuestTags& operator=(const GuestTags&) = delete;

    u32 list() const { return list_; }

private:
    Exec& exec_;
    u32 bytes_;
    u32 list_ = 0;
};

}

// The notification for an object target: OM_UPDATE with the mapped
// attributes.
void Boopsi::send_update(u32 target, const u32* pairs, u32 count, u32 ginfo, u32 flags) {
    const GuestTags list(*exec_, *machine_, pairs, count);
    if (list.list() == 0) {
        return;
    }
    const u32 msg[] = {OM_UPDATE, list.list(), ginfo, flags};
    const u32 task = exec_->this_task();
    const auto entry = [&]() -> TaskDepth* {
        for (TaskDepth& depth : notify_depths_) {
            if (depth.task == task) {
                return &depth;
            }
        }
        return nullptr;
    };
    if (TaskDepth* depth = entry(); depth != nullptr) {
        ++depth->depth;
    } else {
        notify_depths_.push_back({task, 1});
    }
    send(class_of(target), target, msg, 4);
    // The entry may have moved, or gone with a task that ended meanwhile.
    if (TaskDepth* depth = entry(); depth != nullptr && --depth->depth == 0) {
        std::erase_if(notify_depths_, [](const TaskDepth& d) { return d.depth == 0; });
    }
}

void Boopsi::raise_loop(u32 data) {
    Bus b(*machine_);
    b.w32(data + ic_Loop, b.r32(data + ic_Loop) + 1);
    loop_holds_.push_back({exec_->this_task(), data});
}

void Boopsi::lower_loop(u32 data) {
    const u32 task = exec_->this_task();
    for (auto it = loop_holds_.rbegin(); it != loop_holds_.rend(); ++it) {
        if (it->task == task && it->data == data) {
            loop_holds_.erase(std::next(it).base());
            Bus b(*machine_);
            const u32 loop = b.r32(data + ic_Loop);
            b.w32(data + ic_Loop, loop != 0 ? loop - 1 : 0);
            return;
        }
    }
}

void Boopsi::drop_loop_holds(u32 data) {
    std::erase_if(loop_holds_, [data](const LoopHold& hold) { return hold.data == data; });
}

// A task that ends inside a notification leaves no depth behind, and the
// loop counters it raised come down.
void Boopsi::task_ended(u32 task) {
    std::erase_if(notify_depths_, [task](const TaskDepth& d) { return d.task == task; });
    Bus b(*machine_);
    for (const LoopHold& hold : loop_holds_) {
        if (hold.task == task) {
            const u32 loop = b.r32(hold.data + ic_Loop);
            b.w32(hold.data + ic_Loop, loop != 0 ? loop - 1 : 0);
        }
    }
    std::erase_if(loop_holds_, [task](const LoopHold& hold) { return hold.task == task; });
}

void Boopsi::notify_attrs(u32 object, const u32* pairs, u32 count, u32 ginfo, u32 flags) {
    const GuestTags list(*exec_, *machine_, pairs, count);
    if (list.list() == 0) {
        return;
    }
    const u32 msg[] = {OM_NOTIFY, list.list(), ginfo, flags};
    send(class_of(object), object, msg, 4);
}

// icclass: passes the attributes of each OM_UPDATE and OM_NOTIFY on to
// its ICA_TARGET, renamed through its ICA_MAP. While it does, its loop
// counter is up, and a notification that comes back round to it through
// the targets goes no further.
u32 Boopsi::ic_dispatch(u32 cl, u32 object, u32 msg) {
    Bus b(*machine_);
    const u32 method = b.r32(msg);
    if (!b.ok()) {
        return 0;
    }
    const u32 data = object + b.r16(ic_ + cl_InstOffset);
    switch (method) {
    case OM_NEW: {
        const u32 made = call_super(cl, object, msg);
        if (made != 0) {
            set_ic_attrs(made + b.r16(ic_ + cl_InstOffset), b.r32(msg + 4));
        }
        return made;
    }
    case OM_SET:
        set_ic_attrs(data, b.r32(msg + 4));
        return call_super(cl, object, msg);
    case OM_GET: {
        const u32 attr = b.r32(msg + 4);
        if (attr != ICA_TARGET && attr != ICA_MAP) {
            return call_super(cl, object, msg);
        }
        b.w32(b.r32(msg + 8), b.r32(data + (attr == ICA_TARGET ? ic_Target : ic_Map)));
        return b.ok() ? 1 : 0;
    }
    case OM_UPDATE:
    case OM_NOTIFY: {
        if (b.r32(data + ic_Loop) != 0 || !b.ok()) {
            return 0;
        }
        raise_loop(data);
        forward(b.r32(data + ic_Target), b.r32(data + ic_Map), msg, false, 0);
        lower_loop(data);
        return 0;
    }
    case OM_DISPOSE:
        drop_loop_holds(data);
        return call_super(cl, object, msg);
    case ICM_SETLOOP:
        b.w32(data + ic_Loop, b.r32(data + ic_Loop) + 1);
        return 0;
    case ICM_CLEARLOOP: {
        const u32 loop = b.r32(data + ic_Loop);
        b.w32(data + ic_Loop, loop != 0 ? loop - 1 : 0);
        return 0;
    }
    case ICM_CHECKLOOP:
        return b.r32(data + ic_Loop);
    default:
        return call_super(cl, object, msg);
    }
}

void Boopsi::set_ic_attrs(u32 data, u32 tags) {
    Bus b(*machine_);
    u32 budget = tags::kMaxTagItems;
    u32 at = tags;
    for (;;) {
        const u32 item = tags::next(b, at, budget);
        if (item == 0) {
            return;
        }
        const u32 tag = b.r32(item);
        if (tag == ICA_TARGET) {
            b.w32(data + ic_Target, b.r32(item + 4));
        } else if (tag == ICA_MAP) {
            b.w32(data + ic_Map, b.r32(item + 4));
        }
    }
}

// modelclass: an icclass whose OM_UPDATE and OM_NOTIFY go to each of its
// members as OM_UPDATE before they go on to its own target. Members join
// with OM_ADDMEMBER, leave with OM_REMMEMBER and are disposed with it.
u32 Boopsi::model_dispatch(u32 cl, u32 object, u32 msg) {
    Bus b(*machine_);
    const u32 method = b.r32(msg);
    if (!b.ok()) {
        return 0;
    }
    const u32 list = object + b.r16(model_ + cl_InstOffset) + model_Members;
    switch (method) {
    case OM_NEW: {
        const u32 made = call_super(cl, object, msg);
        if (made != 0) {
            const u32 members = made + b.r16(model_ + cl_InstOffset) + model_Members;
            b.w32(members, members + 4);
            b.w32(members + 4, 0);
            b.w32(members + 8, members);
        }
        return made;
    }
    case OM_ADDMEMBER: {
        const u32 member = b.r32(msg + 4);
        if (member != 0) {
            const u32 add[] = {OM_ADDTAIL, list};
            send(class_of(member), member, add, 2);
        }
        return 0;
    }
    case OM_REMMEMBER: {
        const u32 member = b.r32(msg + 4);
        if (member != 0) {
            const u32 remove[] = {OM_REMOVE};
            send(class_of(member), member, remove, 1);
        }
        return 0;
    }
    case OM_DISPOSE: {
        for (u32 step = 0; step < kMaxModelMembers; ++step) {
            const u32 node = b.r32(list);
            if (!b.ok() || node == 0 || b.r32(node) == 0) {
                break;
            }
            const u32 member = node + Object_SIZEOF;
            const u32 remove[] = {OM_REMOVE};
            send(class_of(member), member, remove, 1);
            dispose_object(member);
        }
        return call_super(cl, object, msg);
    }
    case OM_UPDATE:
    case OM_NOTIFY: {
        const u32 data = object + b.r16(ic_ + cl_InstOffset);
        if (b.r32(data + ic_Loop) != 0 || !b.ok()) {
            return 0;
        }
        // The members as the broadcast starts; one taken out by another's
        // update before its turn hears nothing, and one added meanwhile
        // waits for the next.
        std::vector<u32> members;
        for (u32 node = b.r32(list); b.ok() && node != 0 && members.size() < kMaxModelMembers;) {
            const u32 next = b.r32(node);
            if (next == 0) {
                break;
            }
            members.push_back(node + Object_SIZEOF);
            node = next;
        }
        raise_loop(data);
        const u32 update[] = {OM_UPDATE, b.r32(msg + opu_AttrList), b.r32(msg + opu_GInfo),
                              b.r32(msg + opu_Flags)};
        for (const u32 member : members) {
            if (is_member(list, member)) {
                send(class_of(member), member, update, 4);
            }
        }
        lower_loop(data);
        return call_super(cl, object, msg);
    }
    default:
        return call_super(cl, object, msg);
    }
}

bool Boopsi::is_member(u32 list, u32 member) {
    Bus b(*machine_);
    u32 node = b.r32(list);
    for (u32 step = 0; step < kMaxModelMembers && b.ok() && node != 0; ++step) {
        const u32 next = b.r32(node);
        if (next == 0) {
            return false;
        }
        if (node + Object_SIZEOF == member) {
            return true;
        }
        node = next;
    }
    return false;
}

// pointerclass: holds the POINTERA_ attributes of a pointer image, which
// a window takes with WA_Pointer.
u32 Boopsi::pointer_dispatch(u32 cl, u32 object, u32 msg) {
    Bus b(*machine_);
    const u32 method = b.r32(msg);
    if (!b.ok()) {
        return 0;
    }
    switch (method) {
    case OM_NEW: {
        const u32 made = call_super(cl, object, msg);
        if (made == 0) {
            return 0;
        }
        const u32 data = made + b.r16(pointer_ + cl_InstOffset);
        u32 budget = tags::kMaxTagItems;
        u32 at = b.r32(msg + 4);
        for (;;) {
            const u32 item = tags::next(b, at, budget);
            if (item == 0) {
                break;
            }
            const u32 attr = b.r32(item) - POINTERA_BitMap;
            if (attr < kPointerAttrs) {
                b.w32(data + attr * 4, b.r32(item + 4));
            }
        }
        return made;
    }
    case OM_GET: {
        const u32 attr = b.r32(msg + 4) - POINTERA_BitMap;
        if (attr >= kPointerAttrs) {
            return call_super(cl, object, msg);
        }
        b.w32(b.r32(msg + 8), b.r32(object + b.r16(pointer_ + cl_InstOffset) + attr * 4));
        return b.ok() ? 1 : 0;
    }
    default:
        return call_super(cl, object, msg);
    }
}

// imageclass: an Image with CUSTOMIMAGEDEPTH, which Intuition draws by
// sending it IM_DRAW. It draws nothing itself.
u32 Boopsi::image_dispatch(u32 cl, u32 object, u32 msg) {
    Bus b(*machine_);
    const u32 method = b.r32(msg);
    if (!b.ok()) {
        return 0;
    }
    switch (method) {
    case OM_NEW: {
        const u32 image = call_super(cl, object, msg);
        if (image == 0) {
            return 0;
        }
        b.w16(image + ig_Depth, CUSTOMIMAGEDEPTH);
        set_image_attrs(image, b.r32(msg + 4));
        return image;
    }
    case OM_SET:
    case OM_UPDATE:
        return call_super(cl, object, msg) + set_image_attrs(object, b.r32(msg + 4));
    case OM_GET:
        return get_image_attr(object, b.r32(msg + 4), b.r32(msg + 8))
                   ? 1
                   : call_super(cl, object, msg);
    case IM_HITTEST:
    case IM_HITFRAME: {
        const i32 x = static_cast<i16>(b.r16(msg + 4));
        const i32 y = static_cast<i16>(b.r16(msg + 6));
        const i32 w = static_cast<i16>(b.r16(object + ig_Width));
        const i32 h = static_cast<i16>(b.r16(object + ig_Height));
        return x >= 0 && y >= 0 && x < w && y < h ? 1 : 0;
    }
    case IM_FRAMEBOX: {
        // The frame is the contents: imageclass has no frame.
        const u32 contents = b.r32(msg + 4);
        const u32 frame = b.r32(msg + 8);
        for (u32 i = 0; i < 8; i += 2) {
            b.w16(frame + i, b.r16(contents + i));
        }
        return 1;
    }
    case IM_DRAW:
    case IM_DRAWFRAME:
    case IM_ERASE:
    case IM_ERASEFRAME:
    case IM_MOVE:
        return 0;
    default:
        return call_super(cl, object, msg);
    }
}

// sysiclass: a system image of imageclass. SYSIA_Which and SYSIA_Size give
// its size, SYSIA_ReferenceFont its height; IM_DRAW hands it to the
// renderer, since BOOPSI has no drawing of its own.
u32 Boopsi::sysi_dispatch(u32 cl, u32 object, u32 msg) {
    Bus b(*machine_);
    const u32 method = b.r32(msg);
    if (!b.ok()) {
        return 0;
    }
    switch (method) {
    case OM_NEW: {
        const u32 image = call_super(cl, object, msg);
        if (image == 0) {
            return 0;
        }
        u32 which = DEPTHIMAGE;
        u32 size = SYSISIZE_MEDRES;
        u32 draw_info = 0;
        u32 reference_font = 0;
        u32 budget = tags::kMaxTagItems;
        u32 at = b.r32(msg + 4);
        for (;;) {
            const u32 item = tags::next(b, at, budget);
            if (item == 0) {
                break;
            }
            const u32 data = b.r32(item + 4);
            switch (b.r32(item)) {
            case SYSIA_Which:
                which = data;
                break;
            case SYSIA_Size:
                size = data;
                break;
            case SYSIA_DrawInfo:
                draw_info = data;
                break;
            case SYSIA_ReferenceFont:
                reference_font = data;
                break;
            default:
                break;
            }
        }
        u16 width = 0;
        u16 height = 0;
        if (!b.ok() ||
            !sys_image_dimensions(b, which, size, reference_font, width, height)) {
            const u32 dispose[] = {OM_DISPOSE};
            send(class_of(image), image, dispose, 1);
            return 0;
        }
        b.w16(image + si_Which, which);
        b.w16(image + si_Size, size);
        b.w32(image + si_DrawInfo, draw_info);
        // IA_Width and IA_Height do not size a system image.
        b.w16(image + ig_Width, width);
        b.w16(image + ig_Height, height);
        return b.ok() ? image : 0;
    }
    case IM_DRAW:
    case IM_DRAWFRAME: {
        const u32 rp = b.r32(msg + 4);
        const i32 left = static_cast<i16>(b.r16(msg + 8)) +
                         static_cast<i16>(b.r16(object + ig_LeftEdge));
        const i32 top = static_cast<i16>(b.r16(msg + 10)) +
                        static_cast<i16>(b.r16(object + ig_TopEdge));
        i32 width = static_cast<i16>(b.r16(object + ig_Width));
        i32 height = static_cast<i16>(b.r16(object + ig_Height));
        if (method == IM_DRAWFRAME) {
            const i32 frame_width = static_cast<i16>(b.r16(msg + 20));
            const i32 frame_height = static_cast<i16>(b.r16(msg + 22));
            if (frame_width > 0 && frame_height > 0) {
                width = frame_width;
                height = frame_height;
            }
        }
        const u32 which = b.r16(object + si_Which);
        const u32 state = b.r32(msg + 12);
        const u32 called_info = b.r32(msg + 16);
        const u32 draw_info = called_info != 0 ? called_info : b.r32(object + si_DrawInfo);
        if (b.ok() && sys_image_renderer_ != nullptr) {
            sys_image_renderer_(sys_image_renderer_ctx_, rp, which, left, top, width, height,
                                state, draw_info);
        }
        return 0;
    }
    default:
        return call_super(cl, object, msg);
    }
}

u32 Boopsi::set_image_attrs(u32 image, u32 tags) {
    Bus b(*machine_);
    u32 visual = 0;
    u32 budget = tags::kMaxTagItems;
    u32 at = tags;
    for (;;) {
        const u32 item = tags::next(b, at, budget);
        if (item == 0) {
            break;
        }
        const u32 data = b.r32(item + 4);
        switch (b.r32(item)) {
        case IA_Left:
            b.w16(image + ig_LeftEdge, data);
            break;
        case IA_Top:
            b.w16(image + ig_TopEdge, data);
            break;
        case IA_Width:
            b.w16(image + ig_Width, data);
            break;
        case IA_Height:
            b.w16(image + ig_Height, data);
            break;
        case IA_FGPen:
            b.w8(image + ig_PlanePick, data);
            break;
        case IA_BGPen:
            b.w8(image + ig_PlaneOnOff, data);
            break;
        case IA_Data:
            b.w32(image + ig_ImageData, data);
            break;
        default:
            continue;
        }
        ++visual;
    }
    return visual;
}

bool Boopsi::get_image_attr(u32 image, u32 attr, u32 storage) {
    Bus b(*machine_);
    u32 value = 0;
    switch (attr) {
    case IA_Left:
        value = static_cast<u32>(static_cast<i32>(static_cast<i16>(b.r16(image + ig_LeftEdge))));
        break;
    case IA_Top:
        value = static_cast<u32>(static_cast<i32>(static_cast<i16>(b.r16(image + ig_TopEdge))));
        break;
    case IA_Width:
        value = b.r16(image + ig_Width);
        break;
    case IA_Height:
        value = b.r16(image + ig_Height);
        break;
    case IA_FGPen:
        value = b.r8(image + ig_PlanePick);
        break;
    case IA_BGPen:
        value = b.r8(image + ig_PlaneOnOff);
        break;
    case IA_Data:
        value = b.r32(image + ig_ImageData);
        break;
    default:
        return false;
    }
    b.w32(storage, value);
    return b.ok();
}

}
