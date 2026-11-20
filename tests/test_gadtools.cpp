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
constexpr u32 kNewGadget = kData + 0xB00;
constexpr u32 kLabel = kData + 0xB40;
constexpr u32 kText = kData + 0xB60;
constexpr u32 kAnswer = kData + 0xB80;

// struct NewGadget, the gadget kinds and their tags.
constexpr u32 ng_LeftEdge = 0;
constexpr u32 ng_TopEdge = 2;
constexpr u32 ng_Width = 4;
constexpr u32 ng_Height = 6;
constexpr u32 ng_GadgetText = 8;
constexpr u32 ng_GadgetID = 16;
constexpr u32 ng_Flags = 18;
constexpr u32 ng_UserData = 26;
constexpr u32 BUTTON_KIND = 1;
constexpr u32 NUMBER_KIND = 6;
constexpr u32 TEXT_KIND = 13;
constexpr u32 CYCLE_KIND = 7;
constexpr u32 PALETTE_KIND = 8;
constexpr u32 GTCY_Labels = 0x8008000E;
constexpr u32 GTCY_Active = 0x8008000F;
constexpr u32 GTPA_Depth = 0x80080010;
constexpr u32 GTPA_Color = 0x80080011;
constexpr u32 GTPA_ColorOffset = 0x80080012;
constexpr u32 GTPA_NumColors = 0x80080046;
constexpr u32 SCROLLER_KIND = 9;
constexpr u32 STRING_KIND = 12;
constexpr u32 GTSC_Top = 0x80080015;
constexpr u32 GTSC_Total = 0x80080016;
constexpr u32 GTSC_Visible = 0x80080017;
constexpr u32 GTST_String = 0x8008002D;
constexpr u32 GA_Disabled = 0x8003000E;
constexpr u32 GTTX_Text = 0x8008000B;
constexpr u32 GTTX_CopyText = 0x8008000C;
constexpr u32 GTNM_Number = 0x8008000D;
constexpr u32 GTTX_Border = 0x80080039;
constexpr u32 kFillPen = 3;

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
    CHECK_EQ(r.get16(file + mu_Height), 10u);
    CHECK_EQ(r.get16(edit + mu_LeftEdge), 48u);
    CHECK_EQ(r.get16(edit + mu_TopEdge), 0u);
    CHECK_EQ(r.get16(edit + mu_Width), 48u);
    CHECK_EQ(r.get16(edit + mu_Height), 10u);

    // Open: four characters and eight columns of padding, 32 for the
    // shortcut and 16 for the submenu arrow; Wrap's check mark gives the
    // whole column 16 more.
    CHECK_EQ(r.get16(open + mi_LeftEdge), 0u);
    CHECK_EQ(r.get16(open + mi_TopEdge), 0u);
    CHECK_EQ(r.get16(open + mi_Width), 104u);
    CHECK_EQ(r.get16(open + mi_Height), 10u);
    CHECK_EQ(r.get16(bar + mi_LeftEdge), 0u);
    CHECK_EQ(r.get16(bar + mi_TopEdge), 10u);
    CHECK_EQ(r.get16(bar + mi_Width), 104u);
    CHECK_EQ(r.get16(bar + mi_Height), 6u);
    CHECK_EQ(r.get16(wrap + mi_TopEdge), 16u);
    CHECK_EQ(r.get16(wrap + mi_Width), 104u);
    CHECK_EQ(r.get16(wrap + mi_Height), 10u);
    CHECK_EQ(r.get16(quit + mi_TopEdge), 26u);
    CHECK_EQ(r.get16(quit + mi_Width), 104u);
    CHECK_EQ(r.get16(quit + mi_Height), 10u);
    const u32 recent = r.get32(open + mi_SubItem);
    CHECK_EQ(r.get16(recent + mi_TopEdge), 0u);
    CHECK_EQ(r.get16(recent + mi_Width), 56u);
    CHECK_EQ(r.get16(recent + mi_Height), 10u);
    CHECK_EQ(r.get16(paste + mi_LeftEdge), 0u);
    CHECK_EQ(r.get16(paste + mi_TopEdge), 0u);
    CHECK_EQ(r.get16(paste + mi_Width), 48u);
    CHECK_EQ(r.get16(paste + mi_Height), 10u);
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

