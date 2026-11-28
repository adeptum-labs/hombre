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
#include "../slot_list.hpp"
#include "../types.hpp"
#include "exec.hpp"
#include "native.hpp"

#include <vector>

// BOOPSI, intuition's classes and objects, written in C++.
//
// A class is a struct IClass in guest memory, its dispatcher a Hook. An
// object is the instance data of its classes behind a struct _Object, and
// the object pointer points just past that header, as INST_DATA expects.
// The system classes (rootclass, gadgetclass, imageclass, sysiclass,
// icclass, modelclass, pointerclass and those intuition adds) dispatch in
// C++ behind A-line stubs, so a 68000 dispatcher's DoSuperMethod reaches
// them through cl_Super's h_Entry like any other. They are kept in a
// registry by that entry: a method C++ sends to one of them runs its C++
// dispatcher straight away, without a 68000 call. A method sent to a class
// whose dispatcher is 68000 code runs that code as a nested guest call.
//
// A kernel built with -DHOMBRE_BOOPSI_TRACE logs the methods; see
// boopsi_trace.hpp.

namespace hombre {

namespace ndk {

// struct IClass (intuition/classes.h); its Hook comes first.
constexpr u32 h_Entry = 8;
constexpr u32 h_SubEntry = 12;
constexpr u32 h_Data = 16;
constexpr u32 cl_Super = 24;
constexpr u32 cl_ID = 28;
constexpr u32 cl_InstOffset = 32;
constexpr u32 cl_InstSize = 34;
constexpr u32 cl_UserData = 36;
constexpr u32 cl_SubclassCount = 40;
constexpr u32 cl_ObjectCount = 44;
constexpr u32 cl_Flags = 48;
constexpr u32 IClass_SIZEOF = 52;
constexpr u32 CLF_INLIST = 1;

// struct _Object: the header before an object.
constexpr u32 o_Class = 8;
constexpr u32 Object_SIZEOF = 12;

// Methods (intuition/classusr.h, intuition/gadgetclass.h,
// intuition/imageclass.h).
constexpr u32 OM_NEW = 0x101;
constexpr u32 OM_DISPOSE = 0x102;
constexpr u32 OM_SET = 0x103;
constexpr u32 OM_GET = 0x104;
constexpr u32 OM_ADDTAIL = 0x105;
constexpr u32 OM_REMOVE = 0x106;
constexpr u32 OM_NOTIFY = 0x107;
constexpr u32 OM_UPDATE = 0x108;
constexpr u32 OM_ADDMEMBER = 0x109;
constexpr u32 OM_REMMEMBER = 0x10A;
constexpr u32 GM_HITTEST = 0;
constexpr u32 GM_RENDER = 1;
constexpr u32 GM_GOACTIVE = 2;
constexpr u32 GM_HANDLEINPUT = 3;
constexpr u32 GM_GOINACTIVE = 4;
constexpr u32 GM_HELPTEST = 5;
constexpr u32 GM_LAYOUT = 6;
constexpr u32 IM_DRAW = 0x202;
constexpr u32 IM_HITTEST = 0x203;
constexpr u32 IM_ERASE = 0x204;
constexpr u32 IM_MOVE = 0x205;
constexpr u32 IM_DRAWFRAME = 0x206;
constexpr u32 IM_FRAMEBOX = 0x207;
constexpr u32 IM_HITFRAME = 0x208;
constexpr u32 IM_ERASEFRAME = 0x209;

// Results of GM_HITTEST and of the active gadget's methods.
constexpr u32 GMR_GADGETHIT = 4;
constexpr u32 GMR_MEACTIVE = 0;
constexpr u32 GMR_NOREUSE = 2;
constexpr u32 GMR_REUSE = 4;
constexpr u32 GMR_VERIFY = 8;
constexpr u32 GMR_NEXTACTIVE = 16;
constexpr u32 GMR_PREVACTIVE = 32;

// GM_RENDER's gpr_Redraw.
constexpr u32 GREDRAW_UPDATE = 2;
constexpr u32 GREDRAW_REDRAW = 1;
constexpr u32 GREDRAW_TOGGLE = 0;

constexpr u32 GTYP_CUSTOMGADGET = 5;
constexpr u32 GFLG_GADGHBOX = 1;
constexpr u32 GFLG_EXTENDED = 0x8000;
constexpr u32 GFLG_LABELITEXT = 0;
constexpr u32 GFLG_LABELSTRING = 0x1000;
constexpr u32 GFLG_LABELIMAGE = 0x2000;
constexpr u32 GACT_ENDGADGET = 0x4;
constexpr u32 GACT_TOGGLESELECT = 0x100;
constexpr u32 CUSTOMIMAGEDEPTH = 0xFFFF;

// struct ExtGadget, the instance data of gadgetclass.
constexpr u32 gg_MoreFlags = 44;
constexpr u32 ExtGadget_SIZEOF = 56;

// struct GadgetInfo (intuition/cghooks.h).
constexpr u32 gi_Screen = 0;
constexpr u32 gi_Window = 4;
constexpr u32 gi_Requester = 8;
constexpr u32 gi_RastPort = 12;
constexpr u32 gi_Layer = 16;
constexpr u32 gi_Domain = 20;
constexpr u32 gi_Pens = 28;
constexpr u32 gi_DrInfo = 30;
constexpr u32 GadgetInfo_SIZEOF = 58;

// The ICA_ tags (intuition/icclass.h): an object's notification target and
// the map its attributes are renamed through.
constexpr u32 ICA_Dummy = 0x80040000;
constexpr u32 ICA_TARGET = ICA_Dummy + 1;
constexpr u32 ICA_MAP = ICA_Dummy + 2;
constexpr u32 ICTARGET_IDCMP = 0xFFFFFFFF;
// A map that renames an attribute to ICSPECIAL_CODE puts the low word of
// its value into the Code of the IDCMP_IDCMPUPDATE.
constexpr u32 ICSPECIAL_CODE = ICA_Dummy + 3;

// The loop counter of an icclass object (intuition/icclass.h): ICM_SETLOOP
// counts up, ICM_CLEARLOOP down and ICM_CHECKLOOP answers it.
constexpr u32 ICM_SETLOOP = 0x402;
constexpr u32 ICM_CLEARLOOP = 0x403;
constexpr u32 ICM_CHECKLOOP = 0x404;

// opUpdate's opu_Flags: the change is one of several while the user acts.
constexpr u32 OPUF_INTERIM = 1;

// pointerclass attributes (intuition/pointerclass.h).
constexpr u32 POINTERA_Dummy = 0x80039000;
constexpr u32 POINTERA_BitMap = POINTERA_Dummy + 1;
constexpr u32 POINTERA_XOffset = POINTERA_Dummy + 2;
constexpr u32 POINTERA_YOffset = POINTERA_Dummy + 3;
constexpr u32 POINTERA_WordWidth = POINTERA_Dummy + 4;
constexpr u32 POINTERA_XResolution = POINTERA_Dummy + 5;
constexpr u32 POINTERA_YResolution = POINTERA_Dummy + 6;

// propgclass attributes (intuition/gadgetclass.h).
constexpr u32 PGA_Dummy = 0x80031000;
constexpr u32 PGA_Freedom = PGA_Dummy + 1;
constexpr u32 PGA_Borderless = PGA_Dummy + 2;
constexpr u32 PGA_HorizPot = PGA_Dummy + 3;
constexpr u32 PGA_HorizBody = PGA_Dummy + 4;
constexpr u32 PGA_VertPot = PGA_Dummy + 5;
constexpr u32 PGA_VertBody = PGA_Dummy + 6;
constexpr u32 PGA_Total = PGA_Dummy + 7;
constexpr u32 PGA_Visible = PGA_Dummy + 8;
constexpr u32 PGA_Top = PGA_Dummy + 9;
constexpr u32 PGA_NewLook = PGA_Dummy + 10;

// GM_LAYOUT goes to gadgets with a relative box and to those that ask for
// it with GA_RelSpecial.
constexpr u32 GFLG_RELSPECIAL = 0x4000;

// struct opUpdate, the message OM_NOTIFY and OM_UPDATE carry.
constexpr u32 opu_AttrList = 4;
constexpr u32 opu_GInfo = 8;
constexpr u32 opu_Flags = 12;

// Gadget and image attributes.
constexpr u32 GA_Dummy = 0x80030000;
constexpr u32 GA_Left = GA_Dummy + 1;
constexpr u32 GA_RelRight = GA_Dummy + 2;
constexpr u32 GA_Top = GA_Dummy + 3;
constexpr u32 GA_RelBottom = GA_Dummy + 4;
constexpr u32 GA_Width = GA_Dummy + 5;
constexpr u32 GA_RelWidth = GA_Dummy + 6;
constexpr u32 GA_Height = GA_Dummy + 7;
constexpr u32 GA_RelHeight = GA_Dummy + 8;
constexpr u32 GA_Text = GA_Dummy + 9;
constexpr u32 GA_Image = GA_Dummy + 10;
constexpr u32 GA_Border = GA_Dummy + 11;
constexpr u32 GA_SelectRender = GA_Dummy + 12;
constexpr u32 GA_Highlight = GA_Dummy + 13;
constexpr u32 GA_Disabled = GA_Dummy + 14;
constexpr u32 GA_GZZGadget = GA_Dummy + 15;
constexpr u32 GA_ID = GA_Dummy + 16;
constexpr u32 GA_UserData = GA_Dummy + 17;
constexpr u32 GA_SpecialInfo = GA_Dummy + 18;
constexpr u32 GA_Selected = GA_Dummy + 19;
constexpr u32 GA_EndGadget = GA_Dummy + 20;
constexpr u32 GA_Immediate = GA_Dummy + 21;
constexpr u32 GA_RelVerify = GA_Dummy + 22;
constexpr u32 GA_FollowMouse = GA_Dummy + 23;
constexpr u32 GA_RightBorder = GA_Dummy + 24;
constexpr u32 GA_LeftBorder = GA_Dummy + 25;
constexpr u32 GA_TopBorder = GA_Dummy + 26;
constexpr u32 GA_BottomBorder = GA_Dummy + 27;
constexpr u32 GA_ToggleSelect = GA_Dummy + 28;
constexpr u32 GA_SysGadget = GA_Dummy + 29;
constexpr u32 GA_SysGType = GA_Dummy + 30;
constexpr u32 GA_Previous = GA_Dummy + 31;
constexpr u32 GA_Next = GA_Dummy + 32;
constexpr u32 GA_DrawInfo = GA_Dummy + 33;
constexpr u32 GA_IntuiText = GA_Dummy + 34;
constexpr u32 GA_LabelImage = GA_Dummy + 35;
constexpr u32 GA_TabCycle = GA_Dummy + 36;
constexpr u32 GA_GadgetHelp = GA_Dummy + 37;
constexpr u32 GA_Bounds = GA_Dummy + 38;
constexpr u32 GA_RelSpecial = GA_Dummy + 39;

constexpr u32 IA_Dummy = 0x80020000;
constexpr u32 IA_Left = IA_Dummy + 1;
constexpr u32 IA_Top = IA_Dummy + 2;
constexpr u32 IA_Width = IA_Dummy + 3;
constexpr u32 IA_Height = IA_Dummy + 4;
constexpr u32 IA_FGPen = IA_Dummy + 5;
constexpr u32 IA_BGPen = IA_Dummy + 6;
constexpr u32 IA_Data = IA_Dummy + 7;

// sysiclass attributes and values (intuition/imageclass.h).
constexpr u32 SYSIA_Size = IA_Dummy + 0x0B;
constexpr u32 SYSIA_Depth = IA_Dummy + 0x0C;
constexpr u32 SYSIA_Which = IA_Dummy + 0x0D;
constexpr u32 SYSIA_DrawInfo = IA_Dummy + 0x18;
constexpr u32 SYSIA_ReferenceFont = IA_Dummy + 0x19;

constexpr u32 DEPTHIMAGE = 0;
constexpr u32 ZOOMIMAGE = 1;
constexpr u32 SIZEIMAGE = 2;
constexpr u32 CLOSEIMAGE = 3;
constexpr u32 SDEPTHIMAGE = 5;
constexpr u32 LEFTIMAGE = 0x0A;
constexpr u32 UPIMAGE = 0x0B;
constexpr u32 RIGHTIMAGE = 0x0C;
constexpr u32 DOWNIMAGE = 0x0D;
constexpr u32 CHECKIMAGE = 0x0E;
constexpr u32 MXIMAGE = 0x0F;
constexpr u32 MENUCHECK = 0x10;
constexpr u32 AMIGAKEY = 0x11;

constexpr u32 SYSISIZE_MEDRES = 0;
constexpr u32 SYSISIZE_LOWRES = 1;
constexpr u32 SYSISIZE_HIRES = 2;

// The imp_State of IM_DRAW.
constexpr u32 IDS_NORMAL = 0;
constexpr u32 IDS_SELECTED = 1;
constexpr u32 IDS_DISABLED = 2;
constexpr u32 IDS_INACTIVENORMAL = 5;
constexpr u32 IDS_INACTIVESELECTED = 6;

}

// What the BOOPSI trace (boopsi_trace.hpp) keeps: how many traced calls
// each task has open, the entry of the dispatch C++ is about to run, which
// the trace logs already, and the classes MakeClass made, whose h_Entry it
// watches. Part of every build, so that Boopsi is laid out the same with
// and without HOMBRE_BOOPSI_TRACE; it stays empty without it.
struct BoopsiTraceState {
    static constexpr u32 kTasks = 32;
    struct Depth {
        bool used = false;
        u32 task = 0;
        u32 depth = 0;
    };
    Depth depths[kTasks]{};
    // Counts for a task that finds no free entry.
    Depth spare{};
    u32 expected_entry = 0;
    std::vector<u32> classes;
};

class Boopsi {
public:
    static constexpr u32 kMaxPublicClasses = 1024;
    // 68000 code one method call may run, at most.
    static constexpr u64 kDispatchBlocks = 1ull << 24;

