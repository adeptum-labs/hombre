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
#include "shell_window.hpp"

#include <vector>

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
    return static_cast<i16>(r.get16(layer_of(r, window) + ly_MinX)) +
           static_cast<i32>(r.get8(window + wd_BorderLeft));
}

i32 inner_y(GfxRig& r, u32 window) {
    return static_cast<i16>(r.get16(layer_of(r, window) + ly_MinY)) +
           static_cast<i32>(r.get8(window + wd_BorderTop));
}

u32 columns(GfxRig& r, u32 window) {
    const u32 layer = layer_of(r, window);
    const i32 min = static_cast<i16>(r.get16(layer + ly_MinX));
    const i32 max = static_cast<i16>(r.get16(layer + ly_MaxX));
    const i32 borders = static_cast<i32>(r.get8(window + wd_BorderLeft)) +
                        static_cast<i32>(r.get8(window + wd_BorderRight));
    return static_cast<u32>(max - min + 1 - borders) / 8;
}

u32 rows(GfxRig& r, u32 window) {
    const u32 layer = layer_of(r, window);
    const i32 min = static_cast<i16>(r.get16(layer + ly_MinY));
    const i32 max = static_cast<i16>(r.get16(layer + ly_MaxY));
    const i32 borders = static_cast<i32>(r.get8(window + wd_BorderTop)) +
                        static_cast<i32>(r.get8(window + wd_BorderBottom));
    return static_cast<u32>(max - min + 1 - borders) / 8;
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

std::vector<u8> screen_planes(GfxRig& r, u32 screen) {
    const raster::Bitmap bm = screen_bitmap(r, screen);
    const u32 bytes = bm.bytes_per_row * bm.rows;
    std::vector<u8> out(bytes * 2);
    for (u32 i = 0; i < bytes; ++i) {
        out[i] = static_cast<u8>(r.get8(bm.planes[0] + i));
        out[bytes + i] = static_cast<u8>(r.get8(bm.planes[1] + i));
    }
    return out;
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
    const u32 flags = r.get32(window + wd_Flags) &
                      (WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_DRAGBAR);
    CHECK_EQ(flags, static_cast<u32>(WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_DRAGBAR));
    CHECK(r.read_str(r.get32(window + wd_Title)) == "AmigaShell");
    CHECK_EQ(r.get32(layer_of(r, window) + ly_Window), window);
    CHECK_EQ(inner_x(r, window), 1);
    // Below the screen title bar (10 rows and its line), as a real shell window.
    CHECK_EQ(inner_y(r, window), 22);
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

void echo_draws_the_char_and_moves_the_cursor_block() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);

    shell.echo('A');
    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'A'));
    CHECK(cell_all(r, bm, ix, iy, 1, 0, 1));
    CHECK(cell_all(r, bm, ix, iy, 2, 0, 0));

    shell.echo('B');
    CHECK(cell_is(r, bm, ix, iy, 1, 0, 'B'));
    CHECK(cell_all(r, bm, ix, iy, 2, 0, 1));
    CHECK(cell_all(r, bm, ix, iy, 3, 0, 0));
}

void echo_wraps_at_the_last_column_and_scrolls() {
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
    const u32 cell_rows = rows(r, window);
    CHECK(cells >= 4 && cells < 200);
    CHECK(cell_rows >= 4);

    for (u32 i = 0; i < cells; ++i) {
        shell.echo('A');
    }
    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'A'));
    CHECK(cell_is(r, bm, ix, iy, cells - 1, 0, 'A'));
    CHECK(cell_all(r, bm, ix, iy, 0, 1, 1));

    // End the filled line so the row below stays blank, then fill the last
    // row, which has to scroll at the wrap.
    shell.end_line();
    for (u32 row = 2; row + 1 < cell_rows; ++row) {
        write_str(shell, "\n");
    }
    for (u32 i = 0; i < cells; ++i) {
        shell.echo('B');
    }
    CHECK(cell_all(r, bm, ix, iy, 0, 0, 0));
    CHECK(cell_is(r, bm, ix, iy, 0, cell_rows - 2, 'B'));
    CHECK(cell_is(r, bm, ix, iy, cells - 1, cell_rows - 2, 'B'));
    CHECK(cell_all(r, bm, ix, iy, 0, cell_rows - 1, 1));
}

void erase_last_clears_the_cell_and_moves_the_block_back() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);

    shell.echo('A');
    shell.echo('B');
    CHECK(cell_is(r, bm, ix, iy, 1, 0, 'B'));
    CHECK(cell_all(r, bm, ix, iy, 2, 0, 1));

    shell.erase_last();
    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'A'));
    CHECK(cell_all(r, bm, ix, iy, 1, 0, 1));
    CHECK(cell_all(r, bm, ix, iy, 2, 0, 0));
}

