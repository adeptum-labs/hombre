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
#include "libs/chrome.hpp"
#include "libs/chrome_palette.hpp"
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
constexpr u32 kFontAttr = kData + 0x7C0;
constexpr u32 kList = kData + 0x800;
constexpr u32 kMessage = kData + 0x880;
constexpr u32 kBitmap = kData + 0x900;
constexpr u32 kRastPort = kData + 0xA00;
constexpr u32 kNewGadget = kData + 0xB00;
constexpr u32 kLabel = kData + 0xB40;
constexpr u32 kText = kData + 0xB60;
constexpr u32 kAnswer = kData + 0xB80;
constexpr u32 kNewWindow = kData + 0xC00;
constexpr u32 kHead = kData + 0x1000;
constexpr u32 kNodes = kData + 0x1040;

// struct NewGadget, the gadget kinds and their tags.
constexpr u32 ng_LeftEdge = 0;
constexpr u32 ng_TopEdge = 2;
constexpr u32 ng_Width = 4;
constexpr u32 ng_Height = 6;
constexpr u32 ng_GadgetText = 8;
constexpr u32 ng_TextAttr = 12;
constexpr u32 ng_GadgetID = 16;
constexpr u32 ng_Flags = 18;
constexpr u32 ng_VisualInfo = 22;
constexpr u32 ng_UserData = 26;
constexpr u32 BUTTON_KIND = 1;
constexpr u32 CHECKBOX_KIND = 2;
constexpr u32 INTEGER_KIND = 3;
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
constexpr u32 GTIN_Number = 0x8008002F;
constexpr u32 GTIN_MaxChars = 0x80080030;
constexpr u32 GA_Disabled = 0x8003000E;
constexpr u32 GTTX_Text = 0x8008000B;
constexpr u32 GTTX_CopyText = 0x8008000C;
constexpr u32 GTNM_Number = 0x8008000D;
constexpr u32 GTCB_Checked = 0x80080004;
constexpr u32 GTCB_Scaled = 0x80080044;
constexpr u32 GTTX_Border = 0x80080039;
constexpr u32 LISTVIEW_KIND = 4;
constexpr u32 GTLV_Top = 0x80080005;
constexpr u32 GTLV_Labels = 0x80080006;
constexpr u32 GTLV_ReadOnly = 0x80080007;
constexpr u32 GTLV_ScrollWidth = 0x80080008;
constexpr u32 GTST_MaxChars = 0x8008002E;
constexpr u32 GTLV_ShowSelected = 0x80080035;
constexpr u32 GTLV_Selected = 0x80080036;
constexpr u32 GTLV_MakeVisible = 0x8008004E;
constexpr u32 LAYOUTA_Spacing = 0x80038002;
constexpr u32 kFillPen = 3;
constexpr u32 kFillTextPen = 2;
constexpr u32 kTextPen = 1;

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

// A TextAttr at kFontAttr naming the font, and a screen opened in it.
u32 put_font_attr(GfxRig& r, const char* name, u32 y_size) {
    r.str(kFontAttr + TextAttr_SIZEOF, name);
    r.put32(kFontAttr + ta_Name, kFontAttr + TextAttr_SIZEOF);
    r.put16(kFontAttr + ta_YSize, y_size);
    r.put8(kFontAttr + ta_Style, 0);
    r.put8(kFontAttr + ta_Flags, 0);
    return kFontAttr;
}

u32 open_screen_in(GfxRig& r, const char* font, u32 y_size, u32 depth) {
    put_new_screen(r, kNewScreen, 640, 400, depth, 0);
    r.put32(kNewScreen + ns_Font, put_font_attr(r, font, y_size));
    const u32 screen = open_new_screen(r, kNewScreen);
    CHECK(screen != 0);
    return screen;
}

u32 visual_info(GfxRig& r, GadTools& g, u32 screen) {
    r.cpu().a[0] = screen;
    r.cpu().a[1] = 0;
    const u32 vi = gt_result(r, g, gadtools_lvo::GetVisualInfoA);
    CHECK(vi != 0);
    return vi;
}

u32 make_surface(GfxRig& r, u32 depth = 2) {
    constexpr u32 kWidth = 640;
    constexpr u32 kHeight = 400;
    const u32 row_bytes = (kWidth + 15) / 16 * 2;
    const u32 plane_bytes = row_bytes * kHeight;
    r.cpu().a[0] = kBitmap;
    r.cpu().d[0] = depth;
    r.cpu().d[1] = kWidth;
    r.cpu().d[2] = kHeight;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::InitBitMap), ExitReason::Halt);
    for (u32 plane = 0; plane < depth; ++plane) {
        const u32 at = r.exec.alloc(plane_bytes, MEMF_CHIP | MEMF_CLEAR);
        CHECK(at != 0);
        r.put32(kBitmap + bm_Planes + plane * 4, at);
    }
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

// A VisualInfo holds its screen's font open until it is freed.
void visual_info_holds_the_font_of_its_screen() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);
    const u32 screen = open_screen_in(r, "hasklig.font", 13, 2);
    const u32 font = r.intuition.font_of_screen(screen);
    CHECK_EQ(r.get16(font + tf_YSize), 13u);
    const u32 accessors = r.get16(font + tf_Accessors);

    const u32 vi = visual_info(r, g, screen);
    CHECK_EQ(r.get32(vi + 8), font);
    CHECK_EQ(r.get16(font + tf_Accessors), accessors + 1);
    r.cpu().a[0] = vi;
    CHECK_EQ(gt_call(r, g, gadtools_lvo::FreeVisualInfo), ExitReason::Halt);
    CHECK_EQ(r.get16(font + tf_Accessors), accessors);
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

// A strip laid out for the VisualInfo of a screen in hasklig 13, whose
// cells are 7 wide: the labels are measured in it and a text item is the
// font and two rows high. GTMN_TextAttr names another font, which the
// items' IntuiTexts then carry.
void layout_menus_measures_the_font_of_the_screen() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);
    const u32 screen = open_screen_in(r, "hasklig.font", 13, 2);
    const u32 vi = visual_info(r, g, screen);
    CHECK_EQ(r.get16(r.intuition.font_of_screen(screen) + tf_XSize), 7u);

    const u32 file = create_menu_strip(r, g);
    CHECK(file != 0);
    const u32 edit = r.get32(file + mu_NextMenu);
    const u32 open = r.get32(file + mu_FirstItem);
    const u32 bar = r.get32(open + mi_NextItem);
    const u32 wrap = r.get32(bar + mi_NextItem);
    const u32 recent = r.get32(open + mi_SubItem);

    r.cpu().a[0] = file;
    r.cpu().a[1] = vi;
    r.cpu().a[2] = 0;
    CHECK_EQ(gt_result(r, g, gadtools_lvo::LayoutMenusA), 1u);
    CHECK_EQ(r.get16(file + mu_Width), 4u * 7 + 16);
    CHECK_EQ(r.get16(file + mu_Height), 15u);
    CHECK_EQ(r.get16(edit + mu_LeftEdge), 4u * 7 + 16);
    // Open: four characters and the padding, a shortcut, a submenu arrow
    // and the column of check marks.
    CHECK_EQ(r.get16(open + mi_Width), 4u * 7 + 8 + (2 * 7 + 16) + 2 * 7 + 16);
    CHECK_EQ(r.get16(open + mi_Height), 15u);
    CHECK_EQ(r.get16(bar + mi_TopEdge), 15u);
    CHECK_EQ(r.get16(bar + mi_Height), 6u);
    CHECK_EQ(r.get16(wrap + mi_TopEdge), 21u);
    CHECK_EQ(r.get16(wrap + mi_Height), 15u);
    CHECK_EQ(r.get16(recent + mi_Height), 15u);
    CHECK_EQ(r.get32(r.get32(open + mi_ItemFill) + 8), 0u);

    r.cpu().a[0] = file;
    r.cpu().a[1] = vi;
    r.cpu().a[2] = r.tags(kTags, {{0x80080031, put_font_attr(r, "topaz.font", 8)},
                                  {TAG_DONE, 0}});
    CHECK_EQ(gt_result(r, g, gadtools_lvo::LayoutMenusA), 1u);
    CHECK_EQ(r.get16(file + mu_Width), 48u);
    CHECK_EQ(r.get16(open + mi_Width), 104u);
    CHECK_EQ(r.get16(open + mi_Height), 10u);
    CHECK_EQ(r.get32(r.get32(open + mi_ItemFill) + 8), kFontAttr);
    CHECK_EQ(r.get32(r.get32(recent + mi_ItemFill) + 8), kFontAttr);
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
    // A generic gadget is its program's to draw, and lets go with a
    // GADGETUP of code 0 as a button does.
    put_new_gadget(r, 10, 60, 40, 10, 0, 5, 0, 0);
    const u32 generic = create_gadget(r, g, 0, palette, 0);
    code = 9;
    CHECK(g.release(0, generic, 1, 1, code));
    CHECK_EQ(code, 9u);
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

