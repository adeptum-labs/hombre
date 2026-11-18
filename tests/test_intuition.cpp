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

namespace {

constexpr u32 kNewScreen = kData;
constexpr u32 kTagList = kData + 0x100;
constexpr u32 kTitle = kData + 0x200;
constexpr u32 kBuffer = kData + 0x300;
constexpr u32 kNewWindow = kData + 0x500;
constexpr u32 kWinTitle = kData + 0x600;

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
    CHECK_EQ(r.get8(screen + sc_BarHeight), 10u);
    CHECK_EQ(r.get8(screen + sc_WBorTop), 11u);
    CHECK_EQ(r.get8(screen + sc_WBorLeft), 1u);
    CHECK_EQ(r.get8(screen + sc_WBorRight), 1u);
    CHECK_EQ(r.get8(screen + sc_WBorBottom), 1u);
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
    CHECK_EQ(r.get16(map + cm_Count), 4u);
    const u32 colors = r.get32(map + cm_ColorTable);
    CHECK(colors != 0);
    CHECK_EQ(r.get16(colors), 0x0AAAu);
    CHECK_EQ(r.get16(colors + 2), 0x0000u);
    CHECK_EQ(r.get16(colors + 4), 0x0FFFu);
    CHECK_EQ(r.get16(colors + 6), 0x068Bu);

    CHECK_EQ(r.intuition.front_screen(), screen);
    CHECK_EQ(r.display.front_screen, screen);
}

void open_screen_clears_and_draws_title_bar() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    CHECK_EQ(screen_pixel(r, screen, 5, 5), 2u);
    CHECK_EQ(screen_pixel(r, screen, 0, 0), 2u);
    CHECK_EQ(screen_pixel(r, screen, 639, 9), 2u);
    CHECK_EQ(screen_pixel(r, screen, 0, 10), 1u);
    CHECK_EQ(screen_pixel(r, screen, 5, 10), 1u);
    CHECK_EQ(screen_pixel(r, screen, 639, 10), 1u);
    CHECK_EQ(screen_pixel(r, screen, 5, 11), 0u);
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

    // The title is drawn at pen 1 over the bar, which stays pen 2.
    CHECK_EQ(screen_pixel(r, screen, 1, 1), 1u);
    CHECK_EQ(screen_pixel(r, screen, 2, 1), 1u);
    CHECK_EQ(screen_pixel(r, screen, 3, 1), 2u);
    CHECK(refreshed(r, screen, 0, 0, 319, 199));

    // A null NewScreen takes the tags and the defaults.
    const u32 short_tags = r.tags(kTagList, {{SA_Width, 100}, {SA_Height, 50}, {TAG_DONE, 0}});
    const u32 second = open_tags(r, 0, short_tags);
    CHECK(second != 0);
    CHECK_EQ(r.get16(second + sc_Width), 100u);
    CHECK_EQ(r.get16(second + sc_Height), 50u);
    CHECK_EQ(r.get8(second + sc_BitMap + bm_Depth), 2u);
    CHECK_EQ(r.display.front_screen, second);
}

