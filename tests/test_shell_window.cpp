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
#include "hid_mouse.hpp"
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
    const u32 screen = r.result(Base::Intuition, intuition_lvo::OpenScreen);
    r.intuition.set_default_screen(screen);
    return screen;
}

bool open_shell(GfxRig& r, ShellWindow& shell) {
    return shell.open(r.machine, r.exec, r.graphics, r.intuition);
}

// The TextAttr naming hasklig 13, opened and made the system default font
// before windows lay out their cells.
u32 use_hasklig_13(GfxRig& r) {
    const u32 attr = kData + 0x800;
    r.put32(attr + ta_Name, r.str(kData + 0xA00, "hasklig.font"));
    r.put16(attr + ta_YSize, 13);
    r.put8(attr + ta_Style, 0);
    r.put8(attr + ta_Flags, 0);
    const u32 font = r.graphics.open_font(attr);
    r.graphics.set_default_font(font);
    return font;
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

u32 columns(GfxRig& r, u32 window, u32 cell = 8) {
    const u32 layer = layer_of(r, window);
    const i32 min = static_cast<i16>(r.get16(layer + ly_MinX));
    const i32 max = static_cast<i16>(r.get16(layer + ly_MaxX));
    const i32 borders = static_cast<i32>(r.get8(window + wd_BorderLeft)) +
                        static_cast<i32>(r.get8(window + wd_BorderRight));
    return static_cast<u32>(max - min + 1 - borders) / cell;
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

bool cell_is_inverse(GfxRig& r, const raster::Bitmap& bm, i32 ix, i32 iy, u32 column, u32 row,
                     char ch) {
    const u8* glyph_rows = glyph(ch);
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const u32 want = ((glyph_rows[y] >> x) & 1) != 0 ? 0u : 1u;
            const i32 px = ix + static_cast<i32>(column) * 8 + static_cast<i32>(x);
            const i32 py = iy + static_cast<i32>(row) * 8 + static_cast<i32>(y);
            if (pixel(r, bm, px, py) != want) {
                return false;
            }
        }
    }
    return true;
}

bool cell_all(GfxRig& r, const raster::Bitmap& bm, i32 ix, i32 iy, u32 column, u32 row, u32 pen,
              u32 cell_w = 8, u32 cell_h = 8) {
    for (u32 y = 0; y < cell_h; ++y) {
        for (u32 x = 0; x < cell_w; ++x) {
            const i32 px = ix + static_cast<i32>(column * cell_w + x);
            const i32 py = iy + static_cast<i32>(row * cell_h + y);
            if (pixel(r, bm, px, py) != pen) {
                return false;
            }
        }
    }
    return true;
}