void integer_gadgets_hold_their_number_as_text() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);
    const u32 before = avail_mem(r);
    r.cpu().a[0] = kList;
    const u32 context = gt_result(r, g, gadtools_lvo::CreateContext);
    put_new_gadget(r, 10, 20, 60, 12, 0, 5, 0, 0);
    const u32 integer = create_gadget(r, g, INTEGER_KIND, context,
                                      r.tags(kTags, {{GTIN_Number, 42}, {TAG_DONE, 0}}));
    CHECK(integer != 0);
    CHECK_EQ(r.get16(integer + gg_GadgetType), GTYP_GADTOOLS | GTYP_STRGADGET);
    CHECK_EQ(r.get16(integer + gg_Activation), GACT_RELVERIFY | GACT_LONGINT);
    const u32 si = r.get32(integer + gg_SpecialInfo);
    CHECK(si != 0);
    CHECK(r.read_str(r.get32(si + si_Buffer)) == "42");
    CHECK_EQ(r.get32(si + si_LongInt), 42u);
    CHECK_EQ(r.get16(si + si_MaxChars), 11u);
    r.cpu().a[0] = integer;
    r.cpu().a[3] = r.tags(kTags, {{GTIN_Number, kAnswer}, {TAG_DONE, 0}});
    CHECK_EQ(gt_result(r, g, gadtools_lvo::GT_GetGadgetAttrsA), 1u);
    CHECK_EQ(r.get32(kAnswer), 42u);

    // Setting the number rewrites the buffer.
    r.cpu().a[0] = integer;
    r.cpu().a[1] = 0;
    r.cpu().a[3] = r.tags(kTags, {{GTIN_Number, static_cast<u32>(-7)}, {TAG_DONE, 0}});
    CHECK_EQ(gt_call(r, g, gadtools_lvo::GT_SetGadgetAttrsA), ExitReason::Halt);
    CHECK(r.read_str(r.get32(si + si_Buffer)) == "-7");
    CHECK_EQ(r.get32(si + si_LongInt), static_cast<u32>(-7));
    r.cpu().a[0] = integer;
    r.cpu().a[3] = r.tags(kTags, {{GTIN_Number, kAnswer}, {TAG_DONE, 0}});
    CHECK_EQ(gt_result(r, g, gadtools_lvo::GT_GetGadgetAttrsA), 1u);
    CHECK_EQ(r.get32(kAnswer), static_cast<u32>(-7));

    // Without GTIN_Number an integer gadget holds zero.
    put_new_gadget(r, 10, 40, 60, 12, 0, 6, 0, 0);
    const u32 zero = create_gadget(r, g, INTEGER_KIND, integer, 0);
    const u32 zero_si = r.get32(zero + gg_SpecialInfo);
    CHECK(r.read_str(r.get32(zero_si + si_Buffer)) == "0");
    CHECK_EQ(r.get32(zero_si + si_LongInt), 0u);

    // GTIN_MaxChars leaves room for that many characters and the null.
    put_new_gadget(r, 10, 60, 60, 12, 0, 7, 0, 0);
    const u32 limited = create_gadget(
        r, g, INTEGER_KIND, zero,
        r.tags(kTags, {{GTIN_Number, 7}, {GTIN_MaxChars, 4}, {TAG_DONE, 0}}));
    const u32 limited_si = r.get32(limited + gg_SpecialInfo);
    CHECK_EQ(r.get16(limited_si + si_MaxChars), 5u);
    CHECK(r.read_str(r.get32(limited_si + si_Buffer)) == "7");
    CHECK(!logged("gadtools: gadget kind 3 not handled"));

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

// A checkbox is its own 26 by 11 box unless scaled, turns over when let
// go, and reports its state through GTCB_Checked.
void checkboxes_toggle_and_report_their_state() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);
    r.cpu().a[0] = kList;
    const u32 context = gt_result(r, g, gadtools_lvo::CreateContext);

    put_new_gadget(r, 10, 20, 60, 14, 0, 1, 0, 0);
    const u32 checkbox = create_gadget(
        r, g, CHECKBOX_KIND, context, r.tags(kTags, {{GTCB_Checked, 0}, {TAG_DONE, 0}}));
    CHECK(checkbox != 0);
    CHECK_EQ(r.get16(checkbox + gg_Width), 26u);
    CHECK_EQ(r.get16(checkbox + gg_Height), 11u);
    CHECK_EQ(r.get16(checkbox + gg_Flags) & GFLG_SELECTED, 0u);
    CHECK_EQ(r.get16(checkbox + gg_Activation), GACT_RELVERIFY | GACT_TOGGLESELECT);

    u16 code = 0;
    CHECK(g.release(0, checkbox, 5, 5, code));
    CHECK_EQ(code, 1u);
    CHECK_EQ(r.get16(checkbox + gg_Flags) & GFLG_SELECTED, GFLG_SELECTED);
    CHECK(g.release(0, checkbox, 5, 5, code));
    CHECK_EQ(code, 0u);
    CHECK_EQ(r.get16(checkbox + gg_Flags) & GFLG_SELECTED, 0u);

    r.cpu().a[0] = checkbox;
    r.cpu().a[1] = 0;
    r.cpu().a[3] = r.tags(kTags, {{GTCB_Checked, 1}, {TAG_DONE, 0}});
    CHECK_EQ(gt_call(r, g, gadtools_lvo::GT_SetGadgetAttrsA), ExitReason::Halt);
    CHECK_EQ(r.get16(checkbox + gg_Flags) & GFLG_SELECTED, GFLG_SELECTED);
    r.cpu().a[0] = checkbox;
    r.cpu().a[3] = r.tags(kTags, {{GTCB_Checked, kAnswer}, {TAG_DONE, 0}});
    CHECK_EQ(gt_result(r, g, gadtools_lvo::GT_GetGadgetAttrsA), 1u);
    CHECK_EQ(r.get32(kAnswer), 1u);

    // GTCB_Scaled keeps the NewGadget's size.
    put_new_gadget(r, 10, 40, 60, 14, 0, 2, 0, 0);
    const u32 scaled = create_gadget(
        r, g, CHECKBOX_KIND, checkbox, r.tags(kTags, {{GTCB_Scaled, 1}, {TAG_DONE, 0}}));
    CHECK(scaled != 0);
    CHECK_EQ(r.get16(scaled + gg_Width), 60u);
    CHECK_EQ(r.get16(scaled + gg_Height), 14u);

    CHECK(!logged("gadtools: gadget kind 2 not handled"));

    r.cpu().a[0] = context;
    gt_result(r, g, gadtools_lvo::FreeGadgets);
}

