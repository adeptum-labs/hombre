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

#include "intuition.hpp"

#include "../hid_mouse.hpp"
#include "exec.hpp"
#include "graphics.hpp"
#include "raster.hpp"
#include "utility.hpp"
#include "version.hpp"

namespace hombre {

using m68k::Machine;
using native::Bus;
using i16 = __INT16_TYPE__;

namespace {

constexpr char kIntuitionName[] = "intuition.library";

// struct Screen's sc_NextScreen, the pointer before sc_FirstWindow.
constexpr u32 kScNextScreen = 0;
constexpr u32 kWBENCHSCREEN = 1;
constexpr u32 kBarHeight = 10;
constexpr u32 kTitleChunk = 64;
constexpr u32 kMaxTitleChars = 255;

// The colours of the default four-entry palette, RGB4.
constexpr u16 kPalette[4] = {0x0AAA, 0x0000, 0x0FFF, 0x068B};

// The fields of a NewScreen as the SA_ tags can override them.
struct OpenArgs {
    u32 left = 0;
    u32 top = 0;
    u32 width = 0;
    u32 height = 0;
    u32 depth = 0;
    u32 detail = 0;
    u32 block = 1;
    u32 title = 0;
};

void read_new_screen(Bus& b, u32 ns, OpenArgs& a) {
    if (ns == 0) {
        return;
    }
    a.left = b.r16(ns + ndk::ns_LeftEdge);
    a.top = b.r16(ns + ndk::ns_TopEdge);
    a.width = b.r16(ns + ndk::ns_Width);
    a.height = b.r16(ns + ndk::ns_Height);
    a.depth = b.r16(ns + ndk::ns_Depth);
    a.detail = b.r8(ns + ndk::ns_DetailPen);
    a.block = b.r8(ns + ndk::ns_BlockPen);
    a.title = b.r32(ns + ndk::ns_DefaultTitle);
}

void apply_tag(OpenArgs& a, u32 tag, u32 data) {
    switch (tag) {
    case ndk::SA_Left:
        a.left = data;
        break;
    case ndk::SA_Top:
        a.top = data;
        break;
    case ndk::SA_Width:
        a.width = data;
        break;
    case ndk::SA_Height:
        a.height = data;
        break;
    case ndk::SA_Depth:
        a.depth = data;
        break;
    case ndk::SA_DetailPen:
        a.detail = data & 0xFF;
        break;
    case ndk::SA_BlockPen:
        a.block = data & 0xFF;
        break;
    case ndk::SA_Title:
        a.title = data;
        break;
    default:
        break;
    }
}

void read_tags(Bus& b, u32 tags, OpenArgs& a) {
    u32 item = tags;
    u32 budget = Intuition::kMaxTagItems;
    while (item != 0 && budget != 0) {
        const u32 tag = b.r32(item + ndk::ti_Tag);
        const u32 data = b.r32(item + ndk::ti_Data);
        if (!b.ok()) {
            return;
        }
        --budget;
        switch (tag) {
        case ndk::TAG_DONE:
            return;
        case ndk::TAG_IGNORE:
            item += ndk::ti_SIZEOF;
            break;
        case ndk::TAG_MORE:
            item = data;
            break;
        case ndk::TAG_SKIP:
            item += ndk::ti_SIZEOF * (data + 1);
            break;
        default:
            apply_tag(a, tag, data);
            item += ndk::ti_SIZEOF;
            break;
        }
    }
}

// Copies the guest string at src into a fresh block, NUL included; 0 if
// there is no memory or the string cannot be read. bytes is the size of
// the block that was allocated.
u32 copy_title(Bus& b, Exec& exec, u32 src, u32& bytes) {
    bytes = 0;
    u32 length = 0;
    while (b.r8(src + length) != 0) {
        if (!b.ok() || ++length == kMaxTitleChars) {
            return 0;
        }
    }
    const u32 at = exec.alloc(length + 1, ndk::MEMF_PUBLIC);
    if (at == 0) {
        return 0;
    }
    for (u32 i = 0; i <= length; ++i) {
        b.w8(at + i, b.r8(src + i));
    }
    if (!b.ok()) {
        exec.free(at, length + 1);
        return 0;
    }
    bytes = length + 1;
    return at;
}

void refresh_full(DisplayState& display, Machine& m, u32 screen, u32 width, u32 height) {
    if (display.sink.refresh == nullptr) {
        return;
    }
    const Rect full{0, 0, static_cast<i32>(width) - 1, static_cast<i32>(height) - 1};
    display.sink.refresh(display.sink.ctx, m, screen, full);
}

// Draws the pen 1 title from x with the given baseline, inside x1.
void draw_title(Graphics& graphics, Bus& b, u32 rp, u32 title, i32 x, i32 baseline, i32 x1) {
    if (title == 0 || x1 < x) {
        return;
    }
    u32 chars = static_cast<u32>((x1 - x + 1) / 8);
    if (chars > kMaxTitleChars) {
        chars = kMaxTitleChars;
    }
    graphics.move(rp, x, baseline);
    u8 buffer[kTitleChunk];
    for (u32 at = 0; at < chars; at += kTitleChunk) {
        u32 n = chars - at;
        if (n > kTitleChunk) {
            n = kTitleChunk;
        }
        for (u32 i = 0; i < n; ++i) {
            buffer[i] = static_cast<u8>(b.r8(title + at + i));
        }
        if (!b.ok()) {
            return;
        }
        u32 length = 0;
        while (length < n && buffer[length] != 0) {
            ++length;
        }
        if (length != 0) {
            graphics.text(rp, buffer, length);
        }
        if (length < n) {
            return;
        }
    }
}

// The fields of a NewWindow as the WA_ tags can override them.
struct WindowArgs {
    i32 left = 0;
    i32 top = 0;
    u32 width = 0;
    u32 height = 0;
    u32 detail = 0;
    u32 block = 1;
    u32 idcmp = 0;
    u32 flags = 0;
    u32 title = 0;
    u32 screen = 0;
    u32 type = 0;
    u32 min_width = 0;
    u32 min_height = 0;
    u32 max_width = 0;
    u32 max_height = 0;
    bool custom_screen = false;
};

void read_new_window(Bus& b, u32 nw, WindowArgs& a) {
    if (nw == 0) {
        return;
    }
    a.left = static_cast<i16>(b.r16(nw + ndk::nw_LeftEdge));
    a.top = static_cast<i16>(b.r16(nw + ndk::nw_TopEdge));
    a.width = b.r16(nw + ndk::nw_Width);
    a.height = b.r16(nw + ndk::nw_Height);
    a.detail = b.r8(nw + ndk::nw_DetailPen);
    a.block = b.r8(nw + ndk::nw_BlockPen);
    a.idcmp = b.r32(nw + ndk::nw_IDCMPFlags);
    a.flags = b.r32(nw + ndk::nw_Flags);
    a.title = b.r32(nw + ndk::nw_Title);
    a.screen = b.r32(nw + ndk::nw_Screen);
    a.min_width = b.r16(nw + ndk::nw_MinWidth);
    a.min_height = b.r16(nw + ndk::nw_MinHeight);
    a.max_width = b.r16(nw + ndk::nw_MaxWidth);
    a.max_height = b.r16(nw + ndk::nw_MaxHeight);
    a.type = b.r16(nw + ndk::nw_Type);
}

void apply_window_tag(WindowArgs& a, u32 tag, u32 data) {
    switch (tag) {
    case ndk::WA_Left:
        a.left = static_cast<i16>(data);
        break;
    case ndk::WA_Top:
        a.top = static_cast<i16>(data);
        break;
    case ndk::WA_Width:
        a.width = data;
        break;
    case ndk::WA_Height:
        a.height = data;
        break;
    case ndk::WA_DetailPen:
        a.detail = data & 0xFF;
        break;
    case ndk::WA_BlockPen:
        a.block = data & 0xFF;
        break;
    case ndk::WA_IDCMP:
        a.idcmp = data;
        break;
    case ndk::WA_Flags:
        a.flags = data;
        break;
    case ndk::WA_Title:
        a.title = data;
        break;
    case ndk::WA_CustomScreen:
        a.screen = data;
        a.custom_screen = true;
        break;
    default:
        break;
    }
}

void read_window_tags(Bus& b, u32 tags, WindowArgs& a) {
    u32 item = tags;
    u32 budget = Intuition::kMaxTagItems;
    while (item != 0 && budget != 0) {
        const u32 tag = b.r32(item + ndk::ti_Tag);
        const u32 data = b.r32(item + ndk::ti_Data);
        if (!b.ok()) {
            return;
        }
        --budget;
        switch (tag) {
        case ndk::TAG_DONE:
            return;
        case ndk::TAG_IGNORE:
            item += ndk::ti_SIZEOF;
            break;
        case ndk::TAG_MORE:
            item = data;
            break;
        case ndk::TAG_SKIP:
            item += ndk::ti_SIZEOF * (data + 1);
            break;
        default:
            apply_window_tag(a, tag, data);
            item += ndk::ti_SIZEOF;
            break;
        }
    }
}

void init_planar_bitmap(Bus& b, u32 bitmap, u32 bpr, u32 rows, u32 depth, u32 planes) {
    b.w16(bitmap + ndk::bm_BytesPerRow, bpr);
    b.w16(bitmap + ndk::bm_Rows, rows);
    b.w8(bitmap + ndk::bm_Flags, 0);
    b.w8(bitmap + ndk::bm_Depth, depth);
    const u32 plane_bytes = bpr * rows;
    for (u32 plane = 0; plane < 8; ++plane) {
        const u32 addr = plane < depth ? planes + plane * plane_bytes : 0;
        b.w32(bitmap + ndk::bm_Planes + plane * 4, addr);
    }
}

Rect unite_rect(Rect a, const Rect& b) {
    if (b.x1 < b.x0 || b.y1 < b.y0) {
        return a;
    }
    if (a.x1 < a.x0 || a.y1 < a.y0) {
        return b;
    }
    if (b.x0 < a.x0) {
        a.x0 = b.x0;
    }
    if (b.y0 < a.y0) {
        a.y0 = b.y0;
    }
    if (b.x1 > a.x1) {
        a.x1 = b.x1;
    }
    if (b.y1 > a.y1) {
        a.y1 = b.y1;
    }
    return a;
}

void draw_screen_bar(Graphics& graphics, Bus& b, u32 rp, u32 width, u32 title) {
    if (rp == 0 || width == 0) {
        return;
    }
    const i32 right = static_cast<i32>(width) - 1;
    graphics.set_pens(rp, 2, 1, ndk::JAM2);
    graphics.rect_fill(rp, 0, 0, right, static_cast<i32>(kBarHeight) - 1);
    graphics.set_pens(rp, 1, 1, ndk::JAM2);
    graphics.rect_fill(rp, 0, static_cast<i32>(kBarHeight), right,
                       static_cast<i32>(kBarHeight));
    if (title != 0) {
        graphics.set_pens(rp, 1, 2, ndk::JAM1);
        draw_title(graphics, b, rp, title, 1, 7, right);
    }
}

// The Window fields a draw or a free needs.
struct WindowView {
    u32 window = 0;
    u32 screen = 0;
    u32 rp = 0;
    i32 left = 0;
    i32 top = 0;
    i32 width = 0;
    i32 height = 0;
    u32 flags = 0;
    u32 title = 0;
    u32 border_top = 0;
    bool ok = false;
};

WindowView load_window(Bus& b, u32 window) {
    WindowView w;
    if (window == 0) {
        return w;
    }
    w.window = window;
    w.left = static_cast<i16>(b.r16(window + ndk::wd_LeftEdge));
    w.top = static_cast<i16>(b.r16(window + ndk::wd_TopEdge));
    w.width = b.r16(window + ndk::wd_Width);
    w.height = b.r16(window + ndk::wd_Height);
    w.flags = b.r32(window + ndk::wd_Flags);
    w.title = b.r32(window + ndk::wd_Title);
    w.screen = b.r32(window + ndk::wd_WScreen);
    w.rp = b.r32(window + ndk::wd_RPort);
    w.border_top = b.r8(window + ndk::wd_BorderTop);
    w.ok = b.ok();
    return w;
}

constexpr bool in_rect(const Rect& r, i32 x, i32 y) {
    return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1;
}

void draw_gadget(Graphics& graphics, u32 rp, const Rect& box) {
    graphics.set_pens(rp, 2, 1, ndk::JAM2);
    graphics.rect_fill(rp, box.x0, box.y0, box.x1, box.y1);
    graphics.set_pens(rp, 1, 0, ndk::JAM2);
    graphics.rect_fill(rp, box.x0, box.y0, box.x1, box.y0);
    graphics.rect_fill(rp, box.x0, box.y1, box.x1, box.y1);
    graphics.rect_fill(rp, box.x0, box.y0, box.x0, box.y1);
    graphics.rect_fill(rp, box.x1, box.y0, box.x1, box.y1);
}

// The 10-row title bar of a window with its gadgets, line and title, drawn in
// window-relative coordinates through the window's RastPort.
void draw_window_bar(Graphics& graphics, Bus& b, const WindowView& w) {
    const u32 rp = w.rp;
    if (rp == 0 || w.width < 2 || w.height <= static_cast<i32>(kBarHeight)) {
        return;
    }
    const u8 fg = static_cast<u8>(b.r8(rp + ndk::rp_FgPen));
    const u8 bg = static_cast<u8>(b.r8(rp + ndk::rp_BgPen));
    const u8 mode = static_cast<u8>(b.r8(rp + ndk::rp_DrawMode));
    const u16 cx = static_cast<u16>(b.r16(rp + ndk::rp_cp_x));
    const u16 cy = static_cast<u16>(b.r16(rp + ndk::rp_cp_y));
    const i32 left = 1;
    const i32 right = w.width - 2;
    const i32 top = 1;
    const i32 line = static_cast<i32>(kBarHeight);
    graphics.set_pens(rp, 2, 1, ndk::JAM2);
    graphics.rect_fill(rp, left, top, right, line);
    if ((w.flags & ndk::WFLG_CLOSEGADGET) != 0) {
        draw_gadget(graphics, rp, Intuition::close_box_rect(0, 0));
    }
    if ((w.flags & ndk::WFLG_DEPTHGADGET) != 0) {
        draw_gadget(graphics, rp, Intuition::depth_box_rect(0, 0, w.width));
    }
    graphics.set_pens(rp, 1, 0, ndk::JAM2);
    graphics.rect_fill(rp, left, line, right, line);
    const i32 text_x = (w.flags & ndk::WFLG_CLOSEGADGET) != 0 ? 11 : 2;
    const i32 text_x1 = (w.flags & ndk::WFLG_DEPTHGADGET) != 0 ? right - 8 : right;
    graphics.set_pens(rp, 1, 2, ndk::JAM1);
    draw_title(graphics, b, rp, w.title, text_x, 7, text_x1);
    b.w8(rp + ndk::rp_FgPen, fg);
    b.w8(rp + ndk::rp_BgPen, bg);
    b.w8(rp + ndk::rp_DrawMode, mode);
    b.w16(rp + ndk::rp_cp_x, cx);
    b.w16(rp + ndk::rp_cp_y, cy);
}

void draw_window_chrome(Graphics& graphics, Bus& b, const WindowView& w) {
    const u32 rp = w.rp;
    if (rp == 0 || w.width <= 0 || w.height <= 0) {
        return;
    }
    const u8 fg = static_cast<u8>(b.r8(rp + ndk::rp_FgPen));
    const u8 bg = static_cast<u8>(b.r8(rp + ndk::rp_BgPen));
    const u8 mode = static_cast<u8>(b.r8(rp + ndk::rp_DrawMode));
    const u16 cx = static_cast<u16>(b.r16(rp + ndk::rp_cp_x));
    const u16 cy = static_cast<u16>(b.r16(rp + ndk::rp_cp_y));
    const i32 right = w.width - 1;
    const i32 bottom = w.height - 1;
    graphics.set_pens(rp, 0, 0, ndk::JAM2);
    graphics.rect_fill(rp, 0, 0, right, bottom);
    graphics.set_pens(rp, 1, 0, ndk::JAM2);
    graphics.rect_fill(rp, 0, 0, right, 0);
    graphics.rect_fill(rp, 0, bottom, right, bottom);
    graphics.rect_fill(rp, 0, 0, 0, bottom);
    graphics.rect_fill(rp, right, 0, right, bottom);
    if ((w.flags & ndk::WFLG_BORDERLESS) == 0) {
        draw_window_bar(graphics, b, w);
    }
    graphics.set_pens(rp, 0, 0, ndk::JAM2);
    graphics.rect_fill(rp, 1, static_cast<i32>(w.border_top), right - 1, bottom - 1);
    b.w8(rp + ndk::rp_FgPen, fg);
    b.w8(rp + ndk::rp_BgPen, bg);
    b.w8(rp + ndk::rp_DrawMode, mode);
    b.w16(rp + ndk::rp_cp_x, cx);
    b.w16(rp + ndk::rp_cp_y, cy);
}

}

struct IntuitionNatives {
    static Intuition& of(void* ctx) { return *static_cast<Intuition*>(ctx); }