    // How intuition draws a sysiclass image, which needs graphics.library.
    using SysImageRenderer = void (*)(void* ctx, u32 rp, u32 which, i32 left, i32 top,
                                      i32 width, i32 height, u32 state, u32 draw_info);

    // How intuition turns a gadget's notification for ICTARGET_IDCMP into
    // an IDCMP_IDCMPUPDATE: the window from the GadgetInfo, the mapped
    // attribute pairs, the notification's flags and the message's Code.
    using IdcmpUpdateHook = void (*)(void* ctx, u32 window, const u32* tags, u32 count,
                                     u32 flags, u16 code);

    // The dispatcher of a class written in C++, with the context it was
    // registered with: the class, the object and the message, as a Hook
    // gets them in A0, A2 and A1.
    using NativeDispatcher = u32 (*)(void* ctx, u32 cl, u32 object, u32 msg);
    static constexpr u32 kMaxNativeClasses = 32;

    // Makes the system classes and lists them. exec must be initialised.
    // False if exec runs out of memory or native ids.
    bool init(m68k::Machine& m, Exec& exec);

    // A public class over super whose dispatcher is C++, registered so that
    // the methods C++ sends it run without a 68000 call: 0 when exec runs
    // out of memory or native ids, or the registry is full.
    u32 make_native_class(const char* name, u32 super, u32 inst_size, NativeDispatcher dispatcher,
                          void* ctx);