void draw_bevel_box_leaves_the_rastport_as_it_was() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);
    const u32 rp = make_surface(r);
    r.put8(rp + rp_FgPen, 3);
    r.put8(rp + rp_BgPen, 2);
    r.put8(rp + rp_DrawMode, JAM2);
    r.put16(rp + rp_cp_x, 5);
    r.put16(rp + rp_cp_y, 6);

    r.cpu().a[0] = rp;
    r.cpu().a[1] = r.tags(kTags, {{GTBB_Recessed, 1}, {TAG_DONE, 0}});
    r.cpu().d[0] = 20;
    r.cpu().d[1] = 20;
    r.cpu().d[2] = 40;
    r.cpu().d[3] = 30;
    CHECK_EQ(gt_call(r, g, gadtools_lvo::DrawBevelBoxA), ExitReason::Halt);
    CHECK_EQ(r.get8(rp + rp_FgPen), 3u);
    CHECK_EQ(r.get8(rp + rp_BgPen), 2u);
    CHECK_EQ(r.get8(rp + rp_DrawMode), JAM2);
    CHECK_EQ(r.get16(rp + rp_cp_x), 5u);
    CHECK_EQ(r.get16(rp + rp_cp_y), 6u);
}

void put_new_gadget(GfxRig& r, i32 left, i32 top, u32 width, u32 height, u32 label, u32 id,
                    u32 flags, u32 user) {
    r.put16(kNewGadget + ng_LeftEdge, static_cast<u32>(left));
    r.put16(kNewGadget + ng_TopEdge, static_cast<u32>(top));
    r.put16(kNewGadget + ng_Width, width);
    r.put16(kNewGadget + ng_Height, height);
    r.put32(kNewGadget + ng_GadgetText, label);
    r.put16(kNewGadget + ng_GadgetID, id);
    r.put32(kNewGadget + ng_Flags, flags);
    r.put32(kNewGadget + ng_UserData, user);
}

u32 create_gadget(GfxRig& r, GadTools& g, u32 kind, u32 previous, u32 tags) {
    r.cpu().d[0] = kind;
    r.cpu().a[0] = previous;
    r.cpu().a[1] = kNewGadget;
    r.cpu().a[2] = tags;
    return gt_result(r, g, gadtools_lvo::CreateGadgetA);
}

void create_gadget_makes_buttons_and_displays() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);
    const u32 before = avail_mem(r);
    r.cpu().a[0] = kList;
    const u32 context = gt_result(r, g, gadtools_lvo::CreateContext);
    CHECK(context != 0);
    CHECK_EQ(r.get16(context + gg_GadgetType), GTYP_GADTOOLS | GTYP_BOOLGADGET);

    // A button reports when it is let go.
    r.str(kLabel, "_Ok");
    put_new_gadget(r, 10, 20, 60, 14, kLabel, 7, 0, 0x1234);
    const u32 button = create_gadget(r, g, BUTTON_KIND, context, 0);
    CHECK(button != 0);
    CHECK_EQ(r.get32(context + gg_NextGadget), button);
    CHECK_EQ(r.get16(button + gg_LeftEdge), 10u);
    CHECK_EQ(r.get16(button + gg_TopEdge), 20u);
    CHECK_EQ(r.get16(button + gg_Width), 60u);
    CHECK_EQ(r.get16(button + gg_Height), 14u);
    CHECK_EQ(r.get16(button + gg_GadgetType), GTYP_GADTOOLS | GTYP_BOOLGADGET);
    CHECK_EQ(r.get16(button + gg_Activation), GACT_RELVERIFY);
    CHECK_EQ(r.get16(button + gg_GadgetID), 7u);
    CHECK_EQ(r.get32(button + gg_UserData), 0x1234u);

    // A text display keeps a copy of its text; a disabled number display
    // reports its number.
    r.str(kText, "hello");
    put_new_gadget(r, 10, 40, 100, 12, 0, 8, 0, 0);
    const u32 text = create_gadget(
        r, g, TEXT_KIND, button,
        r.tags(kTags, {{GTTX_Text, kText}, {GTTX_CopyText, 1}, {GTTX_Border, 1}, {TAG_DONE, 0}}));
    CHECK(text != 0);
    CHECK_EQ(r.get16(text + gg_Activation), 0u);
    r.str(kText, "XXXXX");
    r.cpu().a[0] = text;
    r.cpu().a[3] = r.tags(kTags, {{GTTX_Text, kAnswer}, {TAG_DONE, 0}});
    CHECK_EQ(gt_result(r, g, gadtools_lvo::GT_GetGadgetAttrsA), 1u);
    CHECK(r.read_str(r.get32(kAnswer)) == "hello");

    put_new_gadget(r, 10, 60, 100, 12, 0, 9, 0, 0);
    const u32 number_tags =
        r.tags(kTags, {{GTNM_Number, 42}, {GA_Disabled, 1}, {TAG_DONE, 0}});
    const u32 number = create_gadget(r, g, NUMBER_KIND, text, number_tags);
    CHECK(number != 0);
    CHECK_EQ(r.get16(number + gg_Flags) & GFLG_DISABLED, GFLG_DISABLED);
    r.cpu().a[0] = number;
    r.cpu().a[1] = 0;
    r.cpu().a[3] = r.tags(kTags, {{GTNM_Number, 7}, {GA_Disabled, 0}, {TAG_DONE, 0}});
    CHECK_EQ(gt_call(r, g, gadtools_lvo::GT_SetGadgetAttrsA), ExitReason::Halt);
    CHECK_EQ(r.get16(number + gg_Flags) & GFLG_DISABLED, 0u);
    r.cpu().a[0] = number;
    r.cpu().a[3] = r.tags(kTags, {{GTNM_Number, kAnswer}, {TAG_DONE, 0}});
    CHECK_EQ(gt_result(r, g, gadtools_lvo::GT_GetGadgetAttrsA), 1u);
    CHECK_EQ(r.get32(kAnswer), 7u);

    // No previous gadget, no gadget.
    CHECK_EQ(create_gadget(r, g, BUTTON_KIND, 0, 0), 0u);

    r.cpu().a[0] = context;
    CHECK_EQ(gt_call(r, g, gadtools_lvo::FreeGadgets), ExitReason::Halt);
    CHECK_EQ(avail_mem(r), before);
}