// Whether the glyph of ch fills the cell at x, y as the font's own bitmap
// has it, the cell being the font's x_size by y_size with its top row at y.
bool rom_cell_is(GfxRig& r, const raster::Bitmap& bm, u32 font, i32 x, i32 y, char ch) {
    const u32 width = r.get16(font + tf_XSize);
    const u32 height = r.get16(font + tf_YSize);
    const u32 lo = r.get8(font + tf_LoChar);
    const u32 data = r.get32(font + tf_CharData);
    const u32 modulo = r.get16(font + tf_Modulo);
    for (u32 row = 0; row < height; ++row) {
        for (u32 col = 0; col < width; ++col) {
            const u32 bit = (static_cast<u8>(ch) - lo) * width + col;
            const u32 set = (r.get8(data + row * modulo + bit / 8) >> (7 - bit % 8)) & 1u;
            if (pixel(r, bm, x + static_cast<i32>(col), y + static_cast<i32>(row)) != set) {
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
    CHECK(r.read_str(r.get32(window + wd_Title)) == "HombreShell");
    CHECK_EQ(r.get32(layer_of(r, window) + ly_Window), window);
    CHECK_EQ(inner_x(r, window), 2);
    // Below the screen title bar (14 rows and its line) and the window's 15-px top border.
    CHECK_EQ(inner_y(r, window), 30);    // The shell takes the keyboard, so its window opens active.
    CHECK_EQ(r.intuition.active_window(), window);
}

void open_without_a_screen_returns_false() {
    GfxRig r;
    ShellWindow shell;
    CHECK(!shell.open(r.machine, r.exec, r.graphics, r.intuition));
    CHECK(!shell.is_open());
    CHECK_EQ(r.display.front_screen, 0u);
}

void hasklig_13_default_font_lays_out_its_cells() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    const u32 font = use_hasklig_13(r);
    CHECK(font != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);
    const u32 cell_w = r.get16(font + tf_XSize);
    const u32 cell_h = r.get16(font + tf_YSize);
    CHECK_EQ(cell_w, 7u);
    CHECK_EQ(cell_h, 13u);

    // On the 640x400 test screen the window has 624 px inside its left
    // and right borders and 368 between its top and bottom ones.
    const u32 cells = columns(r, window, cell_w);
    CHECK(cells < 200);
    CHECK_EQ(cells, 624u / cell_w);
    CHECK_EQ(shell.rows(), 368u / cell_h);

    // A line of exactly the column count wraps; its next character lands
    // one cell height down, on the cell's top row.
    u8 text[200];
    for (u32 i = 0; i < cells; ++i) {
        text[i] = 'A';
    }
    text[cells] = 'B';
    shell.write(text, cells + 1);
    CHECK(rom_cell_is(r, bm, font, ix, iy, 'A'));
    CHECK(rom_cell_is(r, bm, font, ix, iy + static_cast<i32>(cell_h), 'B'));

    // echo draws the byte and the block cursor one cell after it.
    shell.echo('C');
    CHECK(rom_cell_is(r, bm, font, ix + static_cast<i32>(cell_w),
                      iy + static_cast<i32>(cell_h), 'C'));
    CHECK(cell_all(r, bm, ix, iy, 2, 1, 1, cell_w, cell_h));
    CHECK_EQ(pixel(r, bm, ix + static_cast<i32>(3 * cell_w),
                   iy + static_cast<i32>(cell_h)), 0u);
    CHECK_EQ(pixel(r, bm, ix + static_cast<i32>(2 * cell_w),
                   iy + static_cast<i32>(2 * cell_h)), 0u);
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
    CHECK_EQ(r.get8(window + wd_BorderRight), 14u);
    CHECK_EQ(r.get8(window + wd_BorderBottom), 2u);
    CHECK_EQ(rows(r, window), 46u);
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
    const u32 c = (400 - 2 - 12) / 8;
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

void press(GfxRig& r, i32 x, i32 y) {
    r.intuition.pointer_event(x, y, kMouseLeft, kMouseLeft, 0, false);
}

void move_to(GfxRig& r, i32 x, i32 y) {
    r.intuition.pointer_event(x, y, kMouseLeft, 0, 0, true);
}

void release(GfxRig& r, i32 x, i32 y) {
    r.intuition.pointer_event(x, y, 0, 0, kMouseLeft, false);
}

void write_lines(ShellWindow& shell, u32 count, char ch) {
    const u8 line[2] = {static_cast<u8>(ch), '\n'};
    for (u32 i = 0; i < count; ++i) {
        shell.write(line, 2);
    }
}

struct ViewLog {
    std::vector<u32> views;
};

void record_view(void* ctx, u32 view) {
    static_cast<ViewLog*>(ctx)->views.push_back(view);
}

void shell_has_scroll_gadget() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    CHECK(window != 0);

    const u32 gad = r.get32(window + wd_FirstGadget);
    CHECK(gad != 0);
    CHECK_EQ(static_cast<i16>(r.get16(gad + gg_LeftEdge)), static_cast<i16>(-9));
    CHECK_EQ(r.get16(gad + gg_TopEdge),
             static_cast<u32>(r.get8(window + wd_BorderTop)) + 1u);
    CHECK_EQ(r.get16(gad + gg_Width), 8u);
    const i32 expected_h = -(static_cast<i32>(r.get8(window + wd_BorderTop)) + 13);
    CHECK_EQ(static_cast<i16>(r.get16(gad + gg_Height)), static_cast<i16>(expected_h));

    const raster::Bitmap bm = screen_bitmap(r, screen);
    const i32 wl = static_cast<i16>(r.get16(window + wd_LeftEdge));
    const i32 wt = static_cast<i16>(r.get16(window + wd_TopEdge));
    const i32 ww = r.get16(window + wd_Width);
    const i32 wh = r.get16(window + wd_Height);
    const i32 border_right = static_cast<i32>(r.get8(window + wd_BorderRight));
    const i32 box_x0 = ww - 1 + static_cast<i16>(r.get16(gad + gg_LeftEdge));
    const i32 box_y0 = static_cast<i16>(r.get16(gad + gg_TopEdge));
    const i32 box_x1 = box_x0 + static_cast<i16>(r.get16(gad + gg_Width)) - 1;
    const i32 box_y1 = box_y0 + (wh + static_cast<i16>(r.get16(gad + gg_Height))) - 1;
    const Rect size_box = Intuition::size_box_rect(0, 0, ww, wh, r.get32(window + wd_Flags));
    CHECK(box_x0 >= ww - border_right);
    CHECK(box_x1 <= ww - 2);
    CHECK(box_y0 > 14);
    CHECK(box_y1 < size_box.y0);
    CHECK_EQ(box_y1, size_box.y0 - 2);

    // The size gadget shows its 3.1 corner and light box, and dragging it resizes.
    CHECK_EQ(pixel(r, bm, wl + ww - 4, wt + wh - 4), 1u);
    CHECK_EQ(pixel(r, bm, wl + ww - 5, wt + wh - 5), 2u);
    press(r, wl + ww - 5, wt + wh - 5);
    move_to(r, wl + ww - 45, wt + wh - 25);
    release(r, wl + ww - 45, wt + wh - 25);
    CHECK_EQ(r.get16(window + wd_Width), static_cast<u32>(ww - 40));
    CHECK_EQ(r.get16(window + wd_Height), static_cast<u32>(wh - 20));
    CHECK_EQ(r.get16(gad + gg_Flags), GFLG_RELRIGHT | GFLG_RELHEIGHT);
    CHECK_EQ(r.get16(gad + gg_Activation),
             GACT_RIGHTBORDER | GACT_IMMEDIATE | GACT_FOLLOWMOUSE | GACT_RELVERIFY);
    CHECK_EQ(r.get16(gad + gg_GadgetType) & GTYP_GTYPEMASK, GTYP_PROPGADGET);

    const u32 pi = r.get32(gad + gg_SpecialInfo);
    CHECK(pi != 0);
    CHECK_EQ(r.get16(pi + pi_Flags), AUTOKNOB | FREEVERT | PROPNEWLOOK);

    shell.close();
    CHECK_EQ(r.get32(screen + sc_FirstWindow), 0u);
}

void scroll_by_shows_older_rows() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);
    const u32 visible = rows(r, window);
    CHECK(visible >= 10);

    for (u32 i = 0; i < visible + 5; ++i) {
        const u8 line[2] = {static_cast<u8>('A' + (i % 26)), '\n'};
        shell.write(line, 2);
    }
    CHECK(cell_is(r, bm, ix, iy, 0, 0, static_cast<char>('A' + 6)));

    shell.scroll_by(6);
    CHECK_EQ(shell.view_from_bottom(), 6u);
    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'A'));
    CHECK(cell_is(r, bm, ix, iy, 0, 1, 'B'));
    CHECK(cell_is(r, bm, ix, iy, 0, 2, 'C'));

    shell.scroll_by(-3);
    CHECK_EQ(shell.view_from_bottom(), 3u);
    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'D'));
}