    static void close_screen(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).close_screen(m.cpu.a[0]) ? 1 : 0);
    }

    static void open_screen(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).open_screen(m.cpu.a[0], 0));
    }

    static void screen_to_back(void* ctx, Machine& m) { of(ctx).screen_to_back(m.cpu.a[0]); }

    static void screen_to_front(void* ctx, Machine& m) { of(ctx).screen_to_front(m.cpu.a[0]); }

    static void lock_i_base(void*, Machine& m) { native::set_result(m, 0); }

    static void unlock_i_base(void*, Machine&) {}

    static void get_screen_data(void* ctx, Machine& m) {
        Intuition& g = of(ctx);
        Bus b(m);
        const u32 buffer = m.cpu.a[0];
        const u32 size = m.cpu.d[0];
        const u32 type = m.cpu.d[1];
        u32 screen = m.cpu.a[1];
        if (screen == 0) {
            if (type != ndk::CUSTOMSCREEN && type != kWBENCHSCREEN) {
                native::set_result(m, 0);
                return;
            }
            screen = g.front_;
        }
        if (screen == 0 || g.find_screen(screen) >= Intuition::kMaxScreens) {
            native::set_result(m, 0);
            return;
        }
        const u32 bytes = size < ndk::Screen_SIZEOF ? size : ndk::Screen_SIZEOF;
        for (u32 i = 0; i < bytes; ++i) {
            b.w8(buffer + i, b.r8(screen + i));
        }
        if (b.ok()) {
            native::set_result(m, 1);
        }
    }

    static void open_screen_tag_list(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).open_screen(m.cpu.a[0], m.cpu.a[1]));
    }

    static void close_window(void* ctx, Machine& m) { of(ctx).close_window(m.cpu.a[0]); }

    static void modify_idcmp(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).modify_idcmp(m.cpu.a[0], m.cpu.d[0]) ? 1 : 0);
    }

    static void open_window(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).open_window(m.cpu.a[0], 0));
    }

    static void open_window_tag_list(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).open_window(m.cpu.a[0], m.cpu.a[1]));
    }

    static void set_window_titles(void* ctx, Machine& m) {
        of(ctx).set_window_titles(m.cpu.a[0], m.cpu.a[1], m.cpu.a[2]);
    }

    static void window_to_back(void* ctx, Machine& m) { of(ctx).window_to_back(m.cpu.a[0]); }

    static void window_to_front(void* ctx, Machine& m) { of(ctx).window_to_front(m.cpu.a[0]); }
};