void open_screen_null_newscreen_and_taglist_gives_zero_and_faults() {
    GfxRig r;
    r.cpu().a[0] = 0;
    r.cpu().a[1] = 0;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::OpenScreenTagList), ExitReason::BusError);
    CHECK_EQ(r.cpu().fault_addr, kGuestBytes);
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
    u32 screens[4] = {0, 0, 0, 0};
    for (u32& screen : screens) {
        screen = open_new(r, kNewScreen);
        CHECK(screen != 0);
    }
    CHECK_EQ(r.display.front_screen, screens[3]);
    CHECK_EQ(open_new(r, kNewScreen), 0u);
    CHECK(logged("intuition: screen limit reached"));
    CHECK_EQ(r.display.front_screen, screens[3]);
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
    CHECK_EQ(r.get8(window + wd_BorderLeft), 1u);
    CHECK_EQ(r.get8(window + wd_BorderTop), 11u);
    CHECK_EQ(r.get8(window + wd_BorderRight), 1u);
    CHECK_EQ(r.get8(window + wd_BorderBottom), 1u);
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
    CHECK_EQ(window_pixel(r, window, 0, 0), 1u);
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

    CHECK_EQ(screen_pixel(r, screen, 50, 60), 1u);
    CHECK_EQ(screen_pixel(r, screen, 249, 60), 1u);
    CHECK_EQ(screen_pixel(r, screen, 50, 159), 1u);
    CHECK_EQ(screen_pixel(r, screen, 50, 80), 1u);
    CHECK_EQ(screen_pixel(r, screen, 100, 65), 2u);
    CHECK_EQ(screen_pixel(r, screen, 100, 70), 1u);
    CHECK_EQ(screen_pixel(r, screen, 100, 71), 0u);
    CHECK_EQ(screen_pixel(r, screen, 51, 61), 1u);
    CHECK_EQ(screen_pixel(r, screen, 54, 64), 2u);
    CHECK_EQ(screen_pixel(r, screen, 241, 61), 1u);
    CHECK_EQ(screen_pixel(r, screen, 244, 64), 2u);
    CHECK_EQ(screen_pixel(r, screen, 61, 61), 1u);

    // Without the close gadget the bar has no box in that corner.
    put_new_window(r, kNewWindow, 300, 60, 200, 100, 0, 0, 0, 0);
    const u32 plain = open_win(r, kNewWindow);
    CHECK(plain != 0);
    CHECK_EQ(screen_pixel(r, screen, 301, 61), 2u);
    CHECK_EQ(screen_pixel(r, screen, 301, 65), 2u);
    CHECK_EQ(screen_pixel(r, screen, 300, 60), 1u);
}

void open_window_default_is_full_screen_below_bar() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 0, 0, 0, 0, 0, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.get16(window + wd_LeftEdge), 0u);
    CHECK_EQ(r.get16(window + wd_TopEdge), 0u);
    CHECK_EQ(r.get16(window + wd_Width), 640u);
    CHECK_EQ(r.get16(window + wd_Height), 400u);
    const u32 layer = r.get32(window + wd_WLayer);
    CHECK_EQ(r.get16(layer + ly_MinX), 0u);
    CHECK_EQ(r.get16(layer + ly_MinY), 0u);
    CHECK_EQ(r.get16(layer + ly_MaxX), 639u);
    CHECK_EQ(r.get16(layer + ly_MaxY), 399u);
    CHECK_EQ(screen_pixel(r, screen, 0, 0), 1u);
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
    CHECK_EQ(r.cpu().fault_addr, kGuestBytes);

    GfxRig other;
    other.cpu().a[0] = 0;
    other.cpu().a[1] = 0;
    CHECK_EQ(other.call(Base::Intuition, intuition_lvo::OpenWindowTagList),
             ExitReason::BusError);
    CHECK_EQ(other.cpu().fault_addr, kGuestBytes);
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
    CHECK(changed_only_inside(before, after, 80, 400, 51, 71, 248, 158));
    CHECK_EQ(screen_pixel(r, screen, 53, 72), 3u);
    CHECK_EQ(screen_pixel(r, screen, 50, 78), 1u);
    CHECK_EQ(screen_pixel(r, screen, 249, 78), 1u);
    CHECK_EQ(screen_pixel(r, screen, 100, 70), 1u);
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
    CHECK_EQ(r.get16(rp + rp_cp_x), 321u);
    CHECK_EQ(screen_pixel(r, screen, 110, 72), 0u);
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

void set_window_titles_redraws_title() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    r.str(kWinTitle, "A");
    put_new_window(r, kNewWindow, 50, 60, 200, 100, WFLG_CLOSEGADGET, kWinTitle, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(screen_pixel(r, screen, 61, 61), 2u);

    r.str(kWinTitle, "B");
    r.cpu().a[0] = window;
    r.cpu().a[1] = kWinTitle;
    r.cpu().a[2] = 0xFFFFFFFF;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::SetWindowTitles), ExitReason::Halt);
    const u32 title = r.get32(window + wd_Title);
    CHECK(title != 0);
    CHECK(title != kWinTitle);
    CHECK(r.read_str(title) == "B");
    CHECK_EQ(screen_pixel(r, screen, 61, 61), 1u);

    r.cpu().a[0] = window;
    r.cpu().a[1] = 0;
    r.cpu().a[2] = 0xFFFFFFFF;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::SetWindowTitles), ExitReason::Halt);
    CHECK_EQ(r.get32(window + wd_Title), 0u);
    CHECK_EQ(screen_pixel(r, screen, 61, 61), 2u);

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
    CHECK_EQ(screen_pixel(r, screen, 51, 61), 1u);
    CHECK_EQ(screen_pixel(r, screen, 241, 61), 1u);

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
    CHECK_EQ(screen_pixel(r, screen, 11, 21), 1u);
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
    CHECK_EQ(screen_pixel(r, screen, 11, 20), 1u);
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