void scroll_clamps_at_top_and_bottom() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const u32 visible = rows(r, window);

    shell.scroll_by(10);
    CHECK_EQ(shell.view_from_bottom(), 0u);
    shell.scroll_to_top();
    CHECK_EQ(shell.view_from_bottom(), 0u);

    write_lines(shell, visible + 10, 'X');
    shell.scroll_by(1000);
    CHECK_EQ(shell.view_from_bottom(), 11u);
    shell.scroll_by(-1000);
    CHECK_EQ(shell.view_from_bottom(), 0u);
    shell.scroll_to_top();
    CHECK_EQ(shell.view_from_bottom(), 11u);
    shell.scroll_to_bottom();
    CHECK_EQ(shell.view_from_bottom(), 0u);
}

void knob_reflects_view() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const u32 visible = rows(r, window);
    const u32 gad = r.get32(window + wd_FirstGadget);
    const u32 pi = r.get32(gad + gg_SpecialInfo);
    CHECK(pi != 0);

    CHECK_EQ(r.get16(pi + pi_VertBody), MAXBODY);
    CHECK_EQ(r.get16(pi + pi_VertPot), 0u);

    write_lines(shell, visible * 2 - 1, 'X');
    CHECK_EQ(r.get16(pi + pi_VertBody), (MAXBODY * visible) / (visible * 2));
    CHECK_EQ(r.get16(pi + pi_VertPot), MAXPOT);

    shell.scroll_to_top();
    CHECK_EQ(r.get16(pi + pi_VertPot), 0u);

    const u32 step = visible / 3;
    shell.scroll_by(-static_cast<i32>(step));
    CHECK_EQ(r.get16(pi + pi_VertPot), (MAXPOT * step) / visible);
}