namespace {

struct IntuitionFunction {
    u32 lvo;
    m68k::NativeFn fn;
    const char* name;
};

constexpr IntuitionFunction kIntuitionFunctions[] = {
    {intuition_lvo::CloseScreen, IntuitionNatives::close_screen, "CloseScreen"},
    {intuition_lvo::CloseWindow, IntuitionNatives::close_window, "CloseWindow"},
    {intuition_lvo::ModifyIDCMP, IntuitionNatives::modify_idcmp, "ModifyIDCMP"},
    {intuition_lvo::OpenScreen, IntuitionNatives::open_screen, "OpenScreen"},
    {intuition_lvo::OpenWindow, IntuitionNatives::open_window, "OpenWindow"},
    {intuition_lvo::ScreenToBack, IntuitionNatives::screen_to_back, "ScreenToBack"},
    {intuition_lvo::ScreenToFront, IntuitionNatives::screen_to_front, "ScreenToFront"},
    {intuition_lvo::SetWindowTitles, IntuitionNatives::set_window_titles, "SetWindowTitles"},
    {intuition_lvo::WindowToBack, IntuitionNatives::window_to_back, "WindowToBack"},
    {intuition_lvo::WindowToFront, IntuitionNatives::window_to_front, "WindowToFront"},
    {intuition_lvo::LockIBase, IntuitionNatives::lock_i_base, "LockIBase"},
    {intuition_lvo::UnlockIBase, IntuitionNatives::unlock_i_base, "UnlockIBase"},
    {intuition_lvo::GetScreenData, IntuitionNatives::get_screen_data, "GetScreenData"},
    {intuition_lvo::OpenWindowTagList, IntuitionNatives::open_window_tag_list,
     "OpenWindowTagList"},
    {intuition_lvo::OpenScreenTagList, IntuitionNatives::open_screen_tag_list,
     "OpenScreenTagList"},
};

}

Rect Intuition::close_box_rect(i32 left, i32 top) {
    return Rect{left + 1, top + 1, left + 8, top + 8};
}

Rect Intuition::depth_box_rect(i32 left, i32 top, i32 width) {
    const i32 right = left + width - 2;
    return Rect{right - 7, top + 1, right, top + 8};
}

bool Intuition::init(Machine& m, Exec& exec, Graphics& graphics, DisplayState& display) {
    machine_ = &m;
    exec_ = &exec;
    graphics_ = &graphics;
    display_ = &display;
    clock_ = nullptr;
    base_ = 0;
    front_ = 0;
    reply_port_ = 0;
    active_window_ = 0;
    pressed_gadget_window_ = 0;
    pressed_gadget_ = GadgetKind::None;
    mouse_x_ = 0;
    mouse_y_ = 0;
    dropped_messages_ = 0;
    batch_depth_ = 0;
    display.front_screen = 0;
    for (u32& screen : screens_) {
        screen = 0;
    }
    for (u32& window : windows_) {
        window = 0;
    }
    for (u32& msg : messages_) {
        msg = 0;
    }
    for (ScreenRecord& rec : screen_records_) {
        rec = ScreenRecord{};
    }
    for (WindowRecord& rec : window_records_) {
        rec = WindowRecord{};
    }
    for (NativeVector& v : vectors_) {
        v = NativeVector{nullptr, nullptr};
    }
    for (const IntuitionFunction& f : kIntuitionFunctions) {
        vectors_[f.lvo / ndk::LIB_VECTSIZE - 1] = NativeVector{f.fn, f.name};
    }
    const u32 reply_port = exec.create_port();
    if (reply_port == 0) {
        return false;
    }
    const u32 base = exec.make_native_library(kIntuitionName, kHombreLibVersion, vectors_, kVectors,
                                              this);
    if (base == 0) {
        exec.delete_port(reply_port);
        return false;
    }
    reply_port_ = reply_port;
    exec.set_reply_handler(reply_port_, reply_message, this);
    display.damage = DamageSink{on_damage, this};
    base_ = base;
    return true;
}

void Intuition::begin_batch() { ++batch_depth_; }

void Intuition::end_batch() {
    if (batch_depth_ == 0) {
        return;
    }
    --batch_depth_;
    if (batch_depth_ != 0) {
        return;
    }
    for (ScreenRecord& srec : screen_records_) {
        if (srec.addr != 0 && srec.pending.x0 <= srec.pending.x1 &&
            srec.pending.y0 <= srec.pending.y1) {
            const Rect r = srec.pending;
            srec.pending = Rect{0, 0, -1, -1};
            compose(srec.addr, r);
        }
    }
}

void Intuition::compose(u32 screen, Rect r) {
    if (screen == 0 || machine_ == nullptr) {
        return;
    }
    ScreenRecord* srec = find_screen_record(screen);
    if (srec == nullptr || srec->width == 0 || srec->height == 0) {
        return;
    }
    const i32 sw = static_cast<i32>(srec->width);
    const i32 sh = static_cast<i32>(srec->height);
    if (r.x0 < 0) {
        r.x0 = 0;
    }
    if (r.y0 < 0) {
        r.y0 = 0;
    }
    if (r.x1 >= sw) {
        r.x1 = sw - 1;
    }
    if (r.y1 >= sh) {
        r.y1 = sh - 1;
    }
    if (r.x1 < r.x0 || r.y1 < r.y0) {
        return;
    }
    if (batch_depth_ > 0) {
        srec->pending = unite_rect(srec->pending, r);
        return;
    }
    Bus b(*machine_);
    raster::Bitmap dst{};
    raster::Bitmap backdrop{};
    if (!raster::load_bitmap(b, screen + ndk::sc_BitMap, dst) ||
        !raster::load_bitmap(b, srec->backdrop_bitmap, backdrop)) {
        return;
    }
    raster::copy_rect(b, backdrop, r.x0, r.y0, dst, r.x0, r.y0, r.x1 - r.x0 + 1, r.y1 - r.y0 + 1);

    const WindowRecord* order[kMaxWindows]{};
    u32 count = 0;
    u32 win = srec->first_window;
    while (win != 0 && count < kMaxWindows) {
        const WindowRecord* wrec = find_window_record(win);
        if (wrec == nullptr) {
            break;
        }
        order[count++] = wrec;
        win = wrec->next_window;
    }
    for (u32 i = count; i > 0; --i) {
        const WindowRecord* wrec = order[i - 1];
        const i32 wx0 = r.x0 > wrec->left ? r.x0 : wrec->left;
        const i32 wy0 = r.y0 > wrec->top ? r.y0 : wrec->top;
        const i32 wx1 =
            r.x1 < wrec->left + wrec->width - 1 ? r.x1 : wrec->left + wrec->width - 1;
        const i32 wy1 =
            r.y1 < wrec->top + wrec->height - 1 ? r.y1 : wrec->top + wrec->height - 1;
        if (wx0 <= wx1 && wy0 <= wy1) {
            raster::Bitmap wbm{};
            if (raster::load_bitmap(b, wrec->bitmap, wbm)) {
                raster::copy_rect(b, wbm, wx0 - wrec->left, wy0 - wrec->top, dst, wx0, wy0,
                                  wx1 - wx0 + 1, wy1 - wy0 + 1);
            }
        }
    }
    if (b.ok() && display_ != nullptr && display_->front_screen == screen &&
        display_->sink.refresh != nullptr) {
        display_->sink.refresh(display_->sink.ctx, *machine_, screen, r);
    }
}