    // The method sent to the superclass of cl, as DoSuperMethod sends it.
    u32 call_super(u32 cl, u32 object, u32 msg);

    // What the dispatcher of a native class returns for a method it leaves
    // to its superclass as it came: call_super's result, which it must
    // return at once. When 68000 code sent the class the method through
    // h_Entry and the superclass is 68000 code too, the superclass runs
    // after the dispatcher has returned, in the run of the code that sent
    // it, and the class adds no nested run of its own; pass_on then
    // returns 0, which stands for nothing.
    u32 pass_on(u32 cl, u32 object, u32 msg);

    // OM_NOTIFY to the object with the attribute pairs as its tag list, as
    // a class tells its targets of a change.
    void notify_attrs(u32 object, const u32* pairs, u32 count, u32 ginfo, u32 flags);

    // What exec's task end hook does: a task that ended inside a
    // notification counts no depth any more, and the icclass loop counters
    // it raised come down.
    void task_ended(u32 task);

    // The intuition vectors.
    u32 make_class(u32 id, u32 super_id, u32 super_class, u32 inst_size, u32 flags);
    void add_class(u32 cl);
    void remove_class(u32 cl);
    bool free_class(u32 cl);
    u32 new_object(u32 cl, u32 id, u32 tags);
    void dispose_object(u32 object);
    u32 set_attrs(u32 object, u32 tags, u32 ginfo);
    u32 get_attr(u32 attr, u32 object, u32 storage);
    u32 next_object(u32 state);