void knob_drag_scrolls() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);
    const u32 visible = rows(r, window);

    write_lines(shell, 1, 'A');
    write_lines(shell, visible * 2 - 2, 'B');

    const i32 win_left = static_cast<i16>(r.get16(window + wd_LeftEdge));
    const i32 win_top = static_cast<i16>(r.get16(window + wd_TopEdge));
    const i32 win_w = static_cast<i16>(r.get16(window + wd_Width));
    const i32 win_h = static_cast<i16>(r.get16(window + wd_Height));
    const i32 knob_x = win_left + win_w - 5;
    const i32 knob_bottom_y = win_top + win_h - 15;
    const i32 track_top_y = win_top + 18;

    press(r, knob_x, knob_bottom_y);
    move_to(r, knob_x, (knob_bottom_y + track_top_y) / 2);
    CHECK(shell.view_from_bottom() > 0 && shell.view_from_bottom() < visible);
    move_to(r, knob_x, track_top_y);
    release(r, knob_x, track_top_y);

    CHECK_EQ(shell.view_from_bottom(), visible);
    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'A'));
}

void typing_returns_to_bottom() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);
    const u32 visible = rows(r, window);
    const u32 gad = r.get32(window + wd_FirstGadget);
    const u32 pi = r.get32(gad + gg_SpecialInfo);

    write_lines(shell, visible + 10, 'L');
    shell.write_prompt(1, "X");
    shell.echo('a');

    shell.scroll_by(5);
    CHECK_EQ(shell.view_from_bottom(), 5u);
    shell.echo('b');
    CHECK_EQ(shell.view_from_bottom(), 0u);

    shell.scroll_by(5);
    CHECK_EQ(shell.view_from_bottom(), 5u);
    shell.erase_last();
    CHECK_EQ(shell.view_from_bottom(), 0u);

    shell.scroll_by(5);
    CHECK_EQ(shell.view_from_bottom(), 5u);
    shell.end_line();
    CHECK_EQ(shell.view_from_bottom(), 0u);

    shell.scroll_by(5);
    CHECK_EQ(shell.view_from_bottom(), 5u);
    shell.write_prompt(1, "Y");
    CHECK_EQ(shell.view_from_bottom(), 0u);
    CHECK_EQ(r.get16(pi + pi_VertPot), MAXPOT);
    CHECK(cell_is(r, bm, ix, iy, 2, visible - 1, 'Y'));
}