void a_button_draws_raised_and_pressed() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);
    const u32 rp = make_surface(r);
    r.cpu().a[0] = kList;
    const u32 context = gt_result(r, g, gadtools_lvo::CreateContext);
    r.str(kLabel, "Go");
    put_new_gadget(r, 20, 20, 40, 14, kLabel, 1, 0, 0);
    const u32 button = create_gadget(r, g, BUTTON_KIND, context, 0);
    r.put8(rp + rp_FgPen, 2);
    r.put8(rp + rp_DrawMode, JAM2);

    g.render(0, rp, button);
    CHECK_EQ(read_pixel(r, rp, 30, 20), kShinePen);
    CHECK_EQ(read_pixel(r, rp, 20, 30), kShinePen);
    CHECK_EQ(read_pixel(r, rp, 30, 33), kShadowPen);
    CHECK_EQ(read_pixel(r, rp, 59, 30), kShadowPen);
    u32 label_pixels = 0;
    for (i32 y = 21; y < 33; ++y) {
        for (i32 x = 21; x < 59; ++x) {
            label_pixels += read_pixel(r, rp, x, y) == 1 ? 1 : 0;
        }
    }
    CHECK(label_pixels > 0);

    // Pressed: recessed and filled.
    r.put16(button + gg_Flags, r.get16(button + gg_Flags) | GFLG_SELECTED);
    g.render(0, rp, button);
    CHECK_EQ(read_pixel(r, rp, 30, 20), kShadowPen);
    CHECK_EQ(read_pixel(r, rp, 30, 33), kShinePen);
    CHECK_EQ(read_pixel(r, rp, 22, 22), kFillPen);

    // The program's pens and draw mode are untouched.
    CHECK_EQ(r.get8(rp + rp_FgPen), 2u);
    CHECK_EQ(r.get8(rp + rp_DrawMode), JAM2);
    r.cpu().a[0] = context;
    CHECK_EQ(gt_call(r, g, gadtools_lvo::FreeGadgets), ExitReason::Halt);
}

}

