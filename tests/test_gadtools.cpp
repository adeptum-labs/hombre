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

#include "gfx_rig.hpp"
#include "libs/gadtools.hpp"
#include "libs/utility.hpp"

using namespace hombre;
using namespace hombre::m68k;
using namespace hombre::ndk;
using gfx_test::Base;
using gfx_test::GfxRig;
using gfx_test::kCall;
using gfx_test::kData;
using gfx_test::kStack;
using gfx_test::logged;

namespace {

constexpr u32 kNewScreen = kData;
constexpr u32 kNewMenus = kData + 0x200;
constexpr u32 kFakeMenu = kData + 0x400;
constexpr u32 kFileLabel = kData + 0x600;
constexpr u32 kEditLabel = kData + 0x620;
constexpr u32 kOpenLabel = kData + 0x640;
constexpr u32 kRecentLabel = kData + 0x660;
constexpr u32 kWrapLabel = kData + 0x680;
constexpr u32 kQuitLabel = kData + 0x6A0;
constexpr u32 kPasteLabel = kData + 0x6C0;
constexpr u32 kOpenKey = kData + 0x6E0;
constexpr u32 kTags = kData + 0x780;
constexpr u32 kList = kData + 0x800;
constexpr u32 kMessage = kData + 0x880;
constexpr u32 kBitmap = kData + 0x900;
constexpr u32 kRastPort = kData + 0xA00;

// struct NewMenu
constexpr u32 nm_Type = 0;
constexpr u32 nm_Label = 2;
constexpr u32 nm_CommKey = 6;
constexpr u32 nm_Flags = 10;
constexpr u32 nm_UserData = 16;
constexpr u32 NewMenu_SIZEOF = 20;
constexpr u8 NM_END = 0;
constexpr u8 NM_TITLE = 1;
constexpr u8 NM_ITEM = 2;
constexpr u8 NM_SUB = 3;
constexpr u32 NM_BARLABEL = 0xFFFFFFFF;
constexpr u32 NM_ITEMDISABLED = 0x0010;

// struct Menu and struct MenuItem, with the user data after each.
constexpr u32 mu_NextMenu = 0;
constexpr u32 mu_LeftEdge = 4;
constexpr u32 mu_TopEdge = 6;
constexpr u32 mu_Width = 8;
constexpr u32 mu_Height = 10;
constexpr u32 mu_Flags = 12;
constexpr u32 mu_MenuName = 14;
constexpr u32 mu_FirstItem = 18;
constexpr u32 Menu_SIZEOF = 30;
constexpr u32 mi_NextItem = 0;
constexpr u32 mi_LeftEdge = 4;
constexpr u32 mi_TopEdge = 6;
constexpr u32 mi_Width = 8;
constexpr u32 mi_Height = 10;
constexpr u32 mi_Flags = 12;
constexpr u32 mi_ItemFill = 18;
constexpr u32 mi_Command = 26;
constexpr u32 mi_SubItem = 28;
constexpr u32 MenuItem_SIZEOF = 34;
constexpr u32 MENUENABLED = 0x0001;
constexpr u32 CHECKIT = 0x0001;
constexpr u32 ITEMTEXT = 0x0002;
constexpr u32 COMMSEQ = 0x0004;
constexpr u32 ITEMENABLED = 0x0010;
constexpr u32 HIGHCOMP = 0x0040;
constexpr u32 CHECKED = 0x0100;

// struct IntuiText
constexpr u32 it_IText = 12;

constexpr u32 kGadgHNone = 3;
constexpr u32 kShinePen = 2;
constexpr u32 kShadowPen = 1;
constexpr u32 GTBB_Recessed = 0x80080033;

void init_gadtools(GfxRig& r, GadTools& g) {
    CHECK(g.init(r.machine, r.exec, r.graphics));
}

ExitReason gt_call(GfxRig& r, GadTools& g, u32 lvo) {
    r.put16(kCall, 0x4EAE);
    r.put16(kCall + 2, static_cast<u32>(0x10000 - lvo));
    r.put16(kCall + 4, 0x4EF8);  // JMP $400.w
    r.put16(kCall + 6, 0x0400);
    r.jit.flush();
    r.cpu().a[6] = g.gadtools_base();
    r.cpu().a[7] = kStack;
    r.cpu().pc = kCall;
    return r.jit.run(1000000);
}

u32 gt_result(GfxRig& r, GadTools& g, u32 lvo) {
    CHECK_EQ(gt_call(r, g, lvo), ExitReason::Halt);
    return r.cpu().d[0];
}

u32 avail_mem(GfxRig& r) {
    r.cpu().d[1] = 0;
    return r.result(Base::Exec, exec_lvo::AvailMem);
}

void put_new_screen(GfxRig& r, u32 at, u32 width, u32 height, u32 depth, u32 title) {
    r.put16(at + ns_LeftEdge, 0);
    r.put16(at + ns_TopEdge, 0);
    r.put16(at + ns_Width, width);
    r.put16(at + ns_Height, height);
    r.put16(at + ns_Depth, depth);
    r.put8(at + ns_DetailPen, 0);
    r.put8(at + ns_BlockPen, 1);
    r.put16(at + ns_ViewModes, 0);
    r.put16(at + ns_Type, CUSTOMSCREEN);
    r.put32(at + ns_Font, 0);
    r.put32(at + ns_DefaultTitle, title);
    r.put32(at + ns_CustomBitMap, 0);
}

u32 open_new_screen(GfxRig& r, u32 new_screen) {
    r.cpu().a[0] = new_screen;
    return r.result(Base::Intuition, intuition_lvo::OpenScreen);
}

u32 make_surface(GfxRig& r) {
    constexpr u32 kWidth = 640;
    constexpr u32 kHeight = 400;
    const u32 row_bytes = (kWidth + 15) / 16 * 2;
    const u32 plane_bytes = row_bytes * kHeight;
    const u32 plane0 = r.exec.alloc(plane_bytes, MEMF_CHIP | MEMF_CLEAR);
    const u32 plane1 = r.exec.alloc(plane_bytes, MEMF_CHIP | MEMF_CLEAR);
    CHECK(plane0 != 0);
    CHECK(plane1 != 0);
    r.cpu().a[0] = kBitmap;
    r.cpu().d[0] = 2;
    r.cpu().d[1] = kWidth;
    r.cpu().d[2] = kHeight;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::InitBitMap), ExitReason::Halt);
    r.put32(kBitmap + bm_Planes, plane0);
    r.put32(kBitmap + bm_Planes + 4, plane1);
    r.cpu().a[1] = kRastPort;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::InitRastPort), ExitReason::Halt);
    r.put32(kRastPort + rp_BitMap, kBitmap);
    return kRastPort;
}