// A struct List at head of count nodes from nodes on, 0x40 bytes apart,
// each with its name after it.
void put_list(GfxRig& r, u32 head, u32 nodes, const char* const* names, u32 count) {
    r.put32(head, count != 0 ? nodes : head + 4);
    r.put32(head + 4, 0);
    r.put32(head + 8, count != 0 ? nodes + (count - 1) * 0x40 : head);
    for (u32 i = 0; i < count; ++i) {
        const u32 node = nodes + i * 0x40;
        r.put32(node, i + 1 < count ? node + 0x40 : head + 4);
        r.put32(node + 4, i > 0 ? node - 0x40 : head);
        r.put32(node + 10, r.str(node + 0x20, names[i]));
    }
}

constexpr const char* kEightNames[8] = {"Alpha", "Bravo", "Charlie", "Delta",
                                        "Echo",  "Foxtrot", "Golf",  "Hotel"};

u32 list_view_top(GfxRig& r, GadTools& g, u32 list) {
    r.cpu().a[0] = list;
    r.cpu().a[3] = r.tags(kTags, {{GTLV_Top, kAnswer}, {TAG_DONE, 0}});
    CHECK_EQ(gt_result(r, g, gadtools_lvo::GT_GetGadgetAttrsA), 1u);
    return r.get32(kAnswer);
}

void set_list_view(GfxRig& r, GadTools& g, u32 list, u32 window, u32 tags) {
    r.cpu().a[0] = list;
    r.cpu().a[1] = window;
    r.cpu().a[3] = tags;
    CHECK_EQ(gt_call(r, g, gadtools_lvo::GT_SetGadgetAttrsA), ExitReason::Halt);
}

// A list view takes whole rows of its box, less the scroller on its right
// and the string gadget GTLV_ShowSelected names, which it puts beneath the
// rows and fills with the selected label. Its arrows and scroller move its
// top and report nothing.
void list_views_lay_out_their_parts_and_show_the_selected_label() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);
    const u32 before = avail_mem(r);
    r.cpu().a[0] = kList;
    const u32 context = gt_result(r, g, gadtools_lvo::CreateContext);
    put_list(r, kHead, kNodes, kEightNames, 8);

    put_new_gadget(r, 10, 30, 120, 14, 0, 1, 0, 0);
    const u32 string = create_gadget(r, g, STRING_KIND, context,
                                     r.tags(kTags, {{GTST_MaxChars, 5}, {TAG_DONE, 0}}));
    CHECK(string != 0);
    const u32 si = r.get32(string + gg_SpecialInfo);
    CHECK_EQ(r.get16(si + si_MaxChars), 6u);
    put_new_gadget(r, 10, 30, 120, 60, 0, 2, 0, 0);
    const u32 list = create_gadget(
        r, g, LISTVIEW_KIND, string,
        r.tags(kTags, {{GTLV_ShowSelected, string}, {GTLV_Labels, kHead},
                       {LAYOUTA_Spacing, 1}, {TAG_DONE, 0}}));
    CHECK(list != 0);
    // Rows of 9 pixels in 60 less the string's 14 and a frame of 2: four.
    CHECK_EQ(r.get16(list + gg_LeftEdge), 10u);
    CHECK_EQ(r.get16(list + gg_TopEdge), 30u);
    CHECK_EQ(r.get16(list + gg_Width), 104u);
    CHECK_EQ(r.get16(list + gg_Height), 40u);
    CHECK_EQ(r.get16(list + gg_GadgetID), 2u);
    CHECK_EQ(r.get16(string + gg_LeftEdge), 10u);
    CHECK_EQ(r.get16(string + gg_TopEdge), 70u);
    CHECK_EQ(r.get16(string + gg_Width), 120u);
    CHECK_EQ(r.get16(string + gg_Height), 14u);

    // The scroller and the arrows come before the list in the chain.
    const u32 scroller = r.get32(string + gg_NextGadget);
    const u32 up = r.get32(scroller + gg_NextGadget);
    const u32 down = r.get32(up + gg_NextGadget);
    CHECK_EQ(r.get32(down + gg_NextGadget), list);
    CHECK_EQ(r.get32(list + gg_NextGadget), 0u);
    CHECK_EQ(r.get16(scroller + gg_GadgetType), GTYP_GADTOOLS | GTYP_PROPGADGET);
    CHECK_EQ(r.get16(up + gg_GadgetType), GTYP_GADTOOLS | GTYP_BOOLGADGET);
    CHECK_EQ(r.get16(down + gg_GadgetType), GTYP_GADTOOLS | GTYP_BOOLGADGET);
    const u32 parts[3] = {scroller, up, down};
    const u32 tops[3] = {30, 50, 60};
    const u32 heights[3] = {20, 10, 10};
    for (u32 i = 0; i < 3; ++i) {
        CHECK_EQ(r.get16(parts[i] + gg_LeftEdge), 114u);
        CHECK_EQ(r.get16(parts[i] + gg_TopEdge), tops[i]);
        CHECK_EQ(r.get16(parts[i] + gg_Width), 16u);
        CHECK_EQ(r.get16(parts[i] + gg_Height), heights[i]);
    }
    const u32 pi = r.get32(scroller + gg_SpecialInfo);
    CHECK_EQ(r.get16(pi + pi_Flags) & FREEVERT, FREEVERT);
    CHECK_EQ(r.get16(pi + pi_VertBody), 4u * MAXBODY / 8);
    CHECK_EQ(r.get16(pi + pi_VertPot), 0u);

    // GTLV_MakeVisible scrolls no further than it has to, and the selected
    // label goes into the string gadget, as much as that holds.
    set_list_view(r, g, list, 0,
                  r.tags(kTags, {{GTLV_Labels, 0xFFFFFFFF}, {TAG_DONE, 0}}));
    set_list_view(r, g, list, 0,
                  r.tags(kTags, {{GTLV_Labels, kHead}, {GTLV_MakeVisible, 6},
                                 {GTLV_Selected, 6}, {TAG_DONE, 0}}));
    CHECK_EQ(list_view_top(r, g, list), 3u);
    CHECK(r.read_str(r.get32(si + si_Buffer)) == "Golf");
    CHECK_EQ(r.get16(si + si_NumChars), 4u);
    CHECK_EQ(r.get16(pi + pi_VertPot), 3u * MAXPOT / 4);
    set_list_view(r, g, list, 0, r.tags(kTags, {{GTLV_MakeVisible, 4}, {TAG_DONE, 0}}));
    CHECK_EQ(list_view_top(r, g, list), 3u);
    set_list_view(r, g, list, 0, r.tags(kTags, {{GTLV_Selected, 2}, {TAG_DONE, 0}}));
    CHECK(r.read_str(r.get32(si + si_Buffer)) == "Charl");
    // No selection leaves the string as it is.
    set_list_view(r, g, list, 0, r.tags(kTags, {{GTLV_Selected, 0xFFFF}, {TAG_DONE, 0}}));
    r.cpu().a[0] = list;
    r.cpu().a[3] = r.tags(kTags, {{GTLV_Selected, kAnswer}, {TAG_DONE, 0}});
    gt_result(r, g, gadtools_lvo::GT_GetGadgetAttrsA);
    CHECK_EQ(r.get32(kAnswer), 0xFFFFFFFFu);
    CHECK(r.read_str(r.get32(si + si_Buffer)) == "Charl");

    // The second row of a list whose top is 3 is ordinal 4.
    u16 code = 0;
    CHECK(g.release(0, list, 20, 2 + 9, code));
    CHECK_EQ(code, 4u);
    CHECK(r.read_str(r.get32(si + si_Buffer)) == "Echo");
    // Below the fourth row there is none.
    CHECK(!g.release(0, list, 20, 2 + 4 * 9, code));

    // The arrows move the top a row and stop at the list's ends.
    CHECK(!g.release(0, up, 3, 3, code));
    CHECK_EQ(list_view_top(r, g, list), 2u);
    for (u32 i = 0; i < 4; ++i) {
        CHECK(!g.release(0, down, 3, 3, code));
    }
    CHECK_EQ(list_view_top(r, g, list), 4u);
    CHECK_EQ(r.get16(pi + pi_VertPot), MAXPOT);
    for (u32 i = 0; i < 6; ++i) {
        CHECK(!g.release(0, up, 3, 3, code));
    }
    CHECK_EQ(list_view_top(r, g, list), 0u);

    // The scroller's knob moves the top, and its messages are its own.
    r.put16(pi + pi_VertPot, MAXPOT);
    CHECK(!g.prop_moved(0, scroller, code));
    CHECK_EQ(list_view_top(r, g, list), 4u);
    r.put16(pi + pi_VertPot, MAXPOT / 2);
    CHECK(!g.prop_moved(0, scroller, code));
    CHECK_EQ(list_view_top(r, g, list), 2u);

    // A list of fewer labels than rows stays at its top.
    set_list_view(r, g, list, 0, r.tags(kTags, {{GTLV_Labels, 0xFFFFFFFF}, {TAG_DONE, 0}}));
    put_list(r, kHead, kNodes, kEightNames, 2);
    set_list_view(r, g, list, 0,
                  r.tags(kTags, {{GTLV_Labels, kHead}, {GTLV_Top, 5}, {TAG_DONE, 0}}));
    CHECK_EQ(list_view_top(r, g, list), 0u);
    CHECK_EQ(r.get16(pi + pi_VertBody), MAXBODY);

    // An integer gadget that shows the selection takes its number too, and
    // GTLV_ScrollWidth sets the scroller's width.
    put_new_gadget(r, 200, 30, 60, 14, 0, 3, 0, 0);
    const u32 integer = create_gadget(r, g, INTEGER_KIND, list, 0);
    constexpr const char* kSizes[2] = {"8", "11"};
    put_list(r, kHead + 0x400, kNodes + 0x400, kSizes, 2);
    put_new_gadget(r, 200, 30, 60, 50, 0, 4, 0, 0);
    const u32 sizes = create_gadget(
        r, g, LISTVIEW_KIND, integer,
        r.tags(kTags, {{GTLV_ShowSelected, integer}, {GTLV_Labels, kHead + 0x400},
                       {GTLV_Selected, 1}, {GTLV_ScrollWidth, 20}, {TAG_DONE, 0}}));
    CHECK(sizes != 0);
    CHECK_EQ(r.get16(sizes + gg_Width), 40u);
    CHECK_EQ(r.get16(sizes + gg_Height), 36u);
    CHECK_EQ(r.get16(integer + gg_TopEdge), 66u);
    const u32 integer_si = r.get32(integer + gg_SpecialInfo);
    CHECK(r.read_str(r.get32(integer_si + si_Buffer)) == "11");
    CHECK_EQ(r.get32(integer_si + si_LongInt), 11u);
    r.cpu().a[0] = integer;
    r.cpu().a[3] = r.tags(kTags, {{GTIN_Number, kAnswer}, {TAG_DONE, 0}});
    gt_result(r, g, gadtools_lvo::GT_GetGadgetAttrsA);
    CHECK_EQ(r.get32(kAnswer), 11u);

    r.cpu().a[0] = context;
    gt_result(r, g, gadtools_lvo::FreeGadgets);
    CHECK_EQ(avail_mem(r), before);
}