void erase_last_at_the_prompt_start_does_nothing() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));

    shell.write_prompt(1, "X");
    const std::vector<u8> prompt = screen_planes(r, screen);
    shell.erase_last();
    CHECK(screen_planes(r, screen) == prompt);

    shell.echo('A');
    shell.erase_last();
    CHECK(screen_planes(r, screen) == prompt);
    shell.erase_last();
    CHECK(screen_planes(r, screen) == prompt);
}

void erase_last_across_a_wrapped_row() {
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
    CHECK(cells >= 4 && cells < 200);

    for (u32 i = 0; i < cells; ++i) {
        shell.echo('A');
    }
    CHECK(cell_all(r, bm, ix, iy, 0, 1, 1));

    shell.erase_last();
    CHECK(cell_all(r, bm, ix, iy, 0, 1, 0));
    CHECK(cell_all(r, bm, ix, iy, cells - 1, 0, 1));
    CHECK(cell_is(r, bm, ix, iy, cells - 2, 0, 'A'));

    for (u32 i = 1; i < cells; ++i) {
        shell.erase_last();
    }
    CHECK(cell_all(r, bm, ix, iy, 0, 0, 1));
    CHECK(cell_all(r, bm, ix, iy, cells - 1, 0, 0));

    const std::vector<u8> erased = screen_planes(r, screen);
    shell.erase_last();
    CHECK(screen_planes(r, screen) == erased);
}

void end_line_clears_the_cursor_and_starts_a_new_row() {
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

    shell.echo('A');
    shell.end_line();
    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'A'));
    CHECK(cell_all(r, bm, ix, iy, 1, 0, 0));

    shell.echo('B');
    CHECK(cell_is(r, bm, ix, iy, 0, 1, 'B'));
    CHECK(cell_all(r, bm, ix, iy, 1, 1, 1));

    for (u32 row = 1; row + 1 < cell_rows; ++row) {
        shell.end_line();
    }
    CHECK(cell_all(r, bm, ix, iy, 1, 1, 0));
    shell.end_line();
    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'B'));
    CHECK(cell_all(r, bm, ix, iy, 0, cell_rows - 1, 0));
}

void echo_latin1_letter_is_not_a_question_mark() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);

    shell.echo(0xE5);
    CHECK(cell_is(r, bm, ix, iy, 0, 0, static_cast<char>(0xE5)));
    CHECK(!cell_is(r, bm, ix, iy, 0, 0, '?'));
    CHECK(cell_all(r, bm, ix, iy, 1, 0, 1));
}

void typing_functions_do_nothing_after_the_window_closed() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    CHECK(window != 0);

    r.intuition.close_window(window);
    CHECK(!shell.is_open());
    const std::vector<u8> before = screen_planes(r, screen);
    const usize refreshes = r.refreshes.size();

    shell.echo('A');
    shell.erase_last();
    shell.end_line();
    shell.print_line("gone");
    CHECK(screen_planes(r, screen) == before);
    CHECK_EQ(r.refreshes.size(), refreshes);
}

void print_line_starts_a_fresh_row() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);
    CHECK(rows(r, window) >= 5);

    write_str(shell, "AB");
    shell.print_line("CD");
    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'A'));
    CHECK(cell_is(r, bm, ix, iy, 1, 0, 'B'));
    CHECK(cell_all(r, bm, ix, iy, 2, 0, 0));
    CHECK(cell_is(r, bm, ix, iy, 0, 1, 'C'));
    CHECK(cell_is(r, bm, ix, iy, 1, 1, 'D'));
    CHECK(cell_all(r, bm, ix, iy, 2, 1, 0));

    shell.print_line("EF");
    CHECK(cell_is(r, bm, ix, iy, 0, 2, 'E'));
    CHECK(cell_is(r, bm, ix, iy, 1, 2, 'F'));
    shell.echo('G');
    CHECK(cell_is(r, bm, ix, iy, 0, 3, 'G'));
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

void shell_is_not_open_after_its_window_is_closed() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    CHECK(window != 0);

    r.intuition.close_window(window);
    const std::vector<u8> before = screen_planes(r, screen);
    const usize refreshes = r.refreshes.size();
    const usize screens = r.refreshed_screens.size();

    CHECK(!shell.is_open());
    const u8 text[] = {'H'};
    shell.write(text, 1);
    shell.write_prompt(1, "X");
    shell.close();
    CHECK(screen_planes(r, screen) == before);
    CHECK_EQ(r.refreshes.size(), refreshes);
    CHECK_EQ(r.refreshed_screens.size(), screens);
}

void shell_is_not_open_after_its_screen_is_closed() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    CHECK(window != 0);

    CHECK(!r.intuition.close_screen(screen));
    CHECK_EQ(r.intuition.front_screen(), screen);
    CHECK(shell.is_open());

    r.intuition.close_window(window);
    CHECK(r.intuition.close_screen(screen));
    CHECK_EQ(r.intuition.front_screen(), 0u);
    CHECK(!shell.is_open());
    CHECK(!r.intuition.window_open(window));
}