void Intuition::on_damage(void* ctx, Machine&, u32 bitmap, const Rect& rect) {
    if (ctx == nullptr || bitmap == 0 || rect.x1 < rect.x0 || rect.y1 < rect.y0) {
        return;
    }
    Intuition& self = *static_cast<Intuition*>(ctx);
    for (const WindowRecord& wrec : self.window_records_) {
        if (wrec.addr != 0 && wrec.bitmap == bitmap) {
            const Rect screen_rect{rect.x0 + wrec.left, rect.y0 + wrec.top,
                                   rect.x1 + wrec.left, rect.y1 + wrec.top};
            self.compose(wrec.screen, screen_rect);
            return;
        }
    }
    for (const ScreenRecord& srec : self.screen_records_) {
        if (srec.addr != 0 && srec.backdrop_bitmap == bitmap) {
            self.compose(srec.addr, rect);
            return;
        }
    }
}

u32 Intuition::find_screen(u32 screen) const {
    if (screen == 0) {
        return kMaxScreens;
    }
    for (u32 i = 0; i < kMaxScreens; ++i) {
        if (screens_[i] == screen) {
            return i;
        }
    }
    return kMaxScreens;
}

void Intuition::remove_at(u32 index) {
    for (u32 i = index; i + 1 < kMaxScreens; ++i) {
        screens_[i] = screens_[i + 1];
    }
    screens_[kMaxScreens - 1] = 0;
}

Intuition::ScreenRecord* Intuition::find_screen_record(u32 screen) {
    for (ScreenRecord& rec : screen_records_) {
        if (rec.addr == screen) {
            return &rec;
        }
    }
    return nullptr;
}

Intuition::ScreenRecord* Intuition::free_screen_record() {
    for (ScreenRecord& rec : screen_records_) {
        if (rec.addr == 0) {
            return &rec;
        }
    }
    return nullptr;
}

void Intuition::write_links() {
    Bus b(*machine_);
    for (u32 i = 0; i < kMaxScreens; ++i) {
        if (screens_[i] != 0) {
            const u32 next = i + 1 < kMaxScreens ? screens_[i + 1] : 0;
            b.w32(screens_[i] + kScNextScreen, next);
        }
    }
}

// Writes the guest window list from the records, head first.
void Intuition::write_window_links(u32 screen) {
    ScreenRecord* srec = find_screen_record(screen);
    if (srec == nullptr) {
        return;
    }
    Bus b(*machine_);
    b.w32(screen + ndk::sc_FirstWindow, srec->first_window);
    u32 window = srec->first_window;
    for (u32 step = 0; step < kMaxWindows && window != 0; ++step) {
        WindowRecord* rec = find_window_record(window);
        if (rec == nullptr) {
            return;
        }
        b.w32(window + ndk::wd_NextWindow, rec->next_window);
        window = rec->next_window;
    }
}

void Intuition::refresh_screen(u32 screen) {
    if (screen == 0 || display_ == nullptr) {
        return;
    }
    const ScreenRecord* rec = find_screen_record(screen);
    if (rec == nullptr) {
        return;
    }
    refresh_full(*display_, *machine_, screen, rec->width, rec->height);
}

