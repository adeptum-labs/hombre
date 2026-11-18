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
    CHECK_EQ(r.get32(rp + rp_BitMap), screen + sc_BitMap);
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
    return check::finish("test_intuition");
}
