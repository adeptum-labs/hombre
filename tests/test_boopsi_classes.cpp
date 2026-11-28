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

// The BOOPSI classes written in C++: the registry they dispatch through,
// icclass with its loop counter, modelclass and pointerclass; how Intuition
// lays out, feeds and guards the BOOPSI gadgets of its windows; and
// buttongclass and propgclass.

#include "gfx_rig.hpp"
#include "libs/utility.hpp"

#include <algorithm>
#include <vector>

using namespace hombre;
using namespace hombre::m68k;
using namespace hombre::ndk;
using gfx_test::Base;
using gfx_test::GfxRig;
using gfx_test::kData;

namespace {

constexpr u32 kNewScreen = kData;
constexpr u32 kTagList = kData + 0x100;
constexpr u32 kBuffer = kData + 0x300;
constexpr u32 kNewWindow = kData + 0x500;
constexpr u32 kCode = kData + 0x3000;
constexpr u32 kMessage = kData + 0x3100;

Machine* g_machine = nullptr;

u32 open_window(GfxRig& r, u32 idcmp) {
    r.put16(kNewScreen + ns_LeftEdge, 0);
    r.put16(kNewScreen + ns_TopEdge, 0);
    r.put16(kNewScreen + ns_Width, 640);
    r.put16(kNewScreen + ns_Height, 400);
    r.put16(kNewScreen + ns_Depth, 2);
    r.put8(kNewScreen + ns_DetailPen, 0);
    r.put8(kNewScreen + ns_BlockPen, 1);
    r.put16(kNewScreen + ns_ViewModes, 0);
    r.put16(kNewScreen + ns_Type, CUSTOMSCREEN);
    r.put32(kNewScreen + ns_Font, 0);
    r.put32(kNewScreen + ns_DefaultTitle, 0);
    r.put32(kNewScreen + ns_CustomBitMap, 0);
    r.cpu().a[0] = kNewScreen;
    CHECK(r.result(Base::Intuition, intuition_lvo::OpenScreen) != 0);
    r.put16(kNewWindow + nw_LeftEdge, 50);
    r.put16(kNewWindow + nw_TopEdge, 50);
    r.put16(kNewWindow + nw_Width, 200);
    r.put16(kNewWindow + nw_Height, 100);
    r.put8(kNewWindow + nw_DetailPen, 0);
    r.put8(kNewWindow + nw_BlockPen, 1);
    r.put32(kNewWindow + nw_IDCMPFlags, idcmp);
    r.put32(kNewWindow + nw_Flags, WFLG_ACTIVATE);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    r.put32(kNewWindow + nw_Title, 0);
    r.put32(kNewWindow + nw_Screen, 0);
    r.put16(kNewWindow + nw_MinWidth, 0);
    r.put16(kNewWindow + nw_MinHeight, 0);
    r.put16(kNewWindow + nw_MaxWidth, 0);
    r.put16(kNewWindow + nw_MaxHeight, 0);
    r.put16(kNewWindow + nw_Type, 0);
    r.cpu().a[0] = kNewWindow;
    const u32 window = r.result(Base::Intuition, intuition_lvo::OpenWindow);
    CHECK(window != 0);
    // The screen's DrawInfo is made on its first use and stays.
    r.cpu().a[0] = r.get32(window + wd_WScreen);
    CHECK(r.result(Base::Intuition, intuition_lvo::GetScreenDrawInfo) != 0);
    return window;
}

u32 get_msg(GfxRig& r, u32 port) {
    r.cpu().a[0] = port;
    return r.result(Base::Exec, exec_lvo::GetMsg);
}

void reply_msg(GfxRig& r, u32 msg) {
    r.cpu().a[1] = msg;
    CHECK_EQ(r.call(Base::Exec, exec_lvo::ReplyMsg), ExitReason::Halt);
}

// Takes every message off the port: how many were IDCMP_IDCMPUPDATE, and
// the first long pair of the last one's list.
struct Updates {
    u32 count = 0;
    u32 tag = 0;
    u32 data = 0;
};

Updates drain(GfxRig& r, u32 window) {
    const u32 port = r.get32(window + wd_UserPort);
    Updates seen;
    for (u32 msg = get_msg(r, port); msg != 0; msg = get_msg(r, port)) {
        if (r.get32(msg + im_Class) == IDCMP_IDCMPUPDATE) {
            ++seen.count;
            const u32 list = r.get32(msg + im_IAddress);
            seen.tag = r.get32(list);
            seen.data = r.get32(list + 4);
        }
        reply_msg(r, msg);
    }
    return seen;
}

u32 send_notify(GfxRig& r, u32 object, u32 attrs, u32 info) {
    const u32 notify[] = {OM_NOTIFY, attrs, info, 0};
    Boopsi& bo = r.intuition.boopsi();
    return bo.send(bo.class_of(object), object, notify, 4);
}

// What the native test class saw of the last method it got.
struct Seen {
    u32 calls = 0;
    u32 cl = 0;
    u32 object = 0;
    u32 method = 0;
    Boopsi* boopsi = nullptr;
    // gpl_Initial of each GM_LAYOUT, and the ie_Class of each
    // GM_HANDLEINPUT.
    std::vector<u32> layouts;
    std::vector<u32> inputs;
    // Every method, in order.
    std::vector<u32> methods;
};

u32 seen_dispatch(void* ctx, u32 cl, u32 object, u32 msg) {
    Seen& seen = *static_cast<Seen*>(ctx);
    ++seen.calls;
    seen.cl = cl;
    seen.object = object;
    native::Bus b(*g_machine);
    seen.method = b.r32(msg);
    seen.methods.push_back(seen.method);
    switch (seen.method) {
    case 0x7777:
        return 0x1234;
    case GM_LAYOUT:
        seen.layouts.push_back(b.r32(msg + 8));
        return 0;
    case GM_GOACTIVE:
        return GMR_MEACTIVE;
    case GM_HANDLEINPUT: {
        const u32 event = b.r32(msg + 8);
        seen.inputs.push_back(b.r8(event + 4));
        return b.r16(event + 6) == 0xE8 ? GMR_NOREUSE : GMR_MEACTIVE;
    }
    default:
        return seen.boopsi->call_super(cl, object, msg);
    }
}

u64 g_now_us = 0;
u64 fake_clock() { return g_now_us; }

void add_gadget(GfxRig& r, u32 window, u32 gadget) {
    r.cpu().a[0] = window;
    r.cpu().a[1] = gadget;
    r.cpu().a[2] = 0;
    r.cpu().d[0] = 0xFFFF;
    r.cpu().d[1] = 1;
    CHECK(r.result(Base::Intuition, intuition_lvo::AddGList) != 0xFFFFFFFF);
    r.cpu().a[0] = gadget;
    r.cpu().a[1] = window;
    r.cpu().a[2] = 0;
    r.cpu().d[0] = 1;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::RefreshGList), ExitReason::Halt);
}

void a_native_class_dispatches_without_68000_code() {
    GfxRig r;
    g_machine = &r.machine;
    Boopsi& bo = r.intuition.boopsi();
    Seen seen;
    seen.boopsi = &bo;
    const u32 cl = bo.make_native_class("seengclass", bo.gadget_class(), 4, seen_dispatch, &seen);
    CHECK(cl != 0);
    CHECK_EQ(bo.find_class_named("seengclass"), cl);
    CHECK_EQ(r.get32(cl + cl_Super), bo.gadget_class());
    CHECK_EQ(r.get16(cl + cl_InstOffset), ExtGadget_SIZEOF);

    // From C++, with no nested 68000 run left, the method still reaches
    // the class and its superclass: neither needs one.
    u32 taken = 0;
    while (r.exec.enter_nested_run()) {
        ++taken;
    }
    const u32 g = bo.new_object(cl, 0, r.tags(kTagList, {{GA_Width, 9}, {TAG_DONE, 0}}));
    for (u32 i = 0; i < taken; ++i) {
        r.exec.leave_nested_run();
    }
    CHECK(g != 0);
    CHECK_EQ(seen.calls, 1u);
    CHECK_EQ(seen.method, OM_NEW);
    CHECK_EQ(seen.cl, cl);
    CHECK_EQ(r.get16(g + gg_Width), 9u);
    CHECK_EQ(bo.class_of(g), cl);

    // 68000 code reaches it through h_Entry, as DoMethod does.
    r.put32(kMessage, 0x7777);
    const u16 code[] = {
        0x207C, static_cast<u16>(cl >> 16), static_cast<u16>(cl),           // movea.l #cl,a0
        0x227C, static_cast<u16>(kMessage >> 16), static_cast<u16>(kMessage),  // movea.l #msg,a1
        0x247C, static_cast<u16>(g >> 16), static_cast<u16>(g),             // movea.l #g,a2
        0x2668, 0x0008,                                                   // movea.l 8(a0),a3
        0x4E93,                                                           // jsr (a3)
        0x4EF8, 0x0400,                                                   // jmp $400.w
    };
    for (u32 i = 0; i < sizeof code / sizeof code[0]; ++i) {
        r.put16(kCode + i * 2, code[i]);
    }
    r.jit.flush();
    r.cpu().pc = kCode;
    r.cpu().a[7] = gfx_test::kStack;
    CHECK_EQ(r.jit.run(1000000), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0x1234u);
    CHECK_EQ(seen.calls, 2u);
    CHECK_EQ(seen.object, g);

    // The class stays while intuition runs.
    bo.dispose_object(g);
    CHECK(!bo.free_class(cl));
    CHECK(!bo.free_class(bo.ic_class()));
}