void cycles_step_through_their_labels_and_palettes_pick_colours() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);
    r.cpu().a[0] = kList;
    const u32 context = gt_result(r, g, gadtools_lvo::CreateContext);

    // A cycle gadget of three labels, the second active.
    const u32 labels = kText;
    r.str(kText + 0x10, "One");
    r.str(kText + 0x18, "Two");
    r.str(kText + 0x20, "Three");
    r.put32(labels, kText + 0x10);
    r.put32(labels + 4, kText + 0x18);
    r.put32(labels + 8, kText + 0x20);
    r.put32(labels + 12, 0);
    put_new_gadget(r, 10, 20, 120, 14, 0, 3, 0, 0);
    const u32 cycle = create_gadget(
        r, g, CYCLE_KIND, context,
        r.tags(kTags, {{GTCY_Labels, labels}, {GTCY_Active, 1}, {TAG_DONE, 0}}));
    CHECK(cycle != 0);
    CHECK_EQ(r.get16(cycle + gg_Activation), GACT_RELVERIFY);
    u16 code = 0;
    CHECK(g.release(0, cycle, 5, 5, code));
    CHECK_EQ(code, 2u);
    CHECK(g.release(0, cycle, 5, 5, code));
    CHECK_EQ(code, 0u);  // round to the first again
    r.cpu().a[0] = cycle;
    r.cpu().a[3] = r.tags(kTags, {{GTCY_Active, kAnswer}, {TAG_DONE, 0}});
    CHECK_EQ(gt_result(r, g, gadtools_lvo::GT_GetGadgetAttrsA), 1u);
    CHECK_EQ(r.get32(kAnswer), 0u);

    // A palette of four colours from pen 4 in a 40 by 10 box: four cells
    // in a row, each 9 by 6 inside the frame.
    put_new_gadget(r, 10, 40, 40, 10, 0, 4, 0, 0);
    const u32 palette = create_gadget(
        r, g, PALETTE_KIND, cycle,
        r.tags(kTags, {{GTPA_Depth, 2}, {GTPA_ColorOffset, 4}, {GTPA_Color, 5},
                       {TAG_DONE, 0}}));
    CHECK(palette != 0);
    CHECK_EQ(r.get16(palette + gg_Activation), GACT_RELVERIFY);
    CHECK(g.release(0, palette, 30, 5, code));
    CHECK_EQ(code, 7u);
    CHECK(g.release(0, palette, 3, 5, code));
    CHECK_EQ(code, 4u);
    r.cpu().a[0] = palette;
    r.cpu().a[3] = r.tags(kTags, {{GTPA_Color, kAnswer}, {TAG_DONE, 0}});
    CHECK_EQ(gt_result(r, g, gadtools_lvo::GT_GetGadgetAttrsA), 1u);
    CHECK_EQ(r.get32(kAnswer), 4u);
    // GTPA_NumColors counts the colours itself.
    r.cpu().a[0] = palette;
    r.cpu().a[1] = 0;
    r.cpu().a[3] = r.tags(kTags, {{GTPA_NumColors, 2}, {TAG_DONE, 0}});
    gt_result(r, g, gadtools_lvo::GT_SetGadgetAttrsA);
    CHECK(g.release(0, palette, 30, 5, code));
    CHECK_EQ(code, 5u);
    // A button lets go with code 0; a generic gadget is its program's.
    put_new_gadget(r, 10, 60, 40, 10, 0, 5, 0, 0);
    const u32 generic = create_gadget(r, g, 0, palette, 0);
    CHECK(!g.release(0, generic, 1, 1, code));
    CHECK(!g.render(0, kRastPort, generic));

    // FreeGadgets leaves a gadget of the program's own in the chain to it.
    const u32 own = r.exec.alloc(Gadget_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    r.put16(own + gg_GadgetType, GTYP_BOOLGADGET);
    r.put32(generic + gg_NextGadget, own);
    r.put16(own + gg_Flags, 0x1234);
    r.cpu().a[0] = context;
    gt_result(r, g, gadtools_lvo::FreeGadgets);
    CHECK_EQ(r.get16(own + gg_Flags), 0x1234u);
    CHECK(!logged("exec: FreeMem of a bad block"));
    r.exec.free(own, Gadget_SIZEOF);
}