void scrolled_view_keeps_content_on_output() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);
    const u32 visible = rows(r, window);
    const u32 gad = r.get32(window + wd_FirstGadget);
    const u32 pi = r.get32(gad + gg_SpecialInfo);

    write_lines(shell, 1, 'A');
    write_lines(shell, 1, 'B');
    write_lines(shell, visible + 3, 'Z');

    shell.scroll_to_top();
    CHECK_EQ(shell.view_from_bottom(), 6u);
    const u32 body_before = r.get16(pi + pi_VertBody);
    const u32 pot_before = r.get16(pi + pi_VertPot);

    write_lines(shell, 10, 'N');
    CHECK_EQ(shell.view_from_bottom(), 16u);
    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'A'));
    CHECK(cell_is(r, bm, ix, iy, 0, 1, 'B'));
    CHECK(r.get16(pi + pi_VertBody) < body_before);
    // The view stays on the oldest lines, so the knob stays at the top.
    CHECK_EQ(pot_before, 0u);
    CHECK_EQ(r.get16(pi + pi_VertPot), 0u);
}

void eviction_clamps_view() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    r.intuition.size_window_to(window, 320, 100);
    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);
    const u32 cols = columns(r, window);
    const u32 visible = rows(r, window);
    CHECK(cols > 0 && cols <= 80);

    u8 long_line[256];
    const u32 span = cols * 3;
    for (u32 i = 0; i < span; ++i) {
        long_line[i] = 'L';
    }
    long_line[span] = '\n';
    shell.write(long_line, span + 1);
    write_lines(shell, ShellText::kMaxLines - 2, 'S');

    shell.scroll_to_top();
    CHECK_EQ(shell.view_from_bottom(), (ShellText::kMaxLines + 2) - visible);

    write_lines(shell, 1, 'N');
    CHECK_EQ(shell.view_from_bottom(), ShellText::kMaxLines - visible);
    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'S'));
}

void resize_while_scrolled_keeps_top_line() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);

    for (u32 i = 0; i < 60; ++i) {
        const u8 line[2] = {static_cast<u8>('A' + (i % 26)), '\n'};
        shell.write(line, 2);
    }
    const u32 vis0 = rows(r, window);
    CHECK(vis0 < 51);
    shell.scroll_by(static_cast<i32>(61u - vis0 - 10u));
    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'K'));

    r.intuition.size_window_to(window, 400, 200);
    const u32 vis1 = rows(r, window);
    CHECK(vis1 < vis0);
    CHECK_EQ(shell.view_from_bottom(), 61u - vis1 - 10u);
    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'K'));

    r.intuition.size_window_to(window, 400, 350);
    const u32 vis2 = rows(r, window);
    CHECK(vis2 > vis1);
    CHECK_EQ(shell.view_from_bottom(), 61u - vis2 - 10u);
    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'K'));
}