bool box_has_pen(GfxRig& r, u32 rp, i32 x0, i32 y0, i32 x1, i32 y1, u32 pen) {
    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            if (read_pixel(r, rp, x, y) == pen) {
                return true;
            }
        }
    }
    return false;
}

// The first and last column of the box that has the pen, -1 for none.
struct InkSpan {
    i32 first = -1;
    i32 last = -1;
};

InkSpan inked_columns(GfxRig& r, u32 rp, i32 x0, i32 y0, i32 x1, i32 y1, u32 pen) {
    InkSpan span;
    for (i32 x = x0; x <= x1; ++x) {
        for (i32 y = y0; y <= y1; ++y) {
            if (read_pixel(r, rp, x, y) == pen) {
                span.first = span.first < 0 ? x : span.first;
                span.last = x;
                break;
            }
        }
    }
    return span;
}

// A gadget is drawn and laid out in the font its NewGadget names, or else
// in its VisualInfo's, and holds it open until FreeGadgets: hasklig 13,
// with cells 7 wide, where topaz 8 has cells of 8.
void gadgets_draw_in_the_font_of_their_newgadget() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);
    const u32 screen = open_screen_in(r, "hasklig.font", 13, 2);
    const u32 font = r.intuition.font_of_screen(screen);
    const u32 accessors = r.get16(font + tf_Accessors);
    const u32 rp = make_surface(r);
    r.cpu().a[0] = kList;
    const u32 context = gt_result(r, g, gadtools_lvo::CreateContext);

    // A label to the left ends a gap before the gadget: four cells of 7.
    r.str(kLabel, "Name");
    r.str(kText, "xy");
    put_new_gadget(r, 100, 20, 60, 14, kLabel, 1, 0, 0);
    r.put32(kNewGadget + ng_TextAttr, kFontAttr);
    const u32 text = create_gadget(r, g, TEXT_KIND, context,
                                   r.tags(kTags, {{GTTX_Text, kText}, {TAG_DONE, 0}}));
    CHECK(text != 0);
    CHECK_EQ(r.get16(font + tf_Accessors), accessors + 1);
    g.render(0, rp, text);
    const InkSpan label = inked_columns(r, rp, 40, 18, 99, 36, kTextPen);
    CHECK(label.first >= 100 - 8 - 4 * 7 && label.first < 100 - 8 - 3 * 7);
    CHECK(label.last < 100 - 8);
    const InkSpan shown = inked_columns(r, rp, 100, 18, 159, 36, kTextPen);
    CHECK(shown.first >= 100 && shown.last < 100 + 2 * 7);

    // A list view's rows are the font high: two of 13 in a box of 40.
    put_new_gadget(r, 20, 60, 100, 40, 0, 2, 0, 0);
    put_list(r, kHead, kNodes, kEightNames, 3);
    const u32 list = create_gadget(r, g, LISTVIEW_KIND, text,
                                   r.tags(kTags, {{GTLV_Labels, kHead}, {TAG_DONE, 0}}));
    CHECK(list != 0);
    CHECK_EQ(r.get16(list + gg_Height), 2u * 13 + 4);
    CHECK_EQ(r.get16(font + tf_Accessors), accessors + 2);

    // Without a TextAttr the VisualInfo's font is the gadget's; a string
    // gadget names it to Intuition in a StringExtend.
    const u32 vi = visual_info(r, g, screen);
    put_new_gadget(r, 200, 20, 80, 16, 0, 3, 0, 0);
    r.put32(kNewGadget + ng_TextAttr, 0);
    r.put32(kNewGadget + ng_VisualInfo, vi);
    const u32 string = create_gadget(r, g, STRING_KIND, list, 0);
    CHECK(string != 0);
    CHECK((r.get16(string + gg_Flags) & GFLG_STRINGEXTEND) != 0);
    const u32 extend = r.get32(r.get32(string + gg_SpecialInfo) + si_Extension);
    CHECK(extend != 0);
    CHECK_EQ(r.get32(extend + sex_Font), font);
    CHECK_EQ(r.get16(font + tf_Accessors), accessors + 4);  // the VisualInfo's too

    r.cpu().a[0] = context;
    CHECK_EQ(gt_call(r, g, gadtools_lvo::FreeGadgets), ExitReason::Halt);
    CHECK_EQ(r.get16(font + tf_Accessors), accessors + 1);
    r.cpu().a[0] = vi;
    CHECK_EQ(gt_call(r, g, gadtools_lvo::FreeVisualInfo), ExitReason::Halt);
    CHECK_EQ(r.get16(font + tf_Accessors), accessors);
}