void hit_rectangles_match_the_drawn_boxes() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, WFLG_CLOSEGADGET | WFLG_DEPTHGADGET, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);

    const Rect boxes[2] = {Intuition::close_box_rect(50, 60),
                           Intuition::depth_box_rect(50, 60, 200)};
    for (const Rect& box : boxes) {
        for (i32 y = box.y0; y <= box.y1; ++y) {
            for (i32 x = box.x0; x <= box.x1; ++x) {
                const bool border =
                    x == box.x0 || x == box.x1 || y == box.y0 || y == box.y1;
                CHECK_EQ(screen_pixel(r, screen, x, y), border ? 1u : 2u);
            }
        }
        CHECK_EQ(screen_pixel(r, screen, box.x0, box.y1 + 1), 2u);
    }
    CHECK_EQ(screen_pixel(r, screen, boxes[0].x1 + 1, boxes[0].y0), 2u);
    CHECK_EQ(screen_pixel(r, screen, boxes[1].x0 - 1, boxes[1].y0), 2u);
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

    r.intuition.reclaim(keep_win, keep_scr);
    CHECK(r.intuition.window_open(keep_win));
    CHECK(!r.intuition.window_open(extra_win1));
    CHECK(!r.intuition.window_open(extra_win2));
    CHECK_EQ(r.intuition.front_screen(), keep_scr);
    CHECK_EQ(r.display.front_screen, keep_scr);
    CHECK_EQ(r.get32(keep_scr + sc_FirstWindow), keep_win);
    CHECK_EQ(r.get32(keep_win + wd_NextWindow), 0u);
    CHECK_EQ(r.get32(keep_scr), 0u);
    CHECK_EQ(avail_mem(r), baseline);

    r.intuition.reclaim(0, 0);
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

    r.intuition.reclaim(keep_win, keep_scr);
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
    r.intuition.reclaim(keep_win, keep_scr);
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

    r.intuition.reclaim(keep_win, keep_scr);
    CHECK_EQ(r.intuition.active_window(), 0u);
    CHECK(r.intuition.window_open(keep_win));

    // When keep_win is the active window, reclaim leaves it active.
    r.intuition.pointer_event(30, 40, kMouseLeft, kMouseLeft, 0, false);
    CHECK_EQ(r.intuition.active_window(), keep_win);
    put_new_window(r, kNewWindow, 150, 40, 120, 80, 0, 0, 0, 0);
    const u32 inactive_win = open_win(r, kNewWindow);
    CHECK(inactive_win != 0);
    r.intuition.reclaim(keep_win, keep_scr);
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

    r.intuition.reclaim(0, keep_scr);
    CHECK_EQ(r.intuition.front_screen(), keep_scr);
    CHECK_EQ(r.display.front_screen, keep_scr);
    CHECK_EQ(r.get32(keep_scr), 0u);
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
    CHECK_EQ(screen_pixel(r, screen, 105, 64), 0u);
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
    CHECK_EQ(r.get8(window + wd_BorderRight), 10u);
    CHECK_EQ(r.get8(window + wd_BorderBottom), 1u);
    CHECK_EQ(window_pixel(r, window, 195, 95), 2u);
    CHECK_EQ(window_pixel(r, window, 191, 91), 1u);
    CHECK_EQ(window_pixel(r, window, 190, 50), 1u);
    CHECK_EQ(window_pixel(r, window, 189, 50), 0u);
    CHECK_EQ(screen_pixel(r, screen, 245, 155), 2u);
    CHECK_EQ(screen_pixel(r, screen, 241, 151), 1u);
}