void listener_called_on_user_changes_only() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);

    ViewLog log;
    shell.set_view_listener(&record_view, &log);

    write_lines(shell, 60, 'A');
    CHECK(log.views.empty());

    shell.scroll_by(5);
    CHECK_EQ(log.views.size(), 1u);
    CHECK_EQ(log.views[0], 5u);

    write_lines(shell, 10, 'B');
    CHECK_EQ(shell.view_from_bottom(), 15u);
    CHECK_EQ(log.views.size(), 1u);

    shell.scroll_to_top();
    CHECK_EQ(log.views.size(), 2u);
    CHECK_EQ(log.views[1], shell.view_from_bottom());

    write_lines(shell, 1050, 'C');
    CHECK_EQ(log.views.size(), 2u);

    const i32 win_left = static_cast<i16>(r.get16(window + wd_LeftEdge));
    const i32 win_top = static_cast<i16>(r.get16(window + wd_TopEdge));
    const i32 win_w = static_cast<i16>(r.get16(window + wd_Width));
    const i32 win_h = static_cast<i16>(r.get16(window + wd_Height));
    const i32 knob_x = win_left + win_w - 5;
    const i32 below_y = win_top + win_h - 15;
    press(r, knob_x, below_y);
    release(r, knob_x, below_y);
    CHECK_EQ(log.views.size(), 3u);
    CHECK_EQ(log.views[2], shell.view_from_bottom());

    r.intuition.size_window_to(window, 400, 200);
    CHECK_EQ(log.views.size(), 4u);
    CHECK_EQ(log.views[3], shell.view_from_bottom());

    shell.echo('x');
    CHECK_EQ(log.views.size(), 5u);
    CHECK_EQ(log.views[4], 0u);

    r.intuition.size_window_to(window, 500, 300);
    CHECK_EQ(log.views.size(), 5u);
}

void resize_width_while_scrolled_keeps_top_line() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const i32 ix = inner_x(r, window);
    const i32 iy = inner_y(r, window);
    const raster::Bitmap bm = screen_bitmap(r, screen);

    for (u32 i = 0; i < 60; ++i) {
        u8 line[101];
        line[0] = static_cast<u8>('0' + i / 10);
        line[1] = static_cast<u8>('0' + i % 10);
        line[2] = ':';
        for (u32 j = 3; j < 100; ++j) {
            line[j] = static_cast<u8>('a' + (j - 3) % 26);
        }
        line[100] = '\n';
        shell.write(line, 101);
    }

    const u32 cols0 = columns(r, window);
    const u32 vis0 = rows(r, window);
    CHECK(cols0 > 50 && cols0 < 100);
    const u32 rows_per_line0 = (100u - 1u) / cols0 + 1u;
    const u32 total0 = 60u * rows_per_line0 + 1u;
    CHECK(total0 > vis0 + 10u * rows_per_line0);
    shell.scroll_by(static_cast<i32>(total0 - vis0 - 10u * rows_per_line0));
    CHECK(cell_is(r, bm, ix, iy, 0, 0, '1'));
    CHECK(cell_is(r, bm, ix, iy, 1, 0, '0'));
    CHECK(cell_is(r, bm, ix, iy, 2, 0, ':'));
    CHECK(cell_is(r, bm, ix, iy, 3, 0, 'a'));

    const i32 cur_h = static_cast<i16>(r.get16(window + wd_Height));
    r.intuition.size_window_to(window, 320, cur_h);
    CHECK(columns(r, window) < cols0);
    CHECK(cell_is(r, bm, ix, iy, 0, 0, '1'));
    CHECK(cell_is(r, bm, ix, iy, 1, 0, '0'));
    CHECK(cell_is(r, bm, ix, iy, 2, 0, ':'));
    CHECK(cell_is(r, bm, ix, iy, 3, 0, 'a'));
}

void scroll_composes_once() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const u32 visible = rows(r, window);
    CHECK(visible >= 10);

    write_lines(shell, visible + 20, 'A');
    const usize before = r.refreshes.size();
    shell.scroll_by(5);
    CHECK_EQ(r.refreshes.size(), before + 1);
    CHECK_EQ(r.refreshed_screens.back(), screen);

    const i32 wl = static_cast<i16>(r.get16(window + wd_LeftEdge));
    const i32 wt = static_cast<i16>(r.get16(window + wd_TopEdge));
    const i32 ww = static_cast<i16>(r.get16(window + wd_Width));
    const i32 wh = static_cast<i16>(r.get16(window + wd_Height));
    const Rect& dirty = r.refreshes.back();
    CHECK(dirty.x0 >= wl && dirty.y0 >= wt);
    CHECK(dirty.x1 < wl + ww && dirty.y1 < wt + wh);
}