// A list view draws a raised frame around its rows, each label in its row,
// the selected row in the fill pens when GTLV_ShowSelected was given, and
// an arrow in each arrow's box. One only to read is recessed and takes no
// clicks.
void a_list_view_draws_its_rows_and_arrows() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);
    const u32 rp = make_surface(r);
    r.cpu().a[0] = kList;
    const u32 context = gt_result(r, g, gadtools_lvo::CreateContext);
    put_list(r, kHead, kNodes, kEightNames, 3);

    put_new_gadget(r, 20, 20, 100, 40, 0, 1, 0, 0);
    const u32 list = create_gadget(
        r, g, LISTVIEW_KIND, context,
        r.tags(kTags, {{GTLV_ShowSelected, 0}, {GTLV_Labels, kHead}, {GTLV_Selected, 1},
                       {TAG_DONE, 0}}));
    CHECK(list != 0);
    CHECK_EQ(r.get16(list + gg_Width), 84u);
    CHECK_EQ(r.get16(list + gg_Height), 36u);
    CHECK(g.render(0, rp, list));
    CHECK_EQ(read_pixel(r, rp, 20, 20), kShinePen);
    CHECK_EQ(read_pixel(r, rp, 103, 55), kShadowPen);
    // The first row holds its label, the second is the selected one.
    CHECK(box_has_pen(r, rp, 24, 22, 70, 29, kTextPen));
    CHECK(!box_has_pen(r, rp, 22, 22, 101, 29, kFillPen));
    CHECK_EQ(read_pixel(r, rp, 100, 31), kFillPen);
    CHECK(box_has_pen(r, rp, 24, 30, 70, 37, kFillTextPen));
    CHECK(!box_has_pen(r, rp, 22, 46, 101, 53, kTextPen));
    // Held down it looks the same: it is no button.
    r.put16(list + gg_Flags, r.get16(list + gg_Flags) | GFLG_SELECTED);
    CHECK(g.render(0, rp, list));
    CHECK_EQ(read_pixel(r, rp, 100, 23), 0u);
    CHECK_EQ(read_pixel(r, rp, 20, 20), kShinePen);

    // The arrows stand below the scroller, right of the rows.
    const u32 scroller = r.get32(context + gg_NextGadget);
    const u32 up = r.get32(scroller + gg_NextGadget);
    const u32 down = r.get32(up + gg_NextGadget);
    CHECK_EQ(r.get16(up + gg_TopEdge), 36u);
    CHECK_EQ(r.get16(down + gg_TopEdge), 46u);
    CHECK(!box_has_pen(r, rp, 104, 36, 119, 55, kTextPen));
    CHECK(g.render(0, rp, up));
    CHECK(box_has_pen(r, rp, 104, 36, 119, 45, kTextPen));
    CHECK(!box_has_pen(r, rp, 104, 46, 119, 55, kTextPen));
    CHECK(g.render(0, rp, down));
    CHECK(box_has_pen(r, rp, 104, 46, 119, 55, kTextPen));

    // Without GTLV_ShowSelected the selected row is not marked; a list
    // only to read is recessed.
    put_new_gadget(r, 200, 20, 100, 40, 0, 2, 0, 0);
    const u32 plain = create_gadget(
        r, g, LISTVIEW_KIND, list,
        r.tags(kTags, {{GTLV_Labels, kHead}, {GTLV_Selected, 1}, {GTLV_ReadOnly, 1},
                       {TAG_DONE, 0}}));
    CHECK(plain != 0);
    CHECK_EQ(r.get16(plain + gg_Activation), 0u);
    CHECK(g.render(0, rp, plain));
    CHECK(!box_has_pen(r, rp, 202, 22, 281, 53, kFillPen));
    CHECK(box_has_pen(r, rp, 204, 30, 250, 37, kTextPen));
    CHECK_EQ(read_pixel(r, rp, 200, 20), kShadowPen);
    CHECK_EQ(read_pixel(r, rp, 283, 55), kShinePen);

    r.cpu().a[0] = context;
    gt_result(r, g, gadtools_lvo::FreeGadgets);
}