void icclass_passes_updates_on_to_its_target() {
    GfxRig r;
    Boopsi& bo = r.intuition.boopsi();
    const u32 window = open_window(r, IDCMP_IDCMPUPDATE);
    const u32 info = r.intuition.make_gadget_info(window);
    const u32 before = r.exec.free_bytes();

    // An object target takes the renamed attribute as OM_UPDATE.
    const u32 target = bo.new_object(bo.gadget_class(), 0, r.tags(kTagList, {{TAG_DONE, 0}}));
    const u32 source = GA_Dummy + 0x300;
    const u32 map = r.tags(kTagList + 0x40, {{source, GA_Width}, {TAG_DONE, 0}});
    r.str(kBuffer, "icclass");
    r.cpu().a[0] = 0;
    r.cpu().a[1] = kBuffer;
    r.cpu().a[2] = r.tags(kTagList + 0x80, {{ICA_TARGET, target}, {ICA_MAP, map}, {TAG_DONE, 0}});
    const u32 ic = r.result(Base::Intuition, intuition_lvo::NewObjectA);
    CHECK(ic != 0);
    CHECK_EQ(bo.class_of(ic), bo.ic_class());
    CHECK(bo.get_attr(ICA_TARGET, ic, kBuffer + 0x20) != 0);
    CHECK_EQ(r.get32(kBuffer + 0x20), target);
    CHECK(bo.get_attr(ICA_MAP, ic, kBuffer + 0x20) != 0);
    CHECK_EQ(r.get32(kBuffer + 0x20), map);
    send_notify(r, ic, r.tags(kTagList + 0xC0, {{source, 33}, {TAG_DONE, 0}}), info);
    CHECK_EQ(r.get16(target + gg_Width), 33u);

    // ICTARGET_IDCMP: the window hears it, without a GA_ID the object
    // does not have.
    bo.set_attrs(ic, r.tags(kTagList + 0x80, {{ICA_TARGET, ICTARGET_IDCMP}, {TAG_DONE, 0}}), 0);
    send_notify(r, ic, r.tags(kTagList + 0xC0, {{source, 44}, {TAG_DONE, 0}}), info);
    const u32 port = r.get32(window + wd_UserPort);
    const u32 msg = get_msg(r, port);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_IDCMPUPDATE);
    const u32 list = r.get32(msg + im_IAddress);
    CHECK_EQ(r.get32(list), GA_Width);
    CHECK_EQ(r.get32(list + 4), 44u);
    CHECK_EQ(r.get32(list + 8), TAG_DONE);
    reply_msg(r, msg);

    bo.dispose_object(ic);
    bo.dispose_object(target);
    CHECK_EQ(r.exec.free_bytes(), before);
    r.intuition.free_gadget_info(info);
}

void the_loop_counter_stops_a_notification_that_comes_round() {
    GfxRig r;
    Boopsi& bo = r.intuition.boopsi();
    const u32 window = open_window(r, IDCMP_IDCMPUPDATE);
    const u32 info = r.intuition.make_gadget_info(window);

    const u32 ic = bo.new_object(bo.ic_class(), 0, r.tags(kTagList, {{TAG_DONE, 0}}));
    CHECK(ic != 0);
    const u32 setloop[] = {ICM_SETLOOP};
    const u32 clearloop[] = {ICM_CLEARLOOP};
    const u32 checkloop[] = {ICM_CHECKLOOP};
    CHECK_EQ(bo.send(bo.class_of(ic), ic, checkloop, 1), 0u);
    bo.send(bo.class_of(ic), ic, setloop, 1);
    bo.send(bo.class_of(ic), ic, setloop, 1);
    CHECK_EQ(bo.send(bo.class_of(ic), ic, checkloop, 1), 2u);

    // While the counter is up, nothing goes on to the target.
    bo.set_attrs(ic, r.tags(kTagList, {{ICA_TARGET, ICTARGET_IDCMP}, {TAG_DONE, 0}}), 0);
    const u32 attrs = r.tags(kTagList + 0x40, {{GA_Top, 5}, {TAG_DONE, 0}});
    send_notify(r, ic, attrs, info);
    CHECK_EQ(drain(r, window).count, 0u);
    bo.send(bo.class_of(ic), ic, clearloop, 1);
    bo.send(bo.class_of(ic), ic, clearloop, 1);
    bo.send(bo.class_of(ic), ic, clearloop, 1);
    CHECK_EQ(bo.send(bo.class_of(ic), ic, checkloop, 1), 0u);
    send_notify(r, ic, attrs, info);
    CHECK_EQ(drain(r, window).count, 1u);

    // A ring: ic -> model -> ic. The model's member, which tells the
    // window, hears the change once; the ring ends at ic, not at the
    // nesting cap of notifications.
    const u32 member = bo.new_object(
        bo.gadget_class(), 0, r.tags(kTagList + 0x80, {{ICA_TARGET, ICTARGET_IDCMP}, {TAG_DONE, 0}}));
    const u32 model = bo.new_object(bo.model_class(), 0,
                                    r.tags(kTagList + 0xC0, {{ICA_TARGET, ic}, {TAG_DONE, 0}}));
    CHECK(member != 0 && model != 0);
    const u32 add[] = {OM_ADDMEMBER, member};
    bo.send(bo.class_of(model), model, add, 2);
    bo.set_attrs(ic, r.tags(kTagList, {{ICA_TARGET, model}, {TAG_DONE, 0}}), 0);
    send_notify(r, ic, r.tags(kTagList + 0x40, {{GA_Width, 12}, {TAG_DONE, 0}}), info);
    CHECK_EQ(r.get16(member + gg_Width), 12u);
    const Updates seen = drain(r, window);
    CHECK_EQ(seen.count, 1u);
    CHECK_EQ(seen.tag, GA_Width);
    CHECK_EQ(seen.data, 12u);
    CHECK_EQ(bo.send(bo.class_of(ic), ic, checkloop, 1), 0u);
    CHECK_EQ(bo.send(bo.class_of(model), model, checkloop, 1), 0u);

    bo.dispose_object(model);
    bo.dispose_object(ic);
    r.intuition.free_gadget_info(info);
}

void modelclass_broadcasts_to_its_members() {
    GfxRig r;
    Boopsi& bo = r.intuition.boopsi();
    const u32 window = open_window(r, IDCMP_IDCMPUPDATE);
    const u32 info = r.intuition.make_gadget_info(window);
    const u32 before = r.exec.free_bytes();

    const u32 model = bo.new_object(
        bo.model_class(), 0, r.tags(kTagList, {{ICA_TARGET, ICTARGET_IDCMP}, {TAG_DONE, 0}}));
    CHECK(model != 0);
    CHECK_EQ(r.get32(bo.model_class() + cl_Super), bo.ic_class());
    const u32 first = bo.new_object(bo.gadget_class(), 0, r.tags(kTagList + 0x40, {{TAG_DONE, 0}}));
    const u32 second = bo.new_object(bo.gadget_class(), 0, r.tags(kTagList + 0x40, {{TAG_DONE, 0}}));
    const u32 add_first[] = {OM_ADDMEMBER, first};
    const u32 add_second[] = {OM_ADDMEMBER, second};
    bo.send(bo.class_of(model), model, add_first, 2);
    bo.send(bo.class_of(model), model, add_second, 2);

    const u32 update[] = {OM_UPDATE, r.tags(kTagList + 0x80, {{GA_Height, 21}, {TAG_DONE, 0}}),
                          info, 0};
    bo.send(bo.class_of(model), model, update, 4);
    CHECK_EQ(r.get16(first + gg_Height), 21u);
    CHECK_EQ(r.get16(second + gg_Height), 21u);
    Updates seen = drain(r, window);
    CHECK_EQ(seen.count, 1u);
    CHECK_EQ(seen.tag, GA_Height);

    // A member taken out hears no more.
    const u32 remove_first[] = {OM_REMMEMBER, first};
    bo.send(bo.class_of(model), model, remove_first, 2);
    bo.send(bo.class_of(model), model, remove_first, 2);
    send_notify(r, model, r.tags(kTagList + 0x80, {{GA_Height, 30}, {TAG_DONE, 0}}), info);
    CHECK_EQ(r.get16(first + gg_Height), 21u);
    CHECK_EQ(r.get16(second + gg_Height), 30u);
    CHECK_EQ(drain(r, window).count, 1u);

    // Disposing the model disposes the members it still has.
    bo.dispose_object(model);
    CHECK_EQ(r.get32(bo.gadget_class() + cl_ObjectCount), 1u);
    bo.dispose_object(first);
    CHECK_EQ(r.get32(bo.gadget_class() + cl_ObjectCount), 0u);
    CHECK_EQ(r.get32(bo.model_class() + cl_ObjectCount), 0u);
    CHECK_EQ(r.exec.free_bytes(), before);
    r.intuition.free_gadget_info(info);
}