void knob_moves_when_store_is_full() {
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell_window(r, screen);
    const u32 gad = r.get32(window + wd_FirstGadget);
    const u32 pi = r.get32(gad + gg_SpecialInfo);
    CHECK(pi != 0);

    write_lines(shell, ShellText::kMaxLines + 10, 'A');
    shell.scroll_by(500);
    CHECK_EQ(shell.view_from_bottom(), 500u);
    const u32 pot_before = r.get16(pi + pi_VertPot);
    CHECK(pot_before > 0u);

    write_lines(shell, 50, 'B');
    CHECK_EQ(shell.view_from_bottom(), 550u);
    CHECK(r.get16(pi + pi_VertPot) < pot_before);
}

}

void cursor_back_shows_an_inverse_cell() {
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
    shell.echo('C');
    shell.set_cursor_back(1);
    CHECK(cell_is(r, bm, ix, iy, 1, 0, 'B'));
    CHECK(cell_is_inverse(r, bm, ix, iy, 2, 0, 'C'));
    CHECK(cell_all(r, bm, ix, iy, 3, 0, 0));

    shell.set_cursor_back(3);
    CHECK(cell_is_inverse(r, bm, ix, iy, 0, 0, 'A'));
    CHECK(cell_is(r, bm, ix, iy, 2, 0, 'C'));

    // Past the start of the input it stops at the first typed byte.
    shell.set_cursor_back(9);
    CHECK(cell_is_inverse(r, bm, ix, iy, 0, 0, 'A'));

    shell.set_cursor_back(0);
    CHECK(cell_is(r, bm, ix, iy, 0, 0, 'A'));
    CHECK(cell_all(r, bm, ix, iy, 3, 0, 1));
}

void echo_after_cursor_back_puts_the_block_at_the_end() {
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
    shell.set_cursor_back(1);
    shell.echo('C');
    CHECK(cell_is(r, bm, ix, iy, 1, 0, 'B'));
    CHECK(cell_is(r, bm, ix, iy, 2, 0, 'C'));
    CHECK(cell_all(r, bm, ix, iy, 3, 0, 1));

    shell.set_cursor_back(2);
    shell.erase_last();
    CHECK(cell_is(r, bm, ix, iy, 1, 0, 'B'));
    CHECK(cell_all(r, bm, ix, iy, 2, 0, 1));
}

void cursor_back_crosses_a_wrapped_row() {
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

    for (u32 i = 0; i < cells; ++i) {
        shell.echo('A');
    }
    shell.echo('B');
    shell.set_cursor_back(2);
    CHECK(cell_is_inverse(r, bm, ix, iy, cells - 1, 0, 'A'));
    CHECK(cell_is(r, bm, ix, iy, 0, 1, 'B'));
    CHECK(cell_all(r, bm, ix, iy, 1, 1, 0));
}

void shell_opens_on_the_default_screen_not_the_front_one() {
    GfxRig r;
    const u32 first = open_screen(r);
    CHECK(first != 0);
    put_new_screen(r, kNewScreen, 640, 400);
    r.cpu().a[0] = kNewScreen;
    const u32 second = r.result(Base::Intuition, intuition_lvo::OpenScreen);
    CHECK(second != 0);
    CHECK_EQ(r.display.front_screen, second);
    CHECK_EQ(r.intuition.default_screen(), first);

    ShellWindow shell;
    CHECK(open_shell(r, shell));
    CHECK(shell.is_open());
    const u32 window = shell.window();
    CHECK_EQ(r.get32(window + wd_WScreen), first);
    CHECK_EQ(r.get32(first + sc_FirstWindow), window);
    CHECK_EQ(r.get32(second + sc_FirstWindow), 0u);
}

