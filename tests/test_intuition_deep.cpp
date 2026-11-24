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

}

int main() {
    open_screen_hombre_800x600_24();
    window_on_deep_screen_has_deep_bitmap();
    compose_puts_window_pixels_on_screen();
    window_partly_off_screen_clips();
    open_screen_fails_when_fast_memory_runs_out();
    close_screen_frees_deep_memory();
    requester_on_deep_screen_restores_what_it_covered();
    return check::finish("test_intuition_deep");
}
