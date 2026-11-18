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

#include "exec.hpp"
#include "graphics.hpp"
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
// there is no memory or the string cannot be read.
u32 copy_title(Bus& b, Exec& exec, u32 src) {
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
    return at;
}

u32 title_length(Bus& b, u32 title) {
    u32 length = 0;
    if (title != 0) {
        while (b.r8(title + length) != 0 && b.ok()) {
            ++length;
        }
    }
    return length + 1;
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
    a.flags = b.r16(nw + ndk::nw_Flags);
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

// The Window fields a draw or a free needs.
struct WindowView {
    u32 window = 0;
    u32 screen = 0;
    u32 rp = 0;
    u32 layer = 0;
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
    w.flags = b.r16(window + ndk::wd_Flags);
    w.title = b.r32(window + ndk::wd_Title);
    w.screen = b.r32(window + ndk::wd_WScreen);
    w.rp = b.r32(window + ndk::wd_RPort);
    w.layer = b.r32(window + ndk::wd_WLayer);
    w.border_top = b.r8(window + ndk::wd_BorderTop);
    w.ok = b.ok();
    return w;
}

void draw_gadget(Graphics& graphics, u32 rp, i32 x, i32 y) {
    graphics.set_pens(rp, 2, 1, ndk::JAM2);
    graphics.rect_fill(rp, x, y, x + 7, y + 7);
    graphics.set_pens(rp, 1, 0, ndk::JAM2);
    graphics.rect_fill(rp, x, y, x + 7, y);
    graphics.rect_fill(rp, x, y + 7, x + 7, y + 7);
    graphics.rect_fill(rp, x, y, x, y + 7);
    graphics.rect_fill(rp, x + 7, y, x + 7, y + 7);
}

// The 10-row title bar of a window with its gadgets, line and title.
void draw_window_bar(Graphics& graphics, Bus& b, const WindowView& w) {
    const u32 rp = w.screen + ndk::sc_RastPort;
    const i32 left = w.left + 1;
    const i32 right = w.left + w.width - 2;
    const i32 top = w.top + 1;
    const i32 line = w.top + static_cast<i32>(kBarHeight);
    graphics.set_pens(rp, 2, 1, ndk::JAM2);
    graphics.rect_fill(rp, left, top, right, line);
    if ((w.flags & ndk::WFLG_CLOSEGADGET) != 0) {
        draw_gadget(graphics, rp, left, top);
    }
    if ((w.flags & ndk::WFLG_DEPTHGADGET) != 0) {
        draw_gadget(graphics, rp, right - 7, top);
    }
    graphics.set_pens(rp, 1, 0, ndk::JAM2);
    graphics.rect_fill(rp, left, line, right, line);
    const i32 text_x = (w.flags & ndk::WFLG_CLOSEGADGET) != 0 ? w.left + 11 : w.left + 2;
    const i32 text_x1 = (w.flags & ndk::WFLG_DEPTHGADGET) != 0 ? right - 8 : right;
    graphics.set_pens(rp, 1, 2, ndk::JAM1);
    draw_title(graphics, b, rp, w.title, text_x, w.top + 7, text_x1);
}

void draw_window_chrome(Graphics& graphics, Bus& b, const WindowView& w) {
    const u32 rp = w.screen + ndk::sc_RastPort;
    const i32 right = w.left + w.width - 1;
    const i32 bottom = w.top + w.height - 1;
    graphics.set_pens(rp, 0, 0, ndk::JAM2);
    graphics.rect_fill(rp, w.left, w.top, right, bottom);
    graphics.set_pens(rp, 1, 0, ndk::JAM2);
    graphics.rect_fill(rp, w.left, w.top, right, w.top);
    graphics.rect_fill(rp, w.left, bottom, right, bottom);
    graphics.rect_fill(rp, w.left, w.top, w.left, bottom);
    graphics.rect_fill(rp, right, w.top, right, bottom);
    if ((w.flags & ndk::WFLG_BORDERLESS) == 0) {
        draw_window_bar(graphics, b, w);
    }
    graphics.set_pens(rp, 0, 0, ndk::JAM2);
    graphics.rect_fill(rp, w.left + 1, w.top + static_cast<i32>(w.border_top), right - 1,
                       bottom - 1);
}

}