// IPrefs' reset of the Workbench to a mode of the given depth, in the mode's
// own size: SetIPrefs type 1, CloseWorkBench, which has to succeed, and
// OpenWorkBench.
void reset_workbench_to(GfxRig& r, u32 mode_id, u32 depth) {
    const u32 prefs = kData + 0x900;
    r.put32(prefs, mode_id);
    r.put16(prefs + 4, 0xFFFF);
    r.put16(prefs + 6, 0xFFFF);
    r.put16(prefs + 8, depth);
    r.put16(prefs + 10, 3);
    r.cpu().a[0] = prefs;
    r.cpu().d[0] = 12;
    r.cpu().d[1] = 1;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::SetIPrefs), 0u);
    r.cpu().a[0] = 0;
    CHECK_EQ(r.result(Base::Intuition, intuition_lvo::CloseWorkBench), 1u);
    r.cpu().a[0] = 0;
    CHECK(r.result(Base::Intuition, intuition_lvo::OpenWorkBench) != 0);
}

void shell_stays_through_a_workbench_reset_and_lays_out_its_text_again() {
    constexpr u32 kHighRes = 0x8000;
    constexpr u32 kPalHiresLace = 0x29004;
    GfxRig r;
    const u32 screen = open_screen(r);
    CHECK(screen != 0);
    ShellWindow shell;
    CHECK(open_shell(r, shell));
    const u32 window = shell.window();
    write_str(shell, "reset");

    // High Res has 200 rows, not the 400 the window filled below the bar.
    reset_workbench_to(r, kHighRes, 2);
    CHECK(shell.is_open());
    CHECK_EQ(r.get16(screen + sc_Height), 200u);
    CHECK_EQ(r.get16(window + wd_TopEdge), 0u);
    CHECK_EQ(r.get16(window + wd_Width), 640u);
    CHECK_EQ(r.get16(window + wd_Height), 200u);
    CHECK_EQ(rows(r, window), 22u);
    CHECK_EQ(shell.rows(), 22u);

    // What it had written is drawn again, and what follows goes after it.
    raster::Bitmap bm = screen_bitmap(r, screen);
    CHECK(cell_is(r, bm, inner_x(r, window), inner_y(r, window), 0, 0, 'r'));
    CHECK(cell_is(r, bm, inner_x(r, window), inner_y(r, window), 4, 0, 't'));
    write_str(shell, "!");
    bm = screen_bitmap(r, screen);
    CHECK(cell_is(r, bm, inner_x(r, window), inner_y(r, window), 5, 0, '!'));

    // A larger screen of four planes: the window keeps its size and may now
    // grow to the screen's 512 rows.
    reset_workbench_to(r, kPalHiresLace, 4);
    CHECK_EQ(r.get16(screen + sc_Height), 512u);
    CHECK_EQ(r.get16(window + wd_Height), 200u);
    CHECK_EQ(r.get16(window + wd_MaxHeight), 512u);
    bm = screen_bitmap(r, screen);
    CHECK_EQ(bm.depth, 4u);
    CHECK(cell_is(r, bm, inner_x(r, window), inner_y(r, window), 0, 0, 'r'));
    r.intuition.size_window_to(window, 640, 500);
    CHECK_EQ(shell.rows(), 60u);
}

int main() {
    cursor_back_shows_an_inverse_cell();
    echo_after_cursor_back_puts_the_block_at_the_end();
    cursor_back_crosses_a_wrapped_row();
    open_gives_a_window_on_the_front_screen();
    open_without_a_screen_returns_false();
    hasklig_13_default_font_lays_out_its_cells();
    shell_opens_on_the_default_screen_not_the_front_one();
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
    shell_has_scroll_gadget();
    scroll_by_shows_older_rows();
    scroll_clamps_at_top_and_bottom();
    knob_reflects_view();
    knob_drag_scrolls();
    typing_returns_to_bottom();
    scrolled_view_keeps_content_on_output();
    eviction_clamps_view();
    resize_while_scrolled_keeps_top_line();
    listener_called_on_user_changes_only();
    resize_width_while_scrolled_keeps_top_line();
    scroll_composes_once();
    knob_moves_when_store_is_full();
    shell_stays_through_a_workbench_reset_and_lays_out_its_text_again();
    return check::finish("test_shell_window");
}