u32 read_pixel(GfxRig& r, u32 rp, i32 x, i32 y) {
    r.cpu().a[1] = rp;
    r.cpu().d[0] = static_cast<u32>(x);
    r.cpu().d[1] = static_cast<u32>(y);
    return r.result(Base::Graphics, graphics_lvo::ReadPixel);
}

void new_menu(GfxRig& r, u32 at, u8 type, u32 label, u32 key, u16 flags, u32 user) {
    r.put8(at + nm_Type, type);
    r.put32(at + nm_Label, label);
    r.put32(at + nm_CommKey, key);
    r.put16(at + nm_Flags, flags);
    r.put32(at + nm_UserData, user);
}

void put_new_menus(GfxRig& r) {
    r.str(kFileLabel, "File");
    r.str(kEditLabel, "Edit");
    r.str(kOpenLabel, "Open");
    r.str(kRecentLabel, "Recent");
    r.str(kWrapLabel, "Wrap");
    r.str(kQuitLabel, "Quit");
    r.str(kPasteLabel, "Paste");
    r.str(kOpenKey, "O");
    new_menu(r, kNewMenus + 0 * NewMenu_SIZEOF, NM_TITLE, kFileLabel, 0, 0, 0x11111111);
    new_menu(r, kNewMenus + 1 * NewMenu_SIZEOF, NM_ITEM, kOpenLabel, kOpenKey, 0, 0xA1);
    new_menu(r, kNewMenus + 2 * NewMenu_SIZEOF, NM_SUB, kRecentLabel, 0, 0, 0xA2);
    new_menu(r, kNewMenus + 3 * NewMenu_SIZEOF, NM_ITEM, NM_BARLABEL, 0, 0, 0xA3);
    new_menu(r, kNewMenus + 4 * NewMenu_SIZEOF, NM_ITEM, kWrapLabel, 0, CHECKIT | CHECKED, 0xA4);
    new_menu(r, kNewMenus + 5 * NewMenu_SIZEOF, NM_ITEM, kQuitLabel, 0, NM_ITEMDISABLED, 0xA5);
    new_menu(r, kNewMenus + 6 * NewMenu_SIZEOF, NM_TITLE, kEditLabel, 0, 0, 0x22222222);
    new_menu(r, kNewMenus + 7 * NewMenu_SIZEOF, NM_ITEM, kPasteLabel, 0, 0, 0xA7);
    new_menu(r, kNewMenus + 8 * NewMenu_SIZEOF, NM_END, 0, 0, 0, 0);
}