// The arrows of a list view take the look of the surface's depth. On 2 and 4
// planes they are the 3.1 arrow image in the default pens: the fill pen, the
// shine pen along the top and left, the shadow pen along the bottom and right
// and in the triangle, the edges the other way round when held down. On 5, 6
// and 7 planes they are the modern look's button in the chrome pens of that
// depth.
void list_arrows_take_the_look_of_the_surface() {
    for (const u32 depth : {2u, 4u, 5u, 6u, 7u}) {
        GfxRig r;
        GadTools g;
        init_gadtools(r, g);
        const u32 rp = make_surface(r, depth);
        r.cpu().a[0] = kList;
        const u32 context = gt_result(r, g, gadtools_lvo::CreateContext);
        put_list(r, kHead, kNodes, kEightNames, 3);
        put_new_gadget(r, 20, 20, 100, 40, 0, 1, 0, 0);
        const u32 list = create_gadget(r, g, LISTVIEW_KIND, context,
                                       r.tags(kTags, {{GTLV_Labels, kHead}, {TAG_DONE, 0}}));
        CHECK(list != 0);
        const u32 scroller = r.get32(context + gg_NextGadget);
        const u32 up = r.get32(scroller + gg_NextGadget);
        const u32 down = r.get32(up + gg_NextGadget);
        CHECK(g.render(0, rp, up));
        CHECK(g.render(0, rp, down));

        // The arrows are 16 by 10 at (104,36) and (104,46).
        if (chrome::style_for_depth(depth) == chrome::Style::Classic) {
            for (const i32 top : {36, 46}) {
                CHECK_EQ(read_pixel(r, rp, 104, top), kShinePen);
                CHECK_EQ(read_pixel(r, rp, 119, top), kShadowPen);
                CHECK_EQ(read_pixel(r, rp, 104, top + 9), kShinePen);
                CHECK_EQ(read_pixel(r, rp, 119, top + 9), kShadowPen);
                CHECK_EQ(read_pixel(r, rp, 106, top + 5), kFillPen);
            }
            CHECK_EQ(read_pixel(r, rp, 111, 39), kShadowPen);
            CHECK_EQ(read_pixel(r, rp, 111, 50), kShadowPen);
            r.put16(up + gg_Flags, r.get16(up + gg_Flags) | GFLG_SELECTED);
            CHECK(g.render(0, rp, up));
            CHECK_EQ(read_pixel(r, rp, 104, 36), kShadowPen);
            CHECK_EQ(read_pixel(r, rp, 119, 36), kShinePen);
            CHECK_EQ(read_pixel(r, rp, 106, 41), kFillPen);
            continue;
        }
        const auto pen = [depth](u8 chrome_pen) {
            return static_cast<u32>(chrome::physical_pen(chrome_pen, depth));
        };
        CHECK_EQ(read_pixel(r, rp, 104, 36), pen(chrome::kTrack));
        CHECK_EQ(read_pixel(r, rp, 104, 46), pen(chrome::kTrack));
        CHECK(box_has_pen(r, rp, 104, 36, 119, 45, pen(chrome::kFrameDark)));
        CHECK(box_has_pen(r, rp, 104, 46, 119, 55, pen(chrome::kFrameDark)));
        for (i32 y = 36; y <= 55; ++y) {
            for (i32 x = 104; x <= 119; ++x) {
                CHECK(read_pixel(r, rp, x, y) >= pen(chrome::kScreenBar0));
            }
        }
        r.put16(up + gg_Flags, r.get16(up + gg_Flags) | GFLG_SELECTED);
        CHECK(g.render(0, rp, up));
        CHECK(box_has_pen(r, rp, 104, 36, 119, 45, pen(chrome::kFrameLight)));
        CHECK(!box_has_pen(r, rp, 104, 46, 119, 55, pen(chrome::kFrameLight)));
    }
}

u32 get_msg(GfxRig& r, u32 port) {
    r.cpu().a[0] = port;
    return r.result(Base::Exec, exec_lvo::GetMsg);
}

void reply_msg(GfxRig& r, u32 msg) {
    r.cpu().a[1] = msg;
    CHECK_EQ(r.call(Base::Exec, exec_lvo::ReplyMsg), ExitReason::Halt);
}

// In a window a click on a row sends IDCMP_GADGETUP with the row's ordinal;
// the arrows and the scroller scroll the rows and send nothing.
void a_list_view_in_a_window_reports_rows_and_scrolls_by_itself() {
    constexpr u32 kLeft = 1;
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);
    r.intuition.set_gadget_renderer(&GadTools::render_gadget, &g);
    r.intuition.set_gadget_release(&GadTools::release_gadget);
    r.intuition.set_gadget_prop_code(&GadTools::prop_gadget_moved);
    g.set_intuition(&r.intuition);
    put_new_screen(r, kNewScreen, 640, 400, 2, 0);
    CHECK(open_new_screen(r, kNewScreen) != 0);

    r.cpu().a[0] = kList;
    const u32 context = gt_result(r, g, gadtools_lvo::CreateContext);
    put_list(r, kHead, kNodes, kEightNames, 8);
    put_new_gadget(r, 20, 30, 120, 40, 0, 7, 0, 0);
    const u32 list = create_gadget(
        r, g, LISTVIEW_KIND, context,
        r.tags(kTags, {{GTLV_ShowSelected, 0}, {GTLV_Labels, kHead}, {TAG_DONE, 0}}));
    CHECK(list != 0);

    for (u32 i = 0; i < NewWindow_SIZEOF; ++i) {
        r.put8(kNewWindow + i, 0);
    }
    r.put16(kNewWindow + nw_LeftEdge, 50);
    r.put16(kNewWindow + nw_TopEdge, 50);
    r.put16(kNewWindow + nw_Width, 200);
    r.put16(kNewWindow + nw_Height, 120);
    r.put8(kNewWindow + nw_BlockPen, 1);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_GADGETUP | IDCMP_GADGETDOWN | IDCMP_MOUSEMOVE);
    r.put32(kNewWindow + nw_Flags, WFLG_ACTIVATE);
    r.put32(kNewWindow + nw_FirstGadget, context);
    r.cpu().a[0] = kNewWindow;
    const u32 window = r.result(Base::Intuition, intuition_lvo::OpenWindow);
    CHECK(window != 0);
    const u32 port = r.get32(window + wd_UserPort);
    const u32 wrp = r.get32(window + wd_RPort);
    // The window opened with the rows drawn: the first label is there.
    CHECK(box_has_pen(r, wrp, 24, 32, 70, 39, kTextPen));

    // The third row, 8 pixels a row under a frame of 2.
    r.intuition.pointer_event(50 + 30, 50 + 30 + 2 + 16 + 3, kLeft, kLeft, 0, true);
    CHECK_EQ(get_msg(r, port), 0u);
    r.intuition.pointer_event(50 + 30, 50 + 30 + 2 + 16 + 3, 0, 0, kLeft, false);
    const u32 msg = get_msg(r, port);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_GADGETUP);
    CHECK_EQ(r.get16(msg + im_Code), 2u);
    CHECK_EQ(r.get32(msg + im_IAddress), list);
    reply_msg(r, msg);
    CHECK_EQ(get_msg(r, port), 0u);
    CHECK_EQ(read_pixel(r, wrp, 100, 30 + 2 + 16 + 1), kFillPen);

    // The down arrow, the lower of the two below the scroller.
    r.intuition.pointer_event(50 + 124 + 5, 50 + 30 + 26 + 5, kLeft, kLeft, 0, true);
    r.intuition.pointer_event(50 + 124 + 5, 50 + 30 + 26 + 5, 0, 0, kLeft, false);
    CHECK_EQ(get_msg(r, port), 0u);
    CHECK_EQ(list_view_top(r, g, list), 1u);
    // The selected label moved up a row with the rest.
    CHECK_EQ(read_pixel(r, wrp, 100, 30 + 2 + 8 + 1), kFillPen);
    CHECK_EQ(read_pixel(r, wrp, 100, 30 + 2 + 16 + 1), 0u);

    // The knob dragged to the bottom of the scroller.
    r.intuition.pointer_event(50 + 124 + 8, 50 + 30 + 5, kLeft, kLeft, 0, true);
    r.intuition.pointer_event(50 + 124 + 8, 50 + 30 + 25, kLeft, 0, 0, true);
    r.intuition.pointer_event(50 + 124 + 8, 50 + 30 + 25, 0, 0, kLeft, false);
    CHECK_EQ(get_msg(r, port), 0u);
    CHECK_EQ(list_view_top(r, g, list), 4u);
    CHECK(!box_has_pen(r, wrp, 22, 32, 101, 63, kFillPen));

    // A row past the last label sends nothing.
    set_list_view(r, g, list, window,
                  r.tags(kTags, {{GTLV_Labels, 0xFFFFFFFF}, {TAG_DONE, 0}}));
    put_list(r, kHead, kNodes, kEightNames, 2);
    set_list_view(r, g, list, window, r.tags(kTags, {{GTLV_Labels, kHead}, {TAG_DONE, 0}}));
    r.intuition.pointer_event(50 + 30, 50 + 30 + 2 + 16 + 3, kLeft, kLeft, 0, true);
    r.intuition.pointer_event(50 + 30, 50 + 30 + 2 + 16 + 3, 0, 0, kLeft, false);
    CHECK_EQ(get_msg(r, port), 0u);

    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);
    r.cpu().a[0] = context;
    gt_result(r, g, gadtools_lvo::FreeGadgets);
    r.intuition.set_gadget_renderer(nullptr, nullptr);
    r.intuition.set_gadget_release(nullptr);
    r.intuition.set_gadget_prop_code(nullptr);
}