void pointerclass_keeps_its_attributes() {
    GfxRig r;
    Boopsi& bo = r.intuition.boopsi();
    const u32 before = r.exec.free_bytes();
    r.str(kBuffer, "pointerclass");
    r.cpu().a[0] = 0;
    r.cpu().a[1] = kBuffer;
    r.cpu().a[2] = r.tags(kTagList, {{POINTERA_BitMap, 0x4000},
                                    {POINTERA_WordWidth, 1},
                                    {POINTERA_XOffset, static_cast<u32>(-8)},
                                    {POINTERA_YOffset, static_cast<u32>(-7)},
                                    {POINTERA_XResolution, 6},
                                    {POINTERA_YResolution, 2},
                                    {TAG_DONE, 0}});
    const u32 pointer = r.result(Base::Intuition, intuition_lvo::NewObjectA);
    CHECK(pointer != 0);
    CHECK_EQ(bo.class_of(pointer), bo.pointer_class());
    const auto get = [&](u32 attr) {
        CHECK(bo.get_attr(attr, pointer, kBuffer + 0x20) != 0);
        return r.get32(kBuffer + 0x20);
    };
    CHECK_EQ(get(POINTERA_BitMap), 0x4000u);
    CHECK_EQ(get(POINTERA_WordWidth), 1u);
    CHECK_EQ(get(POINTERA_XOffset), static_cast<u32>(-8));
    CHECK_EQ(get(POINTERA_YOffset), static_cast<u32>(-7));
    CHECK_EQ(get(POINTERA_XResolution), 6u);
    CHECK_EQ(get(POINTERA_YResolution), 2u);
    CHECK_EQ(bo.get_attr(GA_Width, pointer, kBuffer + 0x20), 0u);
    bo.dispose_object(pointer);
    CHECK_EQ(r.exec.free_bytes(), before);
}