    // The public class with the guest name id, or the C string name.
    u32 find_class(u32 id) const;
    u32 find_class_named(const char* name) const;

    // The class an object was made from.
    u32 class_of(u32 object) const;
    bool is_object(u32 object) const;

    // What answers a custom gadget's messages: its class when it is an
    // object, else the Hook in its MutualExclude field, as cghooks.h has
    // it. 0 when it has neither. A hook takes the messages the way a class
    // does: Send with it in place of the class.
    u32 custom_dispatcher(u32 gadget) const;

    // Sends the method message msg (in guest memory) to the object, as its
    // own class or as cl: the dispatcher's result.
    u32 do_method(u32 object, u32 msg);
    u32 coerce_method(u32 cl, u32 object, u32 msg);

    // A guest message of up to kMaxMessageLongs longs, made and sent: the
    // method ID then the arguments.
    static constexpr u32 kMaxMessageLongs = 8;
    u32 send(u32 cl, u32 object, const u32* longs, u32 count);

    u32 root_class() const { return root_; }
    u32 gadget_class() const { return gadget_; }
    u32 image_class() const { return image_; }
    u32 sysi_class() const { return sysi_; }
    u32 ic_class() const { return ic_; }
    u32 model_class() const { return model_; }
    u32 pointer_class() const { return pointer_; }