void strings_keep_a_buffer_and_scrollers_their_place() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);
    const u32 before = avail_mem(r);
    r.cpu().a[0] = kList;
    const u32 context = gt_result(r, g, gadtools_lvo::CreateContext);
    r.str(kText, "Work:");
    put_new_gadget(r, 10, 20, 120, 14, 0, 1, 0, 0);
    const u32 string = create_gadget(r, g, STRING_KIND, context,
                                     r.tags(kTags, {{GTST_String, kText}, {TAG_DONE, 0}}));
    CHECK(string != 0);
    r.str(kText, "XXXXX");
    r.cpu().a[0] = string;
    r.cpu().a[3] = r.tags(kTags, {{GTST_String, kAnswer}, {TAG_DONE, 0}});
    CHECK_EQ(gt_result(r, g, gadtools_lvo::GT_GetGadgetAttrsA), 1u);
    CHECK(r.read_str(r.get32(kAnswer)) == "Work:");
    // An Intuition string gadget, its StringInfo over that buffer.
    CHECK_EQ(r.get16(string + gg_GadgetType), GTYP_GADTOOLS | GTYP_STRGADGET);
    const u32 si = r.get32(string + gg_SpecialInfo);
    CHECK(si != 0);
    CHECK_EQ(r.get32(si + si_Buffer), r.get32(kAnswer));
    CHECK_EQ(r.get16(si + si_NumChars), 5u);
    CHECK(r.get16(si + si_MaxChars) >= 64u);
    // An empty string gadget still has a buffer to read.
    put_new_gadget(r, 10, 40, 120, 14, 0, 2, 0, 0);
    const u32 empty = create_gadget(r, g, STRING_KIND, string, 0);
    r.cpu().a[0] = empty;
    r.cpu().a[3] = r.tags(kTags, {{GTST_String, kAnswer}, {TAG_DONE, 0}});
    gt_result(r, g, gadtools_lvo::GT_GetGadgetAttrsA);
    CHECK(r.get32(kAnswer) != 0);
    CHECK(r.read_str(r.get32(kAnswer)).empty());

    put_new_gadget(r, 10, 60, 16, 80, 0, 3, 0, 0);
    const u32 scroller = create_gadget(
        r, g, SCROLLER_KIND, empty,
        r.tags(kTags, {{GTSC_Top, 5}, {GTSC_Total, 100}, {GTSC_Visible, 10}, {TAG_DONE, 0}}));
    CHECK(scroller != 0);
    r.cpu().a[0] = scroller;
    r.cpu().a[3] = r.tags(kTags, {{GTSC_Top, kAnswer}, {GTSC_Total, kAnswer + 4},
                                  {GTSC_Visible, kAnswer + 8}, {TAG_DONE, 0}});
    CHECK_EQ(gt_result(r, g, gadtools_lvo::GT_GetGadgetAttrsA), 3u);
    CHECK_EQ(r.get32(kAnswer), 5u);
    CHECK_EQ(r.get32(kAnswer + 4), 100u);
    CHECK_EQ(r.get32(kAnswer + 8), 10u);
    // An Intuition prop gadget across, a tenth of it showing, the top 5 of
    // 90 along; a knob moved to its end tops at 90.
    CHECK_EQ(r.get16(scroller + gg_GadgetType), GTYP_GADTOOLS | GTYP_PROPGADGET);
    const u32 pi = r.get32(scroller + gg_SpecialInfo);
    CHECK(pi != 0);
    CHECK_EQ(r.get16(pi + pi_Flags) & FREEHORIZ, FREEHORIZ);
    CHECK_EQ(r.get16(pi + pi_HorizBody), 10u * MAXBODY / 100);
    CHECK_EQ(r.get16(pi + pi_HorizPot), 5u * MAXPOT / 90);
    r.put16(pi + pi_HorizPot, MAXPOT);
    CHECK_EQ(g.prop_code(scroller), 90u);
    r.cpu().a[0] = scroller;
    r.cpu().a[3] = r.tags(kTags, {{GTSC_Top, kAnswer}, {TAG_DONE, 0}});
    gt_result(r, g, gadtools_lvo::GT_GetGadgetAttrsA);
    CHECK_EQ(r.get32(kAnswer), 90u);
    CHECK(!logged("gadtools: gadget kind 12 not handled"));
    CHECK(!logged("gadtools: gadget kind 9 not handled"));

    r.cpu().a[0] = context;
    gt_result(r, g, gadtools_lvo::FreeGadgets);
    CHECK_EQ(avail_mem(r), before);
}

