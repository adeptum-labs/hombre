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
#include "hid_mouse.hpp"
#include "libs/chrome.hpp"
#include "libs/chrome_palette.hpp"
#include "libs/display_modes.hpp"
#include "libs/keymap_library.hpp"
#include "libs/raster.hpp"
#include "libs/utility.hpp"

#include <string>
#include <vector>

using namespace hombre;
using namespace hombre::m68k;
using namespace hombre::ndk;
using gfx_test::Base;
using gfx_test::GfxRig;
using gfx_test::kData;
using gfx_test::logged;

namespace hombre {

// The requester state EasyRequestArgs keeps is not part of its public API;
// the tests read it to aim the pointer and to count the buttons.
struct IntuitionTest {
    static u32 button_count(const Intuition& g, u32 window) {
        const Intuition::Requester* req = g.find_requester(window);
        return req != nullptr ? req->button_count : 0;
    }

    static Rect button(const Intuition& g, u32 window, u32 index) {
        const Intuition::Requester* req = g.find_requester(window);
        if (req == nullptr || index >= req->button_count) {
            return Rect{0, 0, -1, -1};
        }
        return req->buttons[index];
    }

    static std::string label(const Intuition& g, u32 window, u32 index) {
        const Intuition::Requester* req = g.find_requester(window);
        if (req == nullptr || index >= req->button_count) {
            return std::string();
        }
        return std::string(req->labels[index], req->label_length[index]);
    }

    static u32 requester_window(const Intuition& g) {
        return g.requesters_.empty() ? 0 : g.requesters_[0].window;
    }

    // Everything Intuition keeps for screens, windows, queued messages,
    // requesters and gadget hooks.
    static usize records_kept(const Intuition& g) {
        return g.screens_.size() + g.windows_.size() + g.messages_.size() +
               g.screen_records_.size() + g.window_records_.size() + g.requesters_.size() +
               g.gadget_hooks_.size();
    }

    static const Intuition::ScreenModePrefs& screen_mode(const Intuition& g) {
        return g.wb_mode_;
    }
};

}

namespace {

constexpr u32 kNewScreen = kData;
constexpr u32 kTagList = kData + 0x100;
constexpr u32 kTitle = kData + 0x200;
constexpr u32 kBuffer = kData + 0x300;
constexpr u32 kNewWindow = kData + 0x500;
constexpr u32 kWinTitle = kData + 0x600;
constexpr u32 kMenu = kData + 0x700;
constexpr u32 kMenu2 = kData + 0x750;
constexpr u32 kItem0 = kData + 0x7A0;
constexpr u32 kItem1 = kData + 0x7E0;
constexpr u32 kItem2 = kData + 0x820;
constexpr u32 kSub0 = kData + 0x860;
constexpr u32 kSub1 = kData + 0x8A0;
constexpr u32 kMenuA = kData + 0xB00;
constexpr u32 kMenuB = kData + 0xB80;
constexpr u32 kItemA0 = kData + 0xC00;
constexpr u32 kItemA1 = kData + 0xC40;
constexpr u32 kItemA2 = kData + 0xC80;
constexpr u32 kItemB0 = kData + 0xD00;
constexpr u32 kItemB1 = kData + 0xD40;
constexpr u32 kSubItem0 = kData + 0xD80;
constexpr u32 kSubItem1 = kData + 0xDC0;
constexpr u32 kFillA0 = kData + 0xE00;
constexpr u32 kFillA1 = kData + 0xE20;
constexpr u32 kFillA2 = kData + 0xE40;
constexpr u32 kFillB0 = kData + 0xE80;
constexpr u32 kFillB1 = kData + 0xEA0;
constexpr u32 kFillSub0 = kData + 0xEC0;
constexpr u32 kFillSub1 = kData + 0xEE0;
constexpr u32 kTextA0 = kData + 0xF00;
constexpr u32 kTextA1 = kData + 0xF20;
constexpr u32 kTextA2 = kData + 0xF40;
constexpr u32 kTextB0 = kData + 0xF80;
constexpr u32 kTextB1 = kData + 0xFA0;
constexpr u32 kTextSub0 = kData + 0xFC0;
constexpr u32 kTextSub1 = kData + 0xFE0;
constexpr u32 kMenuFile = kData + 0x1080;
constexpr u32 kMenuEdit = kData + 0x10A0;
constexpr u32 kEasy = kData + 0x2000;
constexpr u32 kEasyTitle = kData + 0x2040;
constexpr u32 kEasyText = kData + 0x2080;
constexpr u32 kEasyGadgets = kData + 0x20C0;
constexpr u32 kEasyArgs = kData + 0x2100;
constexpr u32 kEasyArg0 = kData + 0x2140;
constexpr u32 kEasyArg1 = kData + 0x2180;
constexpr u32 kEasyArg2 = kData + 0x21C0;
constexpr u32 kIdcmpOut = kData + 0x2200;
constexpr u32 kEasyMessage = kData + 0x2240;
constexpr u32 kEasyTask = kData + 0x2280;
constexpr u32 kIPrefs = kData + 0x2300;
constexpr u32 kAutoBodyText0 = kData + 0x2400;
constexpr u32 kAutoBodyText1 = kData + 0x2440;
constexpr u32 kAutoPosText = kData + 0x2480;
constexpr u32 kAutoNegText = kData + 0x24C0;
constexpr u32 kAutoBody0 = kData + 0x2500;
constexpr u32 kAutoBody1 = kData + 0x2520;
constexpr u32 kAutoPos = kData + 0x2540;
constexpr u32 kAutoNeg = kData + 0x2560;

// struct Menu, struct MenuItem and struct IntuiText
constexpr u32 mu_NextMenu = 0;
constexpr u32 mu_LeftEdge = 4;
constexpr u32 mu_TopEdge = 6;
constexpr u32 mu_Width = 8;
constexpr u32 mu_Height = 10;
constexpr u32 mu_Flags = 12;
constexpr u32 mu_MenuName = 14;
constexpr u32 mu_FirstItem = 18;
constexpr u32 mi_NextItem = 0;
constexpr u32 mi_LeftEdge = 4;
constexpr u32 mi_TopEdge = 6;
constexpr u32 mi_Width = 8;
constexpr u32 mi_Height = 10;
constexpr u32 mi_Flags = 12;
constexpr u32 mi_MutualExclude = 14;
constexpr u32 mi_ItemFill = 18;
constexpr u32 mi_Command = 26;
constexpr u32 mi_SubItem = 28;
constexpr u32 mi_NextSelect = 32;
constexpr u32 it_LeftEdge = 4;
constexpr u32 it_TopEdge = 6;
constexpr u32 it_IText = 12;
constexpr u32 it_NextText = 16;
constexpr u16 MENUENABLED = 0x0001;
constexpr u16 CHECKIT = 0x0001;
constexpr u16 ITEMTEXT = 0x0002;
constexpr u16 MENUTOGGLE = 0x0008;
constexpr u16 ITEMENABLED = 0x0010;
constexpr u16 HIGHCOMP = 0x0040;
constexpr u16 CHECKED = 0x0100;
constexpr u16 MENUNULL = 0xFFFF;
constexpr u32 IDCMP_MENUPICK = 0x00000100;
constexpr u32 kMenuIdcmp = IDCMP_MENUPICK | IDCMP_MOUSEBUTTONS;
constexpr u32 WFLG_RMBTRAP = 0x00010000;

// struct DrawInfo
constexpr u32 dri_NumPens = 2;
constexpr u32 dri_Pens = 4;
constexpr u32 dri_Font = 8;
constexpr u32 dri_Depth = 12;

// struct Image
constexpr u32 img_LeftEdge = 0;
constexpr u32 img_TopEdge = 2;
constexpr u32 img_Width = 4;
constexpr u32 img_Height = 6;
constexpr u32 img_Depth = 8;
constexpr u32 img_ImageData = 10;
constexpr u32 img_PlanePick = 14;
constexpr u32 img_PlaneOnOff = 15;
constexpr u32 img_NextImage = 16;

u32 put_image(GfxRig& r, u32 at, i32 left, i32 top, u32 width, u32 height, u32 depth, u32 data,
              u32 plane_pick, u32 plane_on_off, u32 next = 0) {
    r.put16(at + img_LeftEdge, static_cast<u32>(left));
    r.put16(at + img_TopEdge, static_cast<u32>(top));
    r.put16(at + img_Width, width);
    r.put16(at + img_Height, height);
    r.put16(at + img_Depth, depth);
    r.put32(at + img_ImageData, data);
    r.put8(at + img_PlanePick, plane_pick);
    r.put8(at + img_PlaneOnOff, plane_on_off);
    r.put32(at + img_NextImage, next);
    return at;
}

void draw_image_at(GfxRig& r, u32 rp, u32 image, i32 left, i32 top) {
    r.cpu().a[0] = rp;
    r.cpu().a[1] = image;
    r.cpu().d[0] = static_cast<u32>(left);
    r.cpu().d[1] = static_cast<u32>(top);
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::DrawImage), ExitReason::Halt);
}

void erase_image_at(GfxRig& r, u32 rp, u32 image, i32 left, i32 top) {
    r.cpu().a[0] = rp;
    r.cpu().a[1] = image;
    r.cpu().d[0] = static_cast<u32>(left);
    r.cpu().d[1] = static_cast<u32>(top);
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::EraseImage), ExitReason::Halt);
}

void draw_image_state_at(GfxRig& r, u32 rp, u32 image, i32 left, i32 top, u32 state,
                         u32 draw_info = 0) {
    r.cpu().a[0] = rp;
    r.cpu().a[1] = image;
    r.cpu().d[0] = static_cast<u32>(left);
    r.cpu().d[1] = static_cast<u32>(top);
    r.cpu().d[2] = state;
    r.cpu().a[2] = draw_info;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::DrawImageState), ExitReason::Halt);
}

void put_new_screen(GfxRig& r, u32 at, u32 width, u32 height, u32 depth, u32 detail,
                    u32 block, u32 title) {
    r.put16(at + ns_LeftEdge, 0);
    r.put16(at + ns_TopEdge, 0);
    r.put16(at + ns_Width, width);
    r.put16(at + ns_Height, height);
    r.put16(at + ns_Depth, depth);
    r.put8(at + ns_DetailPen, detail);
    r.put8(at + ns_BlockPen, block);
    r.put16(at + ns_ViewModes, 0);
    r.put16(at + ns_Type, CUSTOMSCREEN);
    r.put32(at + ns_Font, 0);
    r.put32(at + ns_DefaultTitle, title);
    r.put32(at + ns_CustomBitMap, 0);
}

u32 open_new(GfxRig& r, u32 new_screen) {
    r.cpu().a[0] = new_screen;
    return r.result(Base::Intuition, intuition_lvo::OpenScreen);
}

u32 open_tags(GfxRig& r, u32 new_screen, u32 tags) {
    r.cpu().a[0] = new_screen;
    r.cpu().a[1] = tags;
    return r.result(Base::Intuition, intuition_lvo::OpenScreenTagList);
}

void set_iprefs(GfxRig& r, u32 prefs, u32 size, u32 type) {
    r.cpu().a[0] = prefs;
    r.cpu().d[0] = size;
    r.cpu().d[1] = type;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::SetIPrefs), 0u);
}

// One type 8 palette record, then the terminator.
u32 put_palette_pref(GfxRig& r, u32 pen, u32 red, u32 green, u32 blue) {
    r.put16(kIPrefs, pen);
    r.put16(kIPrefs + 2, red);
    r.put16(kIPrefs + 4, green);
    r.put16(kIPrefs + 6, blue);
    r.put16(kIPrefs + 8, 0xFFFF);
    return 10;
}

// A type 9 header and the 13 words of the 3.1 pens, with bar_block in
// BARBLOCKPEN's place.
u32 put_pen_prefs(GfxRig& r, u32 kind, u32 bar_block) {
    const u32 pens[13] = {0, 1, 1, 2, 1, 3, 1, 0, 2, 1, bar_block, 1, 0xFFFF};
    r.put16(kIPrefs, 13);
    r.put16(kIPrefs + 2, kind);
    r.put32(kIPrefs + 4, 0);
    for (u32 i = 0; i < 13; ++i) {
        r.put16(kIPrefs + 8 + i * 2, pens[i]);
    }
    return 8 + 13 * 2;
}

// A struct IFontPrefs at prefs: the TextAttr, the name it points to and
// the word for what the font is.
u32 put_font_prefs(GfxRig& r, u32 prefs, const char* name, u32 y_size, u32 type) {
    r.put32(prefs + ta_Name, prefs + TextAttr_SIZEOF);
    r.put16(prefs + ta_YSize, y_size);
    r.put8(prefs + ta_Style, 0);
    r.put8(prefs + ta_Flags, 0);
    r.str(prefs + TextAttr_SIZEOF, name);
    r.put16(prefs + 44, type);
    return 46;
}

// The hasklig font of y_size, opened through the OpenFont vector and
// closed again: the TextFont address whose accessors the test reads.
u32 font_address(GfxRig& r, u32 y_size) {
    r.str(kBuffer + 0x40, "hasklig.font");
    r.put32(kBuffer + ta_Name, kBuffer + 0x40);
    r.put16(kBuffer + ta_YSize, y_size);
    r.put8(kBuffer + ta_Style, 0);
    r.put8(kBuffer + ta_Flags, 0);
    r.cpu().a[0] = kBuffer;
    const u32 font = r.result(Base::Graphics, graphics_lvo::OpenFont);
    CHECK(font != 0);
    r.cpu().a[1] = font;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::CloseFont), ExitReason::Halt);
    CHECK_EQ(r.get16(font + tf_Accessors), 0u);
    return font;
}

// What the Workbench hooks of CloseWorkBench and OpenWorkBench did.
struct WorkbenchHookState {
    u32 closes = 0;
    u32 opens = 0;
};

bool close_workbench_hook(void* ctx) {
    ++static_cast<WorkbenchHookState*>(ctx)->closes;
    return true;
}

void open_workbench_hook(void* ctx) {
    ++static_cast<WorkbenchHookState*>(ctx)->opens;
}

u32 screen_pixel(GfxRig& r, u32 screen, i32 x, i32 y) {
    r.cpu().a[1] = screen + sc_RastPort;
    r.cpu().d[0] = static_cast<u32>(x);
    r.cpu().d[1] = static_cast<u32>(y);
    return r.result(Base::Graphics, graphics_lvo::ReadPixel);
}

bool refreshed(GfxRig& r, u32 screen, i32 x0, i32 y0, i32 x1, i32 y1) {
    for (usize i = 0; i < r.refreshes.size(); ++i) {
        const Rect& d = r.refreshes[i];
        if (r.refreshed_screens[i] == screen && d.x0 == x0 && d.y0 == y0 && d.x1 == x1 &&
            d.y1 == y1) {
            return true;
        }
    }
    return false;
}

// The free heap in 4096-byte blocks, measured by allocating until it is
// exhausted and freeing the blocks again.
u32 heap_capacity(GfxRig& r) {
    std::vector<u32> blocks;
    for (;;) {
        const u32 at = r.exec.alloc(4096, MEMF_PUBLIC);
        if (at == 0) {
            break;
        }
        blocks.push_back(at);
    }
    const u32 count = static_cast<u32>(blocks.size());
    for (u32 at : blocks) {
        r.exec.free(at, 4096);
    }
    return count;
}

void put_new_window(GfxRig& r, u32 at, i32 left, i32 top, u32 width, u32 height, u32 flags,
                    u32 title, u32 screen, u32 type) {
    r.put16(at + nw_LeftEdge, static_cast<u32>(left));
    r.put16(at + nw_TopEdge, static_cast<u32>(top));
    r.put16(at + nw_Width, width);
    r.put16(at + nw_Height, height);
    r.put8(at + nw_DetailPen, 0);
    r.put8(at + nw_BlockPen, 1);
    r.put32(at + nw_IDCMPFlags, 0);
    r.put32(at + nw_Flags, flags);
    r.put32(at + nw_Title, title);
    r.put32(at + nw_Screen, screen);
    r.put16(at + nw_MinWidth, 0);
    r.put16(at + nw_MinHeight, 0);
    r.put16(at + nw_MaxWidth, 0);
    r.put16(at + nw_MaxHeight, 0);
    r.put16(at + nw_Type, type);
}

u32 open_win(GfxRig& r, u32 new_window) {
    r.cpu().a[0] = new_window;
    return r.result(Base::Intuition, intuition_lvo::OpenWindow);
}

u32 open_win_tags(GfxRig& r, u32 new_window, u32 tags) {
    r.cpu().a[0] = new_window;
    r.cpu().a[1] = tags;
    return r.result(Base::Intuition, intuition_lvo::OpenWindowTagList);
}

u32 window_pixel(GfxRig& r, u32 window, i32 x, i32 y) {
    r.cpu().a[1] = r.get32(window + wd_RPort);
    r.cpu().d[0] = static_cast<u32>(x);
    r.cpu().d[1] = static_cast<u32>(y);
    return r.result(Base::Graphics, graphics_lvo::ReadPixel);
}

std::vector<u8> screen_planes(GfxRig& r, u32 screen) {
    const u32 bitmap = screen + sc_BitMap;
    const u32 bpr = r.get16(bitmap + bm_BytesPerRow);
    const u32 rows = r.get16(bitmap + bm_Rows);
    const u32 bytes = bpr * rows;
    const u32 plane0 = r.get32(bitmap + bm_Planes);
    const u32 plane1 = r.get32(bitmap + bm_Planes + 4);
    std::vector<u8> out(bytes * 2);
    for (u32 i = 0; i < bytes; ++i) {
        out[i] = static_cast<u8>(r.get8(plane0 + i));
        out[bytes + i] = static_cast<u8>(r.get8(plane1 + i));
    }
    return out;
}

std::vector<u8> window_planes(GfxRig& r, u32 window) {
    const u32 rp = r.get32(window + wd_RPort);
    const u32 bitmap = r.get32(rp + rp_BitMap);
    const u32 bpr = r.get16(bitmap + bm_BytesPerRow);
    const u32 rows = r.get16(bitmap + bm_Rows);
    const u32 bytes = bpr * rows;
    std::vector<u8> out(bytes * 2);
    for (u32 plane = 0; plane < 2; ++plane) {
        const u32 base = r.get32(bitmap + bm_Planes + plane * 4);
        for (u32 i = 0; i < bytes; ++i) {
            out[plane * bytes + i] = static_cast<u8>(r.get8(base + i));
        }
    }
    return out;
}

bool changed_only_inside(const std::vector<u8>& before, const std::vector<u8>& after, u32 bpr,
                         u32 rows, i32 x0, i32 y0, i32 x1, i32 y1) {
    const u32 bytes = bpr * rows;
    const u32 bx0 = static_cast<u32>(x0) >> 3;
    const u32 bx1 = static_cast<u32>(x1) >> 3;
    for (u32 plane = 0; plane < 2; ++plane) {
        for (u32 row = 0; row < rows; ++row) {
            for (u32 byte = 0; byte < bpr; ++byte) {
                const u32 at = plane * bytes + row * bpr + byte;
                if (after[at] == before[at]) {
                    continue;
                }
                if (row < static_cast<u32>(y0) || row > static_cast<u32>(y1) || byte < bx0 ||
                    byte > bx1) {
                    return false;
                }
            }
        }
    }
    return true;
}

void screen_font_is_a_textattr_naming_topaz() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const u32 attr = r.get32(screen + sc_Font);
    CHECK(attr != 0);
    CHECK_EQ(r.get16(attr + ta_YSize), 8u);
    CHECK(r.read_str(r.get32(attr + ta_Name)) == "topaz.font");
}

void open_screen_builds_struct_bitmap_and_colormap() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    CHECK_EQ(r.get16(screen + sc_Width), 640u);
    CHECK_EQ(r.get16(screen + sc_Height), 400u);
    CHECK_EQ(r.get8(screen + sc_BarHeight), 14u);
    CHECK_EQ(r.get8(screen + sc_WBorTop), 6u);
    CHECK_EQ(r.get8(screen + sc_WBorLeft), 2u);
    CHECK_EQ(r.get8(screen + sc_WBorRight), 2u);
    CHECK_EQ(r.get8(screen + sc_WBorBottom), 2u);
    CHECK_EQ(r.get8(screen + sc_DetailPen), 0u);
    CHECK_EQ(r.get8(screen + sc_BlockPen), 1u);
    CHECK_EQ(r.get32(screen + sc_Title), 0u);

    const u32 bitmap = screen + sc_BitMap;
    CHECK_EQ(r.get8(bitmap + bm_Flags), 0u);
    CHECK_EQ(r.get8(bitmap + bm_Depth), 2u);
    CHECK_EQ(r.get16(bitmap + bm_BytesPerRow), 80u);
    CHECK_EQ(r.get16(bitmap + bm_Rows), 400u);
    CHECK(r.get32(bitmap + bm_Planes) != 0);
    CHECK(r.get32(bitmap + bm_Planes + 4) != 0);
    for (u32 plane = 2; plane < 8; ++plane) {
        CHECK_EQ(r.get32(bitmap + bm_Planes + plane * 4), 0u);
    }

    const u32 vp = screen + sc_ViewPort;
    const u32 map = r.get32(vp + vp_ColorMap);
    CHECK(map != 0);
    CHECK_EQ(r.get16(vp + vp_DWidth), 640u);
    CHECK_EQ(r.get16(vp + vp_DHeight), 400u);
    CHECK_EQ(r.get8(map + cm_Type), COLORMAP_TYPE_V39);
    CHECK_EQ(r.get16(map + cm_Count), 4u);
    const u32 colors = r.get32(map + cm_ColorTable);
    const u32 low_colors = r.get32(map + cm_LowColorBits);
    CHECK(colors != 0);
    CHECK(low_colors != 0);
    CHECK_EQ(r.get16(colors), 0x0AABu);
    CHECK_EQ(r.get16(colors + 2), 0x0112u);
    CHECK_EQ(r.get16(colors + 4), 0x0FFFu);
    CHECK_EQ(r.get16(colors + 6), 0x036Bu);
    CHECK_EQ(r.get16(low_colors), 0x0060u);
    CHECK_EQ(r.get16(low_colors + 2), 0x0CE2u);
    CHECK_EQ(r.get16(low_colors + 4), 0x0FFFu);
    CHECK_EQ(r.get16(low_colors + 6), 0x0DF6u);

    CHECK_EQ(r.intuition.front_screen(), screen);
    CHECK_EQ(r.display.front_screen, screen);
}

void open_screen_clears_and_draws_title_bar() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    // The bar is BARBLOCKPEN with BARTRIMPEN under it.
    CHECK_EQ(screen_pixel(r, screen, 5, 5), 2u);
    CHECK_EQ(screen_pixel(r, screen, 0, 0), 2u);
    CHECK_EQ(screen_pixel(r, screen, 639, 13), 2u);
    CHECK_EQ(screen_pixel(r, screen, 0, 14), 1u);
    CHECK_EQ(screen_pixel(r, screen, 5, 14), 1u);
    CHECK_EQ(screen_pixel(r, screen, 639, 14), 1u);
    CHECK_EQ(screen_pixel(r, screen, 5, 15), 0u);
    CHECK_EQ(screen_pixel(r, screen, 10, 300), 0u);
    CHECK_EQ(screen_pixel(r, screen, 639, 399), 0u);
}

void open_screen_makes_it_front_and_refreshes_the_whole_screen() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    CHECK_EQ(r.display.front_screen, screen);
    CHECK(!r.refreshes.empty());
    CHECK_EQ(r.refreshes[0].x0, 0);
    CHECK_EQ(r.refreshes[0].y0, 0);
    CHECK_EQ(r.refreshes[0].x1, 639);
    CHECK_EQ(r.refreshes[0].y1, 399);
    CHECK(refreshed(r, screen, 0, 0, 639, 399));
}

void open_screen_taglist_reads_sa_tags() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    r.str(kTitle, "H");
    const u32 tags = r.tags(kTagList, {{SA_Width, 320}, {SA_Height, 200},
                                       {SA_Title, kTitle}, {TAG_DONE, 0}});
    const u32 screen = open_tags(r, kNewScreen, tags);
    CHECK(screen != 0);
    CHECK_EQ(r.get16(screen + sc_Width), 320u);
    CHECK_EQ(r.get16(screen + sc_Height), 200u);
    CHECK_EQ(r.get8(screen + sc_BitMap + bm_Depth), 2u);
    CHECK_EQ(r.get16(screen + sc_BitMap + bm_BytesPerRow), 40u);

    // The title outlives the caller's string.
    const u32 title = r.get32(screen + sc_Title);
    CHECK(title != 0);
    CHECK(title != kTitle);
    CHECK(r.read_str(title) == "H");
    CHECK_EQ(r.get32(screen + sc_DefaultTitle), title);

    // The title is drawn in BARDETAILPEN (pen 1) over the bar in BARBLOCKPEN (pen 2).
    CHECK_EQ(screen_pixel(r, screen, 1, 3), 1u);
    CHECK_EQ(screen_pixel(r, screen, 2, 3), 1u);
    CHECK_EQ(screen_pixel(r, screen, 3, 3), 2u);
    CHECK(refreshed(r, screen, 0, 0, 319, 199));

    // A null NewScreen takes the tags and the defaults.
    const u32 short_tags = r.tags(kTagList, {{SA_Width, 100}, {SA_Height, 50}, {TAG_DONE, 0}});
    const u32 second = open_tags(r, 0, short_tags);
    CHECK(second != 0);
    CHECK_EQ(r.get16(second + sc_Width), 100u);
    CHECK_EQ(r.get16(second + sc_Height), 50u);
    CHECK_EQ(r.get8(second + sc_BitMap + bm_Depth), 1u);
    CHECK_EQ(r.display.front_screen, second);
}

constexpr u32 kViewHires = 0x8000;
constexpr u32 kViewLace = 0x0004;
constexpr u32 kModeHiresLace = 0x8004;
constexpr u32 kModePalLowRes = 0x21000;
constexpr u32 kModePalSuperLace = 0x29024;
constexpr u32 kModePalEhb = 0x21080;
constexpr u32 kModeUnknown = 0x31004;
constexpr u32 kStdSize = 0xFFFFFFFF;
constexpr u32 kErrorCode = kBuffer;
constexpr u32 kDClip = kBuffer + 0x20;

u32 vp_mode_id(GfxRig& r, u32 screen) {
    r.cpu().a[0] = screen + sc_ViewPort;
    return r.result(Base::Graphics, graphics_lvo::GetVPModeID);
}

void open_screen_view_modes_choose_the_default_monitor_mode() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 0, 0, 2, 0, 1, 0);
    r.put16(kNewScreen + ns_ViewModes, kViewHires | kViewLace);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    CHECK_EQ(r.get16(screen + sc_Width), 640u);
    CHECK_EQ(r.get16(screen + sc_Height), 400u);
    CHECK_EQ(r.get16(screen + sc_ViewPort + vp_Modes), kModeHiresLace);
    CHECK_EQ(vp_mode_id(r, screen), kModeHiresLace);
    CHECK_EQ(r.intuition.screen_mode_id(screen), kModeHiresLace);

    // Without mode bits a NewScreen that is not 640x400 is Low Res.
    put_new_screen(r, kNewScreen, 320, 200, 2, 0, 1, 0);
    const u32 low = open_new(r, kNewScreen);
    CHECK(low != 0);
    CHECK_EQ(r.get16(low + sc_ViewPort + vp_Modes), 0u);
    CHECK_EQ(r.intuition.screen_mode_id(low), 0u);
    CHECK_EQ(r.get16(low + sc_ViewPort + vp_DWidth), 320u);
}

void open_screen_standard_size_without_view_modes_keeps_workbench_mode() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 0, 0, 5, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    CHECK_EQ(r.get16(screen + sc_Width), 640u);
    CHECK_EQ(r.get16(screen + sc_Height), 400u);
    CHECK_EQ(r.get8(screen + sc_BitMap + bm_Depth), 5u);
    CHECK_EQ(r.get16(screen + sc_ViewPort + vp_Modes), kModeHiresLace);
    CHECK_EQ(r.intuition.screen_mode_id(screen), kModeHiresLace);
}

void open_screen_display_id_gives_the_modes_size() {
    GfxRig r;
    const u32 tags = r.tags(kTagList, {{SA_DisplayID, kModePalLowRes}, {SA_Width, kStdSize},
                                       {SA_Height, kStdSize}, {SA_Depth, 5}, {TAG_DONE, 0}});
    const u32 screen = open_tags(r, 0, tags);
    CHECK(screen != 0);
    CHECK_EQ(r.get16(screen + sc_Width), 320u);
    CHECK_EQ(r.get16(screen + sc_Height), 256u);
    CHECK_EQ(r.get8(screen + sc_BitMap + bm_Depth), 5u);
    CHECK_EQ(r.get16(screen + sc_ViewPort + vp_DWidth), 320u);
    CHECK_EQ(r.get16(screen + sc_ViewPort + vp_DHeight), 256u);
    CHECK_EQ(r.get16(screen + sc_ViewPort + vp_Modes), 0u);
    CHECK_EQ(r.intuition.screen_mode_id(screen), kModePalLowRes);
    CHECK_EQ(vp_mode_id(r, screen), kModePalLowRes);

    r.cpu().a[0] = screen;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseScreen), ExitReason::Halt);
    CHECK_EQ(r.intuition.screen_mode_id(screen), Intuition::kInvalidModeId);
    r.cpu().a[0] = screen + sc_ViewPort;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::GetVPModeID), ExitReason::Halt);
}

void open_screen_display_id_of_a_super_hires_mode() {
    GfxRig r;
    const u32 tags = r.tags(kTagList, {{SA_DisplayID, kModePalSuperLace}, {SA_Depth, 2},
                                       {TAG_DONE, 0}});
    const u32 screen = open_tags(r, 0, tags);
    CHECK(screen != 0);
    CHECK_EQ(r.get16(screen + sc_Width), 1280u);
    CHECK_EQ(r.get16(screen + sc_Height), 512u);
    CHECK_EQ(r.get16(screen + sc_ViewPort + vp_Modes), 0x8024u);
    CHECK_EQ(vp_mode_id(r, screen), kModePalSuperLace);
}

void display_mode_geometry_follows_the_front_screen() {
    GfxRig r;
    CHECK_EQ(r.display.mode_width, 640u);
    CHECK_EQ(r.display.mode_height, 400u);
    CHECK_EQ(r.display.mode_flags, 0u);

    put_new_screen(r, kNewScreen, 0, 0, 2, 0, 1, 0);
    r.put16(kNewScreen + ns_ViewModes, kViewHires | kViewLace);
    const u32 hires = open_new(r, kNewScreen);
    CHECK_EQ(r.display.mode_width, 640u);
    CHECK_EQ(r.display.mode_height, 400u);
    CHECK_EQ(r.display.mode_flags, display_modes::find(kModeHiresLace)->flags);

    const u32 tags = r.tags(kTagList, {{SA_DisplayID, kModePalSuperLace}, {SA_Depth, 2},
                                       {TAG_DONE, 0}});
    const u32 super = open_tags(r, 0, tags);
    CHECK(hires != 0 && super != 0);
    CHECK_EQ(r.display.mode_width, 1280u);
    CHECK_EQ(r.display.mode_height, 512u);
    CHECK_EQ(r.display.mode_flags, display_modes::find(kModePalSuperLace)->flags);

    r.intuition.screen_to_front(hires);
    CHECK_EQ(r.display.mode_width, 640u);
    CHECK_EQ(r.display.mode_height, 400u);

    r.intuition.screen_to_front(super);
    CHECK_EQ(r.display.mode_width, 1280u);

    // The mode of the screen that comes to the front when the front one closes.
    r.cpu().a[0] = super;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseScreen), ExitReason::Halt);
    CHECK_EQ(r.display.mode_width, 640u);
    CHECK_EQ(r.display.mode_height, 400u);

    r.cpu().a[0] = hires;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseScreen), ExitReason::Halt);
    CHECK_EQ(r.display.mode_width, 640u);
    CHECK_EQ(r.display.mode_flags, 0u);
}

void open_screen_dclip_sets_the_standard_size() {
    GfxRig r;
    r.put16(kDClip + ra_MinX, 0);
    r.put16(kDClip + ra_MinY, 0);
    r.put16(kDClip + ra_MaxX, 359);
    r.put16(kDClip + ra_MaxY, 279);
    const u32 tags = r.tags(kTagList, {{SA_DisplayID, kModePalLowRes}, {SA_DClip, kDClip},
                                       {SA_Width, kStdSize}, {SA_Height, kStdSize},
                                       {SA_Depth, 2}, {TAG_DONE, 0}});
    const u32 screen = open_tags(r, 0, tags);
    CHECK(screen != 0);
    CHECK_EQ(r.get16(screen + sc_Width), 360u);
    CHECK_EQ(r.get16(screen + sc_Height), 280u);
    CHECK_EQ(r.get16(screen + sc_ViewPort + vp_DWidth), 320u);
    CHECK_EQ(r.get16(screen + sc_ViewPort + vp_DHeight), 256u);
}

void open_screen_too_deep_or_unknown_mode_fails_with_an_error_code() {
    GfxRig r;
    r.put32(kErrorCode, 0);
    u32 tags = r.tags(kTagList, {{SA_DisplayID, kModePalEhb}, {SA_Depth, 7},
                                 {SA_ErrorCode, kErrorCode}, {TAG_DONE, 0}});
    CHECK_EQ(open_tags(r, 0, tags), 0u);
    CHECK_EQ(r.get32(kErrorCode), OSERR_TOODEEP);

    tags = r.tags(kTagList, {{SA_DisplayID, kModeUnknown}, {SA_ErrorCode, kErrorCode},
                             {TAG_DONE, 0}});
    CHECK_EQ(open_tags(r, 0, tags), 0u);
    CHECK_EQ(r.get32(kErrorCode), OSERR_UNKNOWNMODE);
    CHECK_EQ(r.intuition.front_screen(), 0u);

    // The caller may leave out SA_ErrorCode.
    tags = r.tags(kTagList, {{SA_DisplayID, kModeUnknown}, {TAG_DONE, 0}});
    CHECK_EQ(open_tags(r, 0, tags), 0u);
}

void open_screen_null_newscreen_and_taglist_gives_zero_and_faults() {
    GfxRig r;
    r.cpu().a[0] = 0;
    r.cpu().a[1] = 0;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::OpenScreenTagList), ExitReason::BusError);
    CHECK_EQ(r.cpu().fault_addr, kTestGuestBytes);
    CHECK_EQ(r.intuition.front_screen(), 0u);
}

void open_screen_out_of_memory_returns_zero_and_frees() {
    GfxRig r;
    std::vector<u32> blocks;
    for (;;) {
        const u32 at = r.exec.alloc(4096, MEMF_PUBLIC);
        if (at == 0) {
            break;
        }
        blocks.push_back(at);
    }
    CHECK(!blocks.empty());
    const u32 hole = blocks[0];
    r.exec.free(hole, 4096);

    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK_EQ(open_new(r, kNewScreen), 0u);
    CHECK(logged("intuition: no memory for a screen"));

    // The screen allocation the failed open made came back.
    const u32 restored = r.exec.alloc(4096, MEMF_PUBLIC);
    CHECK(restored != 0);
    r.exec.free(restored, 4096);
    for (u32 at : blocks) {
        if (at != hole) {
            r.exec.free(at, 4096);
        }
    }
}

void open_screen_limit_returns_zero() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 64, 64, 2, 0, 1, 0);
    u32 front = 0;
    for (u32 i = 0; i < Intuition::kMaxScreens; ++i) {
        front = open_new(r, kNewScreen);
        CHECK(front != 0);
    }
    CHECK_EQ(r.display.front_screen, front);
    CHECK_EQ(open_new(r, kNewScreen), 0u);
    CHECK(logged("intuition: screen limit reached"));
    CHECK_EQ(r.display.front_screen, front);
}

void more_than_four_screens_open_in_depth_order() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 64, 64, 2, 0, 1, 0);
    std::vector<u32> screens;
    for (u32 i = 0; i < 10; ++i) {
        screens.push_back(open_new(r, kNewScreen));
        CHECK(screens.back() != 0);
    }
    CHECK_EQ(r.intuition.front_screen(), screens.back());
    // Each screen opens in front: the one behind it is its NextScreen.
    for (usize i = 1; i < screens.size(); ++i) {
        CHECK_EQ(r.get32(screens[i]), screens[i - 1]);
    }
    CHECK_EQ(r.get32(screens[0]), 0u);

    r.intuition.screen_to_front(screens[3]);
    CHECK_EQ(r.intuition.front_screen(), screens[3]);
    CHECK_EQ(r.get32(screens[3]), screens[9]);
    CHECK_EQ(r.get32(screens[4]), screens[2]);

    for (u32 screen : screens) {
        CHECK(r.intuition.close_screen(screen));
    }
    CHECK_EQ(r.intuition.front_screen(), 0u);
}

void close_screen_twice_and_null_are_ignored() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 64, 64, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.cpu().a[0] = screen;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseScreen), ExitReason::Halt);
    const usize refreshes = r.refreshes.size();
    CHECK_EQ(r.intuition.front_screen(), 0u);
    CHECK_EQ(r.display.front_screen, 0u);
    CHECK_EQ(r.refreshed_screens.back(), 0u);
    CHECK_EQ(r.refreshes.back().x1, 63);

    r.cpu().a[0] = screen;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseScreen), ExitReason::Halt);
    r.cpu().a[0] = 0;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseScreen), ExitReason::Halt);
    CHECK_EQ(r.refreshes.size(), refreshes);
    CHECK_EQ(r.intuition.front_screen(), 0u);
}

void close_front_screen_promotes_next() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 64, 64, 2, 0, 1, 0);
    const u32 first = open_new(r, kNewScreen);
    const u32 second = open_new(r, kNewScreen);
    CHECK(first != 0 && second != 0);
    CHECK_EQ(r.display.front_screen, second);
    CHECK_EQ(r.get32(second), first);
    CHECK_EQ(r.get32(first), 0u);

    const usize before = r.refreshes.size();
    r.cpu().a[0] = second;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseScreen), ExitReason::Halt);
    CHECK_EQ(r.intuition.front_screen(), first);
    CHECK_EQ(r.display.front_screen, first);
    CHECK_EQ(r.get32(first), 0u);
    CHECK_EQ(r.refreshes.size(), before + 1);
    CHECK_EQ(r.refreshed_screens.back(), first);
    CHECK_EQ(r.refreshes.back().x0, 0);
    CHECK_EQ(r.refreshes.back().x1, 63);

    r.cpu().a[0] = first;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseScreen), ExitReason::Halt);
    CHECK_EQ(r.intuition.front_screen(), 0u);
    CHECK_EQ(r.display.front_screen, 0u);
}

void reopen_after_close_reuses_memory() {
    GfxRig r;
    const u32 before = heap_capacity(r);
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    u32 screens[4] = {0, 0, 0, 0};
    for (u32& screen : screens) {
        screen = open_new(r, kNewScreen);
        CHECK(screen != 0);
    }
    for (u32 screen : screens) {
        r.cpu().a[0] = screen;
        CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseScreen), ExitReason::Halt);
    }
    CHECK_EQ(r.intuition.front_screen(), 0u);
    CHECK_EQ(heap_capacity(r), before);

    const u32 again = open_new(r, kNewScreen);
    CHECK(again != 0);
    r.cpu().a[0] = again;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseScreen), ExitReason::Halt);
}

void get_screen_data_copies_struct() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 100, 50, 2, 3, 4, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);

    r.cpu().a[0] = kBuffer;
    r.cpu().d[0] = Screen_SIZEOF;
    r.cpu().d[1] = CUSTOMSCREEN;
    r.cpu().a[1] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::GetScreenData), 1u);
    CHECK_EQ(r.get16(kBuffer + sc_Width), 100u);
    CHECK_EQ(r.get16(kBuffer + sc_Height), 50u);
    CHECK_EQ(r.get8(kBuffer + sc_DetailPen), 3u);
    CHECK_EQ(r.get8(kBuffer + sc_BlockPen), 4u);
    CHECK_EQ(r.get8(kBuffer + sc_BitMap + bm_Depth), 2u);

    // WBENCHSCREEN also names the front screen.
    r.cpu().a[0] = kBuffer;
    r.cpu().d[0] = 4;
    r.cpu().d[1] = 1;
    r.cpu().a[1] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::GetScreenData), 1u);
    CHECK_EQ(r.get16(kBuffer), 0u);

    r.cpu().a[0] = kBuffer;
    r.cpu().d[0] = 4;
    r.cpu().d[1] = CUSTOMSCREEN;
    r.cpu().a[1] = 0x123456;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::GetScreenData), 0u);

    r.cpu().a[0] = kBuffer;
    r.cpu().d[0] = 4;
    r.cpu().d[1] = 12345;
    r.cpu().a[1] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::GetScreenData), 0u);
}

u32 colormap_rgb24(const GfxRig& r, u32 map, u32 pen) {
    const u32 hi = r.get16(r.get32(map + cm_ColorTable) + pen * 2);
    const u32 lo = r.get16(r.get32(map + cm_LowColorBits) + pen * 2);
    const u32 red = (((hi >> 8) & 0xF) << 4) | ((lo >> 8) & 0xF);
    const u32 green = (((hi >> 4) & 0xF) << 4) | ((lo >> 4) & 0xF);
    const u32 blue = ((hi & 0xF) << 4) | (lo & 0xF);
    return (red << 16) | (green << 8) | blue;
}

void open_screen_honours_depth_and_allocates_planes() {
    GfxRig r;
    const u32 before = heap_capacity(r);
    // A 128x64 screen without mode bits is Low Res: depth 0 gives one
    // plane, and more planes than the mode has fail.
    put_new_screen(r, kNewScreen, 128, 64, 9, 0, 1, 0);
    CHECK_EQ(open_new(r, kNewScreen), 0u);
    const u32 requests[] = {8, 3, 0};
    const u32 expected[] = {8, 3, 1};
    for (u32 idx = 0; idx < 3; ++idx) {
        put_new_screen(r, kNewScreen, 128, 64, requests[idx], 0, 1, 0);
        const u32 screen = open_new(r, kNewScreen);
        CHECK(screen != 0);
        const u32 want = expected[idx];
        CHECK_EQ(r.get8(screen + sc_BitMap + bm_Depth), want);
        for (u32 plane = 0; plane < 8; ++plane) {
            const u32 ptr = r.get32(screen + sc_BitMap + bm_Planes + plane * 4);
            if (plane < want) {
                CHECK(ptr != 0);
            } else {
                CHECK_EQ(ptr, 0u);
            }
        }
        r.cpu().a[0] = kBuffer;
        r.cpu().d[0] = Screen_SIZEOF;
        r.cpu().d[1] = CUSTOMSCREEN;
        r.cpu().a[1] = screen;
        CHECK_EQ(r.result(Base::Intuition, intuition_lvo::GetScreenData), 1u);
        CHECK_EQ(r.get8(kBuffer + sc_BitMap + bm_Depth), want);

        put_new_window(r, kNewWindow, 0, 0, 64, 32, 0, 0, screen, CUSTOMSCREEN);
        const u32 win = open_win(r, kNewWindow);
        CHECK(win != 0);
        const u32 win_bm = r.get32(r.get32(win + wd_RPort) + rp_BitMap);
        CHECK_EQ(r.get8(win_bm + bm_Depth), want);
        for (u32 plane = 0; plane < 8; ++plane) {
            const u32 ptr = r.get32(win_bm + bm_Planes + plane * 4);
            if (plane < want) {
                CHECK(ptr != 0);
            } else {
                CHECK_EQ(ptr, 0u);
            }
        }
        r.cpu().a[0] = win;
        CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);
        r.cpu().a[0] = screen;
        CHECK_EQ(r.result(Base::Intuition, intuition_lvo::CloseScreen), 1u);
        CHECK_EQ(heap_capacity(r), before);
    }
}

void open_screen_depth8_sets_default_and_chrome_palettes() {
    GfxRig r;
    const u32 before = heap_capacity(r);
    put_new_screen(r, kNewScreen, 128, 64, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);

    const u32 map = r.get32(screen + sc_ViewPort + vp_ColorMap);
    CHECK(map != 0);
    CHECK_EQ(r.get8(map + cm_Type), COLORMAP_TYPE_V39);
    CHECK_EQ(r.get16(map + cm_Count), 256u);
    CHECK(r.get32(map + cm_ColorTable) != 0);
    CHECK(r.get32(map + cm_LowColorBits) != 0);

    CHECK_EQ(colormap_rgb24(r, map, 0), 0xA0A6B0u);
    CHECK_EQ(colormap_rgb24(r, map, 1), 0x1C1E22u);
    CHECK_EQ(colormap_rgb24(r, map, 2), 0xFFFFFFu);
    CHECK_EQ(colormap_rgb24(r, map, 3), 0x3D6FB6u);
    CHECK_EQ(colormap_rgb24(r, map, 4), 0x7A7E86u);
    CHECK_EQ(colormap_rgb24(r, map, 5), 0xC9CDD4u);
    CHECK_EQ(colormap_rgb24(r, map, 6), 0xD9A441u);
    CHECK_EQ(colormap_rgb24(r, map, 7), 0x9E3B3Bu);

    CHECK_EQ(colormap_rgb24(r, map, 8), 0x000000u);
    CHECK_EQ(colormap_rgb24(r, map, 8 + 1 * 36 + 2 * 6 + 3), 0x336699u);
    CHECK_EQ(colormap_rgb24(r, map, 8 + 5 * 36 + 3 * 6 + 1), 0xFF9933u);
    CHECK_EQ(colormap_rgb24(r, map, chrome::kScreenBar0 - 1), 0xFFFF33u);

    // The screen bar shades white down to a light grey.
    CHECK_EQ(colormap_rgb24(r, map, chrome::kScreenBar0), 0xFFFFFFu);
    CHECK_EQ(colormap_rgb24(r, map, chrome::kScreenBar3), chrome::screen_bar_rgb(0xFFFFFF, 3));
    CHECK(colormap_rgb24(r, map, chrome::kScreenBar3) < 0xE0E0E0u);

    for (u32 i = 0; i < 4; ++i) {
        CHECK_EQ(colormap_rgb24(r, map, chrome::kMenuPaperPen + i), chrome::kMenuPalette[i]);
    }

    // The chrome colours are worked out from the default DrawInfo pens: the
    // bars shade FILLPEN (3) and BACKGROUNDPEN (0), the frame takes
    // SHADOWPEN (1) and SHINEPEN (2), and the close stud keeps its red.
    const u32 active[8] = {0x779ACC, 0x6C8FC1, 0x6184B6, 0x5679AB,
                           0x4C6FA0, 0x416495, 0x36598A, 0x2B4E7F};
    for (u32 i = 0; i < 8; ++i) {
        CHECK_EQ(colormap_rgb24(r, map, chrome::kActiveBar0 + i), active[i]);
    }
    const u32 inactive[6] = {0xAEB3BC, 0xA8ADB6, 0xA2A7B0, 0x9CA1AA, 0x969BA4, 0x90959E};
    for (u32 i = 0; i < 6; ++i) {
        CHECK_EQ(colormap_rgb24(r, map, chrome::kInactiveBar0 + i), inactive[i]);
    }
    CHECK_EQ(colormap_rgb24(r, map, chrome::kFrameDark), 0x1C1E22u);
    CHECK_EQ(colormap_rgb24(r, map, chrome::kFrameLight), 0xFFFFFFu);
    CHECK_EQ(colormap_rgb24(r, map, chrome::kCloseFill), 0xE0605Au);
    CHECK_EQ(colormap_rgb24(r, map, chrome::kCloseRing), 0x9A3330u);
    CHECK_EQ(colormap_rgb24(r, map, chrome::kKnob0), 0xC6CAD0u);
    CHECK_EQ(colormap_rgb24(r, map, chrome::kKnob1), 0xB3B8C0u);
    CHECK_EQ(colormap_rgb24(r, map, chrome::kKnob2), 0xA0A6B0u);
    CHECK_EQ(colormap_rgb24(r, map, chrome::kTrack), 0x787D84u);
    CHECK_EQ(colormap_rgb24(r, map, chrome::kTitleActive), 0x1C1E22u);
    CHECK_EQ(colormap_rgb24(r, map, chrome::kTitleInactive), 0x1C1E22u);

    r.cpu().a[0] = screen;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::CloseScreen), 1u);
    CHECK_EQ(heap_capacity(r), before);

    put_new_screen(r, kNewScreen, 128, 64, 3, 0, 1, 0);
    const u32 depth3 = open_new(r, kNewScreen);
    CHECK(depth3 != 0);
    const u32 map3 = r.get32(depth3 + sc_ViewPort + vp_ColorMap);
    CHECK_EQ(r.get16(map3 + cm_Count), 8u);
    CHECK_EQ(colormap_rgb24(r, map3, 0), 0xA0A6B0u);
    CHECK_EQ(colormap_rgb24(r, map3, 7), 0x9E3B3Bu);
    r.cpu().a[0] = depth3;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::CloseScreen), 1u);
    CHECK_EQ(heap_capacity(r), before);
}

void unimplemented_vector_ends_run() {
    GfxRig r;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::Last), ExitReason::Unimplemented);
    CHECK(logged("intuition: unimplemented LVO -828"));
    CHECK_EQ(r.cpu().pc, r.intuition.intuition_base() - intuition_lvo::Last);
}

void open_window_builds_struct_rastport_and_layer() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.get16(window + wd_LeftEdge), 50u);
    CHECK_EQ(r.get16(window + wd_TopEdge), 60u);
    CHECK_EQ(r.get16(window + wd_Width), 200u);
    CHECK_EQ(r.get16(window + wd_Height), 100u);
    CHECK_EQ(r.get32(window + wd_WScreen), screen);
    CHECK_EQ(r.get8(window + wd_BorderLeft), 2u);
    CHECK_EQ(r.get8(window + wd_BorderTop), 15u);
    CHECK_EQ(r.get8(window + wd_BorderRight), 2u);
    CHECK_EQ(r.get8(window + wd_BorderBottom), 2u);
    CHECK_EQ(r.get32(screen + sc_FirstWindow), window);
    CHECK_EQ(r.get32(window + wd_NextWindow), 0u);

    const u32 rp = r.get32(window + wd_RPort);
    const u32 layer = r.get32(window + wd_WLayer);
    CHECK(rp != 0 && layer != 0);
    const u32 win_bm = r.get32(rp + rp_BitMap);
    CHECK(win_bm != 0);
    CHECK(win_bm != screen + sc_BitMap);
    CHECK_EQ(r.get32(layer + ly_SuperBitMap), win_bm);
    CHECK_EQ(r.get32(rp + rp_Layer), layer);
    CHECK_EQ(r.get8(rp + rp_FgPen), 1u);
    CHECK_EQ(r.get8(rp + rp_BgPen), 0u);
    CHECK_EQ(r.get8(rp + rp_DrawMode), JAM2);
    CHECK_EQ(r.get16(rp + rp_cp_x), 0u);
    CHECK_EQ(r.get16(rp + rp_cp_y), 6u);
    CHECK_EQ(r.get32(layer + ly_rp), rp);
    CHECK_EQ(r.get16(layer + ly_MinX), 50u);
    CHECK_EQ(r.get16(layer + ly_MinY), 60u);
    CHECK_EQ(r.get16(layer + ly_MaxX), 249u);
    CHECK_EQ(r.get16(layer + ly_MaxY), 159u);
    CHECK_EQ(r.get32(layer + ly_Window), window);
    CHECK_EQ(window_pixel(r, window, 0, 0), 2u);
    CHECK_EQ(window_pixel(r, window, -1, 0), 0xFFFFFFFFu);
}

void open_window_draws_chrome() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.str(kWinTitle, "H");
    put_new_window(r, kNewWindow, 50, 60, 200, 100, WFLG_CLOSEGADGET | WFLG_DEPTHGADGET,
                   kWinTitle, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.get32(window + wd_Flags),
             static_cast<u32>(WFLG_CLOSEGADGET | WFLG_DEPTHGADGET));

    // The raised edge round the window: SHINEPEN (2) top and left, SHADOWPEN
    // (1) bottom and right, the right one owning the top right corner.
    CHECK_EQ(screen_pixel(r, screen, 50, 60), 2u);
    CHECK_EQ(screen_pixel(r, screen, 249, 60), 1u);
    CHECK_EQ(screen_pixel(r, screen, 50, 159), 2u);
    CHECK_EQ(screen_pixel(r, screen, 50, 80), 2u);
    CHECK_EQ(screen_pixel(r, screen, 249, 159), 1u);
    // The inactive bar is BACKGROUNDPEN (0) over the line that tops the
    // sunken edge round the inside.
    CHECK_EQ(screen_pixel(r, screen, 100, 65), 0u);
    CHECK_EQ(screen_pixel(r, screen, 100, 74), 1u);
    CHECK_EQ(screen_pixel(r, screen, 100, 75), 0u);
    CHECK_EQ(screen_pixel(r, screen, 51, 100), 1u);
    CHECK_EQ(screen_pixel(r, screen, 248, 100), 2u);
    CHECK_EQ(screen_pixel(r, screen, 100, 158), 2u);
    // The close image: the groove that ends its raised cell, its body and
    // the square etched in the middle, dark on the left and light on the
    // right.
    CHECK_EQ(screen_pixel(r, screen, 65, 67), 1u);
    CHECK_EQ(screen_pixel(r, screen, 66, 67), 2u);
    CHECK_EQ(screen_pixel(r, screen, 53, 67), 0u);
    CHECK_EQ(screen_pixel(r, screen, 54, 67), 1u);
    CHECK_EQ(screen_pixel(r, screen, 57, 67), 0u);
    CHECK_EQ(screen_pixel(r, screen, 60, 67), 2u);
    // The depth image: the groove that starts its raised cell, the dark
    // sheet behind and the light one in front, the body inside both.
    CHECK_EQ(screen_pixel(r, screen, 234, 67), 1u);
    CHECK_EQ(screen_pixel(r, screen, 235, 67), 2u);
    CHECK_EQ(screen_pixel(r, screen, 239, 64), 1u);
    CHECK_EQ(screen_pixel(r, screen, 240, 65), 0u);
    CHECK_EQ(screen_pixel(r, screen, 241, 66), 2u);
    CHECK_EQ(screen_pixel(r, screen, 242, 67), 0u);
    CHECK_EQ(screen_pixel(r, screen, 69, 63), 1u);

    // Without the close gadget the bar has no button in that corner.
    put_new_window(r, kNewWindow, 300, 60, 200, 100, 0, 0, 0, 0);
    const u32 plain = open_win(r, kNewWindow);
    CHECK(plain != 0);
    CHECK_EQ(screen_pixel(r, screen, 308, 62), 0u);
    CHECK_EQ(screen_pixel(r, screen, 301, 65), 0u);
    CHECK_EQ(screen_pixel(r, screen, 300, 60), 2u);
}

void open_window_default_fills_the_screen_below_the_bar() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const u32 bar = r.get8(screen + sc_BarHeight);
    CHECK(bar > 0);
    put_new_window(r, kNewWindow, 0, 0, 0, 0, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.get16(window + wd_LeftEdge), 0u);
    CHECK_EQ(r.get16(window + wd_TopEdge), bar);
    CHECK_EQ(r.get16(window + wd_Width), 640u);
    CHECK_EQ(r.get16(window + wd_Height), 400u - bar);
    const u32 layer = r.get32(window + wd_WLayer);
    CHECK_EQ(r.get16(layer + ly_MinX), 0u);
    CHECK_EQ(r.get16(layer + ly_MinY), bar);
    CHECK_EQ(r.get16(layer + ly_MaxX), 639u);
    CHECK_EQ(r.get16(layer + ly_MaxY), 399u);
    CHECK_EQ(screen_pixel(r, screen, 0, bar), 2u);
}

void windows_open_below_the_screen_bar() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const u32 bar = r.get8(screen + sc_BarHeight);

    // Asked for over the bar, the window opens just below it.
    put_new_window(r, kNewWindow, 20, 3, 200, 100, 0, 0, 0, 0);
    const u32 low = open_win(r, kNewWindow);
    CHECK_EQ(r.get16(low + wd_LeftEdge), 20u);
    CHECK_EQ(r.get16(low + wd_TopEdge), bar);
    CHECK_EQ(r.get16(low + wd_Height), 100u);

    // Below the bar it stays where it asked.
    put_new_window(r, kNewWindow, 20, 50, 200, 100, 0, 0, 0, 0);
    const u32 placed = open_win(r, kNewWindow);
    CHECK_EQ(r.get16(placed + wd_TopEdge), 50u);

    // Too tall for the room below the bar, it moves only as far as it
    // still fits.
    put_new_window(r, kNewWindow, 0, 0, 200, 395, 0, 0, 0, 0);
    const u32 tall = open_win(r, kNewWindow);
    CHECK_EQ(r.get16(tall + wd_TopEdge), 5u);
    CHECK_EQ(r.get16(tall + wd_Height), 395u);

    // A backdrop stays where it asked.
    put_new_window(r, kNewWindow, 0, 0, 200, 100, WFLG_BACKDROP, 0, 0, 0);
    const u32 backdrop = open_win(r, kNewWindow);
    CHECK_EQ(r.get16(backdrop + wd_TopEdge), 0u);
}

void open_window_taglist_reads_wa_tags() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 first = open_new(r, kNewScreen);
    const u32 second = open_new(r, kNewScreen);
    CHECK(first != 0 && second != 0);
    CHECK_EQ(r.display.front_screen, second);

    r.str(kWinTitle, "B");
    put_new_window(r, kNewWindow, 10, 20, 100, 50, 0, 0, second, CUSTOMSCREEN);
    const u32 tags =
        r.tags(kTagList, {{WA_Left, 30}, {WA_Top, 40}, {WA_Width, 200}, {WA_Height, 120},
                          {WA_Flags, WFLG_CLOSEGADGET}, {WA_Title, kWinTitle},
                          {WA_CustomScreen, first}, {TAG_DONE, 0}});
    const u32 window = open_win_tags(r, kNewWindow, tags);
    CHECK(window != 0);
    CHECK_EQ(r.get16(window + wd_LeftEdge), 30u);
    CHECK_EQ(r.get16(window + wd_TopEdge), 40u);
    CHECK_EQ(r.get16(window + wd_Width), 200u);
    CHECK_EQ(r.get16(window + wd_Height), 120u);
    CHECK_EQ(r.get32(window + wd_WScreen), first);
    CHECK((r.get32(window + wd_Flags) & WFLG_CLOSEGADGET) != 0);
    const u32 title = r.get32(window + wd_Title);
    CHECK(title != 0);
    CHECK(title != kWinTitle);
    CHECK(r.read_str(title) == "B");

    // A null NewWindow takes the tags and the front screen.
    const u32 short_tags = r.tags(kTagList, {{WA_Left, 5}, {WA_Top, 5}, {WA_Width, 64},
                                             {WA_Height, 32}, {TAG_DONE, 0}});
    const u32 plain = open_win_tags(r, 0, short_tags);
    CHECK(plain != 0);
    CHECK_EQ(r.get16(plain + wd_Width), 64u);
    CHECK_EQ(r.get16(plain + wd_Height), 32u);
    CHECK_EQ(r.get32(plain + wd_WScreen), second);
}

void open_window_on_unknown_screen_gives_zero() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 10, 20, 100, 50, 0, 0, 0x123456, CUSTOMSCREEN);
    CHECK_EQ(open_win(r, kNewWindow), 0u);
    CHECK(logged("intuition: window without a screen"));
    CHECK_EQ(r.get32(screen + sc_FirstWindow), 0u);
}

void open_window_null_newwindow_gives_zero_and_faults() {
    GfxRig r;
    r.cpu().a[0] = 0;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::OpenWindow), ExitReason::BusError);
    CHECK_EQ(r.cpu().fault_addr, kTestGuestBytes);

    GfxRig other;
    other.cpu().a[0] = 0;
    other.cpu().a[1] = 0;
    CHECK_EQ(other.call(Base::Intuition, intuition_lvo::OpenWindowTagList),
             ExitReason::BusError);
    CHECK_EQ(other.cpu().fault_addr, kTestGuestBytes);
}

void open_window_alloc_failure_gives_zero_and_frees() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);

    std::vector<u32> big;
    for (;;) {
        const u32 at = r.exec.alloc(4096, MEMF_PUBLIC);
        if (at == 0) {
            break;
        }
        big.push_back(at);
    }
    CHECK(!big.empty());
    std::vector<u32> tail;
    for (;;) {
        const u32 at = r.exec.alloc(160, MEMF_PUBLIC);
        if (at == 0) {
            break;
        }
        tail.push_back(at);
    }
    const u32 carved_from = big[0];
    r.exec.free(carved_from, 4096);
    const u32 carved = r.exec.alloc(3936, MEMF_PUBLIC);
    CHECK(carved != 0);

    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    CHECK_EQ(open_win(r, kNewWindow), 0u);
    CHECK(logged("intuition: no memory for a window"));

    // Everything the failed open allocated is free again.
    const u32 restored = r.exec.alloc(160, MEMF_PUBLIC);
    CHECK(restored != 0);
    r.exec.free(restored, 160);
    r.exec.free(carved, 3936);
    for (u32 at : tail) {
        r.exec.free(at, 160);
    }
    for (u32 i = 1; i < big.size(); ++i) {
        r.exec.free(big[i], 4096);
    }
}

// ClipBlit to and from a window reads and writes the window's own bitmap at
// window coordinates, wherever the window sits on the screen: a gadget's
// saved background comes back as it was.
void clip_blit_uses_window_coordinates() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 100, 60, 200, 100, WFLG_BORDERLESS, 0, screen, CUSTOMSCREEN);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 wrp = r.get32(window + wd_RPort);
    const auto plot = [&](u32 rp, i32 x, i32 y, u32 pen) {
        r.cpu().a[1] = rp;
        r.cpu().d[0] = pen;
        r.call(Base::Graphics, graphics_lvo::SetAPen);
        r.cpu().a[1] = rp;
        r.cpu().d[0] = static_cast<u32>(x);
        r.cpu().d[1] = static_cast<u32>(y);
        r.call(Base::Graphics, graphics_lvo::WritePixel);
    };
    const auto pixel = [&](u32 rp, i32 x, i32 y) {
        r.cpu().a[1] = rp;
        r.cpu().d[0] = static_cast<u32>(x);
        r.cpu().d[1] = static_cast<u32>(y);
        return r.result(Base::Graphics, graphics_lvo::ReadPixel);
    };
    const auto clip_blit = [&](u32 src, i32 sx, i32 sy, u32 dst, i32 dx, i32 dy, u32 w, u32 h) {
        r.cpu().a[0] = src;
        r.cpu().d[0] = static_cast<u32>(sx);
        r.cpu().d[1] = static_cast<u32>(sy);
        r.cpu().a[1] = dst;
        r.cpu().d[2] = static_cast<u32>(dx);
        r.cpu().d[3] = static_cast<u32>(dy);
        r.cpu().d[4] = w;
        r.cpu().d[5] = h;
        r.cpu().d[6] = 0xC0;
        r.call(Base::Graphics, graphics_lvo::ClipBlit);
    };
    plot(wrp, 20, 30, 5);
    plot(wrp, 21, 30, 6);

    // An off-screen RastPort over a bitmap of the screen's depth.
    r.cpu().d[0] = 16;
    r.cpu().d[1] = 8;
    r.cpu().d[2] = 8;
    r.cpu().d[3] = 0;
    r.cpu().a[0] = 0;
    const u32 bitmap = r.result(Base::Graphics, graphics_lvo::AllocBitMap);
    CHECK(bitmap != 0);
    const u32 save = kData + 0x3000;
    r.cpu().a[1] = save;
    r.call(Base::Graphics, graphics_lvo::InitRastPort);
    r.put32(save + rp_BitMap, bitmap);

    clip_blit(wrp, 20, 30, save, 0, 0, 2, 1);
    CHECK_EQ(pixel(save, 0, 0), 5u);
    CHECK_EQ(pixel(save, 1, 0), 6u);

    clip_blit(save, 0, 0, wrp, 50, 40, 2, 1);
    CHECK_EQ(pixel(wrp, 50, 40), 5u);
    CHECK_EQ(pixel(wrp, 51, 40), 6u);
}

void open_window_limit_gives_zero() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 0, 0, 32, 32, 0, 0, 0, 0);
    for (u32 i = 0; i < Intuition::kMaxWindows; ++i) {
        CHECK(open_win(r, kNewWindow) != 0);
    }
    CHECK_EQ(open_win(r, kNewWindow), 0u);
    CHECK(logged("intuition: window limit reached"));
    u32 count = 0;
    u32 window = r.get32(screen + sc_FirstWindow);
    while (window != 0 && count <= Intuition::kMaxWindows) {
        ++count;
        window = r.get32(window + wd_NextWindow);
    }
    CHECK_EQ(count, Intuition::kMaxWindows);
}

void more_than_sixteen_windows_open_and_close_in_any_order() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 0, 0, 32, 32, 0, 0, 0, 0);
    std::vector<u32> windows;
    for (u32 i = 0; i < 40; ++i) {
        windows.push_back(open_win(r, kNewWindow));
        CHECK(windows.back() != 0);
    }
    // Each window opens in front of the ones before it.
    u32 behind = r.get32(screen + sc_FirstWindow);
    for (auto window = windows.rbegin(); window != windows.rend(); ++window) {
        CHECK_EQ(behind, *window);
        CHECK(r.intuition.window_open(*window));
        behind = r.get32(behind + wd_NextWindow);
    }
    CHECK_EQ(behind, 0u);

    for (usize i = 0; i < windows.size(); i += 2) {
        r.intuition.close_window(windows[i]);
        CHECK(!r.intuition.window_open(windows[i]));
    }
    for (usize i = 1; i < windows.size(); i += 2) {
        CHECK(r.intuition.window_open(windows[i]));
        r.intuition.close_window(windows[i]);
    }
    CHECK_EQ(r.get32(screen + sc_FirstWindow), 0u);
}

void window_text_goes_into_the_window_only() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 rp = r.get32(window + wd_RPort);

    const std::vector<u8> before = screen_planes(r, screen);
    r.graphics.set_pens(rp, 3, 0, JAM1);
    r.graphics.move(rp, 2 + static_cast<i32>(r.get8(window + wd_BorderLeft)),
                    7 + static_cast<i32>(r.get8(window + wd_BorderTop)));
    const u8 text[] = {'H', 'H'};
    CHECK_EQ(r.graphics.text(rp, text, 2), 16u);
    const std::vector<u8> after = screen_planes(r, screen);
    CHECK(changed_only_inside(before, after, 80, 400, 52, 75, 247, 157));
    CHECK_EQ(screen_pixel(r, screen, 54, 76), 3u);
    CHECK_EQ(screen_pixel(r, screen, 50, 78), 2u);
    CHECK_EQ(screen_pixel(r, screen, 249, 78), 1u);
    CHECK_EQ(screen_pixel(r, screen, 100, 74), 1u);
}

void text_longer_than_window_is_clipped() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 60, 50, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 rp = r.get32(window + wd_RPort);

    const std::vector<u8> before = screen_planes(r, screen);
    u8 text[40];
    for (u32 i = 0; i < 40; ++i) {
        text[i] = 'H';
    }
    r.graphics.set_pens(rp, 1, 0, JAM1);
    r.graphics.move(rp, static_cast<i32>(r.get8(window + wd_BorderLeft)),
                    static_cast<i32>(r.get8(window + wd_BorderTop)) + 6);
    CHECK_EQ(r.graphics.text(rp, text, 40), 320u);
    const std::vector<u8> after = screen_planes(r, screen);
    CHECK(changed_only_inside(before, after, 80, 400, 50, 60, 109, 159));
    CHECK_EQ(r.get16(rp + rp_cp_x), 322u);
    CHECK_EQ(screen_pixel(r, screen, 110, 76), 0u);
}

void close_window_repaints_and_unlinks() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const u32 before = heap_capacity(r);
    r.str(kWinTitle, "H");
    put_new_window(r, kNewWindow, 50, 60, 200, 100, WFLG_CLOSEGADGET, kWinTitle, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.get32(screen + sc_FirstWindow), window);

    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);
    CHECK_EQ(r.get32(screen + sc_FirstWindow), 0u);
    CHECK_EQ(heap_capacity(r), before);
    CHECK_EQ(screen_pixel(r, screen, 50, 60), 0u);
    CHECK_EQ(screen_pixel(r, screen, 100, 65), 0u);
    CHECK_EQ(screen_pixel(r, screen, 249, 159), 0u);
    CHECK_EQ(screen_pixel(r, screen, 5, 5), 2u);
    CHECK_EQ(screen_pixel(r, screen, 5, 14), 1u);
    CHECK(refreshed(r, screen, 50, 60, 249, 159));
}

void close_window_twice_and_null_ignored() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);

    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);
    const usize refreshes = r.refreshes.size();
    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);
    r.cpu().a[0] = 0;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);
    CHECK_EQ(r.refreshes.size(), refreshes);
    CHECK_EQ(r.get32(screen + sc_FirstWindow), 0u);
}

// RefreshWindowFrame draws a window's border again over what a program
// drew there, and leaves what is inside the border alone.
void refresh_window_frame_redraws_the_border_only() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, WFLG_CLOSEGADGET, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 rp = r.get32(window + wd_RPort);
    const i32 inside_x = r.get8(window + wd_BorderLeft) + 10;
    const i32 inside_y = r.get8(window + wd_BorderTop) + 10;
    const u32 left_edge = window_pixel(r, window, 0, 50);
    const u32 bottom_edge = window_pixel(r, window, 100, 99);
    const u32 bar = window_pixel(r, window, 100, 5);

    // Paint over the whole window, border and all.
    r.graphics.set_pens(rp, 3, 0, JAM1);
    r.graphics.rect_fill(rp, 0, 0, 199, 99);
    CHECK_EQ(window_pixel(r, window, 0, 50), 3u);
    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::RefreshWindowFrame), ExitReason::Halt);
    CHECK_EQ(window_pixel(r, window, 0, 50), left_edge);
    CHECK_EQ(window_pixel(r, window, 100, 99), bottom_edge);
    CHECK_EQ(window_pixel(r, window, 100, 5), bar);
    CHECK_EQ(window_pixel(r, window, inside_x, inside_y), 3u);
    CHECK_EQ(screen_pixel(r, screen, 50, 110), left_edge);

    r.cpu().a[0] = 0;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::RefreshWindowFrame), ExitReason::Halt);
    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);
}

void set_window_titles_redraws_title() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.str(kWinTitle, "A");
    put_new_window(r, kNewWindow, 50, 60, 200, 100, WFLG_CLOSEGADGET, kWinTitle, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(screen_pixel(r, screen, 69, 63), 0u);

    r.str(kWinTitle, "B");
    r.cpu().a[0] = window;
    r.cpu().a[1] = kWinTitle;
    r.cpu().a[2] = 0xFFFFFFFF;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::SetWindowTitles), ExitReason::Halt);
    const u32 title = r.get32(window + wd_Title);
    CHECK(title != 0);
    CHECK(title != kWinTitle);
    CHECK(r.read_str(title) == "B");
    CHECK_EQ(screen_pixel(r, screen, 69, 63), 1u);

    r.cpu().a[0] = window;
    r.cpu().a[1] = 0;
    r.cpu().a[2] = 0xFFFFFFFF;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::SetWindowTitles), ExitReason::Halt);
    CHECK_EQ(r.get32(window + wd_Title), 0u);
    CHECK_EQ(screen_pixel(r, screen, 69, 63), 0u);

    r.cpu().a[0] = window;
    r.cpu().a[1] = 0xFFFFFFFF;
    r.cpu().a[2] = 0xFFFFFFFF;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::SetWindowTitles), ExitReason::Halt);
    CHECK_EQ(r.get32(window + wd_Title), 0u);

    r.str(kWinTitle, "S");
    r.cpu().a[0] = window;
    r.cpu().a[1] = 0xFFFFFFFF;
    r.cpu().a[2] = kWinTitle;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::SetWindowTitles), ExitReason::Halt);
    const u32 screen_title = r.get32(screen + sc_Title);
    CHECK(screen_title != 0);
    CHECK(screen_title != kWinTitle);
    CHECK(r.read_str(screen_title) == "S");
}

void window_refresh_reaches_the_sink() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.refreshes.clear();
    r.refreshed_screens.clear();
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK(refreshed(r, screen, 50, 60, 249, 159));
    u32 whole = 0;
    for (usize i = 0; i < r.refreshes.size(); ++i) {
        const Rect& d = r.refreshes[i];
        if (r.refreshed_screens[i] == screen && d.x0 == 50 && d.y0 == 60 && d.x1 == 249 &&
            d.y1 == 159) {
            ++whole;
        }
    }
    CHECK_EQ(whole, 1u);
}

void open_window_reads_32_bit_flags() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);

    const u32 flags = 0x0000100E;
    put_new_window(r, kNewWindow, 50, 60, 200, 100, flags, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.get32(window + wd_Flags), flags);
    // The close gadget's groove and glyph, and the depth gadget's.
    CHECK_EQ(screen_pixel(r, screen, 66, 67), 2u);
    CHECK_EQ(screen_pixel(r, screen, 60, 67), 2u);
    CHECK_EQ(screen_pixel(r, screen, 235, 67), 2u);
    CHECK_EQ(screen_pixel(r, screen, 241, 66), 2u);

    put_new_window(r, kNewWindow, 300, 60, 100, 50, WFLG_BORDERLESS, 0, 0, 0);
    const u32 borderless = open_win(r, kNewWindow);
    CHECK(borderless != 0);
    CHECK_EQ(r.get32(borderless + wd_Flags), static_cast<u32>(WFLG_BORDERLESS));
    CHECK_EQ(r.get8(borderless + wd_BorderTop), 1u);

    const u32 tags = r.tags(kTagList, {{WA_Left, 10}, {WA_Top, 20}, {WA_Width, 100},
                                       {WA_Height, 60}, {WA_Flags, flags}, {TAG_DONE, 0}});
    const u32 tagged = open_win_tags(r, 0, tags);
    CHECK(tagged != 0);
    CHECK_EQ(r.get32(tagged + wd_Flags), flags);
    CHECK_EQ(screen_pixel(r, screen, 20, 27), 2u);
    CHECK_EQ(screen_pixel(r, screen, 11, 21), 3u);
}

void window_open_is_true_until_closed() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK(r.intuition.window_open(window));
    CHECK(!r.intuition.window_open(0));
    CHECK(!r.intuition.window_open(0x123456));

    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);
    CHECK(!r.intuition.window_open(window));
}

void close_screen_with_open_windows_returns_false_and_keeps_both() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);

    r.cpu().a[0] = screen;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::CloseScreen), 0u);
    CHECK_EQ(r.intuition.front_screen(), screen);
    CHECK(r.intuition.window_open(window));
    CHECK_EQ(r.get32(screen + sc_FirstWindow), window);
}

void close_screen_after_windows_returns_true() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);

    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);
    r.cpu().a[0] = screen;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::CloseScreen), 1u);
    CHECK_EQ(r.intuition.front_screen(), 0u);
    CHECK_EQ(r.display.front_screen, 0u);
}

void close_screen_zero_and_unknown_return_false() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);

    r.cpu().a[0] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::CloseScreen), 0u);
    r.cpu().a[0] = 0x123456;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::CloseScreen), 0u);
    CHECK(!r.intuition.close_screen(0));
    CHECK(!r.intuition.close_screen(0x123456));
    CHECK_EQ(r.intuition.front_screen(), screen);
}

void guest_cannot_redirect_frees_on_close_screen() {
    GfxRig r;
    const u32 before = heap_capacity(r);
    r.str(kTitle, "ABCDEFG");
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, kTitle);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);

    const u32 victim = r.exec.alloc(256, MEMF_PUBLIC);
    CHECK(victim != 0);
    for (u32 i = 0; i < 256; ++i) {
        r.put8(victim + i, 0xA5);
    }
    r.put32(screen + sc_BitMap + bm_Planes, victim);
    r.put32(screen + sc_ViewPort + vp_ColorMap, victim);
    const u32 title = r.get32(screen + sc_Title);
    CHECK(title != 0);
    u32 length = 0;
    while (r.get8(title + length) != 0) {
        ++length;
    }
    CHECK_EQ(length, 7u);
    r.put8(title + length, 'X');

    CHECK(r.intuition.close_screen(screen));
    for (u32 i = 0; i < 256; ++i) {
        CHECK_EQ(r.get8(victim + i), 0xA5u);
    }
    for (u32 i = 0; i < 50; ++i) {
        const u32 at = r.exec.alloc(256, MEMF_PUBLIC);
        CHECK(at != 0);
        CHECK(!(at >= victim && at < victim + 256));
        r.exec.free(at, 256);
    }
    r.exec.free(victim, 256);
    CHECK_EQ(heap_capacity(r), before);
}

void guest_cannot_redirect_frees_on_close_window() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const u32 before = heap_capacity(r);

    r.str(kWinTitle, "ABCDEFG");
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, kWinTitle, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 victim = r.exec.alloc(256, MEMF_PUBLIC);
    CHECK(victim != 0);
    for (u32 i = 0; i < 256; ++i) {
        r.put8(victim + i, 0xA5);
    }
    r.put32(window + wd_RPort, victim);
    r.put32(window + wd_WLayer, victim);
    const u32 title = r.get32(window + wd_Title);
    CHECK(title != 0);
    u32 length = 0;
    while (r.get8(title + length) != 0) {
        ++length;
    }
    CHECK_EQ(length, 7u);
    r.put8(title + length, 'X');

    r.intuition.close_window(window);
    for (u32 i = 0; i < 256; ++i) {
        CHECK_EQ(r.get8(victim + i), 0xA5u);
    }
    for (u32 i = 0; i < 50; ++i) {
        const u32 at = r.exec.alloc(256, MEMF_PUBLIC);
        CHECK(at != 0);
        CHECK(!(at >= victim && at < victim + 256));
        r.exec.free(at, 256);
    }
    r.exec.free(victim, 256);
    CHECK_EQ(heap_capacity(r), before);
}

void window_layer_is_the_outer_rectangle() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 10, 20, 100, 60, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 layer = r.get32(window + wd_WLayer);
    CHECK_EQ(r.get16(layer + ly_MinX), 10u);
    CHECK_EQ(r.get16(layer + ly_MinY), 20u);
    CHECK_EQ(r.get16(layer + ly_MaxX), 109u);
    CHECK_EQ(r.get16(layer + ly_MaxY), 79u);

    const u32 rp = r.get32(window + wd_RPort);
    CHECK_EQ(r.get8(rp + rp_FgPen), 1u);
    CHECK_EQ(r.get8(rp + rp_BgPen), 0u);
    CHECK_EQ(r.get8(rp + rp_DrawMode), JAM2);
    CHECK_EQ(r.get16(rp + rp_cp_x), 0u);
    CHECK_EQ(r.get16(rp + rp_cp_y), 6u);
}

void window_rastport_origin_is_the_window_corner() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 10, 20, 100, 60, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 rp = r.get32(window + wd_RPort);

    r.cpu().a[1] = rp;
    r.cpu().d[0] = 3;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetAPen), ExitReason::Halt);
    r.cpu().a[1] = rp;
    r.cpu().d[0] = 0;
    r.cpu().d[1] = 0;
    r.cpu().d[2] = 0;
    r.cpu().d[3] = 0;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::RectFill), ExitReason::Halt);
    CHECK_EQ(screen_pixel(r, screen, 10, 20), 3u);
    CHECK_EQ(screen_pixel(r, screen, 11, 20), 2u);
}

void window_text_below_the_border() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 10, 20, 100, 60, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 rp = r.get32(window + wd_RPort);
    const i32 border_left = static_cast<i32>(r.get8(window + wd_BorderLeft));
    const i32 border_top = static_cast<i32>(r.get8(window + wd_BorderTop));

    r.str(kWinTitle, "A");
    r.cpu().a[1] = rp;
    r.cpu().d[0] = static_cast<u32>(border_left);
    r.cpu().d[1] = static_cast<u32>(border_top + 6);
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::Move), ExitReason::Halt);
    r.cpu().a[1] = rp;
    r.cpu().a[0] = kWinTitle;
    r.cpu().d[0] = 1;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::Text), ExitReason::Halt);

    CHECK_EQ(screen_pixel(r, screen, 10 + border_left + 2, 20 + border_top), 1u);
    CHECK_EQ(screen_pixel(r, screen, 10 + border_left, 20 + border_top), 0u);
}

void window_drawing_is_clipped_to_the_window() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 10, 20, 100, 60, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 rp = r.get32(window + wd_RPort);

    const std::vector<u8> before = screen_planes(r, screen);
    r.cpu().a[1] = rp;
    r.cpu().d[0] = 3;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetAPen), ExitReason::Halt);
    r.cpu().a[1] = rp;
    r.cpu().d[0] = static_cast<u32>(-50);
    r.cpu().d[1] = static_cast<u32>(-50);
    r.cpu().d[2] = 500;
    r.cpu().d[3] = 500;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::RectFill), ExitReason::Halt);

    const std::vector<u8> after = screen_planes(r, screen);
    CHECK(changed_only_inside(before, after, 80, 400, 10, 20, 109, 79));
    CHECK_EQ(screen_pixel(r, screen, 10, 20), 3u);
    CHECK_EQ(screen_pixel(r, screen, 9, 20), 0u);
    CHECK_EQ(screen_pixel(r, screen, 110, 20), 0u);
}

using i16 = __INT16_TYPE__;

u32 avail_mem(GfxRig& r) {
    r.cpu().d[1] = 0;
    return r.result(Base::Exec, exec_lvo::AvailMem);
}

u32 get_msg(GfxRig& r, u32 port) {
    r.cpu().a[0] = port;
    return r.result(Base::Exec, exec_lvo::GetMsg);
}

void reply_msg(GfxRig& r, u32 msg) {
    r.cpu().a[1] = msg;
    CHECK_EQ(r.call(Base::Exec, exec_lvo::ReplyMsg), ExitReason::Halt);
}

u64 g_fake_time_us = 0;
u64 fake_clock() { return g_fake_time_us; }

void window_without_idcmp_has_no_userport() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.get32(window + wd_IDCMPFlags), 0u);
    CHECK_EQ(r.get32(window + wd_UserPort), 0u);
}

void window_with_idcmp_gets_a_userport_and_flags() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const u32 flags = IDCMP_CLOSEWINDOW | IDCMP_MOUSEBUTTONS | IDCMP_ACTIVEWINDOW;
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, flags);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.get32(window + wd_IDCMPFlags), flags);
    const u32 port = r.get32(window + wd_UserPort);
    CHECK(port != 0);
    CHECK_EQ(r.get8(port + LN_TYPE), static_cast<u32>(NT_MSGPORT));
    CHECK_EQ(r.get8(port + mp_Flags), static_cast<u32>(PA_SIGNAL));
    CHECK_EQ(r.exec.get_message(port), 0u);
}

void wa_idcmp_tag_overrides_newwindow_flags() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_CLOSEWINDOW);
    const u32 override_flags = IDCMP_MOUSEBUTTONS | IDCMP_ACTIVEWINDOW;
    const u32 tags = r.tags(kTagList, {{WA_IDCMP, override_flags}, {TAG_DONE, 0}});
    const u32 window = open_win_tags(r, kNewWindow, tags);
    CHECK(window != 0);
    CHECK_EQ(r.get32(window + wd_IDCMPFlags), override_flags);
    CHECK(r.get32(window + wd_UserPort) != 0);

    const u32 zero_tags = r.tags(kTagList, {{WA_IDCMP, 0}, {TAG_DONE, 0}});
    const u32 plain = open_win_tags(r, kNewWindow, zero_tags);
    CHECK(plain != 0);
    CHECK_EQ(r.get32(plain + wd_IDCMPFlags), 0u);
    CHECK_EQ(r.get32(plain + wd_UserPort), 0u);
}

void modify_idcmp_creates_and_deletes_the_port() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.get32(window + wd_UserPort), 0u);

    r.cpu().a[0] = window;
    r.cpu().d[0] = IDCMP_CLOSEWINDOW;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ModifyIDCMP), 1u);
    CHECK_EQ(r.get32(window + wd_IDCMPFlags), IDCMP_CLOSEWINDOW);
    const u32 port = r.get32(window + wd_UserPort);
    CHECK(port != 0);

    r.cpu().a[0] = window;
    r.cpu().d[0] = IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ModifyIDCMP), 1u);
    CHECK_EQ(r.get32(window + wd_IDCMPFlags), IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE);
    CHECK_EQ(r.get32(window + wd_UserPort), port);

    r.cpu().a[0] = window;
    r.cpu().d[0] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ModifyIDCMP), 1u);
    CHECK_EQ(r.get32(window + wd_IDCMPFlags), 0u);
    CHECK_EQ(r.get32(window + wd_UserPort), 0u);
}

void modify_idcmp_zero_flushes_queued_messages() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 before = avail_mem(r);

    r.cpu().a[0] = window;
    r.cpu().d[0] = IDCMP_MOUSEBUTTONS;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ModifyIDCMP), 1u);
    const u32 port = r.get32(window + wd_UserPort);
    CHECK(port != 0);

    r.intuition.pointer_event(80, 90, kMouseLeft, kMouseLeft, 0, false);
    r.intuition.pointer_event(80, 90, 0, 0, kMouseLeft, false);
    const u32 head = r.get32(port + mp_MsgList + LH_HEAD);
    CHECK(r.get32(head + LN_SUCC) != 0);
    CHECK(avail_mem(r) < before);

    r.cpu().a[0] = window;
    r.cpu().d[0] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ModifyIDCMP), 1u);
    CHECK_EQ(r.get32(window + wd_UserPort), 0u);
    CHECK_EQ(avail_mem(r), before);
}

void close_window_frees_queued_messages_and_the_port_and_balances_memory() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const u32 before = avail_mem(r);

    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS | IDCMP_ACTIVEWINDOW);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    r.intuition.pointer_event(80, 90, kMouseLeft, kMouseLeft, 0, false);
    r.intuition.pointer_event(80, 90, 0, 0, kMouseLeft, false);
    r.intuition.pointer_event(80, 90, kMouseRight, kMouseRight, 0, false);
    CHECK(avail_mem(r) < before);

    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);
    CHECK_EQ(avail_mem(r), before);
}

void reply_msg_frees_the_intuimessage_and_ignores_forged_addresses() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 port = r.get32(window + wd_UserPort);
    const u32 before = avail_mem(r);

    r.intuition.pointer_event(60, 70, kMouseLeft, kMouseLeft, 0, false);
    CHECK(avail_mem(r) < before);
    const u32 msg = get_msg(r, port);
    CHECK(msg != 0);
    const u32 reply_port = r.get32(msg + mn_ReplyPort);
    CHECK(reply_port != 0);
    CHECK_EQ(r.get16(msg + mn_Length), IntuiMessage_SIZEOF);
    reply_msg(r, msg);
    CHECK_EQ(avail_mem(r), before);

    const u32 forged = r.exec.alloc(IntuiMessage_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    CHECK(forged != 0);
    r.put32(forged + mn_ReplyPort, reply_port);
    r.put32(forged + im_Class, 0xDEADBEEF);
    const u32 with_forged = avail_mem(r);
    reply_msg(r, forged);
    CHECK_EQ(avail_mem(r), with_forged);
    CHECK_EQ(r.get32(forged + im_Class), 0xDEADBEEFu);
    r.exec.free(forged, IntuiMessage_SIZEOF);
    CHECK_EQ(avail_mem(r), before);
}

void mouse_button_goes_to_the_window_under_the_pointer_with_relative_coordinates() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS);
    const u32 back = open_win(r, kNewWindow);
    put_new_window(r, kNewWindow, 100, 80, 100, 60, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS);
    const u32 front = open_win(r, kNewWindow);
    CHECK(back != 0 && front != 0);
    const u32 back_port = r.get32(back + wd_UserPort);
    const u32 front_port = r.get32(front + wd_UserPort);

    r.intuition.pointer_event(125, 115, kMouseLeft, kMouseLeft, 0, false);
    r.intuition.pointer_event(125, 115, 0, 0, kMouseLeft, false);
    r.intuition.pointer_event(125, 115, kMouseRight, kMouseRight, 0, false);
    r.intuition.pointer_event(125, 115, 0, 0, kMouseRight, false);
    r.intuition.pointer_event(125, 115, kMouseMiddle, kMouseMiddle, 0, false);
    r.intuition.pointer_event(125, 115, 0, 0, kMouseMiddle, false);
    CHECK_EQ(get_msg(r, back_port), 0u);

    const u32 codes[6] = {SELECTDOWN, SELECTUP, MENUDOWN, MENUUP, MIDDLEDOWN, MIDDLEUP};
    for (u32 expected : codes) {
        const u32 msg = get_msg(r, front_port);
        CHECK(msg != 0);
        CHECK_EQ(r.get32(msg + im_Class), IDCMP_MOUSEBUTTONS);
        CHECK_EQ(r.get16(msg + im_Code), expected);
        CHECK_EQ(r.get16(msg + im_Qualifier), 0u);
        CHECK_EQ(r.get32(msg + im_IAddress), 0u);
        CHECK_EQ(r.get16(msg + im_MouseX), 25u);
        CHECK_EQ(r.get16(msg + im_MouseY), 35u);
        CHECK_EQ(r.get32(msg + im_IDCMPWindow), front);
        reply_msg(r, msg);
    }
    CHECK_EQ(get_msg(r, front_port), 0u);
}

void mouse_button_ignored_without_idcmp_flag() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_CLOSEWINDOW);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 port = r.get32(window + wd_UserPort);

    r.intuition.pointer_event(80, 90, kMouseLeft, kMouseLeft, 0, false);
    r.intuition.pointer_event(80, 90, 0, 0, kMouseLeft, false);
    r.intuition.pointer_event(80, 90, kMouseRight, kMouseRight, 0, false);
    CHECK_EQ(get_msg(r, port), 0u);
}

void left_press_activates_the_window_and_posts_active_and_inactive() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 20, 30, 100, 80, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_ACTIVEWINDOW | IDCMP_INACTIVEWINDOW);
    const u32 w1 = open_win(r, kNewWindow);
    put_new_window(r, kNewWindow, 200, 30, 100, 80, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_ACTIVEWINDOW | IDCMP_INACTIVEWINDOW);
    const u32 w2 = open_win(r, kNewWindow);
    CHECK(w1 != 0 && w2 != 0);
    CHECK_EQ(r.intuition.active_window(), 0u);

    r.intuition.pointer_event(40, 50, kMouseLeft, kMouseLeft, 0, false);
    CHECK_EQ(r.intuition.active_window(), w1);
    const u32 m1 = get_msg(r, r.get32(w1 + wd_UserPort));
    CHECK(m1 != 0);
    CHECK_EQ(r.get32(m1 + im_Class), IDCMP_ACTIVEWINDOW);
    CHECK_EQ(r.get32(m1 + im_IDCMPWindow), w1);
    reply_msg(r, m1);
    CHECK_EQ(get_msg(r, r.get32(w2 + wd_UserPort)), 0u);

    r.intuition.pointer_event(45, 55, kMouseLeft, kMouseLeft, 0, false);
    CHECK_EQ(get_msg(r, r.get32(w1 + wd_UserPort)), 0u);

    r.intuition.pointer_event(220, 50, kMouseLeft, kMouseLeft, 0, false);
    CHECK_EQ(r.intuition.active_window(), w2);
    const u32 inactive = get_msg(r, r.get32(w1 + wd_UserPort));
    CHECK(inactive != 0);
    CHECK_EQ(r.get32(inactive + im_Class), IDCMP_INACTIVEWINDOW);
    CHECK_EQ(r.get32(inactive + im_IDCMPWindow), w1);
    reply_msg(r, inactive);
    const u32 active = get_msg(r, r.get32(w2 + wd_UserPort));
    CHECK(active != 0);
    CHECK_EQ(r.get32(active + im_Class), IDCMP_ACTIVEWINDOW);
    CHECK_EQ(r.get32(active + im_IDCMPWindow), w2);
    reply_msg(r, active);

    r.intuition.close_window(w2);
    CHECK_EQ(r.intuition.active_window(), 0u);
}

void move_goes_to_the_active_window_only_with_reportmouse() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 100, 80, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEMOVE);
    const u32 quiet = open_win(r, kNewWindow);
    CHECK(quiet != 0);
    r.intuition.pointer_event(70, 80, 0, 0, 0, true);
    CHECK_EQ(get_msg(r, r.get32(quiet + wd_UserPort)), 0u);

    put_new_window(r, kNewWindow, 50, 60, 100, 80, WFLG_ACTIVATE | WFLG_REPORTMOUSE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEMOVE);
    const u32 reporting = open_win(r, kNewWindow);
    CHECK(reporting != 0);
    r.intuition.pointer_event(40, 55, 0, 0, 0, true);
    const u32 msg = get_msg(r, r.get32(reporting + wd_UserPort));
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_MOUSEMOVE);
    CHECK_EQ(r.get16(msg + im_Code), 0u);
    CHECK_EQ(static_cast<i16>(r.get16(msg + im_MouseX)), -10);
    CHECK_EQ(static_cast<i16>(r.get16(msg + im_MouseY)), -5);
    CHECK_EQ(r.get32(msg + im_IDCMPWindow), reporting);
    reply_msg(r, msg);
}

void report_mouse_turns_move_reports_on_and_off() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    put_new_window(r, kNewWindow, 50, 60, 100, 80, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEMOVE);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 port = r.get32(window + wd_UserPort);

    r.intuition.report_mouse(window, true);
    CHECK_EQ(r.get32(window + wd_Flags) & WFLG_REPORTMOUSE, WFLG_REPORTMOUSE);
    r.intuition.pointer_event(70, 80, 0, 0, 0, true);
    const u32 msg = get_msg(r, port);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_MOUSEMOVE);
    reply_msg(r, msg);

    r.intuition.report_mouse(window, false);
    CHECK_EQ(r.get32(window + wd_Flags) & WFLG_REPORTMOUSE, 0u);
    r.intuition.pointer_event(72, 82, 0, 0, 0, true);
    CHECK_EQ(get_msg(r, port), 0u);
}

void a_drag_image_is_drawn_above_the_windows_until_cleared() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 100, 80, WFLG_ACTIVATE, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 inside = screen_pixel(r, screen, 80, 100);
    const u32 own = window_pixel(r, window, 71 - 50, 92 - 60);
    const u32 outside = screen_pixel(r, screen, 300, 300);
    const u32 solid = put_image(r, kData + 0x1080, 1, 2, 6, 4, 0, 0, 0, 3);

    // Over the window, its corner at x, y plus the image's own offsets.
    r.intuition.set_drag_image(screen, solid, 70, 90);
    CHECK_EQ(screen_pixel(r, screen, 71, 92), 3u);
    CHECK_EQ(screen_pixel(r, screen, 76, 95), 3u);
    CHECK_EQ(screen_pixel(r, screen, 77, 95), inside);
    CHECK_EQ(screen_pixel(r, screen, 70, 92), inside);
    // The window's own bitmap is not drawn into.
    CHECK_EQ(window_pixel(r, window, 71 - 50, 92 - 60), own);

    // Moving it puts back what was under the old place.
    r.intuition.set_drag_image(screen, solid, 299, 298);
    CHECK_EQ(screen_pixel(r, screen, 71, 92), inside);
    CHECK_EQ(screen_pixel(r, screen, 300, 300), 3u);

    r.intuition.clear_drag_image();
    CHECK_EQ(screen_pixel(r, screen, 300, 300), outside);
}

void close_gadget_posts_closewindow_on_release_over_the_box() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, WFLG_CLOSEGADGET, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_CLOSEWINDOW);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 port = r.get32(window + wd_UserPort);
    const Rect box = Intuition::close_box_rect(50, 60);

    r.intuition.pointer_event(box.x0 + 2, box.y0 + 2, kMouseLeft, kMouseLeft, 0, false);
    CHECK_EQ(get_msg(r, port), 0u);
    r.intuition.pointer_event(box.x1 - 1, box.y1 - 1, 0, 0, kMouseLeft, false);
    const u32 msg = get_msg(r, port);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_CLOSEWINDOW);
    CHECK_EQ(r.get16(msg + im_Code), 0u);
    CHECK_EQ(r.get32(msg + im_IDCMPWindow), window);
    reply_msg(r, msg);
}

void close_gadget_release_elsewhere_does_nothing() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, WFLG_CLOSEGADGET, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_CLOSEWINDOW | IDCMP_MOUSEBUTTONS);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 port = r.get32(window + wd_UserPort);
    const Rect box = Intuition::close_box_rect(50, 60);

    r.intuition.pointer_event(box.x0 + 1, box.y0 + 1, kMouseLeft, kMouseLeft, 0, false);
    r.intuition.pointer_event(100, 100, 0, 0, kMouseLeft, false);
    CHECK_EQ(get_msg(r, port), 0u);
}

void close_gadget_without_idcmp_does_nothing() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, WFLG_CLOSEGADGET, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 port = r.get32(window + wd_UserPort);
    const Rect box = Intuition::close_box_rect(50, 60);

    r.intuition.pointer_event(box.x0 + 1, box.y0 + 1, kMouseLeft, kMouseLeft, 0, false);
    r.intuition.pointer_event(box.x0 + 1, box.y0 + 1, 0, 0, kMouseLeft, false);
    CHECK_EQ(get_msg(r, port), 0u);
    CHECK(r.intuition.window_open(window));
}

void depth_gadget_toggles_front_and_back() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 20, 40, 120, 80, WFLG_DEPTHGADGET, 0, 0, 0);
    const u32 w1 = open_win(r, kNewWindow);
    put_new_window(r, kNewWindow, 200, 40, 120, 80, WFLG_DEPTHGADGET, 0, 0, 0);
    const u32 w2 = open_win(r, kNewWindow);
    CHECK(w1 != 0 && w2 != 0);
    CHECK_EQ(r.get32(screen + sc_FirstWindow), w2);
    CHECK_EQ(r.get32(w2 + wd_NextWindow), w1);

    const Rect d2 = Intuition::depth_box_rect(200, 40, 120);
    r.intuition.pointer_event(d2.x0 + 2, d2.y0 + 2, kMouseLeft, kMouseLeft, 0, false);
    r.intuition.pointer_event(d2.x0 + 2, d2.y0 + 2, 0, 0, kMouseLeft, false);
    CHECK_EQ(r.get32(screen + sc_FirstWindow), w1);
    CHECK_EQ(r.get32(w1 + wd_NextWindow), w2);
    CHECK_EQ(r.get32(w2 + wd_NextWindow), 0u);

    r.intuition.pointer_event(d2.x0 + 2, d2.y0 + 2, kMouseLeft, kMouseLeft, 0, false);
    r.intuition.pointer_event(d2.x0 + 2, d2.y0 + 2, 0, 0, kMouseLeft, false);
    CHECK_EQ(r.get32(screen + sc_FirstWindow), w2);
    CHECK_EQ(r.get32(w2 + wd_NextWindow), w1);
    CHECK_EQ(r.get32(w1 + wd_NextWindow), 0u);
}

void gadget_press_is_consumed_without_a_mousebuttons_message() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, WFLG_CLOSEGADGET | WFLG_DEPTHGADGET, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS | IDCMP_CLOSEWINDOW);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 port = r.get32(window + wd_UserPort);

    const Rect close_box = Intuition::close_box_rect(50, 60);
    r.intuition.pointer_event(close_box.x0, close_box.y0, kMouseLeft, kMouseLeft, 0, false);
    CHECK_EQ(get_msg(r, port), 0u);
    r.intuition.pointer_event(close_box.x0, close_box.y0, 0, 0, kMouseLeft, false);
    const u32 close_msg = get_msg(r, port);
    CHECK(close_msg != 0);
    CHECK_EQ(r.get32(close_msg + im_Class), IDCMP_CLOSEWINDOW);
    reply_msg(r, close_msg);
    CHECK_EQ(get_msg(r, port), 0u);

    const Rect depth_box = Intuition::depth_box_rect(50, 60, 200);
    r.intuition.pointer_event(depth_box.x0, depth_box.y0, kMouseLeft, kMouseLeft, 0, false);
    r.intuition.pointer_event(depth_box.x0, depth_box.y0, 0, 0, kMouseLeft, false);
    CHECK_EQ(get_msg(r, port), 0u);
}

// On a four colour screen the bar gadgets are cells as high as the bar that
// reach the window's edges, each parted from the title by a groove: a
// SHADOWPEN (1) line, then a SHINEPEN (2) one. The glyphs are etched on the
// bar's face, here the BACKGROUNDPEN (0) of an inactive window, and lie
// inside the hit rectangles.
void hit_rectangles_hold_the_drawn_glyphs() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, WFLG_CLOSEGADGET | WFLG_DEPTHGADGET, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);

    const Rect close_box = Intuition::close_box_rect(50, 60);
    const Rect depth_box = Intuition::depth_box_rect(50, 60, 200);
    const i32 left = 50;
    const i32 right = 249;
    const i32 top = 60;
    const i32 under = top + chrome::Metrics{}.bar_height();
    const i32 cy = (top + under) / 2;
    CHECK_EQ(cy, (close_box.y0 + close_box.y1) / 2);

    // The close cell: the window's light left and top edges, the dark line
    // under the bar, and the groove where the hit rectangle ends.
    CHECK_EQ(screen_pixel(r, screen, left, cy), 2u);
    CHECK_EQ(screen_pixel(r, screen, left + 5, top), 2u);
    CHECK_EQ(screen_pixel(r, screen, left + 5, under), 1u);
    CHECK_EQ(screen_pixel(r, screen, close_box.x1, cy), 1u);
    CHECK_EQ(screen_pixel(r, screen, close_box.x1 + 1, cy), 2u);
    CHECK_EQ(screen_pixel(r, screen, close_box.x1 + 2, cy), 0u);
    // Its glyph, 7 by 5 in the middle of the cell: dark along the top and
    // left, light along the bottom and right, the face inside.
    const i32 close_cx = (left + 1 + close_box.x1 - 1) / 2;
    CHECK(close_cx - 3 >= close_box.x0 && close_cx + 3 <= close_box.x1);
    CHECK_EQ(screen_pixel(r, screen, close_cx, cy - 2), 1u);
    CHECK_EQ(screen_pixel(r, screen, close_cx - 3, cy), 1u);
    CHECK_EQ(screen_pixel(r, screen, close_cx, cy + 2), 2u);
    CHECK_EQ(screen_pixel(r, screen, close_cx + 3, cy), 2u);
    CHECK_EQ(screen_pixel(r, screen, close_cx, cy), 0u);
    CHECK_EQ(screen_pixel(r, screen, close_cx - 4, cy), 0u);
    CHECK_EQ(screen_pixel(r, screen, close_cx + 4, cy), 0u);

    // The depth cell: the groove where the hit rectangle starts, and the
    // window's dark right edge.
    CHECK_EQ(screen_pixel(r, screen, depth_box.x0 - 1, cy), 0u);
    CHECK_EQ(screen_pixel(r, screen, depth_box.x0, cy), 1u);
    CHECK_EQ(screen_pixel(r, screen, depth_box.x0 + 1, cy), 2u);
    CHECK_EQ(screen_pixel(r, screen, depth_box.x0, top), 2u);
    CHECK_EQ(screen_pixel(r, screen, depth_box.x0 + 5, top), 2u);
    CHECK_EQ(screen_pixel(r, screen, depth_box.x0 + 5, under), 1u);
    CHECK_EQ(screen_pixel(r, screen, right, cy), 1u);
    CHECK_EQ(screen_pixel(r, screen, right - 1, cy), 0u);
    // Its glyph: a dark sheet at the back and a light one in front, lower
    // and to the right, with the face inside both.
    const i32 depth_cx = (depth_box.x0 + 2 + right - 1) / 2;
    CHECK(depth_cx - 3 >= depth_box.x0 && depth_cx + 3 <= depth_box.x1);
    CHECK_EQ(screen_pixel(r, screen, depth_cx - 3, cy - 3), 1u);
    CHECK_EQ(screen_pixel(r, screen, depth_cx + 1, cy - 3), 1u);
    CHECK_EQ(screen_pixel(r, screen, depth_cx - 3, cy + 1), 1u);
    CHECK_EQ(screen_pixel(r, screen, depth_cx - 2, cy - 2), 0u);
    CHECK_EQ(screen_pixel(r, screen, depth_cx - 1, cy - 1), 2u);
    CHECK_EQ(screen_pixel(r, screen, depth_cx + 3, cy + 3), 2u);
    CHECK_EQ(screen_pixel(r, screen, depth_cx + 1, cy + 1), 0u);
    CHECK_EQ(screen_pixel(r, screen, depth_cx, cy), 0u);
}

void messages_carry_the_clock_time() {
    GfxRig r;
    r.intuition.set_clock(fake_clock);
    g_fake_time_us = 42000123ULL;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 port = r.get32(window + wd_UserPort);

    r.intuition.pointer_event(80, 90, kMouseLeft, kMouseLeft, 0, false);
    const u32 msg = get_msg(r, port);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Seconds), 42u);
    CHECK_EQ(r.get32(msg + im_Micros), 123u);
    reply_msg(r, msg);
}

void full_message_table_drops_events_without_leaking() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const u32 before = avail_mem(r);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);

    for (u32 i = 0; i < Intuition::kMaxMessages; ++i) {
        r.intuition.pointer_event(80, 90, kMouseLeft, kMouseLeft, 0, false);
    }
    CHECK_EQ(r.intuition.dropped_messages(), 0u);
    const u32 full_avail = avail_mem(r);

    for (u32 i = 0; i < 10; ++i) {
        r.intuition.pointer_event(80, 90, kMouseLeft, kMouseLeft, 0, false);
    }
    CHECK_EQ(r.intuition.dropped_messages(), 10u);
    CHECK_EQ(avail_mem(r), full_avail);

    r.intuition.close_window(window);
    CHECK_EQ(avail_mem(r), before);
}

void events_for_windows_of_other_screens_are_ignored() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 first = open_new(r, kNewScreen);
    CHECK(first != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100,
                   WFLG_ACTIVATE | WFLG_REPORTMOUSE | WFLG_CLOSEGADGET, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags,
            IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_CLOSEWINDOW);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 port = r.get32(window + wd_UserPort);

    const u32 second = open_new(r, kNewScreen);
    CHECK(second != 0);
    CHECK_EQ(r.intuition.front_screen(), second);

    const Rect close_box = Intuition::close_box_rect(50, 60);
    r.intuition.pointer_event(close_box.x0 + 1, close_box.y0 + 1, kMouseLeft, kMouseLeft, 0,
                              true);
    r.intuition.pointer_event(close_box.x0 + 1, close_box.y0 + 1, 0, 0, kMouseLeft, true);
    r.intuition.pointer_event(100, 100, kMouseLeft, kMouseLeft, 0, true);
    r.intuition.pointer_event(100, 100, 0, 0, kMouseLeft, true);
    CHECK_EQ(get_msg(r, port), 0u);
}

void openwindow_with_activate_flag_activates() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 10, 20, 100, 60, 0, 0, 0, 0);
    const u32 plain = open_win(r, kNewWindow);
    CHECK(plain != 0);
    CHECK_EQ(r.intuition.active_window(), 0u);

    put_new_window(r, kNewWindow, 40, 50, 100, 60, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_ACTIVEWINDOW | IDCMP_INACTIVEWINDOW);
    const u32 first_active = open_win(r, kNewWindow);
    CHECK(first_active != 0);
    CHECK_EQ(r.intuition.active_window(), first_active);
    const u32 m1 = get_msg(r, r.get32(first_active + wd_UserPort));
    CHECK(m1 != 0);
    CHECK_EQ(r.get32(m1 + im_Class), IDCMP_ACTIVEWINDOW);
    reply_msg(r, m1);

    put_new_window(r, kNewWindow, 80, 90, 100, 60, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_ACTIVEWINDOW);
    const u32 second_active = open_win(r, kNewWindow);
    CHECK(second_active != 0);
    CHECK_EQ(r.intuition.active_window(), second_active);
    const u32 m_inactive = get_msg(r, r.get32(first_active + wd_UserPort));
    CHECK(m_inactive != 0);
    CHECK_EQ(r.get32(m_inactive + im_Class), IDCMP_INACTIVEWINDOW);
    reply_msg(r, m_inactive);
    const u32 m2 = get_msg(r, r.get32(second_active + wd_UserPort));
    CHECK(m2 != 0);
    CHECK_EQ(r.get32(m2 + im_Class), IDCMP_ACTIVEWINDOW);
    reply_msg(r, m2);
}

void shared_userport_is_adopted_and_not_deleted() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const u32 before = avail_mem(r);

    const u32 shared = r.exec.create_port();
    CHECK(shared != 0);
    const u32 with_port = avail_mem(r);

    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.get32(window + wd_UserPort), 0u);
    const u32 with_window = avail_mem(r);

    r.put32(window + wd_UserPort, shared);
    r.cpu().a[0] = window;
    r.cpu().d[0] = IDCMP_CLOSEWINDOW | IDCMP_MOUSEBUTTONS;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ModifyIDCMP), 1u);
    CHECK_EQ(r.get32(window + wd_UserPort), shared);
    CHECK_EQ(r.get32(window + wd_IDCMPFlags),
             static_cast<u32>(IDCMP_CLOSEWINDOW | IDCMP_MOUSEBUTTONS));
    CHECK_EQ(avail_mem(r), with_window);

    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);
    CHECK_EQ(r.get32(window + wd_UserPort), shared);
    CHECK_EQ(avail_mem(r), with_port);

    r.exec.delete_port(shared);
    CHECK_EQ(avail_mem(r), before);
}

void shared_userport_gets_messages_for_the_window() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const u32 before = avail_mem(r);

    const u32 shared = r.exec.create_port();
    CHECK(shared != 0);

    put_new_window(r, kNewWindow, 20, 30, 100, 80, 0, 0, 0, 0);
    const u32 w1 = open_win(r, kNewWindow);
    put_new_window(r, kNewWindow, 200, 40, 100, 80, 0, 0, 0, 0);
    const u32 w2 = open_win(r, kNewWindow);
    CHECK(w1 != 0 && w2 != 0);

    r.put32(w1 + wd_UserPort, shared);
    r.cpu().a[0] = w1;
    r.cpu().d[0] = IDCMP_MOUSEBUTTONS | IDCMP_ACTIVEWINDOW;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ModifyIDCMP), 1u);

    r.put32(w2 + wd_UserPort, shared);
    r.cpu().a[0] = w2;
    r.cpu().d[0] = IDCMP_MOUSEBUTTONS | IDCMP_ACTIVEWINDOW;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ModifyIDCMP), 1u);

    r.intuition.pointer_event(50, 60, kMouseLeft, kMouseLeft, 0, false);
    r.intuition.pointer_event(250, 70, kMouseLeft, kMouseLeft, 0, false);

    const u32 m1 = get_msg(r, shared);
    CHECK(m1 != 0);
    CHECK_EQ(r.get32(m1 + im_Class), IDCMP_ACTIVEWINDOW);
    CHECK_EQ(r.get32(m1 + im_IDCMPWindow), w1);
    reply_msg(r, m1);

    const u32 m2 = get_msg(r, shared);
    CHECK(m2 != 0);
    CHECK_EQ(r.get32(m2 + im_Class), IDCMP_MOUSEBUTTONS);
    CHECK_EQ(r.get16(m2 + im_Code), SELECTDOWN);
    CHECK_EQ(r.get16(m2 + im_MouseX), 30u);
    CHECK_EQ(r.get16(m2 + im_MouseY), 30u);
    CHECK_EQ(r.get32(m2 + im_IDCMPWindow), w1);
    reply_msg(r, m2);

    const u32 m3 = get_msg(r, shared);
    CHECK(m3 != 0);
    CHECK_EQ(r.get32(m3 + im_Class), IDCMP_ACTIVEWINDOW);
    CHECK_EQ(r.get32(m3 + im_IDCMPWindow), w2);
    reply_msg(r, m3);

    const u32 m4 = get_msg(r, shared);
    CHECK(m4 != 0);
    CHECK_EQ(r.get32(m4 + im_Class), IDCMP_MOUSEBUTTONS);
    CHECK_EQ(r.get16(m4 + im_Code), SELECTDOWN);
    CHECK_EQ(r.get16(m4 + im_MouseX), 50u);
    CHECK_EQ(r.get16(m4 + im_MouseY), 30u);
    CHECK_EQ(r.get32(m4 + im_IDCMPWindow), w2);
    reply_msg(r, m4);

    CHECK_EQ(get_msg(r, shared), 0u);
    r.intuition.close_window(w2);
    r.intuition.close_window(w1);
    r.exec.delete_port(shared);
    CHECK_EQ(avail_mem(r), before);
}

void shared_userport_close_removes_only_that_windows_messages() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const u32 before = avail_mem(r);

    const u32 shared = r.exec.create_port();
    CHECK(shared != 0);

    put_new_window(r, kNewWindow, 20, 30, 100, 80, 0, 0, 0, 0);
    const u32 w1 = open_win(r, kNewWindow);
    put_new_window(r, kNewWindow, 200, 40, 100, 80, 0, 0, 0, 0);
    const u32 w2 = open_win(r, kNewWindow);
    CHECK(w1 != 0 && w2 != 0);

    r.put32(w1 + wd_UserPort, shared);
    r.cpu().a[0] = w1;
    r.cpu().d[0] = IDCMP_MOUSEBUTTONS;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ModifyIDCMP), 1u);

    r.put32(w2 + wd_UserPort, shared);
    r.cpu().a[0] = w2;
    r.cpu().d[0] = IDCMP_MOUSEBUTTONS;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ModifyIDCMP), 1u);

    r.intuition.pointer_event(40, 50, kMouseLeft, kMouseLeft, 0, false);
    r.intuition.pointer_event(40, 50, 0, 0, kMouseLeft, false);
    r.intuition.pointer_event(220, 60, kMouseLeft, kMouseLeft, 0, false);
    r.intuition.pointer_event(45, 55, kMouseLeft, kMouseLeft, 0, false);
    r.intuition.pointer_event(225, 65, kMouseLeft, kMouseLeft, 0, false);

    r.intuition.close_window(w1);

    const u32 m1 = get_msg(r, shared);
    CHECK(m1 != 0);
    CHECK_EQ(r.get32(m1 + im_IDCMPWindow), w2);
    CHECK_EQ(r.get16(m1 + im_Code), SELECTDOWN);
    CHECK_EQ(r.get16(m1 + im_MouseX), 20u);
    CHECK_EQ(r.get16(m1 + im_MouseY), 20u);
    reply_msg(r, m1);

    const u32 m2 = get_msg(r, shared);
    CHECK(m2 != 0);
    CHECK_EQ(r.get32(m2 + im_IDCMPWindow), w2);
    CHECK_EQ(r.get16(m2 + im_Code), SELECTDOWN);
    CHECK_EQ(r.get16(m2 + im_MouseX), 25u);
    CHECK_EQ(r.get16(m2 + im_MouseY), 25u);
    reply_msg(r, m2);

    CHECK_EQ(get_msg(r, shared), 0u);
    r.intuition.close_window(w2);
    r.exec.delete_port(shared);
    CHECK_EQ(avail_mem(r), before);
}

void shared_userport_modify_idcmp_zero_leaves_the_program_port() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const u32 before = avail_mem(r);

    const u32 shared = r.exec.create_port();
    CHECK(shared != 0);

    put_new_window(r, kNewWindow, 20, 30, 100, 80, 0, 0, 0, 0);
    const u32 w1 = open_win(r, kNewWindow);
    put_new_window(r, kNewWindow, 200, 40, 100, 80, 0, 0, 0, 0);
    const u32 w2 = open_win(r, kNewWindow);
    CHECK(w1 != 0 && w2 != 0);

    r.put32(w1 + wd_UserPort, shared);
    r.cpu().a[0] = w1;
    r.cpu().d[0] = IDCMP_MOUSEBUTTONS;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ModifyIDCMP), 1u);

    r.put32(w2 + wd_UserPort, shared);
    r.cpu().a[0] = w2;
    r.cpu().d[0] = IDCMP_MOUSEBUTTONS;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ModifyIDCMP), 1u);

    r.intuition.pointer_event(40, 50, kMouseLeft, kMouseLeft, 0, false);
    r.intuition.pointer_event(220, 60, kMouseLeft, kMouseLeft, 0, false);

    r.cpu().a[0] = w1;
    r.cpu().d[0] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ModifyIDCMP), 1u);
    CHECK_EQ(r.get32(w1 + wd_IDCMPFlags), 0u);
    CHECK_EQ(r.get32(w1 + wd_UserPort), shared);

    const u32 remaining = get_msg(r, shared);
    CHECK(remaining != 0);
    CHECK_EQ(r.get32(remaining + im_IDCMPWindow), w2);
    reply_msg(r, remaining);
    CHECK_EQ(get_msg(r, shared), 0u);

    r.cpu().a[0] = w1;
    r.cpu().d[0] = IDCMP_MOUSEBUTTONS;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ModifyIDCMP), 1u);
    CHECK_EQ(r.get32(w1 + wd_UserPort), shared);
    r.intuition.pointer_event(50, 70, kMouseLeft, kMouseLeft, 0, false);
    const u32 again = get_msg(r, shared);
    CHECK(again != 0);
    CHECK_EQ(r.get32(again + im_IDCMPWindow), w1);
    reply_msg(r, again);

    r.intuition.close_window(w2);
    r.intuition.close_window(w1);
    r.exec.delete_port(shared);
    CHECK_EQ(avail_mem(r), before);
}

void buttons_go_to_the_active_window_not_the_one_under_the_pointer() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);

    put_new_window(r, kNewWindow, 20, 30, 100, 80, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS);
    const u32 active = open_win(r, kNewWindow);
    put_new_window(r, kNewWindow, 200, 50, 100, 80, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS);
    const u32 under = open_win(r, kNewWindow);
    CHECK(active != 0 && under != 0);
    CHECK_EQ(r.intuition.active_window(), active);

    const u32 active_port = r.get32(active + wd_UserPort);
    const u32 under_port = r.get32(under + wd_UserPort);

    r.intuition.pointer_event(230, 90, 0, 0, kMouseLeft, false);
    r.intuition.pointer_event(230, 90, kMouseRight, kMouseRight, 0, false);
    r.intuition.pointer_event(230, 90, 0, 0, kMouseRight, false);
    r.intuition.pointer_event(230, 90, kMouseMiddle, kMouseMiddle, 0, false);
    r.intuition.pointer_event(230, 90, 0, 0, kMouseMiddle, false);

    CHECK_EQ(get_msg(r, under_port), 0u);
    const u32 codes[5] = {SELECTUP, MENUDOWN, MENUUP, MIDDLEDOWN, MIDDLEUP};
    for (u32 expected : codes) {
        const u32 msg = get_msg(r, active_port);
        CHECK(msg != 0);
        CHECK_EQ(r.get32(msg + im_Class), IDCMP_MOUSEBUTTONS);
        CHECK_EQ(r.get16(msg + im_Code), expected);
        CHECK_EQ(static_cast<i16>(r.get16(msg + im_MouseX)), 210);
        CHECK_EQ(static_cast<i16>(r.get16(msg + im_MouseY)), 60);
        CHECK_EQ(r.get32(msg + im_IDCMPWindow), active);
        reply_msg(r, msg);
    }
    CHECK_EQ(get_msg(r, active_port), 0u);
}

void drag_release_outside_still_reaches_the_active_window() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);

    put_new_window(r, kNewWindow, 100, 120, 100, 80, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 port = r.get32(window + wd_UserPort);

    r.intuition.pointer_event(130, 150, kMouseLeft, kMouseLeft, 0, false);
    const u32 down = get_msg(r, port);
    CHECK(down != 0);
    CHECK_EQ(r.get16(down + im_Code), SELECTDOWN);
    CHECK_EQ(static_cast<i16>(r.get16(down + im_MouseX)), 30);
    CHECK_EQ(static_cast<i16>(r.get16(down + im_MouseY)), 30);
    reply_msg(r, down);

    r.intuition.pointer_event(40, 50, 0, 0, kMouseLeft, true);
    const u32 up_neg = get_msg(r, port);
    CHECK(up_neg != 0);
    CHECK_EQ(r.get32(up_neg + im_Class), IDCMP_MOUSEBUTTONS);
    CHECK_EQ(r.get16(up_neg + im_Code), SELECTUP);
    CHECK_EQ(static_cast<i16>(r.get16(up_neg + im_MouseX)), -60);
    CHECK_EQ(static_cast<i16>(r.get16(up_neg + im_MouseY)), -70);
    CHECK_EQ(r.get32(up_neg + im_IDCMPWindow), window);
    reply_msg(r, up_neg);

    r.intuition.pointer_event(110, 130, kMouseLeft, kMouseLeft, 0, false);
    const u32 down2 = get_msg(r, port);
    CHECK(down2 != 0);
    reply_msg(r, down2);

    r.intuition.pointer_event(350, 300, 0, 0, kMouseLeft, true);
    const u32 up_far = get_msg(r, port);
    CHECK(up_far != 0);
    CHECK_EQ(r.get16(up_far + im_Code), SELECTUP);
    CHECK_EQ(static_cast<i16>(r.get16(up_far + im_MouseX)), 250);
    CHECK_EQ(static_cast<i16>(r.get16(up_far + im_MouseY)), 180);
    CHECK_EQ(r.get32(up_far + im_IDCMPWindow), window);
    reply_msg(r, up_far);
}

void press_in_inactive_window_activates_then_delivers_selectdown() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);

    put_new_window(r, kNewWindow, 20, 30, 100, 80, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_INACTIVEWINDOW | IDCMP_MOUSEBUTTONS);
    const u32 w1 = open_win(r, kNewWindow);
    put_new_window(r, kNewWindow, 200, 40, 100, 80, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_ACTIVEWINDOW | IDCMP_MOUSEBUTTONS);
    const u32 w2 = open_win(r, kNewWindow);
    CHECK(w1 != 0 && w2 != 0);
    CHECK_EQ(r.intuition.active_window(), w1);

    r.intuition.pointer_event(225, 55, kMouseLeft, kMouseLeft, 0, false);
    CHECK_EQ(r.intuition.active_window(), w2);

    const u32 p1 = r.get32(w1 + wd_UserPort);
    const u32 inactive = get_msg(r, p1);
    CHECK(inactive != 0);
    CHECK_EQ(r.get32(inactive + im_Class), IDCMP_INACTIVEWINDOW);
    CHECK_EQ(r.get32(inactive + im_IDCMPWindow), w1);
    reply_msg(r, inactive);
    CHECK_EQ(get_msg(r, p1), 0u);

    const u32 p2 = r.get32(w2 + wd_UserPort);
    const u32 active = get_msg(r, p2);
    CHECK(active != 0);
    CHECK_EQ(r.get32(active + im_Class), IDCMP_ACTIVEWINDOW);
    CHECK_EQ(r.get32(active + im_IDCMPWindow), w2);
    reply_msg(r, active);

    const u32 down = get_msg(r, p2);
    CHECK(down != 0);
    CHECK_EQ(r.get32(down + im_Class), IDCMP_MOUSEBUTTONS);
    CHECK_EQ(r.get16(down + im_Code), SELECTDOWN);
    CHECK_EQ(static_cast<i16>(r.get16(down + im_MouseX)), 25);
    CHECK_EQ(static_cast<i16>(r.get16(down + im_MouseY)), 15);
    CHECK_EQ(r.get32(down + im_IDCMPWindow), w2);
    reply_msg(r, down);
    CHECK_EQ(get_msg(r, p2), 0u);
}

void menu_and_middle_buttons_go_to_the_active_window() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);

    put_new_window(r, kNewWindow, 100, 100, 120, 80, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS);
    const u32 w1 = open_win(r, kNewWindow);
    put_new_window(r, kNewWindow, 20, 20, 60, 60, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS);
    const u32 w2 = open_win(r, kNewWindow);
    CHECK(w1 != 0 && w2 != 0);

    r.intuition.pointer_event(30, 40, kMouseRight, kMouseRight, 0, false);
    r.intuition.pointer_event(30, 40, 0, 0, kMouseRight, false);
    r.intuition.pointer_event(10, 15, kMouseMiddle, kMouseMiddle, 0, false);
    r.intuition.pointer_event(10, 15, 0, 0, kMouseMiddle, false);

    CHECK_EQ(r.intuition.active_window(), w1);
    CHECK_EQ(get_msg(r, r.get32(w2 + wd_UserPort)), 0u);

    const u32 p1 = r.get32(w1 + wd_UserPort);
    const u32 codes[4] = {MENUDOWN, MENUUP, MIDDLEDOWN, MIDDLEUP};
    const i16 xs[4] = {-70, -70, -90, -90};
    const i16 ys[4] = {-60, -60, -85, -85};
    for (u32 i = 0; i < 4; ++i) {
        const u32 msg = get_msg(r, p1);
        CHECK(msg != 0);
        CHECK_EQ(r.get32(msg + im_Class), IDCMP_MOUSEBUTTONS);
        CHECK_EQ(r.get16(msg + im_Code), codes[i]);
        CHECK_EQ(static_cast<i16>(r.get16(msg + im_MouseX)), xs[i]);
        CHECK_EQ(static_cast<i16>(r.get16(msg + im_MouseY)), ys[i]);
        CHECK_EQ(r.get32(msg + im_IDCMPWindow), w1);
        reply_msg(r, msg);
    }
    CHECK_EQ(get_msg(r, p1), 0u);
}

void reclaim_closes_windows_and_screens_but_keeps_the_given_ones() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 keep_scr = open_new(r, kNewScreen);
    CHECK(keep_scr != 0);
    put_new_window(r, kNewWindow, 10, 20, 100, 60, 0, 0, 0, 0);
    const u32 keep_win = open_win(r, kNewWindow);
    CHECK(keep_win != 0);
    const u32 baseline = avail_mem(r);

    r.str(kWinTitle, "Extra");
    put_new_window(r, kNewWindow, 150, 40, 120, 80, 0, kWinTitle, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS);
    const u32 extra_win1 = open_win(r, kNewWindow);
    CHECK(extra_win1 != 0);
    r.intuition.pointer_event(170, 60, kMouseLeft, kMouseLeft, 0, false);

    r.str(kTitle, "ProgramScreen");
    put_new_screen(r, kNewScreen, 320, 200, 2, 0, 1, kTitle);
    const u32 prog_scr = open_new(r, kNewScreen);
    CHECK(prog_scr != 0);
    put_new_window(r, kNewWindow, 20, 20, 80, 50, 0, kWinTitle, prog_scr, CUSTOMSCREEN);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS);
    const u32 extra_win2 = open_win(r, kNewWindow);
    CHECK(extra_win2 != 0);
    r.intuition.pointer_event(40, 40, kMouseLeft, kMouseLeft, 0, false);

    r.intuition.reclaim_owned_by(r.exec.this_task(), keep_win, keep_scr);
    CHECK(r.intuition.window_open(keep_win));
    CHECK(!r.intuition.window_open(extra_win1));
    CHECK(!r.intuition.window_open(extra_win2));
    CHECK_EQ(r.intuition.front_screen(), keep_scr);
    CHECK_EQ(r.display.front_screen, keep_scr);
    CHECK_EQ(r.get32(keep_scr + sc_FirstWindow), keep_win);
    CHECK_EQ(r.get32(keep_win + wd_NextWindow), 0u);
    CHECK_EQ(r.get32(keep_scr), 0u);
    CHECK_EQ(avail_mem(r), baseline);

    r.intuition.reclaim_owned_by(r.exec.this_task(), 0, 0);
    CHECK(!r.intuition.window_open(keep_win));
    CHECK_EQ(r.intuition.front_screen(), 0u);
    CHECK_EQ(r.display.front_screen, 0u);
}

void reclaim_frees_delivered_unreplied_messages_and_balances_memory() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 keep_scr = open_new(r, kNewScreen);
    CHECK(keep_scr != 0);
    put_new_window(r, kNewWindow, 10, 20, 100, 60, 0, 0, 0, 0);
    const u32 keep_win = open_win(r, kNewWindow);
    CHECK(keep_win != 0);
    const u32 baseline = avail_mem(r);

    put_new_window(r, kNewWindow, 120, 50, 160, 90, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS | IDCMP_ACTIVEWINDOW);
    const u32 prog_win = open_win(r, kNewWindow);
    CHECK(prog_win != 0);
    const u32 port = r.get32(prog_win + wd_UserPort);

    r.intuition.pointer_event(140, 70, kMouseLeft, kMouseLeft, 0, false);
    r.intuition.pointer_event(140, 70, 0, 0, kMouseLeft, false);
    r.intuition.pointer_event(140, 70, kMouseRight, kMouseRight, 0, false);
    r.intuition.pointer_event(140, 70, 0, 0, kMouseRight, false);

    // Dequeue two messages without replying, and leave the rest queued.
    CHECK(get_msg(r, port) != 0);
    CHECK(get_msg(r, port) != 0);

    r.intuition.reclaim_owned_by(r.exec.this_task(), keep_win, keep_scr);
    CHECK_EQ(avail_mem(r), baseline);

    // All message slots are free again.
    put_new_window(r, kNewWindow, 120, 50, 160, 90, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS);
    const u32 check_win = open_win(r, kNewWindow);
    CHECK(check_win != 0);
    for (u32 i = 0; i < Intuition::kMaxMessages; ++i) {
        r.intuition.pointer_event(140, 70, kMouseLeft, kMouseLeft, 0, false);
    }
    CHECK_EQ(r.intuition.dropped_messages(), 0u);
    r.intuition.reclaim_owned_by(r.exec.this_task(), keep_win, keep_scr);
    CHECK_EQ(avail_mem(r), baseline);
}

void reclaim_clears_a_closed_active_window() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 keep_scr = open_new(r, kNewScreen);
    CHECK(keep_scr != 0);
    put_new_window(r, kNewWindow, 10, 20, 100, 60, WFLG_ACTIVATE, 0, 0, 0);
    const u32 keep_win = open_win(r, kNewWindow);
    CHECK(keep_win != 0);
    CHECK_EQ(r.intuition.active_window(), keep_win);

    put_new_window(r, kNewWindow, 150, 40, 120, 80, WFLG_ACTIVATE | WFLG_CLOSEGADGET, 0, 0, 0);
    const u32 prog_win = open_win(r, kNewWindow);
    CHECK(prog_win != 0);
    CHECK_EQ(r.intuition.active_window(), prog_win);

    const Rect box = Intuition::close_box_rect(150, 40);
    r.intuition.pointer_event(box.x0 + 1, box.y0 + 1, kMouseLeft, kMouseLeft, 0, false);

    r.intuition.reclaim_owned_by(r.exec.this_task(), keep_win, keep_scr);
    CHECK_EQ(r.intuition.active_window(), 0u);
    CHECK(r.intuition.window_open(keep_win));

    // When keep_win is the active window, reclaim leaves it active.
    r.intuition.pointer_event(30, 40, kMouseLeft, kMouseLeft, 0, false);
    CHECK_EQ(r.intuition.active_window(), keep_win);
    put_new_window(r, kNewWindow, 150, 40, 120, 80, 0, 0, 0, 0);
    const u32 inactive_win = open_win(r, kNewWindow);
    CHECK(inactive_win != 0);
    r.intuition.reclaim_owned_by(r.exec.this_task(), keep_win, keep_scr);
    CHECK_EQ(r.intuition.active_window(), keep_win);
}

void reclaim_restores_the_front_screen() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 keep_scr = open_new(r, kNewScreen);
    CHECK(keep_scr != 0);

    put_new_screen(r, kNewScreen, 320, 200, 2, 0, 1, 0);
    const u32 s2 = open_new(r, kNewScreen);
    const u32 s3 = open_new(r, kNewScreen);
    CHECK(s2 != 0 && s3 != 0);
    CHECK_EQ(r.intuition.front_screen(), s3);

    r.cpu().a[0] = keep_scr;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::ScreenToBack), ExitReason::Halt);
    CHECK_EQ(r.intuition.front_screen(), s3);

    r.intuition.reclaim_owned_by(r.exec.this_task(), 0, keep_scr);
    CHECK_EQ(r.intuition.front_screen(), keep_scr);
    CHECK_EQ(r.display.front_screen, keep_scr);
    CHECK_EQ(r.get32(keep_scr), 0u);
}

void reclaim_owned_by_closes_only_that_owners_windows_screens_and_messages() {
    GfxRig r;
    constexpr u32 task_a = kData + 0x900;
    constexpr u32 task_b = kData + 0x980;

    r.exec.set_this_task(task_a);
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 keep_scr = open_new(r, kNewScreen);
    put_new_window(r, kNewWindow, 10, 20, 100, 60, 0, 0, 0, 0);
    const u32 keep_win = open_win(r, kNewWindow);
    CHECK(keep_scr != 0 && keep_win != 0);

    r.exec.set_this_task(task_b);
    put_new_screen(r, kNewScreen, 400, 300, 2, 0, 1, 0);
    const u32 other_scr = open_new(r, kNewScreen);
    put_new_window(r, kNewWindow, 30, 30, 90, 70, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS);
    const u32 other_win = open_win(r, kNewWindow);
    const u32 other_port = r.get32(other_win + wd_UserPort);
    r.intuition.pointer_event(60, 60, kMouseLeft, kMouseLeft, 0, false);
    CHECK(other_scr != 0 && other_win != 0 && other_port != 0);
    const u32 baseline = avail_mem(r);

    r.exec.set_this_task(task_a);
    put_new_screen(r, kNewScreen, 320, 200, 2, 0, 1, 0);
    const u32 gone_scr = open_new(r, kNewScreen);
    put_new_window(r, kNewWindow, 20, 20, 80, 50, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS);
    const u32 gone_win = open_win(r, kNewWindow);
    r.intuition.pointer_event(40, 40, kMouseLeft, kMouseLeft, 0, false);
    CHECK(gone_scr != 0 && gone_win != 0);
    CHECK_EQ(r.intuition.window_owner(gone_win), task_a);
    CHECK_EQ(r.intuition.window_owner(other_win), task_b);

    r.intuition.reclaim_owned_by(task_a, keep_win, keep_scr);

    CHECK(r.intuition.window_open(keep_win));
    CHECK(!r.intuition.window_open(gone_win));
    CHECK(r.intuition.window_open(other_win));
    CHECK_EQ(r.intuition.front_screen(), keep_scr);
    CHECK_EQ(r.display.front_screen, keep_scr);
    CHECK_EQ(r.get32(keep_scr + sc_FirstWindow), keep_win);
    CHECK_EQ(r.get32(other_scr + sc_FirstWindow), other_win);
    CHECK_EQ(avail_mem(r), baseline);

    // The other task's message is still queued.
    const u32 other_msg = get_msg(r, other_port);
    CHECK(other_msg != 0);
    CHECK_EQ(r.get32(other_msg + im_Class), IDCMP_MOUSEBUTTONS);
    reply_msg(r, other_msg);

    // The closed screen is unknown, so a window cannot be opened on it.
    put_new_window(r, kNewWindow, 0, 0, 40, 40, 0, 0, gone_scr, CUSTOMSCREEN);
    CHECK_EQ(open_win(r, kNewWindow), 0u);
}

void window_draws_into_private_bitmap() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);

    const u32 rp = r.get32(window + wd_RPort);
    const u32 layer = r.get32(window + wd_WLayer);
    CHECK(rp != 0 && layer != 0);
    const u32 win_bm = r.get32(rp + rp_BitMap);
    CHECK(win_bm != 0);
    CHECK(win_bm != screen + sc_BitMap);
    CHECK_EQ(r.get32(layer + ly_SuperBitMap), win_bm);
    CHECK_EQ(r.get16(layer + ly_MinX), 50u);
    CHECK_EQ(r.get16(layer + ly_MinY), 60u);
    CHECK_EQ(r.get16(layer + ly_MaxX), 249u);
    CHECK_EQ(r.get16(layer + ly_MaxY), 159u);
}

void rectfill_in_window_shows_on_screen() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 100, 50, 200, 100, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 rp = r.get32(window + wd_RPort);

    r.cpu().a[1] = rp;
    r.cpu().d[0] = 3;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetAPen), ExitReason::Halt);
    r.cpu().a[1] = rp;
    r.cpu().d[0] = 5;
    r.cpu().d[1] = 15;
    r.cpu().d[2] = 9;
    r.cpu().d[3] = 19;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::RectFill), ExitReason::Halt);

    for (i32 y = 65; y <= 69; ++y) {
        for (i32 x = 105; x <= 109; ++x) {
            CHECK_EQ(screen_pixel(r, screen, x, y), 3u);
        }
    }
    CHECK_EQ(screen_pixel(r, screen, 104, 65), 0u);
    CHECK_EQ(screen_pixel(r, screen, 110, 65), 0u);
    CHECK_EQ(screen_pixel(r, screen, 105, 64), 1u);
    CHECK_EQ(screen_pixel(r, screen, 105, 70), 0u);
}

void upper_window_covers_lower_after_draw() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 50, 200, 100, 0, 0, 0, 0);
    const u32 win_a = open_win(r, kNewWindow);
    put_new_window(r, kNewWindow, 100, 80, 200, 100, 0, 0, 0, 0);
    const u32 win_b = open_win(r, kNewWindow);
    CHECK(win_a != 0 && win_b != 0);

    const u32 rp_a = r.get32(win_a + wd_RPort);
    r.cpu().a[1] = rp_a;
    r.cpu().d[0] = 3;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetAPen), ExitReason::Halt);
    r.cpu().a[1] = rp_a;
    r.cpu().d[0] = 1;
    r.cpu().d[1] = 11;
    r.cpu().d[2] = 198;
    r.cpu().d[3] = 98;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::RectFill), ExitReason::Halt);

    CHECK_EQ(screen_pixel(r, screen, 60, 70), 3u);
    CHECK_EQ(screen_pixel(r, screen, 150, 120), 0u);
}

void close_reveals_lower_window() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 50, 200, 100, 0, 0, 0, 0);
    const u32 win_a = open_win(r, kNewWindow);
    put_new_window(r, kNewWindow, 100, 80, 200, 100, 0, 0, 0, 0);
    const u32 win_b = open_win(r, kNewWindow);
    CHECK(win_a != 0 && win_b != 0);

    const u32 rp_a = r.get32(win_a + wd_RPort);
    r.cpu().a[1] = rp_a;
    r.cpu().d[0] = 3;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetAPen), ExitReason::Halt);
    r.cpu().a[1] = rp_a;
    r.cpu().d[0] = 1;
    r.cpu().d[1] = 11;
    r.cpu().d[2] = 198;
    r.cpu().d[3] = 98;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::RectFill), ExitReason::Halt);
    CHECK_EQ(screen_pixel(r, screen, 150, 120), 0u);

    r.cpu().a[0] = win_b;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);
    CHECK_EQ(screen_pixel(r, screen, 150, 120), 3u);
}

void depth_gadget_repaints() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 50, 200, 100, WFLG_DEPTHGADGET, 0, 0, 0);
    const u32 win_a = open_win(r, kNewWindow);
    put_new_window(r, kNewWindow, 100, 80, 200, 100, WFLG_DEPTHGADGET, 0, 0, 0);
    const u32 win_b = open_win(r, kNewWindow);
    CHECK(win_a != 0 && win_b != 0);

    const u32 rp_a = r.get32(win_a + wd_RPort);
    r.cpu().a[1] = rp_a;
    r.cpu().d[0] = 3;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetAPen), ExitReason::Halt);
    r.cpu().a[1] = rp_a;
    r.cpu().d[0] = 1;
    r.cpu().d[1] = 11;
    r.cpu().d[2] = 198;
    r.cpu().d[3] = 98;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::RectFill), ExitReason::Halt);
    CHECK_EQ(screen_pixel(r, screen, 150, 120), 0u);

    r.intuition.window_to_front(win_a);
    CHECK_EQ(screen_pixel(r, screen, 150, 120), 3u);

    r.intuition.window_to_back(win_a);
    CHECK_EQ(screen_pixel(r, screen, 150, 120), 0u);
}

void screen_bar_survives_window_close() {
    GfxRig r;
    r.str(kTitle, "Workbench");
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, kTitle);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const std::vector<u8> before = screen_planes(r, screen);

    put_new_window(r, kNewWindow, 0, 0, 200, 60, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK(screen_planes(r, screen) != before);

    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);
    CHECK(screen_planes(r, screen) == before);
}

void batch_composes_once() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 rp = r.get32(window + wd_RPort);

    r.refreshes.clear();
    r.refreshed_screens.clear();
    r.intuition.begin_batch();
    r.cpu().a[1] = rp;
    r.cpu().d[0] = 3;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetAPen), ExitReason::Halt);
    for (u32 i = 0; i < 10; ++i) {
        r.cpu().a[1] = rp;
        r.cpu().d[0] = 5 + i * 4;
        r.cpu().d[1] = 15 + i * 2;
        r.cpu().d[2] = 8 + i * 4;
        r.cpu().d[3] = 18 + i * 2;
        CHECK_EQ(r.call(Base::Graphics, graphics_lvo::RectFill), ExitReason::Halt);
    }
    CHECK_EQ(r.refreshes.size(), 0u);
    r.intuition.end_batch();
    CHECK_EQ(r.refreshes.size(), 1u);
    CHECK_EQ(r.refreshed_screens[0], screen);
    CHECK_EQ(r.refreshes[0].x0, 55);
    CHECK_EQ(r.refreshes[0].y0, 75);
    CHECK_EQ(r.refreshes[0].x1, 94);
    CHECK_EQ(r.refreshes[0].y1, 96);
}

void size_gadget_sets_right_border() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, WFLG_SIZEGADGET, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.get8(window + wd_BorderRight), 14u);
    CHECK_EQ(r.get8(window + wd_BorderBottom), 2u);
    // The size image fills the corner of the two borders: its raised edge,
    // the dark corner and the light box inside it.
    CHECK_EQ(window_pixel(r, window, 186, 86), 2u);
    CHECK_EQ(window_pixel(r, window, 199, 99), 1u);
    CHECK_EQ(window_pixel(r, window, 196, 96), 0u);
    CHECK_EQ(window_pixel(r, window, 195, 90), 1u);
    CHECK_EQ(window_pixel(r, window, 190, 95), 1u);
    CHECK_EQ(window_pixel(r, window, 194, 94), 2u);
    // The sunken edge round the inside runs along the right border.
    CHECK_EQ(window_pixel(r, window, 186, 50), 2u);
    CHECK_EQ(window_pixel(r, window, 185, 50), 0u);
    CHECK_EQ(screen_pixel(r, screen, 245, 155), 1u);
    CHECK_EQ(screen_pixel(r, screen, 244, 154), 2u);
}

void size_bbottom_sets_bottom_border() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, WFLG_SIZEGADGET | WFLG_SIZEBBOTTOM, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.get8(window + wd_BorderBottom), 14u);
    CHECK_EQ(r.get8(window + wd_BorderRight), 2u);
    CHECK_EQ(window_pixel(r, window, 100, 86), 2u);
    CHECK_EQ(window_pixel(r, window, 100, 85), 0u);
}

void limits_zero_mean_initial() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.get16(window + wd_MinWidth), 200u);
    CHECK_EQ(r.get16(window + wd_MaxWidth), 200u);
    CHECK_EQ(r.get16(window + wd_MinHeight), 100u);
    CHECK_EQ(r.get16(window + wd_MaxHeight), 100u);
}

void limits_ffff_mean_screen() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    r.put16(kNewWindow + nw_MaxWidth, 0xFFFF);
    r.put16(kNewWindow + nw_MaxHeight, 0xFFFF);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.get16(window + wd_MaxWidth), 640u);
    CHECK_EQ(r.get16(window + wd_MaxHeight), 400u);
    const u32 rp = r.get32(window + wd_RPort);
    const u32 win_bm = r.get32(rp + rp_BitMap);
    CHECK_EQ(r.get16(win_bm + bm_BytesPerRow), 80u);
}

void limits_clamp_to_floor() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    r.put16(kNewWindow + nw_MinWidth, 5);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.get16(window + wd_MinWidth), 40u);
}

void tags_set_gadgets_and_limits() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const u32 tags = r.tags(
        kTagList, {{WA_Left, 50}, {WA_Top, 60}, {WA_Width, 200}, {WA_Height, 100},
                   {WA_SizeGadget, 1}, {WA_DragBar, 1}, {WA_MinWidth, 100},
                   {WA_MaxWidth, ~0u}, {TAG_DONE, 0}});
    const u32 window = open_win_tags(r, 0, tags);
    CHECK(window != 0);
    const u32 flags = r.get32(window + wd_Flags);
    CHECK((flags & WFLG_SIZEGADGET) != 0);
    CHECK((flags & WFLG_DRAGBAR) != 0);
    CHECK_EQ(r.get16(window + wd_MinWidth), 100u);
    CHECK_EQ(r.get16(window + wd_MaxWidth), 640u);
}

// A window at (200,100) 240x160 on a fresh 640x400 screen with the given
// flags, IDCMP and limits; returns the window.
u32 open_drag_window(GfxRig& r, u32 flags, u32 idcmp, u32 min_w, u32 min_h, u32 max_w,
                     u32 max_h) {
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 200, 100, 240, 160, flags, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, idcmp);
    r.put16(kNewWindow + nw_MinWidth, min_w);
    r.put16(kNewWindow + nw_MinHeight, min_h);
    r.put16(kNewWindow + nw_MaxWidth, max_w);
    r.put16(kNewWindow + nw_MaxHeight, max_h);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    return window;
}

void press(GfxRig& r, i32 x, i32 y) {
    r.intuition.pointer_event(x, y, kMouseLeft, kMouseLeft, 0, false);
}

void move_to(GfxRig& r, i32 x, i32 y) {
    r.intuition.pointer_event(x, y, kMouseLeft, 0, 0, true);
}

void release(GfxRig& r, i32 x, i32 y) {
    r.intuition.pointer_event(x, y, 0, 0, kMouseLeft, false);
}

// The IDCMP classes queued on the window's port, in order, replying to none.
std::vector<u32> queued_classes(GfxRig& r, u32 window, std::vector<u32>* messages = nullptr) {
    std::vector<u32> classes;
    const u32 port = r.get32(window + wd_UserPort);
    for (u32 i = 0; i < 64; ++i) {
        const u32 msg = get_msg(r, port);
        if (msg == 0) {
            break;
        }
        classes.push_back(r.get32(msg + im_Class));
        if (messages != nullptr) {
            messages->push_back(msg);
        }
    }
    return classes;
}

void drag_moves_window() {
    GfxRig r;
    const u32 window = open_drag_window(r, WFLG_DRAGBAR, 0, 0, 0, 0, 0);
    const u32 screen = r.get32(window + wd_WScreen);
    press(r, 220, 105);
    move_to(r, 270, 125);
    release(r, 270, 125);
    CHECK_EQ(r.get16(window + wd_LeftEdge), 250u);
    CHECK_EQ(r.get16(window + wd_TopEdge), 120u);
    const u32 layer = r.get32(window + wd_WLayer);
    CHECK_EQ(r.get16(layer + ly_MinX), 250u);
    CHECK_EQ(r.get16(layer + ly_MaxY), 279u);
    // The active title bar (FILLPEN, 3, inside the frame) is now at y 121..133.
    CHECK_EQ(screen_pixel(r, screen, 300, 125), 3u);
    CHECK_EQ(screen_pixel(r, screen, 300, 120), 2u);
    for (i32 y = 100; y < 120; ++y) {
        for (i32 x = 200; x < 250; ++x) {
            CHECK_EQ(screen_pixel(r, screen, x, y), 0u);
        }
    }
}

void drag_clamps_to_screen() {
    GfxRig r;
    const u32 window = open_drag_window(r, WFLG_DRAGBAR, 0, 0, 0, 0, 0);
    press(r, 220, 105);
    move_to(r, -50, -50);
    CHECK_EQ(r.get16(window + wd_LeftEdge), 0u);
    CHECK_EQ(r.get16(window + wd_TopEdge), 0u);
    move_to(r, 2000, 2000);
    release(r, 2000, 2000);
    CHECK_EQ(r.get16(window + wd_LeftEdge), 400u);
    CHECK_EQ(r.get16(window + wd_TopEdge), 240u);
}

void drag_under_other_window_keeps_order() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    put_new_window(r, kNewWindow, 20, 20, 100, 60, WFLG_DRAGBAR, 0, 0, 0);
    const u32 back = open_win(r, kNewWindow);
    put_new_window(r, kNewWindow, 300, 200, 200, 100, 0, 0, 0, 0);
    const u32 front = open_win(r, kNewWindow);
    CHECK(back != 0 && front != 0);
    const u32 rp = r.get32(back + wd_RPort);
    r.cpu().a[1] = rp;
    r.cpu().d[0] = 3;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetAPen), ExitReason::Halt);
    r.cpu().a[1] = rp;
    r.cpu().d[0] = 1;
    r.cpu().d[1] = 11;
    r.cpu().d[2] = 98;
    r.cpu().d[3] = 58;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::RectFill), ExitReason::Halt);
    // Drag the back window so that its interior overlaps the front one.
    press(r, 50, 25);
    move_to(r, 320, 215);
    release(r, 320, 215);
    CHECK_EQ(r.get16(back + wd_LeftEdge), 290u);
    // Overlap: the front window's interior, pen 0.
    CHECK_EQ(screen_pixel(r, screen, 330, 240), 0u);
    // Outside the front window: the back window's pen 3.
    CHECK_EQ(screen_pixel(r, screen, 295, 240), 3u);
}

void resize_grows_and_clears() {
    GfxRig r;
    const u32 window = open_drag_window(r, WFLG_SIZEGADGET, 0, 0, 0, 0xFFFF, 0xFFFF);
    const u32 screen = r.get32(window + wd_WScreen);
    // Mark the interior next to the old right border.
    const u32 rp = r.get32(window + wd_RPort);
    r.cpu().a[1] = rp;
    r.cpu().d[0] = 3;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetAPen), ExitReason::Halt);
    r.cpu().a[1] = rp;
    r.cpu().d[0] = 1;
    r.cpu().d[1] = 11;
    r.cpu().d[2] = 228;
    r.cpu().d[3] = 148;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::RectFill), ExitReason::Halt);
    press(r, 435, 255);
    move_to(r, 475, 285);
    release(r, 475, 285);
    CHECK_EQ(r.get16(window + wd_Width), 280u);
    CHECK_EQ(r.get16(window + wd_Height), 190u);
    // The kept interior keeps its pixels, the new area and the old border
    // strips are cleared.
    CHECK_EQ(window_pixel(r, window, 100, 100), 3u);
    CHECK_EQ(window_pixel(r, window, 232, 100), 0u);
    CHECK_EQ(window_pixel(r, window, 250, 170), 0u);
    CHECK_EQ(window_pixel(r, window, 100, 155), 0u);
    // The size image at the new corner, in the window and on screen: the
    // fill pen of the active window round the dark corner and light box.
    CHECK_EQ(window_pixel(r, window, 276, 186), 3u);
    CHECK_EQ(window_pixel(r, window, 275, 185), 1u);
    CHECK_EQ(window_pixel(r, window, 274, 184), 2u);
    CHECK_EQ(screen_pixel(r, screen, 476, 286), 3u);
    // The old gadget position is interior now.
    CHECK_EQ(screen_pixel(r, screen, 435, 255), 0u);
}

void resize_clamps_to_limits() {
    GfxRig r;
    const u32 window = open_drag_window(r, WFLG_SIZEGADGET, 0, 100, 50, 0xFFFF, 0xFFFF);
    press(r, 435, 255);
    move_to(r, 200, 100);
    CHECK_EQ(r.get16(window + wd_Width), 100u);
    CHECK_EQ(r.get16(window + wd_Height), 50u);
    move_to(r, 900, 900);
    release(r, 900, 900);
    CHECK_EQ(r.get16(window + wd_Width), 440u);
    CHECK_EQ(r.get16(window + wd_Height), 300u);
}

void window_limits_set_and_report_limits() {
    GfxRig r;
    const u32 window = open_drag_window(r, WFLG_SIZEGADGET, 0, 0, 0, 0xFFFF, 0xFFFF);
    r.cpu().a[0] = window;
    r.cpu().d[0] = 100;
    r.cpu().d[1] = 50;
    r.cpu().d[2] = 300;
    r.cpu().d[3] = 200;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::WindowLimits), 1u);
    CHECK_EQ(r.get16(window + wd_MinWidth), 100u);
    CHECK_EQ(r.get16(window + wd_MinHeight), 50u);
    CHECK_EQ(r.get16(window + wd_MaxWidth), 300u);
    CHECK_EQ(r.get16(window + wd_MaxHeight), 200u);

    // Zero leaves that limit as it was.
    r.cpu().d[0] = 0;
    r.cpu().d[1] = 0;
    r.cpu().d[2] = 0;
    r.cpu().d[3] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::WindowLimits), 1u);
    CHECK_EQ(r.get16(window + wd_MinWidth), 100u);
    CHECK_EQ(r.get16(window + wd_MinHeight), 50u);
    CHECK_EQ(r.get16(window + wd_MaxWidth), 300u);
    CHECK_EQ(r.get16(window + wd_MaxHeight), 200u);

    // A minimum over the window's width and a maximum under its height are
    // left out; the minimum height asked for with them is still set.
    r.cpu().d[0] = 400;
    r.cpu().d[1] = 60;
    r.cpu().d[2] = 0;
    r.cpu().d[3] = 100;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::WindowLimits), 0u);
    CHECK_EQ(r.get16(window + wd_MinWidth), 100u);
    CHECK_EQ(r.get16(window + wd_MinHeight), 60u);
    CHECK_EQ(r.get16(window + wd_MaxWidth), 300u);
    CHECK_EQ(r.get16(window + wd_MaxHeight), 200u);

    // A resize drag afterwards stops at the new limits.
    press(r, 435, 255);
    move_to(r, 200, 100);
    CHECK_EQ(r.get16(window + wd_Width), 100u);
    CHECK_EQ(r.get16(window + wd_Height), 60u);
    move_to(r, 900, 900);
    release(r, 900, 900);
    CHECK_EQ(r.get16(window + wd_Width), 300u);
    CHECK_EQ(r.get16(window + wd_Height), 200u);
}

void newsize_coalesced_and_final_on_release() {
    GfxRig r;
    const u32 window = open_drag_window(r, WFLG_SIZEGADGET, IDCMP_NEWSIZE | IDCMP_CHANGEWINDOW,
                                        0, 0, 0xFFFF, 0xFFFF);
    press(r, 435, 255);
    for (i32 i = 1; i <= 5; ++i) {
        move_to(r, 435 + i * 4, 255 + i * 2);
    }
    std::vector<u32> messages;
    std::vector<u32> classes = queued_classes(r, window, &messages);
    CHECK_EQ(classes.size(), 1u);
    CHECK_EQ(classes.size() > 0 ? classes[0] : 0u, IDCMP_NEWSIZE);
    // Replying frees the slot; a further step posts again.
    for (u32 msg : messages) {
        reply_msg(r, msg);
    }
    move_to(r, 460, 270);
    classes = queued_classes(r, window, &messages);
    CHECK_EQ(classes.size(), 1u);
    // Not replied: steps and the release post no NEWSIZE, only CHANGEWINDOW.
    const u32 pending = messages.back();
    move_to(r, 470, 275);
    move_to(r, 480, 280);
    CHECK_EQ(queued_classes(r, window).size(), 0u);
    release(r, 480, 280);
    classes = queued_classes(r, window);
    CHECK_EQ(classes.size(), 1u);
    CHECK_EQ(classes.size() > 0 ? classes[0] : 0u, IDCMP_CHANGEWINDOW);
    CHECK_EQ(r.get16(window + wd_Width), 285u);
    CHECK_EQ(r.get16(window + wd_Height), 185u);
    // The reply to the stale NEWSIZE brings the final size.
    reply_msg(r, pending);
    classes = queued_classes(r, window);
    CHECK_EQ(classes.size(), 1u);
    CHECK_EQ(classes.size() > 0 ? classes[0] : 0u, IDCMP_NEWSIZE);
}

// A guest that never replies keeps one NEWSIZE and one CHANGEWINDOW per
// gesture at most: 100 gestures do not use up the message pool.
void unreplied_resizes_do_not_exhaust_messages() {
    GfxRig r;
    const u32 window = open_drag_window(r, WFLG_SIZEGADGET, IDCMP_NEWSIZE, 0, 0, 0xFFFF,
                                        0xFFFF);
    for (i32 i = 0; i < 100; ++i) {
        const i32 grow = (i % 2 == 0) ? 10 : -10;
        const i32 right = 200 + static_cast<i32>(r.get16(window + wd_Width)) - 1;
        const i32 bottom = 100 + static_cast<i32>(r.get16(window + wd_Height)) - 1;
        press(r, right - 4, bottom - 4);
        move_to(r, right - 4 + grow, bottom - 4 + grow);
        release(r, right - 4 + grow, bottom - 4 + grow);
    }
    CHECK_EQ(r.intuition.dropped_messages(), 0u);
    CHECK_EQ(queued_classes(r, window).size(), 1u);
}

void move_posts_changewindow_only() {
    GfxRig r;
    const u32 window = open_drag_window(r, WFLG_DRAGBAR, IDCMP_NEWSIZE | IDCMP_CHANGEWINDOW, 0,
                                        0, 0, 0);
    press(r, 220, 105);
    move_to(r, 230, 115);
    release(r, 230, 115);
    const std::vector<u32> classes = queued_classes(r, window);
    CHECK_EQ(classes.size(), 1u);
    CHECK_EQ(classes.size() > 0 ? classes[0] : 0u, IDCMP_CHANGEWINDOW);
}

void drag_press_is_not_selectdown() {
    GfxRig r;
    const u32 window = open_drag_window(r, WFLG_DRAGBAR | WFLG_REPORTMOUSE,
                                        IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE, 0, 0, 0, 0);
    press(r, 220, 105);
    move_to(r, 240, 110);
    release(r, 240, 110);
    CHECK_EQ(queued_classes(r, window).size(), 0u);
    // A press in the interior is still a SELECTDOWN.
    press(r, 300, 200);
    const std::vector<u32> classes = queued_classes(r, window);
    CHECK_EQ(classes.size(), 1u);
    CHECK_EQ(classes.size() > 0 ? classes[0] : 0u, IDCMP_MOUSEBUTTONS);
}

u32 g_hook_calls = 0;
u32 g_hook_window = 0;

void count_hook(void* ctx, u32 window) {
    CHECK(ctx == &g_hook_calls);
    g_hook_window = window;
    ++g_hook_calls;
}

void size_hook_called_per_step() {
    GfxRig r;
    const u32 window = open_drag_window(r, WFLG_SIZEGADGET, 0, 0, 0, 0xFFFF, 0xFFFF);
    g_hook_calls = 0;
    r.intuition.set_size_hook(window, &count_hook, &g_hook_calls);
    r.intuition.size_window_to(window, 300, 200);
    r.intuition.size_window_to(window, 300, 200);
    r.intuition.size_window_to(window, 250, 180);
    CHECK_EQ(g_hook_calls, 2u);
    CHECK_EQ(g_hook_window, window);
}

void close_during_drag_ends_drag() {
    GfxRig r;
    const u32 window = open_drag_window(r, WFLG_DRAGBAR, 0, 0, 0, 0, 0);
    press(r, 220, 105);
    move_to(r, 230, 110);
    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);
    move_to(r, 260, 130);
    release(r, 260, 130);
    // A new window where the old one was: a move without a press drags
    // nothing.
    put_new_window(r, kNewWindow, 200, 100, 240, 160, WFLG_DRAGBAR, 0, 0, 0);
    const u32 again = open_win(r, kNewWindow);
    CHECK(again != 0);
    move_to(r, 300, 300);
    CHECK_EQ(r.get16(again + wd_LeftEdge), 200u);
}

void put_prop_info(GfxRig& r, u32 at, u32 flags, u32 hpot, u32 vpot, u32 hbody, u32 vbody) {
    for (u32 i = 0; i < PropInfo_SIZEOF; ++i) {
        r.put8(at + i, 0);
    }
    r.put16(at + pi_Flags, flags);
    r.put16(at + pi_HorizPot, hpot);
    r.put16(at + pi_VertPot, vpot);
    r.put16(at + pi_HorizBody, hbody);
    r.put16(at + pi_VertBody, vbody);
}

void put_gadget(GfxRig& r, u32 at, u32 next, i32 left, i32 top, i32 width, i32 height, u32 flags,
                u32 activation, u32 type, u32 special) {
    for (u32 i = 0; i < Gadget_SIZEOF; ++i) {
        r.put8(at + i, 0);
    }
    r.put32(at + gg_NextGadget, next);
    r.put16(at + gg_LeftEdge, static_cast<u32>(left));
    r.put16(at + gg_TopEdge, static_cast<u32>(top));
    r.put16(at + gg_Width, static_cast<u32>(width));
    r.put16(at + gg_Height, static_cast<u32>(height));
    r.put16(at + gg_Flags, flags);
    r.put16(at + gg_Activation, activation);
    r.put16(at + gg_GadgetType, type);
    r.put32(at + gg_SpecialInfo, special);
}

// A gadget in the left border makes the border wide enough for it, and the
// frame and the inner area follow the wider border. A gadget placed from
// the right edge, or no gadget at all, leaves the default border.
void a_left_border_gadget_widens_the_window_border() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const u32 gad = r.exec.alloc(Gadget_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    CHECK(gad != 0);
    put_gadget(r, gad, 0, 4, 17, 8, 50, GFLG_GADGIMAGE | GFLG_GADGHNONE, GACT_LEFTBORDER,
               GTYP_BOOLGADGET, 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_FirstGadget, gad);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.get8(window + wd_BorderLeft), 14u);
    // The inner line of the frame is at the new border and the interior
    // starts past it.
    CHECK_EQ(window_pixel(r, window, 12, 40), 3u);
    CHECK_EQ(window_pixel(r, window, 13, 40), 1u);
    CHECK_EQ(window_pixel(r, window, 14, 40), 0u);

    put_gadget(r, gad, 0, 4, 17, 8, 50, GFLG_GADGIMAGE | GFLG_GADGHNONE | GFLG_RELRIGHT,
               GACT_LEFTBORDER, GTYP_BOOLGADGET, 0);
    put_new_window(r, kNewWindow, 300, 60, 200, 100, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_FirstGadget, gad);
    const u32 right = open_win(r, kNewWindow);
    CHECK_EQ(r.get8(right + wd_BorderLeft), 2u);

    put_new_window(r, kNewWindow, 50, 200, 200, 100, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    const u32 plain = open_win(r, kNewWindow);
    CHECK_EQ(r.get8(plain + wd_BorderLeft), 2u);
    CHECK_EQ(window_pixel(r, plain, 1, 40), 1u);
    CHECK_EQ(window_pixel(r, plain, 2, 40), 0u);

    for (const u32 close : {window, right, plain}) {
        r.cpu().a[0] = close;
        CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);
    }
    r.exec.free(gad, Gadget_SIZEOF);
}

constexpr u32 kProp1 = kData + 0x700;
constexpr u32 kGad1 = kData + 0x740;
constexpr u32 kProp2 = kData + 0x780;
constexpr u32 kGad2 = kData + 0x7C0;
constexpr u32 kProp3 = kData + 0x800;
constexpr u32 kGad3 = kData + 0x840;
constexpr u32 kProp4 = kData + 0x880;
constexpr u32 kGad4 = kData + 0x8C0;

GfxRig* g_rig_for_render = nullptr;
u32 g_renders = 0;
u32 g_rendered_flags = 0;

bool count_render(void*, u32, u32, u32 gadget) {
    ++g_renders;
    g_rendered_flags = g_rig_for_render->get16(gadget + gg_Flags);
    return true;
}

void a_boolean_gadget_reports_its_release() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    const u32 gad = r.exec.alloc(Gadget_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    CHECK(gad != 0);
    put_gadget(r, gad, 0, 20, 20, 40, 14, GFLG_GADGHCOMP, GACT_RELVERIFY | GACT_IMMEDIATE,
               GTYP_BOOLGADGET, 0);
    put_new_window(r, kNewWindow, 50, 50, 200, 100, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_GADGETUP | IDCMP_GADGETDOWN);
    r.put32(kNewWindow + nw_FirstGadget, gad);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(window != 0);
    const u32 port = r.get32(window + wd_UserPort);
    const u32 rp = r.get32(window + wd_RPort);
    r.put8(rp + rp_FgPen, 2);
    r.put8(rp + rp_DrawMode, JAM2);
    const u32 plain = window_pixel(r, window, 25, 25);

    // Pressed: selected, its box complemented, GADGETDOWN for IMMEDIATE.
    r.intuition.pointer_event(75, 75, 1, 1, 0, true);
    u32 msg = get_msg(r, port);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_GADGETDOWN);
    CHECK_EQ(r.get32(msg + im_IAddress), gad);
    reply_msg(r, msg);
    CHECK_EQ(r.get16(gad + gg_Flags) & GFLG_SELECTED, GFLG_SELECTED);
    CHECK_EQ(window_pixel(r, window, 25, 25), ~plain & 3u);
    CHECK_EQ(r.get8(rp + rp_FgPen), 2u);
    CHECK_EQ(r.get8(rp + rp_DrawMode), JAM2);

    // Let go over it: GADGETUP, and the box as it was.
    r.intuition.pointer_event(75, 75, 0, 0, 1, false);
    msg = get_msg(r, port);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_GADGETUP);
    CHECK_EQ(r.get32(msg + im_IAddress), gad);
    reply_msg(r, msg);
    CHECK_EQ(r.get16(gad + gg_Flags) & GFLG_SELECTED, 0u);
    CHECK_EQ(window_pixel(r, window, 25, 25), plain);

    // Let go away from it: no GADGETUP.
    r.intuition.pointer_event(75, 75, 1, 1, 0, true);
    reply_msg(r, get_msg(r, port));
    r.intuition.pointer_event(200, 120, 1, 0, 0, true);
    r.intuition.pointer_event(200, 120, 0, 0, 1, false);
    CHECK_EQ(get_msg(r, port), 0u);
    CHECK_EQ(r.get16(gad + gg_Flags) & GFLG_SELECTED, 0u);
    CHECK_EQ(window_pixel(r, window, 25, 25), plain);

    // A disabled gadget takes no clicks.
    r.put16(gad + gg_Flags, GFLG_GADGHCOMP | GFLG_DISABLED);
    r.intuition.pointer_event(75, 75, 1, 1, 0, true);
    r.intuition.pointer_event(75, 75, 0, 0, 1, false);
    CHECK_EQ(get_msg(r, port), 0u);

    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);
    r.exec.free(gad, Gadget_SIZEOF);
}

void a_gadtools_gadget_is_drawn_by_its_renderer() {
    GfxRig r;
    g_rig_for_render = &r;
    g_renders = 0;
    r.intuition.set_gadget_renderer(&count_render, nullptr);
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    const u32 gad = r.exec.alloc(Gadget_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    put_gadget(r, gad, 0, 20, 20, 40, 14, GFLG_GADGHNONE, GACT_RELVERIFY,
               GTYP_GADTOOLS | GTYP_BOOLGADGET, 0);
    put_new_window(r, kNewWindow, 50, 50, 200, 100, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_GADGETUP);
    r.put32(kNewWindow + nw_FirstGadget, gad);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(window != 0);
    CHECK_EQ(g_renders, 1u);  // drawn as the window opened

    r.intuition.pointer_event(75, 75, 1, 1, 0, true);
    CHECK_EQ(g_renders, 2u);
    CHECK_EQ(g_rendered_flags & GFLG_SELECTED, GFLG_SELECTED);
    r.intuition.pointer_event(75, 75, 0, 0, 1, false);
    CHECK_EQ(g_renders, 3u);
    CHECK_EQ(g_rendered_flags & GFLG_SELECTED, 0u);
    const u32 msg = get_msg(r, r.get32(window + wd_UserPort));
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_GADGETUP);
    reply_msg(r, msg);
    CHECK(!logged("intuition: gadget type"));

    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);
    r.exec.free(gad, Gadget_SIZEOF);
    r.intuition.set_gadget_renderer(nullptr, nullptr);
    g_rig_for_render = nullptr;
}

void inner_size_tags_size_the_window_around_its_inside() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    const u32 tags = r.tags(kTagList, {{WA_Left, 10}, {WA_Top, 20}, {WA_InnerWidth, 150},
                                       {WA_InnerHeight, 60}, {WA_DragBar, 1},
                                       {WA_CloseGadget, 1}, {TAG_DONE, 0}});
    const u32 window = open_win_tags(r, 0, tags);
    CHECK(window != 0);
    const u32 left = r.get8(window + wd_BorderLeft);
    const u32 right = r.get8(window + wd_BorderRight);
    const u32 top = r.get8(window + wd_BorderTop);
    const u32 bottom = r.get8(window + wd_BorderBottom);
    CHECK_EQ(r.get16(window + wd_Width), 150 + left + right);
    CHECK_EQ(r.get16(window + wd_Height), 60 + top + bottom);
}

void open_window_links_and_draws_prop_gadget() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);

    const u32 gad = r.exec.alloc(Gadget_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    const u32 prop = r.exec.alloc(PropInfo_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    CHECK(gad != 0 && prop != 0);
    put_prop_info(r, prop, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 4);
    put_gadget(r, gad, 0, 20, 20, 16, 60, 0, 0, GTYP_PROPGADGET, prop);
    const u32 before_open = avail_mem(r);

    put_new_window(r, kNewWindow, 50, 50, 200, 100, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_FirstGadget, gad);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(window != 0);
    CHECK_EQ(r.get32(window + wd_FirstGadget), gad);

    // Container frame at (20,20)..(35,79), sunken: SHADOWPEN (1) above and left,
    // SHINEPEN (2) below and right.
    CHECK_EQ(window_pixel(r, window, 20, 20), 1u);
    CHECK_EQ(window_pixel(r, window, 35, 20), 2u);
    CHECK_EQ(window_pixel(r, window, 20, 79), 1u);
    CHECK_EQ(window_pixel(r, window, 35, 79), 2u);
    // Knob at top (inner 21..34 x 21..78, height = 58 * 0x4000 / 0xFFFF = 14 -> y 21..34):
    // raised, SHINEPEN above and left and SHADOWPEN below and right, round BACKGROUNDPEN (0).
    CHECK_EQ(window_pixel(r, window, 21, 21), 2u);
    CHECK_EQ(window_pixel(r, window, 25, 21), 2u);
    CHECK_EQ(window_pixel(r, window, 34, 25), 1u);
    CHECK_EQ(window_pixel(r, window, 25, 34), 1u);
    CHECK_EQ(window_pixel(r, window, 23, 23), 0u);
    CHECK_EQ(window_pixel(r, window, 32, 32), 0u);
    CHECK_EQ(screen_pixel(r, screen, 71, 71), 2u);
    // Container interior below knob is pen 0.
    CHECK_EQ(window_pixel(r, window, 25, 40), 0u);
    CHECK_EQ(window_pixel(r, window, 25, 78), 0u);

    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);
    CHECK_EQ(r.get32(window + wd_FirstGadget), 0u);
    CHECK_EQ(avail_mem(r), before_open);
    CHECK_EQ(r.get16(gad + gg_GadgetType), GTYP_PROPGADGET);
    CHECK_EQ(r.get16(prop + pi_VertBody), MAXBODY / 4);
    r.exec.free(prop, PropInfo_SIZEOF);
    r.exec.free(gad, Gadget_SIZEOF);
}

void wa_gadgets_tag_links_list() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);

    put_prop_info(r, kProp1, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 2);
    put_prop_info(r, kProp2, AUTOKNOB | FREEHORIZ, 0, 0, MAXBODY / 2, MAXBODY);
    put_gadget(r, kGad1, kGad2, 10, 15, 12, 40, 0, 0, GTYP_PROPGADGET, kProp1);
    put_gadget(r, kGad2, 0, 40, 15, 60, 12, 0, 0, GTYP_PROPGADGET, kProp2);

    const u32 tags = r.tags(kTagList, {{WA_Left, 20}, {WA_Top, 30}, {WA_Width, 160},
                                       {WA_Height, 80}, {WA_Gadgets, kGad1}, {TAG_DONE, 0}});
    const u32 window = open_win_tags(r, 0, tags);
    CHECK(window != 0);
    CHECK_EQ(r.get32(window + wd_FirstGadget), kGad1);
    CHECK_EQ(r.get32(kGad1 + gg_NextGadget), kGad2);
    CHECK_EQ(r.get32(kGad2 + gg_NextGadget), 0u);
    CHECK_EQ(window_pixel(r, window, 11, 16), 2u);
    CHECK_EQ(window_pixel(r, window, 41, 16), 2u);
}

void add_and_remove_glist_positions() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    put_new_window(r, kNewWindow, 20, 20, 200, 100, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);

    put_prop_info(r, kProp1, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 2);
    put_prop_info(r, kProp2, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 2);
    put_prop_info(r, kProp3, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 2);
    put_prop_info(r, kProp4, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 2);
    put_gadget(r, kGad1, kGad2, 10, 15, 10, 30, 0, 0, GTYP_PROPGADGET, kProp1);
    put_gadget(r, kGad2, 0, 30, 15, 10, 30, 0, 0, GTYP_PROPGADGET, kProp2);
    put_gadget(r, kGad3, 0, 50, 15, 10, 30, 0, 0, GTYP_PROPGADGET, kProp3);
    put_gadget(r, kGad4, 0, 70, 15, 10, 30, 0, 0, GTYP_PROPGADGET, kProp4);

    // Add only 1 gadget from kGad1 (ignoring kGad2 link).
    CHECK_EQ(r.intuition.add_glist(window, kGad1, -1, 1), 0u);
    CHECK_EQ(r.get32(kGad1 + gg_NextGadget), 0u);
    CHECK_EQ(r.intuition.add_glist(window, kGad2, -1, 1), 1u);
    CHECK_EQ(r.intuition.add_glist(window, kGad3, 0, 1), 0u);
    CHECK_EQ(r.intuition.add_glist(window, kGad4, 1, 1), 1u);

    CHECK_EQ(r.get32(window + wd_FirstGadget), kGad3);
    CHECK_EQ(r.get32(kGad3 + gg_NextGadget), kGad4);
    CHECK_EQ(r.get32(kGad4 + gg_NextGadget), kGad1);
    CHECK_EQ(r.get32(kGad1 + gg_NextGadget), kGad2);
    CHECK_EQ(r.get32(kGad2 + gg_NextGadget), 0u);

    CHECK_EQ(r.intuition.remove_glist(window, kGad4, 1), 1u);
    CHECK_EQ(r.intuition.remove_glist(window, kGad4, 1), 0xFFFFFFFFu);
    CHECK_EQ(r.intuition.remove_glist(window, kGad1, 2), 1u);
    CHECK_EQ(r.get32(window + wd_FirstGadget), kGad3);
    CHECK_EQ(r.get32(kGad3 + gg_NextGadget), 0u);
    CHECK_EQ(r.intuition.remove_glist(window, kGad3, 1), 0u);
    CHECK_EQ(r.get32(window + wd_FirstGadget), 0u);

    // Re-link two gadgets at once with count -1.
    r.put32(kGad1 + gg_NextGadget, kGad2);
    r.put32(kGad2 + gg_NextGadget, 0);
    CHECK_EQ(r.intuition.add_glist(window, kGad1, -1, -1), 0u);
    CHECK_EQ(r.get32(window + wd_FirstGadget), kGad1);
    CHECK_EQ(r.get32(kGad1 + gg_NextGadget), kGad2);
}

void new_modify_prop_moves_knob() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);

    put_prop_info(r, kProp1, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 4);
    put_gadget(r, kGad1, 0, 20, 20, 14, 50, 0, 0, GTYP_PROPGADGET, kProp1);
    put_new_window(r, kNewWindow, 50, 50, 160, 100, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_FirstGadget, kGad1);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(window != 0);

    // Initially knob is at top (inner y 21..68, knob_h = 11 -> y 21..31); its
    // top edge is SHINEPEN (2), its inside and the container's BACKGROUNDPEN (0).
    CHECK_EQ(window_pixel(r, window, 25, 21), 2u);
    CHECK_EQ(window_pixel(r, window, 25, 65), 0u);

    r.intuition.new_modify_prop(kGad1, window, AUTOKNOB | FREEVERT, 0, MAXPOT, MAXBODY,
                                MAXBODY / 4, 1);
    CHECK_EQ(r.get16(kProp1 + pi_VertPot), MAXPOT);
    // Knob moved to bottom (y 58..68).
    CHECK_EQ(window_pixel(r, window, 25, 21), 0u);
    CHECK_EQ(window_pixel(r, window, 25, 57), 0u);
    CHECK_EQ(window_pixel(r, window, 25, 58), 2u);
}

void relative_geometry_follows_resize() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);

    put_prop_info(r, kProp1, AUTOKNOB | FREEVERT | PROPBORDERLESS, 0, 0, MAXBODY, MAXBODY / 4);
    put_gadget(r, kGad1, 0, -9, 16, 8, -26, GFLG_RELRIGHT | GFLG_RELHEIGHT, 0, GTYP_PROPGADGET,
               kProp1);
    put_new_window(r, kNewWindow, 40, 40, 200, 100, WFLG_SIZEGADGET, 0, 0, 0);
    r.put16(kNewWindow + nw_MaxWidth, 0xFFFF);
    r.put16(kNewWindow + nw_MaxHeight, 0xFFFF);
    r.put32(kNewWindow + nw_FirstGadget, kGad1);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(window != 0);

    // At 200x100: box x = 190..197, y = 16..89; knob at x 191..196, y 17..34,
    // its top edge SHINEPEN.
    CHECK_EQ(window_pixel(r, window, 194, 17), 2u);
    CHECK_EQ(window_pixel(r, window, 194, 80), 0u);

    r.intuition.size_window_to(window, 300, 160);
    // At 300x160: box x = 290..297, y = 16..149; knob at x 291..296, y 17..49.
    CHECK_EQ(window_pixel(r, window, 194, 17), 0u);
    CHECK_EQ(window_pixel(r, window, 294, 17), 2u);
    CHECK_EQ(window_pixel(r, window, 294, 140), 0u);
}

std::vector<u32> g_prop_pots;
u32 g_prop_hook_gadget = 0;

void record_prop_hook(void* ctx, u32 gadget, u32 vert_pot) {
    CHECK(ctx == &g_prop_pots);
    g_prop_hook_gadget = gadget;
    g_prop_pots.push_back(vert_pot);
}

void knob_drag_updates_pot_and_hook() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);

    put_prop_info(r, kProp1, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 4);
    // Window at (50, 50); gadget at (20, 20) 14x62 -> inner y 21..80 (h=60);
    // knob_h = 60 * 16383 / 65535 rounded down = 14, so the travel is 46.
    put_gadget(r, kGad1, 0, 20, 20, 14, 62, 0, GACT_IMMEDIATE | GACT_RELVERIFY, GTYP_PROPGADGET,
               kProp1);
    put_new_window(r, kNewWindow, 50, 50, 200, 120, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_FirstGadget, kGad1);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(window != 0);

    g_prop_pots.clear();
    g_prop_hook_gadget = 0;
    r.intuition.set_gadget_hook(kGad1, &record_prop_hook, &g_prop_pots);

    // Grab near bottom of knob (screen y = 50 + 30 = 80, offset = 9 inside 21..35).
    press(r, 77, 80);
    CHECK((r.get16(kProp1 + pi_Flags) & KNOBHIT) != 0);
    CHECK((r.get16(kGad1 + gg_Flags) & GFLG_SELECTED) != 0);

    // Move slightly down: grab offset preserved -> knob top moves 5px -> pot ~ 5/45 * 65535.
    move_to(r, 77, 85);
    move_to(r, 77, 95);
    move_to(r, 77, 110);
    move_to(r, 77, 126);
    release(r, 77, 126);

    CHECK((r.get16(kProp1 + pi_Flags) & KNOBHIT) == 0);
    CHECK((r.get16(kGad1 + gg_Flags) & GFLG_SELECTED) == 0);
    CHECK_EQ(g_prop_hook_gadget, kGad1);
    CHECK_EQ(g_prop_pots.size(), 4u);
    for (usize i = 1; i < g_prop_pots.size(); ++i) {
        CHECK(g_prop_pots[i] > g_prop_pots[i - 1]);
    }
    CHECK(g_prop_pots.back() >= MAXPOT - 1000);
    CHECK_EQ(r.get16(kProp1 + pi_VertPot), g_prop_pots.back());
}

void container_click_pages_by_body() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);

    put_prop_info(r, kProp1, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, 0x4000);
    put_gadget(r, kGad1, 0, 20, 20, 14, 62, 0, GACT_IMMEDIATE | GACT_RELVERIFY, GTYP_PROPGADGET,
               kProp1);
    put_new_window(r, kNewWindow, 50, 50, 200, 120, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_FirstGadget, kGad1);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(window != 0);

    g_prop_pots.clear();
    r.intuition.set_gadget_hook(kGad1, &record_prop_hook, &g_prop_pots);

    // Click below knob (screen y = 50 + 75 = 125).
    press(r, 77, 125);
    release(r, 77, 125);
    CHECK_EQ(r.get16(kProp1 + pi_VertPot), 0x4000u);

    press(r, 77, 125);
    release(r, 77, 125);
    CHECK_EQ(r.get16(kProp1 + pi_VertPot), 0x8000u);

    // Repeat to clamp at MAXPOT.
    press(r, 77, 128);
    release(r, 77, 128);
    press(r, 77, 128);
    release(r, 77, 128);
    CHECK_EQ(r.get16(kProp1 + pi_VertPot), MAXPOT);

    // Click above knob (screen y = 50 + 23 = 73).
    press(r, 77, 73);
    release(r, 77, 73);
    CHECK_EQ(r.get16(kProp1 + pi_VertPot), MAXPOT - 0x4000u);
    CHECK_EQ(g_prop_pots.size(), 5u);
    // Knob visually moved up from bottom.
    CHECK_EQ(window_pixel(r, window, 25, 75), 0u);
}

void gadget_idcmp_down_move_up() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);

    put_prop_info(r, kProp1, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 4);
    put_gadget(r, kGad1, 0, 20, 20, 14, 62, 0,
               GACT_IMMEDIATE | GACT_RELVERIFY | GACT_FOLLOWMOUSE, GTYP_PROPGADGET, kProp1);
    put_new_window(r, kNewWindow, 50, 50, 200, 120, 0, 0, 0, 0);
    // No WFLG_REPORTMOUSE: MOUSEMOVE must still arrive via GACT_FOLLOWMOUSE.
    r.put32(kNewWindow + nw_IDCMPFlags,
            IDCMP_GADGETDOWN | IDCMP_GADGETUP | IDCMP_MOUSEMOVE | IDCMP_MOUSEBUTTONS);
    r.put32(kNewWindow + nw_FirstGadget, kGad1);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(window != 0);

    press(r, 77, 75);
    move_to(r, 77, 90);
    release(r, 77, 90);

    std::vector<u32> msgs;
    const std::vector<u32> classes = queued_classes(r, window, &msgs);
    CHECK_EQ(classes.size(), 3u);
    CHECK_EQ(classes[0], IDCMP_GADGETDOWN);
    CHECK_EQ(classes[1], IDCMP_MOUSEMOVE);
    CHECK_EQ(classes[2], IDCMP_GADGETUP);
    for (u32 msg : msgs) {
        CHECK_EQ(r.get32(msg + im_IAddress), kGad1);
        reply_msg(r, msg);
    }

    // Container click posts GADGETDOWN and GADGETUP without SELECTDOWN/SELECTUP.
    press(r, 77, 73);
    release(r, 77, 73);
    msgs.clear();
    const std::vector<u32> page_classes = queued_classes(r, window, &msgs);
    CHECK_EQ(page_classes.size(), 2u);
    CHECK_EQ(page_classes[0], IDCMP_GADGETDOWN);
    CHECK_EQ(page_classes[1], IDCMP_GADGETUP);
    for (u32 msg : msgs) {
        CHECK_EQ(r.get32(msg + im_IAddress), kGad1);
        reply_msg(r, msg);
    }
}

void disabled_gadget_ignores_input() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);

    put_prop_info(r, kProp1, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, 0x4000);
    put_gadget(r, kGad1, 0, 20, 20, 14, 62, 0, GACT_IMMEDIATE | GACT_RELVERIFY, GTYP_PROPGADGET,
               kProp1);
    put_new_window(r, kNewWindow, 50, 50, 200, 120, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_GADGETDOWN | IDCMP_GADGETUP | IDCMP_MOUSEBUTTONS);
    r.put32(kNewWindow + nw_FirstGadget, kGad1);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(window != 0);

    g_prop_pots.clear();
    r.intuition.set_gadget_hook(kGad1, &record_prop_hook, &g_prop_pots);

    r.cpu().a[0] = kGad1;
    r.cpu().a[1] = window;
    r.cpu().a[2] = 0;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::OffGadget), ExitReason::Halt);
    CHECK((r.get16(kGad1 + gg_Flags) & GFLG_DISABLED) != 0);

    press(r, 77, 120);
    release(r, 77, 120);
    CHECK_EQ(r.get16(kProp1 + pi_VertPot), 0u);
    CHECK_EQ(g_prop_pots.size(), 0u);
    const std::vector<u32> dis_classes = queued_classes(r, window);
    CHECK_EQ(dis_classes.size(), 2u);
    CHECK_EQ(dis_classes[0], IDCMP_MOUSEBUTTONS);
    CHECK_EQ(dis_classes[1], IDCMP_MOUSEBUTTONS);

    r.cpu().a[0] = kGad1;
    r.cpu().a[1] = window;
    r.cpu().a[2] = 0;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::OnGadget), ExitReason::Halt);
    CHECK((r.get16(kGad1 + gg_Flags) & GFLG_DISABLED) == 0);

    press(r, 77, 120);
    release(r, 77, 120);
    CHECK_EQ(r.get16(kProp1 + pi_VertPot), 0x4000u);
    CHECK_EQ(g_prop_pots.size(), 1u);
}

void a_boolean_gadget_takes_the_click_from_the_window() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);

    put_gadget(r, kGad1, 0, 20, 20, 40, 20, 0, GACT_IMMEDIATE, GTYP_BOOLGADGET, 0);
    put_new_window(r, kNewWindow, 50, 50, 200, 100, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_GADGETDOWN | IDCMP_MOUSEBUTTONS);
    r.put32(kNewWindow + nw_FirstGadget, kGad1);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(window != 0);
    CHECK_EQ(r.get32(window + wd_FirstGadget), kGad1);

    r.intuition.refresh_glist(kGad1, window, -1);
    press(r, 80, 80);
    release(r, 80, 80);

    // GADGETDOWN for the IMMEDIATE gadget, and no button messages: the
    // gadget had the click.
    CHECK(!logged("intuition: gadget type 1 not handled"));
    const std::vector<u32> classes = queued_classes(r, window);
    CHECK_EQ(classes.size(), 1u);
    CHECK_EQ(classes[0], IDCMP_GADGETDOWN);
}

void gadget_list_cycle_is_bounded() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);

    put_prop_info(r, kProp1, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 2);
    put_prop_info(r, kProp2, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 2);
    put_gadget(r, kGad1, kGad2, 10, 15, 10, 30, 0, 0, GTYP_PROPGADGET, kProp1);
    put_gadget(r, kGad2, kGad1, 30, 15, 10, 30, 0, 0, GTYP_PROPGADGET, kProp2);

    put_new_window(r, kNewWindow, 50, 50, 200, 100, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_FirstGadget, kGad1);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(window != 0);

    r.intuition.refresh_glist(kGad1, window, -1);
    press(r, 120, 120);
    release(r, 120, 120);
    CHECK_EQ(r.intuition.remove_glist(window, kGad3, -1), 0xFFFFFFFFu);
    r.intuition.close_window(window);
    CHECK(!r.intuition.window_open(window));
}

void close_during_knob_drag() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);

    put_prop_info(r, kProp1, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 4);
    put_gadget(r, kGad1, 0, 20, 20, 14, 62, 0, 0, GTYP_PROPGADGET, kProp1);
    put_new_window(r, kNewWindow, 50, 50, 200, 120, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_FirstGadget, kGad1);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(window != 0);

    g_prop_pots.clear();
    r.intuition.set_gadget_hook(kGad1, &record_prop_hook, &g_prop_pots);
    press(r, 77, 75);
    move_to(r, 77, 90);
    const usize calls_before_close = g_prop_pots.size();
    CHECK(calls_before_close >= 1u);

    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);

    move_to(r, 77, 110);
    release(r, 77, 110);
    CHECK_EQ(g_prop_pots.size(), calls_before_close);

    // Reopen with the same gadget address: hook was cleared on close.
    put_prop_info(r, kProp1, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 4);
    put_new_window(r, kNewWindow, 50, 50, 200, 120, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_FirstGadget, kGad1);
    const u32 again = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(again != 0);
    press(r, 77, 120);
    release(r, 77, 120);
    CHECK_EQ(g_prop_pots.size(), calls_before_close);
}

void wheel_event_posts_newmouse_rawkey() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);

    put_new_window(r, kNewWindow, 50, 50, 200, 100, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_RAWKEY);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.intuition.window_under(100, 100), window);
    CHECK_EQ(r.intuition.window_under(10, 10), 0u);

    r.intuition.wheel_event(2);
    r.intuition.wheel_event(-3);
    r.intuition.wheel_event(0);

    const u32 port = r.get32(window + wd_UserPort);
    const u32 expected[5] = {NM_WHEEL_UP, NM_WHEEL_UP, NM_WHEEL_DOWN, NM_WHEEL_DOWN,
                             NM_WHEEL_DOWN};
    for (u32 code : expected) {
        const u32 msg = get_msg(r, port);
        CHECK(msg != 0);
        CHECK_EQ(r.get32(msg + im_Class), IDCMP_RAWKEY);
        CHECK_EQ(r.get16(msg + im_Code), code);
        reply_msg(r, msg);
    }
    CHECK_EQ(get_msg(r, port), 0u);

    put_new_window(r, kNewWindow, 300, 50, 200, 100, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS);
    const u32 no_raw = open_win(r, kNewWindow);
    CHECK(no_raw != 0);
    r.intuition.wheel_event(2);
    CHECK_EQ(get_msg(r, r.get32(no_raw + wd_UserPort)), 0u);
}

constexpr u16 kQualLShift = 0x0001;
constexpr u16 kKeyUp = 0x80;

// An active window of 200x100 at left, 50 with the IDCMP flags.
u32 open_key_window(GfxRig& r, i32 left, u32 idcmp) {
    put_new_window(r, kNewWindow, left, 50, 200, 100, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, idcmp);
    return open_win(r, kNewWindow);
}

// Takes the next message off the window's port, checks its class, code and
// qualifier, and replies to it.
void expect_key_message(GfxRig& r, u32 window, u32 idcmp_class, u32 code, u32 qualifier) {
    const u32 msg = get_msg(r, r.get32(window + wd_UserPort));
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), idcmp_class);
    CHECK_EQ(r.get16(msg + im_Code), code);
    CHECK_EQ(r.get16(msg + im_Qualifier), qualifier);
    CHECK_EQ(r.get32(msg + im_IAddress), 0u);
    reply_msg(r, msg);
}

void raw_key_posts_rawkey_with_code_and_qualifier() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    r.intuition.raw_key(0x20, 0);  // no active window: nothing happens

    const u32 window = open_key_window(r, 50, IDCMP_RAWKEY);
    CHECK(window != 0);
    r.intuition.raw_key(0x20, kQualLShift);
    r.intuition.raw_key(0x20 | kKeyUp, 0);
    r.intuition.raw_key(0x50, 0x0100);
    expect_key_message(r, window, IDCMP_RAWKEY, 0x20, kQualLShift);
    expect_key_message(r, window, IDCMP_RAWKEY, 0xA0, 0);
    expect_key_message(r, window, IDCMP_RAWKEY, 0x50, 0x0100);
    CHECK_EQ(get_msg(r, r.get32(window + wd_UserPort)), 0u);
}

void raw_key_posts_vanillakey_characters() {
    GfxRig r;
    KeymapLibrary keymaps;
    CHECK(keymaps.init(r.machine, r.exec));
    r.intuition.set_keymap_library(&keymaps);
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);

    const u32 window = open_key_window(r, 50, IDCMP_VANILLAKEY);
    CHECK(window != 0);
    r.intuition.raw_key(0x20, 0);
    r.intuition.raw_key(0x20, kQualLShift);
    r.intuition.raw_key(0x20 | kKeyUp, 0);  // a release types nothing
    r.intuition.raw_key(0x50, 0);           // F1 types a string
    r.intuition.raw_key(0x60, kQualLShift);  // a qualifier key types nothing
    expect_key_message(r, window, IDCMP_VANILLAKEY, 'a', 0);
    expect_key_message(r, window, IDCMP_VANILLAKEY, 'A', kQualLShift);
    CHECK_EQ(get_msg(r, r.get32(window + wd_UserPort)), 0u);
}

void a_window_with_both_key_flags_gets_rawkey() {
    GfxRig r;
    KeymapLibrary keymaps;
    CHECK(keymaps.init(r.machine, r.exec));
    r.intuition.set_keymap_library(&keymaps);
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);

    const u32 window = open_key_window(r, 50, IDCMP_RAWKEY | IDCMP_VANILLAKEY);
    r.intuition.raw_key(0x20, 0);
    expect_key_message(r, window, IDCMP_RAWKEY, 0x20, 0);
    CHECK_EQ(get_msg(r, r.get32(window + wd_UserPort)), 0u);
}

void a_window_without_key_flags_gets_no_keys() {
    GfxRig r;
    KeymapLibrary keymaps;
    CHECK(keymaps.init(r.machine, r.exec));
    r.intuition.set_keymap_library(&keymaps);
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);

    const u32 plain = open_key_window(r, 50, IDCMP_MOUSEBUTTONS);
    CHECK(!r.intuition.wants_keys(plain));
    r.intuition.raw_key(0x20, 0);
    CHECK_EQ(get_msg(r, r.get32(plain + wd_UserPort)), 0u);

    const u32 raw = open_key_window(r, 300, IDCMP_RAWKEY);
    const u32 vanilla = open_key_window(r, 300, IDCMP_VANILLAKEY);
    CHECK(r.intuition.wants_keys(raw));
    CHECK(r.intuition.wants_keys(vanilla));
    CHECK(!r.intuition.wants_keys(0));
    CHECK(!r.intuition.wants_keys(kData));
}

void mouse_buttons_carry_the_keyboard_qualifier() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    const u32 window = open_key_window(r, 50, IDCMP_MOUSEBUTTONS);
    CHECK(window != 0);

    r.intuition.set_qualifier(kQualLShift);
    r.intuition.pointer_event(125, 115, kMouseLeft, kMouseLeft, 0, false);
    const u32 msg = get_msg(r, r.get32(window + wd_UserPort));
    CHECK(msg != 0);
    CHECK_EQ(r.get16(msg + im_Code), SELECTDOWN);
    CHECK_EQ(r.get16(msg + im_Qualifier), kQualLShift);
}

void gadget_lvos_from_guest_calls() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    put_new_window(r, kNewWindow, 50, 50, 200, 120, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);

    put_prop_info(r, kProp1, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 4);
    put_prop_info(r, kProp2, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 2);
    put_gadget(r, kGad1, kGad2, 20, 20, 14, 50, 0, 0, GTYP_PROPGADGET, kProp1);
    put_gadget(r, kGad2, 0, 50, 20, 14, 50, 0, 0, GTYP_PROPGADGET, kProp2);

    r.cpu().a[0] = window;
    r.cpu().a[1] = kGad1;
    r.cpu().d[0] = 0xFFFF;
    r.cpu().d[1] = 2;
    r.cpu().a[2] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::AddGList), 0u);
    CHECK_EQ(r.get32(window + wd_FirstGadget), kGad1);

    r.cpu().a[0] = kGad1;
    r.cpu().a[1] = window;
    r.cpu().a[2] = 0;
    r.cpu().d[0] = 2;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::RefreshGList), ExitReason::Halt);
    CHECK_EQ(window_pixel(r, window, 25, 21), 2u);

    r.cpu().a[0] = kGad1;
    r.cpu().a[1] = window;
    r.cpu().a[2] = 0;
    r.cpu().d[0] = AUTOKNOB | FREEVERT;
    r.cpu().d[1] = 0;
    r.cpu().d[2] = MAXPOT;
    r.cpu().d[3] = MAXBODY;
    r.cpu().d[4] = MAXBODY / 4;
    r.cpu().d[5] = 1;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::NewModifyProp), ExitReason::Halt);
    CHECK_EQ(r.get16(kProp1 + pi_VertPot), MAXPOT);
    CHECK_EQ(window_pixel(r, window, 25, 58), 2u);

    r.cpu().a[0] = window;
    r.cpu().a[1] = kGad1;
    r.cpu().d[0] = 1;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::RemoveGList), 0u);
    CHECK_EQ(r.get32(window + wd_FirstGadget), kGad2);

    r.cpu().a[0] = window;
    r.cpu().a[1] = kGad1;
    r.cpu().d[0] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::AddGadget), 0u);
    CHECK_EQ(r.get32(window + wd_FirstGadget), kGad1);

    r.cpu().a[0] = kGad1;
    r.cpu().a[1] = window;
    r.cpu().a[2] = 0;
    r.cpu().d[0] = AUTOKNOB | FREEVERT;
    r.cpu().d[1] = 0;
    r.cpu().d[2] = 0;
    r.cpu().d[3] = MAXBODY;
    r.cpu().d[4] = MAXBODY / 4;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::ModifyProp), ExitReason::Halt);
    CHECK_EQ(r.get16(kProp1 + pi_VertPot), 0u);
    CHECK_EQ(window_pixel(r, window, 25, 21), 2u);

    r.cpu().a[0] = kGad1;
    r.cpu().a[1] = window;
    r.cpu().a[2] = 0;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::RefreshGadgets), ExitReason::Halt);

    r.cpu().a[0] = window;
    r.cpu().a[1] = kGad2;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::RemoveGadget), 1u);
    CHECK_EQ(r.get32(kGad1 + gg_NextGadget), 0u);
}

void modify_prop_ignores_non_prop_gadget() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    put_new_window(r, kNewWindow, 50, 50, 200, 120, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);

    put_prop_info(r, kProp1, 0x0012, 0x1234, 0x5678, 0x9ABC, 0xDEF0);
    put_gadget(r, kGad1, 0, 20, 20, 20, 20, 0, 0, 1, kProp1);
    r.intuition.add_glist(window, kGad1, -1, 1);

    const std::vector<u8> before = screen_planes(r, r.intuition.front_screen());
    r.intuition.new_modify_prop(kGad1, window, AUTOKNOB | FREEVERT, 0, MAXPOT, MAXBODY,
                                MAXBODY / 4, 1);

    CHECK_EQ(r.get16(kProp1 + pi_Flags), 0x0012u);
    CHECK_EQ(r.get16(kProp1 + pi_HorizPot), 0x1234u);
    CHECK_EQ(r.get16(kProp1 + pi_VertPot), 0x5678u);
    CHECK_EQ(r.get16(kProp1 + pi_HorizBody), 0x9ABCu);
    CHECK_EQ(r.get16(kProp1 + pi_VertBody), 0xDEF0u);
    CHECK(screen_planes(r, r.intuition.front_screen()) == before);
}

void modify_prop_keeps_knobhit_during_drag() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);

    put_prop_info(r, kProp1, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 4);
    put_gadget(r, kGad1, 0, 20, 20, 14, 62, 0, GACT_IMMEDIATE | GACT_RELVERIFY, GTYP_PROPGADGET,
               kProp1);
    put_new_window(r, kNewWindow, 50, 50, 200, 120, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_FirstGadget, kGad1);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(window != 0);

    press(r, 77, 75);
    CHECK((r.get16(kProp1 + pi_Flags) & KNOBHIT) != 0);

    r.intuition.new_modify_prop(kGad1, window, AUTOKNOB | FREEVERT, 0, MAXPOT / 2, MAXBODY,
                                MAXBODY / 4, 1);
    CHECK((r.get16(kProp1 + pi_Flags) & KNOBHIT) != 0);
    CHECK_EQ(r.get16(kProp1 + pi_VertPot), MAXPOT / 2);

    // Disabling during an active drag stops drag_prop from moving the knob.
    g_prop_pots.clear();
    r.intuition.set_gadget_hook(kGad1, &record_prop_hook, &g_prop_pots);
    r.intuition.off_gadget(kGad1, window);
    move_to(r, 77, 115);
    CHECK_EQ(r.get16(kProp1 + pi_VertPot), MAXPOT / 2);
    CHECK(g_prop_pots.empty());

    release(r, 77, 115);
    CHECK((r.get16(kProp1 + pi_Flags) & KNOBHIT) == 0);
}

void remove_glist_clears_hook() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);

    put_prop_info(r, kProp1, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, 0x4000);
    put_prop_info(r, kProp2, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, 0x4000);
    put_gadget(r, kGad1, kGad2, 20, 20, 14, 62, 0, 0, GTYP_PROPGADGET, kProp1);
    put_gadget(r, kGad2, 0, 50, 20, 14, 62, 0, 0, GTYP_PROPGADGET, kProp2);
    put_new_window(r, kNewWindow, 50, 50, 200, 120, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_FirstGadget, kGad1);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(window != 0);

    g_prop_pots.clear();
    r.intuition.set_gadget_hook(kGad1, &record_prop_hook, &g_prop_pots);
    r.intuition.set_gadget_hook(kGad2, &record_prop_hook, &g_prop_pots);

    CHECK_EQ(r.intuition.remove_glist(window, kGad1, 1), 0u);
    CHECK_EQ(r.intuition.add_glist(window, kGad1, -1, 1), 1u);

    // Removed gadget's hook was cleared; surviving gadget's hook stays.
    press(r, 77, 125);
    release(r, 77, 125);
    CHECK(g_prop_pots.empty());

    press(r, 107, 125);
    release(r, 107, 125);
    CHECK_EQ(g_prop_pots.size(), 1u);
}

void border_sizes_with_without_size_gadget_and_borderless() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);

    put_new_window(r, kNewWindow, 20, 20, 160, 80, 0, 0, 0, 0);
    const u32 plain = open_win(r, kNewWindow);
    CHECK(plain != 0);
    CHECK_EQ(r.get8(plain + wd_BorderLeft), 2u);
    CHECK_EQ(r.get8(plain + wd_BorderTop), 15u);
    CHECK_EQ(r.get8(plain + wd_BorderRight), 2u);
    CHECK_EQ(r.get8(plain + wd_BorderBottom), 2u);

    put_new_window(r, kNewWindow, 200, 20, 160, 80, WFLG_SIZEGADGET, 0, 0, 0);
    const u32 sizable = open_win(r, kNewWindow);
    CHECK(sizable != 0);
    CHECK_EQ(r.get8(sizable + wd_BorderLeft), 2u);
    CHECK_EQ(r.get8(sizable + wd_BorderTop), 15u);
    CHECK_EQ(r.get8(sizable + wd_BorderRight), 14u);
    CHECK_EQ(r.get8(sizable + wd_BorderBottom), 2u);

    put_new_window(r, kNewWindow, 20, 120, 160, 80, WFLG_SIZEGADGET | WFLG_SIZEBBOTTOM, 0, 0, 0);
    const u32 sbottom = open_win(r, kNewWindow);
    CHECK(sbottom != 0);
    CHECK_EQ(r.get8(sbottom + wd_BorderRight), 2u);
    CHECK_EQ(r.get8(sbottom + wd_BorderBottom), 14u);

    put_new_window(r, kNewWindow, 200, 120, 160, 80,
                   WFLG_SIZEGADGET | WFLG_SIZEBRIGHT | WFLG_SIZEBBOTTOM, 0, 0, 0);
    const u32 sboth = open_win(r, kNewWindow);
    CHECK(sboth != 0);
    CHECK_EQ(r.get8(sboth + wd_BorderRight), 14u);
    CHECK_EQ(r.get8(sboth + wd_BorderBottom), 14u);

    put_new_window(r, kNewWindow, 380, 120, 160, 80, WFLG_SIZEGADGET | WFLG_SIZEBRIGHT, 0, 0, 0);
    const u32 sright = open_win(r, kNewWindow);
    CHECK(sright != 0);
    CHECK_EQ(r.get8(sright + wd_BorderRight), 14u);
    CHECK_EQ(r.get8(sright + wd_BorderBottom), 2u);

    put_new_window(r, kNewWindow, 380, 20, 160, 80, WFLG_BORDERLESS, 0, 0, 0);
    const u32 borderless = open_win(r, kNewWindow);
    CHECK(borderless != 0);
    CHECK_EQ(r.get8(borderless + wd_BorderLeft), 1u);
    CHECK_EQ(r.get8(borderless + wd_BorderTop), 1u);
    CHECK_EQ(r.get8(borderless + wd_BorderRight), 1u);
    CHECK_EQ(r.get8(borderless + wd_BorderBottom), 1u);
}

void close_depth_size_hit_at_new_positions() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);

    put_new_window(r, kNewWindow, 40, 40, 200, 100,
                   WFLG_CLOSEGADGET | WFLG_DEPTHGADGET | WFLG_SIZEGADGET, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_CLOSEWINDOW);
    r.put16(kNewWindow + nw_MaxWidth, 0xFFFF);
    r.put16(kNewWindow + nw_MaxHeight, 0xFFFF);
    const u32 w1 = open_win(r, kNewWindow);
    CHECK(w1 != 0);

    put_new_window(r, kNewWindow, 100, 70, 160, 80, WFLG_DEPTHGADGET, 0, 0, 0);
    const u32 w2 = open_win(r, kNewWindow);
    CHECK(w2 != 0);
    CHECK_EQ(r.get32(screen + sc_FirstWindow), w2);

    const Rect d2 = Intuition::depth_box_rect(100, 70, 160);
    const i32 d2_cx = (d2.x0 + d2.x1) / 2;
    const i32 d2_cy = (d2.y0 + d2.y1) / 2;
    press(r, d2_cx, d2_cy);
    release(r, d2_cx, d2_cy);
    CHECK_EQ(r.get32(screen + sc_FirstWindow), w1);

    const Rect c1 = Intuition::close_box_rect(40, 40);
    const i32 c1_cx = (c1.x0 + c1.x1) / 2;
    const i32 c1_cy = (c1.y0 + c1.y1) / 2;
    press(r, c1_cx, c1_cy);
    release(r, c1_cx, c1_cy);
    const u32 close_msg = get_msg(r, r.get32(w1 + wd_UserPort));
    CHECK(close_msg != 0);
    CHECK_EQ(r.get32(close_msg + im_Class), IDCMP_CLOSEWINDOW);
    reply_msg(r, close_msg);

    const Rect s1 = Intuition::size_box_rect(40, 40, 200, 100);
    const i32 s1_cx = (s1.x0 + s1.x1) / 2;
    const i32 s1_cy = (s1.y0 + s1.y1) / 2;
    press(r, s1_cx, s1_cy);
    move_to(r, s1_cx + 20, s1_cy + 15);
    release(r, s1_cx + 20, s1_cy + 15);
    CHECK_EQ(r.get16(w1 + wd_Width), 220u);
    CHECK_EQ(r.get16(w1 + wd_Height), 115u);
}

void depth8_chrome_pixel_spot_checks() {
    GfxRig r;
    r.str(kTitle, "Screen");
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, kTitle);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);

    // The screen bar is shaded from top to bottom, over the label pen's
    // line.
    u32 prev_screen_bar = chrome::kScreenBar0;
    for (i32 y = 0; y <= 13; ++y) {
        const u32 p = screen_pixel(r, screen, 100, y);
        CHECK(p >= prev_screen_bar && p <= chrome::kScreenBar3);
        prev_screen_bar = p;
    }
    CHECK_EQ(screen_pixel(r, screen, 100, 0), static_cast<u32>(chrome::kScreenBar0));
    CHECK_EQ(prev_screen_bar, static_cast<u32>(chrome::kScreenBar3));
    CHECK_EQ(screen_pixel(r, screen, 100, 14), static_cast<u32>(chrome::kMenuLabelPen));

    r.str(kWinTitle, "H");
    put_new_window(r, kNewWindow, 40, 30, 220, 120,
                   WFLG_ACTIVATE | WFLG_CLOSEGADGET | WFLG_DEPTHGADGET | WFLG_SIZEGADGET,
                   kWinTitle, 0, 0);
    const u32 w1 = open_win(r, kNewWindow);
    CHECK(w1 != 0);

    CHECK_EQ(screen_pixel(r, screen, 40, 30), 0u);
    CHECK_EQ(screen_pixel(r, screen, 43, 30), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 259, 30), 0u);
    CHECK_EQ(screen_pixel(r, screen, 256, 30), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 40, 149), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 259, 149), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 41, 60), static_cast<u32>(chrome::kFrameLight));
    CHECK_EQ(screen_pixel(r, screen, 258, 60), static_cast<u32>(chrome::kInactiveBar5));
    CHECK_EQ(screen_pixel(r, screen, 100, 148), static_cast<u32>(chrome::kInactiveBar5));
    CHECK_EQ(screen_pixel(r, screen, 42, 45), 0u);

    CHECK_EQ(screen_pixel(r, screen, 120, 31), static_cast<u32>(chrome::kFrameLight));
    u32 prev_bar = screen_pixel(r, screen, 120, 32);
    CHECK_EQ(prev_bar, static_cast<u32>(chrome::kActiveBar0));
    for (i32 row = 2; row <= 13; ++row) {
        const u32 p = screen_pixel(r, screen, 120, 30 + row);
        CHECK(p >= chrome::kActiveBar0 && p <= chrome::kActiveBar7);
        CHECK(p >= prev_bar);
        prev_bar = p;
    }
    CHECK_EQ(prev_bar, static_cast<u32>(chrome::kActiveBar7));
    CHECK_EQ(screen_pixel(r, screen, 120, 44), static_cast<u32>(chrome::kFrameDark));

    // The close button: outline, shaded body and the light square with its
    // dark outline.
    CHECK_EQ(screen_pixel(r, screen, 49, 32), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 45, 33), static_cast<u32>(chrome::kKnob0));
    CHECK_EQ(screen_pixel(r, screen, 45, 41), static_cast<u32>(chrome::kKnob2));
    CHECK_EQ(screen_pixel(r, screen, 49, 37), static_cast<u32>(chrome::kFrameLight));
    CHECK_EQ(screen_pixel(r, screen, 46, 37), static_cast<u32>(chrome::kFrameDark));
    // The depth button: outline, the shaded sheet behind and the light one
    // in front, each with its dark edge.
    CHECK_EQ(screen_pixel(r, screen, 250, 32), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 247, 34), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 248, 35), static_cast<u32>(chrome::kKnob2));
    CHECK_EQ(screen_pixel(r, screen, 249, 36), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 250, 37), static_cast<u32>(chrome::kFrameLight));
    CHECK_EQ(screen_pixel(r, screen, 59, 33), static_cast<u32>(chrome::kTitleActive));
    CHECK_EQ(screen_pixel(r, screen, 256, 146), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 255, 145), static_cast<u32>(chrome::kFrameLight));

    put_new_window(r, kNewWindow, 300, 30, 200, 100,
                   WFLG_ACTIVATE | WFLG_CLOSEGADGET | WFLG_DEPTHGADGET, kWinTitle, 0, 0);
    const u32 w2 = open_win(r, kNewWindow);
    CHECK(w2 != 0);

    u32 prev_inact = screen_pixel(r, screen, 120, 32);
    CHECK_EQ(prev_inact, static_cast<u32>(chrome::kInactiveBar0));
    for (i32 row = 2; row <= 13; ++row) {
        const u32 p = screen_pixel(r, screen, 120, 30 + row);
        CHECK(p >= chrome::kInactiveBar0 && p <= chrome::kInactiveBar5);
        CHECK(p >= prev_inact);
        prev_inact = p;
    }
    CHECK_EQ(prev_inact, static_cast<u32>(chrome::kInactiveBar5));
    // An inactive window keeps the buttons' bodies and greys their symbols.
    CHECK_EQ(screen_pixel(r, screen, 49, 32), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 45, 33), static_cast<u32>(chrome::kKnob0));
    CHECK_EQ(screen_pixel(r, screen, 49, 37), static_cast<u32>(chrome::kFrameLight));
    CHECK_EQ(screen_pixel(r, screen, 46, 37), static_cast<u32>(chrome::kTitleInactive));
    CHECK_EQ(screen_pixel(r, screen, 250, 32), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 249, 36), static_cast<u32>(chrome::kTitleInactive));
    CHECK_EQ(screen_pixel(r, screen, 250, 37), static_cast<u32>(chrome::kFrameLight));
    CHECK_EQ(screen_pixel(r, screen, 59, 33), static_cast<u32>(chrome::kTitleInactive));
    CHECK_EQ(screen_pixel(r, screen, 400, 32), static_cast<u32>(chrome::kActiveBar0));
    CHECK_EQ(screen_pixel(r, screen, 400, 43), static_cast<u32>(chrome::kActiveBar7));
}

void depth8_prop_gadget_chrome_pixels() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);

    put_prop_info(r, kProp1, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 4);
    put_prop_info(r, kProp2, AUTOKNOB | FREEVERT | PROPBORDERLESS, 0, 0, MAXBODY, MAXBODY / 4);
    put_prop_info(r, kProp3, AUTOKNOB | FREEHORIZ, 0, 0, MAXBODY / 4, MAXBODY);
    put_gadget(r, kGad1, kGad2, 20, 20, 12, 40, 0, 0, GTYP_PROPGADGET, kProp1);
    put_gadget(r, kGad2, kGad3, 50, 20, 12, 82, 0, 0, GTYP_PROPGADGET, kProp2);
    put_gadget(r, kGad3, 0, 80, 20, 82, 12, 0, 0, GTYP_PROPGADGET, kProp3);

    put_new_window(r, kNewWindow, 50, 50, 220, 140, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_FirstGadget, kGad1);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(window != 0);

    // kGad1: knob y=21..29 (height 9, no grip), track y=30..58.
    CHECK_EQ(window_pixel(r, window, 20, 20), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(window_pixel(r, window, 20, 50), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(window_pixel(r, window, 25, 50), static_cast<u32>(chrome::kTrack));
    CHECK_EQ(window_pixel(r, window, 21, 21), static_cast<u32>(chrome::kTrack));
    CHECK_EQ(window_pixel(r, window, 23, 21), static_cast<u32>(chrome::kFrameDark));
    const u32 k1_centre = window_pixel(r, window, 25, 25);
    CHECK(k1_centre >= chrome::kKnob0 && k1_centre <= chrome::kKnob2);
    CHECK_EQ(window_pixel(r, window, 25, 22), static_cast<u32>(chrome::kKnob0));
    CHECK_EQ(window_pixel(r, window, 25, 28), static_cast<u32>(chrome::kKnob2));

    // Selecting/dragging kGad1 switches knob to kActiveBar2..kActiveBar5.
    press(r, 75, 75);
    const u32 k1_drag = window_pixel(r, window, 25, 25);
    CHECK(k1_drag >= chrome::kActiveBar2 && k1_drag <= chrome::kActiveBar5);
    release(r, 75, 75);
    const u32 k1_rel = window_pixel(r, window, 25, 25);
    CHECK(k1_rel >= chrome::kKnob0 && k1_rel <= chrome::kKnob2);

    // kGad2: PROPBORDERLESS, knob y=21..40 (height 20 >= 12, 3 grip lines at y=28,30,32).
    CHECK_EQ(window_pixel(r, window, 50, 20), static_cast<u32>(chrome::kTrack));
    CHECK_EQ(window_pixel(r, window, 55, 28), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(window_pixel(r, window, 55, 30), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(window_pixel(r, window, 55, 32), static_cast<u32>(chrome::kFrameDark));
    const u32 k2_fill = window_pixel(r, window, 55, 29);
    CHECK(k2_fill >= chrome::kKnob0 && k2_fill <= chrome::kKnob2);

    // kGad3: FREEHORIZ-only, knob x=81..99, y=21..30 (width 19 >= 12, 3 grip lines at x=88,90,92).
    CHECK_EQ(window_pixel(r, window, 82, 25), static_cast<u32>(chrome::kKnob0));
    CHECK_EQ(window_pixel(r, window, 98, 25), static_cast<u32>(chrome::kKnob2));
    CHECK_EQ(window_pixel(r, window, 88, 25), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(window_pixel(r, window, 90, 25), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(window_pixel(r, window, 92, 25), static_cast<u32>(chrome::kFrameDark));
}

void depth2_fallback_chrome_pens() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);

    put_prop_info(r, kProp1, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 4);
    put_gadget(r, kGad1, 0, 20, 20, 12, 40, 0, 0, GTYP_PROPGADGET, kProp1);
    r.str(kWinTitle, "H");
    put_new_window(r, kNewWindow, 20, 20, 200, 100,
                   WFLG_ACTIVATE | WFLG_CLOSEGADGET | WFLG_DEPTHGADGET, kWinTitle, 0, 0);
    r.put32(kNewWindow + nw_FirstGadget, kGad1);
    const u32 active = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(active != 0);

    // The active window: SHINEPEN (2) edge, FILLPEN (3) bar, FILLTEXTPEN (1) title.
    CHECK_EQ(screen_pixel(r, screen, 20, 20), 2u);
    CHECK_EQ(screen_pixel(r, screen, 100, 25), 3u);
    CHECK_EQ(screen_pixel(r, screen, 39, 23), 1u);
    // The close image: its glyph etched in SHADOWPEN and SHINEPEN, the fill
    // pen inside and round it, and the groove that ends its cell.
    CHECK_EQ(screen_pixel(r, screen, 24, 27), 1u);
    CHECK_EQ(screen_pixel(r, screen, 30, 27), 2u);
    CHECK_EQ(screen_pixel(r, screen, 27, 27), 3u);
    CHECK_EQ(screen_pixel(r, screen, 22, 27), 3u);
    CHECK_EQ(screen_pixel(r, screen, 35, 27), 1u);
    CHECK_EQ(screen_pixel(r, screen, 36, 27), 2u);
    // The knob: SHINEPEN on top, BACKGROUNDPEN inside its edges and in the container.
    CHECK_EQ(window_pixel(r, active, 25, 21), 2u);
    CHECK_EQ(window_pixel(r, active, 25, 25), 0u);
    CHECK_EQ(window_pixel(r, active, 25, 50), 0u);

    // The inactive window: BACKGROUNDPEN (0) bar, TEXTPEN (1) title.
    put_new_window(r, kNewWindow, 260, 20, 160, 80, WFLG_CLOSEGADGET, kWinTitle, 0, 0);
    const u32 inactive = open_win(r, kNewWindow);
    CHECK(inactive != 0);
    CHECK_EQ(screen_pixel(r, screen, 320, 25), 0u);
    CHECK_EQ(screen_pixel(r, screen, 260, 20), 2u);
    CHECK_EQ(screen_pixel(r, screen, 279, 23), 1u);
}

void the_look_follows_the_number_of_planes() {
    CHECK(chrome::style_for_depth(1) == chrome::Style::Classic);
    CHECK(chrome::style_for_depth(4) == chrome::Style::Classic);
    for (u32 depth = 5; depth <= 8; ++depth) {
        CHECK(chrome::style_for_depth(depth) == chrome::Style::Modern);
    }
}

// The classic chrome is drawn in the pens of the screen's DrawInfo once one is
// made, and until then in the pens IPrefs sent for the screen's depth.
void classic_chrome_follows_the_draw_info_or_else_the_pen_prefs() {
    GfxRig r;
    const u32 size = put_pen_prefs(r, 0, 3);
    set_iprefs(r, kIPrefs, size, 9);
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    // The prefs' BARBLOCKPEN.
    CHECK_EQ(screen_pixel(r, screen, 5, 5), 3u);

    // Once there is a DrawInfo, its FILLPEN fills the frame of an active
    // window and its BACKGROUNDPEN that of an inactive one.
    const u32 pens = r.get32(r.intuition.screen_draw_info(screen) + dri_Pens);
    r.put16(pens + 5 * 2, 2);
    r.put16(pens + 7 * 2, 1);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, WFLG_ACTIVATE, 0, 0, 0);
    CHECK(open_win(r, kNewWindow) != 0);
    put_new_window(r, kNewWindow, 300, 60, 200, 100, 0, 0, 0, 0);
    CHECK(open_win(r, kNewWindow) != 0);
    CHECK_EQ(screen_pixel(r, screen, 100, 65), 2u);
    CHECK_EQ(screen_pixel(r, screen, 350, 65), 1u);
}

// Activating a window gives its borders the fill pen, as it does its bar, and
// the window that gives up the activation the background pen. The right
// border of a window with a size gadget is wide enough to show it.
void classic_borders_change_colour_with_the_activation() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 30, 30, 180, 100, WFLG_ACTIVATE | WFLG_SIZEGADGET, 0, 0, 0);
    const u32 first = open_win(r, kNewWindow);
    put_new_window(r, kNewWindow, 250, 30, 180, 100, WFLG_SIZEGADGET, 0, 0, 0);
    const u32 second = open_win(r, kNewWindow);
    CHECK(first != 0 && second != 0);
    CHECK_EQ(r.intuition.active_window(), first);
    CHECK_EQ(screen_pixel(r, screen, 30 + 180 - 8, 30 + 50), 3u);
    CHECK_EQ(screen_pixel(r, screen, 250 + 180 - 8, 30 + 50), 0u);

    press(r, 300, 80);
    release(r, 300, 80);
    CHECK_EQ(r.intuition.active_window(), second);
    CHECK_EQ(screen_pixel(r, screen, 30 + 180 - 8, 30 + 50), 0u);
    CHECK_EQ(screen_pixel(r, screen, 250 + 180 - 8, 30 + 50), 3u);
}

// A classic knob is BACKGROUNDPEN, and FILLPEN while it is dragged.
void classic_knob_takes_the_fill_pen_while_dragged() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    put_prop_info(r, kProp1, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 4);
    put_gadget(r, kGad1, 0, 20, 20, 12, 40, 0, GACT_IMMEDIATE | GACT_RELVERIFY,
               GTYP_PROPGADGET, kProp1);
    put_new_window(r, kNewWindow, 50, 50, 200, 120, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_FirstGadget, kGad1);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(window != 0);

    CHECK_EQ(window_pixel(r, window, 25, 25), 0u);
    press(r, 75, 75);
    CHECK_EQ(window_pixel(r, window, 25, 25), 3u);
    release(r, 75, 75);
    CHECK_EQ(window_pixel(r, window, 25, 25), 0u);
}

void activation_switch_redraws_both_bars() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);

    put_new_window(r, kNewWindow, 30, 30, 180, 100, WFLG_ACTIVATE | WFLG_CLOSEGADGET, 0, 0, 0);
    const u32 w1 = open_win(r, kNewWindow);
    put_new_window(r, kNewWindow, 250, 30, 180, 100, WFLG_CLOSEGADGET, 0, 0, 0);
    const u32 w2 = open_win(r, kNewWindow);
    CHECK(w1 != 0 && w2 != 0);
    CHECK_EQ(r.intuition.active_window(), w1);
    CHECK_EQ(screen_pixel(r, screen, 120, 32), static_cast<u32>(chrome::kActiveBar0));
    CHECK_EQ(screen_pixel(r, screen, 340, 32), static_cast<u32>(chrome::kInactiveBar0));

    r.refreshes.clear();
    press(r, 300, 80);
    release(r, 300, 80);
    CHECK_EQ(r.intuition.active_window(), w2);
    CHECK_EQ(r.refreshes.size(), 1u);
    CHECK_EQ(screen_pixel(r, screen, 120, 32), static_cast<u32>(chrome::kInactiveBar0));
    CHECK_EQ(screen_pixel(r, screen, 120, 43), static_cast<u32>(chrome::kInactiveBar5));
    CHECK_EQ(screen_pixel(r, screen, 340, 32), static_cast<u32>(chrome::kActiveBar0));
    CHECK_EQ(screen_pixel(r, screen, 340, 43), static_cast<u32>(chrome::kActiveBar7));
}

void bar_buttons_are_centred_and_mirror_each_other() {
    const Rect close_box = chrome::close_button_box();
    const Rect depth_box = chrome::depth_button_box(200);
    const chrome::Metrics topaz{};
    CHECK_EQ(close_box.x1 - close_box.x0 + 1, topaz.button_width());
    CHECK_EQ(close_box.y1 - close_box.y0 + 1, topaz.button_height());
    CHECK_EQ(depth_box.x1 - depth_box.x0 + 1, topaz.button_width());
    CHECK_EQ(depth_box.y0, close_box.y0);
    CHECK_EQ(depth_box.y1, close_box.y1);
    // Row 0 is the frame and row bar_height() the line under the bar.
    CHECK_EQ(close_box.y0 - 1, topaz.bar_height() - 1 - close_box.y1);
    CHECK_EQ(close_box.x0, 199 - depth_box.x1);

    const Rect close_hit = Intuition::close_box_rect(50, 60);
    const Rect depth_hit = Intuition::depth_box_rect(50, 60, 200);
    CHECK_EQ(close_hit.x0, 50 + close_box.x0);
    CHECK_EQ(close_hit.y0, 60 + close_box.y0);
    CHECK_EQ(close_hit.x1, 50 + close_box.x1);
    CHECK_EQ(close_hit.y1, 60 + close_box.y1);
    CHECK_EQ(depth_hit.x0, 50 + depth_box.x0);
    CHECK_EQ(depth_hit.y0, 60 + depth_box.y0);
    CHECK_EQ(depth_hit.x1, 50 + depth_box.x1);
    CHECK_EQ(depth_hit.y1, 60 + depth_box.y1);
}

bool held_shade(u32 pen) { return pen >= chrome::kActiveBar2 && pen <= chrome::kActiveBar5; }

bool knob_shade(u32 pen) { return pen >= chrome::kKnob0 && pen <= chrome::kKnob2; }

void held_bar_button_shows_pressed_while_the_pointer_is_over_it() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100,
                   WFLG_ACTIVATE | WFLG_CLOSEGADGET | WFLG_DEPTHGADGET, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_CLOSEWINDOW);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 port = r.get32(window + wd_UserPort);

    const Rect close_box = Intuition::close_box_rect(50, 60);
    const Rect depth_box = Intuition::depth_box_rect(50, 60, 200);
    const i32 close_cx = (close_box.x0 + close_box.x1) / 2;
    const i32 depth_cx = (depth_box.x0 + depth_box.x1) / 2;
    const i32 cy = (close_box.y0 + close_box.y1) / 2;
    // A pixel of each body beside its symbol.
    const i32 close_body = close_box.x0 + 2;
    const i32 depth_body = depth_box.x0 + 1;
    CHECK(knob_shade(screen_pixel(r, screen, close_body, cy)));
    CHECK(knob_shade(screen_pixel(r, screen, depth_body, cy)));

    press(r, close_cx, cy);
    CHECK(held_shade(screen_pixel(r, screen, close_body, cy)));
    // Held, the square's outline turns light and its face dark.
    CHECK_EQ(screen_pixel(r, screen, close_cx - 3, cy), static_cast<u32>(chrome::kFrameLight));
    CHECK_EQ(screen_pixel(r, screen, close_cx, cy), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, close_cx, close_box.y0),
             static_cast<u32>(chrome::kFrameDark));
    CHECK(knob_shade(screen_pixel(r, screen, depth_body, cy)));

    // Off the button it rises again, and back over it it goes down again.
    move_to(r, close_box.x1 + 10, cy);
    CHECK(knob_shade(screen_pixel(r, screen, close_body, cy)));
    CHECK_EQ(screen_pixel(r, screen, close_cx - 3, cy), static_cast<u32>(chrome::kFrameDark));
    move_to(r, close_cx, cy);
    CHECK(held_shade(screen_pixel(r, screen, close_body, cy)));
    release(r, close_cx, cy);
    CHECK(knob_shade(screen_pixel(r, screen, close_body, cy)));
    const u32 msg = get_msg(r, port);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_CLOSEWINDOW);
    reply_msg(r, msg);

    press(r, depth_cx, cy);
    CHECK(held_shade(screen_pixel(r, screen, depth_body, cy)));
    CHECK_EQ(screen_pixel(r, screen, depth_cx + 1, cy + 1),
             static_cast<u32>(chrome::kFrameLight));
    CHECK_EQ(screen_pixel(r, screen, depth_cx - 3, cy - 3),
             static_cast<u32>(chrome::kFrameLight));
    CHECK(knob_shade(screen_pixel(r, screen, close_body, cy)));
    // Released off the button, it rises and the window stays where it is.
    move_to(r, depth_cx, depth_box.y1 + 30);
    CHECK(knob_shade(screen_pixel(r, screen, depth_body, cy)));
    release(r, depth_cx, depth_box.y1 + 30);
    CHECK(knob_shade(screen_pixel(r, screen, depth_body, cy)));
    CHECK_EQ(r.get32(screen + sc_FirstWindow), window);
    CHECK_EQ(get_msg(r, port), 0u);
}

void held_bar_button_sinks_on_a_four_colour_screen() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100,
                   WFLG_ACTIVATE | WFLG_CLOSEGADGET | WFLG_DEPTHGADGET, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);

    const Rect depth_box = Intuition::depth_box_rect(50, 60, 200);
    const i32 cx = (depth_box.x0 + depth_box.x1) / 2;
    const i32 cy = (depth_box.y0 + depth_box.y1) / 2;
    const i32 right = 50 + 200 - 1;
    const i32 top = 60;
    const i32 under = top + chrome::Metrics{}.bar_height();
    // Raised: after the SHADOWPEN line that ends the title, the button's
    // SHINEPEN edge on the left and along the top, the window's SHADOWPEN
    // edge on the right and the line under the bar, and FILLPEN (3)
    // between.
    CHECK_EQ(screen_pixel(r, screen, depth_box.x0, cy), 1u);
    CHECK_EQ(screen_pixel(r, screen, depth_box.x0 + 1, cy), 2u);
    CHECK_EQ(screen_pixel(r, screen, depth_box.x0 + 2, cy), 3u);
    CHECK_EQ(screen_pixel(r, screen, right, cy), 1u);
    CHECK_EQ(screen_pixel(r, screen, right - 1, cy), 3u);
    CHECK_EQ(screen_pixel(r, screen, cx, top), 2u);
    CHECK_EQ(screen_pixel(r, screen, cx, under), 1u);
    press(r, cx, cy);
    // Held, it is sunken: its edges swap their pens, and the dark line that
    // ends the title stays.
    CHECK_EQ(screen_pixel(r, screen, depth_box.x0, cy), 1u);
    CHECK_EQ(screen_pixel(r, screen, depth_box.x0 + 1, cy), 1u);
    CHECK_EQ(screen_pixel(r, screen, depth_box.x0 + 2, cy), 3u);
    CHECK_EQ(screen_pixel(r, screen, right, cy), 2u);
    CHECK_EQ(screen_pixel(r, screen, right - 1, cy), 3u);
    CHECK_EQ(screen_pixel(r, screen, cx, top), 1u);
    CHECK_EQ(screen_pixel(r, screen, cx, under), 2u);
    release(r, cx, cy);
    CHECK_EQ(screen_pixel(r, screen, depth_box.x0 + 1, cy), 2u);
    CHECK_EQ(screen_pixel(r, screen, right, cy), 1u);
    CHECK_EQ(screen_pixel(r, screen, cx, top), 2u);
    CHECK_EQ(screen_pixel(r, screen, cx, under), 1u);
}

void fill_backdrop_rect(GfxRig& r, u32 screen, i32 x0, i32 y0, i32 x1, i32 y1, u8 pen) {
    const u32 brp = r.intuition.backdrop_rastport(screen);
    CHECK(brp != 0);
    r.graphics.set_pens(brp, pen, 0, JAM2);
    r.graphics.rect_fill(brp, x0, y0, x1, y1);
}

void fill_window_rect(GfxRig& r, u32 window, i32 x0, i32 y0, i32 x1, i32 y1, u8 pen) {
    const u32 rp = r.get32(window + wd_RPort);
    r.cpu().a[1] = rp;
    r.cpu().d[0] = pen;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetAPen), ExitReason::Halt);
    r.cpu().a[1] = rp;
    r.cpu().d[0] = static_cast<u32>(x0);
    r.cpu().d[1] = static_cast<u32>(y0);
    r.cpu().d[2] = static_cast<u32>(x1);
    r.cpu().d[3] = static_cast<u32>(y1);
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::RectFill), ExitReason::Halt);
}

u32 rgb24_luminance(u32 rgb24) {
    const u32 r = (rgb24 >> 16) & 0xFF;
    const u32 g = (rgb24 >> 8) & 0xFF;
    const u32 b = rgb24 & 0xFF;
    return 299u * r + 587u * g + 114u * b;
}

void rounded_top_corners_on_depth8_show_backdrop_and_chrome() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    fill_backdrop_rect(r, screen, 0, 15, 639, 399, 5);

    put_new_window(r, kNewWindow, 50, 40, 120, 80, WFLG_ACTIVATE | WFLG_CLOSEGADGET, 0, 0, 0);
    const u32 win = open_win(r, kNewWindow);
    CHECK(win != 0);

    // Top-left cut corner pixels show the backdrop (pen 5).
    for (i32 x = 50; x <= 52; ++x) {
        CHECK_EQ(screen_pixel(r, screen, x, 40), 5u);
    }
    CHECK_EQ(screen_pixel(r, screen, 50, 41), 5u);
    CHECK_EQ(screen_pixel(r, screen, 51, 41), 5u);
    CHECK_EQ(screen_pixel(r, screen, 50, 42), 5u);

    // Top-right cut corner pixels show the backdrop (pen 5).
    for (i32 x = 167; x <= 169; ++x) {
        CHECK_EQ(screen_pixel(r, screen, x, 40), 5u);
    }
    CHECK_EQ(screen_pixel(r, screen, 168, 41), 5u);
    CHECK_EQ(screen_pixel(r, screen, 169, 41), 5u);
    CHECK_EQ(screen_pixel(r, screen, 169, 42), 5u);

    // Pixels just inside the cut corners show window chrome.
    CHECK_EQ(screen_pixel(r, screen, 53, 40), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 52, 41), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 53, 41), static_cast<u32>(chrome::kFrameLight));
    CHECK_EQ(screen_pixel(r, screen, 51, 42), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 166, 40), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 167, 41), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 168, 42), static_cast<u32>(chrome::kFrameDark));

    // Bottom corners stay square.
    CHECK_EQ(screen_pixel(r, screen, 50, 119), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 169, 119), static_cast<u32>(chrome::kFrameDark));

    // Borderless window on an 8-plane screen gets no corner cut.
    put_new_window(r, kNewWindow, 220, 40, 80, 60, WFLG_BORDERLESS, 0, 0, 0);
    const u32 borderless = open_win(r, kNewWindow);
    CHECK(borderless != 0);
    CHECK_EQ(screen_pixel(r, screen, 220, 40), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 299, 40), static_cast<u32>(chrome::kFrameDark));
}

void drop_shadow_darkens_backdrop_and_behind_window_only() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    fill_backdrop_rect(r, screen, 0, 15, 639, 399, 5);

    const u8* strong = nullptr;
    const u8* light = nullptr;
    CHECK(r.intuition.shade_tables(screen, strong, light));
    CHECK(strong[5] != 5u && light[5] != 5u);
    CHECK(strong[2] != 2u && light[2] != 2u);

    // Back window filled with pen 2 inside its interior.
    put_new_window(r, kNewWindow, 150, 80, 140, 90, 0, 0, 0, 0);
    const u32 back_win = open_win(r, kNewWindow);
    CHECK(back_win != 0);
    fill_window_rect(r, back_win, 4, 16, 135, 85, 2);

    // Front window: left=60, top=50, width=120, height=80 -> right=179, bottom=129.
    put_new_window(r, kNewWindow, 60, 50, 120, 80, WFLG_ACTIVATE, 0, 0, 0);
    const u32 front_win = open_win(r, kNewWindow);
    CHECK(front_win != 0);

    // Shadow over backdrop (above back_win at y=60, and left of back_win at x=70,y=131..133).
    CHECK_EQ(screen_pixel(r, screen, 180, 60), static_cast<u32>(strong[5]));
    CHECK_EQ(screen_pixel(r, screen, 181, 60), static_cast<u32>(strong[5]));
    CHECK_EQ(screen_pixel(r, screen, 182, 60), static_cast<u32>(light[5]));
    CHECK_EQ(screen_pixel(r, screen, 183, 60), static_cast<u32>(light[5]));
    CHECK_EQ(screen_pixel(r, screen, 70, 130), static_cast<u32>(strong[5]));
    CHECK_EQ(screen_pixel(r, screen, 70, 131), static_cast<u32>(strong[5]));
    CHECK_EQ(screen_pixel(r, screen, 70, 132), static_cast<u32>(light[5]));
    CHECK_EQ(screen_pixel(r, screen, 70, 133), static_cast<u32>(light[5]));

    // Areas outside the shadow band remain untouched backdrop pen 5.
    CHECK_EQ(screen_pixel(r, screen, 59, 60), 5u);
    CHECK_EQ(screen_pixel(r, screen, 180, 53), 5u);
    CHECK_EQ(screen_pixel(r, screen, 181, 50), 5u);
    CHECK_EQ(screen_pixel(r, screen, 63, 130), 5u);
    CHECK_EQ(screen_pixel(r, screen, 184, 60), 5u);

    // Shadow over back_win's interior (pen 2) at y=100, right+1..right+4 (x=180..183).
    CHECK_EQ(screen_pixel(r, screen, 181, 100), static_cast<u32>(strong[2]));
    CHECK_EQ(screen_pixel(r, screen, 183, 100), static_cast<u32>(light[2]));
    // Corner square over back_win (x=180..183, y=130..133): inner 2x2 strong, rest light.
    CHECK_EQ(screen_pixel(r, screen, 181, 131), static_cast<u32>(strong[2]));
    CHECK_EQ(screen_pixel(r, screen, 182, 131), static_cast<u32>(light[2]));
    CHECK_EQ(screen_pixel(r, screen, 181, 132), static_cast<u32>(light[2]));
    CHECK_EQ(screen_pixel(r, screen, 183, 133), static_cast<u32>(light[2]));

    // front_win's own pixels are never darkened.
    CHECK_EQ(screen_pixel(r, screen, 80, 80), 0u);
    CHECK_EQ(screen_pixel(r, screen, 179, 129), static_cast<u32>(chrome::kFrameDark));

    // Backdrop and borderless windows get no shadow.
    put_new_window(r, kNewWindow, 330, 50, 80, 60, WFLG_BACKDROP, 0, 0, 0);
    CHECK(open_win(r, kNewWindow) != 0);
    CHECK_EQ(screen_pixel(r, screen, 411, 70), 5u);
    CHECK_EQ(screen_pixel(r, screen, 350, 111), 5u);

    put_new_window(r, kNewWindow, 440, 50, 80, 60, WFLG_BORDERLESS, 0, 0, 0);
    CHECK(open_win(r, kNewWindow) != 0);
    CHECK_EQ(screen_pixel(r, screen, 521, 70), 5u);
    CHECK_EQ(screen_pixel(r, screen, 460, 111), 5u);
}

void moving_and_closing_window_removes_old_shadow() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    fill_backdrop_rect(r, screen, 0, 15, 639, 399, 5);

    const u8* strong = nullptr;
    const u8* light = nullptr;
    CHECK(r.intuition.shade_tables(screen, strong, light));

    // Window at (60, 50, 100, 60) -> right=159, bottom=109.
    r.refreshes.clear();
    put_new_window(r, kNewWindow, 60, 50, 100, 60, WFLG_ACTIVATE | WFLG_DRAGBAR, 0, 0, 0);
    const u32 win = open_win(r, kNewWindow);
    CHECK(win != 0);
    CHECK(refreshed(r, screen, 60, 50, 163, 113));
    CHECK_EQ(screen_pixel(r, screen, 161, 60), static_cast<u32>(strong[5]));
    CHECK_EQ(screen_pixel(r, screen, 100, 113), static_cast<u32>(light[5]));

    // MoveWindow-style move restores old shadow area to plain backdrop.
    r.refreshes.clear();
    r.intuition.move_window_to(win, 200, 150);
    CHECK(refreshed(r, screen, 60, 50, 303, 213));
    CHECK_EQ(screen_pixel(r, screen, 161, 60), 5u);
    CHECK_EQ(screen_pixel(r, screen, 100, 113), 5u);
    CHECK_EQ(screen_pixel(r, screen, 301, 160), static_cast<u32>(strong[5]));
    CHECK_EQ(screen_pixel(r, screen, 240, 213), static_cast<u32>(light[5]));

    // Drag path also restores old shadow area.
    press(r, 230, 156);
    move_to(r, 330, 206);
    release(r, 330, 206);
    CHECK_EQ(r.get16(win + wd_LeftEdge), 300u);
    CHECK_EQ(r.get16(win + wd_TopEdge), 200u);
    CHECK_EQ(screen_pixel(r, screen, 301, 160), 5u);
    CHECK_EQ(screen_pixel(r, screen, 240, 213), 5u);
    CHECK_EQ(screen_pixel(r, screen, 401, 210), static_cast<u32>(strong[5]));
    CHECK_EQ(screen_pixel(r, screen, 340, 263), static_cast<u32>(light[5]));

    // Closing the window removes its shadow cleanly.
    r.refreshes.clear();
    r.cpu().a[0] = win;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);
    CHECK(refreshed(r, screen, 300, 200, 403, 263));
    CHECK_EQ(screen_pixel(r, screen, 401, 210), 5u);
    CHECK_EQ(screen_pixel(r, screen, 340, 263), 5u);
}

u32 chrome_pen_on(u32 logical, u32 depth) {
    return chrome::physical_pen(static_cast<u8>(logical), depth);
}

// The chrome pens of an eight-plane screen reach a screen of fewer planes at
// physical_pen: all of them at the top of the ColorMap on seven and six planes,
// the mirrors there and 12 shared pens below them on five.
void chrome_pens_find_their_place_on_every_depth() {
    for (u32 logical = chrome::kScreenBar0; logical < 256; ++logical) {
        CHECK_EQ(chrome_pen_on(logical, 8), logical);
        CHECK_EQ(chrome_pen_on(logical, 7), logical - 128);
        CHECK_EQ(chrome_pen_on(logical, 6), logical - 192);
        CHECK_EQ(chrome_pen_on(logical, 4), logical);
    }
    CHECK_EQ(chrome_pen_on(15, 5), 15u);
    CHECK_EQ(chrome_pen_on(15, 8), 15u);

    bool used[32]{};
    for (u32 logical = chrome::kScreenBar0; logical < chrome::kComplementMirrorBase; ++logical) {
        const u32 pen = chrome_pen_on(logical, 5);
        CHECK(pen >= 16 && pen <= 27);
        used[pen] = true;
    }
    for (u32 pen = 16; pen <= 27; ++pen) {
        CHECK(used[pen]);
    }
    for (u32 pen = 0; pen < chrome::kComplementMirrorPens; ++pen) {
        CHECK_EQ(chrome_pen_on(chrome::mirror_pen(static_cast<u8>(pen)), 5), 28 + pen);
    }

    const auto same = [](u32 a, u32 b) { return chrome_pen_on(a, 5) == chrome_pen_on(b, 5); };
    // The active bar's 8 steps go 3, 2 and 3, the inactive bar's 6 by 3, the
    // screen bar's 4 two and two and the knob's 3 by 2 and 1.
    CHECK(same(chrome::kActiveBar0, chrome::kActiveBar2));
    CHECK(!same(chrome::kActiveBar2, chrome::kActiveBar3));
    CHECK(same(chrome::kActiveBar3, chrome::kActiveBar4));
    CHECK(!same(chrome::kActiveBar4, chrome::kActiveBar5));
    CHECK(same(chrome::kActiveBar5, chrome::kActiveBar7));
    CHECK(same(chrome::kInactiveBar0, chrome::kInactiveBar2));
    CHECK(!same(chrome::kInactiveBar2, chrome::kInactiveBar3));
    CHECK(same(chrome::kInactiveBar3, chrome::kInactiveBar5));
    CHECK(same(chrome::kScreenBar0, chrome::kScreenBar1));
    CHECK(!same(chrome::kScreenBar1, chrome::kScreenBar2));
    CHECK(same(chrome::kScreenBar2, chrome::kScreenBar3));
    CHECK(same(chrome::kKnob0, chrome::kKnob1));
    CHECK(!same(chrome::kKnob1, chrome::kKnob2));
    // The menu's label and lit text share the dark pen with the frame's dark
    // edge and the titles, its paper the light pen with the frame's light edge,
    // the ghost has its own, and the track is the inactive bar's darker pen.
    CHECK(same(chrome::kMenuLabelPen, chrome::kMenuLitPen));
    CHECK(same(chrome::kMenuLabelPen, chrome::kFrameDark));
    CHECK(same(chrome::kMenuLabelPen, chrome::kTitleActive));
    CHECK(same(chrome::kMenuLabelPen, chrome::kTitleInactive));
    CHECK(same(chrome::kMenuPaperPen, chrome::kFrameLight));
    CHECK(!same(chrome::kMenuGhostPen, chrome::kMenuPaperPen));
    CHECK(!same(chrome::kMenuGhostPen, chrome::kMenuLabelPen));
    CHECK(same(chrome::kTrack, chrome::kInactiveBar5));

    // A shared pen has the colour of the middle one of the pens it stands for.
    CHECK_EQ(chrome::logical_pen(chrome_pen_on(chrome::kActiveBar0, 5), 5),
             static_cast<u32>(chrome::kActiveBar1));
    CHECK_EQ(chrome::logical_pen(chrome_pen_on(chrome::kActiveBar3, 5), 5),
             static_cast<u32>(chrome::kActiveBar4));
    CHECK_EQ(chrome::logical_pen(chrome_pen_on(chrome::kActiveBar7, 5), 5),
             static_cast<u32>(chrome::kActiveBar6));
    CHECK_EQ(chrome::logical_pen(chrome_pen_on(chrome::kInactiveBar5, 5), 5),
             static_cast<u32>(chrome::kInactiveBar4));
    CHECK_EQ(chrome::logical_pen(chrome_pen_on(chrome::kFrameDark, 5), 5),
             static_cast<u32>(chrome::kMenuLabelPen));
    // The pens below the chrome range have no chrome pen.
    CHECK_EQ(chrome::logical_pen(15, 5), 0u);
    CHECK_EQ(chrome::logical_pen(27, 6), 0u);
    CHECK_EQ(chrome::logical_pen(91, 7), 0u);
    CHECK_EQ(chrome::logical_pen(219, 8), 0u);
    CHECK_EQ(chrome::logical_pen(220, 8), static_cast<u32>(chrome::kScreenBar0));
}

// What a 5, 6 or 7 plane screen draws of a window: the modern look in the pens
// physical_pen gives the chrome there.
void check_modern_window_chrome(u32 depth) {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, depth, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const auto pen = [depth](u32 logical) { return chrome_pen_on(logical, depth); };

    // The screen bar is shaded from top to bottom, over the label pen's line.
    CHECK_EQ(screen_pixel(r, screen, 100, 0), pen(chrome::kScreenBar0));
    CHECK_EQ(screen_pixel(r, screen, 100, 13), pen(chrome::kScreenBar3));
    CHECK_EQ(screen_pixel(r, screen, 100, 14), pen(chrome::kMenuLabelPen));

    r.str(kWinTitle, "H");
    put_new_window(r, kNewWindow, 40, 30, 220, 120,
                   WFLG_ACTIVATE | WFLG_CLOSEGADGET | WFLG_DEPTHGADGET | WFLG_SIZEGADGET,
                   kWinTitle, 0, 0);
    CHECK(open_win(r, kNewWindow) != 0);

    // The cut corner shows the backdrop, the outline is dark, the left border
    // light and the others the inactive bar's pen.
    CHECK_EQ(screen_pixel(r, screen, 40, 30), 0u);
    CHECK_EQ(screen_pixel(r, screen, 43, 30), pen(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 40, 149), pen(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 41, 60), pen(chrome::kFrameLight));
    CHECK_EQ(screen_pixel(r, screen, 258, 60), pen(chrome::kInactiveBar5));
    CHECK_EQ(screen_pixel(r, screen, 100, 148), pen(chrome::kInactiveBar5));
    CHECK_EQ(screen_pixel(r, screen, 42, 45), 0u);

    // The active title bar: a light line, the bar from its first step to its
    // last, and a dark line.
    CHECK_EQ(screen_pixel(r, screen, 120, 31), pen(chrome::kFrameLight));
    CHECK_EQ(screen_pixel(r, screen, 120, 32), pen(chrome::kActiveBar0));
    CHECK_EQ(screen_pixel(r, screen, 120, 43), pen(chrome::kActiveBar7));
    CHECK_EQ(screen_pixel(r, screen, 120, 44), pen(chrome::kFrameDark));

    // The buttons, the title and the size gadget.
    CHECK_EQ(screen_pixel(r, screen, 49, 32), pen(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 45, 33), pen(chrome::kKnob0));
    CHECK_EQ(screen_pixel(r, screen, 45, 41), pen(chrome::kKnob2));
    CHECK_EQ(screen_pixel(r, screen, 49, 37), pen(chrome::kFrameLight));
    CHECK_EQ(screen_pixel(r, screen, 248, 35), pen(chrome::kKnob2));
    CHECK_EQ(screen_pixel(r, screen, 250, 37), pen(chrome::kFrameLight));
    CHECK_EQ(screen_pixel(r, screen, 59, 33), pen(chrome::kTitleActive));
    CHECK_EQ(screen_pixel(r, screen, 256, 146), pen(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 255, 145), pen(chrome::kFrameLight));

    // A pen of the modern look is not one of the classic look's.
    CHECK(screen_pixel(r, screen, 120, 32) > 3u);
}

void shallow_modern_screens_draw_the_window_chrome_in_the_mapped_pens() {
    for (const u32 depth : {5u, 6u, 7u}) {
        check_modern_window_chrome(depth);
    }
}

void shallow_modern_screens_draw_prop_gadgets_in_the_mapped_pens() {
    for (const u32 depth : {5u, 6u, 7u}) {
        GfxRig r;
        put_new_screen(r, kNewScreen, 640, 400, depth, 0, 1, 0);
        CHECK(open_new(r, kNewScreen) != 0);
        const auto pen = [depth](u32 logical) { return chrome_pen_on(logical, depth); };

        put_prop_info(r, kProp1, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 4);
        put_gadget(r, kGad1, 0, 20, 20, 12, 40, 0, 0, GTYP_PROPGADGET, kProp1);
        put_new_window(r, kNewWindow, 50, 50, 220, 140, WFLG_ACTIVATE, 0, 0, 0);
        r.put32(kNewWindow + nw_FirstGadget, kGad1);
        const u32 window = open_win(r, kNewWindow);
        r.put32(kNewWindow + nw_FirstGadget, 0);
        CHECK(window != 0);

        // The container's dark edge and track, the knob's first and last steps.
        CHECK_EQ(window_pixel(r, window, 20, 20), pen(chrome::kFrameDark));
        CHECK_EQ(window_pixel(r, window, 25, 50), pen(chrome::kTrack));
        CHECK_EQ(window_pixel(r, window, 21, 21), pen(chrome::kTrack));
        CHECK_EQ(window_pixel(r, window, 25, 22), pen(chrome::kKnob0));
        CHECK_EQ(window_pixel(r, window, 25, 28), pen(chrome::kKnob2));

        // Dragged, the knob takes the active bar's pens.
        press(r, 75, 75);
        const u32 held = window_pixel(r, window, 25, 25);
        CHECK(held >= pen(chrome::kActiveBar2) && held <= pen(chrome::kActiveBar5));
        release(r, 75, 75);
        const u32 plain = window_pixel(r, window, 25, 25);
        CHECK(plain >= pen(chrome::kKnob0) && plain <= pen(chrome::kKnob2));
    }
}

// Each pen of the chrome range holds the colour of the chrome pen it stands
// for on an eight-plane screen, the pens below it keep the base palette and
// the colour cube, and the top four pens keep the colours of 0 to 3.
void shallow_modern_screens_hold_the_chrome_colours_at_the_mapped_pens() {
    GfxRig full;
    put_new_screen(full, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 full_screen = open_new(full, kNewScreen);
    CHECK(full_screen != 0);
    const u32 full_map = full.get32(full_screen + sc_ViewPort + vp_ColorMap);

    for (const u32 depth : {5u, 6u, 7u}) {
        GfxRig r;
        put_new_screen(r, kNewScreen, 640, 400, depth, 0, 1, 0);
        const u32 screen = open_new(r, kNewScreen);
        CHECK(screen != 0);
        const u32 map = r.get32(screen + sc_ViewPort + vp_ColorMap);
        const u32 pens = 1u << depth;
        CHECK_EQ(r.get16(map + cm_Count), pens);

        CHECK_EQ(colormap_rgb24(r, map, 0), 0xA0A6B0u);
        CHECK_EQ(colormap_rgb24(r, map, 3), 0x3D6FB6u);
        CHECK_EQ(colormap_rgb24(r, map, 8), 0x000000u);
        CHECK_EQ(colormap_rgb24(r, map, 9), 0x000033u);

        const u32 first = depth == 5 ? 16 : pens - chrome::kChromePens;
        CHECK(first > 15);
        for (u32 pen = first; pen < pens; ++pen) {
            const u32 logical = chrome::logical_pen(pen, depth);
            CHECK(logical >= chrome::kScreenBar0);
            CHECK_EQ(chrome_pen_on(logical, depth), pen);
            CHECK_EQ(colormap_rgb24(r, map, pen), colormap_rgb24(full, full_map, logical));
        }
        for (u32 pen = 0; pen < chrome::kComplementMirrorPens; ++pen) {
            CHECK_EQ(colormap_rgb24(r, map, pens - chrome::kComplementMirrorPens + pen),
                     colormap_rgb24(r, map, pen));
        }
    }
}

// Shadows pick the nearest of the screen's own 2^depth colours.
void shallow_modern_screens_cast_shadows_in_their_own_palette() {
    for (const u32 depth : {5u, 6u, 7u}) {
        GfxRig r;
        put_new_screen(r, kNewScreen, 640, 400, depth, 0, 1, 0);
        const u32 screen = open_new(r, kNewScreen);
        CHECK(screen != 0);
        fill_backdrop_rect(r, screen, 0, 15, 639, 399, 5);

        const u8* strong = nullptr;
        const u8* light = nullptr;
        CHECK(r.intuition.shade_tables(screen, strong, light));
        for (u32 pen = 0; pen < (1u << depth); ++pen) {
            CHECK(strong[pen] < (1u << depth));
            CHECK(light[pen] < (1u << depth));
        }
        CHECK(strong[5] != 5u && light[5] != 5u);

        put_new_window(r, kNewWindow, 60, 50, 120, 80, WFLG_ACTIVATE, 0, 0, 0);
        CHECK(open_win(r, kNewWindow) != 0);
        CHECK_EQ(screen_pixel(r, screen, 180, 60), static_cast<u32>(strong[5]));
        CHECK_EQ(screen_pixel(r, screen, 182, 60), static_cast<u32>(light[5]));
        CHECK_EQ(screen_pixel(r, screen, 59, 60), 5u);
        CHECK_EQ(screen_pixel(r, screen, 184, 60), 5u);
    }
}

// The arrow button of a scroll gadget on a classic screen is the system's
// arrow image of an active window's border, in whatever DrawInfo pens: what
// sysimage draws for UP, DOWN, LEFT and RIGHTIMAGE, held down or not.
void classic_arrow_buttons_are_the_system_images() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    native::Bus bus(r.machine);
    const u32 rp = screen + sc_RastPort;
    const u32 images[4] = {UPIMAGE, DOWNIMAGE, LEFTIMAGE, RIGHTIMAGE};
    const u16 pen_sets[2][12] = {{0, 1, 1, 2, 1, 3, 1, 0, 2, 1, 2, 1},
                                 {0, 1, 2, 3, 2, 1, 0, 3, 2, 1, 2, 1}};
    const i32 sizes[4][2] = {{14, 10}, {14, 14}, {16, 10}, {20, 12}};
    for (const u16* pen_set : {&pen_sets[0][0], &pen_sets[1][0]}) {
        chrome::DrawInfoPens drawinfo;
        for (u32 i = 0; i < sysimage::kPenCount; ++i) {
            drawinfo.pen[i] = pen_set[i];
        }
        for (const i32* size : sizes) {
            for (u32 arrow = 0; arrow < 4; ++arrow) {
                for (const bool pressed : {false, true}) {
                    u8 pens[20 * 14];
                    chrome::paint_arrow_button(pens, size[0], size[1],
                                               static_cast<chrome::Arrow>(arrow), pressed,
                                               chrome::Style::Classic, 2, drawinfo);
                    sysimage::draw(r.graphics, bus, rp, images[arrow],
                                   Rect{20, 60, 20 + size[0] - 1, 60 + size[1] - 1},
                                   pressed ? IDS_SELECTED : IDS_NORMAL, drawinfo.pen);
                    for (i32 y = 0; y < size[1]; ++y) {
                        for (i32 x = 0; x < size[0]; ++x) {
                            CHECK_EQ(static_cast<u32>(pens[y * size[0] + x]),
                                     screen_pixel(r, screen, 20 + x, 60 + y));
                        }
                    }
                }
            }
        }
    }

    // With the 3.1 pens an up arrow on the fill pen has SHINEPEN along its
    // top and left and SHADOWPEN along its bottom and right, and held down
    // the two the other way round; its triangle is SHADOWPEN.
    const chrome::DrawInfoPens defaults{{0, 1, 1, 2, 1, 3, 1, 0, 2, 1, 2, 1}};
    u8 up[14 * 10];
    chrome::paint_arrow_button(up, 14, 10, chrome::Arrow::Up, false, chrome::Style::Classic, 2,
                               defaults);
    CHECK_EQ(static_cast<u32>(up[0]), 2u);
    CHECK_EQ(static_cast<u32>(up[13]), 1u);
    CHECK_EQ(static_cast<u32>(up[9 * 14]), 2u);
    CHECK_EQ(static_cast<u32>(up[9 * 14 + 13]), 1u);
    CHECK_EQ(static_cast<u32>(up[5 * 14 + 3]), 3u);
    CHECK_EQ(static_cast<u32>(up[3 * 14 + 6]), 1u);
    chrome::paint_arrow_button(up, 14, 10, chrome::Arrow::Up, true, chrome::Style::Classic, 2,
                               defaults);
    CHECK_EQ(static_cast<u32>(up[0]), 1u);
    CHECK_EQ(static_cast<u32>(up[13]), 2u);
    CHECK_EQ(static_cast<u32>(up[5 * 14 + 3]), 3u);
}

// On a screen of 5, 6 or 7 planes an arrow button is the modern look's, the
// pens of an eight-plane screen's at their place on that screen.
void shallow_modern_arrow_buttons_use_the_mapped_pens() {
    const chrome::DrawInfoPens drawinfo;
    const i32 sizes[3][2] = {{14, 10}, {14, 14}, {16, 10}};
    for (const u32 depth : {5u, 6u, 7u}) {
        for (const i32* size : sizes) {
            for (u32 arrow = 0; arrow < 4; ++arrow) {
                for (const bool pressed : {false, true}) {
                    u8 full[16 * 14];
                    u8 shallow[16 * 14];
                    const chrome::Arrow which = static_cast<chrome::Arrow>(arrow);
                    chrome::paint_arrow_button(full, size[0], size[1], which, pressed,
                                               chrome::Style::Modern, 8, drawinfo);
                    chrome::paint_arrow_button(shallow, size[0], size[1], which, pressed,
                                               chrome::Style::Modern, depth, drawinfo);
                    for (i32 i = 0; i < size[0] * size[1]; ++i) {
                        CHECK(full[i] >= chrome::kScreenBar0);
                        CHECK_EQ(static_cast<u32>(shallow[i]), chrome_pen_on(full[i], depth));
                        CHECK(shallow[i] != full[i]);
                    }
                }
            }
        }
    }
}

void depth2_screen_has_no_corners_and_no_shadow() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    fill_backdrop_rect(r, screen, 0, 15, 639, 399, 3);

    const u8* strong = nullptr;
    const u8* light = nullptr;
    CHECK(!r.intuition.shade_tables(screen, strong, light));

    put_new_window(r, kNewWindow, 50, 40, 120, 80, WFLG_ACTIVATE, 0, 0, 0);
    const u32 win = open_win(r, kNewWindow);
    CHECK(win != 0);

    // No corner cut on 2-plane screen: the raised edge reaches both corners.
    CHECK_EQ(screen_pixel(r, screen, 50, 40), 2u);
    CHECK_EQ(screen_pixel(r, screen, 169, 40), 1u);

    // No shadow on 2-plane screen.
    CHECK_EQ(screen_pixel(r, screen, 171, 50), 3u);
    CHECK_EQ(screen_pixel(r, screen, 173, 50), 3u);
    CHECK_EQ(screen_pixel(r, screen, 100, 121), 3u);
    CHECK_EQ(screen_pixel(r, screen, 100, 123), 3u);
}

void shade_table_darkens_default_pen0_and_updates_on_setrgb32() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);

    const u8* strong = nullptr;
    const u8* light = nullptr;
    CHECK(r.intuition.shade_tables(screen, strong, light));
    const u32 map = r.get32(screen + sc_ViewPort + vp_ColorMap);
    const u32 rgb0 = colormap_rgb24(r, map, 0);
    const u32 rgb_strong0 = colormap_rgb24(r, map, strong[0]);
    const u32 rgb_light0 = colormap_rgb24(r, map, light[0]);
    CHECK(strong[0] != 0u);
    CHECK(light[0] != 0u);
    CHECK(rgb24_luminance(rgb_strong0) < rgb24_luminance(rgb0));
    CHECK(rgb24_luminance(rgb_light0) < rgb24_luminance(rgb0));

    // Change pen 0 to pure white and pens 10/11 to exact 0.70 / 0.85 targets.
    const u32 vp = screen + sc_ViewPort;
    auto set_rgb32 = [&](u32 pen, u8 cr, u8 cg, u8 cb) {
        r.cpu().a[0] = vp;
        r.cpu().d[0] = pen;
        r.cpu().d[1] = static_cast<u32>(cr) * 0x01010101u;
        r.cpu().d[2] = static_cast<u32>(cg) * 0x01010101u;
        r.cpu().d[3] = static_cast<u32>(cb) * 0x01010101u;
        CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetRGB32), ExitReason::Halt);
    };
    set_rgb32(0, 0xFF, 0xFF, 0xFF);
    set_rgb32(10, 179, 179, 179);  // 255 * 0.70 = 178.5 -> 179
    set_rgb32(11, 217, 217, 217);  // 255 * 0.85 = 216.75 -> 217

    CHECK(r.intuition.shade_tables(screen, strong, light));
    CHECK_EQ(strong[0], 10u);
    CHECK_EQ(light[0], 11u);

    // Opening a window after SetRGB32 uses the rebuilt shade table in compose.
    put_new_window(r, kNewWindow, 50, 40, 100, 60, 0, 0, 0, 0);
    CHECK(open_win(r, kNewWindow) != 0);
    CHECK_EQ(screen_pixel(r, screen, 151, 50), 10u);
    CHECK_EQ(screen_pixel(r, screen, 153, 50), 11u);
}

std::vector<u8> screen_plane_bytes(GfxRig& r, u32 screen) {
    const u32 bm = screen + sc_BitMap;
    const u32 bpr = r.get16(bm + bm_BytesPerRow);
    const u32 rows = r.get16(bm + bm_Rows);
    const u32 depth = r.get8(bm + bm_Depth);
    const u32 plane_bytes = bpr * rows;
    std::vector<u8> out(plane_bytes * depth);
    for (u32 plane = 0; plane < depth; ++plane) {
        const u32 ptr = r.get32(bm + bm_Planes + plane * 4);
        for (u32 i = 0; i < plane_bytes; ++i) {
            out[plane * plane_bytes + i] = static_cast<u8>(r.get8(ptr + i));
        }
    }
    return out;
}

void flush_palette_recomposes_8plane_shadows_and_end_batch_flushes() {
    auto setup_scene = [](GfxRig& r, bool palette_before_windows) -> u32 {
        put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
        const u32 screen = open_new(r, kNewScreen);
        CHECK(screen != 0);
        fill_backdrop_rect(r, screen, 0, 15, 639, 399, 5);

        auto load_new_palette = [&]() {
            const u32 table = r.exec.alloc(256, MEMF_PUBLIC | MEMF_CLEAR);
            CHECK(table != 0);
            u32 p = table;
            // Change pen 5 (backdrop) to bright cyan and pen 2 to bright yellow,
            // and put exact 0.70 / 0.85 shade targets in pens 200..203.
            r.put32(p, (1u << 16) | 5u);
            p += 4;
            r.put32(p, 0x00000000u);
            r.put32(p + 4, 0xFFFFFFFFu);
            r.put32(p + 8, 0xFFFFFFFFu);
            p += 12;
            r.put32(p, (1u << 16) | 2u);
            p += 4;
            r.put32(p, 0xFFFFFFFFu);
            r.put32(p + 4, 0xFFFFFFFFu);
            r.put32(p + 8, 0x00000000u);
            p += 12;
            r.put32(p, (4u << 16) | 200u);
            p += 4;
            // 200: 0.70 * (0,255,255) -> (0, 179, 179)
            r.put32(p, 0u);
            r.put32(p + 4, 179u * 0x01010101u);
            r.put32(p + 8, 179u * 0x01010101u);
            p += 12;
            // 201: 0.85 * (0,255,255) -> (0, 217, 217)
            r.put32(p, 0u);
            r.put32(p + 4, 217u * 0x01010101u);
            r.put32(p + 8, 217u * 0x01010101u);
            p += 12;
            // 202: 0.70 * (255,255,0) -> (179, 179, 0)
            r.put32(p, 179u * 0x01010101u);
            r.put32(p + 4, 179u * 0x01010101u);
            r.put32(p + 8, 0u);
            p += 12;
            // 203: 0.85 * (255,255,0) -> (217, 217, 0)
            r.put32(p, 217u * 0x01010101u);
            r.put32(p + 4, 217u * 0x01010101u);
            r.put32(p + 8, 0u);
            p += 12;
            r.put32(p, 0u);

            r.cpu().a[0] = screen + sc_ViewPort;
            r.cpu().a[1] = table;
            CHECK_EQ(r.call(Base::Graphics, graphics_lvo::LoadRGB32), ExitReason::Halt);
        };

        if (palette_before_windows) {
            load_new_palette();
            r.intuition.flush_palette();
        }
        put_new_window(r, kNewWindow, 150, 80, 140, 90, 0, 0, 0, 0);
        const u32 back_win = open_win(r, kNewWindow);
        CHECK(back_win != 0);
        fill_window_rect(r, back_win, 4, 16, 135, 85, 2);

        put_new_window(r, kNewWindow, 60, 50, 120, 80, WFLG_ACTIVATE, 0, 0, 0);
        const u32 front_win = open_win(r, kNewWindow);
        CHECK(front_win != 0);

        if (!palette_before_windows) {
            r.refreshes.clear();
            load_new_palette();
            CHECK(r.refreshes.empty());
        }
        return screen;
    };

    GfxRig ref;
    const u32 ref_screen = setup_scene(ref, true);
    GfxRig r;
    const u32 screen = setup_scene(r, false);

    // Shadow pixels are still stale until flush_palette() runs.
    CHECK(screen_pixel(r, screen, 180, 60) != 200u);
    r.intuition.flush_palette();
    CHECK_EQ(r.refreshes.size(), 1u);
    CHECK_EQ(r.refreshes[0].x0, 0);
    CHECK_EQ(r.refreshes[0].y0, 0);
    CHECK_EQ(r.refreshes[0].x1, 639);
    CHECK_EQ(r.refreshes[0].y1, 399);

    const u8* strong = nullptr;
    const u8* light = nullptr;
    CHECK(r.intuition.shade_tables(screen, strong, light));
    CHECK_EQ(strong[5], 200u);
    CHECK_EQ(light[5], 201u);
    CHECK_EQ(strong[2], 202u);
    CHECK_EQ(light[2], 203u);

    for (i32 y = 54; y <= 133; ++y) {
        for (i32 x = 64; x <= 183; ++x) {
            if (x <= 179 && y <= 129) {
                continue;
            }
            if ((x <= 181 && y <= 131) && (x >= 180 || y >= 130)) {
                const u32 under = (x >= 154 && x <= 285 && y >= 96 && y <= 165) ? 2u : 5u;
                if (x >= 150 && y >= 80 && !(x >= 154 && y >= 96)) {
                    continue;
                }
                CHECK_EQ(screen_pixel(r, screen, x, y), static_cast<u32>(strong[under]));
            } else if (x >= 180 || y >= 130) {
                const u32 under = (x >= 154 && x <= 285 && y >= 96 && y <= 165) ? 2u : 5u;
                if (x >= 150 && y >= 80 && !(x >= 154 && y >= 96)) {
                    continue;
                }
                CHECK_EQ(screen_pixel(r, screen, x, y), static_cast<u32>(light[under]));
            }
        }
    }
    CHECK(screen_plane_bytes(r, screen) == screen_plane_bytes(ref, ref_screen));

    // Nested batches defer palette flush until the outermost end_batch().
    r.refreshes.clear();
    r.intuition.begin_batch();
    r.intuition.begin_batch();
    for (u32 i = 0; i < 10; ++i) {
        r.cpu().a[0] = screen + sc_ViewPort;
        r.cpu().d[0] = 20 + i;
        r.cpu().d[1] = 0x11111111u * (i + 1);
        r.cpu().d[2] = 0x11111111u * (i + 1);
        r.cpu().d[3] = 0x11111111u * (i + 1);
        CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetRGB32), ExitReason::Halt);
    }
    r.intuition.end_batch();
    CHECK(r.refreshes.empty());
    CHECK(r.display.palette_dirty);
    r.intuition.end_batch();
    CHECK_EQ(r.refreshes.size(), 1u);
    CHECK(!r.display.palette_dirty);
}

void backdrop_window_has_square_corners_no_shadow_and_stays_behind() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    fill_backdrop_rect(r, screen, 0, 15, 639, 399, 5);

    put_new_window(r, kNewWindow, 100, 50, 140, 90, WFLG_ACTIVATE, 0, 0, 0);
    const u32 n1 = open_win(r, kNewWindow);
    CHECK(n1 != 0);
    fill_window_rect(r, n1, 4, 16, 135, 85, 2);

    put_new_window(r, kNewWindow, 60, 40, 120, 80,
                   WFLG_BACKDROP | WFLG_DRAGBAR | WFLG_CLOSEGADGET | WFLG_DEPTHGADGET,
                   0, 0, 0);
    const u32 b1 = open_win(r, kNewWindow);
    CHECK(b1 != 0);

    // b1 opened behind n1.
    CHECK_EQ(r.get32(screen + sc_FirstWindow), n1);
    CHECK_EQ(r.get32(n1 + wd_NextWindow), b1);
    CHECK_EQ(screen_pixel(r, screen, 120, 70), 2u);

    // Square corners (no cut corners) and no drop shadow on b1.
    CHECK_EQ(screen_pixel(r, screen, 60, 40), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, 61, 41), static_cast<u32>(chrome::kFrameLight));
    CHECK_EQ(screen_pixel(r, screen, 62, 41), static_cast<u32>(chrome::kFrameLight));
    CHECK_EQ(screen_pixel(r, screen, 80, 121), 5u);
    CHECK_EQ(screen_pixel(r, screen, 80, 123), 5u);

    put_new_window(r, kNewWindow, 70, 45, 120, 80, WFLG_BACKDROP, 0, 0, 0);
    const u32 b2 = open_win(r, kNewWindow);
    CHECK(b2 != 0);
    CHECK_EQ(r.get32(screen + sc_FirstWindow), n1);
    CHECK_EQ(r.get32(n1 + wd_NextWindow), b2);
    CHECK_EQ(r.get32(b2 + wd_NextWindow), b1);
    CHECK_EQ(screen_pixel(r, screen, 189, 45), static_cast<u32>(chrome::kFrameDark));

    // WindowToFront on b1 brings it ahead of b2, never above normal window n1.
    r.cpu().a[0] = b1;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::WindowToFront), ExitReason::Halt);
    CHECK_EQ(r.get32(screen + sc_FirstWindow), n1);
    CHECK_EQ(r.get32(n1 + wd_NextWindow), b1);
    CHECK_EQ(r.get32(b1 + wd_NextWindow), b2);
    CHECK_EQ(screen_pixel(r, screen, 120, 70), 2u);

    // New normal window opens in front of both n1 and backdrop windows.
    put_new_window(r, kNewWindow, 260, 50, 100, 60, WFLG_ACTIVATE, 0, 0, 0);
    const u32 n2 = open_win(r, kNewWindow);
    CHECK(n2 != 0);
    CHECK_EQ(r.get32(screen + sc_FirstWindow), n2);
    CHECK_EQ(r.get32(n2 + wd_NextWindow), n1);

    // WindowToBack on n2 puts it behind n1, still ahead of backdrop windows.
    r.cpu().a[0] = n2;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::WindowToBack), ExitReason::Halt);
    CHECK_EQ(r.get32(screen + sc_FirstWindow), n1);
    CHECK_EQ(r.get32(n1 + wd_NextWindow), n2);
    CHECK_EQ(r.get32(n2 + wd_NextWindow), b1);
}

void size_box_hit_test_matches_border_placement() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);

    // Right-border default: x width-11..width-2, y height-11..height-2.
    const Rect rb = Intuition::size_box_rect(40, 40, 200, 100, WFLG_SIZEGADGET);
    CHECK_EQ(rb.x0, 229);
    CHECK_EQ(rb.y0, 129);
    CHECK_EQ(rb.x1, 238);
    CHECK_EQ(rb.y1, 138);

    put_new_window(r, kNewWindow, 40, 40, 200, 100, WFLG_SIZEGADGET | WFLG_ACTIVATE, 0, 0, 0);
    r.put16(kNewWindow + nw_MaxWidth, 0xFFFF);
    r.put16(kNewWindow + nw_MaxHeight, 0xFFFF);
    const u32 w_right = open_win(r, kNewWindow);
    CHECK(w_right != 0);

    // 1 px above size box and 1 px outer frames do not trigger resize.
    press(r, 229, 128);
    move_to(r, 239, 138);
    release(r, 239, 138);
    CHECK_EQ(r.get16(w_right + wd_Width), 200u);
    CHECK_EQ(r.get16(w_right + wd_Height), 100u);

    press(r, 239, 135);
    move_to(r, 249, 145);
    release(r, 249, 145);
    CHECK_EQ(r.get16(w_right + wd_Width), 200u);

    press(r, 235, 139);
    move_to(r, 245, 149);
    release(r, 245, 149);
    CHECK_EQ(r.get16(w_right + wd_Height), 100u);

    // Top-left pixel of the 10x10 size box resizes.
    press(r, 229, 129);
    move_to(r, 239, 139);
    release(r, 239, 139);
    CHECK_EQ(r.get16(w_right + wd_Width), 210u);
    CHECK_EQ(r.get16(w_right + wd_Height), 110u);

    // Bottom-border placement: y height-9..height-2.
    const Rect bb =
        Intuition::size_box_rect(300, 40, 200, 100, WFLG_SIZEGADGET | WFLG_SIZEBBOTTOM);
    CHECK_EQ(bb.x0, 489);
    CHECK_EQ(bb.y0, 131);
    CHECK_EQ(bb.x1, 498);
    CHECK_EQ(bb.y1, 138);

    put_new_window(r, kNewWindow, 300, 40, 200, 100,
                   WFLG_SIZEGADGET | WFLG_SIZEBBOTTOM | WFLG_ACTIVATE, 0, 0, 0);
    r.put16(kNewWindow + nw_MaxWidth, 0xFFFF);
    r.put16(kNewWindow + nw_MaxHeight, 0xFFFF);
    const u32 w_bottom = open_win(r, kNewWindow);
    CHECK(w_bottom != 0);

    press(r, 489, 130);
    move_to(r, 499, 140);
    release(r, 499, 140);
    CHECK_EQ(r.get16(w_bottom + wd_Width), 200u);

    press(r, 489, 131);
    move_to(r, 499, 141);
    release(r, 499, 141);
    CHECK_EQ(r.get16(w_bottom + wd_Width), 210u);
    CHECK_EQ(r.get16(w_bottom + wd_Height), 110u);
}

void lock_pub_screen_returns_the_default_screen() {
    GfxRig r;
    r.cpu().a[0] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::LockPubScreen), 0u);
    r.cpu().a[0] = r.str(kBuffer, "Workbench");
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::LockPubScreen), 0u);

    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.intuition.set_default_screen(screen);

    r.cpu().a[0] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::LockPubScreen), screen);
    r.cpu().a[0] = r.str(kBuffer, "Workbench");
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::LockPubScreen), screen);
    r.cpu().a[0] = r.str(kBuffer, "WORKBENCH");
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::LockPubScreen), screen);
    r.cpu().a[0] = r.str(kBuffer, "workbench");
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::LockPubScreen), screen);
    r.cpu().a[0] = r.str(kBuffer, "Other");
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::LockPubScreen), 0u);

    r.cpu().a[0] = screen;
    r.cpu().d[0] = 0x12345678;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::UnlockPubScreen), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0x12345678u);
}

void get_default_pub_screen_writes_workbench() {
    GfxRig r;
    r.str(kBuffer, "overwritten");
    r.cpu().a[0] = kBuffer;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::GetDefaultPubScreen), ExitReason::Halt);
    CHECK(r.read_str(kBuffer) == "Workbench");
    CHECK_EQ(r.get8(kBuffer + 9), 0u);
}

void get_screen_draw_info_has_the_default_pens_and_topaz() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);

    r.cpu().a[0] = screen;
    const u32 info = r.result(Base::Intuition, intuition_lvo::GetScreenDrawInfo);
    CHECK(info != 0);
    CHECK_EQ(r.get16(info + dri_NumPens), 12u);
    const u32 pens = r.get32(info + dri_Pens);
    CHECK(pens != 0);
    const u32 expected[12] = {0, 1, 1, 2, 1, 3, 1, 0, 2, 1, 2, 1};
    for (u32 i = 0; i < 12; ++i) {
        CHECK_EQ(r.get16(pens + i * 2), expected[i]);
    }
    CHECK_EQ(r.get16(pens + 12 * 2), 0xFFFFu);
    CHECK_EQ(r.get32(info + dri_Font), r.graphics.topaz_font());

    r.cpu().a[0] = screen;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::GetScreenDrawInfo), info);
    r.cpu().a[0] = kBuffer;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::GetScreenDrawInfo), 0u);
    r.cpu().a[0] = screen;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::FreeScreenDrawInfo), ExitReason::Halt);
}

void set_iprefs_type8_writes_the_palette_into_the_default_screen() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.intuition.set_default_screen(screen);

    const u32 size = put_palette_pref(r, 0, 0x4444, 0x5555, 0x6666);
    set_iprefs(r, kIPrefs, size, 8);
    const u32 map = r.get32(screen + sc_ViewPort + vp_ColorMap);
    CHECK_EQ(colormap_rgb24(r, map, 0), 0x445566u);

    // A screen opened after the default closed takes the same palette.
    r.cpu().a[0] = screen;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::CloseScreen), 1u);
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 again = open_new(r, kNewScreen);
    CHECK(again != 0);
    const u32 next_map = r.get32(again + sc_ViewPort + vp_ColorMap);
    CHECK_EQ(colormap_rgb24(r, next_map, 0), 0x445566u);
}

constexpr u32 kPointerRecord = kData + 0x2600;
constexpr u32 kPointerBitmapA = kData + 0x2640;
constexpr u32 kPointerBitmapB = kData + 0x2680;
constexpr u32 kPointerPlanes = kData + 0x2700;

// A 16x16 BitMap of two planes of 32 bytes at planes: plane 0 has the left
// eight pixels of every row set and plane 1 pixels 4 to 7 and 12 to 15, so a
// row reads 1 1 1 1 3 3 3 3 0 0 0 0 2 2 2 2.
void put_pointer_bitmap(GfxRig& r, u32 bitmap, u32 planes) {
    r.put16(bitmap + bm_BytesPerRow, 2);
    r.put16(bitmap + bm_Rows, 16);
    r.put8(bitmap + bm_Depth, 2);
    r.put32(bitmap + bm_Planes, planes);
    r.put32(bitmap + bm_Planes + 4, planes + 32);
    for (u32 row = 0; row < 16; ++row) {
        r.put16(planes + row * 2, 0xFF00);
        r.put16(planes + 32 + row * 2, 0x0F0F);
    }
}

// SetIPrefs type 7 with the BitMap, the hot spot and which pointer: what it answers.
u32 set_pointer_prefs(GfxRig& r, u32 bitmap, u32 hot_x, u32 hot_y, u32 which) {
    r.put32(kPointerRecord, bitmap);
    r.put16(kPointerRecord + 4, hot_x);
    r.put16(kPointerRecord + 6, hot_y);
    r.put16(kPointerRecord + 8, 1);
    r.put16(kPointerRecord + 10, 5);  // the size and y-size choices IPrefs passes
    r.put16(kPointerRecord + 12, 0);
    r.put16(kPointerRecord + 14, which);
    r.put32(kPointerRecord + 16, 0);
    r.cpu().a[0] = kPointerRecord;
    r.cpu().d[0] = 20;
    r.cpu().d[1] = 7;
    return r.result(Base::Intuition, intuition_lvo::SetIPrefs);
}

void set_iprefs_type7_takes_the_pointer_image_and_hands_back_the_previous_bitmap() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    put_pointer_bitmap(r, kPointerBitmapA, kPointerPlanes);
    put_pointer_bitmap(r, kPointerBitmapB, kPointerPlanes + 0x40);
    CHECK(r.display.pointer == nullptr);
    r.refreshes.clear();

    CHECK_EQ(set_pointer_prefs(r, kPointerBitmapA, 3, 5, 0), 0xFFFFFFFFu);
    const PointerShape* shape = r.display.pointer;
    CHECK(shape != nullptr);
    CHECK(!r.refreshes.empty());
    CHECK_EQ(shape->width, 16);
    CHECK_EQ(shape->height, 16);
    CHECK_EQ(shape->hot_x, 3);
    CHECK_EQ(shape->hot_y, 5);
    const u8 row[16] = {1, 1, 1, 1, 3, 3, 3, 3, 0, 0, 0, 0, 2, 2, 2, 2};
    for (u32 x = 0; x < 16; ++x) {
        CHECK_EQ(shape->pixels[0][x], row[x]);
        CHECK_EQ(shape->pixels[15][x], row[x]);
    }

    // Which 1 is the busy pointer: kept, not shown.
    CHECK_EQ(set_pointer_prefs(r, kPointerBitmapB, 7, 7, 1), 0xFFFFFFFFu);
    CHECK(r.display.pointer == shape);
    CHECK_EQ(shape->hot_x, 3);
    CHECK_EQ(set_pointer_prefs(r, kPointerBitmapA, 7, 7, 1), kPointerBitmapB);

    // A second normal pointer gives back the first BitMap.
    CHECK_EQ(set_pointer_prefs(r, kPointerBitmapB, 0, 1, 0), kPointerBitmapA);
    CHECK_EQ(r.display.pointer->hot_x, 0);
    CHECK_EQ(r.display.pointer->hot_y, 1);
}

void set_iprefs_type7_refuses_a_record_without_an_image() {
    GfxRig r;
    put_pointer_bitmap(r, kPointerBitmapA, kPointerPlanes);
    CHECK_EQ(set_pointer_prefs(r, 0, 0, 0, 0), 0u);
    r.put16(kPointerRecord + 8, 0);
    r.put32(kPointerRecord, kPointerBitmapA);
    r.cpu().a[0] = kPointerRecord;
    r.cpu().d[0] = 20;
    r.cpu().d[1] = 7;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::SetIPrefs), 0u);
    r.put16(kPointerRecord + 8, 1);
    r.put16(kPointerBitmapA + bm_Rows, 0);
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::SetIPrefs), 0u);
    CHECK(r.display.pointer == nullptr);
}

void set_iprefs_type8_pens_8_to_10_are_the_pointer_colours() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.intuition.set_default_screen(screen);
    const u32 map = r.get32(screen + sc_ViewPort + vp_ColorMap);
    put_pointer_bitmap(r, kPointerBitmapA, kPointerPlanes);
    CHECK_EQ(set_pointer_prefs(r, kPointerBitmapA, 0, 0, 0), 0xFFFFFFFFu);
    const PointerShape* shape = r.display.pointer;
    const u32 pen8 = colormap_rgb24(r, map, 8);
    const u32 pen9 = colormap_rgb24(r, map, 9);
    const u32 pen10 = colormap_rgb24(r, map, 10);

    r.put16(kIPrefs, 8);
    r.put16(kIPrefs + 2, 0x1111);
    r.put16(kIPrefs + 4, 0x2222);
    r.put16(kIPrefs + 6, 0x3333);
    r.put16(kIPrefs + 8, 9);
    r.put16(kIPrefs + 10, 0x4444);
    r.put16(kIPrefs + 12, 0x5555);
    r.put16(kIPrefs + 14, 0x6666);
    r.put16(kIPrefs + 16, 10);
    r.put16(kIPrefs + 18, 0x7777);
    r.put16(kIPrefs + 20, 0x8888);
    r.put16(kIPrefs + 22, 0x9999);
    r.put16(kIPrefs + 24, 0xFFFF);
    r.refreshes.clear();
    set_iprefs(r, kIPrefs, 24, 8);
    CHECK_EQ(shape->rgb[1], 0x112233u);
    CHECK_EQ(shape->rgb[2], 0x445566u);
    CHECK_EQ(shape->rgb[3], 0x778899u);
    CHECK(!r.refreshes.empty());
    CHECK_EQ(colormap_rgb24(r, map, 8), pen8);
    CHECK_EQ(colormap_rgb24(r, map, 9), pen9);
    CHECK_EQ(colormap_rgb24(r, map, 10), pen10);

    // The colours go to a pointer that arrives later, and pens 0 to 7 stay screen pens.
    const u32 size = put_palette_pref(r, 1, 0xAAAA, 0xBBBB, 0xCCCC);
    set_iprefs(r, kIPrefs, size, 8);
    CHECK_EQ(colormap_rgb24(r, map, 1), 0xAABBCCu);
    CHECK_EQ(shape->rgb[1], 0x112233u);
    put_pointer_bitmap(r, kPointerBitmapB, kPointerPlanes + 0x40);
    CHECK_EQ(set_pointer_prefs(r, kPointerBitmapB, 0, 0, 0), kPointerBitmapA);
    CHECK_EQ(r.display.pointer->rgb[2], 0x445566u);
}

void set_iprefs_type9_and_apply_pen_prefs_rewrite_the_draw_info() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 first = open_new(r, kNewScreen);
    CHECK(first != 0);
    r.intuition.set_default_screen(first);

    const u32 size = put_pen_prefs(r, 1, 5);
    set_iprefs(r, kIPrefs, size, 9);

    // A fresh screen made default takes kind 1's pens.
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.intuition.set_default_screen(screen);
    r.cpu().a[0] = screen;
    const u32 info = r.result(Base::Intuition, intuition_lvo::GetScreenDrawInfo);
    CHECK(info != 0);
    const u32 pens = r.get32(info + dri_Pens);
    CHECK_EQ(r.get16(pens + 10 * 2), 5u);

    // The same set rewrites a DrawInfo that already exists.
    put_pen_prefs(r, 1, 7);
    set_iprefs(r, kIPrefs, size, 9);
    r.intuition.apply_pen_prefs(screen);
    CHECK_EQ(r.get16(pens + 10 * 2), 7u);

    // PEN_C3 and PEN_C0 name the palette's colours 4 and 7, kept in pens 4
    // and 7.
    put_pen_prefs(r, 1, 0xFEFC);
    set_iprefs(r, kIPrefs, size, 9);
    r.intuition.apply_pen_prefs(screen);
    CHECK_EQ(r.get16(pens + 10 * 2), 4u);
    put_pen_prefs(r, 1, 0xFEFF);
    set_iprefs(r, kIPrefs, size, 9);
    r.intuition.apply_pen_prefs(screen);
    CHECK_EQ(r.get16(pens + 10 * 2), 7u);
}

// SetIPrefs type 2 opens the font it names and keeps it: type 0 as the
// default font InitRastPort gives a RastPort, type 1 as the screen font.
// The TextAttr is copied out of IPrefs' stack, and a font the screen font
// replaces is closed.
void set_iprefs_type2_sets_the_system_and_screen_fonts() {
    GfxRig r;
    const u32 topaz = r.graphics.topaz_font();
    const u32 hasklig13 = font_address(r, 13);
    const u32 hasklig11 = font_address(r, 11);
    CHECK(hasklig13 != hasklig11);

    const u32 size = put_font_prefs(r, kIPrefs, "hasklig.font", 13, 0);
    set_iprefs(r, kIPrefs, size, 2);
    // Whatever IPrefs' stack held is gone, the font is not.
    for (u32 i = 0; i < size; ++i) {
        r.put8(kIPrefs + i, 0xAA);
    }
    CHECK_EQ(r.graphics.default_font(), hasklig13);
    CHECK_EQ(r.get32(r.graphics.graphics_base() + 154), hasklig13);
    CHECK_EQ(r.get16(hasklig13 + tf_Accessors), 1u);  // the default's
    CHECK_EQ(r.intuition.screen_font(), topaz);
    CHECK_EQ(r.intuition.screen_font_attr(), r.graphics.topaz_attr());

    // Type 1 moves only the screen font.
    put_font_prefs(r, kIPrefs, "hasklig.font", 13, 1);
    set_iprefs(r, kIPrefs, size, 2);
    for (u32 i = 0; i < size; ++i) {
        r.put8(kIPrefs + i, 0xAA);
    }
    CHECK_EQ(r.graphics.default_font(), hasklig13);
    CHECK_EQ(r.intuition.screen_font(), hasklig13);
    CHECK_EQ(r.get16(hasklig13 + tf_Accessors), 2u);  // the default and the screen's
    const u32 attr = r.intuition.screen_font_attr();
    CHECK(attr != 0);
    CHECK(r.read_str(r.get32(attr + ta_Name)) == "hasklig.font");
    CHECK_EQ(r.get16(attr + ta_YSize), 13u);

    // Another screen font closes the one it replaces, and the same one
    // again leaves the accessor where it was.
    put_font_prefs(r, kIPrefs, "hasklig.font", 11, 1);
    set_iprefs(r, kIPrefs, size, 2);
    CHECK_EQ(r.intuition.screen_font(), hasklig11);
    CHECK_EQ(r.get16(hasklig13 + tf_Accessors), 1u);
    CHECK_EQ(r.get16(hasklig11 + tf_Accessors), 1u);
    CHECK_EQ(r.get16(r.intuition.screen_font_attr() + ta_YSize), 11u);
    set_iprefs(r, kIPrefs, size, 2);
    CHECK_EQ(r.intuition.screen_font(), hasklig11);
    CHECK_EQ(r.get16(hasklig11 + tf_Accessors), 1u);
}

void close_and_open_workbench_follow_the_windows_and_the_pens() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.intuition.set_default_screen(screen);

    // A window that is not the Workbench's keeps CloseWorkBench from
    // resetting the screen.
    put_new_window(r, kNewWindow, 80, 60, 200, 100, 0, 0, screen, CUSTOMSCREEN);
    const u32 plain = open_win(r, kNewWindow);
    CHECK(plain != 0);
    r.cpu().a[0] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::CloseWorkBench), 0u);
    r.cpu().a[0] = plain;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::CloseWindow), ExitReason::Halt);

    put_new_window(r, kNewWindow, 80, 60, 200, 100, WFLG_WBENCHWINDOW, 0, screen,
                   CUSTOMSCREEN);
    const u32 workbench = open_win(r, kNewWindow);
    CHECK(workbench != 0);
    CHECK_EQ(r.get32(workbench + wd_Flags) & WFLG_WBENCHWINDOW, WFLG_WBENCHWINDOW);

    WorkbenchHookState hooks;
    r.intuition.set_workbench_hooks(
        Intuition::WorkbenchHooks{close_workbench_hook, open_workbench_hook, &hooks});
    r.cpu().a[0] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::CloseWorkBench), 1u);
    CHECK_EQ(hooks.closes, 1u);
    CHECK(logged("intuition: Workbench closed"));

    const u32 size = put_pen_prefs(r, 1, 5);
    set_iprefs(r, kIPrefs, size, 9);
    r.cpu().a[0] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::OpenWorkBench), screen);
    CHECK_EQ(hooks.opens, 1u);
    CHECK(logged("intuition: Workbench opened"));

    r.cpu().a[0] = screen;
    const u32 info = r.result(Base::Intuition, intuition_lvo::GetScreenDrawInfo);
    CHECK(info != 0);
    const u32 pens = r.get32(info + dri_Pens);
    CHECK_EQ(r.get16(pens + 10 * 2), 5u);

    // A second OpenWorkBench without a close resets the screen but opens
    // no Workbench windows.
    r.cpu().a[0] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::OpenWorkBench), screen);
    CHECK_EQ(hooks.opens, 1u);
}

constexpr u32 kModeHighRes = 0x8000;
constexpr u32 kModeEhbHighRes = 0x8080;
constexpr u32 kModeNtscHiresLace = 0x19004;
constexpr u32 kModePalHiresLace = 0x29004;
constexpr u32 kWindowGadgets = WFLG_SIZEGADGET | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                               WFLG_DRAGBAR;

// A struct ScreenModePrefs, as IPrefs sends it in SetIPrefs type 1.
void send_screen_mode(GfxRig& r, u32 mode_id, u32 width, u32 height, u32 depth,
                      u32 control = 3) {
    r.put32(kIPrefs, mode_id);
    r.put16(kIPrefs + 4, width);
    r.put16(kIPrefs + 6, height);
    r.put16(kIPrefs + 8, depth);
    r.put16(kIPrefs + 10, control);
    set_iprefs(r, kIPrefs, 12, 1);
}

// IPrefs' reset of the Workbench: CloseWorkBench, which has to succeed, and
// OpenWorkBench.
u32 reset_workbench(GfxRig& r) {
    r.cpu().a[0] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::CloseWorkBench), 1u);
    r.cpu().a[0] = 0;
    return r.result(Base::Intuition, intuition_lvo::OpenWorkBench);
}

u32 window_bitmap(GfxRig& r, u32 window) {
    return r.get32(r.get32(window + wd_RPort) + rp_BitMap);
}

// The highest pen drawn anywhere on the screen's BitMap.
u32 highest_pen_on_screen(GfxRig& r, u32 screen) {
    native::Bus bus(r.machine);
    raster::Bitmap bitmap{};
    CHECK(raster::load_bitmap(bus, screen + sc_BitMap, bitmap));
    u32 highest = 0;
    for (u32 y = 0; y < bitmap.rows; ++y) {
        for (u32 x = 0; x < bitmap.bytes_per_row * 8; ++x) {
            const u32 pen =
                raster::read_pixel(bus, bitmap, static_cast<i32>(x), static_cast<i32>(y));
            highest = pen > highest ? pen : highest;
        }
    }
    return highest;
}

struct SizeHookState {
    GfxRig* rig = nullptr;
    u32 calls = 0;
    u32 width = 0;
    u32 height = 0;
};

void record_window_size(void* ctx, u32 window) {
    SizeHookState& state = *static_cast<SizeHookState*>(ctx);
    ++state.calls;
    state.width = state.rig->get16(window + wd_Width);
    state.height = state.rig->get16(window + wd_Height);
}

void set_iprefs_type_1_keeps_the_screen_mode_record() {
    GfxRig r;
    CHECK(!IntuitionTest::screen_mode(r.intuition).set);
    send_screen_mode(r, kModePalHiresLace, 0xFFFF, 0xFFFF, 4, 3);
    const auto& mode = IntuitionTest::screen_mode(r.intuition);
    CHECK(mode.set);
    CHECK_EQ(mode.mode_id, kModePalHiresLace);
    CHECK_EQ(mode.width, 0xFFFFu);
    CHECK_EQ(mode.height, 0xFFFFu);
    CHECK_EQ(mode.depth, 4u);
    CHECK_EQ(mode.control, 3u);

    // A record of fewer than 12 bytes is not taken; a later one replaces it.
    r.put32(kIPrefs, kModeHiresLace);
    set_iprefs(r, kIPrefs, 11, 1);
    CHECK_EQ(mode.mode_id, kModePalHiresLace);
    send_screen_mode(r, kModeHiresLace, 640, 400, 8, 1);
    CHECK_EQ(mode.mode_id, kModeHiresLace);
    CHECK_EQ(mode.width, 640u);
    CHECK_EQ(mode.height, 400u);
    CHECK_EQ(mode.depth, 8u);
    CHECK_EQ(mode.control, 1u);
}

void close_workbench_does_not_wait_for_a_window_marked_to_stay() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.intuition.set_default_screen(screen);
    put_new_window(r, kNewWindow, 80, 60, 200, 100, 0, 0, screen, CUSTOMSCREEN);
    const u32 marked = open_win(r, kNewWindow);
    CHECK(marked != 0);
    r.cpu().a[0] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::CloseWorkBench), 0u);

    r.intuition.keep_through_workbench_reset(0);
    r.intuition.keep_through_workbench_reset(0x1234);
    r.intuition.keep_through_workbench_reset(marked);
    r.cpu().a[0] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::CloseWorkBench), 1u);
    CHECK(r.intuition.window_open(marked));

    // An ordinary window still keeps CloseWorkBench from succeeding.
    put_new_window(r, kNewWindow, 300, 60, 200, 100, 0, 0, screen, CUSTOMSCREEN);
    CHECK(open_win(r, kNewWindow) != 0);
    r.cpu().a[0] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::CloseWorkBench), 0u);
}

void open_workbench_rebuilds_the_screen_for_the_mode_of_the_prefs() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 512, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.intuition.set_default_screen(screen);
    put_new_window(r, kNewWindow, 20, 30, 600, 450, kWindowGadgets, 0, screen, CUSTOMSCREEN);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    r.intuition.keep_through_workbench_reset(window);
    r.cpu().a[0] = screen;
    const u32 info = r.result(Base::Intuition, intuition_lvo::GetScreenDrawInfo);
    CHECK_EQ(r.get16(info + dri_Depth), 8u);
    const u32 old_planes = r.get32(screen + sc_BitMap + bm_Planes);
    const u32 old_window_planes = r.get32(window_bitmap(r, window) + bm_Planes);

    send_screen_mode(r, kModePalHiresLace, 0xFFFF, 0xFFFF, 4);
    CHECK_EQ(reset_workbench(r), screen);

    const u32 vp = screen + sc_ViewPort;
    CHECK_EQ(r.get16(screen + sc_Width), 640u);
    CHECK_EQ(r.get16(screen + sc_Height), 512u);
    CHECK_EQ(r.get8(screen + sc_BitMap + bm_Depth), 4u);
    CHECK_EQ(r.get16(screen + sc_BitMap + bm_BytesPerRow), 80u);
    CHECK_EQ(r.get16(screen + sc_BitMap + bm_Rows), 512u);
    CHECK(r.get32(screen + sc_BitMap + bm_Planes) != old_planes);
    CHECK_EQ(r.get32(screen + sc_BitMap + bm_Planes + 4 * 4), 0u);
    CHECK_EQ(r.get16(vp + vp_Modes), kModeHiresLace);
    CHECK_EQ(r.get16(vp + vp_DWidth), 640u);
    CHECK_EQ(r.get16(vp + vp_DHeight), 512u);
    CHECK_EQ(vp_mode_id(r, screen), kModePalHiresLace);
    CHECK_EQ(r.intuition.screen_mode_id(screen), kModePalHiresLace);
    CHECK_EQ(r.get16(r.get32(vp + vp_ColorMap) + cm_Count), 16u);
    CHECK_EQ(r.get16(info + dri_Depth), 4u);
    CHECK_EQ(r.display.mode_width, 640u);
    CHECK_EQ(r.display.mode_height, 512u);
    CHECK_EQ(r.display.mode_flags, display_modes::find(kModePalHiresLace)->flags);

    // The window stays, on planes of the new depth, inside the screen.
    CHECK(r.intuition.window_open(window));
    CHECK_EQ(r.get16(window + wd_LeftEdge), 20u);
    CHECK_EQ(r.get16(window + wd_TopEdge), 30u);
    CHECK_EQ(r.get16(window + wd_Width), 600u);
    CHECK_EQ(r.get16(window + wd_Height), 450u);
    CHECK_EQ(r.get8(window_bitmap(r, window) + bm_Depth), 4u);
    CHECK(r.get32(window_bitmap(r, window) + bm_Planes) != old_window_planes);

    // The screen and the window are drawn again in the classic pens: the
    // window's raised edge starts in SHINEPEN.
    CHECK_EQ(screen_pixel(r, screen, 20, 30), 2u);
    CHECK_EQ(screen_pixel(r, screen, 400, 14), 1u);
    CHECK(highest_pen_on_screen(r, screen) <= 3u);
    CHECK(logged("intuition: Workbench screen 640x512x4"));
}

void open_workbench_fits_its_windows_to_a_smaller_screen() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.intuition.set_default_screen(screen);
    put_new_window(r, kNewWindow, 20, 30, 600, 350, kWindowGadgets, 0, screen, CUSTOMSCREEN);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    r.intuition.keep_through_workbench_reset(window);
    SizeHookState hook;
    hook.rig = &r;
    r.intuition.set_size_hook(window, record_window_size, &hook);

    send_screen_mode(r, kModeHighRes, 0xFFFF, 0xFFFF, 2);
    CHECK_EQ(reset_workbench(r), screen);

    CHECK_EQ(r.get16(screen + sc_Width), 640u);
    CHECK_EQ(r.get16(screen + sc_Height), 200u);
    CHECK_EQ(r.get8(screen + sc_BitMap + bm_Depth), 2u);
    CHECK_EQ(r.get16(screen + sc_ViewPort + vp_Modes), kModeHighRes);
    CHECK_EQ(r.display.mode_height, 200u);

    // Moved up and shrunk to the screen's 200 rows; its width fits as it was.
    CHECK(r.intuition.window_open(window));
    CHECK_EQ(r.get16(window + wd_LeftEdge), 20u);
    CHECK_EQ(r.get16(window + wd_TopEdge), 0u);
    CHECK_EQ(r.get16(window + wd_Width), 600u);
    CHECK_EQ(r.get16(window + wd_Height), 200u);
    CHECK_EQ(r.get16(window + wd_MinHeight), 200u);
    CHECK_EQ(r.get16(window + wd_MaxHeight), 200u);
    CHECK_EQ(r.get16(window_bitmap(r, window) + bm_Rows), 200u);
    CHECK_EQ(r.get8(window_bitmap(r, window) + bm_Depth), 2u);

    // The window's own layout ran once, for the size it has now.
    CHECK_EQ(hook.calls, 1u);
    CHECK_EQ(hook.width, 600u);
    CHECK_EQ(hook.height, 200u);
}

// A size hook that closes another window, as a program's own layout code
// might.
struct ClosingHook {
    Intuition* intuition = nullptr;
    u32 other = 0;
};

void close_other_window(void* ctx, u32) {
    ClosingHook& hook = *static_cast<ClosingHook*>(ctx);
    if (hook.other != 0) {
        hook.intuition->close_window(hook.other);
        hook.other = 0;
    }
}

void open_workbench_lets_a_size_hook_close_a_window() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.intuition.set_default_screen(screen);
    put_new_window(r, kNewWindow, 20, 30, 300, 200, kWindowGadgets, 0, screen, CUSTOMSCREEN);
    const u32 first = open_win(r, kNewWindow);
    put_new_window(r, kNewWindow, 40, 50, 300, 200, kWindowGadgets, 0, screen, CUSTOMSCREEN);
    const u32 second = open_win(r, kNewWindow);
    CHECK(first != 0 && second != 0);
    r.intuition.keep_through_workbench_reset(first);
    r.intuition.keep_through_workbench_reset(second);
    ClosingHook hook{&r.intuition, second};
    r.intuition.set_size_hook(first, close_other_window, &hook);
    const u32 free_before = r.exec.free_bytes();
    const u32 exec_base = r.get32(4);

    send_screen_mode(r, kModeHighRes, 0xFFFF, 0xFFFF, 2);
    CHECK_EQ(reset_workbench(r), screen);

    CHECK(r.intuition.window_open(first));
    CHECK(!r.intuition.window_open(second));
    CHECK_EQ(r.get8(window_bitmap(r, first) + bm_Depth), 2u);
    // Nothing was written for the closed window, and the planes made for it
    // went back.
    CHECK_EQ(r.get32(4), exec_base);
    CHECK(r.exec.free_bytes() > free_before);
}

void open_workbench_keeps_a_screen_that_has_the_mode_of_the_prefs() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.intuition.set_default_screen(screen);
    put_new_window(r, kNewWindow, 20, 30, 300, 200, 0, 0, screen, CUSTOMSCREEN);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    r.intuition.keep_through_workbench_reset(window);
    const u32 map = r.get32(screen + sc_ViewPort + vp_ColorMap);
    const u32 backdrop = r.get32(r.intuition.backdrop_rastport(screen) + rp_BitMap);
    const u32 planes = r.get32(screen + sc_BitMap + bm_Planes);
    const u32 backdrop_planes = r.get32(backdrop + bm_Planes);
    const u32 colors = r.get32(map + cm_ColorTable);
    const u32 window_planes = r.get32(window_bitmap(r, window) + bm_Planes);
    const auto unchanged = [&] {
        CHECK_EQ(r.get32(screen + sc_BitMap + bm_Planes), planes);
        CHECK_EQ(r.get32(backdrop + bm_Planes), backdrop_planes);
        CHECK_EQ(r.get32(map + cm_ColorTable), colors);
        CHECK_EQ(r.get32(window_bitmap(r, window) + bm_Planes), window_planes);
        CHECK_EQ(r.get16(screen + sc_Height), 400u);
        CHECK_EQ(r.get8(screen + sc_BitMap + bm_Depth), 8u);
        CHECK_EQ(r.intuition.screen_mode_id(screen), kModeHiresLace);
    };

    // The default monitor and NTSC are one here, and 0xFFFF is the mode's size.
    send_screen_mode(r, kModeNtscHiresLace, 0xFFFF, 0xFFFF, 8, 1);
    CHECK_EQ(reset_workbench(r), screen);
    unchanged();

    send_screen_mode(r, kModeHiresLace, 640, 400, 8);
    CHECK_EQ(reset_workbench(r), screen);
    unchanged();
}

void open_workbench_keeps_the_screen_for_a_mode_it_cannot_have() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.intuition.set_default_screen(screen);
    const u32 planes = r.get32(screen + sc_BitMap + bm_Planes);
    const auto unchanged = [&] {
        CHECK_EQ(r.get32(screen + sc_BitMap + bm_Planes), planes);
        CHECK_EQ(r.get16(screen + sc_Width), 640u);
        CHECK_EQ(r.get16(screen + sc_Height), 400u);
        CHECK_EQ(r.get8(screen + sc_BitMap + bm_Depth), 8u);
    };

    send_screen_mode(r, kModeUnknown, 0xFFFF, 0xFFFF, 8);
    CHECK_EQ(reset_workbench(r), screen);
    CHECK(logged("intuition: Workbench screen mode $31004 not usable"));
    unchanged();

    // Seven planes are more than Extra Half-Brite has, and no planes at all
    // make no screen.
    send_screen_mode(r, kModeEhbHighRes, 0xFFFF, 0xFFFF, 7);
    CHECK_EQ(reset_workbench(r), screen);
    CHECK(logged("intuition: Workbench screen mode $8080 not usable"));
    unchanged();

    send_screen_mode(r, kModeHiresLace, 0xFFFF, 0xFFFF, 0);
    CHECK_EQ(reset_workbench(r), screen);
    CHECK(logged("intuition: Workbench screen mode $8004 not usable"));
    unchanged();
}

void open_workbench_keeps_the_screen_when_memory_runs_out() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.intuition.set_default_screen(screen);
    put_new_window(r, kNewWindow, 20, 30, 300, 200, 0, 0, screen, CUSTOMSCREEN);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    r.intuition.keep_through_workbench_reset(window);
    const u32 planes = r.get32(screen + sc_BitMap + bm_Planes);
    const u32 window_planes = r.get32(window_bitmap(r, window) + bm_Planes);

    // Room for small blocks stays, none for the planes of a larger screen.
    std::vector<u32> blocks;
    for (;;) {
        const u32 at = r.exec.alloc(4096, MEMF_PUBLIC);
        if (at == 0) {
            break;
        }
        blocks.push_back(at);
    }
    CHECK(blocks.size() > 8);
    for (u32 i = 0; i < 8; ++i) {
        r.exec.free(blocks[i], 4096);
    }
    send_screen_mode(r, kModePalHiresLace, 0xFFFF, 0xFFFF, 8);
    CHECK_EQ(reset_workbench(r), screen);
    CHECK(logged("intuition: no memory for the Workbench screen mode"));
    CHECK_EQ(r.get16(screen + sc_Height), 400u);
    CHECK_EQ(r.get32(screen + sc_BitMap + bm_Planes), planes);
    CHECK_EQ(r.get32(window_bitmap(r, window) + bm_Planes), window_planes);
    CHECK_EQ(r.intuition.screen_mode_id(screen), kModeHiresLace);
    for (u32 i = 8; i < blocks.size(); ++i) {
        r.exec.free(blocks[i], 4096);
    }
}

// The window chrome, the title bars and the gadgets of screens of one and
// four planes use the DrawInfo's pens, 0 to 3 by default, which a screen of
// one plane shows as 0 and 1; nothing is drawn in a pen above 3.
void shallow_screens_draw_the_chrome_in_the_classic_pens() {
    for (const u32 depth : {1u, 4u}) {
        GfxRig r;
        put_new_screen(r, kNewScreen, 640, 400, depth, 0, 1, 0);
        const u32 screen = open_new(r, kNewScreen);
        CHECK(screen != 0);
        CHECK_EQ(r.get16(r.get32(screen + sc_ViewPort + vp_ColorMap) + cm_Count), 1u << depth);

        put_prop_info(r, kProp1, AUTOKNOB | FREEVERT, 0, 0, MAXBODY, MAXBODY / 4);
        put_gadget(r, kGad1, 0, 20, 20, 12, 40, 0, 0, GTYP_PROPGADGET, kProp1);
        r.str(kWinTitle, "H");
        put_new_window(r, kNewWindow, 20, 20, 200, 100,
                       WFLG_ACTIVATE | WFLG_CLOSEGADGET | WFLG_DEPTHGADGET, kWinTitle, 0, 0);
        r.put32(kNewWindow + nw_FirstGadget, kGad1);
        CHECK(open_win(r, kNewWindow) != 0);

        const u32 mask = (1u << depth) - 1;
        CHECK_EQ(screen_pixel(r, screen, 20, 20), 2u & mask);
        CHECK_EQ(screen_pixel(r, screen, 100, 25), 3u & mask);
        // The close glyph's dark and light edges, the face round it and
        // the light line of the groove that ends its cell.
        CHECK_EQ(screen_pixel(r, screen, 24, 27), 1u & mask);
        CHECK_EQ(screen_pixel(r, screen, 30, 27), 2u & mask);
        CHECK_EQ(screen_pixel(r, screen, 22, 27), 3u & mask);
        CHECK_EQ(screen_pixel(r, screen, 36, 27), 2u & mask);
        CHECK(highest_pen_on_screen(r, screen) <= 3u);
    }
}

void chrome_palette_follows_the_default_screen_pens() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.intuition.set_default_screen(screen);
    const u32 map = r.get32(screen + sc_ViewPort + vp_ColorMap);

    // Pen 2 is the default BARBLOCKPEN, the menu paper.
    u32 size = put_palette_pref(r, 2, 0xFFFF, 0, 0);
    set_iprefs(r, kIPrefs, size, 8);
    CHECK_EQ(colormap_rgb24(r, map, chrome::kMenuPaperPen), 0xFF0000u);
    CHECK_EQ(colormap_rgb24(r, map, chrome::kScreenBar3), chrome::screen_bar_rgb(0xFF0000, 3));

    // Pen 3 is the default FILLPEN, the active title bar.
    size = put_palette_pref(r, 3, 0, 0, 0xFFFF);
    set_iprefs(r, kIPrefs, size, 8);
    CHECK_EQ(colormap_rgb24(r, map, chrome::kActiveBar0), 0x4D4DFFu);
    CHECK_EQ(colormap_rgb24(r, map, chrome::kActiveBar7), 0x0000B3u);
}

// The top four pens of a screen of 16 colours or more keep the colours of
// 0..3, so a complemented icon of four colours looks as on two planes.
void complement_mirrors_follow_the_low_pens() {
    for (const u32 depth : {4u, 5u, 6u, 7u, 8u}) {
        GfxRig r;
        put_new_screen(r, kNewScreen, 640, 400, depth, 0, 1, 0);
        const u32 screen = open_new(r, kNewScreen);
        CHECK(screen != 0);
        r.intuition.set_default_screen(screen);
        const u32 map = r.get32(screen + sc_ViewPort + vp_ColorMap);
        const u32 first = (1u << depth) - chrome::kComplementMirrorPens;

        const auto mirrors_match = [&] {
            for (u32 pen = 0; pen < chrome::kComplementMirrorPens; ++pen) {
                CHECK_EQ(colormap_rgb24(r, map, first + pen), colormap_rgb24(r, map, pen));
            }
        };
        mirrors_match();

        const u32 colours[chrome::kComplementMirrorPens] = {0x112233, 0x445566, 0x778899,
                                                            0xAABBCC};
        for (u32 pen = 0; pen < chrome::kComplementMirrorPens; ++pen) {
            const u32 size = put_palette_pref(r, pen, (colours[pen] >> 8) & 0xFF00,
                                             colours[pen] & 0xFF00,
                                             (colours[pen] << 8) & 0xFF00);
            set_iprefs(r, kIPrefs, size, 8);
            mirrors_match();
        }
    }
}

void menu_strip_vectors_write_the_window_field() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);

    r.cpu().a[0] = window;
    r.cpu().a[1] = kMenu;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::SetMenuStrip), 1u);
    CHECK_EQ(r.get32(window + 28), kMenu);
    r.cpu().a[0] = window;
    r.cpu().a[1] = kMenu2;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ResetMenuStrip), 1u);
    CHECK_EQ(r.get32(window + 28), kMenu2);

    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::ClearMenuStrip), ExitReason::Halt);
    CHECK_EQ(r.get32(window + 28), 0u);

    r.cpu().a[0] = 0;
    r.cpu().a[1] = kMenu;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::SetMenuStrip), 0u);
}

void item_address_follows_the_menu_numbers() {
    GfxRig r;
    r.put32(kMenu + mu_NextMenu, kMenu2);
    r.put32(kMenu + mu_FirstItem, kItem0);
    r.put32(kMenu2 + mu_NextMenu, 0);
    r.put32(kMenu2 + mu_FirstItem, kItem2);
    r.put32(kItem0 + mi_NextItem, kItem1);
    r.put32(kItem0 + mi_SubItem, kSub0);
    r.put32(kItem1 + mi_NextItem, 0);
    r.put32(kItem1 + mi_SubItem, 0);
    r.put32(kItem2 + mi_NextItem, 0);
    r.put32(kItem2 + mi_SubItem, 0);
    r.put32(kSub0 + mi_NextItem, kSub1);
    r.put32(kSub0 + mi_SubItem, 0);
    r.put32(kSub1 + mi_NextItem, 0);
    r.put32(kSub1 + mi_SubItem, 0);

    r.cpu().a[0] = kMenu;
    r.cpu().d[0] = 0xFFFF;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ItemAddress), 0u);
    r.cpu().d[0] = 0xF800;  // menu 0, item 0, no subitem
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ItemAddress), kItem0);
    r.cpu().d[0] = 0xF820;  // menu 0, item 1
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ItemAddress), kItem1);
    r.cpu().d[0] = 0xF801;  // menu 1, item 0
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ItemAddress), kItem2);
    r.cpu().d[0] = 0x0000;  // menu 0, item 0, subitem 0
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ItemAddress), kSub0);
    r.cpu().d[0] = 0x0800;  // menu 0, item 0, subitem 1
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ItemAddress), kSub1);
    r.cpu().d[0] = 0xF802;  // menu 2 does not exist
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ItemAddress), 0u);
    r.cpu().d[0] = 0xF8A0;  // item 5 does not exist
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ItemAddress), 0u);
    r.cpu().d[0] = 0x2800;  // subitem 5 does not exist
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ItemAddress), 0u);

    r.cpu().a[0] = 0;
    r.cpu().d[0] = 0xF800;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ItemAddress), 0u);
}

void put_menu(GfxRig& r, u32 at, u32 name, i32 left, u32 width, u16 flags, u32 first,
              u32 next) {
    r.put32(at + mu_NextMenu, next);
    r.put16(at + mu_LeftEdge, static_cast<u32>(left));
    r.put16(at + mu_TopEdge, 0);
    r.put16(at + mu_Width, width);
    r.put16(at + mu_Height, 9);
    r.put16(at + mu_Flags, flags);
    r.put32(at + mu_MenuName, name);
    r.put32(at + mu_FirstItem, first);
}

void put_item(GfxRig& r, u32 at, u32 fill, u32 text, i32 left, i32 top, i32 width, i32 height,
              u16 flags, u32 next, u32 sub, u32 mutual, u8 command) {
    if (fill != 0) {
        r.put16(fill + 4, 2);
        r.put16(fill + 6, 1);
        r.put32(fill + it_IText, text);
    }
    r.put32(at + mi_NextItem, next);
    r.put16(at + mi_LeftEdge, static_cast<u32>(left));
    r.put16(at + mi_TopEdge, static_cast<u32>(top));
    r.put16(at + mi_Width, static_cast<u32>(width));
    r.put16(at + mi_Height, static_cast<u32>(height));
    r.put16(at + mi_Flags, flags);
    r.put32(at + mi_MutualExclude, mutual);
    r.put32(at + mi_ItemFill, fill);
    r.put8(at + mi_Command, command);
    r.put32(at + mi_SubItem, sub);
    r.put16(at + mi_NextSelect, 0);
}

void put_two_title_strip(GfxRig& r) {
    r.str(kMenuFile, "File");
    r.str(kMenuEdit, "Edit");
    r.str(kTextA0, "Open");
    r.str(kTextA1, "Save");
    r.str(kTextA2, "Quit");
    r.str(kTextB0, "Paste");
    r.str(kTextB1, "Copy");
    r.str(kTextSub0, "One");
    r.str(kTextSub1, "Two");
    put_menu(r, kMenuA, kMenuFile, 0, 48, MENUENABLED, kItemA0, kMenuB);
    put_menu(r, kMenuB, kMenuEdit, 48, 48, MENUENABLED, kItemB0, 0);
    const u16 flags = ITEMTEXT | ITEMENABLED | HIGHCOMP;
    put_item(r, kItemA0, kFillA0, kTextA0, 0, 0, 68, 9, flags, kItemA1, 0, 0, 0);
    put_item(r, kItemA1, kFillA1, kTextA1, 0, 9, 68, 9, flags, kItemA2, 0, 0, 0);
    put_item(r, kItemA2, kFillA2, kTextA2, 0, 18, 68, 9, flags, 0, 0, 0, 0);
    put_item(r, kItemB0, kFillB0, kTextB0, 0, 0, 60, 9, flags, kItemB1, 0, 0, 0);
    put_item(r, kItemB1, kFillB1, kTextB1, 0, 9, 60, 9, flags, 0, 0, 0, 0);
}

u32 open_menu_window(GfxRig& r, u32 flags, u32 idcmp) {
    put_new_window(r, kNewWindow, 20, 30, 120, 80, flags, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, idcmp);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    return window;
}

void set_strip(GfxRig& r, u32 window, u32 strip) {
    r.cpu().a[0] = window;
    r.cpu().a[1] = strip;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::SetMenuStrip), 1u);
}

void menu_press(GfxRig& r, i32 x, i32 y) {
    r.intuition.pointer_event(x, y, kMouseRight, kMouseRight, 0, false);
}

void menu_move(GfxRig& r, i32 x, i32 y) {
    r.intuition.pointer_event(x, y, 0, 0, 0, true);
}

void menu_release(GfxRig& r, i32 x, i32 y) {
    r.intuition.pointer_event(x, y, 0, 0, kMouseRight, false);
}

u32 window_message(GfxRig& r, u32 window) {
    return get_msg(r, r.get32(window + wd_UserPort));
}

// A screen opened in a font of its own is laid out by it: the bar is
// WBorTop and the font high, sc_Font, the RastPort and the DrawInfo name
// the font, and a window on it gets a title bar of WBorTop + YSize + 1
// with buttons that fill it.
void screen_with_sa_font_sizes_its_bars_by_the_font() {
    GfxRig r;
    const u32 hasklig13 = font_address(r, 13);
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 tags = r.tags(kTagList, {{SA_Font, kBuffer}, {TAG_DONE, 0}});
    const u32 screen = open_tags(r, kNewScreen, tags);
    CHECK(screen != 0);
    CHECK_EQ(r.get8(screen + sc_WBorTop), 6u);
    CHECK_EQ(r.get8(screen + sc_BarHeight), 19u);
    const u32 attr = r.get32(screen + sc_Font);
    CHECK(attr != 0 && attr != kBuffer);
    CHECK(r.read_str(r.get32(attr + ta_Name)) == "hasklig.font");
    CHECK_EQ(r.get16(attr + ta_YSize), 13u);
    CHECK_EQ(r.get32(screen + sc_RastPort + rp_Font), hasklig13);
    CHECK_EQ(r.get16(screen + sc_RastPort + rp_TxHeight), 13u);
    CHECK_EQ(r.intuition.font_of_screen(screen), hasklig13);
    CHECK_EQ(r.get32(r.intuition.screen_draw_info(screen) + dri_Font), hasklig13);
    CHECK_EQ(r.get16(hasklig13 + tf_Accessors), 1u);
    CHECK_EQ(screen_pixel(r, screen, 300, 19), static_cast<u32>(chrome::kMenuLabelPen));
    CHECK(screen_pixel(r, screen, 300, 18) != static_cast<u32>(chrome::kMenuLabelPen));

    put_new_window(r, kNewWindow, 40, 60, 200, 100, WFLG_CLOSEGADGET | WFLG_DEPTHGADGET, 0,
                   screen, CUSTOMSCREEN);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_CLOSEWINDOW);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.get8(window + wd_BorderTop), 20u);
    CHECK_EQ(r.get32(window + wd_IFont), hasklig13);
    CHECK_EQ(r.get32(r.get32(window + wd_RPort) + rp_Font), hasklig13);
    CHECK_EQ(r.get16(hasklig13 + tf_Accessors), 2u);
    CHECK_EQ(window_pixel(r, window, 100, 19), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(window_pixel(r, window, 100, 20), 0u);

    chrome::Metrics bar;
    bar.font_height = 13;
    const Rect close_box = chrome::close_button_box(bar);
    CHECK_EQ(close_box.y1 - close_box.y0 + 1, 16);
    CHECK_EQ(close_box.x1 - close_box.x0 + 1, 18);
    CHECK_EQ(close_box.y0 - 1, bar.bar_height() - 1 - close_box.y1);
    // The corner of the button a topaz bar's would not reach.
    const i32 x = 40 + close_box.x1 - 1;
    const i32 y = 60 + close_box.y1 - 1;
    r.intuition.pointer_event(x, y, kMouseLeft, kMouseLeft, 0, false);
    r.intuition.pointer_event(x, y, 0, 0, kMouseLeft, false);
    const u32 msg = window_message(r, window);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_CLOSEWINDOW);
    reply_msg(r, msg);

    r.intuition.close_window(window);
    CHECK_EQ(r.get16(hasklig13 + tf_Accessors), 1u);
    CHECK(r.intuition.close_screen(screen));
    CHECK_EQ(r.get16(hasklig13 + tf_Accessors), 0u);
}

// NewScreen.Font names the font as SA_Font does; a font not in memory
// leaves the screen in the default font.
void screen_takes_the_font_of_its_newscreen() {
    GfxRig r;
    const u32 hasklig11 = font_address(r, 11);
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    r.put32(kNewScreen + ns_Font, kBuffer);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    CHECK_EQ(r.get8(screen + sc_BarHeight), 17u);
    CHECK_EQ(r.intuition.font_of_screen(screen), hasklig11);

    r.str(kBuffer + 0x40, "nowhere.font");
    const u32 other = open_new(r, kNewScreen);
    CHECK(other != 0);
    CHECK_EQ(r.get8(other + sc_BarHeight), 14u);
    CHECK_EQ(r.intuition.font_of_screen(other), r.graphics.topaz_font());
    CHECK(r.read_str(r.get32(r.get32(other + sc_Font) + ta_Name)) == "topaz.font");
}

// SA_SysFont 1 is the screen font of the preferences, which the windows
// use for their title bars but not in their RastPorts; SA_SysFont 0 is
// the default font.
void sa_sysfont_picks_the_screen_font_or_the_default_font() {
    GfxRig r;
    const u32 topaz = r.graphics.topaz_font();
    const u32 hasklig13 = font_address(r, 13);
    const u32 size = put_font_prefs(r, kIPrefs, "hasklig.font", 13, 1);
    set_iprefs(r, kIPrefs, size, 2);

    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 preferred =
        open_tags(r, kNewScreen, r.tags(kTagList, {{SA_SysFont, 1}, {TAG_DONE, 0}}));
    CHECK(preferred != 0);
    CHECK_EQ(r.get8(preferred + sc_BarHeight), 19u);
    CHECK_EQ(r.intuition.font_of_screen(preferred), hasklig13);
    put_new_window(r, kNewWindow, 40, 60, 200, 100, 0, 0, preferred, CUSTOMSCREEN);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.get8(window + wd_BorderTop), 20u);
    CHECK_EQ(r.get32(window + wd_IFont), hasklig13);
    CHECK_EQ(r.get32(r.get32(window + wd_RPort) + rp_Font), topaz);

    const u32 fixed =
        open_tags(r, kNewScreen, r.tags(kTagList, {{SA_SysFont, 0}, {TAG_DONE, 0}}));
    CHECK(fixed != 0);
    CHECK_EQ(r.get8(fixed + sc_BarHeight), 14u);
    CHECK_EQ(r.intuition.font_of_screen(fixed), topaz);
}

// The default screen is in the screen font of the preferences: it takes a
// new one at once while the Workbench has no windows, moving a window out
// from under its higher bar, and otherwise when OpenWorkBench starts it
// over. A window keeps the title bar it opened with.
void default_screen_follows_the_screen_font_of_the_prefs() {
    GfxRig r;
    const u32 topaz = r.graphics.topaz_font();
    const u32 hasklig13 = font_address(r, 13);
    const u32 hasklig11 = font_address(r, 11);
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.intuition.set_default_screen(screen);
    CHECK_EQ(r.get8(screen + sc_BarHeight), 14u);
    const u32 attr = r.get32(screen + sc_Font);

    put_new_window(r, kNewWindow, 0, 15, 300, 100, 0, 0, screen, CUSTOMSCREEN);
    const u32 early = open_win(r, kNewWindow);
    CHECK(early != 0);
    CHECK_EQ(r.get16(early + wd_TopEdge), 15u);

    const u32 size = put_font_prefs(r, kIPrefs, "hasklig.font", 13, 1);
    set_iprefs(r, kIPrefs, size, 2);
    CHECK_EQ(r.get8(screen + sc_BarHeight), 19u);
    CHECK_EQ(r.get32(screen + sc_Font), attr);
    CHECK(r.read_str(r.get32(attr + ta_Name)) == "hasklig.font");
    CHECK_EQ(r.get16(attr + ta_YSize), 13u);
    CHECK_EQ(r.get32(r.intuition.screen_draw_info(screen) + dri_Font), hasklig13);
    CHECK_EQ(screen_pixel(r, screen, 400, 19), static_cast<u32>(chrome::kMenuLabelPen));
    CHECK_EQ(r.get16(early + wd_TopEdge), 19u);
    CHECK_EQ(r.get8(early + wd_BorderTop), 15u);
    CHECK_EQ(r.get32(early + wd_IFont), topaz);

    put_new_window(r, kNewWindow, 320, 60, 200, 100, WFLG_WBENCHWINDOW, 0, screen,
                   CUSTOMSCREEN);
    const u32 drawer = open_win(r, kNewWindow);
    CHECK(drawer != 0);
    CHECK_EQ(r.get8(drawer + wd_BorderTop), 20u);
    CHECK_EQ(r.get32(drawer + wd_IFont), hasklig13);
    CHECK_EQ(r.get32(r.get32(drawer + wd_RPort) + rp_Font), topaz);

    put_font_prefs(r, kIPrefs, "hasklig.font", 11, 1);
    set_iprefs(r, kIPrefs, size, 2);
    CHECK_EQ(r.get8(screen + sc_BarHeight), 19u);
    r.intuition.close_window(drawer);
    r.intuition.close_window(early);
    r.cpu().a[0] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::OpenWorkBench), screen);
    CHECK_EQ(r.get8(screen + sc_BarHeight), 17u);
    CHECK_EQ(r.intuition.font_of_screen(screen), hasklig11);
    CHECK_EQ(r.get16(hasklig13 + tf_Accessors), 0u);
    CHECK_EQ(screen_pixel(r, screen, 400, 17), static_cast<u32>(chrome::kMenuLabelPen));
    CHECK(screen_pixel(r, screen, 400, 19) != static_cast<u32>(chrome::kMenuLabelPen));
}

// The menu bar is as high as the screen's, the titles are drawn in the
// screen's font and the box of a title hangs below the higher bar.
void menu_bar_and_boxes_follow_the_screen_font() {
    GfxRig r;
    font_address(r, 13);
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen =
        open_tags(r, kNewScreen, r.tags(kTagList, {{SA_Font, kBuffer}, {TAG_DONE, 0}}));
    CHECK(screen != 0);
    put_two_title_strip(r);
    const u32 window = open_menu_window(r, WFLG_ACTIVATE, kMenuIdcmp);
    set_strip(r, window, kMenuA);

    menu_press(r, 300, 50);
    CHECK_EQ(screen_pixel(r, screen, 300, 18), 2u);
    CHECK_EQ(screen_pixel(r, screen, 300, 19), 1u);
    // "File" stands on a baseline lower than topaz reaches.
    u32 inked = 0;
    for (i32 x = 12; x < 40; ++x) {
        inked += screen_pixel(r, screen, x, 12) == 1 ? 1 : 0;
    }
    CHECK(inked != 0);

    // The first item starts two rows into the box under the bar, so row
    // 35 is the second item's here and the third's under a topaz bar.
    menu_move(r, 60, 5);
    menu_move(r, 60, 35);
    menu_release(r, 60, 35);
    const u32 msg = window_message(r, window);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_MENUPICK);
    CHECK_EQ(r.get16(msg + im_Code), static_cast<u32>(1 | (1 << 5) | (31 << 11)));
    reply_msg(r, msg);
}

void menu_right_press_opens_the_bar_and_the_second_item_picks() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_two_title_strip(r);
    const u32 window = open_menu_window(r, WFLG_ACTIVATE, kMenuIdcmp);
    set_strip(r, window, kMenuA);

    const std::vector<u8> before = screen_planes(r, screen);
    const u32 under = screen_pixel(r, screen, 60, 40);
    menu_press(r, 30, 50);
    CHECK_EQ(window_message(r, window), 0u);
    CHECK_EQ(screen_pixel(r, screen, 300, 5), 2u);
    CHECK_EQ(screen_pixel(r, screen, 300, 14), 1u);
    CHECK(screen_planes(r, screen) != before);
    // No title opens until the pointer reaches one.
    CHECK_EQ(screen_pixel(r, screen, 60, 40), under);

    // The open title and the highlighted item are inverted: BARBLOCKPEN (2)
    // turns to BARDETAILPEN's (1) on two planes.
    menu_move(r, 60, 5);
    CHECK_EQ(screen_pixel(r, screen, 54, 5), 1u);
    CHECK_EQ(screen_pixel(r, screen, 100, 30), 2u);
    menu_move(r, 60, 30);
    CHECK_EQ(screen_pixel(r, screen, 100, 30), 1u);
    menu_release(r, 60, 30);

    const u32 msg = window_message(r, window);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_MENUPICK);
    CHECK_EQ(r.get16(msg + im_Code), static_cast<u32>(1 | (1 << 5) | (31 << 11)));
    CHECK_EQ(r.get32(msg + im_IDCMPWindow), window);
    reply_msg(r, msg);
    CHECK_EQ(window_message(r, window), 0u);
    CHECK(screen_planes(r, screen) == before);
}

void menu_release_over_nothing_posts_menunull() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_two_title_strip(r);
    const u32 window = open_menu_window(r, WFLG_ACTIVATE, kMenuIdcmp);
    set_strip(r, window, kMenuA);

    const std::vector<u8> before = screen_planes(r, screen);
    menu_press(r, 30, 50);
    CHECK_EQ(window_message(r, window), 0u);
    menu_release(r, 30, 50);

    const u32 msg = window_message(r, window);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_MENUPICK);
    CHECK_EQ(r.get16(msg + im_Code), static_cast<u32>(MENUNULL));
    reply_msg(r, msg);
    CHECK_EQ(window_message(r, window), 0u);
    CHECK(screen_planes(r, screen) == before);
}

void menu_disabled_item_posts_menunull() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_two_title_strip(r);
    r.put16(kItemA1 + mi_Flags, ITEMTEXT | HIGHCOMP);
    const u32 window = open_menu_window(r, WFLG_ACTIVATE, IDCMP_MENUPICK);
    set_strip(r, window, kMenuA);

    menu_press(r, 30, 50);
    menu_move(r, 10, 5);
    menu_move(r, 10, 30);
    CHECK_EQ(screen_pixel(r, screen, 60, 30), 2u);
    menu_release(r, 10, 30);

    const u32 msg = window_message(r, window);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_MENUPICK);
    CHECK_EQ(r.get16(msg + im_Code), static_cast<u32>(MENUNULL));
    reply_msg(r, msg);
}

void menu_checkit_with_menutoggle_toggles_checked() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_two_title_strip(r);
    const u16 flags = ITEMTEXT | ITEMENABLED | HIGHCOMP | CHECKIT | MENUTOGGLE;
    r.put16(kItemA0 + mi_Flags, flags);
    const u32 window = open_menu_window(r, WFLG_ACTIVATE, IDCMP_MENUPICK);
    set_strip(r, window, kMenuA);

    for (u32 pick = 0; pick < 2; ++pick) {
        menu_press(r, 10, 5);
        menu_move(r, 10, 21);
        menu_release(r, 10, 21);
        const u32 msg = window_message(r, window);
        CHECK(msg != 0);
        CHECK_EQ(r.get32(msg + im_Class), IDCMP_MENUPICK);
        CHECK_EQ(r.get16(msg + im_Code), static_cast<u32>(0xF800));
        reply_msg(r, msg);
        CHECK_EQ(r.get16(kItemA0 + mi_Flags),
                 static_cast<u32>(pick == 0 ? (flags | CHECKED) : flags));
        CHECK_EQ(r.get16(kItemA0 + mi_NextSelect), static_cast<u32>(MENUNULL));
    }
    CHECK_EQ(r.get16(kItemA0 + mi_Flags), static_cast<u32>(flags));
}

void menu_mutual_exclude_clears_the_other_item() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_two_title_strip(r);
    const u16 base = ITEMTEXT | ITEMENABLED | HIGHCOMP | CHECKIT;
    r.put16(kItemA0 + mi_Flags, base);
    r.put32(kItemA0 + mi_MutualExclude, 2);
    r.put16(kItemA1 + mi_Flags, base | CHECKED);
    const u32 window = open_menu_window(r, WFLG_ACTIVATE, IDCMP_MENUPICK);
    set_strip(r, window, kMenuA);

    menu_press(r, 30, 50);
    menu_move(r, 10, 5);
    menu_move(r, 10, 21);
    menu_release(r, 10, 21);

    const u32 msg = window_message(r, window);
    CHECK(msg != 0);
    reply_msg(r, msg);
    CHECK_EQ(r.get16(kItemA0 + mi_Flags), static_cast<u32>(base | CHECKED));
    CHECK_EQ(r.get16(kItemA1 + mi_Flags), static_cast<u32>(base));
}

void menu_subitem_pick_carries_the_subitem_number() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_two_title_strip(r);
    put_item(r, kItemA0, kFillA0, kTextA0, 0, 0, 68, 9, ITEMTEXT | ITEMENABLED | HIGHCOMP,
             kItemA1, kSubItem0, 0, 0);
    put_item(r, kSubItem0, kFillSub0, kTextSub0, 0, 0, 40, 9, ITEMTEXT | ITEMENABLED, kSubItem1,
             0, 0, 0);
    put_item(r, kSubItem1, kFillSub1, kTextSub1, 0, 9, 40, 9, ITEMTEXT | ITEMENABLED, 0, 0, 0,
             0);
    const u32 window = open_menu_window(r, WFLG_ACTIVATE, IDCMP_MENUPICK);
    set_strip(r, window, kMenuA);

    menu_press(r, 30, 50);
    menu_move(r, 10, 5);
    menu_move(r, 10, 21);
    // Sideways through the paper and the shared border into the submenu.
    menu_move(r, 72, 21);
    menu_move(r, 90, 30);
    menu_release(r, 90, 30);

    const u32 msg = window_message(r, window);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_MENUPICK);
    CHECK_EQ(r.get16(msg + im_Code), static_cast<u32>(1 << 11));
    reply_msg(r, msg);
    CHECK_EQ(r.get16(kSubItem1 + mi_NextSelect), static_cast<u32>(MENUNULL));
}

void menu_moving_off_the_box_clears_the_item_but_keeps_the_title() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_two_title_strip(r);
    const u32 window = open_menu_window(r, WFLG_ACTIVATE, IDCMP_MENUPICK);
    set_strip(r, window, kMenuA);

    menu_press(r, 10, 5);
    menu_move(r, 10, 21);
    CHECK_EQ(screen_pixel(r, screen, 60, 21), 1u);
    menu_move(r, 300, 200);
    CHECK_EQ(screen_pixel(r, screen, 60, 21), 2u);
    // Past the last title on the bar the open title stays open too.
    menu_move(r, 300, 5);
    CHECK_EQ(screen_pixel(r, screen, 60, 21), 2u);
    CHECK_EQ(screen_pixel(r, screen, 6, 5), 1u);
    menu_release(r, 300, 5);

    const u32 msg = window_message(r, window);
    CHECK(msg != 0);
    CHECK_EQ(r.get16(msg + im_Code), static_cast<u32>(MENUNULL));
    reply_msg(r, msg);
}

void menu_disabled_title_never_opens() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_two_title_strip(r);
    r.put16(kMenuB + mu_Flags, 0);
    const u32 window = open_menu_window(r, WFLG_ACTIVATE, IDCMP_MENUPICK);
    set_strip(r, window, kMenuA);

    menu_press(r, 60, 5);
    CHECK_EQ(screen_pixel(r, screen, 54, 5), 2u);
    menu_move(r, 60, 21);
    menu_release(r, 60, 21);
    const u32 msg = window_message(r, window);
    CHECK(msg != 0);
    CHECK_EQ(r.get16(msg + im_Code), static_cast<u32>(MENUNULL));
    reply_msg(r, msg);
}

// On a screen of four colours a menu is drawn in the pens of the screen bar:
// BARBLOCKPEN paper in a frame of BARDETAILPEN, the highlighted item inverted,
// and nothing cast beside the box.
void classic_menu_boxes_are_framed_square_and_cast_no_shadow() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    fill_backdrop_rect(r, screen, 0, 15, 639, 399, 3);
    put_two_title_strip(r);
    const u32 window = open_menu_window(r, WFLG_ACTIVATE, IDCMP_MENUPICK);
    set_strip(r, window, kMenuA);
    const u32 beside = screen_pixel(r, screen, 79, 40);
    const u32 below = screen_pixel(r, screen, 60, 47);

    menu_press(r, 10, 5);
    menu_move(r, 10, 21);
    CHECK_EQ(screen_pixel(r, screen, 4, 30), 1u);
    CHECK_EQ(screen_pixel(r, screen, 5, 30), 2u);
    CHECK_EQ(screen_pixel(r, screen, 60, 21), 1u);
    CHECK_EQ(screen_pixel(r, screen, 79, 40), beside);
    CHECK_EQ(screen_pixel(r, screen, 60, 47), below);

    menu_release(r, 300, 200);
    const u32 msg = window_message(r, window);
    CHECK(msg != 0);
    reply_msg(r, msg);
}

void menu_on_a_chrome_screen_highlights_ghosts_and_restores() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 8, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_two_title_strip(r);
    r.put16(kItemA1 + mi_Flags, ITEMTEXT | HIGHCOMP);
    const u32 window = open_menu_window(r, WFLG_ACTIVATE, IDCMP_MENUPICK);
    set_strip(r, window, kMenuA);
    const std::vector<u8> before = screen_planes(r, screen);
    const u32 shadowed = screen_pixel(r, screen, 79, 40);

    menu_press(r, 10, 5);
    menu_move(r, 10, 21);
    // The bar keeps the screen title bar's shading and label, and the
    // open title and the highlighted item get the active title bar's.
    CHECK_EQ(screen_pixel(r, screen, 300, 0), static_cast<u32>(chrome::kScreenBar0));
    CHECK_EQ(screen_pixel(r, screen, 300, 14), static_cast<u32>(chrome::kMenuLabelPen));
    const u32 title = screen_pixel(r, screen, 6, 7);
    CHECK(title >= chrome::kActiveBar0 && title <= chrome::kActiveBar7);
    const u32 lit = screen_pixel(r, screen, 9, 21);
    CHECK(lit >= chrome::kActiveBar0 && lit <= chrome::kActiveBar7);
    CHECK_EQ(screen_pixel(r, screen, 5, 30), static_cast<u32>(chrome::kMenuPaperPen));
    CHECK_EQ(screen_pixel(r, screen, 4, 30), static_cast<u32>(chrome::kFrameDark));
    // The disabled item's label is the ghost pen, never the label pen.
    u32 grey = 0;
    u32 dark = 0;
    for (i32 y = 26; y <= 34; ++y) {
        for (i32 x = 8; x <= 72; ++x) {
            const u32 pen = screen_pixel(r, screen, x, y);
            grey += pen == chrome::kMenuGhostPen ? 1 : 0;
            dark += pen == chrome::kMenuLabelPen ? 1 : 0;
        }
    }
    CHECK(grey > 0);
    CHECK_EQ(dark, 0u);
    // The box casts the window shadow.
    CHECK(screen_pixel(r, screen, 79, 40) != shadowed);

    menu_release(r, 300, 200);
    const u32 msg = window_message(r, window);
    CHECK(msg != 0);
    reply_msg(r, msg);
    CHECK(screen_planes(r, screen) == before);
}

// A menu on a 5, 6 or 7 plane screen has the modern look in the pens
// physical_pen gives the chrome there.
void menu_on_a_shallow_modern_screen_highlights_ghosts_and_restores() {
    for (const u32 depth : {5u, 6u, 7u}) {
        GfxRig r;
        put_new_screen(r, kNewScreen, 640, 400, depth, 0, 1, 0);
        const u32 screen = open_new(r, kNewScreen);
        CHECK(screen != 0);
        const auto pen = [depth](u32 logical) { return chrome_pen_on(logical, depth); };
        put_two_title_strip(r);
        r.put16(kItemA1 + mi_Flags, ITEMTEXT | HIGHCOMP);
        const u32 window = open_menu_window(r, WFLG_ACTIVATE, IDCMP_MENUPICK);
        set_strip(r, window, kMenuA);
        const std::vector<u8> before = screen_planes(r, screen);
        const u32 shadowed = screen_pixel(r, screen, 79, 40);

        menu_press(r, 10, 5);
        menu_move(r, 10, 21);
        CHECK_EQ(screen_pixel(r, screen, 300, 0), pen(chrome::kScreenBar0));
        CHECK_EQ(screen_pixel(r, screen, 300, 14), pen(chrome::kMenuLabelPen));
        const u32 title = screen_pixel(r, screen, 6, 7);
        CHECK(title >= pen(chrome::kActiveBar0) && title <= pen(chrome::kActiveBar7));
        const u32 lit = screen_pixel(r, screen, 9, 21);
        CHECK(lit >= pen(chrome::kActiveBar0) && lit <= pen(chrome::kActiveBar7));
        CHECK_EQ(screen_pixel(r, screen, 5, 30), pen(chrome::kMenuPaperPen));
        CHECK_EQ(screen_pixel(r, screen, 4, 30), pen(chrome::kFrameDark));
        // The disabled item's label is the ghost pen, never the label pen.
        u32 grey = 0;
        u32 dark = 0;
        for (i32 y = 26; y <= 34; ++y) {
            for (i32 x = 8; x <= 72; ++x) {
                const u32 at = screen_pixel(r, screen, x, y);
                grey += at == pen(chrome::kMenuGhostPen) ? 1 : 0;
                dark += at == pen(chrome::kMenuLabelPen) ? 1 : 0;
            }
        }
        CHECK(grey > 0);
        CHECK_EQ(dark, 0u);
        CHECK(screen_pixel(r, screen, 79, 40) != shadowed);

        menu_release(r, 300, 200);
        const u32 msg = window_message(r, window);
        CHECK(msg != 0);
        reply_msg(r, msg);
        CHECK(screen_planes(r, screen) == before);
    }
}

void menu_rmbtrap_window_still_gets_menudown_and_menuup() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_two_title_strip(r);
    const u32 window =
        open_menu_window(r, WFLG_ACTIVATE | WFLG_RMBTRAP, IDCMP_MOUSEBUTTONS);
    set_strip(r, window, kMenuA);

    menu_press(r, 30, 50);
    menu_release(r, 30, 50);

    const u32 down = window_message(r, window);
    CHECK(down != 0);
    CHECK_EQ(r.get32(down + im_Class), IDCMP_MOUSEBUTTONS);
    CHECK_EQ(r.get16(down + im_Code), static_cast<u32>(MENUDOWN));
    reply_msg(r, down);
    const u32 up = window_message(r, window);
    CHECK(up != 0);
    CHECK_EQ(r.get16(up + im_Code), static_cast<u32>(MENUUP));
    reply_msg(r, up);
    CHECK_EQ(window_message(r, window), 0u);
}

void menu_mode_ends_without_a_pick_when_the_strip_or_window_goes() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_two_title_strip(r);
    const u32 window = open_menu_window(r, WFLG_ACTIVATE, kMenuIdcmp);
    set_strip(r, window, kMenuA);
    const std::vector<u8> before = screen_planes(r, screen);

    menu_press(r, 30, 50);
    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::ClearMenuStrip), ExitReason::Halt);
    menu_release(r, 30, 50);
    const u32 up = window_message(r, window);
    CHECK(up != 0);
    CHECK_EQ(r.get16(up + im_Code), static_cast<u32>(MENUUP));
    reply_msg(r, up);
    CHECK_EQ(window_message(r, window), 0u);
    CHECK(screen_planes(r, screen) == before);

    set_strip(r, window, kMenuA);
    menu_press(r, 30, 50);
    CHECK_EQ(window_message(r, window), 0u);
    r.intuition.close_window(window);
    CHECK(!r.intuition.window_open(window));
}

void open_window_logs_the_title() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.str(kWinTitle, "Clock");
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, kWinTitle, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK(logged("intuition: window Clock opened"));
}

void draw_image_reads_plane_pick_and_plane_on_off() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 100, 50, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const u32 rp = screen + sc_RastPort;

    const u32 data = kData + 0x1800;
    r.put8(data, 0xA0);
    r.put8(data + 2, 0xC0);  // plane 1: rows are whole words
    const u32 two_planes = put_image(r, kData + 0x1000, 0, 0, 4, 1, 2, data, 3, 0);
    draw_image_at(r, rp, two_planes, 10, 10);
    CHECK_EQ(screen_pixel(r, screen, 10, 10), 3u);
    CHECK_EQ(screen_pixel(r, screen, 11, 10), 2u);
    CHECK_EQ(screen_pixel(r, screen, 12, 10), 1u);
    CHECK_EQ(screen_pixel(r, screen, 13, 10), 0u);

    r.put8(data, 0xA0);
    const u32 one_plane = put_image(r, kData + 0x1040, 0, 0, 4, 1, 1, data, 1, 2);
    draw_image_at(r, rp, one_plane, 10, 20);
    CHECK_EQ(screen_pixel(r, screen, 10, 20), 3u);
    CHECK_EQ(screen_pixel(r, screen, 11, 20), 2u);
    CHECK_EQ(screen_pixel(r, screen, 12, 20), 3u);
    CHECK_EQ(screen_pixel(r, screen, 13, 20), 2u);

    const u32 solid = put_image(r, kData + 0x1080, 0, 0, 3, 2, 0, 0, 0, 2);
    draw_image_at(r, rp, solid, 20, 10);
    for (i32 y = 10; y < 12; ++y) {
        for (i32 x = 20; x < 23; ++x) {
            CHECK_EQ(screen_pixel(r, screen, x, y), 2u);
        }
    }
}

void erase_image_clears_the_image_box() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 100, 50, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const u32 rp = screen + sc_RastPort;
    // The pens of the RastPort are not what erasing draws with.
    r.cpu().a[1] = rp;
    r.cpu().d[0] = 2;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetBPen), ExitReason::Halt);

    const u32 solid = put_image(r, kData + 0x1080, 0, 0, 12, 6, 0, 0, 0, 3);
    draw_image_at(r, rp, solid, 2, 2);
    CHECK_EQ(screen_pixel(r, screen, 5, 5), 3u);

    const u32 data = kData + 0x1800;
    r.put8(data, 0xFF);
    const u32 image = put_image(r, kData + 0x1000, 1, 2, 4, 2, 1, data, 1, 0);
    erase_image_at(r, rp, image, 4, 3);
    for (i32 y = 5; y < 7; ++y) {
        for (i32 x = 5; x < 9; ++x) {
            CHECK_EQ(screen_pixel(r, screen, x, y), 0u);
        }
    }
    CHECK_EQ(screen_pixel(r, screen, 9, 5), 3u);
    CHECK_EQ(screen_pixel(r, screen, 4, 5), 3u);
    CHECK_EQ(screen_pixel(r, screen, 5, 4), 3u);
    CHECK_EQ(screen_pixel(r, screen, 5, 7), 3u);
}

void draw_image_takes_word_offsets_and_follows_the_chain() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 100, 50, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const u32 rp = screen + sc_RastPort;

    // Two images in a chain, each at its own LeftEdge and TopEdge.
    const u32 second = put_image(r, kData + 0x1040, 6, 3, 2, 1, 0, 0, 0, 2);
    const u32 first = put_image(r, kData + 0x1000, 1, 1, 2, 1, 0, 0, 0, 1, second);
    // What a caller leaves in the high halves of D0 and D1 does not count.
    r.cpu().a[0] = rp;
    r.cpu().a[1] = first;
    r.cpu().d[0] = 0xABCD0000u | 10;
    r.cpu().d[1] = 0x7FFF0000u | 20;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::DrawImage), ExitReason::Halt);
    CHECK_EQ(screen_pixel(r, screen, 10, 21), 0u);
    CHECK_EQ(screen_pixel(r, screen, 11, 21), 1u);
    CHECK_EQ(screen_pixel(r, screen, 12, 21), 1u);
    CHECK_EQ(screen_pixel(r, screen, 13, 21), 0u);
    CHECK_EQ(screen_pixel(r, screen, 16, 23), 2u);
    CHECK_EQ(screen_pixel(r, screen, 17, 23), 2u);
    CHECK_EQ(screen_pixel(r, screen, 18, 23), 0u);

    // A negative WORD offset moves the image left and up.
    r.cpu().a[0] = rp;
    r.cpu().a[1] = second;
    r.cpu().d[0] = 0x0000FFFCu;  // -4
    r.cpu().d[1] = 0x0001FFFEu;  // -2
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::DrawImage), ExitReason::Halt);
    CHECK_EQ(screen_pixel(r, screen, 2, 1), 2u);
    CHECK_EQ(screen_pixel(r, screen, 3, 1), 2u);
    // Beside the image the screen bar shows, BARBLOCKPEN (2).
    CHECK_EQ(screen_pixel(r, screen, 4, 1), 2u);

    r.cpu().a[0] = rp;
    r.cpu().a[1] = first;
    r.cpu().d[0] = 0x12340000u | 10;
    r.cpu().d[1] = 0xFFFF0000u | 20;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::EraseImage), ExitReason::Halt);
    CHECK_EQ(screen_pixel(r, screen, 11, 21), 0u);
    CHECK_EQ(screen_pixel(r, screen, 12, 21), 0u);
    CHECK_EQ(screen_pixel(r, screen, 16, 23), 0u);
    CHECK_EQ(screen_pixel(r, screen, 17, 23), 0u);
}

void draw_image_leaves_out_what_it_cannot_draw() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 100, 50, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const u32 rp = screen + sc_RastPort;
    const u32 data = kData + 0x1800;
    for (u32 i = 0; i < 8; ++i) {
        r.put8(data + i, 0xFF);
    }
    // A chain that loops, images of no size and of too many planes: the
    // call returns and only the drawable image shows.
    const u32 empty = put_image(r, kData + 0x1000, 0, 0, 0, 4, 1, data, 1, 0);
    const u32 negative = put_image(r, kData + 0x1040, 0, 0, 0xFFF0, 4, 1, data, 1, 0);
    const u32 deep = put_image(r, kData + 0x1080, 0, 0, 4, 1, 9, data, 1, 0);
    const u32 good = put_image(r, kData + 0x10C0, 20, 0, 4, 1, 1, data, 1, 0);
    r.put32(empty + img_NextImage, negative);
    r.put32(negative + img_NextImage, deep);
    r.put32(deep + img_NextImage, good);
    r.put32(good + img_NextImage, empty);
    // What the images leave out shows the screen bar, BARBLOCKPEN (2).
    draw_image_at(r, rp, empty, 10, 10);
    for (i32 x = 8; x < 20; ++x) {
        CHECK_EQ(screen_pixel(r, screen, x, 10), 2u);
    }
    for (i32 x = 30; x < 34; ++x) {
        CHECK_EQ(screen_pixel(r, screen, x, 10), 1u);
    }
    CHECK_EQ(screen_pixel(r, screen, 34, 10), 2u);

    // PlanePick asks for a second plane the image's Depth does not have:
    // that bit of the pen stays clear.
    const u32 shallow = put_image(r, kData + 0x1100, 0, 0, 4, 1, 1, data, 3, 0);
    draw_image_at(r, rp, shallow, 40, 10);
    CHECK_EQ(screen_pixel(r, screen, 40, 10), 1u);

    // An image wholly outside the bitmap draws nothing.
    draw_image_at(r, rp, good, 200, 10);
    draw_image_at(r, rp, good, 10, 200);
    draw_image_at(r, rp, good, -100, -100);
    CHECK_EQ(screen_pixel(r, screen, 99, 10), 2u);
    draw_image_at(r, 0, good, 10, 10);
    draw_image_at(r, rp, 0, 10, 10);
}

void draw_image_clips_at_the_window_edge() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 200, 100, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 20, 20, 40, 30, WFLG_BORDERLESS | WFLG_ACTIVATE, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 rp = r.get32(window + wd_RPort);

    const u32 data = kData + 0x1800;
    for (u32 i = 0; i < 3; ++i) {
        r.put8(data + i, 0xFF);
    }
    const u32 image = put_image(r, kData + 0x1000, 0, 0, 8, 2, 1, data, 1, 0);
    draw_image_at(r, rp, image, 36, 10);
    for (i32 x = 36; x < 40; ++x) {
        CHECK_EQ(window_pixel(r, window, x, 10), 1u);
    }
    CHECK_EQ(window_pixel(r, window, 35, 10), 0u);
    CHECK_EQ(screen_pixel(r, screen, 60, 30), 0u);

    // Two columns left of the window: the image's second row lands on
    // row 5, its last six columns inside, and nothing beyond its width.
    CHECK_EQ(window_pixel(r, window, 6, 5), 0u);
    draw_image_at(r, rp, image, -2, 4);
    CHECK_EQ(window_pixel(r, window, 2, 5), 1u);
    CHECK_EQ(window_pixel(r, window, 5, 5), 1u);
    CHECK_EQ(window_pixel(r, window, 6, 5), 0u);
}

void draw_image_complemented_inverts_the_pens_in_the_image_planes() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 100, 50, 3, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    const u32 rp = screen + sc_RastPort;

    // A four-pixel image of two planes in pens 0, 1, 2 and 3, on a cleared
    // row and on a row of pen 5: what lay under the image makes no
    // difference, and on eight colours it keeps to its own four pens,
    // 0 and 3 swapped and 1 and 2, as on a screen of two planes.
    r.graphics.set_pens(rp, 0, 0, JAM1);
    r.graphics.rect_fill(rp, 10, 10, 13, 10);
    r.graphics.set_pens(rp, 5, 0, JAM1);
    r.graphics.rect_fill(rp, 10, 12, 13, 12);
    const u32 data = kData + 0x1800;
    r.put16(data, 0x5000);      // plane 0: 0101
    r.put16(data + 2, 0x3000);  // plane 1: 0011
    const u32 image = put_image(r, kData + 0x1000, 0, 0, 4, 1, 2, data, 3, 0);
    for (i32 y : {10, 12}) {
        r.intuition.draw_image_complemented(rp, image, 10, y);
        CHECK_EQ(screen_pixel(r, screen, 10, y), 3u);
        CHECK_EQ(screen_pixel(r, screen, 11, y), 2u);
        CHECK_EQ(screen_pixel(r, screen, 12, y), 1u);
        CHECK_EQ(screen_pixel(r, screen, 13, y), 0u);
    }

    // The same image drawn plainly writes its own pens.
    r.intuition.draw_image(rp, image, 10, 10);
    for (u32 x = 0; x < 4; ++x) {
        CHECK_EQ(screen_pixel(r, screen, 10 + static_cast<i32>(x), 10), x);
    }

    // An image of one plane is complemented in the first two, as on a
    // screen of four colours.
    r.put8(data + 0x40, 0xA0);
    const u32 flat = put_image(r, kData + 0x1080, 0, 0, 4, 1, 1, data + 0x40, 1, 0);
    r.intuition.draw_image_complemented(rp, flat, 10, 14);
    CHECK_EQ(screen_pixel(r, screen, 10, 14), 2u);
    CHECK_EQ(screen_pixel(r, screen, 11, 14), 3u);

    // An image without data is its PlaneOnOff pen, complemented in every
    // plane.
    const u32 solid = put_image(r, kData + 0x1100, 0, 0, 4, 1, 0, 0, 0, 1);
    r.intuition.draw_image_complemented(rp, solid, 20, 10);
    CHECK_EQ(screen_pixel(r, screen, 20, 10), 6u);
    CHECK_EQ(screen_pixel(r, screen, 23, 10), 6u);
}

}

namespace {

void put_easy(GfxRig& r, u32 at, u32 flags, u32 title, u32 text, u32 gadgets) {
    r.put32(at + es_StructSize, EasyStruct_SIZEOF);
    r.put32(at + es_Flags, flags);
    r.put32(at + es_Title, title);
    r.put32(at + es_TextFormat, text);
    r.put32(at + es_GadgetFormat, gadgets);
}

u32 build_requester(GfxRig& r, u32 window, u32 easy, u32 idcmp, u32 args) {
    r.cpu().a[0] = window;
    r.cpu().a[1] = easy;
    r.cpu().d[0] = idcmp;
    r.cpu().a[3] = args;
    return r.result(Base::Intuition, intuition_lvo::BuildEasyRequestArgs);
}

i32 handle_requester(GfxRig& r, u32 window, u32 idcmp_ptr, bool wait) {
    r.cpu().a[0] = window;
    r.cpu().a[1] = idcmp_ptr;
    r.cpu().d[0] = wait ? 1 : 0;
    return static_cast<i32>(r.result(Base::Intuition, intuition_lvo::SysReqHandler));
}

void free_requester(GfxRig& r, u32 window) {
    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::FreeSysRequest), ExitReason::Halt);
}

i32 easy_request(GfxRig& r, u32 window, u32 easy, u32 idcmp_ptr, u32 args) {
    r.cpu().a[0] = window;
    r.cpu().a[1] = easy;
    r.cpu().a[2] = idcmp_ptr;
    r.cpu().a[3] = args;
    return static_cast<i32>(r.result(Base::Intuition, intuition_lvo::EasyRequestArgs));
}

void click_requester_button(GfxRig& r, u32 window, u32 index) {
    const Rect box = IntuitionTest::button(r.intuition, window, index);
    CHECK(box.x1 >= box.x0 && box.y1 >= box.y0);
    const i32 left = static_cast<i16>(r.get16(window + wd_LeftEdge));
    const i32 top = static_cast<i16>(r.get16(window + wd_TopEdge));
    const i32 x = left + (box.x0 + box.x1) / 2;
    const i32 y = top + (box.y0 + box.y1) / 2;
    press(r, x, y);
    release(r, x, y);
}

std::vector<u32> requester_button_pixels(GfxRig& r, u32 window, u32 index) {
    const Rect box = IntuitionTest::button(r.intuition, window, index);
    std::vector<u32> pixels;
    for (i32 y = box.y0; y <= box.y1; ++y) {
        for (i32 x = box.x0; x <= box.x1; ++x) {
            pixels.push_back(window_pixel(r, window, x, y));
        }
    }
    return pixels;
}

u32 open_requester_screen(GfxRig& r) {
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.intuition.set_default_screen(screen);
    return screen;
}

void requester_builds_on_the_default_screen_with_its_title_and_buttons() {
    GfxRig r;
    const u32 screen = open_requester_screen(r);
    r.str(kEasyTitle, "Q");
    r.str(kEasyText, "first line\nsecond line");
    r.str(kEasyGadgets, "Yes|Maybe|No");
    put_easy(r, kEasy, 0, kEasyTitle, kEasyText, kEasyGadgets);
    const u32 window = build_requester(r, 0, kEasy, 0, 0);
    CHECK(window != 0);
    CHECK_EQ(r.get32(window + wd_WScreen), screen);
    CHECK(r.read_str(r.get32(window + wd_Title)) == "Q");
    CHECK(r.get32(window + wd_Title) != kEasyTitle);
    CHECK_EQ(IntuitionTest::button_count(r.intuition, window), 3u);
    CHECK(IntuitionTest::label(r.intuition, window, 0) == "Yes");
    CHECK(IntuitionTest::label(r.intuition, window, 1) == "Maybe");
    CHECK(IntuitionTest::label(r.intuition, window, 2) == "No");
    CHECK_EQ(r.get32(window + wd_Flags),
             static_cast<u32>(WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_ACTIVATE | WFLG_RMBTRAP));
    CHECK_EQ(r.get32(window + wd_IDCMPFlags), static_cast<u32>(IDCMP_MOUSEBUTTONS));
    const i32 left = static_cast<i16>(r.get16(window + wd_LeftEdge));
    const i32 top = static_cast<i16>(r.get16(window + wd_TopEdge));
    const i32 width = static_cast<i32>(r.get16(window + wd_Width));
    const i32 height = static_cast<i32>(r.get16(window + wd_Height));
    CHECK_EQ(left, (640 - width) / 2);
    CHECK_EQ(top, (400 - height) / 2);
    CHECK(logged("intuition: requester Q opened"));

    free_requester(r, window);
    CHECK(!r.intuition.window_open(window));
    CHECK_EQ(IntuitionTest::button_count(r.intuition, window), 0u);
    CHECK_EQ(r.get32(screen + sc_FirstWindow), 0u);
}

// A requester's text and labels are in the font of its screen's titles,
// and its boxes sized by it: topaz 8 with cells of 8, then hasklig 13
// with cells of 7.
void requester_is_laid_out_in_the_font_of_its_screen() {
    GfxRig r;
    const u32 screen = open_requester_screen(r);
    r.str(kEasyTitle, "Q");
    r.str(kEasyText, "first line\nsecond line");
    r.str(kEasyGadgets, "Yes|Maybe|No");
    put_easy(r, kEasy, 0, kEasyTitle, kEasyText, kEasyGadgets);
    // Three buttons as wide as "Maybe" and its padding with two gaps, the
    // margins and the borders; two lines, a gap and a button high.
    const u32 plain = build_requester(r, 0, kEasy, 0, 0);
    CHECK(plain != 0);
    CHECK_EQ(r.get16(plain + wd_Width), 3u * (5 * 8 + 12) + 2 * 8 + 2 * 8 + 4);
    CHECK_EQ(r.get16(plain + wd_Height), (2u * 8 + 16) + 8 + (8 + 6) + 2 * 8 + 15 + 2);
    free_requester(r, plain);

    const u32 size = put_font_prefs(r, kIPrefs, "hasklig.font", 13, 1);
    set_iprefs(r, kIPrefs, size, 2);
    CHECK_EQ(r.get8(screen + sc_BarHeight), 19u);
    const u32 window = build_requester(r, 0, kEasy, 0, 0);
    CHECK(window != 0);
    CHECK_EQ(r.get16(window + wd_Width), 3u * (5 * 7 + 12) + 2 * 8 + 2 * 8 + 4);
    CHECK_EQ(r.get16(window + wd_Height), (2u * 13 + 16) + 8 + (13 + 6) + 2 * 8 + 20 + 2);
    free_requester(r, window);
}

void requester_handler_without_input_returns_minus_two() {
    GfxRig r;
    open_requester_screen(r);
    r.str(kEasyText, "body");
    r.str(kEasyGadgets, "Ok");
    put_easy(r, kEasy, 0, 0, kEasyText, kEasyGadgets);
    const u32 window = build_requester(r, 0, kEasy, 0, 0);
    CHECK(window != 0);
    CHECK_EQ(handle_requester(r, window, 0, false), -2);
    free_requester(r, window);
}

void requester_buttons_answer_with_their_numbers() {
    GfxRig r;
    open_requester_screen(r);
    r.str(kEasyText, "one line");
    r.str(kEasyGadgets, "Yes|Maybe|No");
    put_easy(r, kEasy, 0, 0, kEasyText, kEasyGadgets);
    const u32 window = build_requester(r, 0, kEasy, 0, 0);
    CHECK(window != 0);

    const i32 expected[3] = {1, 2, 0};
    for (u32 i = 0; i < 3; ++i) {
        click_requester_button(r, window, i);
        CHECK_EQ(handle_requester(r, window, 0, false), expected[i]);
    }
    free_requester(r, window);

    // One button is the rightmost and answers 0.
    r.str(kEasyGadgets, "Ok");
    put_easy(r, kEasy, 0, 0, kEasyText, kEasyGadgets);
    const u32 single = build_requester(r, 0, kEasy, 0, 0);
    CHECK(single != 0);
    click_requester_button(r, single, 0);
    CHECK_EQ(handle_requester(r, single, 0, false), 0);
    free_requester(r, single);
}

void requester_release_outside_raises_the_button_again() {
    GfxRig r;
    open_requester_screen(r);
    r.str(kEasyText, "body");
    r.str(kEasyGadgets, "Ok");
    put_easy(r, kEasy, 0, 0, kEasyText, kEasyGadgets);
    const u32 window = build_requester(r, 0, kEasy, 0, 0);
    CHECK(window != 0);
    const std::vector<u32> raised = requester_button_pixels(r, window, 0);
    const Rect box = IntuitionTest::button(r.intuition, window, 0);
    const i32 left = static_cast<i16>(r.get16(window + wd_LeftEdge));
    const i32 top = static_cast<i16>(r.get16(window + wd_TopEdge));
    press(r, left + (box.x0 + box.x1) / 2, top + (box.y0 + box.y1) / 2);
    CHECK_EQ(handle_requester(r, window, 0, false), -2);
    CHECK(requester_button_pixels(r, window, 0) != raised);

    release(r, 1, 1);
    CHECK_EQ(handle_requester(r, window, 0, false), -2);
    CHECK(requester_button_pixels(r, window, 0) == raised);
    free_requester(r, window);
}

// The buttons of a requester on a 5, 6 or 7 plane screen are raised in the
// modern look's pens.
void requester_buttons_on_a_shallow_modern_screen_use_the_mapped_pens() {
    for (const u32 depth : {5u, 6u, 7u}) {
        GfxRig r;
        put_new_screen(r, kNewScreen, 640, 400, depth, 0, 1, 0);
        const u32 screen = open_new(r, kNewScreen);
        CHECK(screen != 0);
        r.intuition.set_default_screen(screen);
        r.str(kEasyText, "body");
        r.str(kEasyGadgets, "Ok");
        put_easy(r, kEasy, 0, 0, kEasyText, kEasyGadgets);
        const u32 window = build_requester(r, 0, kEasy, 0, 0);
        CHECK(window != 0);
        const Rect box = IntuitionTest::button(r.intuition, window, 0);
        const auto pen = [depth](u32 logical) { return chrome_pen_on(logical, depth); };
        CHECK_EQ(window_pixel(r, window, box.x0, box.y0), pen(chrome::kFrameLight));
        CHECK_EQ(window_pixel(r, window, box.x1, box.y1), pen(chrome::kFrameDark));
        CHECK_EQ(window_pixel(r, window, box.x0 + 2, box.y0 + 2), pen(chrome::kInactiveBar0));
        free_requester(r, window);
    }
}

void requester_caller_idcmp_class_returns_minus_one() {
    GfxRig r;
    open_requester_screen(r);
    r.str(kEasyTitle, "Keys");
    r.str(kEasyText, "press a key");
    r.str(kEasyGadgets, "Cancel");
    put_easy(r, kEasy, 0, kEasyTitle, kEasyText, kEasyGadgets);
    const u32 window = build_requester(r, 0, kEasy, IDCMP_RAWKEY, 0);
    CHECK(window != 0);
    const u32 port = r.get32(window + wd_UserPort);
    CHECK(port != 0);
    r.put32(kEasyMessage + mn_ReplyPort, 0);
    r.put32(kEasyMessage + im_Class, IDCMP_RAWKEY);
    r.put16(kEasyMessage + im_Code, 0x20);
    r.put16(kEasyMessage + im_MouseX, 0);
    r.put16(kEasyMessage + im_MouseY, 0);
    CHECK(r.exec.post_message(port, kEasyMessage));

    CHECK_EQ(handle_requester(r, window, kIdcmpOut, false), -1);
    CHECK_EQ(r.get32(kIdcmpOut), static_cast<u32>(IDCMP_RAWKEY));
    CHECK_EQ(r.exec.get_message(port), 0u);
    CHECK_EQ(r.get8(kEasyMessage + LN_TYPE), static_cast<u32>(NT_FREEMSG));
    free_requester(r, window);
}

void requester_title_falls_back_to_the_window_then_the_system() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.str(kWinTitle, "Ref");
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, kWinTitle, 0, 0);
    const u32 reference = open_win(r, kNewWindow);
    CHECK(reference != 0);
    r.str(kEasyText, "body");
    r.str(kEasyGadgets, "Ok");
    put_easy(r, kEasy, 0, 0, kEasyText, kEasyGadgets);

    u32 window = build_requester(r, reference, kEasy, 0, 0);
    CHECK(window != 0);
    CHECK_EQ(r.get32(window + wd_WScreen), screen);
    CHECK(r.read_str(r.get32(window + wd_Title)) == "Ref");
    free_requester(r, window);

    r.intuition.set_default_screen(screen);
    window = build_requester(r, 0, kEasy, 0, 0);
    CHECK(window != 0);
    CHECK(r.read_str(r.get32(window + wd_Title)) == "System Request");
    free_requester(r, window);
}

void requester_formats_text_then_gadgets_from_one_stream() {
    GfxRig r;
    open_requester_screen(r);
    r.str(kEasyText, "%s has %ld");
    r.str(kEasyGadgets, "%s|Cancel");
    r.str(kEasyArg0, "Adam");
    r.str(kEasyArg2, "Save");
    r.put32(kEasyArgs, kEasyArg0);
    r.put32(kEasyArgs + 4, 42);
    r.put32(kEasyArgs + 8, kEasyArg2);
    put_easy(r, kEasy, 0, 0, kEasyText, kEasyGadgets);
    const u32 window = build_requester(r, 0, kEasy, 0, kEasyArgs);
    CHECK(window != 0);
    CHECK_EQ(IntuitionTest::button_count(r.intuition, window), 2u);
    CHECK(IntuitionTest::label(r.intuition, window, 0) == "Save");
    CHECK(IntuitionTest::label(r.intuition, window, 1) == "Cancel");
    free_requester(r, window);
}

void free_sys_request_ignores_zero_and_one() {
    GfxRig r;
    r.cpu().a[0] = 0;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::FreeSysRequest), ExitReason::Halt);
    r.cpu().a[0] = 1;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::FreeSysRequest), ExitReason::Halt);
}

void more_than_eight_requesters_are_kept_at_once() {
    GfxRig r;
    open_requester_screen(r);
    r.str(kEasyText, "body");
    r.str(kEasyGadgets, "Ok");
    put_easy(r, kEasy, 0, 0, kEasyText, kEasyGadgets);
    std::vector<u32> windows;
    for (u32 i = 0; i < 10; ++i) {
        windows.push_back(build_requester(r, 0, kEasy, 0, 0));
        CHECK(windows.back() != 0);
    }
    for (u32 window : windows) {
        CHECK_EQ(IntuitionTest::button_count(r.intuition, window), 1u);
    }
    for (u32 window : windows) {
        free_requester(r, window);
        CHECK_EQ(IntuitionTest::button_count(r.intuition, window), 0u);
    }
    CHECK_EQ(IntuitionTest::requester_window(r.intuition), 0u);
}

void ignore_gadget_hook(void*, u32, u32) {}

void making_intuition_again_clears_everything_it_kept() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    std::vector<u32> screens;
    for (u32 i = 0; i < 6; ++i) {
        screens.push_back(open_new(r, kNewScreen));
        CHECK(screens.back() != 0);
    }
    put_new_window(r, kNewWindow, 0, 0, 32, 32, 0, 0, 0, 0);
    std::vector<u32> windows;
    for (u32 i = 0; i < 19; ++i) {
        windows.push_back(open_win(r, kNewWindow));
        CHECK(windows.back() != 0);
    }
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_MOUSEBUTTONS);
    windows.push_back(open_win(r, kNewWindow));
    CHECK(windows.back() != 0);
    r.intuition.pointer_event(10, 30, kMouseLeft, kMouseLeft, 0, false);
    r.intuition.set_gadget_hook(kData + 0x400, ignore_gadget_hook, nullptr);
    r.str(kEasyText, "body");
    r.str(kEasyGadgets, "Ok");
    put_easy(r, kEasy, 0, 0, kEasyText, kEasyGadgets);
    CHECK(build_requester(r, 0, kEasy, 0, 0) != 0);
    CHECK(IntuitionTest::requester_window(r.intuition) != 0);
    CHECK(r.intuition.active_window() != 0);
    // The screens and their records, the 21 windows and theirs, a queued
    // message, the requester and the gadget hook.
    CHECK(IntuitionTest::records_kept(r.intuition) >= 6u + 6u + 21u + 21u + 1u + 1u + 1u);

    CHECK(r.intuition.init(r.machine, r.exec, r.graphics, r.display));
    CHECK_EQ(IntuitionTest::records_kept(r.intuition), 0u);
    CHECK_EQ(r.intuition.front_screen(), 0u);
    CHECK_EQ(r.display.front_screen, 0u);
    CHECK_EQ(r.intuition.active_window(), 0u);
    for (u32 window : windows) {
        CHECK(!r.intuition.window_open(window));
    }

    const u32 screen = r.intuition.open_screen(kNewScreen, 0);
    CHECK(screen != 0);
    const u32 window = r.intuition.open_window(kNewWindow, 0);
    CHECK(window != 0);
    CHECK(r.intuition.window_open(window));
    CHECK_EQ(IntuitionTest::records_kept(r.intuition), 4u);
}

u64 requester_clock(void*) { return 0; }

struct RequesterClick {
    GfxRig* rig;
    bool clicked = false;
};

bool click_the_requester(void* ctx) {
    RequesterClick& click = *static_cast<RequesterClick*>(ctx);
    if (click.clicked) {
        return true;
    }
    const u32 window = IntuitionTest::requester_window(click.rig->intuition);
    if (window == 0) {
        return true;
    }
    const Rect box = IntuitionTest::button(click.rig->intuition, window, 0);
    if (box.x1 < box.x0) {
        return false;
    }
    const i32 left = static_cast<i16>(click.rig->get16(window + wd_LeftEdge));
    const i32 top = static_cast<i16>(click.rig->get16(window + wd_TopEdge));
    const i32 x = left + (box.x0 + box.x1) / 2;
    const i32 y = top + (box.y0 + box.y1) / 2;
    click.rig->intuition.pointer_event(x, y, kMouseLeft, kMouseLeft, 0, false);
    click.rig->intuition.pointer_event(x, y, 0, 0, kMouseLeft, false);
    click.clicked = true;
    return true;
}

void requester_memory_comes_back_after_easy_request() {
    GfxRig r;
    open_requester_screen(r);
    r.str(kEasyTitle, "Easy");
    r.str(kEasyText, "Hi");
    r.str(kEasyGadgets, "Ok");
    put_easy(r, kEasy, 0, kEasyTitle, kEasyText, kEasyGadgets);
    for (u32 i = 0; i < TC_SIZE; ++i) {
        r.put8(kEasyTask + i, 0);
    }
    r.put8(kEasyTask + LN_TYPE, NT_TASK);
    r.put32(kEasyTask + TC_SIGALLOC, 0x0000FFFF);
    r.exec.set_this_task(kEasyTask);
    TaskHost host{};
    host.now_us = requester_clock;
    r.exec.set_task_host(host);
    RequesterClick click{&r};
    r.exec.set_idle_hook(IdleHook{click_the_requester, &click});

    r.cpu().d[0] = 0;
    const u32 before = avail_mem(r);
    CHECK_EQ(easy_request(r, 0, kEasy, 0, 0), 0);
    r.cpu().d[0] = 0;
    CHECK_EQ(avail_mem(r), before);
    CHECK(click.clicked);
    CHECK_EQ(IntuitionTest::requester_window(r.intuition), 0u);
}

struct RequesterKey {
    GfxRig* rig;
    bool posted = false;
};

bool press_a_key_at_the_requester(void* ctx) {
    RequesterKey& key = *static_cast<RequesterKey*>(ctx);
    if (key.posted) {
        return true;
    }
    const u32 window = IntuitionTest::requester_window(key.rig->intuition);
    if (window == 0) {
        return true;
    }
    const u32 port = key.rig->get32(window + wd_UserPort);
    key.rig->put32(kEasyMessage + mn_ReplyPort, 0);
    key.rig->put32(kEasyMessage + im_Class, IDCMP_RAWKEY);
    key.rig->put16(kEasyMessage + im_Code, 0x20);
    key.posted = key.rig->exec.post_message(port, kEasyMessage);
    return true;
}

// EasyRequestArgs ends on the IDCMP classes *IDCMP_ptr holds on entry and
// writes the class that ended it back there; es_Flags plays no part.
void easy_request_ends_on_the_classes_its_idcmp_pointer_holds() {
    GfxRig r;
    open_requester_screen(r);
    r.str(kEasyTitle, "Keys");
    r.str(kEasyText, "press a key");
    r.str(kEasyGadgets, "Cancel");
    put_easy(r, kEasy, 0, kEasyTitle, kEasyText, kEasyGadgets);
    for (u32 i = 0; i < TC_SIZE; ++i) {
        r.put8(kEasyTask + i, 0);
    }
    r.put8(kEasyTask + LN_TYPE, NT_TASK);
    r.put32(kEasyTask + TC_SIGALLOC, 0x0000FFFF);
    r.exec.set_this_task(kEasyTask);
    TaskHost host{};
    host.now_us = requester_clock;
    r.exec.set_task_host(host);
    RequesterKey key{&r};
    r.exec.set_idle_hook(IdleHook{press_a_key_at_the_requester, &key});

    r.put32(kIdcmpOut, IDCMP_RAWKEY);
    CHECK_EQ(easy_request(r, 0, kEasy, kIdcmpOut, 0), -1);
    CHECK(key.posted);
    CHECK_EQ(r.get32(kIdcmpOut), static_cast<u32>(IDCMP_RAWKEY));
    CHECK_EQ(IntuitionTest::requester_window(r.intuition), 0u);
}

void put_auto_itext(GfxRig& r, u32 at, i32 left, i32 top, u32 text, u32 next = 0) {
    r.put16(at + it_LeftEdge, static_cast<u32>(left));
    r.put16(at + it_TopEdge, static_cast<u32>(top));
    r.put32(at + it_IText, text);
    r.put32(at + it_NextText, next);
}

void put_auto_request_texts(GfxRig& r) {
    r.str(kAutoBodyText0, "first line");
    r.str(kAutoBodyText1, "second line");
    r.str(kAutoPosText, "Retry");
    r.str(kAutoNegText, "Cancel");
    put_auto_itext(r, kAutoBody0, 0, 0, kAutoBodyText0, kAutoBody1);
    put_auto_itext(r, kAutoBody1, 0, 10, kAutoBodyText1);
    put_auto_itext(r, kAutoPos, 0, 0, kAutoPosText);
    put_auto_itext(r, kAutoNeg, 0, 0, kAutoNegText);
}

void prepare_auto_request(GfxRig& r) {
    for (u32 i = 0; i < TC_SIZE; ++i) {
        r.put8(kEasyTask + i, 0);
    }
    r.put8(kEasyTask + LN_TYPE, NT_TASK);
    r.put32(kEasyTask + TC_SIGALLOC, 0x0000FFFF);
    r.exec.set_this_task(kEasyTask);
    TaskHost host{};
    host.now_us = requester_clock;
    r.exec.set_task_host(host);
}

u32 call_auto_request(GfxRig& r, u32 window, u32 body, u32 pos, u32 neg, u32 pos_flags,
                      u32 neg_flags, u32 width, u32 height) {
    r.cpu().a[0] = window;
    r.cpu().a[1] = body;
    r.cpu().a[2] = pos;
    r.cpu().a[3] = neg;
    r.cpu().d[0] = pos_flags;
    r.cpu().d[1] = neg_flags;
    r.cpu().d[2] = width;
    r.cpu().d[3] = height;
    return r.result(Base::Intuition, intuition_lvo::AutoRequest);
}

struct AutoClick {
    GfxRig* rig;
    u32 button = 0;
    bool clicked = false;
    u32 window = 0;
    u32 screen = 0;
    u32 buttons = 0;
    i32 left = 0;
    i32 top = 0;
    i32 width = 0;
    i32 height = 0;
    std::string title{};
};

bool click_auto_request(void* ctx) {
    AutoClick& click = *static_cast<AutoClick*>(ctx);
    if (click.clicked) {
        return true;
    }
    if (click.window == 0) {
        click.window = IntuitionTest::requester_window(click.rig->intuition);
        if (click.window == 0) {
            return true;
        }
        const u32 window = click.window;
        click.screen = click.rig->get32(window + wd_WScreen);
        click.buttons = IntuitionTest::button_count(click.rig->intuition, window);
        click.title = click.rig->read_str(click.rig->get32(window + wd_Title));
        click.left = static_cast<i16>(click.rig->get16(window + wd_LeftEdge));
        click.top = static_cast<i16>(click.rig->get16(window + wd_TopEdge));
        click.width = static_cast<i16>(click.rig->get16(window + wd_Width));
        click.height = static_cast<i16>(click.rig->get16(window + wd_Height));
    }
    const Rect box = IntuitionTest::button(click.rig->intuition, click.window, click.button);
    CHECK(box.x1 >= box.x0 && box.y1 >= box.y0);
    const i32 x = click.left + (box.x0 + box.x1) / 2;
    const i32 y = click.top + (box.y0 + box.y1) / 2;
    click.rig->intuition.pointer_event(x, y, kMouseLeft, kMouseLeft, 0, false);
    click.rig->intuition.pointer_event(x, y, 0, 0, kMouseLeft, false);
    click.clicked = true;
    return true;
}

void auto_request_answers_with_the_button_that_was_chosen() {
    for (u32 button = 0; button < 2; ++button) {
        GfxRig r;
        open_requester_screen(r);
        put_auto_request_texts(r);
        prepare_auto_request(r);
        AutoClick click{&r, button};
        r.exec.set_idle_hook(IdleHook{click_auto_request, &click});
        CHECK_EQ(call_auto_request(r, 0, kAutoBody0, kAutoPos, kAutoNeg, 0, 0, 0, 0),
                 button == 0 ? 1u : 0u);
        CHECK(click.clicked);
        CHECK_EQ(click.buttons, 2u);
        CHECK_EQ(IntuitionTest::requester_window(r.intuition), 0u);
    }
}

void auto_request_without_a_positive_button_only_has_the_negative() {
    GfxRig r;
    open_requester_screen(r);
    r.str(kAutoBodyText0, "body");
    r.str(kAutoNegText, "Cancel");
    put_auto_itext(r, kAutoBody0, 0, 0, kAutoBodyText0);
    put_auto_itext(r, kAutoNeg, 0, 0, kAutoNegText);
    prepare_auto_request(r);
    AutoClick click{&r, 0};
    r.exec.set_idle_hook(IdleHook{click_auto_request, &click});
    CHECK_EQ(call_auto_request(r, 0, kAutoBody0, 0, kAutoNeg, 0, 0, 0, 0), 0u);
    CHECK(click.clicked);
    CHECK_EQ(click.buttons, 1u);
}

void auto_request_uses_the_windows_screen_and_the_default_one() {
    {
        GfxRig r;
        put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
        const u32 screen = open_new(r, kNewScreen);
        CHECK(screen != 0);
        r.str(kWinTitle, "Ref");
        put_new_window(r, kNewWindow, 50, 60, 100, 60, 0, kWinTitle, screen, CUSTOMSCREEN);
        const u32 reference = open_win(r, kNewWindow);
        CHECK(reference != 0);
        put_auto_request_texts(r);
        prepare_auto_request(r);
        AutoClick on_window{&r, 0};
        r.exec.set_idle_hook(IdleHook{click_auto_request, &on_window});
        CHECK_EQ(call_auto_request(r, reference, kAutoBody0, kAutoPos, kAutoNeg, 0, 0, 0, 0),
                 1u);
        CHECK_EQ(on_window.screen, screen);
        CHECK(on_window.title == "Ref");
        CHECK_EQ(on_window.left, (640 - on_window.width) / 2);
        CHECK_EQ(on_window.top, (400 - on_window.height) / 2);
    }
    {
        GfxRig r;
        const u32 screen = open_requester_screen(r);
        put_auto_request_texts(r);
        prepare_auto_request(r);
        AutoClick on_default{&r, 0};
        r.exec.set_idle_hook(IdleHook{click_auto_request, &on_default});
        CHECK_EQ(call_auto_request(r, 0, kAutoBody0, kAutoPos, kAutoNeg, 0, 0, 0, 0), 1u);
        CHECK_EQ(on_default.screen, screen);
        CHECK(on_default.title == "System Request");
        CHECK_EQ(on_default.left, (640 - on_default.width) / 2);
        CHECK_EQ(on_default.top, (400 - on_default.height) / 2);
    }
}

struct AutoFlag {
    GfxRig* rig;
    u32 idcmp_class;
    bool posted = false;
};

bool post_auto_request_class(void* ctx) {
    AutoFlag& flag = *static_cast<AutoFlag*>(ctx);
    if (flag.posted) {
        return true;
    }
    const u32 window = IntuitionTest::requester_window(flag.rig->intuition);
    if (window == 0) {
        return true;
    }
    const u32 port = flag.rig->get32(window + wd_UserPort);
    flag.rig->put32(kEasyMessage + mn_ReplyPort, 0);
    flag.rig->put32(kEasyMessage + im_Class, flag.idcmp_class);
    flag.rig->put16(kEasyMessage + im_Code, 0);
    flag.rig->put16(kEasyMessage + im_MouseX, 0);
    flag.rig->put16(kEasyMessage + im_MouseY, 0);
    flag.posted = flag.rig->exec.post_message(port, kEasyMessage);
    return true;
}

void auto_request_answers_by_the_flag_class_that_came() {
    for (u32 positive = 0; positive < 2; ++positive) {
        GfxRig r;
        open_requester_screen(r);
        put_auto_request_texts(r);
        prepare_auto_request(r);
        AutoFlag flag{&r, IDCMP_RAWKEY};
        r.exec.set_idle_hook(IdleHook{post_auto_request_class, &flag});
        const u32 pos_flags = positive != 0 ? IDCMP_RAWKEY : 0;
        const u32 neg_flags = positive != 0 ? 0 : IDCMP_RAWKEY;
        CHECK_EQ(
            call_auto_request(r, 0, kAutoBody0, kAutoPos, kAutoNeg, pos_flags, neg_flags, 0, 0),
            positive);
        CHECK(flag.posted);
        CHECK_EQ(IntuitionTest::requester_window(r.intuition), 0u);
    }
}

}


// A gadgetclass subclass dispatcher in 68000 code, at kData + 0x1C00: hit
// on GM_HITTEST, counts GM_RENDER at kData + 0x1F00, stays active on
// GM_GOACTIVE, and ends GM_HANDLEINPUT with GMR_NOREUSE | GMR_VERIFY and
// termination 7. Any other method goes to the superclass, as
// DoSuperMethod does.
constexpr u32 kDispatcher = kData + 0x1C00;
constexpr u32 kRenders = kData + 0x1F00;

void put_dispatcher(GfxRig& r) {
    const u16 code[] = {
        0x2011,                          // move.l (a1),d0
        0x0C80, 0x0000, 0x0000, 0x6604,  // cmpi.l #GM_HITTEST,d0; bne.s
        0x7004, 0x4E75,                  // moveq #GMR_GADGETHIT,d0; rts
        0x0C80, 0x0000, 0x0001, 0x660A,  // cmpi.l #GM_RENDER,d0; bne.s
        0x52B9, static_cast<u16>(kRenders >> 16), static_cast<u16>(kRenders),  // addq.l #1
        0x7000, 0x4E75,                  // moveq #0,d0; rts
        0x0C80, 0x0000, 0x0002, 0x6604,  // cmpi.l #GM_GOACTIVE,d0; bne.s
        0x7000, 0x4E75,                  // moveq #GMR_MEACTIVE,d0; rts
        0x0C80, 0x0000, 0x0003, 0x660C,  // cmpi.l #GM_HANDLEINPUT,d0; bne.s
        0x2069, 0x000C,                  // movea.l 12(a1),a0: the termination
        0x7007, 0x2080,                  // moveq #7,d0; move.l d0,(a0)
        0x700A, 0x4E75,                  // moveq #NOREUSE|VERIFY,d0; rts
        0x2068, 0x0018,                  // movea.l cl_Super(a0),a0
        0x2F28, 0x0008, 0x4E75,          // move.l h_Entry(a0),-(sp); rts
    };
    for (u32 i = 0; i < sizeof code / sizeof code[0]; ++i) {
        r.put16(kDispatcher + i * 2, code[i]);
    }
    r.put32(kRenders, 0);
    r.jit.flush();
}

void boopsi_system_classes_and_gadget_objects() {
    GfxRig r;
    hombre::Boopsi& bo = r.intuition.boopsi();
    const u32 root = bo.find_class_named("rootclass");
    const u32 gadget_class = bo.find_class_named("gadgetclass");
    CHECK(root != 0 && gadget_class != 0 && bo.find_class_named("imageclass") != 0);
    CHECK_EQ(r.get32(gadget_class + cl_Super), root);
    CHECK_EQ(r.get16(gadget_class + cl_InstOffset), 0u);
    CHECK_EQ(r.get16(gadget_class + cl_InstSize), ExtGadget_SIZEOF);
    const u32 before = r.exec.free_bytes();

    // NewObjectA by public name: the GA_ tags fill the Gadget.
    const u32 previous = r.exec.alloc(Gadget_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    r.str(kTitle, "gadgetclass");
    r.cpu().a[0] = 0;
    r.cpu().a[1] = kTitle;
    r.cpu().a[2] = r.tags(kTagList, {{GA_Left, 10}, {GA_Top, 20}, {GA_Width, 30},
                                      {GA_Height, 40}, {GA_ID, 7}, {GA_RelVerify, 1},
                                      {GA_UserData, 99}, {GA_Previous, previous},
                                      {TAG_DONE, 0}});
    const u32 g = r.result(Base::Intuition, intuition_lvo::NewObjectA);
    CHECK(g != 0);
    CHECK_EQ(r.get16(g + gg_GadgetType), GTYP_CUSTOMGADGET);
    CHECK_EQ(r.get16(g + gg_LeftEdge), 10u);
    CHECK_EQ(r.get16(g + gg_TopEdge), 20u);
    CHECK_EQ(r.get16(g + gg_Width), 30u);
    CHECK_EQ(r.get16(g + gg_Height), 40u);
    CHECK_EQ(r.get16(g + gg_GadgetID), 7u);
    CHECK_EQ(r.get16(g + gg_Activation) & GACT_RELVERIFY, GACT_RELVERIFY);
    CHECK_EQ(r.get32(g + gg_UserData), 99u);
    CHECK_EQ(r.get32(previous + gg_NextGadget), g);
    CHECK_EQ(bo.class_of(g), gadget_class);
    CHECK_EQ(r.get32(gadget_class + cl_ObjectCount), 1u);

    // GetAttr and SetAttrsA.
    r.cpu().d[0] = GA_Left;
    r.cpu().a[0] = g;
    r.cpu().a[1] = kBuffer;
    CHECK(r.result(Base::Intuition, intuition_lvo::GetAttr) != 0);
    CHECK_EQ(r.get32(kBuffer), 10u);
    r.cpu().a[0] = g;
    r.cpu().a[1] = r.tags(kTagList, {{GA_Disabled, 1}, {GA_Left, 12}, {TAG_DONE, 0}});
    CHECK(r.result(Base::Intuition, intuition_lvo::SetAttrsA) != 0);
    CHECK_EQ(r.get16(g + gg_Flags) & GFLG_DISABLED, GFLG_DISABLED);
    CHECK_EQ(r.get16(g + gg_LeftEdge), 12u);

    // DisposeObject gives its memory back.
    r.cpu().a[0] = g;
    r.call(Base::Intuition, intuition_lvo::DisposeObject);
    CHECK_EQ(r.get32(gadget_class + cl_ObjectCount), 0u);
    r.exec.free(previous, Gadget_SIZEOF);
    CHECK_EQ(r.exec.free_bytes(), before);

    // An unknown class makes no object.
    r.str(kTitle, "noclass");
    r.cpu().a[0] = 0;
    r.cpu().a[1] = kTitle;
    r.cpu().a[2] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::NewObjectA), 0u);
}

void a_class_in_68000_code_reaches_its_superclass() {
    GfxRig r;
    put_dispatcher(r);
    hombre::Boopsi& bo = r.intuition.boopsi();
    r.str(kTitle, "testgclass");
    r.str(kTitle + 0x20, "gadgetclass");
    r.cpu().a[0] = kTitle;
    r.cpu().a[1] = kTitle + 0x20;
    r.cpu().a[2] = 0;
    r.cpu().d[0] = 8;
    r.cpu().d[1] = 0;
    const u32 cl = r.result(Base::Intuition, intuition_lvo::MakeClass);
    CHECK(cl != 0);
    CHECK_EQ(r.get16(cl + cl_InstOffset), ExtGadget_SIZEOF);
    CHECK_EQ(r.get16(cl + cl_InstSize), 8u);
    r.put32(cl + h_Entry, kDispatcher);
    CHECK_EQ(bo.find_class_named("testgclass"), 0u);
    r.cpu().a[0] = cl;
    r.call(Base::Intuition, intuition_lvo::AddClass);
    CHECK_EQ(bo.find_class_named("testgclass"), cl);

    // OM_NEW passes through the 68000 dispatcher to gadgetclass.
    r.cpu().a[0] = 0;
    r.cpu().a[1] = kTitle;
    r.cpu().a[2] = r.tags(kTagList, {{GA_Width, 16}, {TAG_DONE, 0}});
    const u32 g = r.result(Base::Intuition, intuition_lvo::NewObjectA);
    CHECK(g != 0);
    CHECK_EQ(bo.class_of(g), cl);
    CHECK_EQ(r.get16(g + gg_GadgetType), GTYP_CUSTOMGADGET);
    CHECK_EQ(r.get16(g + gg_Width), 16u);

    // A class with objects or subclasses stays.
    r.cpu().a[0] = cl;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::FreeClass), 0u);
    r.cpu().a[0] = g;
    r.call(Base::Intuition, intuition_lvo::DisposeObject);
    r.cpu().a[0] = cl;
    r.call(Base::Intuition, intuition_lvo::RemoveClass);
    CHECK_EQ(bo.find_class_named("testgclass"), 0u);
    r.cpu().a[0] = cl;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::FreeClass), 1u);
}

void a_boopsi_gadget_draws_itself_and_takes_the_click() {
    GfxRig r;
    put_dispatcher(r);
    hombre::Boopsi& bo = r.intuition.boopsi();
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    const u32 cl = bo.make_class(0, 0, bo.gadget_class(), 0, 0);
    r.put32(cl + h_Entry, kDispatcher);
    const u32 tags = r.tags(kTagList, {{GA_Left, 20}, {GA_Top, 20}, {GA_Width, 40},
                                       {GA_Height, 14}, {GA_RelVerify, 1}, {GA_Immediate, 1},
                                       {TAG_DONE, 0}});
    const u32 g = bo.new_object(cl, 0, tags);
    CHECK(g != 0);
    put_new_window(r, kNewWindow, 50, 50, 200, 100, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_GADGETUP | IDCMP_GADGETDOWN);
    r.put32(kNewWindow + nw_FirstGadget, g);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(window != 0);
    const u32 port = r.get32(window + wd_UserPort);
    // Drawn with the window.
    CHECK(r.get32(kRenders) >= 1u);

    // The press: GM_HITTEST, GM_GOACTIVE and GADGETDOWN; the release:
    // GM_HANDLEINPUT ends with GADGETUP carrying the termination.
    r.intuition.pointer_event(75, 75, 1, 1, 0, true);
    u32 msg = get_msg(r, port);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_GADGETDOWN);
    CHECK_EQ(r.get32(msg + im_IAddress), g);
    reply_msg(r, msg);
    r.intuition.pointer_event(75, 75, 0, 0, 1, false);
    msg = get_msg(r, port);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_GADGETUP);
    CHECK_EQ(r.get16(msg + im_Code), 7u);
    CHECK_EQ(r.get32(msg + im_IAddress), g);
    reply_msg(r, msg);

    // A press beside it reaches no gadget.
    r.intuition.pointer_event(150, 120, 1, 1, 0, true);
    r.intuition.pointer_event(150, 120, 0, 0, 1, false);
    CHECK_EQ(get_msg(r, port), 0u);

    r.cpu().a[0] = window;
    r.call(Base::Intuition, intuition_lvo::CloseWindow);
    bo.dispose_object(g);
    CHECK(bo.free_class(cl));
}

void a_boopsi_notify_posts_idcmpupdate() {
    GfxRig r;
    Boopsi& bo = r.intuition.boopsi();
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    put_new_window(r, kNewWindow, 50, 50, 200, 100, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_IDCMPUPDATE);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 port = r.get32(window + wd_UserPort);

    const u32 source = GA_Dummy + 0x200;
    const u32 renamed = GA_Dummy + 0x201;
    const u32 map = r.tags(kTagList, {{source, renamed}, {TAG_DONE, 0}});
    const u32 cl = bo.find_class_named("gadgetclass");
    const u32 g = bo.new_object(cl, 0,
                                r.tags(kTagList + 0x40,
                                       {{GA_ID, 42}, {ICA_TARGET, ICTARGET_IDCMP},
                                        {ICA_MAP, map}, {TAG_DONE, 0}}));
    CHECK(g != 0);
    CHECK_EQ(r.get16(g + gg_GadgetID), 42u);

    // OM_GET answers the notification attributes.
    CHECK(bo.get_attr(ICA_TARGET, g, kBuffer) != 0);
    CHECK_EQ(r.get32(kBuffer), ICTARGET_IDCMP);
    CHECK(bo.get_attr(ICA_MAP, g, kBuffer) != 0);
    CHECK_EQ(r.get32(kBuffer), map);

    // The screen's DrawInfo, made on its first use, stays: count from after.
    const u32 info = r.intuition.make_gadget_info(window);
    CHECK(info != 0);
    const u32 before = r.exec.free_bytes();
    const u32 attrs = r.tags(kTagList + 0x80, {{source, 7}, {TAG_DONE, 0}});
    const u32 notify[] = {OM_NOTIFY, attrs, info, 0};
    bo.send(bo.class_of(g), g, notify, 4);

    const u32 msg = get_msg(r, port);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_IDCMPUPDATE);
    CHECK_EQ(r.get16(msg + im_Code), 0u);
    const u32 list = r.get32(msg + im_IAddress);
    CHECK(list != 0);
    CHECK_EQ(r.get32(list), renamed);
    CHECK_EQ(r.get32(list + 4), 7u);
    CHECK_EQ(r.get32(list + 8), GA_ID);
    CHECK_EQ(r.get32(list + 12), 42u);
    CHECK_EQ(r.get32(list + 16), TAG_DONE);
    reply_msg(r, msg);
    CHECK_EQ(r.exec.free_bytes(), before);
    r.intuition.free_gadget_info(info);

    bo.dispose_object(g);
}

void a_boopsi_notify_without_idcmpupdate_is_dropped() {
    GfxRig r;
    Boopsi& bo = r.intuition.boopsi();
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    put_new_window(r, kNewWindow, 50, 50, 200, 100, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_GADGETUP);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 port = r.get32(window + wd_UserPort);

    const u32 cl = bo.find_class_named("gadgetclass");
    const u32 g = bo.new_object(
        cl, 0, r.tags(kTagList, {{ICA_TARGET, ICTARGET_IDCMP}, {TAG_DONE, 0}}));
    CHECK(g != 0);

    // The screen's DrawInfo, made on its first use, stays: make it first.
    r.cpu().a[0] = r.get32(window + wd_WScreen);
    CHECK(r.result(Base::Intuition, intuition_lvo::GetScreenDrawInfo) != 0);
    const u32 before = r.exec.free_bytes();
    const u32 info = r.intuition.make_gadget_info(window);
    CHECK(info != 0);
    const u32 attrs = r.tags(kTagList + 0x40, {{GA_Width, 9}, {TAG_DONE, 0}});
    const u32 notify[] = {OM_NOTIFY, attrs, info, 0};
    bo.send(bo.class_of(g), g, notify, 4);
    r.intuition.free_gadget_info(info);

    CHECK_EQ(get_msg(r, port), 0u);
    CHECK_EQ(r.exec.free_bytes(), before);

    bo.dispose_object(g);
}

void a_boopsi_notify_chains_through_an_object_target() {
    GfxRig r;
    Boopsi& bo = r.intuition.boopsi();
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    put_new_window(r, kNewWindow, 50, 50, 200, 100, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_IDCMPUPDATE);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 port = r.get32(window + wd_UserPort);

    const u32 cl = bo.find_class_named("gadgetclass");
    const u32 target = bo.new_object(
        cl, 0, r.tags(kTagList, {{ICA_TARGET, ICTARGET_IDCMP}, {TAG_DONE, 0}}));
    CHECK(target != 0);
    const u32 source = GA_Dummy + 0x200;
    const u32 map = r.tags(kTagList + 0x40, {{source, GA_Width}, {TAG_DONE, 0}});
    const u32 changed = bo.new_object(
        cl, 0, r.tags(kTagList + 0x80,
                      {{GA_ID, 42}, {ICA_TARGET, target}, {ICA_MAP, map}, {TAG_DONE, 0}}));
    CHECK(changed != 0);
    CHECK_EQ(r.get16(target + gg_Width), 0u);

    // The screen's DrawInfo, made on its first use, stays: make it first.
    r.cpu().a[0] = r.get32(window + wd_WScreen);
    CHECK(r.result(Base::Intuition, intuition_lvo::GetScreenDrawInfo) != 0);
    const u32 before = r.exec.free_bytes();
    const u32 info = r.intuition.make_gadget_info(window);
    CHECK(info != 0);
    const u32 attrs = r.tags(kTagList + 0xC0, {{source, 7}, {TAG_DONE, 0}});
    const u32 notify[] = {OM_NOTIFY, attrs, info, 0};
    bo.send(bo.class_of(changed), changed, notify, 4);
    r.intuition.free_gadget_info(info);

    // The target took the OM_UPDATE as an attribute change.
    CHECK_EQ(r.get16(target + gg_Width), 7u);

    // And passed it on to the window as an IDCMPUPDATE.
    const u32 msg = get_msg(r, port);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_IDCMPUPDATE);
    const u32 list = r.get32(msg + im_IAddress);
    CHECK(list != 0);
    CHECK_EQ(r.get32(list), GA_Width);
    CHECK_EQ(r.get32(list + 4), 7u);
    CHECK_EQ(r.get32(list + 8), GA_ID);
    CHECK_EQ(r.get32(list + 12), 42u);
    CHECK_EQ(r.get32(list + 16), TAG_DONE);
    reply_msg(r, msg);
    CHECK_EQ(r.exec.free_bytes(), before);

    bo.dispose_object(changed);
    bo.dispose_object(target);
}

void sysiclass_sizes_and_lifecycle() {
    GfxRig r;
    Boopsi& bo = r.intuition.boopsi();
    const u32 sysi = bo.find_class_named("sysiclass");
    const u32 image_class = bo.find_class_named("imageclass");
    CHECK(sysi != 0 && image_class != 0);
    CHECK_EQ(r.get32(sysi + cl_Super), image_class);
    const u32 before = r.exec.free_bytes();
    r.str(kTitle, "sysiclass");

    // CLOSEIMAGE at the default MEDRES size.
    r.cpu().a[0] = 0;
    r.cpu().a[1] = kTitle;
    r.cpu().a[2] = r.tags(kTagList, {{SYSIA_Which, CLOSEIMAGE}, {TAG_DONE, 0}});
    const u32 close = r.result(Base::Intuition, intuition_lvo::NewObjectA);
    CHECK(close != 0);
    CHECK_EQ(r.get16(close + img_Width), 20u);
    CHECK_EQ(r.get16(close + img_Height), 11u);
    r.cpu().a[0] = close;
    r.call(Base::Intuition, intuition_lvo::DisposeObject);

    // A reference font of 8 sets the height of AMIGAKEY to 8 and of the
    // others to 11.
    r.put16(kBuffer + 20, 8);
    r.cpu().a[0] = 0;
    r.cpu().a[1] = kTitle;
    r.cpu().a[2] = r.tags(kTagList, {{SYSIA_Which, AMIGAKEY},
                                    {SYSIA_ReferenceFont, kBuffer}, {TAG_DONE, 0}});
    const u32 amiga = r.result(Base::Intuition, intuition_lvo::NewObjectA);
    CHECK(amiga != 0);
    CHECK_EQ(r.get16(amiga + img_Width), 23u);
    CHECK_EQ(r.get16(amiga + img_Height), 8u);

    r.cpu().a[0] = 0;
    r.cpu().a[1] = kTitle;
    r.cpu().a[2] = r.tags(kTagList, {{SYSIA_Which, DEPTHIMAGE},
                                    {SYSIA_ReferenceFont, kBuffer}, {TAG_DONE, 0}});
    const u32 depth = r.result(Base::Intuition, intuition_lvo::NewObjectA);
    CHECK(depth != 0);
    CHECK_EQ(r.get16(depth + img_Width), 23u);
    CHECK_EQ(r.get16(depth + img_Height), 11u);

    // The size gadget and the arrows keep the size of the borders they sit
    // in whatever the reference font.
    r.put16(kBuffer + 20, 15);
    struct BorderSized {
        u32 which;
        u32 height;
    };
    const BorderSized border_sized[] = {
        {SIZEIMAGE, 10}, {UPIMAGE, 11}, {DOWNIMAGE, 11}, {LEFTIMAGE, 10}, {RIGHTIMAGE, 10}};
    for (const BorderSized& sized : border_sized) {
        r.cpu().a[0] = 0;
        r.cpu().a[1] = kTitle;
        r.cpu().a[2] = r.tags(kTagList, {{SYSIA_Which, sized.which},
                                        {SYSIA_ReferenceFont, kBuffer}, {TAG_DONE, 0}});
        const u32 image = r.result(Base::Intuition, intuition_lvo::NewObjectA);
        CHECK(image != 0);
        CHECK_EQ(r.get16(image + img_Height), sized.height);
        r.cpu().a[0] = image;
        r.call(Base::Intuition, intuition_lvo::DisposeObject);
    }

    r.cpu().a[0] = amiga;
    r.call(Base::Intuition, intuition_lvo::DisposeObject);
    r.cpu().a[0] = depth;
    r.call(Base::Intuition, intuition_lvo::DisposeObject);

    // An unknown Which makes no object and leaves none behind.
    r.cpu().a[0] = 0;
    r.cpu().a[1] = kTitle;
    r.cpu().a[2] = r.tags(kTagList, {{SYSIA_Which, 0x7FFF}, {TAG_DONE, 0}});
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::NewObjectA), 0u);
    CHECK_EQ(r.get32(sysi + cl_ObjectCount), 0u);
    CHECK_EQ(r.exec.free_bytes(), before);
}

void draw_image_state_draws_a_sysiclass_image() {
    GfxRig r;
    Boopsi& bo = r.intuition.boopsi();
    const u32 sysi = bo.find_class_named("sysiclass");
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    put_new_window(r, kNewWindow, 50, 50, 200, 100, WFLG_ACTIVATE, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 rp = r.get32(window + wd_RPort);
    const u32 image = bo.new_object(
        sysi, 0, r.tags(kTagList, {{SYSIA_Which, CLOSEIMAGE}, {TAG_DONE, 0}}));
    CHECK(image != 0);
    const i32 left = 30;
    const i32 top = 20;
    const i32 right = left + 19;
    const i32 bottom = top + 10;
    const u32 bitmap = r.get32(rp + rp_BitMap);
    const u32 bpr = r.get16(bitmap + bm_BytesPerRow);
    const u32 rows = r.get16(bitmap + bm_Rows);

    const std::vector<u8> before = window_planes(r, window);
    draw_image_state_at(r, rp, image, left, top, IDS_NORMAL);
    const std::vector<u8> normal = window_planes(r, window);
    CHECK(normal != before);
    CHECK(changed_only_inside(before, normal, bpr, rows, left, top, right, bottom));

    draw_image_state_at(r, rp, image, left, top, IDS_SELECTED);
    const std::vector<u8> selected = window_planes(r, window);
    CHECK(selected != normal);
    CHECK(changed_only_inside(before, selected, bpr, rows, left, top, right, bottom));

    // The caller's pens and draw mode survive the image.
    r.put8(rp + rp_FgPen, 3);
    r.put8(rp + rp_BgPen, 2);
    r.put8(rp + rp_DrawMode, JAM2);
    draw_image_state_at(r, rp, image, left, top, IDS_NORMAL);
    CHECK_EQ(r.get8(rp + rp_FgPen), 3u);
    CHECK_EQ(r.get8(rp + rp_BgPen), 2u);
    CHECK_EQ(r.get8(rp + rp_DrawMode), static_cast<u32>(JAM2));

    bo.dispose_object(image);
}

// A program that draws a window frame of its own lays the border images
// over the frame's lines, so each image has to carry the stretch it covers:
// light along its top and left, dark along its bottom and right.
void border_images_carry_the_frame_lines_they_cover() {
    GfxRig r;
    Boopsi& bo = r.intuition.boopsi();
    const u32 sysi = bo.find_class_named("sysiclass");
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    put_new_window(r, kNewWindow, 50, 50, 200, 100, WFLG_ACTIVATE, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const u32 rp = r.get32(window + wd_RPort);
    // The pens of no DrawInfo.
    const u32 shine = 2;
    const u32 shadow = 1;
    const u32 fill = 3;
    const u32 background = 0;
    const i32 left = 40;
    const i32 top = 30;
    // True when every pixel from (x0, y0) to (x1, y1) of the image has pen.
    const auto all = [&r, window, left, top](i32 x0, i32 y0, i32 x1, i32 y1, u32 pen) {
        for (i32 y = y0; y <= y1; ++y) {
            for (i32 x = x0; x <= x1; ++x) {
                if (window_pixel(r, window, left + x, top + y) != pen) {
                    return false;
                }
            }
        }
        return true;
    };
    const auto draw = [&r, &bo, sysi, rp, left, top](u32 which, u32 state) {
        const u32 image =
            bo.new_object(sysi, 0, r.tags(kTagList, {{SYSIA_Which, which}, {TAG_DONE, 0}}));
        CHECK(image != 0);
        draw_image_state_at(r, rp, image, left, top, state);
        bo.dispose_object(image);
    };

    // An arrow of the right border, 18 by 11: the sunken box's light line
    // down its left, the window's dark edge down its right.
    for (const u32 which : {UPIMAGE, DOWNIMAGE}) {
        draw(which, IDS_NORMAL);
        CHECK(all(0, 0, 0, 10, shine));
        CHECK(all(0, 0, 16, 0, shine));
        CHECK(all(17, 0, 17, 10, shadow));
        CHECK(all(1, 10, 17, 10, shadow));
        CHECK_EQ(window_pixel(r, window, left + 1, top + 1), fill);
    }
    // In an inactive window the lines stay and the face is the background.
    draw(UPIMAGE, IDS_INACTIVENORMAL);
    CHECK(all(0, 0, 0, 10, shine));
    CHECK(all(17, 0, 17, 10, shadow));
    CHECK_EQ(window_pixel(r, window, left + 1, top + 1), background);

    // The size gadget, 18 by 10, in the window's corner: both of the
    // window's dark edges run through it.
    draw(SIZEIMAGE, IDS_NORMAL);
    CHECK(all(0, 0, 0, 8, shine));
    CHECK(all(0, 0, 16, 0, shine));
    CHECK(all(17, 0, 17, 9, shadow));
    CHECK(all(0, 9, 17, 9, shadow));

    // An arrow of the bottom border, 16 by 10: light along its top, the
    // window's dark edge along its bottom.
    for (const u32 which : {LEFTIMAGE, RIGHTIMAGE}) {
        draw(which, IDS_NORMAL);
        CHECK(all(0, 0, 15, 0, shine));
        CHECK(all(0, 0, 0, 8, shine));
        CHECK(all(0, 9, 15, 9, shadow));
        CHECK(all(15, 1, 15, 9, shadow));
    }

    // The close gadget, 20 by 11, is a raised button: the window's light
    // edges along its top and left, the title bar's dark line along its
    // bottom and a dark edge on its right, before the light line that
    // starts the title.
    draw(CLOSEIMAGE, IDS_NORMAL);
    CHECK(all(0, 0, 19, 0, shine));
    CHECK(all(0, 0, 0, 10, shine));
    CHECK(all(1, 10, 19, 10, shadow));
    CHECK(all(18, 1, 18, 10, shadow));
    CHECK(all(19, 0, 19, 9, shine));
    // Its glyph is a square of 7 by 5 etched into the face: dark along the
    // top and left, light along the bottom and right, the face inside and
    // around it.
    const auto close_glyph = [&all, shine, shadow](u32 face) {
        return all(6, 3, 11, 3, shadow) && all(6, 3, 6, 7, shadow) &&
               all(12, 3, 12, 7, shine) && all(7, 7, 12, 7, shine) && all(7, 4, 11, 6, face) &&
               all(1, 1, 5, 9, face) && all(13, 1, 17, 9, face) && all(6, 1, 12, 2, face) &&
               all(6, 8, 12, 9, face);
    };
    CHECK(close_glyph(fill));
    // Held down its edges swap their pens; the title's light line and the
    // glyph stay.
    draw(CLOSEIMAGE, IDS_SELECTED);
    CHECK(all(0, 0, 18, 0, shadow));
    CHECK(all(0, 0, 0, 10, shadow));
    CHECK(all(1, 10, 17, 10, shine));
    CHECK(all(18, 1, 18, 10, shine));
    CHECK(all(19, 0, 19, 9, shine));
    CHECK_EQ(window_pixel(r, window, left + 19, top + 10), shadow);
    CHECK(close_glyph(fill));
    // In an inactive window the face is the background.
    draw(CLOSEIMAGE, IDS_INACTIVENORMAL);
    CHECK(all(18, 1, 18, 10, shadow));
    CHECK(close_glyph(background));

    // The depth and zoom gadgets, 23 by 11, start with the dark line that
    // ends the gadget on their left, and a raised button follows. Only the
    // depth gadget's right edge is the window's, dark to the top. Held
    // down, the button's edges swap their pens and the first line stays.
    for (const u32 which : {ZOOMIMAGE, DEPTHIMAGE}) {
        draw(which, IDS_NORMAL);
        CHECK(all(0, 0, 21, 0, shine));
        CHECK(all(0, 1, 0, 10, shadow));
        CHECK(all(1, 1, 1, 9, shine));
        CHECK(all(0, 10, 22, 10, shadow));
        CHECK(all(22, 1, 22, 10, shadow));
        CHECK_EQ(window_pixel(r, window, left + 22, top), which == DEPTHIMAGE ? shadow : shine);
        CHECK(all(2, 1, 7, 9, fill));
        CHECK(all(15, 1, 21, 9, fill));
        draw(which, IDS_SELECTED);
        CHECK(all(1, 0, 21, 0, shadow));
        CHECK(all(0, 1, 0, 10, shadow));
        CHECK(all(1, 1, 1, 9, shadow));
        CHECK(all(2, 10, 21, 10, shine));
        CHECK(all(22, 1, 22, 10, shine));
    }
    // The zoom glyph is a square of 7 by 7 etched like the close gadget's,
    // with a small light sheet outlined in the dark pen in its top left
    // corner.
    for (const u32 state : {IDS_NORMAL, IDS_INACTIVENORMAL}) {
        const u32 face = state == IDS_NORMAL ? fill : background;
        draw(ZOOMIMAGE, state);
        CHECK(all(8, 2, 13, 2, shadow));
        CHECK(all(8, 2, 8, 8, shadow));
        CHECK(all(14, 2, 14, 8, shine));
        CHECK(all(9, 8, 14, 8, shine));
        CHECK(all(11, 2, 11, 5, shadow));
        CHECK(all(8, 5, 11, 5, shadow));
        CHECK(all(9, 3, 10, 4, shine));
        CHECK(all(12, 3, 13, 7, face));
        CHECK(all(9, 6, 13, 7, face));
    }
    // The depth glyph is two sheets of 5 by 5 on the face: a dark outline
    // at the back and a light one in front, lower and to the right, which
    // covers the corner of the first.
    for (const u32 state : {IDS_NORMAL, IDS_INACTIVENORMAL}) {
        const u32 face = state == IDS_NORMAL ? fill : background;
        draw(DEPTHIMAGE, state);
        CHECK(all(8, 2, 12, 2, shadow));
        CHECK(all(8, 2, 8, 6, shadow));
        CHECK(all(12, 2, 12, 3, shadow));
        CHECK(all(8, 6, 9, 6, shadow));
        CHECK(all(9, 3, 11, 3, face));
        CHECK(all(9, 4, 9, 5, face));
        CHECK(all(10, 4, 14, 4, shine));
        CHECK(all(10, 8, 14, 8, shine));
        CHECK(all(10, 4, 10, 8, shine));
        CHECK(all(14, 4, 14, 8, shine));
        CHECK(all(11, 5, 13, 7, face));
        CHECK(all(13, 2, 14, 3, face));
        CHECK(all(8, 7, 9, 8, face));
    }

    // The screen's depth gadget takes the screen bar's pens: a line of the
    // detail pen down its left and the bar's trim line along its bottom.
    // Its sheets are both outlined in the detail pen: a background one
    // behind a light one.
    const u32 bar_detail = 1;
    const u32 bar_block = 2;
    const u32 bar_trim = 1;
    draw(SDEPTHIMAGE, IDS_NORMAL);
    CHECK(all(0, 0, 0, 10, bar_detail));
    CHECK(all(0, 10, 22, 10, bar_trim));
    CHECK(all(1, 0, 22, 0, bar_block));
    CHECK(all(8, 1, 12, 1, bar_detail));
    CHECK(all(9, 2, 11, 2, background));
    CHECK(all(10, 3, 14, 3, bar_detail));
    CHECK(all(10, 7, 14, 7, bar_detail));
    CHECK(all(11, 4, 13, 6, shine));
}

// An IntuiText is measured and printed in the font its ITextFont names:
// hasklig 13, with cells of 7. Without one IntuiTextLength measures the
// default font and PrintIText draws in the RastPort's.
void intuitext_is_measured_and_printed_in_its_font() {
    GfxRig r;
    const u32 hasklig13 = font_address(r, 13);
    const u32 baseline = r.get16(hasklig13 + tf_Baseline);
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    const u32 it = kBuffer + 0x100;
    r.str(kTitle, "Hello");
    r.put8(it, 1);
    r.put8(it + 1, 0);
    r.put8(it + 2, JAM1);
    r.put16(it + 4, 0);
    r.put16(it + 6, 0);
    r.put32(it + 8, kBuffer);
    r.put32(it + 12, kTitle);
    r.put32(it + 16, 0);
    r.cpu().a[0] = it;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::IntuiTextLength), 35u);

    put_new_window(r, kNewWindow, 50, 50, 200, 100, WFLG_ACTIVATE, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    const auto inked = [&](i32 x0, i32 y0, i32 x1, i32 y1) {
        u32 count = 0;
        for (i32 y = y0; y <= y1; ++y) {
            for (i32 x = x0; x <= x1; ++x) {
                count += window_pixel(r, window, x, y) == 1 ? 1 : 0;
            }
        }
        return count;
    };
    const auto print = [&](i32 left, i32 top) {
        r.cpu().a[0] = r.get32(window + wd_RPort);
        r.cpu().a[1] = it;
        r.cpu().d[0] = static_cast<u32>(left);
        r.cpu().d[1] = static_cast<u32>(top);
        CHECK_EQ(r.call(Base::Intuition, intuition_lvo::PrintIText), ExitReason::Halt);
    };
    // The letters stand on the font's baseline, below topaz's eight rows.
    print(10, 30);
    CHECK(inked(10, 30 + static_cast<i32>(baseline), 44, 30 + static_cast<i32>(baseline)) != 0);
    CHECK_EQ(inked(45, 30, 60, 42), 0u);
    CHECK(baseline > 7);

    // No font of its own: the RastPort's topaz, and the default font's
    // width.
    r.put32(it + 8, 0);
    print(10, 60);
    CHECK(inked(10, 60, 49, 67) != 0);
    CHECK_EQ(inked(10, 68, 60, 72), 0u);
    r.cpu().a[0] = it;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::IntuiTextLength), 40u);
    const u32 size = put_font_prefs(r, kIPrefs, "hasklig.font", 13, 0);
    set_iprefs(r, kIPrefs, size, 2);
    r.cpu().a[0] = it;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::IntuiTextLength), 35u);
}

void intuitext_borders_and_blocking_requests() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    // IntuiTextLength: 8 pixels a character of topaz.
    const u32 it = kBuffer;
    r.str(kTitle, "Hello");
    r.put32(it + 12, kTitle);
    r.put32(it + 16, 0);
    r.cpu().a[0] = it;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::IntuiTextLength), 40u);

    const u32 gad = r.exec.alloc(Gadget_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    put_gadget(r, gad, 0, 20, 20, 40, 14, GFLG_GADGHCOMP, GACT_RELVERIFY, GTYP_BOOLGADGET, 0);
    put_new_window(r, kNewWindow, 50, 50, 200, 100, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_GADGETUP);
    r.put32(kNewWindow + nw_FirstGadget, gad);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    const u32 port = r.get32(window + wd_UserPort);
    const u32 rp = r.get32(window + wd_RPort);

    // DrawBorder: a line of pen 1 through two points.
    const u32 border = kBuffer + 0x40;
    const u32 points = kBuffer + 0x60;
    r.put16(points, 10);
    r.put16(points + 2, 60);
    r.put16(points + 4, 20);
    r.put16(points + 6, 60);
    r.put16(border, 0);
    r.put16(border + 2, 0);
    r.put8(border + 4, 1);
    r.put8(border + 5, 0);
    r.put8(border + 6, JAM1);
    r.put8(border + 7, 2);
    r.put32(border + 8, points);
    r.put32(border + 12, 0);
    r.cpu().a[0] = rp;
    r.cpu().a[1] = border;
    r.cpu().d[0] = 2;
    r.cpu().d[1] = 3;
    r.call(Base::Intuition, intuition_lvo::DrawBorder);
    CHECK_EQ(window_pixel(r, window, 12, 63), 1u);
    CHECK_EQ(window_pixel(r, window, 22, 63), 1u);
    CHECK_EQ(window_pixel(r, window, 23, 63), 0u);

    // InitRequester clears it; Request blocks the window's gadgets until
    // EndRequest.
    const u32 req = kBuffer + 0x100;
    for (u32 i = 0; i < 112; ++i) {
        r.put8(req + i, 0xAA);
    }
    r.cpu().a[0] = req;
    r.call(Base::Intuition, intuition_lvo::InitRequester);
    CHECK_EQ(r.get32(req), 0u);
    CHECK_EQ(r.get32(req + 108), 0u);
    r.cpu().a[0] = req;
    r.cpu().a[1] = window;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::Request), 1u);
    r.intuition.pointer_event(75, 75, 1, 1, 0, true);
    r.intuition.pointer_event(75, 75, 0, 0, 1, false);
    CHECK_EQ(get_msg(r, port), 0u);
    r.cpu().a[0] = req;
    r.cpu().a[1] = window;
    r.call(Base::Intuition, intuition_lvo::EndRequest);
    r.intuition.pointer_event(75, 75, 1, 1, 0, true);
    r.intuition.pointer_event(75, 75, 0, 0, 1, false);
    const u32 msg = get_msg(r, port);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_GADGETUP);
    reply_msg(r, msg);
}

// A string gadget places its cursor by the widths of its font: the one of
// its StringExtend, or the font its window's titles are in. Here both are
// hasklig 13, whose cells are 7 wide where topaz's are 8.
void a_string_gadget_measures_its_own_font() {
    GfxRig r;
    const u32 hasklig13 = font_address(r, 13);
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    const u32 gad = r.exec.alloc(Gadget_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    const u32 si = r.exec.alloc(StringInfo_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    const u32 extend = r.exec.alloc(StringExtend_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    const u32 buffer = r.exec.alloc(40, MEMF_PUBLIC | MEMF_CLEAR);
    r.str(buffer, "abcdef");
    r.put32(extend + sex_Font, hasklig13);
    r.put32(si + si_Buffer, buffer);
    r.put16(si + si_MaxChars, 40);
    r.put16(si + si_NumChars, 6);
    r.put32(si + si_Extension, extend);
    put_gadget(r, gad, 0, 20, 20, 120, 14, GFLG_GADGHCOMP | GFLG_STRINGEXTEND, GACT_RELVERIFY,
               GTYP_STRGADGET, 0);
    r.put32(gad + gg_SpecialInfo, si);
    put_new_window(r, kNewWindow, 50, 50, 200, 100, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_FirstGadget, gad);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(window != 0);

    // Two pixels into the fourth cell of seven, which is the third of eight.
    r.intuition.pointer_event(50 + 20 + 3 * 7 + 2, 50 + 24, 1, 1, 0, true);
    r.intuition.pointer_event(50 + 20 + 3 * 7 + 2, 50 + 24, 0, 0, 1, false);
    CHECK(r.intuition.string_active());
    CHECK_EQ(r.get16(si + si_BufferPos), 3u);
    // The cursor is that cell complemented, the font high.
    CHECK_EQ(window_pixel(r, window, 20 + 3 * 7, 20), 3u);
    CHECK_EQ(window_pixel(r, window, 20 + 3 * 7 + 6, 32), 3u);
    CHECK_EQ(window_pixel(r, window, 20 + 4 * 7, 20), 0u);
    r.intuition.string_key(0x1B);

    // In a box four cells wide the cursor after the text leaves room for
    // the last three characters.
    r.put16(gad + gg_Width, 28);
    r.put16(si + si_BufferPos, 6);
    r.cpu().a[0] = gad;
    r.cpu().a[1] = window;
    r.cpu().a[2] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ActivateGadget), 1u);
    CHECK_EQ(r.get16(si + si_DispPos), 3u);
    r.intuition.string_key(0x1B);
    r.intuition.close_window(window);

    // Without a StringExtend the font is the one of the window's titles.
    r.put16(gad + gg_Flags, GFLG_GADGHCOMP);
    r.put16(gad + gg_Width, 120);
    r.put16(si + si_DispPos, 0);
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    r.put32(kNewScreen + ns_Font, kBuffer);
    CHECK(open_new(r, kNewScreen) != 0);
    r.put32(kNewWindow + nw_FirstGadget, gad);
    const u32 other = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(other != 0);
    r.intuition.pointer_event(50 + 20 + 3 * 7 + 2, 50 + 24, 1, 1, 0, true);
    r.intuition.pointer_event(50 + 20 + 3 * 7 + 2, 50 + 24, 0, 0, 1, false);
    CHECK_EQ(r.get16(si + si_BufferPos), 3u);
}

void a_string_gadget_is_edited_from_the_keyboard() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    // A string gadget of 40 characters at most, holding "ab".
    const u32 gad = r.exec.alloc(Gadget_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    const u32 si = r.exec.alloc(StringInfo_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    const u32 buffer = r.exec.alloc(40, MEMF_PUBLIC | MEMF_CLEAR);
    r.str(buffer, "ab");
    r.put32(si + si_Buffer, buffer);
    r.put16(si + si_MaxChars, 40);
    r.put16(si + si_NumChars, 2);
    put_gadget(r, gad, 0, 20, 20, 120, 10, GFLG_GADGHCOMP, GACT_RELVERIFY, GTYP_STRGADGET, 0);
    r.put32(gad + gg_SpecialInfo, si);
    put_new_window(r, kNewWindow, 50, 50, 200, 100, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_GADGETUP);
    r.put32(kNewWindow + nw_FirstGadget, gad);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    const u32 port = r.get32(window + wd_UserPort);
    CHECK(!r.intuition.string_active());

    // A click after the text puts the cursor at its end; typing goes in
    // there, Backspace and the cursor keys edit it.
    r.intuition.pointer_event(50 + 20 + 100, 50 + 24, 1, 1, 0, true);
    r.intuition.pointer_event(50 + 20 + 100, 50 + 24, 0, 0, 1, false);
    CHECK(r.intuition.string_active());
    CHECK_EQ(r.get16(si + si_BufferPos), 2u);
    for (const u8 key : {u8('c'), u8(0x08), u8('d'), u8(0x9B), u8('D'), u8(0x9B), u8('D'),
                         u8('X'), u8(0x7F)}) {
        r.intuition.string_key(key);
    }
    CHECK(r.read_str(buffer) == "aXd");
    CHECK_EQ(r.get16(si + si_NumChars), 3u);
    CHECK_EQ(get_msg(r, port), 0u);
    // Return ends it with GADGETUP.
    r.intuition.string_key('\n');
    CHECK(!r.intuition.string_active());
    const u32 msg = get_msg(r, port);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_GADGETUP);
    CHECK_EQ(r.get32(msg + im_IAddress), gad);
    reply_msg(r, msg);

    // ActivateGadget starts the editing again; Esc ends it with no word.
    r.cpu().a[0] = gad;
    r.cpu().a[1] = window;
    r.cpu().a[2] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ActivateGadget), 1u);
    CHECK(r.intuition.string_active());
    r.intuition.string_key(0x1B);
    CHECK(!r.intuition.string_active());
    CHECK_EQ(get_msg(r, port), 0u);
    // Typing is not taken past MaxChars - 1.
    r.cpu().a[0] = gad;
    r.cpu().a[1] = window;
    r.call(Base::Intuition, intuition_lvo::ActivateGadget);
    for (u32 i = 0; i < 50; ++i) {
        r.intuition.string_key('z');
    }
    CHECK_EQ(r.get16(si + si_NumChars), 39u);
    // A press elsewhere ends it.
    r.intuition.pointer_event(200, 130, 1, 1, 0, true);
    r.intuition.pointer_event(200, 130, 0, 0, 1, false);
    CHECK(!r.intuition.string_active());

    // DoubleClick: within 1.02 seconds.
    r.cpu().d[0] = 10;
    r.cpu().d[1] = 500000;
    r.cpu().d[2] = 11;
    r.cpu().d[3] = 520000;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::DoubleClick), 1u);
    r.cpu().d[0] = 10;
    r.cpu().d[1] = 0;
    r.cpu().d[2] = 11;
    r.cpu().d[3] = 20001;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::DoubleClick), 0u);

    // The double-click time follows SetPrefs: 0.3 seconds makes half a
    // second too long.
    constexpr u32 kPrefsAt = kData + 0x3800;
    for (u32 i = 0; i < 232; ++i) {
        r.put8(kPrefsAt + i, 0);
    }
    r.put32(kPrefsAt + 20, 0);
    r.put32(kPrefsAt + 24, 300000);
    r.cpu().a[0] = kPrefsAt;
    r.cpu().d[0] = 232;
    r.call(Base::Intuition, intuition_lvo::SetPrefs);
    r.cpu().d[0] = 10;
    r.cpu().d[1] = 0;
    r.cpu().d[2] = 10;
    r.cpu().d[3] = 500000;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::DoubleClick), 0u);
    r.cpu().d[0] = 10;
    r.cpu().d[1] = 0;
    r.cpu().d[2] = 10;
    r.cpu().d[3] = 250000;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::DoubleClick), 1u);

    // QueryOverscan: the mode's rectangle, nothing for an unknown ID.
    constexpr u32 kRectAt = kData + 0x3900;
    for (u32 i = 0; i < 8; i += 2) {
        r.put16(kRectAt + i, 0x5555);
    }
    r.cpu().a[0] = 0xFFFFFFFF;
    r.cpu().a[1] = kRectAt;
    r.cpu().d[0] = 1;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::QueryOverscan), 0u);
    CHECK_EQ(r.get16(kRectAt + 4), 0x5555u);
    r.cpu().a[0] = 0x8004;
    r.cpu().a[1] = kRectAt;
    r.cpu().d[0] = 2;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::QueryOverscan), 1u);
    CHECK_EQ(r.get16(kRectAt + 0), 0u);
    CHECK_EQ(r.get16(kRectAt + 2), 0u);
    CHECK_EQ(r.get16(kRectAt + 4), 639u);
    CHECK_EQ(r.get16(kRectAt + 6), 399u);
    r.cpu().a[0] = 0x21000;
    r.cpu().d[0] = 1;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::QueryOverscan), 1u);
    CHECK_EQ(r.get16(kRectAt + 0), 0u);
    CHECK_EQ(r.get16(kRectAt + 2), 0u);
    CHECK_EQ(r.get16(kRectAt + 4), 319u);
    CHECK_EQ(r.get16(kRectAt + 6), 255u);
    r.cpu().a[0] = 0x31004;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::QueryOverscan), 0u);
}

void a_long_int_gadget_takes_only_numbers_and_keeps_the_value() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    const u32 gad = r.exec.alloc(Gadget_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    const u32 si = r.exec.alloc(StringInfo_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    const u32 buffer = r.exec.alloc(16, MEMF_PUBLIC | MEMF_CLEAR);
    r.put32(si + si_Buffer, buffer);
    r.put16(si + si_MaxChars, 10);
    put_gadget(r, gad, 0, 20, 20, 120, 10, GFLG_GADGHCOMP, GACT_RELVERIFY | GACT_LONGINT,
               GTYP_STRGADGET, 0);
    r.put32(gad + gg_SpecialInfo, si);
    put_new_window(r, kNewWindow, 50, 50, 200, 100, WFLG_ACTIVATE, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_GADGETUP);
    r.put32(kNewWindow + nw_FirstGadget, gad);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    const u32 port = r.get32(window + wd_UserPort);

    r.cpu().a[0] = gad;
    r.cpu().a[1] = window;
    r.cpu().a[2] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::ActivateGadget), 1u);
    // The letter is not taken; the number is read and written back as its
    // canonical decimal.
    for (const u8 key : {u8('0'), u8('4'), u8('x'), u8('2')}) {
        r.intuition.string_key(key);
    }
    CHECK(r.read_str(buffer) == "042");
    r.intuition.string_key('\n');
    CHECK(!r.intuition.string_active());
    CHECK_EQ(r.get32(si + si_LongInt), 42u);
    CHECK_EQ(r.get16(si + si_NumChars), 2u);
    CHECK_EQ(r.get16(si + si_BufferPos), 2u);
    CHECK(r.read_str(buffer) == "42");
    const u32 msg = get_msg(r, port);
    CHECK(msg != 0);
    CHECK_EQ(r.get32(msg + im_Class), IDCMP_GADGETUP);
    CHECK_EQ(r.get32(msg + im_IAddress), gad);
    reply_msg(r, msg);
}

// AllocRemember notes every block on the key's list; FreeRemember with
// reallyForget gives the blocks and the nodes back and empties the key.
void remember_lists_allocate_and_free() {
    GfxRig r;
    constexpr u32 kKey = kData + 0x800;
    r.put32(kKey, 0);
    const u32 before = r.exec.free_bytes();
    u32 blocks[3]{};
    for (u32 i = 0; i < 3; ++i) {
        r.cpu().a[0] = kKey;
        r.cpu().d[0] = 100 + i;
        r.cpu().d[1] = MEMF_CLEAR;
        blocks[i] = r.result(Base::Intuition, intuition_lvo::AllocRemember);
        CHECK(blocks[i] != 0);
    }
    const u32 head = r.get32(kKey);
    CHECK_EQ(r.get32(head + 8), blocks[2]);
    CHECK_EQ(r.get32(head + 4), 102u);
    CHECK_EQ(r.get32(r.get32(head) + 8), blocks[1]);
    CHECK(r.exec.free_bytes() < before);
    r.cpu().a[0] = kKey;
    r.cpu().d[0] = 1;
    r.result(Base::Intuition, intuition_lvo::FreeRemember);
    CHECK_EQ(r.get32(kKey), 0u);
    CHECK_EQ(r.exec.free_bytes(), before);
}

// The active window gets an INTUITICKS every tenth of a second when it
// asks for them, but never a second one before it replied to the first.
void intuiticks_reach_the_active_window_one_at_a_time() {
    GfxRig r;
    r.intuition.set_clock(fake_clock);
    g_fake_time_us = 10000000ULL;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, 0, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_INTUITICKS);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    r.intuition.pointer_event(80, 90, kMouseLeft, kMouseLeft, 0, false);
    r.intuition.pointer_event(80, 90, 0, kMouseLeft, 0, false);
    CHECK_EQ(r.intuition.active_window(), window);
    const u32 port = r.get32(window + wd_UserPort);

    g_fake_time_us += 200000;
    r.intuition.intui_ticks();
    const u32 first = get_msg(r, port);
    CHECK(first != 0);
    CHECK_EQ(r.get32(first + im_Class), IDCMP_INTUITICKS);

    // Unreplied: no second tick, however long it waits.
    g_fake_time_us += 200000;
    r.intuition.intui_ticks();
    CHECK_EQ(get_msg(r, port), 0u);

    // Replied: the next tenth of a second brings another, not sooner.
    reply_msg(r, first);
    g_fake_time_us += 50000;
    r.intuition.intui_ticks();
    CHECK_EQ(get_msg(r, port), 0u);
    g_fake_time_us += 100000;
    r.intuition.intui_ticks();
    const u32 second = get_msg(r, port);
    CHECK(second != 0);
    CHECK_EQ(r.get32(second + im_Class), IDCMP_INTUITICKS);
    reply_msg(r, second);
}

// The four WORDs a WA_Zoom tag points to.
constexpr u32 kZoomBox = kData + 0x2600;

void put_zoom_box(GfxRig& r, i32 left, i32 top, i32 width, i32 height) {
    r.put16(kZoomBox, static_cast<u32>(left));
    r.put16(kZoomBox + 2, static_cast<u32>(top));
    r.put16(kZoomBox + 4, static_cast<u32>(width));
    r.put16(kZoomBox + 6, static_cast<u32>(height));
}

// A 300 by 150 window at 100, 80 on a fresh 640 by 400 screen of depth
// planes, which can be sized from 100 by 40 up to the screen. WA_Zoom gives
// it a zoom gadget whose other box is the one put_zoom_box stored.
u32 open_zoom_window(GfxRig& r, u32 depth, u32 idcmp = 0, u32 title = 0,
                     u32 depth_gadget = 1) {
    put_new_screen(r, kNewScreen, 640, 400, depth, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);
    const u32 tags = r.tags(
        kTagList, {{WA_Left, 100}, {WA_Top, 80}, {WA_Width, 300}, {WA_Height, 150},
                   {WA_DepthGadget, depth_gadget}, {WA_CloseGadget, 1}, {WA_DragBar, 1},
                   {WA_Activate, 1}, {WA_IDCMP, idcmp}, {WA_Title, title}, {WA_MinWidth, 100},
                   {WA_MinHeight, 40}, {WA_MaxWidth, ~0u}, {WA_MaxHeight, ~0u},
                   {WA_Zoom, kZoomBox}, {TAG_DONE, 0}});
    const u32 window = open_win_tags(r, 0, tags);
    CHECK(window != 0);
    return window;
}

Rect zoom_button_of(GfxRig& r, u32 window) {
    return Intuition::zoom_box_rect(static_cast<i16>(r.get16(window + wd_LeftEdge)),
                                    static_cast<i16>(r.get16(window + wd_TopEdge)),
                                    static_cast<i16>(r.get16(window + wd_Width)),
                                    r.get32(window + wd_Flags));
}

// A click on the middle of the window's zoom button.
void click_zoom(GfxRig& r, u32 window) {
    const Rect box = zoom_button_of(r, window);
    press(r, (box.x0 + box.x1) / 2, (box.y0 + box.y1) / 2);
    release(r, (box.x0 + box.x1) / 2, (box.y0 + box.y1) / 2);
}

void check_window_box(GfxRig& r, u32 window, i32 left, i32 top, i32 width, i32 height) {
    CHECK_EQ(static_cast<i32>(r.get16(window + wd_LeftEdge)), left);
    CHECK_EQ(static_cast<i32>(r.get16(window + wd_TopEdge)), top);
    CHECK_EQ(static_cast<i32>(r.get16(window + wd_Width)), width);
    CHECK_EQ(static_cast<i32>(r.get16(window + wd_Height)), height);
}

// The classes queued on the window's port, in order, each message replied to.
std::vector<u32> drained_classes(GfxRig& r, u32 window) {
    std::vector<u32> messages;
    const std::vector<u32> classes = queued_classes(r, window, &messages);
    for (const u32 msg : messages) {
        reply_msg(r, msg);
    }
    return classes;
}

bool screen_has_pen(GfxRig& r, u32 screen, const Rect& area, u32 pen) {
    for (i32 y = area.y0; y <= area.y1; ++y) {
        for (i32 x = area.x0; x <= area.x1; ++x) {
            if (screen_pixel(r, screen, x, y) == pen) {
                return true;
            }
        }
    }
    return false;
}

void wa_zoom_gives_the_window_a_zoom_gadget_beside_the_depth_gadget() {
    GfxRig r;
    put_zoom_box(r, 20, 40, 120, 60);
    const u32 window = open_zoom_window(r, 2);
    const u32 flags = r.get32(window + wd_Flags);
    CHECK_EQ(flags & WFLG_HASZOOM, WFLG_HASZOOM);

    // As large as the depth button, ending in the depth button's first
    // column.
    const Rect zoom = Intuition::zoom_box_rect(100, 80, 300, flags);
    const Rect depth = Intuition::depth_box_rect(100, 80, 300);
    CHECK_EQ(zoom.x1, depth.x0);
    CHECK_EQ(zoom.x1 - zoom.x0, depth.x1 - depth.x0);
    CHECK_EQ(zoom.y0, depth.y0);
    CHECK_EQ(zoom.y1, depth.y1);
    // Without a depth gadget it takes the depth button's place.
    const Rect alone = Intuition::zoom_box_rect(100, 80, 300, WFLG_HASZOOM);
    CHECK_EQ(alone.x0, depth.x0);
    CHECK_EQ(alone.x1, depth.x1);
    CHECK_EQ(alone.y0, depth.y0);
    CHECK_EQ(alone.y1, depth.y1);
}

void a_window_without_a_depth_gadget_has_the_zoom_button_in_its_place() {
    GfxRig r;
    put_zoom_box(r, 20, 40, 120, 60);
    const u32 window = open_zoom_window(r, 2, IDCMP_NEWSIZE, 0, 0);
    const u32 flags = r.get32(window + wd_Flags);
    CHECK_EQ(flags & (WFLG_HASZOOM | WFLG_DEPTHGADGET), WFLG_HASZOOM);
    const Rect box = Intuition::zoom_box_rect(100, 80, 300, flags);
    const Rect depth = Intuition::depth_box_rect(100, 80, 300);
    CHECK_EQ(box.x0, depth.x0);
    CHECK_EQ(box.x1, depth.x1);
    // Its cell starts with the groove that ends the title and reaches the
    // window's dark right edge, which keeps the top right corner.
    const u32 screen = r.get32(window + wd_WScreen);
    const i32 right = 100 + 300 - 1;
    const i32 cy = (box.y0 + box.y1) / 2;
    CHECK_EQ(screen_pixel(r, screen, box.x0, cy), 1u);
    CHECK_EQ(screen_pixel(r, screen, box.x0 + 1, cy), 2u);
    CHECK_EQ(screen_pixel(r, screen, right - 1, cy), 3u);
    CHECK_EQ(screen_pixel(r, screen, right, cy), 1u);
    CHECK_EQ(screen_pixel(r, screen, right - 1, 80), 2u);
    CHECK_EQ(screen_pixel(r, screen, right, 80), 1u);
    click_zoom(r, window);
    check_window_box(r, window, 20, 40, 120, 60);
}

void a_window_without_wa_zoom_has_no_zoom_gadget() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    // The flag is Intuition's to set: the NewWindow's is dropped.
    put_new_window(r, kNewWindow, 100, 80, 300, 150,
                   WFLG_ACTIVATE | WFLG_DEPTHGADGET | WFLG_HASZOOM, 0, 0, 0);
    r.put32(kNewWindow + nw_IDCMPFlags, IDCMP_NEWSIZE | IDCMP_CHANGEWINDOW | IDCMP_MOUSEBUTTONS);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.get32(window + wd_Flags) & WFLG_HASZOOM, 0u);

    // Nothing is drawn where the zoom button would be, and a click there
    // is the window's like any other.
    const Rect box = Intuition::zoom_box_rect(100, 80, 300, WFLG_DEPTHGADGET | WFLG_HASZOOM);
    const i32 cx = (box.x0 + box.x1) / 2;
    const i32 cy = (box.y0 + box.y1) / 2;
    CHECK_EQ(screen_pixel(r, screen, cx - 1, cy - 1), 3u);
    press(r, cx, cy);
    release(r, cx, cy);
    const std::vector<u32> mouse{IDCMP_MOUSEBUTTONS, IDCMP_MOUSEBUTTONS};
    CHECK(drained_classes(r, window) == mouse);
    check_window_box(r, window, 100, 80, 300, 150);

    // ZipWindow has no other box to take it to.
    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::ZipWindow), ExitReason::Halt);
    check_window_box(r, window, 100, 80, 300, 150);
    CHECK_EQ(drained_classes(r, window).size(), 0u);

    // A WA_Zoom without a box gives no gadget either.
    const u32 tags = r.tags(kTagList, {{WA_Left, 20}, {WA_Top, 20}, {WA_Width, 100},
                                       {WA_Height, 60}, {WA_Zoom, 0}, {TAG_DONE, 0}});
    const u32 plain = open_win_tags(r, 0, tags);
    CHECK(plain != 0);
    CHECK_EQ(r.get32(plain + wd_Flags) & WFLG_HASZOOM, 0u);
}

void the_classic_zoom_button_sits_left_of_the_depth_button() {
    GfxRig r;
    put_zoom_box(r, 20, 40, 120, 60);
    const u32 window = open_zoom_window(r, 2);
    const u32 screen = r.get32(window + wd_WScreen);
    const Rect zoom = zoom_button_of(r, window);
    const Rect depth = Intuition::depth_box_rect(100, 80, 300);
    const i32 cx = (zoom.x0 + zoom.x1) / 2;
    const i32 cy = (zoom.y0 + zoom.y1) / 2;

    // A raised cell as the depth button's is: the window's light edge along
    // the top and its dark line under the bar, a groove of a dark and a
    // light line on its left, and on its right the one it shares with the
    // depth button.
    const i32 top = 80;
    const i32 under = top + chrome::Metrics{}.bar_height();
    CHECK_EQ(screen_pixel(r, screen, cx, top), 2u);
    CHECK_EQ(screen_pixel(r, screen, cx, under), 1u);
    CHECK_EQ(screen_pixel(r, screen, zoom.x0 - 2, cy), 3u);
    CHECK_EQ(screen_pixel(r, screen, zoom.x0 - 1, cy), 1u);
    CHECK_EQ(screen_pixel(r, screen, zoom.x0, cy), 2u);
    CHECK_EQ(screen_pixel(r, screen, zoom.x0 + 1, cy), 3u);
    CHECK_EQ(zoom.x1, depth.x0);
    CHECK_EQ(screen_pixel(r, screen, zoom.x1 - 1, cy), 3u);
    CHECK_EQ(screen_pixel(r, screen, zoom.x1, cy), 1u);
    CHECK_EQ(screen_pixel(r, screen, zoom.x1 + 1, cy), 2u);
    // A square etched into the bar's FILLPEN (3), dark at the top left and
    // light at the bottom right, with a small light sheet outlined in the
    // dark pen in its top left corner.
    CHECK_EQ(screen_pixel(r, screen, cx - 3, cy - 3), 1u);
    CHECK_EQ(screen_pixel(r, screen, cx - 1, cy - 1), 2u);
    CHECK_EQ(screen_pixel(r, screen, cx, cy), 1u);
    CHECK_EQ(screen_pixel(r, screen, cx + 2, cy + 2), 3u);
    CHECK_EQ(screen_pixel(r, screen, cx + 3, cy + 3), 2u);
    // The depth button keeps the window's dark right edge.
    CHECK_EQ(screen_pixel(r, screen, 100 + 300 - 1, cy), 1u);
}

void the_modern_zoom_button_sits_left_of_the_depth_button() {
    GfxRig r;
    put_zoom_box(r, 20, 40, 120, 60);
    const u32 window = open_zoom_window(r, 8);
    const u32 screen = r.get32(window + wd_WScreen);
    const Rect zoom = zoom_button_of(r, window);
    const Rect depth = Intuition::depth_box_rect(100, 80, 300);
    const i32 cx = (zoom.x0 + zoom.x1) / 2;
    const i32 cy = (zoom.y0 + zoom.y1) / 2;

    // Its own shaded body between outlines, the right one being the depth
    // button's left.
    CHECK_EQ(zoom.x1, depth.x0);
    CHECK_EQ(screen_pixel(r, screen, zoom.x0, cy), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, zoom.x1, cy), static_cast<u32>(chrome::kFrameDark));
    CHECK(knob_shade(screen_pixel(r, screen, zoom.x0 + 1, cy)));
    CHECK(knob_shade(screen_pixel(r, screen, depth.x0 + 1, cy)));
    // A shaded sheet with a light one in its top left corner, both outlined.
    CHECK_EQ(screen_pixel(r, screen, cx - 3, cy - 3), static_cast<u32>(chrome::kFrameDark));
    CHECK_EQ(screen_pixel(r, screen, cx - 2, cy - 2), static_cast<u32>(chrome::kFrameLight));
    CHECK_EQ(screen_pixel(r, screen, cx - 1, cy - 1), static_cast<u32>(chrome::kFrameLight));
    CHECK_EQ(screen_pixel(r, screen, cx + 2, cy + 2), static_cast<u32>(chrome::kKnob2));
    CHECK_EQ(screen_pixel(r, screen, cx + 3, cy + 3), static_cast<u32>(chrome::kFrameDark));
}

void the_title_ends_before_the_zoom_button() {
    for (const u32 depth : {2u, 8u}) {
        GfxRig r;
        put_zoom_box(r, 20, 40, 120, 60);
        r.str(kWinTitle, "A title much too long to fit between the bar buttons");
        const u32 window = open_zoom_window(r, depth, 0, kWinTitle);
        const u32 screen = r.get32(window + wd_WScreen);
        const u32 flags = r.get32(window + wd_Flags);
        const Rect zoom = zoom_button_of(r, window);
        const i32 first = 100 + chrome::title_left(flags);
        const i32 last = 100 + chrome::title_right(300, flags);
        CHECK_EQ(last, zoom.x0 - chrome::kTitleGapRight);
        // The title's ink: FILLTEXTPEN on the classic screen.
        const u32 ink = depth == 2 ? 1u : static_cast<u32>(chrome::kTitleActive);
        CHECK(screen_has_pen(r, screen, Rect{first, 83, last, 90}, ink));
        // On the classic screen the column before the button is the dark
        // line that ends the title, in the pen of the ink.
        const i32 gap_end = depth == 2 ? zoom.x0 - 2 : zoom.x0 - 1;
        CHECK(!screen_has_pen(r, screen, Rect{last + 1, 83, gap_end, 90}, ink));
        if (depth == 2) {
            CHECK_EQ(screen_pixel(r, screen, zoom.x0 - 1, 83), 1u);
        }
    }
}

void a_click_on_the_zoom_button_swaps_the_box_both_ways() {
    GfxRig r;
    put_zoom_box(r, 20, 40, 120, 60);
    const u32 window = open_zoom_window(
        r, 2, IDCMP_NEWSIZE | IDCMP_CHANGEWINDOW | IDCMP_MOUSEBUTTONS);
    const u32 layer = r.get32(window + wd_WLayer);
    const std::vector<u32> resized{IDCMP_NEWSIZE, IDCMP_CHANGEWINDOW};

    // The press and the release are the gadget's: the window gets no
    // MOUSEBUTTONS for them.
    click_zoom(r, window);
    check_window_box(r, window, 20, 40, 120, 60);
    CHECK_EQ(r.get16(layer + ly_MinX), 20u);
    CHECK_EQ(r.get16(layer + ly_MinY), 40u);
    CHECK_EQ(r.get16(layer + ly_MaxX), 139u);
    CHECK_EQ(r.get16(layer + ly_MaxY), 99u);
    CHECK(drained_classes(r, window) == resized);

    // The box it had is the other one now.
    click_zoom(r, window);
    check_window_box(r, window, 100, 80, 300, 150);
    CHECK_EQ(r.get16(layer + ly_MinX), 100u);
    CHECK_EQ(r.get16(layer + ly_MaxX), 399u);
    CHECK_EQ(r.get16(layer + ly_MaxY), 229u);
    CHECK(drained_classes(r, window) == resized);

    click_zoom(r, window);
    check_window_box(r, window, 20, 40, 120, 60);
    CHECK(drained_classes(r, window) == resized);
}

void a_zoom_that_only_moves_the_window_posts_changewindow_alone() {
    GfxRig r;
    put_zoom_box(r, 300, 200, 300, 150);
    const u32 window = open_zoom_window(r, 2, IDCMP_NEWSIZE | IDCMP_CHANGEWINDOW);
    click_zoom(r, window);
    check_window_box(r, window, 300, 200, 300, 150);
    const std::vector<u32> moved{IDCMP_CHANGEWINDOW};
    CHECK(drained_classes(r, window) == moved);
}

void a_left_and_top_of_minus_one_keep_the_place() {
    {
        GfxRig r;
        put_zoom_box(r, -1, -1, 150, 70);
        const u32 window = open_zoom_window(r, 2, IDCMP_NEWSIZE | IDCMP_CHANGEWINDOW);
        click_zoom(r, window);
        check_window_box(r, window, 100, 80, 150, 70);
        click_zoom(r, window);
        check_window_box(r, window, 100, 80, 300, 150);
    }
    {
        // The place it keeps is the one the window is at when it zooms.
        GfxRig r;
        put_zoom_box(r, -1, -1, 150, 70);
        const u32 window = open_zoom_window(r, 2, IDCMP_NEWSIZE | IDCMP_CHANGEWINDOW);
        r.intuition.move_window_to(window, 200, 120);
        click_zoom(r, window);
        check_window_box(r, window, 200, 120, 150, 70);
        click_zoom(r, window);
        check_window_box(r, window, 200, 120, 300, 150);
    }
}

void the_other_box_is_held_to_the_screen_and_to_the_limits() {
    {
        GfxRig r;
        put_zoom_box(r, 600, 380, 200, 100);
        const u32 window = open_zoom_window(r, 2);
        click_zoom(r, window);
        check_window_box(r, window, 440, 300, 200, 100);
    }
    {
        GfxRig r;
        put_zoom_box(r, 20, 40, 50, 10);
        const u32 window = open_zoom_window(r, 2);
        click_zoom(r, window);
        check_window_box(r, window, 20, 40, 100, 40);
    }
    {
        GfxRig r;
        put_zoom_box(r, 20, 40, 700, 500);
        const u32 window = open_zoom_window(r, 2);
        click_zoom(r, window);
        check_window_box(r, window, 0, 0, 640, 400);
    }
}

void a_held_zoom_button_sinks_on_a_four_colour_screen() {
    GfxRig r;
    put_zoom_box(r, 20, 40, 120, 60);
    const u32 window = open_zoom_window(r, 2, IDCMP_NEWSIZE | IDCMP_CHANGEWINDOW);
    const u32 screen = r.get32(window + wd_WScreen);
    const Rect zoom = zoom_button_of(r, window);
    const Rect depth = Intuition::depth_box_rect(100, 80, 300);
    const i32 cx = (zoom.x0 + zoom.x1) / 2;
    const i32 cy = (zoom.y0 + zoom.y1) / 2;

    const i32 right = 100 + 300 - 1;
    // Raised: light on the left, after the dark line that ends the title,
    // and on the right the dark line that starts the depth button.
    CHECK_EQ(screen_pixel(r, screen, zoom.x0 - 1, cy), 1u);
    CHECK_EQ(screen_pixel(r, screen, zoom.x0, cy), 2u);
    CHECK_EQ(screen_pixel(r, screen, zoom.x0 + 1, cy), 3u);
    CHECK_EQ(screen_pixel(r, screen, zoom.x1, cy), 1u);
    press(r, cx, cy);
    // Held, it is sunken: dark on the left, after the dark line that ends
    // the title, and light on the right, over the line that starts the
    // depth button, which stays raised.
    CHECK_EQ(screen_pixel(r, screen, zoom.x0 - 1, cy), 1u);
    CHECK_EQ(screen_pixel(r, screen, zoom.x0, cy), 1u);
    CHECK_EQ(screen_pixel(r, screen, zoom.x0 + 1, cy), 3u);
    CHECK_EQ(screen_pixel(r, screen, zoom.x1, cy), 2u);
    CHECK_EQ(screen_pixel(r, screen, zoom.x1 - 1, cy), 3u);
    CHECK_EQ(zoom.x1, depth.x0);
    CHECK_EQ(screen_pixel(r, screen, depth.x0 + 1, cy), 2u);
    CHECK_EQ(screen_pixel(r, screen, right, cy), 1u);

    // Off the button it rises, and let go there it does nothing.
    move_to(r, cx, zoom.y1 + 40);
    CHECK_EQ(screen_pixel(r, screen, zoom.x0, cy), 2u);
    CHECK_EQ(screen_pixel(r, screen, zoom.x1, cy), 1u);
    release(r, cx, zoom.y1 + 40);
    check_window_box(r, window, 100, 80, 300, 150);
    CHECK_EQ(drained_classes(r, window).size(), 0u);

    // Let go over it, the window zooms and the button rises with it.
    press(r, cx, cy);
    release(r, cx, cy);
    check_window_box(r, window, 20, 40, 120, 60);
    const Rect zoomed = zoom_button_of(r, window);
    CHECK_EQ(screen_pixel(r, screen, zoomed.x0 + 1, (zoomed.y0 + zoomed.y1) / 2), 3u);
}

void a_held_zoom_button_shows_pressed_on_a_modern_screen() {
    GfxRig r;
    put_zoom_box(r, 20, 40, 120, 60);
    const u32 window = open_zoom_window(r, 8);
    const u32 screen = r.get32(window + wd_WScreen);
    const Rect zoom = zoom_button_of(r, window);
    const Rect depth = Intuition::depth_box_rect(100, 80, 300);
    const i32 cx = (zoom.x0 + zoom.x1) / 2;
    const i32 cy = (zoom.y0 + zoom.y1) / 2;

    CHECK(knob_shade(screen_pixel(r, screen, zoom.x0 + 1, cy)));
    press(r, cx, cy);
    CHECK(held_shade(screen_pixel(r, screen, zoom.x0 + 1, cy)));
    // The sheet's outline turns light and lets the body show through.
    CHECK_EQ(screen_pixel(r, screen, cx + 3, cy + 3), static_cast<u32>(chrome::kFrameLight));
    CHECK(held_shade(screen_pixel(r, screen, cx + 2, cy + 2)));
    CHECK(knob_shade(screen_pixel(r, screen, depth.x0 + 1, cy)));
    // Let go off the button, it rises and the window stays.
    move_to(r, cx, zoom.y1 + 40);
    release(r, cx, zoom.y1 + 40);
    CHECK(knob_shade(screen_pixel(r, screen, zoom.x0 + 1, cy)));
    check_window_box(r, window, 100, 80, 300, 150);
}

void zipwindow_and_changewindowbox_move_and_size_through_the_vectors() {
    GfxRig r;
    CHECK_EQ(intuition_lvo::MoveWindow, 168u);
    CHECK_EQ(intuition_lvo::SizeWindow, 288u);
    CHECK_EQ(intuition_lvo::ChangeWindowBox, 486u);
    CHECK_EQ(intuition_lvo::ZipWindow, 504u);
    put_zoom_box(r, 20, 40, 120, 60);
    const u32 window = open_zoom_window(r, 2, IDCMP_NEWSIZE | IDCMP_CHANGEWINDOW);
    const std::vector<u32> resized{IDCMP_NEWSIZE, IDCMP_CHANGEWINDOW};
    const std::vector<u32> moved{IDCMP_CHANGEWINDOW};

    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::ZipWindow), ExitReason::Halt);
    check_window_box(r, window, 20, 40, 120, 60);
    CHECK(drained_classes(r, window) == resized);
    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::ZipWindow), ExitReason::Halt);
    check_window_box(r, window, 100, 80, 300, 150);
    CHECK(drained_classes(r, window) == resized);

    auto change = [&](i32 left, i32 top, i32 width, i32 height) {
        r.cpu().a[0] = window;
        r.cpu().d[0] = static_cast<u32>(left);
        r.cpu().d[1] = static_cast<u32>(top);
        r.cpu().d[2] = static_cast<u32>(width);
        r.cpu().d[3] = static_cast<u32>(height);
        CHECK_EQ(r.call(Base::Intuition, intuition_lvo::ChangeWindowBox), ExitReason::Halt);
    };
    change(10, 20, 200, 90);
    check_window_box(r, window, 10, 20, 200, 90);
    CHECK(drained_classes(r, window) == resized);
    // A move alone, and a call that changes nothing.
    change(30, 50, 200, 90);
    check_window_box(r, window, 30, 50, 200, 90);
    CHECK(drained_classes(r, window) == moved);
    change(30, 50, 200, 90);
    CHECK_EQ(drained_classes(r, window).size(), 0u);
    // A box that moves and grows at once, and one that moves and shrinks,
    // ends where it was asked to, held to the screen.
    change(600, 380, 300, 100);
    check_window_box(r, window, 340, 300, 300, 100);
    CHECK(drained_classes(r, window) == resized);
    change(0, 0, 600, 380);
    check_window_box(r, window, 0, 0, 600, 380);
    CHECK(drained_classes(r, window) == resized);
    change(500, 300, 100, 60);
    check_window_box(r, window, 500, 300, 100, 60);
    CHECK(drained_classes(r, window) == resized);

    change(100, 100, 200, 90);
    drained_classes(r, window);
    r.cpu().a[0] = window;
    r.cpu().d[0] = 10;
    r.cpu().d[1] = static_cast<u32>(-4);
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::MoveWindow), ExitReason::Halt);
    check_window_box(r, window, 110, 96, 200, 90);
    CHECK(drained_classes(r, window) == moved);
    r.cpu().a[0] = window;
    r.cpu().d[0] = 20;
    r.cpu().d[1] = static_cast<u32>(-10);
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::SizeWindow), ExitReason::Halt);
    check_window_box(r, window, 110, 96, 220, 80);
    CHECK(drained_classes(r, window) == resized);
}

void sizing_by_zoom_leaves_a_pending_newsize_alone_until_it_is_replied_to() {
    GfxRig r;
    put_zoom_box(r, 20, 40, 120, 60);
    const u32 window = open_zoom_window(r, 2, IDCMP_NEWSIZE | IDCMP_CHANGEWINDOW);
    click_zoom(r, window);
    std::vector<u32> messages;
    CHECK_EQ(queued_classes(r, window, &messages).size(), 2u);
    // The program has not replied to the NEWSIZE when the window zooms back:
    // the reply brings the one the new size needs.
    click_zoom(r, window);
    const std::vector<u32> moved{IDCMP_CHANGEWINDOW};
    CHECK(queued_classes(r, window) == moved);
    for (const u32 msg : messages) {
        reply_msg(r, msg);
    }
    const std::vector<u32> again{IDCMP_NEWSIZE};
    CHECK(queued_classes(r, window) == again);
}

int main() {
    intuiticks_reach_the_active_window_one_at_a_time();
    a_string_gadget_measures_its_own_font();
    a_string_gadget_is_edited_from_the_keyboard();
    a_long_int_gadget_takes_only_numbers_and_keeps_the_value();
    intuitext_is_measured_and_printed_in_its_font();
    intuitext_borders_and_blocking_requests();
    boopsi_system_classes_and_gadget_objects();
    a_class_in_68000_code_reaches_its_superclass();
    a_boopsi_gadget_draws_itself_and_takes_the_click();
    a_boopsi_notify_posts_idcmpupdate();
    a_boopsi_notify_without_idcmpupdate_is_dropped();
    a_boopsi_notify_chains_through_an_object_target();
    sysiclass_sizes_and_lifecycle();
    draw_image_state_draws_a_sysiclass_image();
    border_images_carry_the_frame_lines_they_cover();
    screen_font_is_a_textattr_naming_topaz();
    open_screen_builds_struct_bitmap_and_colormap();
    open_screen_clears_and_draws_title_bar();
    open_screen_makes_it_front_and_refreshes_the_whole_screen();
    open_screen_taglist_reads_sa_tags();
    open_screen_view_modes_choose_the_default_monitor_mode();
    open_screen_standard_size_without_view_modes_keeps_workbench_mode();
    open_screen_display_id_gives_the_modes_size();
    open_screen_display_id_of_a_super_hires_mode();
    display_mode_geometry_follows_the_front_screen();
    open_screen_dclip_sets_the_standard_size();
    open_screen_too_deep_or_unknown_mode_fails_with_an_error_code();
    open_screen_null_newscreen_and_taglist_gives_zero_and_faults();
    open_screen_out_of_memory_returns_zero_and_frees();
    open_screen_limit_returns_zero();
    more_than_four_screens_open_in_depth_order();
    close_screen_twice_and_null_are_ignored();
    close_front_screen_promotes_next();
    reopen_after_close_reuses_memory();
    get_screen_data_copies_struct();
    open_screen_honours_depth_and_allocates_planes();
    open_screen_depth8_sets_default_and_chrome_palettes();
    unimplemented_vector_ends_run();
    open_window_builds_struct_rastport_and_layer();
    a_left_border_gadget_widens_the_window_border();
    open_window_draws_chrome();
    open_window_default_fills_the_screen_below_the_bar();
    windows_open_below_the_screen_bar();
    open_window_taglist_reads_wa_tags();
    open_window_on_unknown_screen_gives_zero();
    clip_blit_uses_window_coordinates();
    open_window_null_newwindow_gives_zero_and_faults();
    open_window_alloc_failure_gives_zero_and_frees();
    open_window_limit_gives_zero();
    more_than_sixteen_windows_open_and_close_in_any_order();
    window_text_goes_into_the_window_only();
    text_longer_than_window_is_clipped();
    close_window_repaints_and_unlinks();
    close_window_twice_and_null_ignored();
    set_window_titles_redraws_title();
    refresh_window_frame_redraws_the_border_only();
    window_refresh_reaches_the_sink();
    open_window_reads_32_bit_flags();
    window_open_is_true_until_closed();
    close_screen_with_open_windows_returns_false_and_keeps_both();
    close_screen_after_windows_returns_true();
    close_screen_zero_and_unknown_return_false();
    guest_cannot_redirect_frees_on_close_screen();
    guest_cannot_redirect_frees_on_close_window();
    window_layer_is_the_outer_rectangle();
    window_rastport_origin_is_the_window_corner();
    window_text_below_the_border();
    window_drawing_is_clipped_to_the_window();
    window_without_idcmp_has_no_userport();
    window_with_idcmp_gets_a_userport_and_flags();
    wa_idcmp_tag_overrides_newwindow_flags();
    modify_idcmp_creates_and_deletes_the_port();
    modify_idcmp_zero_flushes_queued_messages();
    close_window_frees_queued_messages_and_the_port_and_balances_memory();
    reply_msg_frees_the_intuimessage_and_ignores_forged_addresses();
    mouse_button_goes_to_the_window_under_the_pointer_with_relative_coordinates();
    mouse_button_ignored_without_idcmp_flag();
    left_press_activates_the_window_and_posts_active_and_inactive();
    move_goes_to_the_active_window_only_with_reportmouse();
    report_mouse_turns_move_reports_on_and_off();
    a_drag_image_is_drawn_above_the_windows_until_cleared();
    close_gadget_posts_closewindow_on_release_over_the_box();
    close_gadget_release_elsewhere_does_nothing();
    close_gadget_without_idcmp_does_nothing();
    depth_gadget_toggles_front_and_back();
    gadget_press_is_consumed_without_a_mousebuttons_message();
    hit_rectangles_hold_the_drawn_glyphs();
    messages_carry_the_clock_time();
    full_message_table_drops_events_without_leaking();
    events_for_windows_of_other_screens_are_ignored();
    openwindow_with_activate_flag_activates();
    shared_userport_is_adopted_and_not_deleted();
    shared_userport_gets_messages_for_the_window();
    shared_userport_close_removes_only_that_windows_messages();
    shared_userport_modify_idcmp_zero_leaves_the_program_port();
    buttons_go_to_the_active_window_not_the_one_under_the_pointer();
    drag_release_outside_still_reaches_the_active_window();
    press_in_inactive_window_activates_then_delivers_selectdown();
    menu_and_middle_buttons_go_to_the_active_window();
    reclaim_closes_windows_and_screens_but_keeps_the_given_ones();
    reclaim_frees_delivered_unreplied_messages_and_balances_memory();
    reclaim_clears_a_closed_active_window();
    reclaim_restores_the_front_screen();
    reclaim_owned_by_closes_only_that_owners_windows_screens_and_messages();
    window_draws_into_private_bitmap();
    rectfill_in_window_shows_on_screen();
    upper_window_covers_lower_after_draw();
    close_reveals_lower_window();
    depth_gadget_repaints();
    screen_bar_survives_window_close();
    batch_composes_once();
    size_gadget_sets_right_border();
    size_bbottom_sets_bottom_border();
    limits_zero_mean_initial();
    limits_ffff_mean_screen();
    limits_clamp_to_floor();
    tags_set_gadgets_and_limits();
    drag_moves_window();
    drag_clamps_to_screen();
    drag_under_other_window_keeps_order();
    resize_grows_and_clears();
    resize_clamps_to_limits();
    window_limits_set_and_report_limits();
    remember_lists_allocate_and_free();
    newsize_coalesced_and_final_on_release();
    unreplied_resizes_do_not_exhaust_messages();
    move_posts_changewindow_only();
    drag_press_is_not_selectdown();
    size_hook_called_per_step();
    close_during_drag_ends_drag();
    open_window_links_and_draws_prop_gadget();
    a_boolean_gadget_reports_its_release();
    a_gadtools_gadget_is_drawn_by_its_renderer();
    inner_size_tags_size_the_window_around_its_inside();
    wa_gadgets_tag_links_list();
    add_and_remove_glist_positions();
    new_modify_prop_moves_knob();
    relative_geometry_follows_resize();
    knob_drag_updates_pot_and_hook();
    container_click_pages_by_body();
    gadget_idcmp_down_move_up();
    disabled_gadget_ignores_input();
    a_boolean_gadget_takes_the_click_from_the_window();
    gadget_list_cycle_is_bounded();
    close_during_knob_drag();
    wheel_event_posts_newmouse_rawkey();
    raw_key_posts_rawkey_with_code_and_qualifier();
    raw_key_posts_vanillakey_characters();
    a_window_with_both_key_flags_gets_rawkey();
    a_window_without_key_flags_gets_no_keys();
    mouse_buttons_carry_the_keyboard_qualifier();
    gadget_lvos_from_guest_calls();
    modify_prop_ignores_non_prop_gadget();
    modify_prop_keeps_knobhit_during_drag();
    remove_glist_clears_hook();
    border_sizes_with_without_size_gadget_and_borderless();
    close_depth_size_hit_at_new_positions();
    depth8_chrome_pixel_spot_checks();
    depth8_prop_gadget_chrome_pixels();
    depth2_fallback_chrome_pens();
    the_look_follows_the_number_of_planes();
    classic_chrome_follows_the_draw_info_or_else_the_pen_prefs();
    classic_borders_change_colour_with_the_activation();
    classic_knob_takes_the_fill_pen_while_dragged();
    activation_switch_redraws_both_bars();
    bar_buttons_are_centred_and_mirror_each_other();
    held_bar_button_shows_pressed_while_the_pointer_is_over_it();
    held_bar_button_sinks_on_a_four_colour_screen();
    rounded_top_corners_on_depth8_show_backdrop_and_chrome();
    drop_shadow_darkens_backdrop_and_behind_window_only();
    moving_and_closing_window_removes_old_shadow();
    chrome_pens_find_their_place_on_every_depth();
    shallow_modern_screens_draw_the_window_chrome_in_the_mapped_pens();
    shallow_modern_screens_draw_prop_gadgets_in_the_mapped_pens();
    shallow_modern_screens_hold_the_chrome_colours_at_the_mapped_pens();
    shallow_modern_screens_cast_shadows_in_their_own_palette();
    classic_arrow_buttons_are_the_system_images();
    shallow_modern_arrow_buttons_use_the_mapped_pens();
    depth2_screen_has_no_corners_and_no_shadow();
    shade_table_darkens_default_pen0_and_updates_on_setrgb32();
    flush_palette_recomposes_8plane_shadows_and_end_batch_flushes();
    backdrop_window_has_square_corners_no_shadow_and_stays_behind();
    size_box_hit_test_matches_border_placement();
    lock_pub_screen_returns_the_default_screen();
    get_default_pub_screen_writes_workbench();
    get_screen_draw_info_has_the_default_pens_and_topaz();
    set_iprefs_type8_writes_the_palette_into_the_default_screen();
    set_iprefs_type7_takes_the_pointer_image_and_hands_back_the_previous_bitmap();
    set_iprefs_type7_refuses_a_record_without_an_image();
    set_iprefs_type8_pens_8_to_10_are_the_pointer_colours();
    set_iprefs_type9_and_apply_pen_prefs_rewrite_the_draw_info();
    set_iprefs_type2_sets_the_system_and_screen_fonts();
    screen_with_sa_font_sizes_its_bars_by_the_font();
    screen_takes_the_font_of_its_newscreen();
    sa_sysfont_picks_the_screen_font_or_the_default_font();
    default_screen_follows_the_screen_font_of_the_prefs();
    menu_bar_and_boxes_follow_the_screen_font();
    close_and_open_workbench_follow_the_windows_and_the_pens();
    set_iprefs_type_1_keeps_the_screen_mode_record();
    close_workbench_does_not_wait_for_a_window_marked_to_stay();
    open_workbench_rebuilds_the_screen_for_the_mode_of_the_prefs();
    open_workbench_fits_its_windows_to_a_smaller_screen();
    open_workbench_lets_a_size_hook_close_a_window();
    open_workbench_keeps_a_screen_that_has_the_mode_of_the_prefs();
    open_workbench_keeps_the_screen_for_a_mode_it_cannot_have();
    open_workbench_keeps_the_screen_when_memory_runs_out();
    shallow_screens_draw_the_chrome_in_the_classic_pens();
    chrome_palette_follows_the_default_screen_pens();
    complement_mirrors_follow_the_low_pens();
    menu_strip_vectors_write_the_window_field();
    item_address_follows_the_menu_numbers();
    menu_right_press_opens_the_bar_and_the_second_item_picks();
    menu_release_over_nothing_posts_menunull();
    menu_disabled_item_posts_menunull();
    menu_checkit_with_menutoggle_toggles_checked();
    menu_mutual_exclude_clears_the_other_item();
    menu_subitem_pick_carries_the_subitem_number();
    menu_moving_off_the_box_clears_the_item_but_keeps_the_title();
    menu_disabled_title_never_opens();
    classic_menu_boxes_are_framed_square_and_cast_no_shadow();
    menu_on_a_chrome_screen_highlights_ghosts_and_restores();
    menu_on_a_shallow_modern_screen_highlights_ghosts_and_restores();
    menu_rmbtrap_window_still_gets_menudown_and_menuup();
    menu_mode_ends_without_a_pick_when_the_strip_or_window_goes();
    open_window_logs_the_title();
    draw_image_reads_plane_pick_and_plane_on_off();
    erase_image_clears_the_image_box();
    draw_image_takes_word_offsets_and_follows_the_chain();
    draw_image_leaves_out_what_it_cannot_draw();
    draw_image_complemented_inverts_the_pens_in_the_image_planes();
    draw_image_clips_at_the_window_edge();
    requester_builds_on_the_default_screen_with_its_title_and_buttons();
    requester_is_laid_out_in_the_font_of_its_screen();
    requester_handler_without_input_returns_minus_two();
    requester_buttons_answer_with_their_numbers();
    requester_release_outside_raises_the_button_again();
    requester_buttons_on_a_shallow_modern_screen_use_the_mapped_pens();
    requester_caller_idcmp_class_returns_minus_one();
    requester_title_falls_back_to_the_window_then_the_system();
    requester_formats_text_then_gadgets_from_one_stream();
    free_sys_request_ignores_zero_and_one();
    more_than_eight_requesters_are_kept_at_once();
    making_intuition_again_clears_everything_it_kept();
    requester_memory_comes_back_after_easy_request();
    easy_request_ends_on_the_classes_its_idcmp_pointer_holds();
    auto_request_answers_with_the_button_that_was_chosen();
    auto_request_without_a_positive_button_only_has_the_negative();
    auto_request_uses_the_windows_screen_and_the_default_one();
    auto_request_answers_by_the_flag_class_that_came();
    wa_zoom_gives_the_window_a_zoom_gadget_beside_the_depth_gadget();
    a_window_without_a_depth_gadget_has_the_zoom_button_in_its_place();
    a_window_without_wa_zoom_has_no_zoom_gadget();
    the_classic_zoom_button_sits_left_of_the_depth_button();
    the_modern_zoom_button_sits_left_of_the_depth_button();
    the_title_ends_before_the_zoom_button();
    a_click_on_the_zoom_button_swaps_the_box_both_ways();
    a_zoom_that_only_moves_the_window_posts_changewindow_alone();
    a_left_and_top_of_minus_one_keep_the_place();
    the_other_box_is_held_to_the_screen_and_to_the_limits();
    a_held_zoom_button_sinks_on_a_four_colour_screen();
    a_held_zoom_button_shows_pressed_on_a_modern_screen();
    zipwindow_and_changewindowbox_move_and_size_through_the_vectors();
    sizing_by_zoom_leaves_a_pending_newsize_alone_until_it_is_replied_to();
    return check::finish("test_intuition");
}