u32 create_menu_strip(GfxRig& r, GadTools& g) {
    put_new_menus(r);
    r.cpu().a[0] = kNewMenus;
    return gt_result(r, g, gadtools_lvo::CreateMenusA);
}

void visual_info_holds_the_screen_and_is_freed() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);

    r.cpu().a[0] = 0;
    CHECK_EQ(gt_result(r, g, gadtools_lvo::GetVisualInfoA), 0u);

    put_new_screen(r, kNewScreen, 640, 400, 2, 0);
    const u32 screen = open_new_screen(r, kNewScreen);
    CHECK(screen != 0);
    const u32 before = avail_mem(r);
    r.cpu().a[0] = screen;
    const u32 vi = gt_result(r, g, gadtools_lvo::GetVisualInfoA);
    CHECK(vi != 0);
    CHECK_EQ(r.get32(vi), screen);
    CHECK_EQ(r.get32(vi + 4), r.get32(screen + sc_Font));
    CHECK(avail_mem(r) < before);

    r.cpu().a[0] = vi;
    CHECK_EQ(gt_call(r, g, gadtools_lvo::FreeVisualInfo), ExitReason::Halt);
    CHECK_EQ(avail_mem(r), before);
}

void create_context_stores_and_frees_the_gadget() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);

    r.put32(kList, 0);
    const u32 before = avail_mem(r);
    r.cpu().a[0] = kList;
    const u32 gadget = gt_result(r, g, gadtools_lvo::CreateContext);
    CHECK(gadget != 0);
    CHECK_EQ(r.get32(kList), gadget);
    CHECK_EQ(r.get16(gadget + gg_Flags), kGadgHNone);
    r.cpu().a[0] = gadget;
    CHECK_EQ(gt_call(r, g, gadtools_lvo::FreeGadgets), ExitReason::Halt);
    CHECK_EQ(avail_mem(r), before);
}

void create_menus_builds_the_chain_and_flags() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);

    const u32 file = create_menu_strip(r, g);
    CHECK(file != 0);
    const u32 edit = r.get32(file + mu_NextMenu);
    CHECK(edit != 0);
    CHECK_EQ(r.get32(edit + mu_NextMenu), 0u);
    CHECK_EQ(r.get32(file + mu_MenuName), kFileLabel);
    CHECK_EQ(r.get32(edit + mu_MenuName), kEditLabel);
    CHECK_EQ(r.get16(file + mu_Flags), MENUENABLED);
    CHECK_EQ(r.get16(edit + mu_Flags), MENUENABLED);
    CHECK_EQ(r.get32(file + Menu_SIZEOF), 0x11111111u);
    CHECK_EQ(r.get32(edit + Menu_SIZEOF), 0x22222222u);

    const u32 open = r.get32(file + mu_FirstItem);
    const u32 bar = r.get32(open + mi_NextItem);
    const u32 wrap = r.get32(bar + mi_NextItem);
    const u32 quit = r.get32(wrap + mi_NextItem);
    const u32 paste = r.get32(edit + mu_FirstItem);
    CHECK(open != 0 && bar != 0 && wrap != 0 && quit != 0 && paste != 0);
    CHECK_EQ(r.get32(quit + mi_NextItem), 0u);
    CHECK_EQ(r.get32(paste + mi_NextItem), 0u);

    const u32 recent = r.get32(open + mi_SubItem);
    CHECK(recent != 0);
    CHECK_EQ(r.get32(recent + mi_NextItem), 0u);
    CHECK_EQ(r.get32(recent + mi_SubItem), 0u);

    CHECK_EQ(r.get16(open + mi_Flags), ITEMTEXT | HIGHCOMP | ITEMENABLED | COMMSEQ);
    CHECK_EQ(r.get8(open + mi_Command), static_cast<u32>('O'));
    CHECK_EQ(r.get16(recent + mi_Flags), ITEMTEXT | HIGHCOMP | ITEMENABLED);
    CHECK_EQ(r.get16(wrap + mi_Flags), ITEMTEXT | HIGHCOMP | ITEMENABLED | CHECKIT | CHECKED);
    CHECK_EQ(r.get16(quit + mi_Flags), ITEMTEXT | HIGHCOMP);
    CHECK_EQ(r.get16(bar + mi_Flags), HIGHCOMP);
    CHECK_EQ(r.get16(paste + mi_Flags), ITEMTEXT | HIGHCOMP | ITEMENABLED);

    CHECK_EQ(r.get32(r.get32(open + mi_ItemFill) + it_IText), kOpenLabel);
    CHECK_EQ(r.get32(r.get32(recent + mi_ItemFill) + it_IText), kRecentLabel);
    CHECK_EQ(r.get32(r.get32(wrap + mi_ItemFill) + it_IText), kWrapLabel);
    CHECK_EQ(r.get32(r.get32(quit + mi_ItemFill) + it_IText), kQuitLabel);
    CHECK_EQ(r.get32(r.get32(paste + mi_ItemFill) + it_IText), kPasteLabel);

    CHECK_EQ(r.get32(open + MenuItem_SIZEOF), 0xA1u);
    CHECK_EQ(r.get32(recent + MenuItem_SIZEOF), 0xA2u);
    CHECK_EQ(r.get32(bar + MenuItem_SIZEOF), 0xA3u);
    CHECK_EQ(r.get32(wrap + MenuItem_SIZEOF), 0xA4u);
    CHECK_EQ(r.get32(quit + MenuItem_SIZEOF), 0xA5u);
    CHECK_EQ(r.get32(paste + MenuItem_SIZEOF), 0xA7u);

    r.put8(kNewMenus, NM_END);
    r.cpu().a[0] = kNewMenus;
    CHECK_EQ(gt_result(r, g, gadtools_lvo::CreateMenusA), 0u);
    r.cpu().a[0] = 0;
    CHECK_EQ(gt_result(r, g, gadtools_lvo::CreateMenusA), 0u);
}