void gm_layout_goes_to_gadgets_with_a_relative_box() {
    GfxRig r;
    g_machine = &r.machine;
    Boopsi& bo = r.intuition.boopsi();
    Seen seen;
    seen.boopsi = &bo;
    const u32 cl = bo.make_native_class("layoutgclass", bo.gadget_class(), 0, seen_dispatch, &seen);
    const u32 window = open_window(r, 0);

    // A box of its own: no layout.
    const u32 fixed = bo.new_object(cl, 0, r.tags(kTagList, {{GA_Width, 10}, {GA_Height, 10},
                                                            {TAG_DONE, 0}}));
    add_gadget(r, window, fixed);
    CHECK(seen.layouts.empty());

    // GA_RelWidth and GA_RelSpecial both get it as they join the window.
    const u32 wide = bo.new_object(cl, 0, r.tags(kTagList, {{GA_RelWidth, static_cast<u32>(-20)},
                                                           {GA_Height, 10}, {TAG_DONE, 0}}));
    add_gadget(r, window, wide);
    CHECK_EQ(seen.layouts.size(), 1u);
    CHECK_EQ(seen.layouts[0], 1u);
    const u32 special = bo.new_object(cl, 0, r.tags(kTagList, {{GA_RelSpecial, 1}, {GA_Width, 4},
                                                              {GA_Height, 4}, {TAG_DONE, 0}}));
    CHECK_EQ(r.get16(special + gg_Flags) & GFLG_RELSPECIAL, GFLG_RELSPECIAL);
    add_gadget(r, window, special);
    CHECK_EQ(seen.layouts.size(), 2u);

    // A new size sends it again, not initial, to both.
    CHECK(r.intuition.window_limits(window, 50, 50, 400, 300));
    r.intuition.size_window_to(window, 300, 150);
    CHECK_EQ(r.get16(window + wd_Width), 300u);
    CHECK_EQ(seen.layouts.size(), 4u);
    CHECK_EQ(seen.layouts[2], 0u);
    CHECK_EQ(seen.layouts[3], 0u);

    // A window opened with the gadget lays it out before drawing it.
    seen.layouts.clear();
    const u32 other = bo.new_object(cl, 0, r.tags(kTagList, {{GA_RelHeight, static_cast<u32>(-4)},
                                                            {GA_Width, 10}, {TAG_DONE, 0}}));
    r.put32(kNewWindow + nw_FirstGadget, other);
    r.cpu().a[0] = kNewWindow;
    const u32 second = r.result(Base::Intuition, intuition_lvo::OpenWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(second != 0);
    CHECK_EQ(seen.layouts.size(), 1u);
    CHECK_EQ(seen.layouts[0], 1u);
}

void the_active_custom_gadget_gets_timer_events() {
    GfxRig r;
    g_machine = &r.machine;
    g_now_us = 1000000;
    r.intuition.set_clock(fake_clock);
    Boopsi& bo = r.intuition.boopsi();
    Seen seen;
    seen.boopsi = &bo;
    const u32 cl = bo.make_native_class("tickgclass", bo.gadget_class(), 0, seen_dispatch, &seen);
    const u32 window = open_window(r, 0);
    const u32 g = bo.new_object(cl, 0, r.tags(kTagList, {{GA_Left, 20}, {GA_Top, 20},
                                                        {GA_Width, 30}, {GA_Height, 12},
                                                        {TAG_DONE, 0}}));
    add_gadget(r, window, g);

    // Nothing to send while no gadget has the input.
    r.intuition.intui_ticks();
    CHECK(seen.inputs.empty());
    r.intuition.pointer_event(75, 75, 1, 1, 0, true);
    g_now_us += 100000;
    r.intuition.intui_ticks();
    CHECK_EQ(seen.inputs.size(), 1u);
    CHECK_EQ(seen.inputs[0], 6u);
    // At most ten a second.
    r.intuition.intui_ticks();
    CHECK_EQ(seen.inputs.size(), 1u);
    r.intuition.pointer_event(75, 75, 0, 0, 1, false);
    CHECK_EQ(seen.inputs.size(), 2u);
    CHECK_EQ(seen.inputs[1], 2u);
    g_now_us += 100000;
    r.intuition.intui_ticks();
    CHECK_EQ(seen.inputs.size(), 2u);
}

// The window's IDCMP_IDCMPUPDATE messages and GADGETUPs, in order.
struct Heard {
    std::vector<u32> updates;  // the data of the first pair
    std::vector<u32> tags;     // the tag of the first pair
    u32 gadget_ups = 0;
    u32 up_code = 0;
};

Heard hear(GfxRig& r, u32 window) {
    const u32 port = r.get32(window + wd_UserPort);
    Heard heard;
    for (u32 msg = get_msg(r, port); msg != 0; msg = get_msg(r, port)) {
        const u32 cls = r.get32(msg + im_Class);
        if (cls == IDCMP_IDCMPUPDATE) {
            const u32 list = r.get32(msg + im_IAddress);
            heard.tags.push_back(r.get32(list));
            heard.updates.push_back(r.get32(list + 4));
        } else if (cls == IDCMP_GADGETUP) {
            ++heard.gadget_ups;
            heard.up_code = r.get16(msg + im_Code);
        }
        reply_msg(r, msg);
    }
    return heard;
}

void buttongclass_repeats_while_held() {
    GfxRig r;
    g_now_us = 1000000;
    r.intuition.set_clock(fake_clock);
    Boopsi& bo = r.intuition.boopsi();
    const u32 window = open_window(r, IDCMP_IDCMPUPDATE | IDCMP_GADGETUP);
    const u32 arrow = bo.new_object(bo.sysi_class(), 0,
                                    r.tags(kTagList, {{SYSIA_Which, UPIMAGE}, {TAG_DONE, 0}}));
    CHECK(arrow != 0);
    r.str(kBuffer, "buttongclass");
    r.cpu().a[0] = 0;
    r.cpu().a[1] = kBuffer;
    r.cpu().a[2] = r.tags(kTagList + 0x40, {{GA_Left, 20}, {GA_Top, 20}, {GA_Image, arrow},
                                           {GA_ID, 5}, {GA_RelVerify, 1},
                                           {ICA_TARGET, ICTARGET_IDCMP}, {TAG_DONE, 0}});
    const u32 button = r.result(Base::Intuition, intuition_lvo::NewObjectA);
    CHECK(button != 0);
    CHECK_EQ(r.get32(bo.class_of(button) + cl_Super), bo.gadget_class());
    // The image's size.
    CHECK_EQ(r.get16(button + gg_Width), 18u);
    CHECK_EQ(r.get16(button + gg_Height), 11u);
    add_gadget(r, window, button);

    // The press selects it and tells the window its GA_ID.
    r.intuition.pointer_event(75, 75, 1, 1, 0, true);
    CHECK_EQ(r.get16(button + gg_Flags) & GFLG_SELECTED, GFLG_SELECTED);
    Heard heard = hear(r, window);
    CHECK_EQ(heard.updates.size(), 1u);
    CHECK_EQ(heard.tags[0], GA_ID);
    CHECK_EQ(heard.updates[0], 5u);

    // Each timer event while held says it again.
    for (u32 i = 0; i < 3; ++i) {
        g_now_us += 100000;
        r.intuition.intui_ticks();
    }
    CHECK_EQ(hear(r, window).updates.size(), 3u);

    // Off the button it shows released and stays quiet; back on, it
    // repeats again.
    r.intuition.pointer_event(150, 120, 1, 0, 0, true);
    CHECK_EQ(r.get16(button + gg_Flags) & GFLG_SELECTED, 0u);
    g_now_us += 100000;
    r.intuition.intui_ticks();
    CHECK_EQ(hear(r, window).updates.size(), 0u);
    r.intuition.pointer_event(76, 76, 1, 0, 0, true);
    CHECK_EQ(r.get16(button + gg_Flags) & GFLG_SELECTED, GFLG_SELECTED);

    // Let go over it: a last update and the GADGETUP.
    r.intuition.pointer_event(76, 76, 0, 0, 1, false);
    CHECK_EQ(r.get16(button + gg_Flags) & GFLG_SELECTED, 0u);
    heard = hear(r, window);
    CHECK_EQ(heard.updates.size(), 1u);
    CHECK_EQ(heard.gadget_ups, 1u);
    CHECK_EQ(heard.up_code, 5u);

    // Let go off it: nothing.
    r.intuition.pointer_event(75, 75, 1, 1, 0, true);
    hear(r, window);
    r.intuition.pointer_event(150, 120, 0, 0, 1, true);
    heard = hear(r, window);
    CHECK_EQ(heard.updates.size(), 0u);
    CHECK_EQ(heard.gadget_ups, 0u);
}

u32 get(GfxRig& r, u32 attr, u32 object) {
    CHECK(r.intuition.boopsi().get_attr(attr, object, kBuffer + 0x40) != 0);
    return r.get32(kBuffer + 0x40);
}

void propgclass_counts_lines_and_follows_the_knob() {
    GfxRig r;
    g_now_us = 1000000;
    r.intuition.set_clock(fake_clock);
    Boopsi& bo = r.intuition.boopsi();
    const u32 window = open_window(r, IDCMP_IDCMPUPDATE | IDCMP_GADGETUP);
    r.str(kBuffer, "propgclass");
    r.cpu().a[0] = 0;
    r.cpu().a[1] = kBuffer;
    r.cpu().a[2] = r.tags(kTagList, {{GA_Left, 20}, {GA_Top, 20}, {GA_Width, 16},
                                    {GA_Height, 100}, {GA_RelVerify, 1}, {GA_ID, 3},
                                    {PGA_Freedom, FREEVERT}, {PGA_Total, 100},
                                    {PGA_Visible, 10}, {PGA_Top, 45},
                                    {ICA_TARGET, ICTARGET_IDCMP}, {TAG_DONE, 0}});
    const u32 prop = r.result(Base::Intuition, intuition_lvo::NewObjectA);
    CHECK(prop != 0);
    const u32 pi = r.get32(prop + gg_SpecialInfo);
    CHECK(pi != 0);
    CHECK_EQ(r.get16(pi + pi_Flags) & (FREEVERT | FREEHORIZ | AUTOKNOB), FREEVERT | AUTOKNOB);
    CHECK_EQ(r.get16(pi + pi_VertBody), 10u * 0xFFFF / 100);
    CHECK_EQ(r.get16(pi + pi_VertPot), 45u * 0xFFFF / 90);
    CHECK_EQ(get(r, PGA_Top, prop), 45u);
    CHECK_EQ(get(r, PGA_Total, prop), 100u);
    CHECK_EQ(get(r, PGA_Visible, prop), 10u);
    // A top past the end stops at the last page.
    bo.set_attrs(prop, r.tags(kTagList, {{PGA_Top, 500}, {TAG_DONE, 0}}), 0);
    CHECK_EQ(get(r, PGA_Top, prop), 90u);
    CHECK_EQ(r.get16(pi + pi_VertPot), 0xFFFFu);
    CHECK(bo.set_attrs(prop, r.tags(kTagList, {{PGA_Top, 45}, {TAG_DONE, 0}}), 0) != 0);
    // Setting what it has already changes nothing.
    CHECK_EQ(bo.set_attrs(prop, r.tags(kTagList, {{PGA_Top, 45}, {TAG_DONE, 0}}), 0), 0u);
    add_gadget(r, window, prop);

    // The box is window rows 20..119; inside it rows 21..118 carry the
    // knob, 9 rows high, with 89 rows of travel: at 45 of 90 it starts at
    // row 21 + 44.
    const i32 knob_top = 50 + 21 + 44;
    r.intuition.pointer_event(78, knob_top + 4, 1, 1, 0, true);
    CHECK_EQ(r.get16(pi + pi_Flags) & KNOBHIT, KNOBHIT);
    CHECK(hear(r, window).updates.empty());
    r.intuition.pointer_event(78, knob_top + 24, 1, 0, 0, true);
    const u32 dragged = get(r, PGA_Top, prop);
    CHECK(dragged > 45u);
    Heard heard = hear(r, window);
    CHECK_EQ(heard.updates.size(), 1u);
    CHECK_EQ(heard.tags[0], PGA_Top);
    CHECK_EQ(heard.updates[0], dragged);
    r.intuition.pointer_event(78, knob_top + 24, 0, 0, 1, false);
    CHECK_EQ(r.get16(pi + pi_Flags) & KNOBHIT, 0u);
    heard = hear(r, window);
    CHECK_EQ(heard.updates.size(), 1u);
    CHECK_EQ(heard.updates[0], dragged);
    CHECK_EQ(heard.gadget_ups, 1u);
    CHECK_EQ(heard.up_code, dragged);

    // A press above the knob moves it a page up, and each timer event
    // while held another, until the knob reaches the pointer.
    bo.set_attrs(prop, r.tags(kTagList, {{PGA_Top, 90}, {TAG_DONE, 0}}), 0);
    r.intuition.pointer_event(78, 50 + 22, 1, 1, 0, true);
    CHECK_EQ(get(r, PGA_Top, prop), 80u);
    g_now_us += 100000;
    r.intuition.intui_ticks();
    CHECK_EQ(get(r, PGA_Top, prop), 70u);
    for (u32 i = 0; i < 20; ++i) {
        g_now_us += 100000;
        r.intuition.intui_ticks();
    }
    CHECK_EQ(get(r, PGA_Top, prop), 0u);
    r.intuition.pointer_event(78, 50 + 22, 0, 0, 1, false);
    heard = hear(r, window);
    CHECK(heard.updates.size() >= 3u);
    CHECK_EQ(heard.updates.back(), 0u);
}

// The index of the first method of seen equal to method from at on, or
// the count when there is none.
usize first_of(const Seen& seen, u32 method, usize from = 0) {
    for (usize i = from; i < seen.methods.size(); ++i) {
        if (seen.methods[i] == method) {
            return i;
        }
    }
    return seen.methods.size();
}

usize count_of(const Seen& seen, u32 method) {
    usize n = 0;
    for (const u32 m : seen.methods) {
        n += m == method ? 1 : 0;
    }
    return n;
}

void a_gadget_is_laid_out_before_it_is_drawn_and_once_per_resize() {
    GfxRig r;
    g_machine = &r.machine;
    Boopsi& bo = r.intuition.boopsi();
    Seen seen;
    seen.boopsi = &bo;
    const u32 cl = bo.make_native_class("ordergclass", bo.gadget_class(), 0, seen_dispatch, &seen);
    open_window(r, 0);

    // OpenWindow: GM_LAYOUT, then GM_RENDER.
    const u32 g = bo.new_object(cl, 0, r.tags(kTagList, {{GA_RelWidth, static_cast<u32>(-20)},
                                                        {GA_Top, 20}, {GA_Height, 10},
                                                        {TAG_DONE, 0}}));
    r.put32(kNewWindow + nw_FirstGadget, g);
    r.put32(kNewWindow + nw_Flags, WFLG_ACTIVATE | WFLG_SIZEGADGET | WFLG_DRAGBAR);
    r.put16(kNewWindow + nw_MinWidth, 50);
    r.put16(kNewWindow + nw_MinHeight, 50);
    r.put16(kNewWindow + nw_MaxWidth, 600);
    r.put16(kNewWindow + nw_MaxHeight, 380);
    seen.methods.clear();
    r.cpu().a[0] = kNewWindow;
    const u32 window = r.result(Base::Intuition, intuition_lvo::OpenWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(window != 0);
    CHECK(first_of(seen, GM_LAYOUT) < first_of(seen, GM_RENDER));
    CHECK(first_of(seen, GM_RENDER) < seen.methods.size());

    // SizeWindow-like: laid out, then drawn.
    seen.methods.clear();
    r.intuition.size_window_to(window, 260, 120);
    CHECK_EQ(count_of(seen, GM_LAYOUT), 1u);
    CHECK(first_of(seen, GM_LAYOUT) < first_of(seen, GM_RENDER));

    // ChangeWindowBox that moves and grows: one layout.
    seen.methods.clear();
    r.cpu().a[0] = window;
    r.cpu().d[0] = 10;
    r.cpu().d[1] = 10;
    r.cpu().d[2] = 300;
    r.cpu().d[3] = 200;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::ChangeWindowBox), ExitReason::Halt);
    CHECK_EQ(r.get16(window + wd_Width), 300u);
    CHECK_EQ(count_of(seen, GM_LAYOUT), 1u);
    CHECK(first_of(seen, GM_LAYOUT) < first_of(seen, GM_RENDER));
    CHECK(first_of(seen, GM_RENDER, first_of(seen, GM_LAYOUT)) < seen.methods.size());

    // A size drag lays out once, at its end, and draws the gadget then: a
    // relative gadget is not drawn in a box it was not laid out for.
    seen.methods.clear();
    const i32 corner_x = 10 + 300 - 3;
    const i32 corner_y = 10 + 200 - 3;
    r.intuition.pointer_event(corner_x, corner_y, 1, 1, 0, true);
    r.intuition.pointer_event(corner_x - 20, corner_y - 10, 1, 0, 0, true);
    r.intuition.pointer_event(corner_x - 40, corner_y - 20, 1, 0, 0, true);
    CHECK_EQ(r.get16(window + wd_Width), 260u);
    CHECK_EQ(count_of(seen, GM_LAYOUT), 0u);
    CHECK_EQ(count_of(seen, GM_RENDER), 0u);
    seen.methods.clear();
    r.intuition.pointer_event(corner_x - 40, corner_y - 20, 0, 0, 1, false);
    CHECK_EQ(count_of(seen, GM_LAYOUT), 1u);
    CHECK_EQ(seen.layouts.back(), 0u);
    CHECK(first_of(seen, GM_RENDER, first_of(seen, GM_LAYOUT)) < seen.methods.size());
}

