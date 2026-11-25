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

#include "bitmap_surface.hpp"
#include "gfx_rig.hpp"
#include "libs/chrome_paint.hpp"

#include <vector>

using namespace hombre;
using namespace hombre::ndk;
using hombre::m68k::ExitReason;
using gfx_test::Base;
using gfx_test::GfxRig;

namespace {

constexpr i32 kWindowWidth = 320;
constexpr i32 kWindowHeight = 120;
constexpr u32 kTitleAddress = gfx_test::kData;
constexpr u32 kRastPortAddress = gfx_test::kData + 0x200;

std::vector<u8> monitor_bytes(u8 value = 0) {
    return std::vector<u8>(kMonitorWidth * kMonitorHeight, value);
}

chrome::Title monitor_title() {
    return chrome::Title{reinterpret_cast<const u8*>("Monitor"), 7};
}

chrome::WindowChrome monitor_window() {
    chrome::WindowChrome w{};
    w.width = kWindowWidth;
    w.height = kWindowHeight;
    w.flags = WFLG_CLOSEGADGET | WFLG_ACTIVATE;
    w.title = kTitleAddress;
    w.active = true;
    w.border_left = 2;
    w.border_right = 2;
    w.border_bottom = 2;
    w.border_top = static_cast<u32>(w.metrics.border_top());
    return w;
}

struct Canvas {
    u32 bitmap;
    u32 rp;
};

Canvas make_canvas(GfxRig& r, i32 width, i32 height, u32 depth) {
    r.cpu().d[0] = static_cast<u32>(width);
    r.cpu().d[1] = static_cast<u32>(height);
    r.cpu().d[2] = depth;
    r.cpu().d[3] = 0;
    r.cpu().a[0] = 0;
    const Canvas canvas{r.result(Base::Graphics, graphics_lvo::AllocBitMap), kRastPortAddress};
    CHECK(canvas.bitmap != 0);
    r.cpu().a[1] = canvas.rp;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::InitRastPort), ExitReason::Halt);
    r.put32(canvas.rp + rp_BitMap, canvas.bitmap);
    return canvas;
}

// The pen of the planar bitmap pixel at x, y.
u32 pen_at(const GfxRig& r, u32 bitmap, i32 x, i32 y) {
    const u32 bytes_per_row = r.get16(bitmap + bm_BytesPerRow);
    const u32 depth = r.get8(bitmap + bm_Depth);
    const u32 at = static_cast<u32>(y) * bytes_per_row + static_cast<u32>(x) / 8;
    const u8 bit = static_cast<u8>(0x80u >> (static_cast<u32>(x) & 7));
    u32 pen = 0;
    for (u32 plane = 0; plane < depth; ++plane) {
        const u32 base = r.get32(bitmap + bm_Planes + plane * 4);
        if ((r.get8(base + at) & bit) != 0) {
            pen |= 1u << plane;
        }
    }
    return pen;
}

void fill_is_cut_to_the_clip() {
    std::vector<u8> pens = monitor_bytes(0xAB);
    BitmapSurface bitmap_surface;
    bitmap_surface.init(pens.data());
    const chrome::Surface surface = bitmap_surface.surface();
    bitmap_surface.set_clip(Rect{100, 100, 199, 199});

    surface.fill(surface.ctx, Rect{0, 0, 50, 50}, 7);
    bool untouched = true;
    for (u8 pen : pens) {
        untouched = untouched && pen == 0xAB;
    }
    CHECK(untouched);

    surface.fill(surface.ctx, Rect{90, 90, 110, 110}, 3);
    CHECK_EQ(pens[100 * kMonitorWidth + 100], 3);
    CHECK_EQ(pens[110 * kMonitorWidth + 110], 3);
    CHECK_EQ(pens[99 * kMonitorWidth + 100], 0xAB);
    CHECK_EQ(pens[100 * kMonitorWidth + 99], 0xAB);
    CHECK_EQ(pens[110 * kMonitorWidth + 111], 0xAB);
    CHECK_EQ(pens[111 * kMonitorWidth + 110], 0xAB);
}

void text_metrics_match_the_builtin_font() {
    std::vector<u8> pens = monitor_bytes();
    BitmapSurface bitmap_surface;
    bitmap_surface.init(pens.data());
    const chrome::Surface surface = bitmap_surface.surface();
    const u8* chars = reinterpret_cast<const u8*>("0123456789");

    CHECK_EQ(surface.text_fit(surface.ctx, 0, chars, 10, 33), 4u);
    CHECK_EQ(surface.text_width(surface.ctx, 0, chars, 4), 32);
}

void window_matches_graphics_library() {
    GfxRig r;
    r.str(kTitleAddress, "Monitor");
    const Canvas canvas = make_canvas(r, kWindowWidth, kWindowHeight, 8);
    const chrome::WindowChrome window = monitor_window();
    native::Bus bus(r.machine);
    chrome::draw_window_chrome(r.graphics, bus, canvas.rp, window);

    std::vector<u8> pens = monitor_bytes();
    BitmapSurface bitmap_surface;
    bitmap_surface.init(pens.data());
    chrome::paint_modern_window(bitmap_surface.surface(), window, monitor_title());

    u32 mismatches = 0;
    for (i32 y = 0; y < kWindowHeight; ++y) {
        for (i32 x = 0; x < kWindowWidth; ++x) {
            const u32 at = static_cast<u32>(y) * kMonitorWidth + static_cast<u32>(x);
            if (static_cast<u32>(pens[at]) != pen_at(r, canvas.bitmap, x, y)) {
                ++mismatches;
            }
        }
    }
    CHECK_EQ(mismatches, 0u);
}

void bands_match_one_paint() {
    std::vector<u8> single = monitor_bytes();
    std::vector<u8> bands = monitor_bytes();
    BitmapSurface whole;
    whole.init(single.data());
    BitmapSurface banded;
    banded.init(bands.data());

    chrome::paint_modern_window(whole.surface(), monitor_window(), monitor_title());
    for (i32 y = 0; y < kWindowHeight; y += 16) {
        const i32 bottom = y + 15 < kWindowHeight - 1 ? y + 15 : kWindowHeight - 1;
        banded.set_clip(Rect{0, y, kWindowWidth - 1, bottom});
        chrome::paint_modern_window(banded.surface(), monitor_window(), monitor_title());
    }
    CHECK(single == bands);
}

void palette_has_the_chrome_colours() {
    u32 rgb[256];
    monitor_palette(rgb);
    CHECK_EQ(rgb[0], 0xAAAAAAu);
    CHECK_EQ(rgb[228], 0x6D9BE0u);
    CHECK_EQ(rgb[242], 0x2A2D33u);
}

}

int main() {
    fill_is_cut_to_the_clip();
    text_metrics_match_the_builtin_font();
    window_matches_graphics_library();
    bands_match_one_paint();
    palette_has_the_chrome_colours();
    return check::finish("test_bitmap_surface");
}