void size_bbottom_sets_bottom_border() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    const u32 screen = open_new(r, kNewScreen);
    CHECK(screen != 0);
    put_new_window(r, kNewWindow, 50, 60, 200, 100, WFLG_SIZEGADGET | WFLG_SIZEBBOTTOM, 0, 0, 0);
    const u32 window = open_win(r, kNewWindow);
    CHECK(window != 0);
    CHECK_EQ(r.get8(window + wd_BorderBottom), 10u);
    CHECK_EQ(r.get8(window + wd_BorderRight), 1u);
    CHECK_EQ(window_pixel(r, window, 100, 90), 1u);
    CHECK_EQ(window_pixel(r, window, 100, 89), 0u);
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
    // The title bar (pen 2 inside the frame) is now at y 121..129.
    CHECK_EQ(screen_pixel(r, screen, 300, 125), 2u);
    CHECK_EQ(screen_pixel(r, screen, 300, 120), 1u);
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
    // The size gadget at the new corner, in the window and on screen.
    CHECK_EQ(window_pixel(r, window, 275, 185), 2u);
    CHECK_EQ(screen_pixel(r, screen, 475, 285), 2u);
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

constexpr u32 kProp1 = kData + 0x700;
constexpr u32 kGad1 = kData + 0x740;
constexpr u32 kProp2 = kData + 0x780;
constexpr u32 kGad2 = kData + 0x7C0;
constexpr u32 kProp3 = kData + 0x800;
constexpr u32 kGad3 = kData + 0x840;
constexpr u32 kProp4 = kData + 0x880;
constexpr u32 kGad4 = kData + 0x8C0;

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

    // Container frame (pen 1) at (20,20)..(35,79).
    CHECK_EQ(window_pixel(r, window, 20, 20), 1u);
    CHECK_EQ(window_pixel(r, window, 35, 20), 1u);
    CHECK_EQ(window_pixel(r, window, 20, 79), 1u);
    CHECK_EQ(window_pixel(r, window, 35, 79), 1u);
    // Knob at top (inner 21..34 x 21..78, height = 58 * 0x4000 / 0xFFFF = 14 -> y 21..34).
    CHECK_EQ(window_pixel(r, window, 21, 21), 1u);
    CHECK_EQ(window_pixel(r, window, 34, 34), 1u);
    CHECK_EQ(window_pixel(r, window, 22, 22), 2u);
    CHECK_EQ(window_pixel(r, window, 33, 33), 2u);
    CHECK_EQ(screen_pixel(r, screen, 72, 72), 2u);
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
    CHECK_EQ(window_pixel(r, window, 12, 17), 2u);
    CHECK_EQ(window_pixel(r, window, 42, 17), 2u);
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

    // Initially knob is at top (inner y 21..68, knob_h = 12 -> y 21..32).
    CHECK_EQ(window_pixel(r, window, 25, 22), 2u);
    CHECK_EQ(window_pixel(r, window, 25, 65), 0u);

    r.intuition.new_modify_prop(kGad1, window, AUTOKNOB | FREEVERT, 0, MAXPOT, MAXBODY,
                                MAXBODY / 4, 1);
    CHECK_EQ(r.get16(kProp1 + pi_VertPot), MAXPOT);
    // Knob moved to bottom (y 57..68).
    CHECK_EQ(window_pixel(r, window, 25, 22), 0u);
    CHECK_EQ(window_pixel(r, window, 25, 65), 2u);
}

void relative_geometry_follows_resize() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);

    put_prop_info(r, kProp1, AUTOKNOB | FREEVERT | PROPBORDERLESS, 0, 0, MAXBODY, MAXBODY / 4);
    put_gadget(r, kGad1, 0, -8, 11, 8, -21, GFLG_RELRIGHT | GFLG_RELHEIGHT, 0, GTYP_PROPGADGET,
               kProp1);
    put_new_window(r, kNewWindow, 40, 40, 200, 100, WFLG_SIZEGADGET, 0, 0, 0);
    r.put16(kNewWindow + nw_MaxWidth, 0xFFFF);
    r.put16(kNewWindow + nw_MaxHeight, 0xFFFF);
    r.put32(kNewWindow + nw_FirstGadget, kGad1);
    const u32 window = open_win(r, kNewWindow);
    r.put32(kNewWindow + nw_FirstGadget, 0);
    CHECK(window != 0);

    // At 200x100: box x = 191..198, y = 11..89; knob at x 192..197, y 12..30.
    CHECK_EQ(window_pixel(r, window, 195, 20), 2u);
    CHECK_EQ(window_pixel(r, window, 195, 80), 0u);

    r.intuition.size_window_to(window, 300, 160);
    // At 300x160: box x = 291..298, y = 11..149; knob at x 292..297, y 12..45.
    CHECK_EQ(window_pixel(r, window, 195, 20), 0u);
    CHECK_EQ(window_pixel(r, window, 295, 20), 2u);
    CHECK_EQ(window_pixel(r, window, 295, 140), 0u);
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