void layout_menus_places_titles_and_items() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);

    const u32 file = create_menu_strip(r, g);
    CHECK(file != 0);
    const u32 edit = r.get32(file + mu_NextMenu);
    const u32 open = r.get32(file + mu_FirstItem);
    const u32 bar = r.get32(open + mi_NextItem);
    const u32 wrap = r.get32(bar + mi_NextItem);
    const u32 quit = r.get32(wrap + mi_NextItem);
    const u32 paste = r.get32(edit + mu_FirstItem);

    r.cpu().a[0] = file;
    CHECK_EQ(gt_result(r, g, gadtools_lvo::LayoutMenusA), 1u);

    CHECK_EQ(r.get16(file + mu_LeftEdge), 0u);
    CHECK_EQ(r.get16(file + mu_TopEdge), 0u);
    CHECK_EQ(r.get16(file + mu_Width), 48u);
    CHECK_EQ(r.get16(file + mu_Height), 9u);
    CHECK_EQ(r.get16(edit + mu_LeftEdge), 48u);
    CHECK_EQ(r.get16(edit + mu_TopEdge), 0u);
    CHECK_EQ(r.get16(edit + mu_Width), 48u);
    CHECK_EQ(r.get16(edit + mu_Height), 9u);

    CHECK_EQ(r.get16(open + mi_LeftEdge), 0u);
    CHECK_EQ(r.get16(open + mi_TopEdge), 11u);
    CHECK_EQ(r.get16(open + mi_Width), 68u);
    CHECK_EQ(r.get16(open + mi_Height), 9u);
    CHECK_EQ(r.get16(bar + mi_LeftEdge), 0u);
    CHECK_EQ(r.get16(bar + mi_TopEdge), 20u);
    CHECK_EQ(r.get16(bar + mi_Width), 68u);
    CHECK_EQ(r.get16(bar + mi_Height), 4u);
    CHECK_EQ(r.get16(wrap + mi_TopEdge), 24u);
    CHECK_EQ(r.get16(wrap + mi_Width), 68u);
    CHECK_EQ(r.get16(wrap + mi_Height), 9u);
    CHECK_EQ(r.get16(quit + mi_TopEdge), 33u);
    CHECK_EQ(r.get16(quit + mi_Width), 68u);
    CHECK_EQ(r.get16(quit + mi_Height), 9u);
    CHECK_EQ(r.get16(paste + mi_LeftEdge), 0u);
    CHECK_EQ(r.get16(paste + mi_TopEdge), 11u);
    CHECK_EQ(r.get16(paste + mi_Width), 44u);
    CHECK_EQ(r.get16(paste + mi_Height), 9u);
}

