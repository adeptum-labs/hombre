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

#include "../common/font.hpp"
#include "gfx_rig.hpp"
#include "libs/dos.hpp"
#include "libs/raster.hpp"
#include "libs/shell_window.hpp"

using namespace hombre;
using namespace hombre::m68k;
using namespace hombre::ndk;
using gfx_test::Base;
using gfx_test::GfxRig;
using gfx_test::kData;

namespace {

using i16 = __INT16_TYPE__;

constexpr u32 kNewScreen = kData;

void put_new_screen(GfxRig& r, u32 at, u32 width, u32 height) {
    r.put16(at + ns_LeftEdge, 0);
    r.put16(at + ns_TopEdge, 0);
    r.put16(at + ns_Width, width);
    r.put16(at + ns_Height, height);
    r.put16(at + ns_Depth, 2);
    r.put8(at + ns_DetailPen, 0);
    r.put8(at + ns_BlockPen, 1);
    r.put16(at + ns_ViewModes, 0);
    r.put16(at + ns_Type, CUSTOMSCREEN);
    r.put32(at + ns_Font, 0);
    r.put32(at + ns_DefaultTitle, 0);
    r.put32(at + ns_CustomBitMap, 0);
}

u32 open_screen(GfxRig& r) {
    put_new_screen(r, kNewScreen, 640, 400);
    r.cpu().a[0] = kNewScreen;
    return r.result(Base::Intuition, intuition_lvo::OpenScreen);
}

bool open_shell(GfxRig& r, ShellWindow& shell) {
    return shell.open(r.machine, r.exec, r.graphics, r.intuition, r.display);
}

u32 shell_window(GfxRig& r, u32 screen) { return r.get32(screen + sc_FirstWindow); }

u32 layer_of(GfxRig& r, u32 window) { return r.get32(window + wd_WLayer); }

i32 inner_x(GfxRig& r, u32 window) {
    return static_cast<i16>(r.get16(layer_of(r, window) + ly_MinX));
}

i32 inner_y(GfxRig& r, u32 window) {
    return static_cast<i16>(r.get16(layer_of(r, window) + ly_MinY));
}

u32 columns(GfxRig& r, u32 window) {
    const u32 layer = layer_of(r, window);
    const i32 min = static_cast<i16>(r.get16(layer + ly_MinX));
    const i32 max = static_cast<i16>(r.get16(layer + ly_MaxX));
    return static_cast<u32>(max - min + 1) / 8;
}

u32 rows(GfxRig& r, u32 window) {
    const u32 layer = layer_of(r, window);
    const i32 min = static_cast<i16>(r.get16(layer + ly_MinY));
    const i32 max = static_cast<i16>(r.get16(layer + ly_MaxY));
    return static_cast<u32>(max - min + 1) / 8;
}

raster::Bitmap screen_bitmap(GfxRig& r, u32 screen) {
    raster::Bitmap bm{};
    const u32 at = screen + sc_BitMap;
    bm.bytes_per_row = r.get16(at + bm_BytesPerRow);
    bm.rows = r.get16(at + bm_Rows);
    bm.depth = r.get8(at + bm_Depth);
    for (u32 plane = 0; plane < 8; ++plane) {
        bm.planes[plane] = r.get32(at + bm_Planes + plane * 4);
    }
    bm.addr = at;
    return bm;
}

u32 pixel(GfxRig& r, const raster::Bitmap& bm, i32 x, i32 y) {
    native::Bus bus(r.machine);
    return raster::read_pixel(bus, bm, x, y);
}

bool cell_is(GfxRig& r, const raster::Bitmap& bm, i32 ix, i32 iy, u32 column, u32 row, char ch) {
    const u8* glyph_rows = glyph(ch);
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const u32 want = ((glyph_rows[y] >> x) & 1) != 0 ? 1u : 0u;
            const i32 px = ix + static_cast<i32>(column) * 8 + static_cast<i32>(x);
            const i32 py = iy + static_cast<i32>(row) * 8 + static_cast<i32>(y);
            if (pixel(r, bm, px, py) != want) {
                return false;
            }
        }
    }
    return true;
}

bool cell_all(GfxRig& r, const raster::Bitmap& bm, i32 ix, i32 iy, u32 column, u32 row, u32 pen) {
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const i32 px = ix + static_cast<i32>(column) * 8 + static_cast<i32>(x);
            const i32 py = iy + static_cast<i32>(row) * 8 + static_cast<i32>(y);
            if (pixel(r, bm, px, py) != pen) {
                return false;
            }
        }
    }
    return true;
}

void write_str(ShellWindow& shell, const char* text) {
    u32 length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    shell.write(reinterpret_cast<const u8*>(text), length);
}

void open_gives_a_window_on_the_front_screen() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    CHECK(shell.is_open());
    const u32 window = shell_window(r, screen);
    CHECK(window != 0);
    CHECK_EQ(r.get32(window + wd_WScreen), screen);
    const u32 flags = r.get16(window + wd_Flags) &
                      (WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_DRAGBAR);
    CHECK_EQ(flags, static_cast<u32>(WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_DRAGBAR));
    CHECK(r.read_str(r.get32(window + wd_Title)) == "AmigaShell");
    CHECK_EQ(r.get32(layer_of(r, window) + ly_Window), window);
    CHECK_EQ(inner_x(r, window), 1);
    CHECK_EQ(inner_y(r, window), 11);
}

