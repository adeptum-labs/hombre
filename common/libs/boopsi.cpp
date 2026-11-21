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

#include "gfx.hpp"
#include "tags.hpp"

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
// setting its height; false for an unknown Which.
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
    if (reference_font != 0) {
        const u16 y_size = b.r16(reference_font + tf_YSize);
        if (!b.ok()) {
            return false;
        }
        height = static_cast<u16>(which == MENUCHECK || which == AMIGAKEY ? y_size : y_size + 3);
    }
    return true;
}

constexpr u32 GTYP_GZZGADGET = 0x2000;

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

    // A dispatcher as a Hook calls it: the class in A0, the object in A2
    // and the message in A1.
    static void root(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).root_dispatch(m.cpu.a[0], m.cpu.a[2], m.cpu.a[1]));
    }
    static void gadget(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).gadget_dispatch(m.cpu.a[0], m.cpu.a[2], m.cpu.a[1]));
    }
    static void image(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).image_dispatch(m.cpu.a[0], m.cpu.a[2], m.cpu.a[1]));
    }
    static void sysi(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).sysi_dispatch(m.cpu.a[0], m.cpu.a[2], m.cpu.a[1]));
    }
};

bool Boopsi::init(Machine& m, Exec& exec) {
    machine_ = &m;
    exec_ = &exec;
    for (u32& slot : public_) {
        slot = 0;
    }
    root_ = make_system_class("rootclass", 0, 0, Natives::root);
    root_entry_ = root_ != 0 ? Bus(m).r32(root_ + h_Entry) : 0;
    gadget_ = root_ != 0 ? make_system_class("gadgetclass", root_, ExtGadget_SIZEOF,
                                             Natives::gadget)
                         : 0;
    gadget_entry_ = gadget_ != 0 ? Bus(m).r32(gadget_ + h_Entry) : 0;
    image_ = root_ != 0 ? make_system_class("imageclass", root_, Image_SIZEOF, Natives::image)
                        : 0;
    image_entry_ = image_ != 0 ? Bus(m).r32(image_ + h_Entry) : 0;
    sysi_ = image_ != 0
                ? make_system_class("sysiclass", image_, SysImage_SIZEOF, Natives::sysi)
                : 0;
    sysi_entry_ = sysi_ != 0 ? Bus(m).r32(sysi_ + h_Entry) : 0;
    return root_ != 0 && gadget_ != 0 && image_ != 0 && sysi_ != 0;
}

// A public class whose dispatcher is the native entry, behind an A-line
// stub that 68000 code can call through h_Entry.
u32 Boopsi::make_system_class(const char* name, u32 super, u32 inst_size, m68k::NativeFn entry) {
    const u16 id = machine_->jit->add_native(entry, this, name);
    if (id == 0) {
        LogLine().text("intuition: no native ids left for ").text(name).send();
        return 0;
    }
    const u32 stub = exec_->alloc(4, MEMF_PUBLIC);
    if (stub == 0) {
        return 0;
    }
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
    for (u32& slot : public_) {
        if (slot == 0) {
            slot = cl;
            b.w32(cl + cl_Flags, b.r32(cl + cl_Flags) | CLF_INLIST);
            return;
        }
    }
    LogLine().text("intuition: public class table full").send();
}

void Boopsi::remove_class(u32 cl) {
    if (cl == 0) {
        return;
    }
    for (u32& slot : public_) {
        if (slot == cl) {
            slot = 0;
        }
    }
    Bus b(*machine_);
    b.w32(cl + cl_Flags, b.r32(cl + cl_Flags) & ~CLF_INLIST);
}

// FreeClass: false while the class has objects or subclasses. The system
// classes stay.
bool Boopsi::free_class(u32 cl) {
    if (cl == 0 || cl == root_ || cl == gadget_ || cl == image_ || cl == sysi_) {
        return false;
    }
    Bus b(*machine_);
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
    const u32 cl = class_of(object);
    if (cl == 0) {
        return false;
    }
    Bus b(*machine_);
    const u32 entry = b.r32(cl + h_Entry);
    return b.ok() && entry != 0;
}

u32 Boopsi::object_bytes(u32 cl) const {
    Bus b(*machine_);
    return Object_SIZEOF + b.r16(cl + cl_InstOffset) + b.r16(cl + cl_InstSize);
}

u32 Boopsi::do_method(u32 object, u32 msg) {
    return object != 0 ? coerce_method(class_of(object), object, msg) : 0;
}

// The class's dispatcher on the message: in C++ for a system class, as
// a nested guest call for one written in 68000 code.
u32 Boopsi::coerce_method(u32 cl, u32 object, u32 msg) {
    if (cl == 0) {
        return 0;
    }
    Bus b(*machine_);
    const u32 entry = b.r32(cl + h_Entry);
    if (!b.ok() || entry == 0) {
        return 0;
    }
    if (entry == root_entry_) {
        return root_dispatch(cl, object, msg);
    }
    if (entry == gadget_entry_) {
        return gadget_dispatch(cl, object, msg);
    }
    if (entry == image_entry_) {
        return image_dispatch(cl, object, msg);
    }
    if (entry == sysi_entry_) {
        return sysi_dispatch(cl, object, msg);
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
    const m68k::ExitReason why = m.jit->call_guest(entry, kDispatchBlocks);
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
    case OM_SET:
    case OM_UPDATE:
        return call_super(cl, object, msg) + set_gadget_attrs(object, b.r32(msg + 4), false);
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