struct IntuitionNatives {
    static Intuition& of(void* ctx) { return *static_cast<Intuition*>(ctx); }

    static void close_screen(void* ctx, Machine& m) { of(ctx).close_screen(m.cpu.a[0]); }

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

    static void open_window(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).open_window(m.cpu.a[0], 0));
    }

    static void open_window_tag_list(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).open_window(m.cpu.a[0], m.cpu.a[1]));
    }

    static void set_window_titles(void* ctx, Machine& m) {
        of(ctx).set_window_titles(m.cpu.a[0], m.cpu.a[1], m.cpu.a[2]);
    }

    static void window_to_back(void*, Machine&) {}

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

bool Intuition::init(Machine& m, Exec& exec, Graphics& graphics, DisplayState& display) {
    machine_ = &m;
    exec_ = &exec;
    graphics_ = &graphics;
    display_ = &display;
    base_ = 0;
    front_ = 0;
    display.front_screen = 0;
    for (u32& screen : screens_) {
        screen = 0;
    }
    for (u32& window : windows_) {
        window = 0;
    }
    for (NativeVector& v : vectors_) {
        v = NativeVector{nullptr, nullptr};
    }
    for (const IntuitionFunction& f : kIntuitionFunctions) {
        vectors_[f.lvo / ndk::LIB_VECTSIZE - 1] = NativeVector{f.fn, f.name};
    }
    const u32 base = exec.make_native_library(kIntuitionName, kHombreLibVersion, vectors_, kVectors,
                                              this);
    if (base == 0) {
        return false;
    }
    base_ = base;
    return true;
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

void Intuition::write_links() {
    Bus b(*machine_);
    for (u32 i = 0; i < kMaxScreens; ++i) {
        if (screens_[i] != 0) {
            const u32 next = i + 1 < kMaxScreens ? screens_[i + 1] : 0;
            b.w32(screens_[i] + kScNextScreen, next);
        }
    }
}

void Intuition::refresh_screen(u32 screen) {
    if (screen == 0 || display_ == nullptr) {
        return;
    }
    Bus b(*machine_);
    const u32 width = b.r16(screen + ndk::sc_Width);
    const u32 height = b.r16(screen + ndk::sc_Height);
    if (b.ok()) {
        refresh_full(*display_, *machine_, screen, width, height);
    }
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

    const u64 bytes_per_row = (static_cast<u64>(a.width) + 15) / 16 * 2;
    const u64 planes_bytes = 2 * bytes_per_row * a.height;
    if (planes_bytes > 0xFFFFFFFF) {
        LogLine().text("intuition: screen too large").send();
        return 0;
    }
    const u32 bpr = static_cast<u32>(bytes_per_row);

    const u32 screen = exec_->alloc(ndk::Screen_SIZEOF, ndk::MEMF_CHIP | ndk::MEMF_CLEAR);
    if (screen == 0) {
        LogLine().text("intuition: no memory for a screen").send();
        return 0;
    }
    const u32 planes = exec_->alloc(static_cast<u32>(planes_bytes),
                                    ndk::MEMF_CHIP | ndk::MEMF_CLEAR);
    if (planes == 0) {
        exec_->free(screen, ndk::Screen_SIZEOF);
        LogLine().text("intuition: no memory for a screen").send();
        return 0;
    }
    const u32 map = exec_->alloc(ndk::ColorMap_SIZEOF, ndk::MEMF_CHIP | ndk::MEMF_CLEAR);
    if (map == 0) {
        exec_->free(planes, static_cast<u32>(planes_bytes));
        exec_->free(screen, ndk::Screen_SIZEOF);
        LogLine().text("intuition: no memory for a screen").send();
        return 0;
    }
    const u32 colors = exec_->alloc(8, ndk::MEMF_CHIP | ndk::MEMF_CLEAR);
    if (colors == 0) {
        exec_->free(map, ndk::ColorMap_SIZEOF);
        exec_->free(planes, static_cast<u32>(planes_bytes));
        exec_->free(screen, ndk::Screen_SIZEOF);
        LogLine().text("intuition: no memory for a screen").send();
        return 0;
    }
    u32 title = 0;
    if (a.title != 0) {
        title = copy_title(b, *exec_, a.title);
        if (title == 0) {
            exec_->free(colors, 8);
            exec_->free(map, ndk::ColorMap_SIZEOF);
            exec_->free(planes, static_cast<u32>(planes_bytes));
            exec_->free(screen, ndk::Screen_SIZEOF);
            LogLine().text("intuition: no memory for a screen").send();
            return 0;
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
    b.w32(screen + ndk::sc_Font, graphics_->topaz_font());
    b.w32(screen + kScNextScreen, 0);

    b.w16(bitmap + ndk::bm_BytesPerRow, bpr);
    b.w16(bitmap + ndk::bm_Rows, a.height);
    b.w8(bitmap + ndk::bm_Flags, 0);
    b.w8(bitmap + ndk::bm_Depth, a.depth);
    b.w32(bitmap + ndk::bm_Planes, planes);
    b.w32(bitmap + ndk::bm_Planes + 4, planes + bpr * a.height);
    for (u32 plane = 2; plane < 8; ++plane) {
        b.w32(bitmap + ndk::bm_Planes + plane * 4, 0);
    }

    graphics_->init_rastport(rp);
    b.w32(rp + ndk::rp_BitMap, bitmap);

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

    const i32 right = static_cast<i32>(a.width) - 1;
    const i32 bottom = static_cast<i32>(a.height) - 1;
    graphics_->set_pens(rp, 0, 0, ndk::JAM2);
    graphics_->rect_fill(rp, 0, 0, right, bottom);
    graphics_->set_pens(rp, 2, 1, ndk::JAM2);
    graphics_->rect_fill(rp, 0, 0, right, static_cast<i32>(kBarHeight) - 1);
    graphics_->set_pens(rp, 1, 1, ndk::JAM2);
    graphics_->rect_fill(rp, 0, static_cast<i32>(kBarHeight), right,
                         static_cast<i32>(kBarHeight));
    if (title != 0) {
        graphics_->set_pens(rp, 1, 2, ndk::JAM1);
        draw_title(*graphics_, b, rp, title, 1, 7, right);
    }
    return screen;
}

void Intuition::close_screen(u32 screen) {
    const u32 index = find_screen(screen);
    if (index >= kMaxScreens) {
        return;
    }
    Bus b(*machine_);
    u32 doomed[kMaxWindows];
    u32 doomed_count = 0;
    for (u32 window : windows_) {
        if (window != 0 && b.r32(window + ndk::wd_WScreen) == screen) {
            doomed[doomed_count++] = window;
        }
    }
    for (u32 i = 0; i < doomed_count; ++i) {
        close_window(doomed[i]);
    }
    const u32 width = b.r16(screen + ndk::sc_Width);
    const u32 height = b.r16(screen + ndk::sc_Height);
    const u32 title = b.r32(screen + ndk::sc_Title);
    const u32 bitmap = screen + ndk::sc_BitMap;
    const u32 bpr = b.r16(bitmap + ndk::bm_BytesPerRow);
    const u32 rows = b.r16(bitmap + ndk::bm_Rows);
    const u32 planes = b.r32(bitmap + ndk::bm_Planes);
    const u32 map = b.r32(screen + ndk::sc_ViewPort + ndk::vp_ColorMap);
    const u32 colors = map != 0 ? b.r32(map + ndk::cm_ColorTable) : 0;
    const u32 title_size = title_length(b, title);
    const bool was_front = index == 0;

    remove_at(index);
    write_links();
    if (planes != 0) {
        exec_->free(planes, 2 * bpr * rows);
    }
    if (colors != 0) {
        exec_->free(colors, 8);
    }
    if (map != 0) {
        exec_->free(map, ndk::ColorMap_SIZEOF);
    }
    if (title != 0) {
        exec_->free(title, title_size);
    }
    exec_->free(screen, ndk::Screen_SIZEOF);

    if (was_front) {
        front_ = screens_[0];
        display_->front_screen = front_;
        if (front_ != 0) {
            refresh_screen(front_);
        } else {
            refresh_full(*display_, *machine_, 0, width, height);
        }
    }
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
    if (!b.ok() || screen_w <= 0 || screen_h <= 0) {
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
    const bool borderless = (a.flags & ndk::WFLG_BORDERLESS) != 0;
    const u32 border_top = borderless ? 1 : 11;

    const u32 window = exec_->alloc(ndk::Window_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (window == 0) {
        LogLine().text("intuition: no memory for a window").send();
        return 0;
    }
    const u32 rp = exec_->alloc(ndk::RastPort_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (rp == 0) {
        exec_->free(window, ndk::Window_SIZEOF);
        LogLine().text("intuition: no memory for a window").send();
        return 0;
    }
    const u32 layer = exec_->alloc(ndk::Layer_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (layer == 0) {
        exec_->free(rp, ndk::RastPort_SIZEOF);
        exec_->free(window, ndk::Window_SIZEOF);
        LogLine().text("intuition: no memory for a window").send();
        return 0;
    }
    u32 title = 0;
    if (a.title != 0) {
        title = copy_title(b, *exec_, a.title);
        if (title == 0) {
            exec_->free(layer, ndk::Layer_SIZEOF);
            exec_->free(rp, ndk::RastPort_SIZEOF);
            exec_->free(window, ndk::Window_SIZEOF);
            LogLine().text("intuition: no memory for a window").send();
            return 0;
        }
    }

    const u32 bitmap = screen + ndk::sc_BitMap;
    b.w16(window + ndk::wd_LeftEdge, static_cast<u32>(left));
    b.w16(window + ndk::wd_TopEdge, static_cast<u32>(top));
    b.w16(window + ndk::wd_Width, static_cast<u32>(width));
    b.w16(window + ndk::wd_Height, static_cast<u32>(height));
    b.w16(window + ndk::wd_MinWidth, a.min_width);
    b.w16(window + ndk::wd_MinHeight, a.min_height);
    b.w16(window + ndk::wd_MaxWidth, a.max_width);
    b.w16(window + ndk::wd_MaxHeight, a.max_height);
    b.w16(window + ndk::wd_Flags, a.flags);
    b.w32(window + ndk::wd_Title, title);
    b.w32(window + ndk::wd_WScreen, screen);
    b.w32(window + ndk::wd_RPort, rp);
    b.w8(window + ndk::wd_BorderLeft, 1);
    b.w8(window + ndk::wd_BorderTop, border_top);
    b.w8(window + ndk::wd_BorderRight, 1);
    b.w8(window + ndk::wd_BorderBottom, 1);
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

    const i32 inner_x0 = left + 1;
    const i32 inner_y0 = top + static_cast<i32>(border_top);
    const i32 inner_x1 = left + width - 2;
    const i32 inner_y1 = top + height - 2;
    b.w32(layer + ndk::ly_rp, rp);
    b.w16(layer + ndk::ly_MinX, static_cast<u32>(inner_x0));
    b.w16(layer + ndk::ly_MinY, static_cast<u32>(inner_y0));
    b.w16(layer + ndk::ly_MaxX, static_cast<u32>(inner_x1));
    b.w16(layer + ndk::ly_MaxY, static_cast<u32>(inner_y1));
    b.w32(layer + ndk::ly_Window, window);

    const u32 head = b.r32(screen + ndk::sc_FirstWindow);
    b.w32(window + ndk::wd_NextWindow, head);
    b.w32(screen + ndk::sc_FirstWindow, window);
    for (u32& slot : windows_) {
        if (slot == 0) {
            slot = window;
            break;
        }
    }

    draw_window_chrome(*graphics_, b, load_window(b, window));
    return window;
}

void Intuition::close_window(u32 window) {
    const u32 index = find_window(window);
    if (index >= kMaxWindows) {
        return;
    }
    Bus b(*machine_);
    const WindowView w = load_window(b, window);
    if (!w.ok) {
        remove_window_at(index);
        exec_->free(window, ndk::Window_SIZEOF);
        return;
    }
    if (find_screen(w.screen) < kMaxScreens) {
        const u32 next = b.r32(window + ndk::wd_NextWindow);
        const u32 head = b.r32(w.screen + ndk::sc_FirstWindow);
        if (head == window) {
            b.w32(w.screen + ndk::sc_FirstWindow, next);
        } else {
            u32 prev = head;
            for (u32 step = 0; step < kMaxWindows && prev != 0; ++step) {
                const u32 link = b.r32(prev + ndk::wd_NextWindow);
                if (link == window) {
                    b.w32(prev + ndk::wd_NextWindow, next);
                    break;
                }
                prev = link;
            }
        }
        const u32 rp = w.screen + ndk::sc_RastPort;
        graphics_->set_pens(rp, 0, 0, ndk::JAM2);
        graphics_->rect_fill(rp, w.left, w.top, w.left + w.width - 1, w.top + w.height - 1);
    }
    const u32 title_size = title_length(b, w.title);
    remove_window_at(index);
    if (w.title != 0) {
        exec_->free(w.title, title_size);
    }
    if (w.layer != 0) {
        exec_->free(w.layer, ndk::Layer_SIZEOF);
    }
    if (w.rp != 0) {
        exec_->free(w.rp, ndk::RastPort_SIZEOF);
    }
    exec_->free(window, ndk::Window_SIZEOF);
}

void Intuition::set_window_titles(u32 window, u32 title, u32 screen_title) {
    const u32 index = find_window(window);
    if (index >= kMaxWindows) {
        return;
    }
    Bus b(*machine_);
    bool redraw = false;
    if (title != 0xFFFFFFFF) {
        u32 copy = 0;
        if (title != 0) {
            copy = copy_title(b, *exec_, title);
            if (copy == 0) {
                return;
            }
        }
        const u32 old = b.r32(window + ndk::wd_Title);
        const u32 old_size = title_length(b, old);
        b.w32(window + ndk::wd_Title, copy);
        if (old != 0) {
            exec_->free(old, old_size);
        }
        redraw = true;
    }
    if (screen_title != 0xFFFFFFFF) {
        const WindowView w = load_window(b, window);
        if (w.ok && find_screen(w.screen) < kMaxScreens) {
            u32 copy = 0;
            if (screen_title != 0) {
                copy = copy_title(b, *exec_, screen_title);
                if (copy == 0) {
                    return;
                }
            }
            const u32 old = b.r32(w.screen + ndk::sc_Title);
            const u32 old_size = title_length(b, old);
            b.w32(w.screen + ndk::sc_Title, copy);
            b.w32(w.screen + ndk::sc_DefaultTitle, copy);
            if (old != 0) {
                exec_->free(old, old_size);
            }
        }
    }
    if (redraw) {
        const WindowView w = load_window(b, window);
        if ((w.flags & ndk::WFLG_BORDERLESS) == 0) {
            draw_window_bar(*graphics_, b, w);
        }
    }
}

void Intuition::window_to_front(u32 window) {
    const u32 index = find_window(window);
    if (index >= kMaxWindows) {
        return;
    }
    Bus b(*machine_);
    const u32 screen = b.r32(window + ndk::wd_WScreen);
    if (!b.ok() || screen == 0 || find_screen(screen) >= kMaxScreens) {
        return;
    }
    const u32 head = b.r32(screen + ndk::sc_FirstWindow);
    if (head == window) {
        return;
    }
    u32 prev = head;
    u32 next = 0;
    bool found = false;
    for (u32 step = 0; step < kMaxWindows && prev != 0; ++step) {
        next = b.r32(prev + ndk::wd_NextWindow);
        if (next == window) {
            found = true;
            break;
        }
        prev = next;
    }
    if (!found) {
        return;
    }
    b.w32(prev + ndk::wd_NextWindow, b.r32(window + ndk::wd_NextWindow));
    b.w32(window + ndk::wd_NextWindow, head);
    b.w32(screen + ndk::sc_FirstWindow, window);
}

}