void open_without_a_screen_returns_false() {
    GfxRig r;
    ShellWindow shell;
    CHECK(!shell.open(r.machine, r.exec, r.graphics, r.intuition, r.display));
    CHECK(!shell.is_open());
    CHECK_EQ(r.display.front_screen, 0u);
}

void text_lands_in_cell_0_0_of_the_inner_area() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);

    const u8 text[] = {'H'};
    shell.write(text, 1);
    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'H'));
    CHECK(cell_all(r, bm, ix, iy, 1, 0, 0));
    CHECK(cell_all(r, bm, ix, iy, 0, 1, 0));
    CHECK_EQ(pixel(r, bm, ix, iy), 1u);
}

void newline_cr_and_tab_move_the_cursor() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);

    write_str(shell, "H\tH\nH\rH");
    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'H'));
    CHECK(cell_is(r, bm, ix, iy, 8, 0, 'H'));
    CHECK(cell_is(r, bm, ix, iy, 0, 1, 'H'));
    CHECK(cell_all(r, bm, ix, iy, 1, 0, 0));
    CHECK(cell_all(r, bm, ix, iy, 9, 0, 0));
    CHECK(cell_all(r, bm, ix, iy, 1, 1, 0));
}

void a_line_wider_than_the_window_wraps() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);
    const u32 cells = columns(r, window);
    CHECK(cells > 1 && cells < 200);

    u8 text[200];
    for (u32 i = 0; i <= cells; ++i) {
        text[i] = 'H';
    }
    shell.write(text, cells + 1);
    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'H'));
    CHECK(cell_is(r, bm, ix, iy, cells - 1, 0, 'H'));
    CHECK(cell_is(r, bm, ix, iy, 0, 1, 'H'));
    CHECK(cell_all(r, bm, ix, iy, 1, 1, 0));
}

void overflow_scrolls_by_one_cell_row() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);
    const u32 cell_rows = rows(r, window);
    CHECK(cell_rows >= 4);

    write_str(shell, "A");
    write_str(shell, "\n");
    write_str(shell, "B");
    write_str(shell, "\n");
    for (u32 row = 2; row + 1 < cell_rows; ++row) {
        write_str(shell, "\n");
    }
    write_str(shell, "\n");

    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'B'));
    CHECK(cell_all(r, bm, ix, iy, 0, cell_rows - 1, 0));
    CHECK(cell_all(r, bm, ix, iy, 1, cell_rows - 1, 0));
}

void prompt_text_and_block_cursor() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);

    shell.write_prompt(1, "Workbench3.1");
    CHECK(cell_is(r, bm, ix, iy, 0, 0, '1'));
    CHECK(cell_is(r, bm, ix, iy, 1, 0, '.'));
    CHECK(cell_is(r, bm, ix, iy, 2, 0, 'W'));
    CHECK(cell_is(r, bm, ix, iy, 15, 0, '>'));
    CHECK(cell_all(r, bm, ix, iy, 16, 0, 0));
    CHECK(cell_all(r, bm, ix, iy, 17, 0, 1));
}

void prompt_starts_on_a_new_row_when_mid_line() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);

    write_str(shell, "AB");
    shell.write_prompt(1, "X");
    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'A'));
    CHECK(cell_is(r, bm, ix, iy, 1, 0, 'B'));
    CHECK(cell_is(r, bm, ix, iy, 0, 1, '1'));
    CHECK(cell_is(r, bm, ix, iy, 1, 1, '.'));
    CHECK(cell_is(r, bm, ix, iy, 2, 1, 'X'));
    CHECK(cell_is(r, bm, ix, iy, 3, 1, ':'));
    CHECK(cell_is(r, bm, ix, iy, 4, 1, '>'));
    CHECK(cell_all(r, bm, ix, iy, 5, 1, 0));
    CHECK(cell_all(r, bm, ix, iy, 6, 1, 1));
}

void non_printable_bytes_draw_question_mark() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);

    const u8 text[] = {0x01, 0x7F};
    shell.write(text, 2);
    CHECK(cell_is(r, bm, ix, iy, 0, 0, '?'));
    CHECK(cell_is(r, bm, ix, iy, 1, 0, '?'));
}

void sink_forwards_to_write() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);

    ConsoleWrite out = &ShellWindow::sink;
    const u8 text[] = {'H'};
    out(&shell, text, 1);
    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'H'));
}

void close_is_idempotent() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    CHECK(shell.is_open());
    shell.close();
    CHECK(!shell.is_open());
    CHECK_EQ(r.get32(screen + sc_FirstWindow), 0u);
    const usize refreshes = r.refreshes.size();
    shell.close();
    CHECK_EQ(r.refreshes.size(), refreshes);
    CHECK_EQ(r.get32(screen + sc_FirstWindow), 0u);

    const u8 text[] = {'H'};
    shell.write(text, 1);
}

}

int main() {
    open_gives_a_window_on_the_front_screen();
    open_without_a_screen_returns_false();
    text_lands_in_cell_0_0_of_the_inner_area();
    newline_cr_and_tab_move_the_cursor();
    a_line_wider_than_the_window_wraps();
    overflow_scrolls_by_one_cell_row();
    prompt_text_and_block_cursor();
    prompt_starts_on_a_new_row_when_mid_line();
    non_printable_bytes_draw_question_mark();
    sink_forwards_to_write();
    close_is_idempotent();
    return check::finish("test_shell_window");
}