constexpr u32 SLIDER_KIND = 11;
constexpr u32 GTSL_Min = 0x80080026;
constexpr u32 GTSL_Max = 0x80080027;
constexpr u32 GTSL_Level = 0x80080028;
constexpr u32 GTSL_MaxLevelLen = 0x80080029;
constexpr u32 GTSL_LevelFormat = 0x8008002A;
constexpr u32 GTSL_LevelPlace = 0x8008002B;
constexpr u32 GTSL_DispFunc = 0x8008002C;
constexpr u32 PLACETEXT_RIGHT = 0x2;
constexpr u32 kFormat = kLabel;
constexpr u32 kOtherFormat = kText;
constexpr u32 kDispFunc = kCall + 0x100;

// The pens of the box, row by row, to tell one drawing of text from another.
std::string pixels_of(GfxRig& r, u32 rp, i32 x0, i32 y0, i32 x1, i32 y1) {
    std::string pens;
    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            pens += static_cast<char>('0' + read_pixel(r, rp, x, y));
        }
    }
    return pens;
}

// A slider whose level, 0 to 255, shows in a field of 3 characters left of
// its 120 by 10 box at 100, top; the format is the one at kFormat.
u32 create_level_slider(GfxRig& r, GadTools& g, u32 previous, i32 top, u32 level, u32 extra_tag,
                        u32 extra_data) {
    put_new_gadget(r, 100, top, 120, 10, 0, 1, 0, 0);
    return create_gadget(
        r, g, SLIDER_KIND, previous,
        r.tags(kTags, {{GTSL_Min, 0}, {GTSL_Max, 255}, {GTSL_Level, level},
                       {GTSL_MaxLevelLen, 3}, {GTSL_LevelFormat, kFormat},
                       {extra_tag, extra_data}, {TAG_DONE, 0}}));
}

// A slider shows its level as text only when it has a level format: in a
// field of GTSL_MaxLevelLen characters, 8 pixels from its box, to the left
// of it unless GTSL_LevelPlace says otherwise. The field is cleared first.
void a_slider_draws_its_level_beside_its_box() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);
    const u32 rp = make_surface(r);
    r.cpu().a[0] = kList;
    const u32 context = gt_result(r, g, gadtools_lvo::CreateContext);

    // Without a format there is no text anywhere.
    put_new_gadget(r, 100, 20, 120, 10, 0, 1, 0, 0);
    const u32 bare = create_gadget(
        r, g, SLIDER_KIND, context,
        r.tags(kTags, {{GTSL_Min, 0}, {GTSL_Max, 255}, {GTSL_Level, 5}, {GTSL_MaxLevelLen, 3},
                       {TAG_DONE, 0}}));
    CHECK(bare != 0);
    CHECK(g.render(0, rp, bare));
    CHECK(!box_has_pen(r, rp, 0, 15, 639, 35, kTextPen));

    // "%3lu" gives "  5": the field is 3 cells of 8 ending 8 before the box,
    // and the 5 is in its last cell. What was in the field is gone.
    r.str(kFormat, "%3lu");
    r.graphics.set_pens(rp, kFillPen, 0, JAM1);
    r.graphics.rect_fill(rp, 68, 22, 91, 27);
    const u32 left = create_level_slider(r, g, bare, 20, 5, GTSL_LevelPlace, 0);
    CHECK(left != 0);
    CHECK(g.render(0, rp, left));
    const InkSpan beside = inked_columns(r, rp, 0, 15, 639, 35, kTextPen);
    CHECK(beside.first >= 68 + 16 && beside.last <= 91);
    CHECK(!box_has_pen(r, rp, 68, 15, 91, 35, kFillPen));

    // To the right the field starts 8 pixels past the box.
    const u32 right = create_level_slider(r, g, left, 60, 5, GTSL_LevelPlace, PLACETEXT_RIGHT);
    CHECK(g.render(0, rp, right));
    const InkSpan after = inked_columns(r, rp, 100, 55, 639, 75, kTextPen);
    CHECK(after.first >= 100 + 120 + 8 + 16 && after.last <= 100 + 120 + 8 + 23);

    // A minus sign in the format puts the digits first in the field.
    r.str(kOtherFormat, "%-3lu");
    put_new_gadget(r, 100, 100, 120, 10, 0, 1, 0, 0);
    const u32 flush = create_gadget(
        r, g, SLIDER_KIND, right,
        r.tags(kTags, {{GTSL_Max, 255}, {GTSL_Level, 5}, {GTSL_MaxLevelLen, 3},
                       {GTSL_LevelFormat, kOtherFormat}, {TAG_DONE, 0}}));
    CHECK(g.render(0, rp, flush));
    const InkSpan first_cell = inked_columns(r, rp, 0, 95, 639, 115, kTextPen);
    CHECK(first_cell.first >= 68 && first_cell.last < 68 + 8);

    r.cpu().a[0] = context;
    gt_result(r, g, gadtools_lvo::FreeGadgets);
}

// The level shown is what GTSL_DispFunc answers to the gadget and the
// level pushed as C code takes them, here 1 << level, and the registers of
// the caller are left as they were. %d takes the level as a word.
void a_slider_shows_what_its_display_function_makes_of_the_level() {
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);
    const u32 rp = make_surface(r);
    r.cpu().a[0] = kList;
    const u32 context = gt_result(r, g, gadtools_lvo::CreateContext);
    // move.l 8(sp),d1; moveq #1,d0; lsl.l d1,d0; rts
    const u32 code[] = {0x222F, 0x0008, 0x7001, 0xE3A8, 0x4E75};
    for (u32 i = 0; i < 5; ++i) {
        r.put16(kDispFunc + 2 * i, code[i]);
    }
    r.jit.flush();
    r.str(kFormat, "%ld");
    r.str(kOtherFormat, "%d");
    const u32 eight = create_level_slider(r, g, context, 20, 8, GTSL_LevelPlace, 0);
    const u32 shifted = create_level_slider(r, g, eight, 60, 3, GTSL_DispFunc, kDispFunc);
    const u32 three = create_level_slider(r, g, shifted, 100, 3, GTSL_LevelPlace, 0);
    CHECK(eight != 0 && shifted != 0 && three != 0);

    CHECK(g.render(0, rp, eight));
    CHECK(g.render(0, rp, three));
    r.cpu().d[3] = 0xCAFE;
    r.cpu().a[2] = 0x1234;
    r.cpu().a[7] = kStack;
    CHECK(g.render(0, rp, shifted));
    CHECK_EQ(r.cpu().d[3], 0xCAFEu);
    CHECK_EQ(r.cpu().a[2], 0x1234u);
    CHECK_EQ(r.cpu().a[7], kStack);

    const std::string eights = pixels_of(r, rp, 68, 20, 91, 29);
    CHECK(eights.find_first_not_of('0') != std::string::npos);
    CHECK(pixels_of(r, rp, 68, 60, 91, 69) == eights);
    CHECK(pixels_of(r, rp, 68, 100, 91, 109) != eights);

    // A word format shows the same level as a long one.
    put_new_gadget(r, 100, 140, 120, 10, 0, 1, 0, 0);
    const u32 word = create_gadget(
        r, g, SLIDER_KIND, three,
        r.tags(kTags, {{GTSL_Max, 255}, {GTSL_Level, 8}, {GTSL_MaxLevelLen, 3},
                       {GTSL_LevelFormat, kOtherFormat}, {TAG_DONE, 0}}));
    CHECK(g.render(0, rp, word));
    CHECK(pixels_of(r, rp, 68, 140, 91, 149) == pixels_of(r, rp, 68, 20, 91, 29));

    r.cpu().a[0] = context;
    gt_result(r, g, gadtools_lvo::FreeGadgets);
}

