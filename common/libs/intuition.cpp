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

// Draws the title at pen 1 over the bar, which stays pen 2 where the glyph
// leaves it alone.
void draw_title(Graphics& graphics, Bus& b, u32 rp, u32 title) {
    graphics.move(rp, 1, 7);
    u8 buffer[kTitleChunk];
    for (u32 at = 0; at < kMaxTitleChars; at += kTitleChunk) {
        u32 n = kMaxTitleChars - at;
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
};

namespace {

struct IntuitionFunction {
    u32 lvo;
    m68k::NativeFn fn;
    const char* name;
};

constexpr IntuitionFunction kIntuitionFunctions[] = {
    {intuition_lvo::CloseScreen, IntuitionNatives::close_screen, "CloseScreen"},
    {intuition_lvo::OpenScreen, IntuitionNatives::open_screen, "OpenScreen"},
    {intuition_lvo::ScreenToBack, IntuitionNatives::screen_to_back, "ScreenToBack"},
    {intuition_lvo::ScreenToFront, IntuitionNatives::screen_to_front, "ScreenToFront"},
    {intuition_lvo::LockIBase, IntuitionNatives::lock_i_base, "LockIBase"},
    {intuition_lvo::UnlockIBase, IntuitionNatives::unlock_i_base, "UnlockIBase"},
    {intuition_lvo::GetScreenData, IntuitionNatives::get_screen_data, "GetScreenData"},
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
        draw_title(*graphics_, b, rp, title);
    }
    return screen;
}

void Intuition::close_screen(u32 screen) {
    const u32 index = find_screen(screen);
    if (index >= kMaxScreens) {
        return;
    }
    Bus b(*machine_);
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

}