// A dispatcher in 68000 code, at kCode, for a subclass of modelclass that
// asks itself ICM_CHECKLOOP on each OM_UPDATE: counts at kLoopsSeen the
// updates it drops because the loop counter is up, and passes every other
// method and update to modelclass.
constexpr u32 kModelDispatcher = kCode;
constexpr u32 kLoopsSeen = kCode + 0xF0;

void put_model_dispatcher(GfxRig& r) {
    const u16 code[] = {
        0x2011,                          // move.l (a1),d0
        0x0C80, 0x0000, 0x0108,          // cmpi.l #OM_UPDATE,d0
        0x662A,                          // bne.s super
        0x48E7, 0x00E0,                  // movem.l a0-a2,-(sp)
        0x2F3C, 0x0000, 0x0404,          // move.l #ICM_CHECKLOOP,-(sp)
        0x224F,                          // movea.l sp,a1
        0x2068, 0x0018,                  // movea.l cl_Super(a0),a0
        0x2668, 0x0008,                  // movea.l h_Entry(a0),a3
        0x4E93,                          // jsr (a3)
        0x588F,                          // addq.l #4,sp
        0x4CDF, 0x0700,                  // movem.l (sp)+,a0-a2
        0x4A80,                          // tst.l d0
        0x670A,                          // beq.s super
        0x52B9, static_cast<u16>(kLoopsSeen >> 16), static_cast<u16>(kLoopsSeen),
        0x7000, 0x4E75,                  // moveq #0,d0; rts
        0x2068, 0x0018,                  // super: movea.l cl_Super(a0),a0
        0x2F28, 0x0008, 0x4E75,          // move.l h_Entry(a0),-(sp); rts
    };
    for (u32 i = 0; i < sizeof code / sizeof code[0]; ++i) {
        r.put16(kModelDispatcher + i * 2, code[i]);
    }
    r.put32(kLoopsSeen, 0);
    r.jit.flush();
}

void a_68000_model_subclass_sees_the_loop_during_a_broadcast() {
    GfxRig r;
    Boopsi& bo = r.intuition.boopsi();
    put_model_dispatcher(r);
    const u32 cl = bo.make_class(0, 0, bo.model_class(), 4, 0);
    CHECK(cl != 0);
    r.put32(cl + h_Entry, kModelDispatcher);
    const u32 model = bo.new_object(cl, 0, r.tags(kTagList, {{TAG_DONE, 0}}));
    CHECK(model != 0);
    // A member that sends the update straight back, and one that keeps it.
    const u32 back = bo.new_object(bo.ic_class(), 0,
                                   r.tags(kTagList, {{ICA_TARGET, model}, {TAG_DONE, 0}}));
    const u32 keeper = bo.new_object(bo.gadget_class(), 0, r.tags(kTagList, {{TAG_DONE, 0}}));
    const u32 add_back[] = {OM_ADDMEMBER, back};
    const u32 add_keeper[] = {OM_ADDMEMBER, keeper};
    bo.send(cl, model, add_back, 2);
    bo.send(cl, model, add_keeper, 2);

    const u32 update[] = {OM_UPDATE, r.tags(kTagList + 0x40, {{GA_Width, 17}, {TAG_DONE, 0}}), 0,
                          0};
    bo.send(cl, model, update, 4);
    CHECK_EQ(r.get16(keeper + gg_Width), 17u);
    CHECK_EQ(r.get32(kLoopsSeen), 1u);
    const u32 checkloop[] = {ICM_CHECKLOOP};
    CHECK_EQ(bo.send(cl, model, checkloop, 1), 0u);
    bo.dispose_object(model);
    CHECK_EQ(r.get32(bo.gadget_class() + cl_ObjectCount), 0u);
    CHECK_EQ(r.get32(bo.ic_class() + cl_ObjectCount), 0u);
}

// A gadgetclass subclass in 68000 code at kCode + 0x100: method $7000
// answers $55, every other goes to gadgetclass.
constexpr u32 kPlainDispatcher = kCode + 0x100;

u32 over_68000_dispatch(void* ctx, u32 cl, u32 object, u32 msg) {
    return static_cast<Boopsi*>(ctx)->call_super(cl, object, msg);
}

void a_native_class_over_a_68000_superclass() {
    GfxRig r;
    Boopsi& bo = r.intuition.boopsi();
    const u16 code[] = {
        0x0C91, 0x0000, 0x7000,  // cmpi.l #$7000,(a1)
        0x6604,                  // bne.s super
        0x7055, 0x4E75,          // moveq #$55,d0; rts
        0x2068, 0x0018,          // super: movea.l cl_Super(a0),a0
        0x2F28, 0x0008, 0x4E75,  // move.l h_Entry(a0),-(sp); rts
    };
    for (u32 i = 0; i < sizeof code / sizeof code[0]; ++i) {
        r.put16(kPlainDispatcher + i * 2, code[i]);
    }
    r.jit.flush();
    const u32 middle = bo.make_class(0, 0, bo.gadget_class(), 4, 0);
    r.put32(middle + h_Entry, kPlainDispatcher);
    const u32 top = bo.make_native_class("overgclass", middle, 4, over_68000_dispatch, &bo);
    CHECK(top != 0);
    CHECK_EQ(r.get16(top + cl_InstOffset), ExtGadget_SIZEOF + 4);
    const u32 g = bo.new_object(top, 0, r.tags(kTagList, {{GA_Width, 23}, {TAG_DONE, 0}}));
    CHECK(g != 0);
    CHECK_EQ(r.get16(g + gg_Width), 23u);
    const u32 ask[] = {0x7000};
    CHECK_EQ(bo.send(top, g, ask, 1), 0x55u);
    bo.dispose_object(g);
    CHECK_EQ(r.get32(top + cl_ObjectCount), 0u);
}