void non_prop_gadget_is_linked_not_hit() {
    GfxRig r;
    put_new_screen(r, kNewScreen, 640, 400, 2, 0, 1, 0);
    CHECK(open_new(r, kNewScreen) != 0);

    put_gadget(r, kGad1, 0, 20, 20, 40, 20, 0, GACT_IMMEDIATE, 1, 0);
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

    u32 count = 0;
    for (const std::string& line : gfx_test::g_lines) {
        if (line == "intuition: gadget type 1 not handled") {
            ++count;
        }
    }
    CHECK_EQ(count, 1u);
    const std::vector<u32> classes = queued_classes(r, window);
    CHECK_EQ(classes.size(), 2u);
    CHECK_EQ(classes[0], IDCMP_MOUSEBUTTONS);
    CHECK_EQ(classes[1], IDCMP_MOUSEBUTTONS);
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
    CHECK_EQ(window_pixel(r, window, 25, 22), 2u);

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
    CHECK_EQ(window_pixel(r, window, 25, 65), 2u);

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
    CHECK_EQ(window_pixel(r, window, 25, 22), 2u);

    r.cpu().a[0] = kGad1;
    r.cpu().a[1] = window;
    r.cpu().a[2] = 0;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::RefreshGadgets), ExitReason::Halt);

    r.cpu().a[0] = window;
    r.cpu().a[1] = kGad2;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::RemoveGadget), 1u);
    CHECK_EQ(r.get32(kGad1 + gg_NextGadget), 0u);
}

}

int main() {
    screen_font_is_a_textattr_naming_topaz();
    open_screen_builds_struct_bitmap_and_colormap();
    open_screen_clears_and_draws_title_bar();
    open_screen_makes_it_front_and_refreshes_the_whole_screen();
    open_screen_taglist_reads_sa_tags();
    open_screen_null_newscreen_and_taglist_gives_zero_and_faults();
    open_screen_out_of_memory_returns_zero_and_frees();
    open_screen_limit_returns_zero();
    close_screen_twice_and_null_are_ignored();
    close_front_screen_promotes_next();
    reopen_after_close_reuses_memory();
    get_screen_data_copies_struct();
    unimplemented_vector_ends_run();
    open_window_builds_struct_rastport_and_layer();
    open_window_draws_chrome();
    open_window_default_is_full_screen_below_bar();
    open_window_taglist_reads_wa_tags();
    open_window_on_unknown_screen_gives_zero();
    open_window_null_newwindow_gives_zero_and_faults();
    open_window_alloc_failure_gives_zero_and_frees();
    open_window_limit_gives_zero();
    window_text_goes_into_the_window_only();
    text_longer_than_window_is_clipped();
    close_window_repaints_and_unlinks();
    close_window_twice_and_null_ignored();
    set_window_titles_redraws_title();
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
    close_gadget_posts_closewindow_on_release_over_the_box();
    close_gadget_release_elsewhere_does_nothing();
    close_gadget_without_idcmp_does_nothing();
    depth_gadget_toggles_front_and_back();
    gadget_press_is_consumed_without_a_mousebuttons_message();
    hit_rectangles_match_the_drawn_boxes();
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
    newsize_coalesced_and_final_on_release();
    unreplied_resizes_do_not_exhaust_messages();
    move_posts_changewindow_only();
    drag_press_is_not_selectdown();
    size_hook_called_per_step();
    close_during_drag_ends_drag();
    open_window_links_and_draws_prop_gadget();
    wa_gadgets_tag_links_list();
    add_and_remove_glist_positions();
    new_modify_prop_moves_knob();
    relative_geometry_follows_resize();
    knob_drag_updates_pot_and_hook();
    container_click_pages_by_body();
    gadget_idcmp_down_move_up();
    disabled_gadget_ignores_input();
    non_prop_gadget_is_linked_not_hit();
    gadget_list_cycle_is_bounded();
    close_during_knob_drag();
    wheel_event_posts_newmouse_rawkey();
    gadget_lvos_from_guest_calls();
    return check::finish("test_intuition");
}