u32 Intuition::open_screen(u32 new_screen, u32 tags) {
    Bus b(*machine_);
    if (new_screen == 0 && tags == 0) {
        // No screen description: the run ends as a bus error. The address
        // is even and one past guest memory.
        b.r32((machine_->mem.size + 1) & ~1u);
        return 0;
    }
    OpenArgs a;
    read_new_screen(b, new_screen, a);
    read_tags(b, tags, a);
    if (!b.ok()) {
        return 0;
    }
    if (a.width == 0) {
        a.width = 640;
    }
    if (a.height == 0) {
        a.height = 400;
    }
    if (a.depth != 2) {
        a.depth = 2;
    }
    if (a.width > 0xFFFF) {
        a.width = 0xFFFF;
    }
    if (a.height > 0xFFFF) {
        a.height = 0xFFFF;
    }

    u32 open = 0;
    for (u32 screen : screens_) {
        if (screen != 0) {
            ++open;
        }
    }
    if (open == kMaxScreens) {
        LogLine().text("intuition: screen limit reached").send();
        return 0;
    }
    ScreenRecord* rec = free_screen_record();
    if (rec == nullptr) {
        LogLine().text("intuition: screen limit reached").send();
        return 0;
    }

    const u64 bytes_per_row = (static_cast<u64>(a.width) + 15) / 16 * 2;
    const u64 planes_bytes = 2 * bytes_per_row * a.height;
    if (planes_bytes > 0xFFFFFFFF) {
        LogLine().text("intuition: screen too large").send();
        return 0;
    }
    const u32 bpr = static_cast<u32>(bytes_per_row);
    const u32 pbytes = static_cast<u32>(planes_bytes);

    u32 screen = 0;
    u32 planes = 0;
    u32 backdrop_bitmap = 0;
    u32 backdrop_planes = 0;
    u32 backdrop_rp = 0;
    u32 map = 0;
    u32 colors = 0;
    u32 title = 0;
    u32 title_bytes = 0;
    auto fail = [&]() -> u32 {
        if (title != 0) {
            exec_->free(title, title_bytes);
        }
        if (colors != 0) {
            exec_->free(colors, 8);
        }
        if (map != 0) {
            exec_->free(map, ndk::ColorMap_SIZEOF);
        }
        if (backdrop_rp != 0) {
            exec_->free(backdrop_rp, ndk::RastPort_SIZEOF);
        }
        if (backdrop_planes != 0) {
            exec_->free(backdrop_planes, pbytes);
        }
        if (backdrop_bitmap != 0) {
            exec_->free(backdrop_bitmap, ndk::BitMap_SIZEOF);
        }
        if (planes != 0) {
            exec_->free(planes, pbytes);
        }
        if (screen != 0) {
            exec_->free(screen, ndk::Screen_SIZEOF);
        }
        LogLine().text("intuition: no memory for a screen").send();
        return 0;
    };

    screen = exec_->alloc(ndk::Screen_SIZEOF, ndk::MEMF_CHIP | ndk::MEMF_CLEAR);
    if (screen == 0) {
        return fail();
    }
    planes = exec_->alloc(pbytes, ndk::MEMF_CHIP | ndk::MEMF_CLEAR);
    if (planes == 0) {
        return fail();
    }
    backdrop_bitmap = exec_->alloc(ndk::BitMap_SIZEOF, ndk::MEMF_CHIP | ndk::MEMF_CLEAR);
    if (backdrop_bitmap == 0) {
        return fail();
    }
    backdrop_planes = exec_->alloc(pbytes, ndk::MEMF_CHIP | ndk::MEMF_CLEAR);
    if (backdrop_planes == 0) {
        return fail();
    }
    backdrop_rp = exec_->alloc(ndk::RastPort_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (backdrop_rp == 0) {
        return fail();
    }
    map = exec_->alloc(ndk::ColorMap_SIZEOF, ndk::MEMF_CHIP | ndk::MEMF_CLEAR);
    if (map == 0) {
        return fail();
    }
    colors = exec_->alloc(8, ndk::MEMF_CHIP | ndk::MEMF_CLEAR);
    if (colors == 0) {
        return fail();
    }
    if (a.title != 0) {
        title = copy_title(b, *exec_, a.title, title_bytes);
        if (title == 0) {
            return fail();
        }
    }

    const u32 bitmap = screen + ndk::sc_BitMap;
    const u32 rp = screen + ndk::sc_RastPort;
    const u32 vp = screen + ndk::sc_ViewPort;
    b.w16(screen + ndk::sc_LeftEdge, a.left);
    b.w16(screen + ndk::sc_TopEdge, a.top);
    b.w16(screen + ndk::sc_Width, a.width);
    b.w16(screen + ndk::sc_Height, a.height);
    b.w8(screen + ndk::sc_BarHeight, kBarHeight);
    b.w8(screen + ndk::sc_WBorTop, 11);
    b.w8(screen + ndk::sc_WBorLeft, 1);
    b.w8(screen + ndk::sc_WBorRight, 1);
    b.w8(screen + ndk::sc_WBorBottom, 1);
    b.w8(screen + ndk::sc_DetailPen, a.detail);
    b.w8(screen + ndk::sc_BlockPen, a.block);
    b.w32(screen + ndk::sc_Title, title);
    b.w32(screen + ndk::sc_DefaultTitle, title);
    b.w32(screen + ndk::sc_Font, graphics_->topaz_attr());
    b.w32(screen + kScNextScreen, 0);

    init_planar_bitmap(b, bitmap, bpr, a.height, a.depth, planes);
    init_planar_bitmap(b, backdrop_bitmap, bpr, a.height, a.depth, backdrop_planes);

    graphics_->init_rastport(rp);
    b.w32(rp + ndk::rp_BitMap, bitmap);
    graphics_->init_rastport(backdrop_rp);
    b.w32(backdrop_rp + ndk::rp_BitMap, backdrop_bitmap);

    b.w32(vp + ndk::vp_ColorMap, map);
    b.w16(vp + ndk::vp_DWidth, a.width);
    b.w16(vp + ndk::vp_DHeight, a.height);
    b.w16(map + ndk::cm_Count, 4);
    b.w32(map + ndk::cm_ColorTable, colors);
    for (u32 i = 0; i < 4; ++i) {
        b.w16(colors + i * 2, kPalette[i]);
    }

    for (u32 i = kMaxScreens - 1; i > 0; --i) {
        screens_[i] = screens_[i - 1];
    }
    screens_[0] = screen;
    front_ = screen;
    display_->front_screen = screen;
    write_links();
    rec->addr = screen;
    rec->screen_bytes = ndk::Screen_SIZEOF;
    rec->planes = planes;
    rec->planes_bytes = pbytes;
    rec->backdrop_bitmap = backdrop_bitmap;
    rec->backdrop_bitmap_bytes = ndk::BitMap_SIZEOF;
    rec->backdrop_planes = backdrop_planes;
    rec->backdrop_planes_bytes = pbytes;
    rec->backdrop_rp = backdrop_rp;
    rec->backdrop_rp_bytes = ndk::RastPort_SIZEOF;
    rec->colormap = map;
    rec->colormap_bytes = ndk::ColorMap_SIZEOF;
    rec->colors = colors;
    rec->colors_bytes = 8;
    rec->title = title;
    rec->title_bytes = title_bytes;
    rec->width = a.width;
    rec->height = a.height;
    rec->first_window = 0;
    rec->pending = Rect{0, 0, -1, -1};

    const i32 right = static_cast<i32>(a.width) - 1;
    const i32 bottom = static_cast<i32>(a.height) - 1;
    begin_batch();
    graphics_->set_pens(backdrop_rp, 0, 0, ndk::JAM2);
    graphics_->rect_fill(backdrop_rp, 0, 0, right, bottom);
    draw_screen_bar(*graphics_, b, backdrop_rp, a.width, title);
    compose(screen, Rect{0, 0, right, bottom});
    end_batch();
    return screen;
}

bool Intuition::close_screen(u32 screen) {
    const u32 index = find_screen(screen);
    if (index >= kMaxScreens) {
        return false;
    }
    for (const WindowRecord& window : window_records_) {
        if (window.addr != 0 && window.screen == screen) {
            return false;
        }
    }
    ScreenRecord* rec = find_screen_record(screen);
    if (rec == nullptr) {
        return false;
    }
    const bool was_front = index == 0;
    const u32 width = rec->width;
    const u32 height = rec->height;

    remove_at(index);
    write_links();
    if (rec->backdrop_rp != 0) {
        exec_->free(rec->backdrop_rp, rec->backdrop_rp_bytes);
    }
    if (rec->backdrop_planes != 0) {
        exec_->free(rec->backdrop_planes, rec->backdrop_planes_bytes);
    }
    if (rec->backdrop_bitmap != 0) {
        exec_->free(rec->backdrop_bitmap, rec->backdrop_bitmap_bytes);
    }
    if (rec->planes != 0) {
        exec_->free(rec->planes, rec->planes_bytes);
    }
    if (rec->colors != 0) {
        exec_->free(rec->colors, rec->colors_bytes);
    }
    if (rec->colormap != 0) {
        exec_->free(rec->colormap, rec->colormap_bytes);
    }
    if (rec->title != 0) {
        exec_->free(rec->title, rec->title_bytes);
    }
    exec_->free(screen, rec->screen_bytes);
    *rec = ScreenRecord{};

    if (was_front) {
        front_ = screens_[0];
        display_->front_screen = front_;
        if (front_ != 0) {
            refresh_screen(front_);
        } else {
            refresh_full(*display_, *machine_, 0, width, height);
        }
    }
    return true;
}

void Intuition::screen_to_front(u32 screen) {
    const u32 index = find_screen(screen);
    if (index >= kMaxScreens) {
        return;
    }
    for (u32 i = index; i > 0; --i) {
        screens_[i] = screens_[i - 1];
    }
    screens_[0] = screen;
    front_ = screen;
    display_->front_screen = screen;
    write_links();
    refresh_screen(screen);
}

void Intuition::screen_to_back(u32 screen) {
    const u32 index = find_screen(screen);
    if (index >= kMaxScreens) {
        return;
    }
    u32 open = 0;
    for (u32 known : screens_) {
        if (known != 0) {
            ++open;
        }
    }
    for (u32 i = index; i + 1 < open; ++i) {
        screens_[i] = screens_[i + 1];
    }
    screens_[open - 1] = screen;
    front_ = screens_[0];
    display_->front_screen = front_;
    write_links();
    refresh_screen(front_);
}

u32 Intuition::find_window(u32 window) const {
    if (window == 0) {
        return kMaxWindows;
    }
    for (u32 i = 0; i < kMaxWindows; ++i) {
        if (windows_[i] == window) {
            return i;
        }
    }
    return kMaxWindows;
}

Intuition::WindowRecord* Intuition::find_window_record(u32 window) {
    for (WindowRecord& rec : window_records_) {
        if (rec.addr == window) {
            return &rec;
        }
    }
    return nullptr;
}

const Intuition::WindowRecord* Intuition::find_window_record(u32 window) const {
    for (const WindowRecord& rec : window_records_) {
        if (rec.addr == window) {
            return &rec;
        }
    }
    return nullptr;
}

void Intuition::reply_message(void* ctx, u32 message) {
    static_cast<Intuition*>(ctx)->free_tracked_message(message);
}

bool Intuition::is_tracked_message(u32 message) const {
    if (machine_ == nullptr) {
        return false;
    }
    const u32 addr = native::guest_address(*machine_, message);
    if (addr == 0) {
        return false;
    }
    for (u32 slot : messages_) {
        if (slot == addr) {
            return true;
        }
    }
    return false;
}

void Intuition::free_tracked_message(u32 message) {
    if (exec_ == nullptr || machine_ == nullptr) {
        return;
    }
    const u32 addr = native::guest_address(*machine_, message);
    if (addr == 0) {
        return;
    }
    for (u32& slot : messages_) {
        if (slot == addr) {
            slot = 0;
            exec_->free(addr, ndk::IntuiMessage_SIZEOF);
            return;
        }
    }
}

void Intuition::flush_window_messages(u32 port) {
    if (port == 0 || exec_ == nullptr) {
        return;
    }
    for (u32 i = 0; i < kMaxMessages; ++i) {
        const u32 msg = exec_->get_message(port);
        if (msg == 0) {
            break;
        }
        free_tracked_message(msg);
    }
}

void Intuition::remove_window_messages(u32 port, u32 window) {
    if (port == 0 || window == 0 || exec_ == nullptr || machine_ == nullptr) {
        return;
    }
    Bus b(*machine_);
    const u32 list = port + ndk::mp_MsgList;
    u32 node = b.r32(list + ndk::LH_HEAD);
    for (u32 step = 0; step < kMaxMessages * 32 && node != 0; ++step) {
        const u32 succ = b.r32(node + ndk::LN_SUCC);
        if (!b.ok() || succ == 0) {
            break;
        }
        if (is_tracked_message(node) && b.r32(node + ndk::im_IDCMPWindow) == window) {
            const u32 pred = b.r32(node + ndk::LN_PRED);
            if (!b.ok()) {
                break;
            }
            b.w32(pred + ndk::LN_SUCC, succ);
            b.w32(succ + ndk::LN_PRED, pred);
            if (!b.ok()) {
                break;
            }
            free_tracked_message(node);
        }
        node = succ;
    }
}

void Intuition::post_intuimessage(u32 window, u32 idcmp_class, u16 code, u32 iaddress) {
    if (window == 0 || find_window(window) >= kMaxWindows) {
        return;
    }
    const WindowRecord* rec = find_window_record(window);
    if (rec == nullptr || rec->screen != front_ || rec->user_port == 0) {
        return;
    }
    Bus b(*machine_);
    const u32 idcmp = b.r32(window + ndk::wd_IDCMPFlags);
    if (!b.ok() || (idcmp & idcmp_class) == 0) {
        return;
    }
    u32* slot = nullptr;
    for (u32& entry : messages_) {
        if (entry == 0) {
            slot = &entry;
            break;
        }
    }
    if (slot == nullptr) {
        ++dropped_messages_;
        return;
    }
    const u32 msg = exec_->alloc(ndk::IntuiMessage_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (msg == 0) {
        return;
    }
    const u64 us = clock_ != nullptr ? clock_() : 0;
    const u32 seconds = static_cast<u32>(us / 1000000);
    const u32 micros = static_cast<u32>(us % 1000000);
    const i16 rel_x = static_cast<i16>(mouse_x_ - rec->left);
    const i16 rel_y = static_cast<i16>(mouse_y_ - rec->top);
    b.w32(msg + ndk::mn_ReplyPort, reply_port_);
    b.w16(msg + ndk::mn_Length, ndk::IntuiMessage_SIZEOF);
    b.w32(msg + ndk::im_Class, idcmp_class);
    b.w16(msg + ndk::im_Code, code);
    b.w16(msg + ndk::im_Qualifier, 0);
    b.w32(msg + ndk::im_IAddress, iaddress);
    b.w16(msg + ndk::im_MouseX, static_cast<u16>(rel_x));
    b.w16(msg + ndk::im_MouseY, static_cast<u16>(rel_y));
    b.w32(msg + ndk::im_Seconds, seconds);
    b.w32(msg + ndk::im_Micros, micros);
    b.w32(msg + ndk::im_IDCMPWindow, window);
    b.w32(msg + ndk::im_SpecialLink, 0);
    if (!b.ok() || !exec_->post_message(rec->user_port, msg)) {
        exec_->free(msg, ndk::IntuiMessage_SIZEOF);
        return;
    }
    *slot = msg;
}

void Intuition::activate_window(u32 window) {
    if (window == active_window_) {
        return;
    }
    const u32 prev = active_window_;
    active_window_ = window;
    if (prev != 0) {
        post_intuimessage(prev, ndk::IDCMP_INACTIVEWINDOW, 0);
    }
    if (window != 0) {
        post_intuimessage(window, ndk::IDCMP_ACTIVEWINDOW, 0);
    }
}

u32 Intuition::window_at(i32 x, i32 y) const {
    if (front_ == 0) {
        return 0;
    }
    for (const ScreenRecord& srec : screen_records_) {
        if (srec.addr != front_) {
            continue;
        }
        u32 window = srec.first_window;
        for (u32 step = 0; step < kMaxWindows && window != 0; ++step) {
            const WindowRecord* rec = find_window_record(window);
            if (rec == nullptr) {
                return 0;
            }
            if (x >= rec->left && x < rec->left + rec->width && y >= rec->top &&
                y < rec->top + rec->height) {
                return window;
            }
            window = rec->next_window;
        }
        return 0;
    }
    return 0;
}

Intuition::WindowRecord* Intuition::free_window_record() {
    for (WindowRecord& rec : window_records_) {
        if (rec.addr == 0) {
            return &rec;
        }
    }
    return nullptr;
}

void Intuition::remove_window_at(u32 index) {
    for (u32 i = index; i + 1 < kMaxWindows; ++i) {
        windows_[i] = windows_[i + 1];
    }
    windows_[kMaxWindows - 1] = 0;
}

u32 Intuition::open_window(u32 new_window, u32 tags) {
    Bus b(*machine_);
    if (new_window == 0 && tags == 0) {
        b.r32((machine_->mem.size + 1) & ~1u);
        return 0;
    }
    WindowArgs a;
    read_new_window(b, new_window, a);
    read_window_tags(b, tags, a);
    if (!b.ok()) {
        return 0;
    }

    u32 screen = 0;
    if (a.custom_screen && a.screen != 0) {
        screen = a.screen;
    } else if (a.type == ndk::CUSTOMSCREEN && a.screen != 0) {
        screen = a.screen;
    } else {
        screen = front_;
    }
    if (screen == 0 || find_screen(screen) >= kMaxScreens) {
        LogLine().text("intuition: window without a screen").send();
        return 0;
    }
    const i32 screen_w = static_cast<i32>(b.r16(screen + ndk::sc_Width));
    const i32 screen_h = static_cast<i32>(b.r16(screen + ndk::sc_Height));
    const u32 depth = b.r8(screen + ndk::sc_BitMap + ndk::bm_Depth);
    if (!b.ok() || screen_w <= 0 || screen_h <= 0 || depth == 0 || depth > 8) {
        return 0;
    }

    u32 open = 0;
    for (u32 window : windows_) {
        if (window != 0) {
            ++open;
        }
    }
    if (open == kMaxWindows) {
        LogLine().text("intuition: window limit reached").send();
        return 0;
    }
    WindowRecord* rec = free_window_record();
    ScreenRecord* srec = find_screen_record(screen);
    if (rec == nullptr || srec == nullptr) {
        LogLine().text("intuition: window limit reached").send();
        return 0;
    }

    i32 left = a.left < 0 ? 0 : a.left;
    i32 top = a.top < 0 ? 0 : a.top;
    if (left >= screen_w) {
        left = screen_w - 1;
    }
    if (top >= screen_h) {
        top = screen_h - 1;
    }
    const u32 room_x = static_cast<u32>(screen_w - left);
    const u32 room_y = static_cast<u32>(screen_h - top);
    const i32 width =
        a.width == 0 || a.width > room_x ? static_cast<i32>(room_x) : static_cast<i32>(a.width);
    const i32 height =
        a.height == 0 || a.height > room_y ? static_cast<i32>(room_y) : static_cast<i32>(a.height);
    const i32 max_w = width;
    const i32 max_h = height;
    const bool borderless = (a.flags & ndk::WFLG_BORDERLESS) != 0;
    const u32 border_top = borderless ? 1 : 11;

    const u64 bytes_per_row = (static_cast<u64>(max_w) + 15) / 16 * 2;
    const u64 total_planes_bytes =
        static_cast<u64>(depth) * bytes_per_row * static_cast<u64>(max_h);
    if (total_planes_bytes == 0 || total_planes_bytes > 0xFFFFFFFF) {
        LogLine().text("intuition: no memory for a window").send();
        return 0;
    }
    const u32 bpr = static_cast<u32>(bytes_per_row);
    const u32 planes_bytes = static_cast<u32>(total_planes_bytes);

    u32 window = 0;
    u32 rp = 0;
    u32 layer = 0;
    u32 bitmap = 0;
    u32 planes = 0;
    u32 title = 0;
    u32 title_bytes = 0;
    u32 user_port = 0;
    bool user_port_owned = false;
    auto fail = [&]() -> u32 {
        if (user_port_owned && user_port != 0) {
            exec_->delete_port(user_port);
        }
        if (title != 0) {
            exec_->free(title, title_bytes);
        }
        if (planes != 0) {
            exec_->free(planes, planes_bytes);
        }
        if (bitmap != 0) {
            exec_->free(bitmap, ndk::BitMap_SIZEOF);
        }
        if (layer != 0) {
            exec_->free(layer, ndk::Layer_SIZEOF);
        }
        if (rp != 0) {
            exec_->free(rp, ndk::RastPort_SIZEOF);
        }
        if (window != 0) {
            exec_->free(window, ndk::Window_SIZEOF);
        }
        LogLine().text("intuition: no memory for a window").send();
        return 0;
    };

    window = exec_->alloc(ndk::Window_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (window == 0) {
        return fail();
    }
    rp = exec_->alloc(ndk::RastPort_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (rp == 0) {
        return fail();
    }
    layer = exec_->alloc(ndk::Layer_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (layer == 0) {
        return fail();
    }
    bitmap = exec_->alloc(ndk::BitMap_SIZEOF, ndk::MEMF_CHIP | ndk::MEMF_CLEAR);
    if (bitmap == 0) {
        return fail();
    }
    planes = exec_->alloc(planes_bytes, ndk::MEMF_CHIP | ndk::MEMF_CLEAR);
    if (planes == 0) {
        return fail();
    }
    if (a.title != 0) {
        title = copy_title(b, *exec_, a.title, title_bytes);
        if (title == 0) {
            return fail();
        }
    }
    if (a.idcmp != 0) {
        user_port = b.r32(window + ndk::wd_UserPort);
        if (user_port == 0) {
            user_port = exec_->create_port();
            if (user_port == 0) {
                return fail();
            }
            user_port_owned = true;
        }
    }

    init_planar_bitmap(b, bitmap, bpr, static_cast<u32>(max_h), depth, planes);

    b.w16(window + ndk::wd_LeftEdge, static_cast<u32>(left));
    b.w16(window + ndk::wd_TopEdge, static_cast<u32>(top));
    b.w16(window + ndk::wd_Width, static_cast<u32>(width));
    b.w16(window + ndk::wd_Height, static_cast<u32>(height));
    b.w16(window + ndk::wd_MinWidth, a.min_width);
    b.w16(window + ndk::wd_MinHeight, a.min_height);
    b.w16(window + ndk::wd_MaxWidth, a.max_width);
    b.w16(window + ndk::wd_MaxHeight, a.max_height);
    b.w32(window + ndk::wd_Flags, a.flags);
    b.w32(window + ndk::wd_Title, title);
    b.w32(window + ndk::wd_WScreen, screen);
    b.w32(window + ndk::wd_RPort, rp);
    b.w8(window + ndk::wd_BorderLeft, 1);
    b.w8(window + ndk::wd_BorderTop, border_top);
    b.w8(window + ndk::wd_BorderRight, 1);
    b.w8(window + ndk::wd_BorderBottom, 1);
    b.w32(window + ndk::wd_IDCMPFlags, a.idcmp);
    b.w32(window + ndk::wd_UserPort, user_port);
    b.w8(window + ndk::wd_DetailPen, a.detail);
    b.w8(window + ndk::wd_BlockPen, a.block);
    b.w32(window + ndk::wd_WLayer, layer);
    b.w32(window + ndk::wd_IFont, graphics_->topaz_font());
    b.w32(window + ndk::wd_NextWindow, 0);

    graphics_->init_rastport(rp);
    b.w32(rp + ndk::rp_BitMap, bitmap);
    b.w32(rp + ndk::rp_Layer, layer);
    b.w8(rp + ndk::rp_FgPen, 1);
    b.w8(rp + ndk::rp_BgPen, 0);
    b.w8(rp + ndk::rp_DrawMode, ndk::JAM2);
    b.w16(rp + ndk::rp_cp_x, 0);
    b.w16(rp + ndk::rp_cp_y, 6);

    const i32 right = left + width - 1;
    const i32 bottom = top + height - 1;
    b.w32(layer + ndk::ly_rp, rp);
    b.w16(layer + ndk::ly_MinX, static_cast<u32>(left));
    b.w16(layer + ndk::ly_MinY, static_cast<u32>(top));
    b.w16(layer + ndk::ly_MaxX, static_cast<u32>(right));
    b.w16(layer + ndk::ly_MaxY, static_cast<u32>(bottom));
    b.w32(layer + ndk::ly_SuperBitMap, bitmap);
    b.w32(layer + ndk::ly_Window, window);

    rec->addr = window;
    rec->window_bytes = ndk::Window_SIZEOF;
    rec->screen = screen;
    rec->next_window = srec->first_window;
    rec->left = left;
    rec->top = top;
    rec->width = width;
    rec->height = height;
    rec->max_w = max_w;
    rec->max_h = max_h;
    rec->min_w = width;
    rec->min_h = height;
    rec->lim_w = width;
    rec->lim_h = height;
    rec->rp = rp;
    rec->rp_bytes = ndk::RastPort_SIZEOF;
    rec->layer = layer;
    rec->layer_bytes = ndk::Layer_SIZEOF;
    rec->bitmap = bitmap;
    rec->bitmap_bytes = ndk::BitMap_SIZEOF;
    rec->planes = planes;
    rec->planes_bytes = planes_bytes;
    rec->title = title;
    rec->title_bytes = title_bytes;
    rec->user_port = user_port;
    rec->user_port_owned = user_port_owned;
    rec->size_hook = nullptr;
    rec->size_ctx = nullptr;
    srec->first_window = window;
    write_window_links(screen);
    for (u32& slot : windows_) {
        if (slot == 0) {
            slot = window;
            break;
        }
    }

    begin_batch();
    WindowView w = load_window(b, window);
    w.rp = rp;
    draw_window_chrome(*graphics_, b, w);
    compose(screen, Rect{left, top, right, bottom});
    end_batch();
    if ((a.flags & ndk::WFLG_ACTIVATE) != 0) {
        activate_window(window);
    }
    return window;
}

void Intuition::close_window(u32 window) {
    const u32 index = find_window(window);
    if (index >= kMaxWindows) {
        return;
    }
    if (active_window_ == window) {
        active_window_ = 0;
    }
    if (pressed_gadget_window_ == window) {
        pressed_gadget_window_ = 0;
        pressed_gadget_ = GadgetKind::None;
    }
    WindowRecord* rec = find_window_record(window);
    if (rec == nullptr) {
        remove_window_at(index);
        return;
    }
    const u32 screen = rec->screen;
    const i32 left = rec->left;
    const i32 top = rec->top;
    const i32 width = rec->width;
    const i32 height = rec->height;

    ScreenRecord* srec = find_screen_record(screen);
    if (srec != nullptr) {
        if (srec->first_window == window) {
            srec->first_window = rec->next_window;
        } else {
            u32 prev = srec->first_window;
            for (u32 step = 0; step < kMaxWindows && prev != 0; ++step) {
                WindowRecord* prec = find_window_record(prev);
                if (prec == nullptr) {
                    break;
                }
                if (prec->next_window == window) {
                    prec->next_window = rec->next_window;
                    break;
                }
                prev = prec->next_window;
            }
        }
        write_window_links(screen);
    }

    remove_window_at(index);
    if (rec->user_port != 0) {
        if (rec->user_port_owned) {
            flush_window_messages(rec->user_port);
            exec_->delete_port(rec->user_port);
            Bus(*machine_).w32(window + ndk::wd_UserPort, 0);
        } else {
            remove_window_messages(rec->user_port, window);
        }
    }
    if (rec->title != 0) {
        exec_->free(rec->title, rec->title_bytes);
    }
    if (rec->planes != 0) {
        exec_->free(rec->planes, rec->planes_bytes);
    }
    if (rec->bitmap != 0) {
        exec_->free(rec->bitmap, rec->bitmap_bytes);
    }
    if (rec->layer != 0) {
        exec_->free(rec->layer, rec->layer_bytes);
    }
    if (rec->rp != 0) {
        exec_->free(rec->rp, rec->rp_bytes);
    }
    exec_->free(window, rec->window_bytes);
    *rec = WindowRecord{};

    if (srec != nullptr) {
        compose(screen, Rect{left, top, left + width - 1, top + height - 1});
    }
}

bool Intuition::modify_idcmp(u32 window, u32 flags) {
    if (find_window(window) >= kMaxWindows) {
        return false;
    }
    WindowRecord* rec = find_window_record(window);
    if (rec == nullptr) {
        return false;
    }
    Bus b(*machine_);
    const u32 guest_port = b.r32(window + ndk::wd_UserPort);
    if (!b.ok()) {
        return false;
    }
    if (guest_port != 0 && !(rec->user_port_owned && guest_port == rec->user_port)) {
        if (rec->user_port != 0 && rec->user_port != guest_port) {
            if (rec->user_port_owned) {
                flush_window_messages(rec->user_port);
                exec_->delete_port(rec->user_port);
            } else {
                remove_window_messages(rec->user_port, window);
            }
        }
        rec->user_port = guest_port;
        rec->user_port_owned = false;
    } else if (guest_port == 0 && !rec->user_port_owned && rec->user_port != 0) {
        remove_window_messages(rec->user_port, window);
        rec->user_port = 0;
    }
    if (flags == 0) {
        if (rec->user_port != 0) {
            if (rec->user_port_owned) {
                flush_window_messages(rec->user_port);
                exec_->delete_port(rec->user_port);
                b.w32(window + ndk::wd_UserPort, 0);
            } else {
                remove_window_messages(rec->user_port, window);
            }
            rec->user_port = 0;
            rec->user_port_owned = false;
        }
        b.w32(window + ndk::wd_IDCMPFlags, 0);
        return b.ok();
    }
    if (rec->user_port == 0) {
        const u32 port = exec_->create_port();
        if (port == 0) {
            return false;
        }
        rec->user_port = port;
        rec->user_port_owned = true;
        b.w32(window + ndk::wd_UserPort, port);
    }
    b.w32(window + ndk::wd_IDCMPFlags, flags);
    return b.ok();
}

void Intuition::set_window_titles(u32 window, u32 title, u32 screen_title) {
    const u32 index = find_window(window);
    if (index >= kMaxWindows) {
        return;
    }
    WindowRecord* rec = find_window_record(window);
    if (rec == nullptr) {
        return;
    }
    Bus b(*machine_);
    bool redraw = false;
    if (title != 0xFFFFFFFF) {
        u32 copy = 0;
        u32 bytes = 0;
        if (title != 0) {
            copy = copy_title(b, *exec_, title, bytes);
            if (copy == 0) {
                return;
            }
        }
        if (rec->title != 0) {
            exec_->free(rec->title, rec->title_bytes);
        }
        rec->title = copy;
        rec->title_bytes = bytes;
        b.w32(window + ndk::wd_Title, copy);
        redraw = true;
    }
    if (screen_title != 0xFFFFFFFF) {
        ScreenRecord* srec = find_screen_record(rec->screen);
        if (srec != nullptr) {
            u32 copy = 0;
            u32 bytes = 0;
            if (screen_title != 0) {
                copy = copy_title(b, *exec_, screen_title, bytes);
                if (copy == 0) {
                    return;
                }
            }
            if (srec->title != 0) {
                exec_->free(srec->title, srec->title_bytes);
            }
            srec->title = copy;
            srec->title_bytes = bytes;
            b.w32(rec->screen + ndk::sc_Title, copy);
            b.w32(rec->screen + ndk::sc_DefaultTitle, copy);
            begin_batch();
            draw_screen_bar(*graphics_, b, srec->backdrop_rp, srec->width, srec->title);
            end_batch();
        }
    }
    if (redraw) {
        WindowView w = load_window(b, window);
        w.rp = rec->rp;
        if ((w.flags & ndk::WFLG_BORDERLESS) == 0) {
            begin_batch();
            draw_window_bar(*graphics_, b, w);
            end_batch();
        }
    }
}

void Intuition::window_to_front(u32 window) {
    const u32 index = find_window(window);
    if (index >= kMaxWindows) {
        return;
    }
    WindowRecord* rec = find_window_record(window);
    if (rec == nullptr) {
        return;
    }
    ScreenRecord* srec = find_screen_record(rec->screen);
    if (srec == nullptr || srec->first_window == window) {
        return;
    }
    u32 prev = srec->first_window;
    for (u32 step = 0; step < kMaxWindows && prev != 0; ++step) {
        WindowRecord* prec = find_window_record(prev);
        if (prec == nullptr) {
            return;
        }
        if (prec->next_window == window) {
            prec->next_window = rec->next_window;
            rec->next_window = srec->first_window;
            srec->first_window = window;
            write_window_links(rec->screen);
            compose(rec->screen,
                    Rect{rec->left, rec->top, rec->left + rec->width - 1,
                         rec->top + rec->height - 1});
            return;
        }
        prev = prec->next_window;
    }
}

void Intuition::window_to_back(u32 window) {
    const u32 index = find_window(window);
    if (index >= kMaxWindows) {
        return;
    }
    WindowRecord* rec = find_window_record(window);
    if (rec == nullptr || rec->next_window == 0) {
        return;
    }
    ScreenRecord* srec = find_screen_record(rec->screen);
    if (srec == nullptr || srec->first_window == 0) {
        return;
    }
    if (srec->first_window == window) {
        srec->first_window = rec->next_window;
    } else {
        u32 prev = srec->first_window;
        bool unlinked = false;
        for (u32 step = 0; step < kMaxWindows && prev != 0; ++step) {
            WindowRecord* prec = find_window_record(prev);
            if (prec == nullptr) {
                return;
            }
            if (prec->next_window == window) {
                prec->next_window = rec->next_window;
                unlinked = true;
                break;
            }
            prev = prec->next_window;
        }
        if (!unlinked) {
            return;
        }
    }
    u32 tail = srec->first_window;
    for (u32 step = 0; step < kMaxWindows && tail != 0; ++step) {
        WindowRecord* trec = find_window_record(tail);
        if (trec == nullptr) {
            return;
        }
        if (trec->next_window == 0) {
            trec->next_window = window;
            rec->next_window = 0;
            write_window_links(rec->screen);
            compose(rec->screen,
                    Rect{rec->left, rec->top, rec->left + rec->width - 1,
                         rec->top + rec->height - 1});
            return;
        }
        tail = trec->next_window;
    }
}

void Intuition::set_size_hook(u32 window, WindowHook hook, void* ctx) {
    if (find_window(window) >= kMaxWindows) {
        return;
    }
    WindowRecord* rec = find_window_record(window);
    if (rec == nullptr) {
        return;
    }
    rec->size_hook = hook;
    rec->size_ctx = ctx;
}

void Intuition::pointer_event(i32 x, i32 y, u8 buttons, u8 pressed, u8 released, bool moved) {
    (void)buttons;
    mouse_x_ = x;
    mouse_y_ = y;
    if (front_ == 0) {
        return;
    }
    Bus b(*machine_);
    const u32 hit = window_at(x, y);
    bool left_press_consumed = false;
    bool left_release_consumed = false;

    if ((pressed & kMouseLeft) != 0) {
        if (hit != 0) {
            activate_window(hit);
            const WindowView w = load_window(b, hit);
            if (w.ok && (w.flags & ndk::WFLG_BORDERLESS) == 0) {
                if ((w.flags & ndk::WFLG_CLOSEGADGET) != 0 &&
                    in_rect(close_box_rect(w.left, w.top), x, y)) {
                    pressed_gadget_window_ = hit;
                    pressed_gadget_ = GadgetKind::Close;
                    left_press_consumed = true;
                } else if ((w.flags & ndk::WFLG_DEPTHGADGET) != 0 &&
                           in_rect(depth_box_rect(w.left, w.top, w.width), x, y)) {
                    pressed_gadget_window_ = hit;
                    pressed_gadget_ = GadgetKind::Depth;
                    left_press_consumed = true;
                }
            }
        }
        if (!left_press_consumed) {
            pressed_gadget_window_ = 0;
            pressed_gadget_ = GadgetKind::None;
        }
    }

    if ((released & kMouseLeft) != 0 && pressed_gadget_ != GadgetKind::None) {
        const u32 gad_window = pressed_gadget_window_;
        const GadgetKind gad_kind = pressed_gadget_;
        pressed_gadget_window_ = 0;
        pressed_gadget_ = GadgetKind::None;
        left_release_consumed = true;
        if (gad_window != 0 && hit == gad_window) {
            const WindowView w = load_window(b, gad_window);
            if (w.ok && (w.flags & ndk::WFLG_BORDERLESS) == 0) {
                if (gad_kind == GadgetKind::Close && (w.flags & ndk::WFLG_CLOSEGADGET) != 0 &&
                    in_rect(close_box_rect(w.left, w.top), x, y)) {
                    post_intuimessage(gad_window, ndk::IDCMP_CLOSEWINDOW, 0);
                } else if (gad_kind == GadgetKind::Depth &&
                           (w.flags & ndk::WFLG_DEPTHGADGET) != 0 &&
                           in_rect(depth_box_rect(w.left, w.top, w.width), x, y)) {
                    const ScreenRecord* srec = find_screen_record(front_);
                    if (srec != nullptr && srec->first_window == gad_window) {
                        window_to_back(gad_window);
                    } else {
                        window_to_front(gad_window);
                    }
                }
            }
        }
    }

    if (active_window_ != 0) {
        if ((pressed & kMouseLeft) != 0 && !left_press_consumed) {
            post_intuimessage(active_window_, ndk::IDCMP_MOUSEBUTTONS, ndk::SELECTDOWN);
        }
        if ((released & kMouseLeft) != 0 && !left_release_consumed) {
            post_intuimessage(active_window_, ndk::IDCMP_MOUSEBUTTONS, ndk::SELECTUP);
        }
        if ((pressed & kMouseRight) != 0) {
            post_intuimessage(active_window_, ndk::IDCMP_MOUSEBUTTONS, ndk::MENUDOWN);
        }
        if ((released & kMouseRight) != 0) {
            post_intuimessage(active_window_, ndk::IDCMP_MOUSEBUTTONS, ndk::MENUUP);
        }
        if ((pressed & kMouseMiddle) != 0) {
            post_intuimessage(active_window_, ndk::IDCMP_MOUSEBUTTONS, ndk::MIDDLEDOWN);
        }
        if ((released & kMouseMiddle) != 0) {
            post_intuimessage(active_window_, ndk::IDCMP_MOUSEBUTTONS, ndk::MIDDLEUP);
        }
    }

    if (moved && active_window_ != 0) {
        const WindowView w = load_window(b, active_window_);
        if (w.ok && (w.flags & ndk::WFLG_REPORTMOUSE) != 0) {
            post_intuimessage(active_window_, ndk::IDCMP_MOUSEMOVE, 0);
        }
    }
}

void Intuition::reclaim(u32 keep_window, u32 keep_screen) {
    u32 win_to_close[kMaxWindows]{};
    u32 win_count = 0;
    for (u32 window : windows_) {
        if (window != 0 && window != keep_window) {
            win_to_close[win_count++] = window;
        }
    }
    for (u32 i = 0; i < win_count; ++i) {
        close_window(win_to_close[i]);
    }

    u32 scr_to_close[kMaxScreens]{};
    u32 scr_count = 0;
    for (u32 screen : screens_) {
        if (screen != 0 && screen != keep_screen) {
            scr_to_close[scr_count++] = screen;
        }
    }
    for (u32 i = 0; i < scr_count; ++i) {
        close_screen(scr_to_close[i]);
    }
    if (keep_screen != 0 && front_ != keep_screen && find_screen(keep_screen) < kMaxScreens) {
        screen_to_front(keep_screen);
    }

    if (keep_window != 0) {
        if (const WindowRecord* krec = find_window_record(keep_window);
            krec != nullptr && krec->user_port != 0) {
            remove_window_messages(krec->user_port, keep_window);
        }
    }
    for (u32& slot : messages_) {
        if (slot != 0) {
            const u32 msg = slot;
            slot = 0;
            if (exec_ != nullptr) {
                exec_->free(msg, ndk::IntuiMessage_SIZEOF);
            }
        }
    }

    if (active_window_ != 0 && find_window(active_window_) >= kMaxWindows) {
        active_window_ = 0;
    }
    pressed_gadget_window_ = 0;
    pressed_gadget_ = GadgetKind::None;
}

bool Intuition::window_open(u32 window) const {
    return find_window(window) < kMaxWindows;
}

}