// A slider is a prop gadget over its levels, and its messages carry the
// level; a list view shows its nodes and picks the row clicked.
void sliders_carry_levels_and_list_views_pick_rows() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);
    constexpr u32 kSliderKind = 11;
    constexpr u32 kListViewKind = 4;
    constexpr u32 GTSL_Min = 0x80080026;
    constexpr u32 GTSL_Max = 0x80080027;
    constexpr u32 GTSL_Level = 0x80080028;
    constexpr u32 GTLV_Labels = 0x80080006;
    constexpr u32 GTLV_Selected = 0x80080036;
    r.cpu().a[0] = kList;
    const u32 context = gt_result(r, g, gadtools_lvo::CreateContext);
    put_new_gadget(r, 10, 10, 120, 10, 0, 1, 0, 0);
    const u32 slider = create_gadget(
        r, g, kSliderKind, context,
        r.tags(kTags, {{GTSL_Min, 10}, {GTSL_Max, 19}, {GTSL_Level, 15}, {TAG_DONE, 0}}));
    CHECK(slider != 0);
    CHECK_EQ(r.get16(slider + gg_GadgetType), GTYP_GADTOOLS | GTYP_PROPGADGET);
    const u32 pi = r.get32(slider + gg_SpecialInfo);
    CHECK_EQ(r.get16(pi + pi_HorizBody), MAXBODY / 10);
    CHECK_EQ(r.get16(pi + pi_HorizPot), 5u * MAXPOT / 9);
    r.put16(pi + pi_HorizPot, MAXPOT);
    CHECK_EQ(g.prop_code(slider), 19u);
    r.put16(pi + pi_HorizPot, 0);
    CHECK_EQ(g.prop_code(slider), 10u);
    r.cpu().a[0] = slider;
    r.cpu().a[3] = r.tags(kTags, {{GTSL_Level, kAnswer}, {TAG_DONE, 0}});
    CHECK_EQ(gt_result(r, g, gadtools_lvo::GT_GetGadgetAttrsA), 1u);
    CHECK_EQ(r.get32(kAnswer), 10u);

    // struct List with three nodes.
    constexpr u32 kHead = kData + 0x1000;
    const u32 nodes[3] = {kData + 0x1040, kData + 0x1080, kData + 0x10C0};
    const char* names[3] = {"One", "Two", "Three"};
    r.put32(kHead, nodes[0]);
    r.put32(kHead + 4, 0);
    r.put32(kHead + 8, nodes[2]);
    for (u32 i = 0; i < 3; ++i) {
        r.put32(nodes[i], i + 1 < 3 ? nodes[i + 1] : kHead + 4);
        r.put32(nodes[i] + 4, i > 0 ? nodes[i - 1] : kHead);
        r.put32(nodes[i] + 10, r.str(nodes[i] + 0x20, names[i]));
    }
    put_new_gadget(r, 10, 30, 120, 40, 0, 2, 0, 0);
    const u32 list = create_gadget(r, g, kListViewKind, slider,
                                   r.tags(kTags, {{GTLV_Labels, kHead}, {TAG_DONE, 0}}));
    CHECK(list != 0);
    CHECK_EQ(r.get16(list + gg_Activation) & GACT_RELVERIFY, GACT_RELVERIFY);
    u16 code = 0;
    // Rows are 8 pixels high under a 2-pixel frame: y 11 is the second row.
    CHECK(g.release(0, list, 20, 11, code));
    CHECK_EQ(code, 1u);
    CHECK(!g.release(0, list, 20, 30, code));
    r.cpu().a[0] = list;
    r.cpu().a[3] = r.tags(kTags, {{GTLV_Selected, kAnswer}, {TAG_DONE, 0}});
    gt_result(r, g, gadtools_lvo::GT_GetGadgetAttrsA);
    CHECK_EQ(r.get32(kAnswer), 1u);
    CHECK(!logged("gadtools: gadget kind 11 not handled"));
    CHECK(!logged("gadtools: gadget kind 4 not handled"));
    r.cpu().a[0] = context;
    gt_result(r, g, gadtools_lvo::FreeGadgets);
}

int main() {
    sliders_carry_levels_and_list_views_pick_rows();
    strings_keep_a_buffer_and_scrollers_their_place();
    cycles_step_through_their_labels_and_palettes_pick_colours();
    visual_info_holds_the_screen_and_is_freed();
    create_context_stores_and_frees_the_gadget();
    create_menus_builds_the_chain_and_flags();
    layout_menus_places_titles_and_items();
    free_menus_returns_memory_and_rejects_foreign_strips();
    imsg_vectors_go_through_the_port();
    filter_vectors_return_a1();
    draw_bevel_box_uses_shine_and_shadow_pens();
    draw_bevel_box_recessed_swaps_the_pens();
    draw_bevel_box_leaves_the_rastport_as_it_was();
    create_gadget_makes_buttons_and_displays();
    a_button_draws_raised_and_pressed();
    return check::finish("test_gadtools");
}