void shell_has_size_gadget() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    CHECK(window != 0);
    CHECK_EQ(r.get8(window + wd_BorderRight), 10u);
    CHECK_EQ(r.get16(window + wd_MinWidth), 160u);
    CHECK((r.get32(window + wd_Flags) & WFLG_SIZEGADGET) != 0);
}

void shell_rewraps_on_resize() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    CHECK(window != 0);

    u8 line[71];
    for (u32 i = 0; i < 70; ++i) {
        line[i] = static_cast<u8>('A' + (i % 26));
    }
    line[70] = '\n';
    shell.write(line, 71);

    const i32 cur_h = static_cast<i16>(r.get16(window + wd_Height));
    r.intuition.size_window_to(window, 400, cur_h);

    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);
    const u32 c = (400 - 1 - 10) / 8;
    CHECK_EQ(columns(r, window), c);
    CHECK(!cell_all(r, bm, ix, iy, 0, 1, 0));
    CHECK(cell_is(r, bm, ix, iy, 0, 1, static_cast<char>(line[c])));
    CHECK(!cell_all(r, bm, ix, iy, c - 1, 0, 0));
    CHECK(cell_is(r, bm, ix, iy, c - 1, 0, static_cast<char>(line[c - 1])));
    CHECK(cell_all(r, bm, ix, iy, 70 - c, 1, 0));
}

void shell_erase_after_resize_stops_at_prompt() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    CHECK(window != 0);

    shell.write_prompt(1, "Workbench3.1");
    shell.echo('a');
    shell.echo('b');
    shell.echo('c');

    const i32 cur_h = static_cast<i16>(r.get16(window + wd_Height));
    r.intuition.size_window_to(window, 160, cur_h);

    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);
    CHECK_EQ(columns(r, window), 18u);
    CHECK(cell_is(r, bm, ix, iy, 17, 0, 'a'));
    CHECK(cell_is(r, bm, ix, iy, 0, 1, 'b'));
    CHECK(cell_is(r, bm, ix, iy, 1, 1, 'c'));
    CHECK(cell_all(r, bm, ix, iy, 2, 1, 1));

    for (u32 i = 0; i < 3; ++i) {
        shell.erase_last();
    }
    const std::vector<u8> at_prompt = screen_planes(r, screen);
    CHECK(cell_is(r, bm, ix, iy, 0, 0, '1'));
    CHECK(cell_is(r, bm, ix, iy, 1, 0, '.'));
    CHECK(cell_is(r, bm, ix, iy, 2, 0, 'W'));
    CHECK(cell_is(r, bm, ix, iy, 15, 0, '>'));
    CHECK(cell_all(r, bm, ix, iy, 16, 0, 0));
    CHECK(cell_all(r, bm, ix, iy, 17, 0, 1));
    CHECK(cell_all(r, bm, ix, iy, 0, 1, 0));

    shell.erase_last();
    shell.erase_last();
    CHECK(screen_planes(r, screen) == at_prompt);
}

// The prompt and an input of n bytes, the window then shrunk to its minimum.
void shrunk_shell_with_input(GfxRig& r, ShellWindow& shell, u32 n) {
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    CHECK(window != 0);
    shell.write_prompt(1, "X");
    for (u32 i = 0; i < n; ++i) {
        shell.echo('a');
    }
    r.intuition.size_window_to(window, 160, 50);
}

void erase_past_top_row_redraws() {
    GfxRig r;
    ShellWindow shell;
    // 6 prompt bytes and 80 typed ones: 5 rows of 18 columns in a window
    // with room for 4, so the first row of the line is above the window.
    shrunk_shell_with_input(r, shell, 80);
    for (u32 i = 0; i < 90; ++i) {
        shell.erase_last();
    }
    GfxRig fresh;
    ShellWindow empty;
    shrunk_shell_with_input(fresh, empty, 0);
    const u32 screen = r.intuition.front_screen();
    CHECK(screen_planes(r, screen) == screen_planes(fresh, fresh.intuition.front_screen()));
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
    echo_draws_the_char_and_moves_the_cursor_block();
    echo_wraps_at_the_last_column_and_scrolls();
    erase_last_clears_the_cell_and_moves_the_block_back();
    erase_last_at_the_prompt_start_does_nothing();
    erase_last_across_a_wrapped_row();
    end_line_clears_the_cursor_and_starts_a_new_row();
    echo_latin1_letter_is_not_a_question_mark();
    typing_functions_do_nothing_after_the_window_closed();
    print_line_starts_a_fresh_row();
    close_is_idempotent();
    shell_is_not_open_after_its_window_is_closed();
    shell_is_not_open_after_its_screen_is_closed();
    shell_has_size_gadget();
    shell_rewraps_on_resize();
    shell_erase_after_resize_stops_at_prompt();
    erase_past_top_row_redraws();
    return check::finish("test_shell_window");
}