// A gadgetclass subclass in 68000 code at kCode + 0x140 that sends every
// method to its superclass with a JSR, as DoSuperMethod does.
constexpr u32 kSuperCaller = kCode + 0x140;

void put_super_caller(GfxRig& r) {
    const u16 code[] = {
        0x2F0B,          // move.l a3,-(sp)
        0x2068, 0x0018,  // movea.l cl_Super(a0),a0
        0x2668, 0x0008,  // movea.l h_Entry(a0),a3
        0x4E93,          // jsr (a3)
        0x265F,          // movea.l (sp)+,a3
        0x4E75,          // rts
    };
    for (u32 i = 0; i < sizeof code / sizeof code[0]; ++i) {
        r.put16(kSuperCaller + i * 2, code[i]);
    }
    r.jit.flush();
}

// A native class that answers method $7001 with $66 and passes every
// other on.
struct PassOn {
    Boopsi* boopsi = nullptr;
    u32 calls = 0;
};

u32 pass_on_dispatch(void* ctx, u32 cl, u32 object, u32 msg) {
    PassOn& s = *static_cast<PassOn*>(ctx);
    ++s.calls;
    native::Bus b(*g_machine);
    if (b.r32(msg) == 0x7001) {
        return 0x66;
    }
    return s.boopsi->pass_on(cl, object, msg);
}

// Takes every nested 68000 run exec has left but keep; returns how many.
u32 take_nested_runs(GfxRig& r, u32 keep) {
    u32 taken = 0;
    while (r.exec.enter_nested_run()) {
        ++taken;
    }
    for (u32 i = 0; i < keep && taken > 0; ++i) {
        r.exec.leave_nested_run();
        --taken;
    }
    return taken;
}

void give_back_nested_runs(GfxRig& r, u32 taken) {
    for (u32 i = 0; i < taken; ++i) {
        r.exec.leave_nested_run();
    }
}

void a_native_class_passes_a_method_on_to_68000_code_without_nesting() {
    GfxRig r;
    g_machine = &r.machine;
    Boopsi& bo = r.intuition.boopsi();
    const u16 plain[] = {
        0x0C91, 0x0000, 0x7000,  // cmpi.l #$7000,(a1)
        0x6604,                  // bne.s super
        0x7055, 0x4E75,          // moveq #$55,d0; rts
        0x2068, 0x0018,          // super: movea.l cl_Super(a0),a0
        0x2F28, 0x0008, 0x4E75,  // move.l h_Entry(a0),-(sp); rts
    };
    for (u32 i = 0; i < sizeof plain / sizeof plain[0]; ++i) {
        r.put16(kPlainDispatcher + i * 2, plain[i]);
    }
    put_super_caller(r);
    PassOn s;
    s.boopsi = &bo;
    // 68000 code over the native class over 68000 code over gadgetclass.
    const u32 bottom = bo.make_class(0, 0, bo.gadget_class(), 4, 0);
    r.put32(bottom + h_Entry, kPlainDispatcher);
    const u32 middle = bo.make_native_class("passgclass", bottom, 4, pass_on_dispatch, &s);
    CHECK(middle != 0);
    const u32 top = bo.make_class(0, 0, middle, 4, 0);
    r.put32(top + h_Entry, kSuperCaller);

    // With one nested run left, which the top class takes, OM_NEW goes
    // all the way down to gadgetclass.
    u32 taken = take_nested_runs(r, 1);
    const u32 g = bo.new_object(top, 0, r.tags(kTagList, {{GA_Width, 31}, {TAG_DONE, 0}}));
    CHECK(g != 0);
    CHECK_EQ(r.get16(g + gg_Width), 31u);
    const u32 ask[] = {0x7000};
    CHECK_EQ(bo.send(top, g, ask, 1), 0x55u);
    const u32 own[] = {0x7001};
    CHECK_EQ(bo.send(top, g, own, 1), 0x66u);
    CHECK_EQ(s.calls, 3u);
    give_back_nested_runs(r, taken);

    // 68000 code with no nested run left at all reaches the bottom class
    // through the native one.
    constexpr u32 kAsk = kCode + 0x200;
    r.put32(kAsk, 0x7000);
    const u16 code[] = {
        0x207C, static_cast<u16>(top >> 16), static_cast<u16>(top),   // movea.l #top,a0
        0x227C, static_cast<u16>(kAsk >> 16), static_cast<u16>(kAsk),  // movea.l #msg,a1
        0x247C, static_cast<u16>(g >> 16), static_cast<u16>(g),               // movea.l #g,a2
        0x2668, 0x0008,                                                     // movea.l 8(a0),a3
        0x4E93,                                                             // jsr (a3)
        0x4EF8, 0x0400,                                                     // jmp $400.w
    };
    for (u32 i = 0; i < sizeof code / sizeof code[0]; ++i) {
        r.put16(kCode + i * 2, code[i]);
    }
    r.jit.flush();
    taken = take_nested_runs(r, 0);
    r.cpu().pc = kCode;
    r.cpu().a[7] = gfx_test::kStack;
    CHECK_EQ(r.jit.run(1000000), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0x55u);
    CHECK_EQ(r.cpu().a[2], g);
    CHECK_EQ(s.calls, 4u);

    // Sent from C++, the method is passed on with a nested run of its own,
    // which it cannot have without one left.
    CHECK_EQ(bo.send(middle, g, ask, 1), 0u);
    give_back_nested_runs(r, taken);
    CHECK_EQ(bo.send(middle, g, ask, 1), 0x55u);

    // Called by its A-line id from C++, as a device vector is, the class
    // passes the method on with a nested run and answers it itself.
    const u16 id = static_cast<u16>(r.get16(r.get32(middle + h_Entry)) & 0x0FFF);
    r.cpu().a[0] = middle;
    r.cpu().a[1] = kAsk;
    r.cpu().a[2] = g;
    r.cpu().d[0] = 0;
    r.cpu().pc = 0x1234;
    CHECK(r.jit.call_native(id));
    CHECK_EQ(r.cpu().d[0], 0x55u);
    CHECK_EQ(r.cpu().pc, 0x1234u);
    bo.dispose_object(g);
    CHECK_EQ(r.get32(top + cl_ObjectCount), 0u);
    CHECK_EQ(r.get32(middle + cl_ObjectCount), 0u);
}

// A native class whose run ends inside it (a fault in what it called)
// sends nothing more: its superclass in 68000 code does not run.
u32 ending_dispatch(void* ctx, u32 cl, u32 object, u32 msg) {
    g_machine->cpu.exit = ExitReason::BusError;
    return static_cast<Boopsi*>(ctx)->call_super(cl, object, msg);
}

void an_ended_run_calls_no_superclass() {
    GfxRig r;
    g_machine = &r.machine;
    Boopsi& bo = r.intuition.boopsi();
    constexpr u32 kMarker = kCode + 0x300;
    constexpr u32 kMarking = kCode + 0x180;
    const u16 marking[] = {
        0x23FC, 0x0000, 0x0001,                                              // move.l #1,
        static_cast<u16>(kMarker >> 16), static_cast<u16>(kMarker),          //   kMarker
        0x4E75,                                                              // rts
    };
    for (u32 i = 0; i < sizeof marking / sizeof marking[0]; ++i) {
        r.put16(kMarking + i * 2, marking[i]);
    }
    const u32 bottom = bo.make_class(0, 0, bo.gadget_class(), 0, 0);
    r.put32(bottom + h_Entry, kMarking);
    const u32 top = bo.make_native_class("endinggclass", bottom, 0, ending_dispatch, &bo);
    CHECK(top != 0);
    r.put32(kMarker, 0);
    constexpr u32 kAsk = kCode + 0x200;
    r.put32(kAsk, 0x7000);
    const u16 code[] = {
        0x207C, static_cast<u16>(top >> 16), static_cast<u16>(top),     // movea.l #top,a0
        0x227C, static_cast<u16>(kAsk >> 16), static_cast<u16>(kAsk),  // movea.l #msg,a1
        0x2668, 0x0008,                                                 // movea.l 8(a0),a3
        0x4E93,                                                         // jsr (a3)
        0x4EF8, 0x0400,                                                 // jmp $400.w
    };
    for (u32 i = 0; i < sizeof code / sizeof code[0]; ++i) {
        r.put16(kCode + i * 2, code[i]);
    }
    r.jit.flush();
    r.cpu().pc = kCode;
    r.cpu().a[7] = gfx_test::kStack;
    CHECK_EQ(r.jit.run(1000000), ExitReason::BusError);
    CHECK_EQ(r.get32(kMarker), 0u);
    // From C++ outside any run the same superclass runs.
    const u32 msg[] = {0x7000};
    bo.send(bottom, 0, msg, 1);
    CHECK_EQ(r.get32(kMarker), 1u);
}