    void set_sys_image_renderer(SysImageRenderer renderer, void* ctx) {
        sys_image_renderer_ = renderer;
        sys_image_renderer_ctx_ = ctx;
    }

    void set_idcmp_update_hook(IdcmpUpdateHook hook, void* ctx) {
        idcmp_update_hook_ = hook;
        idcmp_update_hook_ctx_ = ctx;
    }

private:
    struct Natives;

    // The notification target and map of a gadgetclass object, kept beside
    // the object so its instance data stays as its class sized it.
    struct IcaEntry {
        u32 object = 0;
        u32 target = 0;
        u32 map = 0;
    };

    // A class of the registry: the A-line stub its h_Entry points at and
    // the C++ that answers it.
    struct NativeClass {
        Boopsi* boopsi = nullptr;
        u32 entry = 0;
        NativeDispatcher dispatcher = nullptr;
        void* ctx = nullptr;
    };

    // The method 68000 code sent a native class through h_Entry, which the
    // class answers in the Jit's native frame: the class it has reached
    // and, once pass_on leaves it there, the 68000 superclass that goes on
    // with it.
    struct EntryCall {
        u32 cl = 0;
        u32 object = 0;
        u32 msg = 0;
        u32 forward = 0;
    };

    // The registry entry of the class whose h_Entry is entry, if any.
    const NativeClass* native_at(u32 entry) const;

    static constexpr u32 kMaxIcaEntries = 4096;
    static constexpr u32 kMaxNotifyTags = 32;
    static constexpr u32 kMaxNotifyDepth = 8;

    u32 dispatch(u32 cl, u32 object, u32 msg);
    u32 root_dispatch(u32 cl, u32 object, u32 msg);
    u32 gadget_dispatch(u32 cl, u32 object, u32 msg);
    u32 image_dispatch(u32 cl, u32 object, u32 msg);
    u32 sysi_dispatch(u32 cl, u32 object, u32 msg);
    u32 ic_dispatch(u32 cl, u32 object, u32 msg);
    u32 model_dispatch(u32 cl, u32 object, u32 msg);
    u32 pointer_dispatch(u32 cl, u32 object, u32 msg);
    void set_ic_attrs(u32 data, u32 tags);
    // Whether the object is still on the member list of a model.
    bool is_member(u32 list, u32 member);
    IcaEntry* find_ica(u32 object);
    IcaEntry* ica_slot(u32 object);
    void drop_ica(u32 object);
    u32 map_tag(native::Bus& b, u32 map, u32 tag);
    u32 notify(u32 gadget, u32 msg);
    // Passes the attributes of an OM_NOTIFY or OM_UPDATE on to target,
    // renamed through map; with_id adds GA_ID id when the list has none.
    void forward(u32 target, u32 map, u32 msg, bool with_id, u32 id);
    void send_update(u32 target, const u32* pairs, u32 count, u32 ginfo, u32 flags);
    u32 set_gadget_attrs(u32 gadget, u32 tags, bool initial);
    bool get_gadget_attr(u32 gadget, u32 attr, u32 storage);
    u32 set_image_attrs(u32 image, u32 tags);
    bool get_image_attr(u32 image, u32 attr, u32 storage);
    u32 object_bytes(u32 cl) const;

    m68k::Machine* machine_ = nullptr;
    Exec* exec_ = nullptr;
    u32 root_ = 0;
    u32 gadget_ = 0;
    u32 image_ = 0;
    u32 sysi_ = 0;
    u32 ic_ = 0;
    u32 model_ = 0;
    u32 pointer_ = 0;
    NativeClass natives_[kMaxNativeClasses]{};
    u32 native_count_ = 0;
    SysImageRenderer sys_image_renderer_ = nullptr;
    void* sys_image_renderer_ctx_ = nullptr;
    IdcmpUpdateHook idcmp_update_hook_ = nullptr;
    void* idcmp_update_hook_ctx_ = nullptr;
    SlotList<IcaEntry> ica_{kMaxIcaEntries};
    // How deep each task is in notifications, and the icclass loop
    // counters raised while a notification goes on: both given back when a
    // task ends inside one.
    struct TaskDepth {
        u32 task = 0;
        u32 depth = 0;
    };
    struct LoopHold {
        u32 task = 0;
        u32 data = 0;
    };
    std::vector<TaskDepth> notify_depths_;
    std::vector<LoopHold> loop_holds_;
    void raise_loop(u32 data);
    void lower_loop(u32 data);
    void drop_loop_holds(u32 data);
    std::vector<u32> public_;
    BoopsiTraceState trace_;
};

}
