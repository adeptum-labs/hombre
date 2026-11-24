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
#include "libs/cybergraphics.hpp"
#include "libs/display_modes.hpp"

using namespace gfx_test;
using hombre::PixFmt;
namespace ndk = hombre::ndk;
namespace display_modes = hombre::display_modes;

namespace {

constexpr u32 kScreenTags = kData + 0x100;
constexpr u32 kWindowTags = kData + 0x200;
constexpr u32 kMiB = 1024 * 1024;
// The rig's fast memory is 8 MiB: 800x600 at 4 bytes a pixel fits twice.
const u32 k800x24 = display_modes::hombre_id(0, 2);
const u32 k800x16 = display_modes::hombre_id(0, 1);
const u32 k1600x24 = display_modes::hombre_id(2, 2);

u32 open_screen(GfxRig& r, u32 mode_id) {
    const u32 tags = r.tags(kScreenTags, {{ndk::SA_DisplayID, mode_id}, {ndk::SA_Depth, 8},
                                          {0, 0}});
    return r.intuition.open_screen(0, tags);
}

u32 open_window(GfxRig& r, u32 screen, i32 left, i32 top, i32 width, i32 height) {
    const u32 tags = r.tags(kWindowTags, {{ndk::WA_Left, static_cast<u32>(left)},
                                          {ndk::WA_Top, static_cast<u32>(top)},
                                          {ndk::WA_Width, static_cast<u32>(width)},
                                          {ndk::WA_Height, static_cast<u32>(height)},
                                          {ndk::WA_CustomScreen, screen},
                                          {0, 0}});
    return r.intuition.open_window(0, tags);
}

u32 value_at(GfxRig& r, u32 bitmap, u32 x, u32 y) {
    const hombre::DeepBitmap* d = r.graphics.deep(bitmap);
    const u32 at = d->pixels + y * d->bytes_per_row + x * hombre::bytes_per_pixel(d->fmt);
    return d->fmt == PixFmt::Argb32 ? r.get32(at) : d->fmt == PixFmt::Rgb16 ? r.get16(at)
                                                                            : r.get8(at);
}

void open_screen_hombre_800x600_24() {
    GfxRig r;
    display_modes::configure_hombre(1280, 800, 8 * kMiB);
    const u32 screen = open_screen(r, k800x24);
    CHECK(screen != 0);
    const u32 bitmap = screen + ndk::sc_BitMap;
    const hombre::DeepBitmap* deep = r.graphics.deep(bitmap);
    CHECK(deep != nullptr);
    CHECK(deep->fmt == PixFmt::Argb32);
    CHECK_EQ(deep->width, 800u);
    CHECK_EQ(deep->height, 600u);
    CHECK(deep->pixels >= kTestChipEnd);
    CHECK(deep->palette != nullptr);
    CHECK_EQ(r.get16(screen + ndk::sc_Width), 800u);
    CHECK_EQ(r.get8(bitmap + ndk::bm_Depth), 24u);
    display_modes::configure_hombre(0, 0, 0);
}

void window_on_deep_screen_has_deep_bitmap() {
    GfxRig r;
    display_modes::configure_hombre(1280, 800, 8 * kMiB);
    const u32 screen = open_screen(r, k800x16);
    CHECK(screen != 0);
    const u32 window = open_window(r, screen, 100, 50, 200, 120);
    CHECK(window != 0);
    const u32 rp = r.get32(window + ndk::wd_RPort);
    const hombre::DeepBitmap* deep = r.graphics.deep(r.get32(rp + ndk::rp_BitMap));
    CHECK(deep != nullptr);
    CHECK(deep->fmt == PixFmt::Rgb16);
    CHECK(deep->palette == r.graphics.deep(screen + ndk::sc_BitMap)->palette);
    display_modes::configure_hombre(0, 0, 0);
}

void compose_puts_window_pixels_on_screen() {
    GfxRig r;
    display_modes::configure_hombre(1280, 800, 8 * kMiB);
    const u32 screen = open_screen(r, k800x24);
    const u32 window = open_window(r, screen, 100, 50, 200, 120);
    const u32 rp = r.get32(window + ndk::wd_RPort);
    r.graphics.set_pens(rp, 2, 0, ndk::JAM1);
    r.graphics.rect_fill(rp, 20, 20, 30, 30);
    const u32 window_bitmap = r.get32(rp + ndk::rp_BitMap);
    const u32 expected = value_at(r, window_bitmap, 25, 25);
    CHECK(expected != 0);
    CHECK_EQ(value_at(r, screen + ndk::sc_BitMap, 125, 75), expected);
    display_modes::configure_hombre(0, 0, 0);
}

void window_partly_off_screen_clips() {
    GfxRig r;
    display_modes::configure_hombre(1280, 800, 8 * kMiB);
    const u32 screen = open_screen(r, k800x24);
    const u32 window = open_window(r, screen, 700, 500, 200, 200);
    CHECK(window != 0);
    const u32 rp = r.get32(window + ndk::wd_RPort);
    r.graphics.set_pens(rp, 2, 0, ndk::JAM1);
    r.graphics.rect_fill(rp, 0, 0, 199, 199);
    const u32 bitmap = screen + ndk::sc_BitMap;
    const hombre::DeepBitmap* deep = r.graphics.deep(bitmap);
    // The window's visible corner shows; the screen's own memory ends where
    // it did, with the byte past it untouched by the compose.
    CHECK(value_at(r, bitmap, 799, 599) != 0);
    CHECK(deep->pixels + deep->pixel_bytes <= kTestGuestBytes);
    display_modes::configure_hombre(0, 0, 0);
}

void open_screen_fails_when_fast_memory_runs_out() {
    GfxRig r;
    // Listed as if fast memory were large; 1600x1200 at 4 bytes twice does
    // not fit the rig's 8 MiB.
    display_modes::configure_hombre(1280, 800, 512 * kMiB);
    const u32 fast = r.exec.free_bytes(ndk::MEMF_FAST);
    const u32 any = r.exec.free_bytes();
    CHECK_EQ(open_screen(r, k1600x24), 0u);
    CHECK_EQ(r.exec.free_bytes(ndk::MEMF_FAST), fast);
    CHECK_EQ(r.exec.free_bytes(), any);
    display_modes::configure_hombre(0, 0, 0);
}

void close_screen_frees_deep_memory() {
    GfxRig r;
    display_modes::configure_hombre(1280, 800, 8 * kMiB);
    const u32 fast = r.exec.free_bytes(ndk::MEMF_FAST);
    const u32 screen = open_screen(r, k800x24);
    const u32 window = open_window(r, screen, 10, 10, 100, 100);
    const u32 window_bitmap = r.get32(r.get32(window + ndk::wd_RPort) + ndk::rp_BitMap);
    r.intuition.close_window(window);
    CHECK(r.graphics.deep(window_bitmap) == nullptr);
    CHECK(r.intuition.close_screen(screen));
    CHECK(r.graphics.deep(screen + ndk::sc_BitMap) == nullptr);
    CHECK_EQ(r.exec.free_bytes(ndk::MEMF_FAST), fast);
    display_modes::configure_hombre(0, 0, 0);
}

// A requester on a deep screen keeps what it covers in a deep bitmap and
// gives it back when it ends.
void requester_on_deep_screen_restores_what_it_covered() {
    GfxRig r;
    display_modes::configure_hombre(1280, 800, 8 * kMiB);
    const u32 screen = open_screen(r, k800x24);
    const u32 window = open_window(r, screen, 100, 50, 200, 120);
    const u32 rp = r.get32(window + ndk::wd_RPort);
    const u32 window_bitmap = r.get32(rp + ndk::rp_BitMap);
    const u32 before = value_at(r, window_bitmap, 40, 40);
    const u32 req = kData + 0x400;
    r.cpu().a[0] = req;
    r.call(Base::Intuition, hombre::intuition_lvo::InitRequester);
    r.put16(req + 4, 30);
    r.put16(req + 6, 30);
    r.put16(req + 8, 60);
    r.put16(req + 10, 40);
    r.put8(req + 30, 2);
    r.cpu().a[0] = req;
    r.cpu().a[1] = window;
    CHECK_EQ(r.result(Base::Intuition, hombre::intuition_lvo::Request), 1u);
    CHECK(value_at(r, window_bitmap, 40, 40) != before);
    r.cpu().a[0] = req;
    r.cpu().a[1] = window;
    r.call(Base::Intuition, hombre::intuition_lvo::EndRequest);
    CHECK_EQ(value_at(r, window_bitmap, 40, 40), before);
    display_modes::configure_hombre(0, 0, 0);
}

// A window on an RGB screen casts its shadow by darkening the colours
// beside it, at 70% in the strong band.
void shadow_on_argb32_screen_darkens_the_colour() {
    GfxRig r;
    display_modes::configure_hombre(1280, 800, 8 * kMiB);
    const u32 screen = open_screen(r, k800x24);
    const u32 bitmap = screen + ndk::sc_BitMap;
    const u32 backdrop = value_at(r, bitmap, 400, 400) & 0xFFFFFF;
    const u32 window = open_window(r, screen, 100, 50, 200, 120);
    CHECK(window != 0);
    const u32 shadow = value_at(r, bitmap, 300, 100) & 0xFFFFFF;
    CHECK(shadow != backdrop);
    const u32 red = (backdrop >> 16) & 0xFF;
    CHECK_EQ((shadow >> 16) & 0xFF, red * 70 / 100);
    display_modes::configure_hombre(0, 0, 0);
}

void set_iprefs(GfxRig& r, u32 prefs, u32 size, u32 type) {
    r.cpu().a[0] = prefs;
    r.cpu().d[0] = size;
    r.cpu().d[1] = type;
    CHECK_EQ(r.result(Base::Intuition, hombre::intuition_lvo::SetIPrefs), 0u);
}

// IPrefs sends a screen mode and resets the Workbench, as ScreenMode's Use
// does.
void switch_workbench(GfxRig& r, u32 mode_id, u32 width, u32 height, u32 depth) {
    constexpr u32 kPrefs = kData + 0x600;
    r.put32(kPrefs, mode_id);
    r.put16(kPrefs + 4, width);
    r.put16(kPrefs + 6, height);
    r.put16(kPrefs + 8, depth);
    r.put16(kPrefs + 10, 3);
    set_iprefs(r, kPrefs, 12, 1);
    r.cpu().a[0] = 0;
    CHECK_EQ(r.result(Base::Intuition, hombre::intuition_lvo::CloseWorkBench), 1u);
    r.cpu().a[0] = 0;
    r.call(Base::Intuition, hombre::intuition_lvo::OpenWorkBench);
}

// The Workbench goes from an Amiga mode to a HOMBRE one and back with its
// window kept: deep in the HOMBRE mode, planar again after.
void workbench_reopens_amiga_hombre_amiga() {
    GfxRig r;
    display_modes::configure_hombre(1280, 800, 8 * kMiB);
    const u32 screen = open_screen(r, 0x8004);
    CHECK(screen != 0);
    r.intuition.set_default_screen(screen);
    const u32 window = open_window(r, screen, 20, 30, 300, 200);
    r.intuition.keep_through_workbench_reset(window);
    const auto window_bitmap = [&]() {
        return r.get32(r.get32(window + ndk::wd_RPort) + ndk::rp_BitMap);
    };
    CHECK(r.graphics.deep(window_bitmap()) == nullptr);

    switch_workbench(r, k800x24, 0xFFFF, 0xFFFF, 8);
    CHECK(r.graphics.deep(screen + ndk::sc_BitMap) != nullptr);
    CHECK(r.graphics.deep(window_bitmap()) != nullptr);
    CHECK(r.graphics.deep(window_bitmap())->fmt == PixFmt::Argb32);
    CHECK(r.display.mode_deep);
    CHECK_EQ(r.get16(screen + ndk::sc_Width), 800u);
    CHECK(r.intuition.window_open(window));

    switch_workbench(r, 0x8004, 0xFFFF, 0xFFFF, 8);
    CHECK(r.graphics.deep(screen + ndk::sc_BitMap) == nullptr);
    CHECK(r.graphics.deep(window_bitmap()) == nullptr);
    CHECK_EQ(r.get8(window_bitmap() + ndk::bm_Depth), 8u);
    CHECK(!r.display.mode_deep);
    CHECK(r.intuition.window_open(window));
    display_modes::configure_hombre(0, 0, 0);
}

// A listed HOMBRE mode opens in the format of the depth asked for: up to 8
// bits 8-bit, up to 16 bits 16-bit, deeper 24-bit. The screen's own ModeID
// is then the mode of that format.
void listed_mode_depth_picks_the_format() {
    GfxRig r;
    display_modes::configure_hombre(1280, 800, 8 * kMiB);
    const u32 listed = display_modes::hombre_id(0, display_modes::kAnyDepth);
    const struct {
        u32 depth;
        PixFmt fmt;
        u32 bm_depth;
        u32 code;
    } cases[] = {{4, PixFmt::Lut8, 8, 0}, {16, PixFmt::Rgb16, 16, 1},
                 {24, PixFmt::Argb32, 24, 2}};
    for (const auto& c : cases) {
        const u32 tags = r.tags(kScreenTags, {{ndk::SA_DisplayID, listed},
                                              {ndk::SA_Depth, c.depth},
                                              {0, 0}});
        const u32 screen = r.intuition.open_screen(0, tags);
        CHECK(screen != 0);
        const hombre::DeepBitmap* deep = r.graphics.deep(screen + ndk::sc_BitMap);
        CHECK(deep != nullptr);
        CHECK(deep->fmt == c.fmt);
        CHECK_EQ(r.get8(screen + ndk::sc_BitMap + ndk::bm_Depth), c.bm_depth);
        r.cpu().a[0] = screen + ndk::sc_ViewPort;
        CHECK_EQ(r.result(Base::Graphics, hombre::graphics_lvo::GetVPModeID),
                 display_modes::hombre_id(0, c.code));
        r.cpu().a[0] = screen;
        r.call(Base::Intuition, hombre::intuition_lvo::CloseScreen);
    }
    display_modes::configure_hombre(0, 0, 0);
}

// ScreenMode's colours pick the Workbench's format on a listed mode.
void workbench_colours_pick_the_format() {
    GfxRig r;
    display_modes::configure_hombre(1280, 800, 8 * kMiB);
    const u32 screen = open_screen(r, 0x8004);
    CHECK(screen != 0);
    r.intuition.set_default_screen(screen);
    const u32 listed = display_modes::hombre_id(0, display_modes::kAnyDepth);
    switch_workbench(r, listed, 0xFFFF, 0xFFFF, 16);
    const hombre::DeepBitmap* deep = r.graphics.deep(screen + ndk::sc_BitMap);
    CHECK(deep != nullptr);
    CHECK(deep->fmt == PixFmt::Rgb16);
    CHECK(r.display.mode_fmt == PixFmt::Rgb16);
    switch_workbench(r, listed, 0xFFFF, 0xFFFF, 24);
    deep = r.graphics.deep(screen + ndk::sc_BitMap);
    CHECK(deep != nullptr);
    CHECK(deep->fmt == PixFmt::Argb32);
    switch_workbench(r, listed, 0xFFFF, 0xFFFF, 8);
    deep = r.graphics.deep(screen + ndk::sc_BitMap);
    CHECK(deep != nullptr);
    CHECK(deep->fmt == PixFmt::Lut8);
    display_modes::configure_hombre(0, 0, 0);
}

// Palette prefs IPrefs sends reach what pens draw on a 24-bit screen.
void palette_prefs_reach_a_deep_screen() {
    GfxRig r;
    display_modes::configure_hombre(1280, 800, 8 * kMiB);
    const u32 screen = open_screen(r, k800x24);
    r.intuition.set_default_screen(screen);
    constexpr u32 kPrefs = kData + 0x700;
    r.put16(kPrefs, 2);
    r.put16(kPrefs + 2, 0xFFFF);
    r.put16(kPrefs + 4, 0);
    r.put16(kPrefs + 6, 0);
    r.put16(kPrefs + 8, 0xFFFF);
    set_iprefs(r, kPrefs, 10, 8);
    const u32 rp = screen + ndk::sc_RastPort;
    r.graphics.set_pens(rp, 2, 0, ndk::JAM1);
    r.graphics.rect_fill(rp, 0, 100, 0, 100);
    CHECK_EQ(value_at(r, screen + ndk::sc_BitMap, 0, 100) & 0xFFFFFF, 0xFF0000u);
    display_modes::configure_hombre(0, 0, 0);
}

// With the table of deep bitmaps full, a deep screen cannot open: 0, not a
// screen with a BitMap nothing draws into.
void open_screen_fails_when_the_deep_table_is_full() {
    GfxRig r;
    display_modes::configure_hombre(1280, 800, 8 * kMiB);
    std::vector<u32> taken;
    for (u32 i = 0; i < hombre::DeepBitmaps::kCapacity; ++i) {
        const u32 bm = r.graphics.alloc_deep_bitmap(1, 1, PixFmt::Lut8);
        if (bm == 0) {
            break;
        }
        taken.push_back(bm);
    }
    CHECK_EQ(r.graphics.alloc_deep_bitmap(1, 1, PixFmt::Lut8), 0u);
    const u32 fast = r.exec.free_bytes(ndk::MEMF_FAST);
    CHECK_EQ(open_screen(r, k800x24), 0u);
    CHECK_EQ(r.exec.free_bytes(ndk::MEMF_FAST), fast);
    r.graphics.free_deep_bitmap(taken.back());
    r.graphics.free_deep_bitmap(taken[0]);
    CHECK(open_screen(r, k800x24) != 0);
    display_modes::configure_hombre(0, 0, 0);
}

// A program that locks its window's bitmap and writes at the window's place
// on the screen, as on a graphics card, hits the window's own pixels.
void lock_on_a_window_bitmap_takes_screen_coordinates() {
    GfxRig r;
    display_modes::configure_hombre(1280, 800, 8 * kMiB);
    hombre::CyberGraphics cgx;
    CHECK(cgx.init(r.machine, r.exec, r.graphics));
    const u32 screen = open_screen(r, k800x24);
    const u32 window = open_window(r, screen, 300, 200, 200, 120);
    const u32 bitmap = r.get32(r.get32(window + ndk::wd_RPort) + ndk::rp_BitMap);
    constexpr u32 kLockTags = kData + 0x800;
    constexpr u32 kOut = kData + 0x900;
    r.tags(kLockTags, {{0x84001007, kOut}, {0x84001006, kOut + 4}, {0x84001001, kOut + 8},
                       {0, 0}});
    r.put16(kCall, 0x4EAE);
    r.put16(kCall + 2, static_cast<u16>(0x10000 - 168));
    r.put16(kCall + 4, 0x4EF8);
    r.put16(kCall + 6, 0x0400);
    r.jit.flush();
    r.machine.cpu.a[0] = bitmap;
    r.machine.cpu.a[1] = kLockTags;
    r.machine.cpu.a[6] = cgx.base();
    r.machine.cpu.a[7] = kStack;
    r.machine.cpu.pc = kCall;
    CHECK(r.jit.run(1000000) == ExitReason::Halt);
    CHECK(r.machine.cpu.d[0] != 0);
    const u32 base = r.get32(kOut);
    const u32 row = r.get32(kOut + 4);
    CHECK(r.get32(kOut + 8) >= 500u);
    // Pixel 5,5 of the window is screen pixel 305,205.
    const hombre::DeepBitmap* deep = r.graphics.deep(bitmap);
    CHECK_EQ(base + 205 * row + 305 * 4, deep->pixels + 5 * deep->bytes_per_row + 5 * 4);
    display_modes::configure_hombre(0, 0, 0);
}

}

int main() {
    listed_mode_depth_picks_the_format();
    workbench_colours_pick_the_format();
    open_screen_hombre_800x600_24();
    window_on_deep_screen_has_deep_bitmap();
    compose_puts_window_pixels_on_screen();
    window_partly_off_screen_clips();
    open_screen_fails_when_fast_memory_runs_out();
    close_screen_frees_deep_memory();
    requester_on_deep_screen_restores_what_it_covered();
    shadow_on_argb32_screen_darkens_the_colour();
    workbench_reopens_amiga_hombre_amiga();
    palette_prefs_reach_a_deep_screen();
    open_screen_fails_when_the_deep_table_is_full();
    lock_on_a_window_bitmap_takes_screen_coordinates();
    return check::finish("test_intuition_deep");
}