// In a window the level is drawn again by GT_SetGadgetAttrs and by each
// move of the knob, here a click on the track left of it.
void a_slider_in_a_window_draws_its_level_again_when_it_changes() {
    constexpr u32 kLeft = 1;
    GfxRig r;
    GadTools g;
    init_gadtools(r, g);
    r.intuition.set_gadget_renderer(&GadTools::render_gadget, &g);
    r.intuition.set_gadget_prop_code(&GadTools::prop_gadget_moved);
    g.set_intuition(&r.intuition);
    put_new_screen(r, kNewScreen, 640, 400, 2, 0);
    CHECK(open_new_screen(r, kNewScreen) != 0);
    r.cpu().a[0] = kList;
    const u32 context = gt_result(r, g, gadtools_lvo::CreateContext);
    r.str(kFormat, "%3lu");
    put_new_gadget(r, 60, 30, 120, 10, 0, 1, 0, 0);
    const u32 slider = create_gadget(
        r, g, SLIDER_KIND, context,
        r.tags(kTags, {{GTSL_Min, 0}, {GTSL_Max, 9}, {GTSL_Level, 5}, {GTSL_MaxLevelLen, 3},
                       {GTSL_LevelFormat, kFormat}, {TAG_DONE, 0}}));
    CHECK(slider != 0);
    for (u32 i = 0; i < NewWindow_SIZEOF; ++i) {
        r.put8(kNewWindow + i, 0);
    }
    r.put16(kNewWindow + nw_LeftEdge, 50);
    r.put16(kNewWindow + nw_TopEdge, 50);
    r.put16(kNewWindow + nw_Width, 200);
    r.put16(kNewWindow + nw_Height, 120);
    r.put8(kNewWindow + nw_BlockPen, 1);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_GADGETUP | IDCMP_GADGETDOWN | IDCMP_MOUSEMOVE);
    r.put32(kNewWindow + nw_Flags, WFLG_ACTIVATE);
    r.put32(kNewWindow + nw_FirstGadget, context);
    r.cpu().a[0] = kNewWindow;
    const u32 window = r.result(Base::Intuition, intuition_lvo::OpenWindow);
    CHECK(window != 0);
    const u32 port = r.get32(window + wd_UserPort);
    const u32 wrp = r.get32(window + wd_RPort);

    // The field is 3 cells of 8 ending 8 before the box, at 28 to 51.
    r.cpu().a[0] = window;
    CHECK_EQ(gt_call(r, g, gadtools_lvo::GT_RefreshWindow), ExitReason::Halt);
    CHECK(box_has_pen(r, wrp, 28, 30, 51, 39, kTextPen));
    const std::string five = pixels_of(r, wrp, 28, 30, 51, 39);

    r.cpu().a[0] = slider;
    r.cpu().a[1] = window;
    r.cpu().a[3] = r.tags(kTags, {{GTSL_Level, 7}, {TAG_DONE, 0}});
    CHECK_EQ(gt_call(r, g, gadtools_lvo::GT_SetGadgetAttrsA), ExitReason::Halt);
    const std::string seven = pixels_of(r, wrp, 28, 30, 51, 39);
    CHECK(box_has_pen(r, wrp, 28, 30, 51, 39, kTextPen));
    CHECK(seven != five);

    r.intuition.pointer_event(50 + 62, 50 + 35, kLeft, kLeft, 0, true);
    r.intuition.pointer_event(50 + 62, 50 + 35, 0, 0, kLeft, false);
    for (u32 msg = get_msg(r, port); msg != 0; msg = get_msg(r, port)) {
        reply_msg(r, msg);
    }
    r.cpu().a[0] = slider;
    r.cpu().a[3] = r.tags(kTags, {{GTSL_Level, kAnswer}, {TAG_DONE, 0}});
    CHECK_EQ(gt_result(r, g, gadtools_lvo::GT_GetGadgetAttrsA), 1u);
    const u32 moved_to = r.get32(kAnswer);
    CHECK(moved_to < 7);
    const std::string moved = pixels_of(r, wrp, 28, 30, 51, 39);
    CHECK(moved != seven);
    r.cpu().a[0] = slider;
    r.cpu().a[1] = window;
    r.cpu().a[3] = r.tags(kTags, {{GTSL_Level, moved_to}, {TAG_DONE, 0}});
    CHECK_EQ(gt_call(r, g, gadtools_lvo::GT_SetGadgetAttrsA), ExitReason::Halt);
    CHECK(pixels_of(r, wrp, 28, 30, 51, 39) == moved);

    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);
    r.cpu().a[0] = context;
    gt_result(r, g, gadtools_lvo::FreeGadgets);
    r.intuition.set_gadget_prop_code(nullptr);
}

int main() {
    a_slider_draws_its_level_beside_its_box();
    a_slider_shows_what_its_display_function_makes_of_the_level();
    a_slider_in_a_window_draws_its_level_again_when_it_changes();
    list_views_lay_out_their_parts_and_show_the_selected_label();
    a_list_view_draws_its_rows_and_arrows();
    list_arrows_take_the_look_of_the_surface();
    a_list_view_in_a_window_reports_rows_and_scrolls_by_itself();
    checkboxes_toggle_and_report_their_state();
    sliders_carry_levels_and_list_views_pick_rows();
    strings_keep_a_buffer_and_scrollers_their_place();
    integer_gadgets_hold_their_number_as_text();
    cycles_step_through_their_labels_and_palettes_pick_colours();
    visual_info_holds_the_screen_and_is_freed();
    visual_info_holds_the_font_of_its_screen();
    create_context_stores_and_frees_the_gadget();
    create_menus_builds_the_chain_and_flags();
    layout_menus_places_titles_and_items();
    layout_menus_measures_the_font_of_the_screen();
    free_menus_returns_memory_and_rejects_foreign_strips();
    imsg_vectors_go_through_the_port();
    filter_vectors_return_a1();
    draw_bevel_box_uses_shine_and_shadow_pens();
    draw_bevel_box_recessed_swaps_the_pens();
    draw_bevel_box_leaves_the_rastport_as_it_was();
    create_gadget_makes_buttons_and_displays();
    a_button_draws_raised_and_pressed();
    gadgets_draw_in_the_font_of_their_newgadget();
    return check::finish("test_gadtools");
}