void free_menus_returns_memory_and_rejects_foreign_strips() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);

    put_new_menus(r);
    const u32 before = avail_mem(r);
    r.cpu().a[0] = kNewMenus;
    const u32 file = gt_result(r, g, gadtools_lvo::CreateMenusA);
    CHECK(file != 0);
    CHECK(avail_mem(r) < before);

    r.cpu().a[0] = file;
    CHECK_EQ(gt_call(r, g, gadtools_lvo::FreeMenus), ExitReason::Halt);
    CHECK_EQ(avail_mem(r), before);

    r.cpu().a[0] = kFakeMenu;
    CHECK_EQ(gt_call(r, g, gadtools_lvo::FreeMenus), ExitReason::Halt);
    CHECK(logged("gadtools: FreeMenus of a strip it did not make"));
}

void imsg_vectors_go_through_the_port() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);

    const u32 port = r.exec.create_port();
    const u32 reply_port = r.exec.create_port();
    CHECK(port != 0);
    CHECK(reply_port != 0);
    r.put32(kMessage + mn_ReplyPort, reply_port);
    CHECK(r.exec.post_message(port, kMessage));

    r.cpu().a[0] = port;
    CHECK_EQ(gt_result(r, g, gadtools_lvo::GT_GetIMsg), kMessage);
    CHECK_EQ(r.exec.get_message(port), 0u);

    r.cpu().a[1] = kMessage;
    CHECK_EQ(gt_call(r, g, gadtools_lvo::GT_ReplyIMsg), ExitReason::Halt);
    CHECK_EQ(r.exec.get_message(reply_port), kMessage);
}

void filter_vectors_return_a1() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);

    r.cpu().a[1] = 0x12345678;
    CHECK_EQ(gt_result(r, g, gadtools_lvo::GT_FilterIMsg), 0x12345678u);
    r.cpu().a[1] = 0;
    CHECK_EQ(gt_result(r, g, gadtools_lvo::GT_FilterIMsg), 0u);
    r.cpu().a[1] = 0x9ABCDEF0;
    CHECK_EQ(gt_result(r, g, gadtools_lvo::GT_PostFilterIMsg), 0x9ABCDEF0u);
}

void draw_bevel_box_uses_shine_and_shadow_pens() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);
    const u32 rp = make_surface(r);

    r.cpu().a[0] = rp;
    r.cpu().a[1] = 0;
    r.cpu().d[0] = 20;
    r.cpu().d[1] = 20;
    r.cpu().d[2] = 40;
    r.cpu().d[3] = 30;
    CHECK_EQ(gt_call(r, g, gadtools_lvo::DrawBevelBoxA), ExitReason::Halt);

    CHECK_EQ(read_pixel(r, rp, 30, 20), kShinePen);
    CHECK_EQ(read_pixel(r, rp, 20, 30), kShinePen);
    CHECK_EQ(read_pixel(r, rp, 59, 20), kShinePen);
    CHECK_EQ(read_pixel(r, rp, 20, 49), kShinePen);
    CHECK_EQ(read_pixel(r, rp, 30, 49), kShadowPen);
    CHECK_EQ(read_pixel(r, rp, 59, 30), kShadowPen);
    CHECK_EQ(read_pixel(r, rp, 59, 49), kShadowPen);
    CHECK_EQ(read_pixel(r, rp, 30, 30), 0u);
}

void draw_bevel_box_recessed_swaps_the_pens() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);
    const u32 rp = make_surface(r);
    const u32 tags = r.tags(kTags, {{GTBB_Recessed, 1}, {TAG_DONE, 0}});

    r.cpu().a[0] = rp;
    r.cpu().a[1] = tags;
    r.cpu().d[0] = 20;
    r.cpu().d[1] = 20;
    r.cpu().d[2] = 40;
    r.cpu().d[3] = 30;
    CHECK_EQ(gt_call(r, g, gadtools_lvo::DrawBevelBoxA), ExitReason::Halt);

    CHECK_EQ(read_pixel(r, rp, 30, 20), kShadowPen);
    CHECK_EQ(read_pixel(r, rp, 20, 30), kShadowPen);
    CHECK_EQ(read_pixel(r, rp, 30, 49), kShinePen);
    CHECK_EQ(read_pixel(r, rp, 59, 30), kShinePen);
    CHECK_EQ(read_pixel(r, rp, 30, 30), 0u);
}

}

int main() {
    visual_info_holds_the_screen_and_is_freed();
    create_context_stores_and_frees_the_gadget();
    create_menus_builds_the_chain_and_flags();
    layout_menus_places_titles_and_items();
    free_menus_returns_memory_and_rejects_foreign_strips();
    imsg_vectors_go_through_the_port();
    filter_vectors_return_a1();
    draw_bevel_box_uses_shine_and_shadow_pens();
    draw_bevel_box_recessed_swaps_the_pens();
    return check::finish("test_gadtools");
}