void a_native_class_passes_a_method_on_to_a_native_superclass() {
    GfxRig r;
    g_machine = &r.machine;
    Boopsi& bo = r.intuition.boopsi();
    put_super_caller(r);
    PassOn s;
    s.boopsi = &bo;
    const u32 middle = bo.make_native_class("passgclass", bo.gadget_class(), 4, pass_on_dispatch,
                                            &s);
    CHECK(middle != 0);
    const u32 top = bo.make_class(0, 0, middle, 4, 0);
    r.put32(top + h_Entry, kSuperCaller);
    const u32 g = bo.new_object(top, 0, r.tags(kTagList, {{GA_Width, 12}, {TAG_DONE, 0}}));
    CHECK(g != 0);
    CHECK_EQ(r.get16(g + gg_Width), 12u);
    const u32 own[] = {0x7001};
    CHECK_EQ(bo.send(top, g, own, 1), 0x66u);
    bo.dispose_object(g);
    CHECK_EQ(r.get32(top + cl_ObjectCount), 0u);
    CHECK_EQ(s.calls, 3u);
}

// What the member class does with an OM_UPDATE: counts it, and may take
// another member out, take itself out and dispose of itself, or end the
// task it runs in.
struct Member {
    Boopsi* boopsi = nullptr;
    u32 model = 0;
    u32 remove_next = 0;
    bool dispose_self = false;
    bool end_task = false;
    std::vector<u32> updated;
};

u32 member_dispatch(void* ctx, u32 cl, u32 object, u32 msg) {
    Member& s = *static_cast<Member*>(ctx);
    native::Bus b(*g_machine);
    if (b.r32(msg) != OM_UPDATE) {
        return s.boopsi->call_super(cl, object, msg);
    }
    s.updated.push_back(object);
    if (s.remove_next != 0) {
        const u32 remove[] = {OM_REMMEMBER, s.remove_next};
        s.remove_next = 0;
        s.boopsi->send(s.boopsi->class_of(s.model), s.model, remove, 2);
    }
    if (s.dispose_self) {
        s.dispose_self = false;
        const u32 remove[] = {OM_REMMEMBER, object};
        s.boopsi->send(s.boopsi->class_of(s.model), s.model, remove, 2);
        s.boopsi->dispose_object(object);
    }
    if (s.end_task) {
        s.end_task = false;
        s.boopsi->task_ended(0);
    }
    return 0;
}

void members_may_leave_or_go_during_a_broadcast() {
    GfxRig r;
    g_machine = &r.machine;
    Boopsi& bo = r.intuition.boopsi();
    Member s;
    s.boopsi = &bo;
    const u32 cl = bo.make_native_class("memberclass", bo.root_class(), 0, member_dispatch, &s);
    const u32 model = bo.new_object(bo.model_class(), 0, r.tags(kTagList, {{TAG_DONE, 0}}));
    s.model = model;
    u32 m[3];
    for (u32& member : m) {
        member = bo.new_object(cl, 0, r.tags(kTagList, {{TAG_DONE, 0}}));
        const u32 add[] = {OM_ADDMEMBER, member};
        bo.send(bo.class_of(model), model, add, 2);
    }
    const u32 update[] = {OM_UPDATE, r.tags(kTagList + 0x40, {{GA_ID, 1}, {TAG_DONE, 0}}), 0, 0};

    // The first takes the second out: the third still hears it.
    s.remove_next = m[1];
    bo.send(bo.class_of(model), model, update, 4);
    CHECK(s.updated == (std::vector<u32>{m[0], m[2]}));

    // A member that leaves and disposes of itself: the rest go on.
    s.updated.clear();
    s.dispose_self = true;
    bo.send(bo.class_of(model), model, update, 4);
    CHECK(s.updated == (std::vector<u32>{m[0], m[2]}));
    s.updated.clear();
    bo.send(bo.class_of(model), model, update, 4);
    CHECK(s.updated == (std::vector<u32>{m[2]}));

    // A task that ends inside the broadcast leaves the loop counter down.
    s.end_task = true;
    bo.send(bo.class_of(model), model, update, 4);
    const u32 checkloop[] = {ICM_CHECKLOOP};
    CHECK_EQ(bo.send(bo.class_of(model), model, checkloop, 1), 0u);
    s.updated.clear();
    bo.send(bo.class_of(model), model, update, 4);
    CHECK(s.updated == (std::vector<u32>{m[2]}));

    // A removed node keeps its successor for a walker standing on it.
    const u32 node = m[1] - Object_SIZEOF;
    CHECK(r.get32(node) != 0);
    CHECK_EQ(r.get32(node + 4), 0u);
    bo.dispose_object(m[1]);
    bo.dispose_object(model);
    CHECK_EQ(r.get32(cl + cl_ObjectCount), 0u);
}

void icspecial_code_sets_the_code_of_the_update() {
    GfxRig r;
    Boopsi& bo = r.intuition.boopsi();
    const u32 window = open_window(r, IDCMP_IDCMPUPDATE);
    const u32 info = r.intuition.make_gadget_info(window);
    const u32 source = GA_Dummy + 0x300;
    const u32 map = r.tags(kTagList, {{source, ICSPECIAL_CODE}, {TAG_DONE, 0}});
    const u32 port = r.get32(window + wd_UserPort);
    for (const u32 cl : {bo.ic_class(), bo.gadget_class()}) {
        const u32 object = bo.new_object(
            cl, 0, r.tags(kTagList + 0x40,
                          {{ICA_TARGET, ICTARGET_IDCMP}, {ICA_MAP, map}, {TAG_DONE, 0}}));
        send_notify(r, object, r.tags(kTagList + 0x80, {{source, 0x51234}, {TAG_DONE, 0}}), info);
        const u32 msg = get_msg(r, port);
        CHECK(msg != 0);
        CHECK_EQ(r.get32(msg + im_Class), IDCMP_IDCMPUPDATE);
        CHECK_EQ(r.get16(msg + im_Code), 0x1234u);
        reply_msg(r, msg);
        bo.dispose_object(object);
    }
    r.intuition.free_gadget_info(info);
}

// A tick and a move that come while the gadget is in GM_HANDLEINPUT, as
// when its method waits: the tick is skipped, the move waits for the
// method to end, and nothing of the running call is freed under it.
struct Reenter {
    GfxRig* rig = nullptr;
    Boopsi* boopsi = nullptr;
    u32 depth = 0;
    u32 max_depth = 0;
    u32 calls = 0;
    u32 timers = 0;
    u32 outer_info = 0;
    bool info_freed = false;
    bool remove_self = false;
    u32 window = 0;
};

u32 reenter_dispatch(void* ctx, u32 cl, u32 object, u32 msg) {
    Reenter& s = *static_cast<Reenter*>(ctx);
    native::Bus b(*g_machine);
    switch (b.r32(msg)) {
    case GM_GOACTIVE:
        return GMR_MEACTIVE;
    case GM_HANDLEINPUT: {
        ++s.depth;
        ++s.calls;
        s.max_depth = std::max(s.max_depth, s.depth);
        if (b.r8(b.r32(msg + 8) + 4) == 6) {
            ++s.timers;
        }
        if (s.depth == 1 && s.calls == 1) {
            s.outer_info = b.r32(msg + 4);
            g_now_us += 100000;
            s.rig->intuition.intui_ticks();
            s.rig->intuition.pointer_event(80, 76, 1, 0, 0, true);
            if (s.remove_self) {
                s.rig->cpu().a[0] = s.window;
                s.rig->cpu().a[1] = object;
                s.rig->cpu().d[0] = 1;
                s.rig->call(Base::Intuition, intuition_lvo::RemoveGList);
            }
            const u32 again = s.rig->exec.alloc(GadgetInfo_SIZEOF + 2, MEMF_PUBLIC);
            s.info_freed = again == s.outer_info;
            s.rig->exec.free(again, GadgetInfo_SIZEOF + 2);
        }
        --s.depth;
        return GMR_MEACTIVE;
    }
    default:
        return s.boopsi->call_super(cl, object, msg);
    }
}

void input_that_comes_during_a_gadget_method_waits_for_it() {
    for (const bool remove_self : {false, true}) {
        GfxRig r;
        g_machine = &r.machine;
        g_now_us = 1000000;
        r.intuition.set_clock(fake_clock);
        Boopsi& bo = r.intuition.boopsi();
        Reenter s;
        s.rig = &r;
        s.boopsi = &bo;
        s.remove_self = remove_self;
        const u32 cl = bo.make_native_class("reentergclass", bo.gadget_class(), 0, reenter_dispatch,
                                            &s);
        const u32 window = open_window(r, 0);
        s.window = window;
        const u32 g = bo.new_object(cl, 0, r.tags(kTagList, {{GA_Left, 20}, {GA_Top, 20},
                                                            {GA_Width, 30}, {GA_Height, 12},
                                                            {TAG_DONE, 0}}));
        add_gadget(r, window, g);
        const u32 before = r.exec.free_bytes();
        r.intuition.pointer_event(75, 75, 1, 1, 0, true);
        r.intuition.pointer_event(76, 75, 1, 0, 0, true);
        CHECK_EQ(s.max_depth, 1u);
        CHECK_EQ(s.timers, 0u);
        CHECK(!s.info_freed);
        // The move came after, unless the gadget had gone.
        CHECK_EQ(s.calls, remove_self ? 1u : 2u);
        if (remove_self) {
            // Its GadgetInfo and InputEvent are given back once it returned.
            CHECK_EQ(r.exec.free_bytes(), before);
        }
        r.intuition.pointer_event(80, 76, 0, 0, 1, false);
    }
}

void a_horizontal_prop_follows_the_knob_and_clamps_its_top() {
    GfxRig r;
    g_now_us = 1000000;
    r.intuition.set_clock(fake_clock);
    Boopsi& bo = r.intuition.boopsi();
    const u32 window = open_window(r, IDCMP_IDCMPUPDATE);
    const u32 prop = bo.new_object(
        bo.find_class_named("propgclass"), 0,
        r.tags(kTagList, {{GA_Left, 20}, {GA_Top, 20}, {GA_Width, 100}, {GA_Height, 12},
                          {PGA_Freedom, FREEHORIZ}, {PGA_Total, 1000}, {PGA_Visible, 100},
                          {PGA_Top, 450}, {ICA_TARGET, ICTARGET_IDCMP}, {TAG_DONE, 0}}));
    CHECK(prop != 0);
    const u32 pi = r.get32(prop + gg_SpecialInfo);
    CHECK_EQ(r.get16(pi + pi_HorizBody), 100u * 0xFFFF / 1000);
    CHECK_EQ(r.get16(pi + pi_HorizPot), 450u * 0xFFFF / 900);
    CHECK_EQ(r.get16(pi + pi_VertPot), 0u);
    bo.set_attrs(prop, r.tags(kTagList, {{PGA_Top, static_cast<u32>(-3)}, {TAG_DONE, 0}}), 0);
    CHECK_EQ(get(r, PGA_Top, prop), 0u);
    CHECK_EQ(r.get16(pi + pi_HorizPot), 0u);
    bo.set_attrs(prop, r.tags(kTagList, {{PGA_Top, 450}, {TAG_DONE, 0}}), 0);
    add_gadget(r, window, prop);

    // Inside columns 21..118 the knob is 9 wide with 89 of travel, at 450
    // of 900 from column 21 + 44.
    const i32 knob_left = 50 + 21 + 44;
    r.intuition.pointer_event(knob_left + 4, 50 + 26, 1, 1, 0, true);
    CHECK_EQ(r.get16(pi + pi_Flags) & KNOBHIT, KNOBHIT);
    r.intuition.pointer_event(knob_left + 4 + 30, 50 + 26, 1, 0, 0, true);
    const u32 moved = get(r, PGA_Top, prop);
    CHECK(moved > 450u);
    Heard heard = hear(r, window);
    CHECK_EQ(heard.updates.size(), 1u);
    CHECK_EQ(heard.tags[0], PGA_Top);
    CHECK_EQ(heard.updates[0], moved);
    // A move along the other axis moves no knob and says nothing.
    r.intuition.pointer_event(knob_left + 4 + 30, 50 + 28, 1, 0, 0, true);
    CHECK(hear(r, window).updates.empty());
    r.intuition.pointer_event(knob_left + 4 + 30, 50 + 28, 0, 0, 1, false);
    heard = hear(r, window);
    CHECK_EQ(heard.updates.size(), 1u);
    CHECK_EQ(heard.updates[0], moved);

    // A press left of the knob pages a hundred lines left.
    r.intuition.pointer_event(50 + 22, 50 + 26, 1, 1, 0, true);
    CHECK_EQ(get(r, PGA_Top, prop), moved - 100);
    r.intuition.pointer_event(50 + 22, 50 + 26, 0, 0, 1, false);
}

// With an input task, events wait in the queue for it, in order, a move
// taking the place of the move before it and a tick waiting only once.
bool fake_create(void*, u32) { return true; }
void fake_destroy(void*, u32) {}

void with_an_input_task_events_wait_for_it_in_order() {
    GfxRig r;
    g_machine = &r.machine;
    g_now_us = 1000000;
    r.intuition.set_clock(fake_clock);
    r.exec.set_task_host(hombre::TaskHost{fake_create, nullptr, fake_destroy, nullptr, nullptr,
                                          nullptr, nullptr, nullptr});
    Boopsi& bo = r.intuition.boopsi();
    Seen seen;
    seen.boopsi = &bo;
    const u32 cl = bo.make_native_class("queuegclass", bo.gadget_class(), 0, seen_dispatch, &seen);
    const u32 window = open_window(r, IDCMP_MOUSEBUTTONS);
    const u32 g = bo.new_object(cl, 0, r.tags(kTagList, {{GA_Left, 20}, {GA_Top, 20},
                                                        {GA_Width, 30}, {GA_Height, 12},
                                                        {TAG_DONE, 0}}));
    add_gadget(r, window, g);
    CHECK(r.intuition.start_input_task());
    const u32 task = r.intuition.input_task();
    CHECK(task != 0);
    CHECK(r.intuition.start_input_task());
    CHECK_EQ(r.intuition.input_task(), task);

    seen.methods.clear();
    r.intuition.pointer_event(75, 75, 1, 1, 0, true);
    r.intuition.pointer_event(76, 75, 1, 0, 0, true);
    r.intuition.pointer_event(77, 75, 1, 0, 0, true);
    g_now_us += 100000;
    r.intuition.intui_ticks();
    g_now_us += 100000;
    r.intuition.intui_ticks();
    r.intuition.pointer_event(77, 75, 1, 0, 1, false);
    // Nothing ran; the task was told.
    CHECK(seen.methods.empty());
    CHECK(r.intuition.input_pending());
    CHECK((r.get32(task + TC_SIGRECVD) & (1u << 16)) != 0);

    r.intuition.run_input();
    CHECK(!r.intuition.input_pending());
    // Hit test and activation, one move, one timer event, the release and
    // the end of the gadget's input.
    CHECK(seen.methods == (std::vector<u32>{GM_HITTEST, GM_GOACTIVE, GM_HANDLEINPUT,
                                             GM_HANDLEINPUT, GM_HANDLEINPUT, GM_GOINACTIVE}));
    CHECK(seen.inputs == (std::vector<u32>{2, 6, 2}));
}

}

int main() {
    a_native_class_dispatches_without_68000_code();
    icclass_passes_updates_on_to_its_target();
    the_loop_counter_stops_a_notification_that_comes_round();
    modelclass_broadcasts_to_its_members();
    pointerclass_keeps_its_attributes();
    gm_layout_goes_to_gadgets_with_a_relative_box();
    the_active_custom_gadget_gets_timer_events();
    buttongclass_repeats_while_held();
    propgclass_counts_lines_and_follows_the_knob();
    a_gadget_is_laid_out_before_it_is_drawn_and_once_per_resize();
    a_68000_model_subclass_sees_the_loop_during_a_broadcast();
    a_native_class_over_a_68000_superclass();
    a_native_class_passes_a_method_on_to_68000_code_without_nesting();
    a_native_class_passes_a_method_on_to_a_native_superclass();
    an_ended_run_calls_no_superclass();
    members_may_leave_or_go_during_a_broadcast();
    icspecial_code_sets_the_code_of_the_update();
    input_that_comes_during_a_gadget_method_waits_for_it();
    a_horizontal_prop_follows_the_knob_and_clamps_its_top();
    with_an_input_task_events_wait_for_it_in_order();
    return check::finish("test_boopsi_classes");
}
