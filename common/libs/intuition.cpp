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
#include "chrome.hpp"
#include "chrome_palette.hpp"
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
constexpr u32 kMaxTitleChars = 255;

constexpr u32 kBasePalette[8] = {
    0xA0A6B0, 0x1C1E22, 0xFFFFFF, 0x3D6FB6, 0x7A7E86, 0xC9CDD4, 0xD9A441, 0x9E3B3B,
};
constexpr u32 kCubeLevels[6] = {0x00, 0x33, 0x66, 0x99, 0xCC, 0xFF};

constexpr u32 default_palette_rgb24(u32 pen) {
    if (pen < 8) {
        return kBasePalette[pen];
    }
    if (pen < 224) {
        const u32 idx = pen - 8;
        const u32 r = idx / 36;
        const u32 g = (idx / 6) % 6;
        const u32 b = idx % 6;
        return (kCubeLevels[r] << 16) | (kCubeLevels[g] << 8) | kCubeLevels[b];
    }
    if (pen < chrome::kChromeBase) {
        const u32 v = 0x10u + (pen - 224u) * 0x20u;
        return (v << 16) | (v << 8) | v;
    }
    if (pen < 256) {
        return chrome::kChromePalette[pen - chrome::kChromeBase];
    }
    return 0;
}

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
    u32 gadgets = 0;
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
    a.gadgets = b.r32(nw + ndk::nw_FirstGadget);
    a.title = b.r32(nw + ndk::nw_Title);
    a.screen = b.r32(nw + ndk::nw_Screen);
    a.min_width = b.r16(nw + ndk::nw_MinWidth);
    a.min_height = b.r16(nw + ndk::nw_MinHeight);
    a.max_width = b.r16(nw + ndk::nw_MaxWidth);
    a.max_height = b.r16(nw + ndk::nw_MaxHeight);
    a.type = b.r16(nw + ndk::nw_Type);
}

void set_window_flag(u32& flags, u32 bit, u32 data) {
    if (data != 0) {
        flags |= bit;
    } else {
        flags &= ~bit;
    }
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
    case ndk::WA_Gadgets:
        a.gadgets = data;
        break;
    case ndk::WA_Title:
        a.title = data;
        break;
    case ndk::WA_CustomScreen:
        a.screen = data;
        a.custom_screen = true;
        break;
    case ndk::WA_MinWidth:
        a.min_width = data;
        break;
    case ndk::WA_MinHeight:
        a.min_height = data;
        break;
    case ndk::WA_MaxWidth:
        a.max_width = data;
        break;
    case ndk::WA_MaxHeight:
        a.max_height = data;
        break;
    case ndk::WA_SizeGadget:
        set_window_flag(a.flags, ndk::WFLG_SIZEGADGET, data);
        break;
    case ndk::WA_DragBar:
        set_window_flag(a.flags, ndk::WFLG_DRAGBAR, data);
        break;
    case ndk::WA_DepthGadget:
        set_window_flag(a.flags, ndk::WFLG_DEPTHGADGET, data);
        break;
    case ndk::WA_CloseGadget:
        set_window_flag(a.flags, ndk::WFLG_CLOSEGADGET, data);
        break;
    case ndk::WA_Backdrop:
        set_window_flag(a.flags, ndk::WFLG_BACKDROP, data);
        break;
    case ndk::WA_ReportMouse:
        set_window_flag(a.flags, ndk::WFLG_REPORTMOUSE, data);
        break;
    case ndk::WA_Borderless:
        set_window_flag(a.flags, ndk::WFLG_BORDERLESS, data);
        break;
    case ndk::WA_Activate:
        set_window_flag(a.flags, ndk::WFLG_ACTIVATE, data);
        break;
    case ndk::WA_SizeBRight:
        set_window_flag(a.flags, ndk::WFLG_SIZEBRIGHT, data);
        break;
    case ndk::WA_SizeBBottom:
        set_window_flag(a.flags, ndk::WFLG_SIZEBBOTTOM, data);
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

constexpr u8 kTopCornerInsets[3] = {3, 2, 1};
constexpr i32 kShadowSize = 4;

constexpr bool has_rounded_corners(u32 screen_depth, u32 window_flags) {
    return screen_depth >= 8 &&
           (window_flags & (ndk::WFLG_BORDERLESS | ndk::WFLG_BACKDROP)) == 0;
}

constexpr bool has_shadow(u32 screen_depth, u32 window_flags) {
    return screen_depth >= 8 &&
           (window_flags & (ndk::WFLG_BORDERLESS | ndk::WFLG_BACKDROP)) == 0;
}

Rect window_damage_rect(u32 screen_depth, i32 left, i32 top, i32 width, i32 height,
                        u32 window_flags) {
    Rect r{left, top, left + width - 1, top + height - 1};
    if (has_shadow(screen_depth, window_flags)) {
        r.x1 += kShadowSize;
        r.y1 += kShadowSize;
    }
    return r;
}

Rect intersect_rect(const Rect& a, const Rect& b) {
    return Rect{
        a.x0 > b.x0 ? a.x0 : b.x0,
        a.y0 > b.y0 ? a.y0 : b.y0,
        a.x1 < b.x1 ? a.x1 : b.x1,
        a.y1 < b.y1 ? a.y1 : b.y1,
    };
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
    u32 border_left = 0;
    u32 border_top = 0;
    u32 border_right = 0;
    u32 border_bottom = 0;
    u32 first_gadget = 0;
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
    w.border_left = b.r8(window + ndk::wd_BorderLeft);
    w.border_top = b.r8(window + ndk::wd_BorderTop);
    w.border_right = b.r8(window + ndk::wd_BorderRight);
    w.border_bottom = b.r8(window + ndk::wd_BorderBottom);
    w.first_gadget = b.r32(window + ndk::wd_FirstGadget);
    w.ok = b.ok();
    return w;
}

constexpr bool in_rect(const Rect& r, i32 x, i32 y) {
    return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1;
}

i32 clamp_i32(i32 v, i32 lo, i32 hi) {
    if (v < lo) {
        return lo;
    }
    return v > hi ? hi : v;
}

i32 prop_knob_span(i32 inner_span, u32 body) {
    i32 span = static_cast<i32>((static_cast<u64>(inner_span) * body) / ndk::MAXBODY);
    if (span < 4) {
        span = 4;
    }
    return span > inner_span ? inner_span : span;
}

u16 pot_from_offset(i32 offset, i32 travel) {
    if (travel <= 0) {
        return 0;
    }
    offset = clamp_i32(offset, 0, travel);
    const u64 num = static_cast<u64>(offset) * ndk::MAXPOT + static_cast<u64>(travel) / 2;
    const u64 val = num / static_cast<u64>(travel);
    return static_cast<u16>(val > ndk::MAXPOT ? ndk::MAXPOT : val);
}

struct PropGeometry {
    Rect box{};
    Rect inner{};
    Rect knob{};
    u32 pi = 0;
    u16 pflags = 0;
    u16 hpot = 0;
    u16 vpot = 0;
    u16 hbody = 0;
    u16 vbody = 0;
    i32 travel_x = 0;
    i32 travel_y = 0;
    bool ok = false;
};

Rect resolve_gadget_box(Bus& b, const WindowView& w, u32 gadget) {
    i32 x = static_cast<i16>(b.r16(gadget + ndk::gg_LeftEdge));
    i32 y = static_cast<i16>(b.r16(gadget + ndk::gg_TopEdge));
    i32 gw = static_cast<i16>(b.r16(gadget + ndk::gg_Width));
    i32 gh = static_cast<i16>(b.r16(gadget + ndk::gg_Height));
    const u32 flags = b.r16(gadget + ndk::gg_Flags);
    if (!b.ok()) {
        return Rect{0, 0, -1, -1};
    }
    if ((flags & ndk::GFLG_RELRIGHT) != 0) {
        x += w.width - 1;
    }
    if ((flags & ndk::GFLG_RELBOTTOM) != 0) {
        y += w.height - 1;
    }
    if ((flags & ndk::GFLG_RELWIDTH) != 0) {
        gw += w.width;
    }
    if ((flags & ndk::GFLG_RELHEIGHT) != 0) {
        gh += w.height;
    }
    if (gw <= 0 || gh <= 0) {
        return Rect{0, 0, -1, -1};
    }
    return Rect{x, y, x + gw - 1, y + gh - 1};
}

PropGeometry load_prop_geometry(Bus& b, const WindowView& w, u32 gadget) {
    PropGeometry g;
    g.box = resolve_gadget_box(b, w, gadget);
    if (g.box.x1 < g.box.x0 || g.box.y1 < g.box.y0) {
        return g;
    }
    g.pi = b.r32(gadget + ndk::gg_SpecialInfo);
    if (g.pi == 0 || !b.ok()) {
        return g;
    }
    g.pflags = static_cast<u16>(b.r16(g.pi + ndk::pi_Flags));
    g.hpot = static_cast<u16>(b.r16(g.pi + ndk::pi_HorizPot));
    g.vpot = static_cast<u16>(b.r16(g.pi + ndk::pi_VertPot));
    g.hbody = static_cast<u16>(b.r16(g.pi + ndk::pi_HorizBody));
    g.vbody = static_cast<u16>(b.r16(g.pi + ndk::pi_VertBody));
    if (!b.ok()) {
        return g;
    }
    g.inner = Rect{g.box.x0 + 1, g.box.y0 + 1, g.box.x1 - 1, g.box.y1 - 1};
    const i32 inner_w = g.inner.x1 - g.inner.x0 + 1;
    const i32 inner_h = g.inner.y1 - g.inner.y0 + 1;
    if (inner_w <= 0 || inner_h <= 0) {
        g.knob = Rect{0, 0, -1, -1};
        g.ok = true;
        return g;
    }
    i32 knob_w = inner_w;
    i32 knob_x0 = g.inner.x0;
    if ((g.pflags & ndk::FREEHORIZ) != 0) {
        knob_w = prop_knob_span(inner_w, g.hbody);
        g.travel_x = inner_w - knob_w;
        knob_x0 = g.inner.x0 +
                  static_cast<i32>((static_cast<u64>(g.travel_x) * g.hpot) / ndk::MAXPOT);
    }
    i32 knob_h = inner_h;
    i32 knob_y0 = g.inner.y0;
    if ((g.pflags & ndk::FREEVERT) != 0) {
        knob_h = prop_knob_span(inner_h, g.vbody);
        g.travel_y = inner_h - knob_h;
        knob_y0 = g.inner.y0 +
                  static_cast<i32>((static_cast<u64>(g.travel_y) * g.vpot) / ndk::MAXPOT);
    }
    g.knob = Rect{knob_x0, knob_y0, knob_x0 + knob_w - 1, knob_y0 + knob_h - 1};
    g.ok = true;
    return g;
}

chrome::WindowChrome to_chrome(const WindowView& w, bool active) {
    return chrome::WindowChrome{
        w.width,
        w.height,
        w.flags,
        w.title,
        w.border_left,
        w.border_top,
        w.border_right,
        w.border_bottom,
        active,
    };
}

void draw_prop_gadget(Graphics& graphics, Bus& b, const WindowView& w, u32 gadget) {
    const u32 rp = w.rp;
    if (rp == 0) {
        return;
    }
    const PropGeometry g = load_prop_geometry(b, w, gadget);
    if (!g.ok) {
        return;
    }
    const u16 gflags = static_cast<u16>(b.r16(gadget + ndk::gg_Flags));
    if (!b.ok()) {
        return;
    }
    chrome::draw_prop_gadget(graphics, b, rp, chrome::PropChrome{g.box, g.knob, g.pflags, gflags});
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

    static void add_gadget(void* ctx, Machine& m) {
        native::set_result(
            m, of(ctx).add_glist(m.cpu.a[0], m.cpu.a[1],
                                 static_cast<i16>(m.cpu.d[0] & 0xFFFF), 1));
    }

    static void modify_prop(void* ctx, Machine& m) {
        of(ctx).new_modify_prop(m.cpu.a[0], m.cpu.a[1], m.cpu.d[0] & 0xFFFF,
                                m.cpu.d[1] & 0xFFFF, m.cpu.d[2] & 0xFFFF, m.cpu.d[3] & 0xFFFF,
                                m.cpu.d[4] & 0xFFFF, 1);
    }

    static void off_gadget(void* ctx, Machine& m) {
        of(ctx).off_gadget(m.cpu.a[0], m.cpu.a[1]);
    }

    static void on_gadget(void* ctx, Machine& m) {
        of(ctx).on_gadget(m.cpu.a[0], m.cpu.a[1]);
    }

    static void refresh_gadgets(void* ctx, Machine& m) {
        of(ctx).refresh_glist(m.cpu.a[0], m.cpu.a[1], -1);
    }

    static void remove_gadget(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).remove_glist(m.cpu.a[0], m.cpu.a[1], 1));
    }

    static void refresh_glist(void* ctx, Machine& m) {
        of(ctx).refresh_glist(m.cpu.a[0], m.cpu.a[1], static_cast<i16>(m.cpu.d[0] & 0xFFFF));
    }

    static void add_glist(void* ctx, Machine& m) {
        native::set_result(
            m, of(ctx).add_glist(m.cpu.a[0], m.cpu.a[1],
                                 static_cast<i16>(m.cpu.d[0] & 0xFFFF),
                                 static_cast<i16>(m.cpu.d[1] & 0xFFFF)));
    }

    static void remove_glist(void* ctx, Machine& m) {
        native::set_result(
            m, of(ctx).remove_glist(m.cpu.a[0], m.cpu.a[1],
                                    static_cast<i16>(m.cpu.d[0] & 0xFFFF)));
    }

    static void new_modify_prop(void* ctx, Machine& m) {
        of(ctx).new_modify_prop(m.cpu.a[0], m.cpu.a[1], m.cpu.d[0] & 0xFFFF,
                                m.cpu.d[1] & 0xFFFF, m.cpu.d[2] & 0xFFFF, m.cpu.d[3] & 0xFFFF,
                                m.cpu.d[4] & 0xFFFF, static_cast<i16>(m.cpu.d[5] & 0xFFFF));
    }
};

namespace {

struct IntuitionFunction {
    u32 lvo;
    m68k::NativeFn fn;
    const char* name;
};

constexpr IntuitionFunction kIntuitionFunctions[] = {
    {intuition_lvo::AddGadget, IntuitionNatives::add_gadget, "AddGadget"},
    {intuition_lvo::CloseScreen, IntuitionNatives::close_screen, "CloseScreen"},
    {intuition_lvo::CloseWindow, IntuitionNatives::close_window, "CloseWindow"},
    {intuition_lvo::ModifyIDCMP, IntuitionNatives::modify_idcmp, "ModifyIDCMP"},
    {intuition_lvo::ModifyProp, IntuitionNatives::modify_prop, "ModifyProp"},
    {intuition_lvo::OffGadget, IntuitionNatives::off_gadget, "OffGadget"},
    {intuition_lvo::OnGadget, IntuitionNatives::on_gadget, "OnGadget"},
    {intuition_lvo::OpenScreen, IntuitionNatives::open_screen, "OpenScreen"},
    {intuition_lvo::OpenWindow, IntuitionNatives::open_window, "OpenWindow"},
    {intuition_lvo::RefreshGadgets, IntuitionNatives::refresh_gadgets, "RefreshGadgets"},
    {intuition_lvo::RemoveGadget, IntuitionNatives::remove_gadget, "RemoveGadget"},
    {intuition_lvo::ScreenToBack, IntuitionNatives::screen_to_back, "ScreenToBack"},
    {intuition_lvo::ScreenToFront, IntuitionNatives::screen_to_front, "ScreenToFront"},
    {intuition_lvo::SetWindowTitles, IntuitionNatives::set_window_titles, "SetWindowTitles"},
    {intuition_lvo::WindowToBack, IntuitionNatives::window_to_back, "WindowToBack"},
    {intuition_lvo::WindowToFront, IntuitionNatives::window_to_front, "WindowToFront"},
    {intuition_lvo::LockIBase, IntuitionNatives::lock_i_base, "LockIBase"},
    {intuition_lvo::UnlockIBase, IntuitionNatives::unlock_i_base, "UnlockIBase"},
    {intuition_lvo::GetScreenData, IntuitionNatives::get_screen_data, "GetScreenData"},
    {intuition_lvo::RefreshGList, IntuitionNatives::refresh_glist, "RefreshGList"},
    {intuition_lvo::AddGList, IntuitionNatives::add_glist, "AddGList"},
    {intuition_lvo::RemoveGList, IntuitionNatives::remove_glist, "RemoveGList"},
    {intuition_lvo::NewModifyProp, IntuitionNatives::new_modify_prop, "NewModifyProp"},
    {intuition_lvo::OpenWindowTagList, IntuitionNatives::open_window_tag_list,
     "OpenWindowTagList"},
    {intuition_lvo::OpenScreenTagList, IntuitionNatives::open_screen_tag_list,
     "OpenScreenTagList"},
};

}

Rect Intuition::close_box_rect(i32 left, i32 top) {
    return Rect{left + 3, top + 2, left + 13, top + 12};
}

Rect Intuition::depth_box_rect(i32 left, i32 top, i32 width) {
    const i32 cx = left + width - 9;
    return Rect{cx - 5, top + 2, cx + 5, top + 12};
}

Rect Intuition::size_box_rect(i32 left, i32 top, i32 width, i32 height, u32 flags) {
    const i32 right = left + width - 1;
    const i32 bottom = top + height - 1;
    const i32 y0 = (flags & ndk::WFLG_SIZEBBOTTOM) != 0 ? bottom - 8 : bottom - 10;
    return Rect{right - 10, y0, right - 1, bottom - 1};
}

bool Intuition::init(Machine& m, Exec& exec, Graphics& graphics, DisplayState& display) {
    machine_ = &m;
    exec_ = &exec;
    graphics_ = &graphics;
    display_ = &display;
    clock_ = nullptr;
    base_ = 0;
    front_ = 0;
    default_screen_ = 0;
    reply_port_ = 0;
    active_window_ = 0;
    pressed_gadget_window_ = 0;
    pressed_gadget_ = GadgetKind::None;
    drag_ = Drag::None;
    drag_window_ = 0;
    drag_gadget_ = 0;
    mouse_x_ = 0;
    mouse_y_ = 0;
    dropped_messages_ = 0;
    batch_depth_ = 0;
    display.front_screen = 0;
    display.palette_dirty = false;
    for (u32& screen : screens_) {
        screen = 0;
    }
    for (u32& window : windows_) {
        window = 0;
    }
    for (u32& msg : messages_) {
        msg = 0;
    }
    for (u32& owner : message_owners_) {
        owner = 0;
    }
    for (ScreenRecord& rec : screen_records_) {
        rec = ScreenRecord{};
    }
    for (WindowRecord& rec : window_records_) {
        rec = WindowRecord{};
    }
    for (GadgetHookEntry& entry : gadget_hooks_) {
        entry = GadgetHookEntry{};
    }
    for (bool& seen : unhandled_gadget_types_) {
        seen = false;
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

void Intuition::flush_palette() {
    if (display_ == nullptr || !display_->palette_dirty || machine_ == nullptr ||
        batch_depth_ > 0) {
        return;
    }
    display_->palette_dirty = false;
    const u32 screen = display_->front_screen;
    if (screen == 0) {
        return;
    }
    const ScreenRecord* srec = find_screen_record(screen);
    if (srec != nullptr) {
        if (srec->depth >= 8) {
            compose(screen, Rect{0, 0, static_cast<i32>(srec->width) - 1,
                                 static_cast<i32>(srec->height) - 1});
        } else {
            refresh_screen(screen);
        }
        return;
    }
    Bus b(*machine_);
    const u32 width = b.r16(screen + ndk::sc_Width);
    const u32 height = b.r16(screen + ndk::sc_Height);
    if (b.ok() && width > 0 && height > 0) {
        refresh_full(*display_, *machine_, screen, width, height);
    }
}

void Intuition::end_batch() {
    if (batch_depth_ == 0) {
        return;
    }
    --batch_depth_;
    if (batch_depth_ != 0) {
        return;
    }
    if (display_ != nullptr && display_->palette_dirty) {
        ScreenRecord* front = find_screen_record(display_->front_screen);
        if (front != nullptr && (front->depth >= 8 ||
                                 (front->pending.x0 <= front->pending.x1 &&
                                  front->pending.y0 <= front->pending.y1))) {
            display_->palette_dirty = false;
            front->pending = Rect{0, 0, static_cast<i32>(front->width) - 1,
                                  static_cast<i32>(front->height) - 1};
        }
    }
    for (ScreenRecord& srec : screen_records_) {
        if (srec.addr != 0 && srec.pending.x0 <= srec.pending.x1 &&
            srec.pending.y0 <= srec.pending.y1) {
            const Rect r = srec.pending;
            srec.pending = Rect{0, 0, -1, -1};
            compose(srec.addr, r);
        }
    }
    flush_palette();
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
    if (srec->depth >= 8 && display_ != nullptr && srec->palette_gen != display_->palette_gen) {
        rebuild_shade_tables(*srec);
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
        if (has_shadow(srec->depth, wrec->flags) && wrec->width > 0 && wrec->height > 0) {
            const i32 wleft = wrec->left;
            const i32 wtop = wrec->top;
            const i32 wright = wleft + wrec->width - 1;
            const i32 wbottom = wtop + wrec->height - 1;
            struct ShadowBand {
                Rect rect;
                const u8* table;
            };
            const ShadowBand bands[4] = {
                {Rect{wright + 1, wtop + 4, wright + 2, wbottom}, srec->strong_shade},
                {Rect{wright + 3, wtop + 4, wright + 4, wbottom + 2}, srec->light_shade},
                {Rect{wleft + 4, wbottom + 1, wright + 2, wbottom + 2}, srec->strong_shade},
                {Rect{wleft + 4, wbottom + 3, wright + 4, wbottom + 4}, srec->light_shade},
            };
            for (const ShadowBand& band : bands) {
                const Rect clipped = intersect_rect(band.rect, r);
                if (clipped.x0 <= clipped.x1 && clipped.y0 <= clipped.y1) {
                    raster::remap_rect(b, dst, clipped, band.table, 256);
                }
            }
        }
        const i32 wx0 = r.x0 > wrec->left ? r.x0 : wrec->left;
        const i32 wy0 = r.y0 > wrec->top ? r.y0 : wrec->top;
        const i32 wx1 =
            r.x1 < wrec->left + wrec->width - 1 ? r.x1 : wrec->left + wrec->width - 1;
        const i32 wy1 =
            r.y1 < wrec->top + wrec->height - 1 ? r.y1 : wrec->top + wrec->height - 1;
        if (wx0 <= wx1 && wy0 <= wy1) {
            raster::Bitmap wbm{};
            if (raster::load_bitmap(b, wrec->bitmap, wbm)) {
                if (has_rounded_corners(srec->depth, wrec->flags)) {
                    raster::copy_rect_skip(b, wbm, wx0 - wrec->left, wy0 - wrec->top, dst, wx0,
                                           wy0, wx1 - wx0 + 1, wy1 - wy0 + 1, wrec->width,
                                           kTopCornerInsets, 3);
                } else {
                    raster::copy_rect(b, wbm, wx0 - wrec->left, wy0 - wrec->top, dst, wx0, wy0,
                                      wx1 - wx0 + 1, wy1 - wy0 + 1);
                }
            }
        }
    }
    if (b.ok() && display_ != nullptr && display_->front_screen == screen &&
        display_->sink.refresh != nullptr) {
        display_->sink.refresh(display_->sink.ctx, *machine_, screen, r);
    }
}

void Intuition::rebuild_shade_tables(ScreenRecord& srec) {
    if (machine_ == nullptr || srec.addr == 0 || srec.depth == 0 || srec.depth > 8) {
        return;
    }
    const u32 count = 1u << srec.depth;
    for (u32 i = 0; i < 256; ++i) {
        srec.strong_shade[i] = static_cast<u8>(i);
        srec.light_shade[i] = static_cast<u8>(i);
    }
    Bus b(*machine_);
    const u32 map = b.r32(srec.addr + ndk::sc_ViewPort + ndk::vp_ColorMap);
    if (map == 0 || !b.ok()) {
        return;
    }
    const u8 cm_type = static_cast<u8>(b.r8(map + ndk::cm_Type));
    const u32 cm_count = b.r16(map + ndk::cm_Count);
    const u32 table = b.r32(map + ndk::cm_ColorTable);
    const u32 low_table =
        cm_type >= ndk::COLORMAP_TYPE_V39 ? b.r32(map + ndk::cm_LowColorBits) : 0;
    if (!b.ok() || table == 0) {
        return;
    }
    const u32 n = cm_count < count ? cm_count : count;
    u8 red[256]{};
    u8 green[256]{};
    u8 blue[256]{};
    u32 lum[256]{};
    for (u32 i = 0; i < n; ++i) {
        const u32 hi = b.r16(table + i * 2);
        if (low_table != 0) {
            const u32 lo = b.r16(low_table + i * 2);
            red[i] = static_cast<u8>((((hi >> 8) & 0xF) << 4) | ((lo >> 8) & 0xF));
            green[i] = static_cast<u8>((((hi >> 4) & 0xF) << 4) | ((lo >> 4) & 0xF));
            blue[i] = static_cast<u8>(((hi & 0xF) << 4) | (lo & 0xF));
        } else {
            red[i] = static_cast<u8>(((hi >> 8) & 0xF) * 0x11);
            green[i] = static_cast<u8>(((hi >> 4) & 0xF) * 0x11);
            blue[i] = static_cast<u8>((hi & 0xF) * 0x11);
        }
        lum[i] = 299u * red[i] + 587u * green[i] + 114u * blue[i];
    }
    if (!b.ok()) {
        return;
    }
    for (u32 p = 0; p < n; ++p) {
        const i32 str_r = (static_cast<i32>(red[p]) * 70 + 50) / 100;
        const i32 str_g = (static_cast<i32>(green[p]) * 70 + 50) / 100;
        const i32 str_b = (static_cast<i32>(blue[p]) * 70 + 50) / 100;
        const i32 lgt_r = (static_cast<i32>(red[p]) * 85 + 50) / 100;
        const i32 lgt_g = (static_cast<i32>(green[p]) * 85 + 50) / 100;
        const i32 lgt_b = (static_cast<i32>(blue[p]) * 85 + 50) / 100;
        u32 best_strong = p;
        u32 best_strong_dist = 0xFFFFFFFFu;
        u32 best_light = p;
        u32 best_light_dist = 0xFFFFFFFFu;
        for (u32 q = 0; q < n; ++q) {
            if (q == p || lum[q] >= lum[p]) {
                continue;
            }
            const i32 drs = static_cast<i32>(red[q]) - str_r;
            const i32 dgs = static_cast<i32>(green[q]) - str_g;
            const i32 dbs = static_cast<i32>(blue[q]) - str_b;
            const u32 dist_s = static_cast<u32>(drs * drs + dgs * dgs + dbs * dbs);
            if (dist_s < best_strong_dist) {
                best_strong_dist = dist_s;
                best_strong = q;
            }
            const i32 drl = static_cast<i32>(red[q]) - lgt_r;
            const i32 dgl = static_cast<i32>(green[q]) - lgt_g;
            const i32 dbl = static_cast<i32>(blue[q]) - lgt_b;
            const u32 dist_l = static_cast<u32>(drl * drl + dgl * dgl + dbl * dbl);
            if (dist_l < best_light_dist) {
                best_light_dist = dist_l;
                best_light = q;
            }
        }
        srec.strong_shade[p] = static_cast<u8>(best_strong);
        srec.light_shade[p] = static_cast<u8>(best_light);
    }
    if (display_ != nullptr) {
        srec.palette_gen = display_->palette_gen;
    }
}

bool Intuition::shade_tables(u32 screen, const u8*& strong, const u8*& light) {
    ScreenRecord* srec = find_screen_record(screen);
    if (srec == nullptr || srec->depth < 8) {
        return false;
    }
    if (display_ != nullptr && srec->palette_gen != display_->palette_gen) {
        rebuild_shade_tables(*srec);
    }
    strong = srec->strong_shade;
    light = srec->light_shade;
    return true;
}

u32 Intuition::backdrop_rastport(u32 screen) const {
    for (const ScreenRecord& rec : screen_records_) {
        if (rec.addr == screen) {
            return rec.backdrop_rp;
        }
    }
    return 0;
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
    if (a.depth == 0 || a.depth > 8) {
        a.depth = 8;
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
    const u64 planes_bytes = static_cast<u64>(a.depth) * bytes_per_row * a.height;
    if (planes_bytes > 0xFFFFFFFF) {
        LogLine().text("intuition: screen too large").send();
        return 0;
    }
    const u32 bpr = static_cast<u32>(bytes_per_row);
    const u32 pbytes = static_cast<u32>(planes_bytes);
    const u32 color_count = 1u << a.depth;
    const u32 color_bytes = color_count * 2;

    u32 screen = 0;
    u32 planes = 0;
    u32 backdrop_bitmap = 0;
    u32 backdrop_planes = 0;
    u32 backdrop_rp = 0;
    u32 map = 0;
    u32 colors = 0;
    u32 low_colors = 0;
    u32 title = 0;
    u32 title_bytes = 0;
    auto fail = [&]() -> u32 {
        if (title != 0) {
            exec_->free(title, title_bytes);
        }
        if (low_colors != 0) {
            exec_->free(low_colors, color_bytes);
        }
        if (colors != 0) {
            exec_->free(colors, color_bytes);
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
    colors = exec_->alloc(color_bytes, ndk::MEMF_CHIP | ndk::MEMF_CLEAR);
    if (colors == 0) {
        return fail();
    }
    low_colors = exec_->alloc(color_bytes, ndk::MEMF_CHIP | ndk::MEMF_CLEAR);
    if (low_colors == 0) {
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
    b.w8(screen + ndk::sc_BarHeight, chrome::kBarHeight);
    b.w8(screen + ndk::sc_WBorTop, chrome::kBorderTop);
    b.w8(screen + ndk::sc_WBorLeft, chrome::kBorderLeft);
    b.w8(screen + ndk::sc_WBorRight, chrome::kBorderRight);
    b.w8(screen + ndk::sc_WBorBottom, chrome::kBorderBottom);
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
    b.w8(map + ndk::cm_Flags, 0);
    b.w8(map + ndk::cm_Type, ndk::COLORMAP_TYPE_V39);
    b.w16(map + ndk::cm_Count, color_count);
    b.w32(map + ndk::cm_ColorTable, colors);
    b.w32(map + ndk::cm_LowColorBits, low_colors);
    b.w32(map + ndk::cm_vp, vp);
    for (u32 i = 0; i < color_count; ++i) {
        const u32 rgb = default_palette_rgb24(i);
        const u32 r8 = (rgb >> 16) & 0xFF;
        const u32 g8 = (rgb >> 8) & 0xFF;
        const u32 b8 = rgb & 0xFF;
        const u32 hi = ((r8 >> 4) << 8) | ((g8 >> 4) << 4) | (b8 >> 4);
        const u32 lo = ((r8 & 0xF) << 8) | ((g8 & 0xF) << 4) | (b8 & 0xF);
        b.w16(colors + i * 2, hi);
        b.w16(low_colors + i * 2, lo);
    }

    for (u32 i = kMaxScreens - 1; i > 0; --i) {
        screens_[i] = screens_[i - 1];
    }
    screens_[0] = screen;
    front_ = screen;
    display_->front_screen = screen;
    display_->palette_dirty = false;
    write_links();
    rec->addr = screen;
    rec->owner = exec_->this_task();
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
    rec->colors_bytes = color_bytes;
    rec->low_colors = low_colors;
    rec->low_colors_bytes = color_bytes;
    rec->title = title;
    rec->title_bytes = title_bytes;
    rec->width = a.width;
    rec->height = a.height;
    rec->depth = a.depth;
    rec->first_window = 0;
    rec->pending = Rect{0, 0, -1, -1};
    if (a.depth >= 8) {
        rebuild_shade_tables(*rec);
    }

    const i32 right = static_cast<i32>(a.width) - 1;
    const i32 bottom = static_cast<i32>(a.height) - 1;
    begin_batch();
    graphics_->set_pens(backdrop_rp, 0, 0, ndk::JAM2);
    graphics_->rect_fill(backdrop_rp, 0, 0, right, bottom);
    chrome::draw_screen_bar(*graphics_, b, backdrop_rp, a.width, title);
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
    if (rec->low_colors != 0) {
        exec_->free(rec->low_colors, rec->low_colors_bytes);
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
    if (default_screen_ == screen) {
        default_screen_ = 0;
    }

    if (was_front) {
        front_ = screens_[0];
        display_->front_screen = front_;
        display_->palette_dirty = false;
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
    display_->palette_dirty = false;
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
    display_->palette_dirty = false;
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
    for (u32 i = 0; i < kMaxMessages; ++i) {
        if (messages_[i] == addr) {
            messages_[i] = 0;
            message_owners_[i] = 0;
            Bus b(*machine_);
            const u32 idcmp_class = b.r32(addr + ndk::im_Class);
            const u32 window = b.r32(addr + ndk::im_IDCMPWindow);
            if (b.ok() && idcmp_class == ndk::IDCMP_NEWSIZE && window != 0) {
                WindowRecord* rec = find_window_record(window);
                if (rec != nullptr) {
                    rec->newsize_pending = false;
                    // A size reached while this NEWSIZE waited for its
                    // reply is reported now that the window takes one again.
                    if (drag_ == Drag::None || drag_window_ != window) {
                        post_newsize(*rec);
                    }
                }
            }
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

bool Intuition::post_intuimessage(u32 window, u32 idcmp_class, u16 code, u32 iaddress) {
    if (window == 0 || find_window(window) >= kMaxWindows) {
        return false;
    }
    const WindowRecord* rec = find_window_record(window);
    if (rec == nullptr || rec->screen != front_ || rec->user_port == 0) {
        return false;
    }
    Bus b(*machine_);
    const u32 idcmp = b.r32(window + ndk::wd_IDCMPFlags);
    if (!b.ok() || (idcmp & idcmp_class) == 0) {
        return false;
    }
    u32 slot = kMaxMessages;
    for (u32 i = 0; i < kMaxMessages; ++i) {
        if (messages_[i] == 0) {
            slot = i;
            break;
        }
    }
    if (slot == kMaxMessages) {
        ++dropped_messages_;
        return false;
    }
    const u32 msg = exec_->alloc(ndk::IntuiMessage_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (msg == 0) {
        return false;
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
        return false;
    }
    messages_[slot] = msg;
    message_owners_[slot] = rec->owner;
    return true;
}

void Intuition::redraw_window_bar(u32 window) {
    if (window == 0 || machine_ == nullptr || graphics_ == nullptr ||
        find_window(window) >= kMaxWindows) {
        return;
    }
    const WindowRecord* rec = find_window_record(window);
    if (rec == nullptr) {
        return;
    }
    Bus b(*machine_);
    WindowView w = load_window(b, window);
    if (!w.ok || (w.flags & ndk::WFLG_BORDERLESS) != 0) {
        return;
    }
    w.rp = rec->rp;
    chrome::draw_window_bar(*graphics_, b, rec->rp, to_chrome(w, active_window_ == window));
    const i32 bar_bottom = static_cast<i32>(chrome::kBarHeight);
    const i32 y1 = rec->height - 1 < bar_bottom ? rec->height - 1 : bar_bottom;
    if (rec->width > 0 && y1 >= 0) {
        compose(rec->screen,
                Rect{rec->left, rec->top, rec->left + rec->width - 1, rec->top + y1});
    }
}

void Intuition::activate_window(u32 window) {
    if (window == active_window_) {
        return;
    }
    const u32 prev = active_window_;
    active_window_ = window;
    begin_batch();
    if (prev != 0) {
        post_intuimessage(prev, ndk::IDCMP_INACTIVEWINDOW, 0);
        redraw_window_bar(prev);
    }
    if (window != 0) {
        post_intuimessage(window, ndk::IDCMP_ACTIVEWINDOW, 0);
        redraw_window_bar(window);
    }
    end_batch();
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
    const bool borderless = (a.flags & ndk::WFLG_BORDERLESS) != 0;
    const bool has_size = !borderless && (a.flags & ndk::WFLG_SIZEGADGET) != 0;
    const bool size_bottom = has_size && (a.flags & ndk::WFLG_SIZEBBOTTOM) != 0;
    const bool size_right = has_size && (!size_bottom || (a.flags & ndk::WFLG_SIZEBRIGHT) != 0);
    const u32 border_left = borderless ? 1 : chrome::kBorderLeft;
    const u32 border_top = borderless ? 1 : chrome::kBorderTop;
    const u32 border_right =
        borderless ? 1 : (size_right ? chrome::kSizeBorderRight : chrome::kBorderRight);
    const u32 border_bottom =
        borderless ? 1 : (size_bottom ? chrome::kSizeBorderBottom : chrome::kBorderBottom);

    const i32 floor_w = 40;
    const i32 floor_h = static_cast<i32>(border_top) + 10;
    i32 min_w = a.min_width == 0 ? width
                : a.min_width > static_cast<u32>(screen_w) ? screen_w
                : static_cast<i32>(a.min_width);
    i32 min_h = a.min_height == 0 ? height
                : a.min_height > static_cast<u32>(screen_h) ? screen_h
                : static_cast<i32>(a.min_height);
    i32 lim_w = a.max_width == 0 ? width
                : a.max_width >= 0xFFFF || a.max_width > static_cast<u32>(screen_w)
                    ? screen_w
                    : static_cast<i32>(a.max_width);
    i32 lim_h = a.max_height == 0 ? height
                : a.max_height >= 0xFFFF || a.max_height > static_cast<u32>(screen_h)
                    ? screen_h
                    : static_cast<i32>(a.max_height);
    if (min_w < floor_w) {
        min_w = floor_w;
    }
    if (min_w > screen_w) {
        min_w = screen_w;
    }
    if (lim_w < floor_w) {
        lim_w = floor_w;
    }
    if (lim_w > screen_w) {
        lim_w = screen_w;
    }
    if (min_h < floor_h) {
        min_h = floor_h;
    }
    if (min_h > screen_h) {
        min_h = screen_h;
    }
    if (lim_h < floor_h) {
        lim_h = floor_h;
    }
    if (lim_h > screen_h) {
        lim_h = screen_h;
    }
    if (min_w > width) {
        min_w = width;
    }
    if (lim_w < width) {
        lim_w = width;
    }
    if (min_h > height) {
        min_h = height;
    }
    if (lim_h < height) {
        lim_h = height;
    }
    const i32 max_w = lim_w;
    const i32 max_h = lim_h;

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
    b.w16(window + ndk::wd_MinWidth, static_cast<u32>(min_w));
    b.w16(window + ndk::wd_MinHeight, static_cast<u32>(min_h));
    b.w16(window + ndk::wd_MaxWidth, static_cast<u32>(lim_w));
    b.w16(window + ndk::wd_MaxHeight, static_cast<u32>(lim_h));
    b.w32(window + ndk::wd_Flags, a.flags);
    b.w32(window + ndk::wd_Title, title);
    b.w32(window + ndk::wd_WScreen, screen);
    b.w32(window + ndk::wd_RPort, rp);
    b.w8(window + ndk::wd_BorderLeft, border_left);
    b.w8(window + ndk::wd_BorderTop, border_top);
    b.w8(window + ndk::wd_BorderRight, border_right);
    b.w8(window + ndk::wd_BorderBottom, border_bottom);
    b.w32(window + ndk::wd_IDCMPFlags, a.idcmp);
    b.w32(window + ndk::wd_UserPort, user_port);
    b.w8(window + ndk::wd_DetailPen, a.detail);
    b.w8(window + ndk::wd_BlockPen, a.block);
    b.w32(window + ndk::wd_WLayer, layer);
    b.w32(window + ndk::wd_IFont, graphics_->topaz_font());
    b.w32(window + ndk::wd_FirstGadget, a.gadgets);
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
    rec->owner = exec_->this_task();
    rec->window_bytes = ndk::Window_SIZEOF;
    rec->screen = screen;
    rec->next_window = 0;
    rec->left = left;
    rec->top = top;
    rec->width = width;
    rec->height = height;
    rec->flags = a.flags;
    rec->max_w = max_w;
    rec->max_h = max_h;
    rec->min_w = min_w;
    rec->min_h = min_h;
    rec->lim_w = lim_w;
    rec->lim_h = lim_h;
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
    rec->newsize_pending = false;
    rec->newsize_w = width;
    rec->newsize_h = height;
    const u32 insert_after =
        (a.flags & ndk::WFLG_BACKDROP) != 0 ? last_normal_window(*srec) : 0;
    insert_window_after(*srec, *rec, insert_after);
    write_window_links(screen);
    for (u32& slot : windows_) {
        if (slot == 0) {
            slot = window;
            break;
        }
    }

    begin_batch();
    if ((a.flags & ndk::WFLG_ACTIVATE) != 0) {
        const u32 prev = active_window_;
        active_window_ = window;
        if (prev != 0) {
            post_intuimessage(prev, ndk::IDCMP_INACTIVEWINDOW, 0);
            redraw_window_bar(prev);
        }
        post_intuimessage(window, ndk::IDCMP_ACTIVEWINDOW, 0);
    }
    WindowView w = load_window(b, window);
    w.rp = rp;
    chrome::draw_window_chrome(*graphics_, b, rp, to_chrome(w, active_window_ == window));
    if (a.gadgets != 0) {
        draw_gadget_list(b, window, a.gadgets, kMaxGadgets);
    }
    compose(screen, window_damage_rect(srec->depth, left, top, width, height, a.flags));
    end_batch();
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
    if (drag_window_ == window) {
        drag_window_ = 0;
        drag_gadget_ = 0;
        drag_ = Drag::None;
    }
    clear_window_gadget_hooks(window);
    Bus(*machine_).w32(window + ndk::wd_FirstGadget, 0);
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
    const u32 flags = rec->flags;

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
        compose(screen, window_damage_rect(srec->depth, left, top, width, height, flags));
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
            chrome::draw_screen_bar(*graphics_, b, srec->backdrop_rp, srec->width, srec->title);
            end_batch();
        }
    }
    if (redraw) {
        WindowView w = load_window(b, window);
        w.rp = rec->rp;
        if ((w.flags & ndk::WFLG_BORDERLESS) == 0) {
            begin_batch();
            chrome::draw_window_bar(*graphics_, b, rec->rp,
                                    to_chrome(w, active_window_ == window));
            end_batch();
        }
    }
}

u32 Intuition::last_normal_window(const ScreenRecord& srec, u32 skip_window) const {
    u32 last_normal = 0;
    u32 cur = srec.first_window;
    for (u32 step = 0; step < kMaxWindows && cur != 0; ++step) {
        const WindowRecord* wrec = find_window_record(cur);
        if (wrec == nullptr) {
            break;
        }
        if (cur != skip_window && (wrec->flags & ndk::WFLG_BACKDROP) == 0) {
            last_normal = cur;
        }
        cur = wrec->next_window;
    }
    return last_normal;
}

u32 Intuition::last_window(const ScreenRecord& srec, u32 skip_window) const {
    u32 last = 0;
    u32 cur = srec.first_window;
    for (u32 step = 0; step < kMaxWindows && cur != 0; ++step) {
        const WindowRecord* wrec = find_window_record(cur);
        if (wrec == nullptr) {
            break;
        }
        if (cur != skip_window) {
            last = cur;
        }
        cur = wrec->next_window;
    }
    return last;
}

void Intuition::insert_window_after(ScreenRecord& srec, WindowRecord& rec, u32 after) {
    if (after == 0) {
        rec.next_window = srec.first_window;
        srec.first_window = rec.addr;
        return;
    }
    WindowRecord* arec = find_window_record(after);
    if (arec == nullptr) {
        rec.next_window = srec.first_window;
        srec.first_window = rec.addr;
        return;
    }
    rec.next_window = arec->next_window;
    arec->next_window = rec.addr;
}

bool Intuition::move_window_after(ScreenRecord& srec, WindowRecord& rec, u32 after) {
    u32 cur_prev = 0;
    if (srec.first_window != rec.addr) {
        u32 p = srec.first_window;
        for (u32 step = 0; step < kMaxWindows && p != 0; ++step) {
            const WindowRecord* prec = find_window_record(p);
            if (prec == nullptr) {
                return false;
            }
            if (prec->next_window == rec.addr) {
                cur_prev = p;
                break;
            }
            p = prec->next_window;
        }
        if (cur_prev == 0) {
            return false;
        }
    }
    if (cur_prev == after) {
        return false;
    }
    if (cur_prev == 0) {
        srec.first_window = rec.next_window;
    } else {
        WindowRecord* prec = find_window_record(cur_prev);
        if (prec == nullptr) {
            return false;
        }
        prec->next_window = rec.next_window;
    }
    insert_window_after(srec, rec, after);
    return true;
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
    if (srec == nullptr) {
        return;
    }
    const bool backdrop = (rec->flags & ndk::WFLG_BACKDROP) != 0;
    const u32 after = backdrop ? last_normal_window(*srec, window) : 0;
    if (!move_window_after(*srec, *rec, after)) {
        return;
    }
    write_window_links(rec->screen);
    compose(rec->screen, window_damage_rect(srec->depth, rec->left, rec->top, rec->width,
                                            rec->height, rec->flags));
}

void Intuition::window_to_back(u32 window) {
    const u32 index = find_window(window);
    if (index >= kMaxWindows) {
        return;
    }
    WindowRecord* rec = find_window_record(window);
    if (rec == nullptr) {
        return;
    }
    ScreenRecord* srec = find_screen_record(rec->screen);
    if (srec == nullptr || srec->first_window == 0) {
        return;
    }
    const bool backdrop = (rec->flags & ndk::WFLG_BACKDROP) != 0;
    const u32 after =
        backdrop ? last_window(*srec, window) : last_normal_window(*srec, window);
    if (!move_window_after(*srec, *rec, after)) {
        return;
    }
    write_window_links(rec->screen);
    compose(rec->screen, window_damage_rect(srec->depth, rec->left, rec->top, rec->width,
                                            rec->height, rec->flags));
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
    begin_batch();
    route_pointer(x, y, buttons, pressed, released, moved);
    end_batch();
}

void Intuition::route_pointer(i32 x, i32 y, u8 buttons, u8 pressed, u8 released, bool moved) {
    (void)buttons;
    mouse_x_ = x;
    mouse_y_ = y;
    if (front_ == 0) {
        return;
    }
    Bus b(*machine_);
    // A drag owns the pointer: the window follows it and gets no mouse
    // messages until the left button is released.
    const bool dragging = drag_ != Drag::None;
    if (dragging && moved) {
        drag_to(x, y);
    }
    const u32 hit = window_at(x, y);
    bool left_press_consumed = false;
    bool left_release_consumed = false;
    if (dragging && (released & kMouseLeft) != 0) {
        end_drag();
        left_release_consumed = true;
    }

    if ((pressed & kMouseLeft) != 0) {
        if (hit != 0) {
            activate_window(hit);
            const WindowView w = load_window(b, hit);
            if (w.ok) {
                const bool bordered = (w.flags & ndk::WFLG_BORDERLESS) == 0;
                if (bordered && (w.flags & ndk::WFLG_CLOSEGADGET) != 0 &&
                    in_rect(close_box_rect(w.left, w.top), x, y)) {
                    pressed_gadget_window_ = hit;
                    pressed_gadget_ = GadgetKind::Close;
                    left_press_consumed = true;
                } else if (bordered && (w.flags & ndk::WFLG_DEPTHGADGET) != 0 &&
                           in_rect(depth_box_rect(w.left, w.top, w.width), x, y)) {
                    pressed_gadget_window_ = hit;
                    pressed_gadget_ = GadgetKind::Depth;
                    left_press_consumed = true;
                } else if (!dragging && hit_prop_gadget(b, hit, x, y)) {
                    left_press_consumed = true;
                } else if (bordered && !dragging && start_drag(hit, x, y)) {
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
        if (gad_window != 0 && hit == gad_window && gad_kind != GadgetKind::Prop) {
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

    if (moved && !dragging && active_window_ != 0) {
        const WindowView w = load_window(b, active_window_);
        if (w.ok && (w.flags & ndk::WFLG_REPORTMOUSE) != 0) {
            post_intuimessage(active_window_, ndk::IDCMP_MOUSEMOVE, 0);
        }
    }
}

void Intuition::move_window_to(u32 window, i32 left, i32 top) {
    if (window == 0) {
        return;
    }
    WindowRecord* rec = find_window_record(window);
    if (rec == nullptr || find_window(window) >= kMaxWindows) {
        return;
    }
    const ScreenRecord* srec = find_screen_record(rec->screen);
    if (srec == nullptr) {
        return;
    }
    const i32 max_left = static_cast<i32>(srec->width) - rec->width;
    const i32 max_top = static_cast<i32>(srec->height) - rec->height;
    left = left > max_left ? max_left : left;
    top = top > max_top ? max_top : top;
    left = left < 0 ? 0 : left;
    top = top < 0 ? 0 : top;
    if (left == rec->left && top == rec->top) {
        return;
    }
    const Rect old = window_damage_rect(srec->depth, rec->left, rec->top, rec->width, rec->height,
                                          rec->flags);
    rec->left = left;
    rec->top = top;
    Bus b(*machine_);
    b.w16(window + ndk::wd_LeftEdge, static_cast<u32>(left));
    b.w16(window + ndk::wd_TopEdge, static_cast<u32>(top));
    b.w16(rec->layer + ndk::ly_MinX, static_cast<u32>(left));
    b.w16(rec->layer + ndk::ly_MinY, static_cast<u32>(top));
    b.w16(rec->layer + ndk::ly_MaxX, static_cast<u32>(left + rec->width - 1));
    b.w16(rec->layer + ndk::ly_MaxY, static_cast<u32>(top + rec->height - 1));
    begin_batch();
    compose(rec->screen, old);
    compose(rec->screen,
            window_damage_rect(srec->depth, left, top, rec->width, rec->height, rec->flags));
    end_batch();
}

void Intuition::size_window_to(u32 window, i32 width, i32 height) {
    if (window == 0) {
        return;
    }
    WindowRecord* rec = find_window_record(window);
    if (rec == nullptr || find_window(window) >= kMaxWindows) {
        return;
    }
    const ScreenRecord* srec = find_screen_record(rec->screen);
    if (srec == nullptr) {
        return;
    }
    const i32 room_w = static_cast<i32>(srec->width) - rec->left;
    const i32 room_h = static_cast<i32>(srec->height) - rec->top;
    const i32 max_w = rec->lim_w < room_w ? rec->lim_w : room_w;
    const i32 max_h = rec->lim_h < room_h ? rec->lim_h : room_h;
    width = width > max_w ? max_w : width;
    height = height > max_h ? max_h : height;
    width = width < rec->min_w ? rec->min_w : width;
    height = height < rec->min_h ? rec->min_h : height;
    if (width == rec->width && height == rec->height) {
        return;
    }
    Bus b(*machine_);
    WindowView w = load_window(b, window);
    if (!w.ok) {
        return;
    }
    const i32 ix0 = static_cast<i32>(w.border_left);
    const i32 iy0 = static_cast<i32>(w.border_top);
    const i32 old_ix1 = rec->width - static_cast<i32>(w.border_right) - 1;
    const i32 old_iy1 = rec->height - static_cast<i32>(w.border_bottom) - 1;
    const i32 new_ix1 = width - static_cast<i32>(w.border_right) - 1;
    const i32 new_iy1 = height - static_cast<i32>(w.border_bottom) - 1;
    const Rect keep{ix0, iy0, old_ix1 < new_ix1 ? old_ix1 : new_ix1,
                    old_iy1 < new_iy1 ? old_iy1 : new_iy1};
    const Rect old = window_damage_rect(srec->depth, rec->left, rec->top, rec->width, rec->height,
                                          rec->flags);

    begin_batch();
    rec->width = width;
    rec->height = height;
    b.w16(window + ndk::wd_Width, static_cast<u32>(width));
    b.w16(window + ndk::wd_Height, static_cast<u32>(height));
    b.w16(rec->layer + ndk::ly_MaxX, static_cast<u32>(rec->left + width - 1));
    b.w16(rec->layer + ndk::ly_MaxY, static_cast<u32>(rec->top + height - 1));
    w.width = width;
    w.height = height;
    w.rp = rec->rp;
    chrome::draw_window_chrome(*graphics_, b, rec->rp, to_chrome(w, active_window_ == window), &keep);
    if (w.first_gadget != 0) {
        draw_gadget_list(b, window, w.first_gadget, kMaxGadgets);
    }
    if (rec->size_hook != nullptr) {
        rec->size_hook(rec->size_ctx, window);
    }
    // The hook may close the window.
    rec = find_window_record(window);
    if (rec != nullptr && find_window(window) < kMaxWindows) {
        post_newsize(*rec);
        compose(rec->screen, old);
        compose(rec->screen,
                window_damage_rect(srec->depth, rec->left, rec->top, width, height, rec->flags));
    }
    end_batch();
}

void Intuition::post_newsize(WindowRecord& rec) {
    if (rec.newsize_pending ||
        (rec.newsize_w == rec.width && rec.newsize_h == rec.height)) {
        return;
    }
    if (post_intuimessage(rec.addr, ndk::IDCMP_NEWSIZE, 0)) {
        rec.newsize_pending = true;
        rec.newsize_w = rec.width;
        rec.newsize_h = rec.height;
    }
}

bool Intuition::start_drag(u32 window, i32 x, i32 y) {
    if (window == 0) {
        return false;
    }
    const WindowRecord* rec = find_window_record(window);
    if (rec == nullptr) {
        return false;
    }
    Bus b(*machine_);
    const u32 flags = b.r32(window + ndk::wd_Flags);
    if (!b.ok() || (flags & ndk::WFLG_BORDERLESS) != 0) {
        return false;
    }
    const i32 right = rec->left + rec->width - 1;
    const i32 bottom = rec->top + rec->height - 1;
    if ((flags & ndk::WFLG_SIZEGADGET) != 0 &&
        in_rect(size_box_rect(rec->left, rec->top, rec->width, rec->height, flags), x, y)) {
        drag_ = Drag::Size;
        grab_x_ = right - x;
        grab_y_ = bottom - y;
    } else if ((flags & ndk::WFLG_DRAGBAR) != 0 && x >= rec->left && x <= right &&
               y >= rec->top && y <= rec->top + static_cast<i32>(chrome::kBarHeight)) {
        drag_ = Drag::Move;
        grab_x_ = x - rec->left;
        grab_y_ = y - rec->top;
    } else {
        return false;
    }
    drag_window_ = window;
    return true;
}

void Intuition::drag_to(i32 x, i32 y) {
    if (drag_ == Drag::Prop) {
        drag_prop(x, y);
        return;
    }
    const WindowRecord* rec = drag_window_ != 0 ? find_window_record(drag_window_) : nullptr;
    if (rec == nullptr) {
        drag_ = Drag::None;
        drag_window_ = 0;
        drag_gadget_ = 0;
        return;
    }
    if (drag_ == Drag::Move) {
        move_window_to(drag_window_, x - grab_x_, y - grab_y_);
    } else if (drag_ == Drag::Size) {
        size_window_to(drag_window_, x + grab_x_ - rec->left + 1, y + grab_y_ - rec->top + 1);
    }
}

void Intuition::end_drag() {
    if (drag_ == Drag::Prop) {
        end_prop_drag();
        return;
    }
    const u32 window = drag_window_;
    const Drag kind = drag_;
    drag_ = Drag::None;
    drag_window_ = 0;
    drag_gadget_ = 0;
    if (window == 0) {
        return;
    }
    WindowRecord* rec = find_window_record(window);
    if (rec == nullptr || find_window(window) >= kMaxWindows) {
        return;
    }
    if (kind == Drag::Size) {
        post_newsize(*rec);
    }
    post_intuimessage(window, ndk::IDCMP_CHANGEWINDOW, 0);
}

void Intuition::reclaim_owned_by(u32 task, u32 keep_window, u32 keep_screen) {
    reclaim_owned_by(task, &keep_window, keep_window != 0 ? 1u : 0u, keep_screen);
}

void Intuition::reclaim_owned_by(u32 task, const u32* keep_windows, u32 count,
                                 u32 keep_screen) {
    u32 win_to_close[kMaxWindows]{};
    u32 win_count = 0;
    for (u32 window : windows_) {
        if (window == 0) {
            continue;
        }
        const WindowRecord* rec = find_window_record(window);
        if (rec == nullptr || rec->owner != task) {
            continue;
        }
        bool keep = false;
        for (u32 i = 0; i < count && !keep; ++i) {
            keep = keep_windows[i] == window;
        }
        if (!keep) {
            win_to_close[win_count++] = window;
        }
    }
    for (u32 i = 0; i < win_count; ++i) {
        close_window(win_to_close[i]);
    }

    u32 scr_to_close[kMaxScreens]{};
    u32 scr_count = 0;
    for (u32 screen : screens_) {
        if (screen == 0 || screen == keep_screen) {
            continue;
        }
        const ScreenRecord* rec = find_screen_record(screen);
        if (rec != nullptr && rec->owner == task) {
            scr_to_close[scr_count++] = screen;
        }
    }
    for (u32 i = 0; i < scr_count; ++i) {
        close_screen(scr_to_close[i]);
    }
    if (keep_screen != 0 && front_ != keep_screen && find_screen(keep_screen) < kMaxScreens) {
        screen_to_front(keep_screen);
    }

    for (u32 i = 0; i < count; ++i) {
        if (keep_windows[i] != 0) {
            if (const WindowRecord* krec = find_window_record(keep_windows[i]);
                krec != nullptr && krec->owner == task && krec->user_port != 0) {
                remove_window_messages(krec->user_port, keep_windows[i]);
            }
        }
    }
    for (u32 i = 0; i < kMaxMessages; ++i) {
        if (messages_[i] != 0 && message_owners_[i] == task) {
            const u32 msg = messages_[i];
            messages_[i] = 0;
            message_owners_[i] = 0;
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

u32 Intuition::window_owner(u32 window) const {
    if (window == 0) {
        return 0;
    }
    const WindowRecord* rec = find_window_record(window);
    return rec != nullptr ? rec->owner : 0;
}

bool Intuition::window_open(u32 window) const {
    return find_window(window) < kMaxWindows;
}

void Intuition::log_unhandled_gadget_type(u32 gtype) {
    const u32 idx = gtype & ndk::GTYP_GTYPEMASK;
    if (!unhandled_gadget_types_[idx]) {
        unhandled_gadget_types_[idx] = true;
        LogLine().text("intuition: gadget type ").dec(idx).text(" not handled").send();
    }
}

u32 Intuition::find_gadget_window(u32 gadget) const {
    if (gadget == 0 || machine_ == nullptr) {
        return 0;
    }
    const u32 target = native::guest_address(*machine_, gadget);
    Bus b(*machine_);
    for (u32 window : windows_) {
        if (window == 0) {
            continue;
        }
        u32 cur = b.r32(window + ndk::wd_FirstGadget);
        for (u32 step = 0; step < kMaxGadgets && cur != 0; ++step) {
            if (native::guest_address(*machine_, cur) == target) {
                return window;
            }
            cur = b.r32(cur + ndk::gg_NextGadget);
            if (!b.ok()) {
                break;
            }
        }
    }
    return 0;
}

void Intuition::clear_gadget_hook(u32 gadget) {
    if (gadget == 0 || machine_ == nullptr) {
        return;
    }
    const u32 target = native::guest_address(*machine_, gadget);
    for (GadgetHookEntry& entry : gadget_hooks_) {
        if (entry.gadget != 0 && native::guest_address(*machine_, entry.gadget) == target) {
            entry = GadgetHookEntry{};
        }
    }
}

void Intuition::clear_window_gadget_hooks(u32 window) {
    if (window == 0 || machine_ == nullptr) {
        return;
    }
    Bus b(*machine_);
    u32 gadget = b.r32(window + ndk::wd_FirstGadget);
    for (u32 step = 0; step < kMaxGadgets && gadget != 0; ++step) {
        clear_gadget_hook(gadget);
        gadget = b.r32(gadget + ndk::gg_NextGadget);
        if (!b.ok()) {
            break;
        }
    }
    for (GadgetHookEntry& entry : gadget_hooks_) {
        if (entry.window == window) {
            entry = GadgetHookEntry{};
        }
    }
}

void Intuition::call_gadget_hook(u32 gadget, u32 vert_pot) {
    if (gadget == 0 || machine_ == nullptr) {
        return;
    }
    const u32 target = native::guest_address(*machine_, gadget);
    for (const GadgetHookEntry& entry : gadget_hooks_) {
        if (entry.gadget != 0 && native::guest_address(*machine_, entry.gadget) == target &&
            entry.hook != nullptr) {
            entry.hook(entry.ctx, gadget, vert_pot);
            return;
        }
    }
}

void Intuition::set_gadget_hook(u32 gadget, GadgetHook hook, void* ctx) {
    if (gadget == 0 || machine_ == nullptr) {
        return;
    }
    const u32 target = native::guest_address(*machine_, gadget);
    GadgetHookEntry* slot = nullptr;
    for (GadgetHookEntry& entry : gadget_hooks_) {
        if (entry.gadget != 0 && native::guest_address(*machine_, entry.gadget) == target) {
            slot = &entry;
            break;
        }
    }
    if (hook == nullptr) {
        if (slot != nullptr) {
            *slot = GadgetHookEntry{};
        }
        return;
    }
    if (slot == nullptr) {
        for (GadgetHookEntry& entry : gadget_hooks_) {
            if (entry.gadget == 0) {
                slot = &entry;
                break;
            }
        }
    }
    if (slot == nullptr) {
        return;
    }
    slot->gadget = gadget;
    slot->window = find_gadget_window(gadget);
    slot->hook = hook;
    slot->ctx = ctx;
}

void Intuition::draw_gadget_list(Bus& b, u32 window, u32 first, u32 max_count) {
    if (window == 0 || first == 0 || max_count == 0) {
        return;
    }
    const WindowRecord* rec = find_window_record(window);
    if (rec == nullptr || find_window(window) >= kMaxWindows) {
        return;
    }
    WindowView w = load_window(b, window);
    if (!w.ok) {
        return;
    }
    w.rp = rec->rp;
    const u32 limit = max_count < kMaxGadgets ? max_count : kMaxGadgets;
    u32 gadget = first;
    for (u32 step = 0; step < limit && gadget != 0; ++step) {
        const u32 gtype = b.r16(gadget + ndk::gg_GadgetType) & ndk::GTYP_GTYPEMASK;
        const u32 next = b.r32(gadget + ndk::gg_NextGadget);
        if (!b.ok()) {
            return;
        }
        if (gtype == ndk::GTYP_PROPGADGET) {
            draw_prop_gadget(*graphics_, b, w, gadget);
        } else {
            log_unhandled_gadget_type(gtype);
        }
        gadget = next;
    }
}

u32 Intuition::add_glist(u32 window, u32 gadget, i32 position, i32 count) {
    if (window == 0 || gadget == 0 || count == 0 || !window_open(window)) {
        return 0xFFFFFFFF;
    }
    Bus b(*machine_);
    const u32 max_add = count < 0 ? kMaxGadgets : static_cast<u32>(count);
    const u32 add_limit = max_add < kMaxGadgets ? max_add : kMaxGadgets;
    u32 last = gadget;
    u32 added = 1;
    while (added < add_limit) {
        const u32 next = b.r32(last + ndk::gg_NextGadget);
        if (!b.ok()) {
            return 0xFFFFFFFF;
        }
        if (next == 0) {
            break;
        }
        last = next;
        ++added;
    }

    u32 cur = b.r32(window + ndk::wd_FirstGadget);
    if (!b.ok()) {
        return 0xFFFFFFFF;
    }
    u32 prev = 0;
    u32 pos = 0;
    const u32 target = position < 0 ? kMaxGadgets : static_cast<u32>(position);
    while (cur != 0 && pos < target && pos < kMaxGadgets) {
        prev = cur;
        cur = b.r32(cur + ndk::gg_NextGadget);
        if (!b.ok()) {
            return 0xFFFFFFFF;
        }
        ++pos;
    }

    b.w32(last + ndk::gg_NextGadget, cur);
    if (prev == 0) {
        b.w32(window + ndk::wd_FirstGadget, gadget);
    } else {
        b.w32(prev + ndk::gg_NextGadget, gadget);
    }
    if (!b.ok()) {
        return 0xFFFFFFFF;
    }
    u32 linked = gadget;
    for (u32 step = 0; step < added && linked != 0; ++step) {
        const u32 masked = native::guest_address(*machine_, linked);
        for (GadgetHookEntry& entry : gadget_hooks_) {
            if (entry.gadget != 0 && native::guest_address(*machine_, entry.gadget) == masked) {
                entry.window = window;
            }
        }
        linked = b.r32(linked + ndk::gg_NextGadget);
        if (!b.ok()) {
            break;
        }
    }
    return pos;
}

u32 Intuition::remove_glist(u32 window, u32 gadget, i32 count) {
    if (window == 0 || gadget == 0 || count == 0 || !window_open(window)) {
        return 0xFFFFFFFF;
    }
    Bus b(*machine_);
    const u32 target = native::guest_address(*machine_, gadget);
    const u32 max_rem = count < 0 ? kMaxGadgets : static_cast<u32>(count);
    const u32 rem_limit = max_rem < kMaxGadgets ? max_rem : kMaxGadgets;
    u32 prev = 0;
    u32 cur = b.r32(window + ndk::wd_FirstGadget);
    if (!b.ok()) {
        return 0xFFFFFFFF;
    }
    for (u32 pos = 0; pos < kMaxGadgets && cur != 0; ++pos) {
        if (native::guest_address(*machine_, cur) == target) {
            u32 tail = cur;
            u32 after = b.r32(tail + ndk::gg_NextGadget);
            if (!b.ok()) {
                return 0xFFFFFFFF;
            }
            u32 removed = 1;
            while (removed < rem_limit && after != 0) {
                tail = after;
                after = b.r32(tail + ndk::gg_NextGadget);
                if (!b.ok()) {
                    return 0xFFFFFFFF;
                }
                ++removed;
            }
            if (prev == 0) {
                b.w32(window + ndk::wd_FirstGadget, after);
            } else {
                b.w32(prev + ndk::gg_NextGadget, after);
            }
            b.w32(tail + ndk::gg_NextGadget, 0);
            if (!b.ok()) {
                return 0xFFFFFFFF;
            }
            u32 rem = cur;
            for (u32 step = 0; step < removed && rem != 0; ++step) {
                if (drag_ == Drag::Prop && drag_window_ == window &&
                    native::guest_address(*machine_, drag_gadget_) ==
                        native::guest_address(*machine_, rem)) {
                    drag_ = Drag::None;
                    drag_window_ = 0;
                    drag_gadget_ = 0;
                }
                clear_gadget_hook(rem);
                rem = b.r32(rem + ndk::gg_NextGadget);
                if (!b.ok()) {
                    break;
                }
            }
            return pos;
        }
        prev = cur;
        cur = b.r32(cur + ndk::gg_NextGadget);
        if (!b.ok()) {
            return 0xFFFFFFFF;
        }
    }
    return 0xFFFFFFFF;
}

void Intuition::refresh_glist(u32 gadget, u32 window, i32 count) {
    if (window == 0 || gadget == 0 || count == 0 || !window_open(window)) {
        return;
    }
    Bus b(*machine_);
    const u32 max_count = count < 0 ? kMaxGadgets : static_cast<u32>(count);
    begin_batch();
    draw_gadget_list(b, window, gadget, max_count);
    end_batch();
}

void Intuition::new_modify_prop(u32 gadget, u32 window, u32 flags, u32 hpot, u32 vpot, u32 hbody,
                                u32 vbody, i32 count) {
    if (window == 0 || gadget == 0 || !window_open(window)) {
        return;
    }
    Bus b(*machine_);
    const u32 gtype = b.r16(gadget + ndk::gg_GadgetType) & ndk::GTYP_GTYPEMASK;
    if (!b.ok() || gtype != ndk::GTYP_PROPGADGET) {
        return;
    }
    const u32 pi = b.r32(gadget + ndk::gg_SpecialInfo);
    if (!b.ok() || pi == 0) {
        return;
    }
    u32 new_flags = flags & 0xFFFF;
    if (drag_ == Drag::Prop && drag_window_ == window &&
        native::guest_address(*machine_, drag_gadget_) ==
            native::guest_address(*machine_, gadget)) {
        new_flags |= ndk::KNOBHIT;
    }
    b.w16(pi + ndk::pi_Flags, new_flags);
    b.w16(pi + ndk::pi_HorizPot, hpot & 0xFFFF);
    b.w16(pi + ndk::pi_VertPot, vpot & 0xFFFF);
    b.w16(pi + ndk::pi_HorizBody, hbody & 0xFFFF);
    b.w16(pi + ndk::pi_VertBody, vbody & 0xFFFF);
    if (!b.ok()) {
        return;
    }
    refresh_glist(gadget, window, count);
}

void Intuition::set_gadget_disabled(u32 gadget, u32 window, bool disabled) {
    if (window == 0 || gadget == 0 || !window_open(window)) {
        return;
    }
    Bus b(*machine_);
    const u32 flags = b.r16(gadget + ndk::gg_Flags);
    if (!b.ok()) {
        return;
    }
    const u32 updated = disabled ? (flags | ndk::GFLG_DISABLED) : (flags & ~ndk::GFLG_DISABLED);
    b.w16(gadget + ndk::gg_Flags, updated);
    if (b.ok()) {
        refresh_glist(gadget, window, 1);
    }
}

void Intuition::on_gadget(u32 gadget, u32 window) {
    set_gadget_disabled(gadget, window, false);
}

void Intuition::off_gadget(u32 gadget, u32 window) {
    set_gadget_disabled(gadget, window, true);
}

void Intuition::wheel_event(i32 steps) {
    if (steps == 0 || active_window_ == 0) {
        return;
    }
    const u16 code = steps > 0 ? static_cast<u16>(ndk::NM_WHEEL_UP)
                               : static_cast<u16>(ndk::NM_WHEEL_DOWN);
    const i64 abs_steps = steps > 0 ? static_cast<i64>(steps) : -static_cast<i64>(steps);
    for (i64 i = 0; i < abs_steps; ++i) {
        if (!post_intuimessage(active_window_, ndk::IDCMP_RAWKEY, code)) {
            break;
        }
    }
}

bool Intuition::hit_prop_gadget(Bus& b, u32 window, i32 x, i32 y) {
    const WindowRecord* rec = find_window_record(window);
    if (rec == nullptr) {
        return false;
    }
    const WindowView w = load_window(b, window);
    if (!w.ok) {
        return false;
    }
    const i32 wx = x - w.left;
    const i32 wy = y - w.top;
    u32 gadget = w.first_gadget;
    for (u32 step = 0; step < kMaxGadgets && gadget != 0; ++step) {
        const u32 gflags = b.r16(gadget + ndk::gg_Flags);
        const u32 activation = b.r16(gadget + ndk::gg_Activation);
        const u32 gtype = b.r16(gadget + ndk::gg_GadgetType) & ndk::GTYP_GTYPEMASK;
        const u32 next = b.r32(gadget + ndk::gg_NextGadget);
        if (!b.ok()) {
            return false;
        }
        if (gtype != ndk::GTYP_PROPGADGET) {
            log_unhandled_gadget_type(gtype);
            gadget = next;
            continue;
        }
        if ((gflags & ndk::GFLG_DISABLED) != 0) {
            gadget = next;
            continue;
        }
        const PropGeometry g = load_prop_geometry(b, w, gadget);
        if (!g.ok || !in_rect(g.box, wx, wy)) {
            gadget = next;
            continue;
        }
        if (g.knob.x0 <= g.knob.x1 && g.knob.y0 <= g.knob.y1 && in_rect(g.knob, wx, wy)) {
            b.w16(g.pi + ndk::pi_Flags, g.pflags | ndk::KNOBHIT);
            b.w16(gadget + ndk::gg_Flags, gflags | ndk::GFLG_SELECTED);
            drag_ = Drag::Prop;
            drag_window_ = window;
            drag_gadget_ = gadget;
            grab_x_ = wx - g.knob.x0;
            grab_y_ = wy - g.knob.y0;
            WindowView draw_w = w;
            draw_w.rp = rec->rp;
            draw_prop_gadget(*graphics_, b, draw_w, gadget);
            if ((activation & ndk::GACT_IMMEDIATE) != 0) {
                post_intuimessage(window, ndk::IDCMP_GADGETDOWN, 0, gadget);
            }
            return true;
        }
        u32 new_hpot = g.hpot;
        u32 new_vpot = g.vpot;
        if ((g.pflags & ndk::FREEVERT) != 0 && g.knob.y0 <= g.knob.y1) {
            if (wy < g.knob.y0) {
                const i32 dec = static_cast<i32>(g.vpot) - static_cast<i32>(g.vbody);
                new_vpot = dec < 0 ? 0 : static_cast<u32>(dec);
            } else if (wy > g.knob.y1) {
                const u32 inc = static_cast<u32>(g.vpot) + static_cast<u32>(g.vbody);
                new_vpot = inc > ndk::MAXPOT ? ndk::MAXPOT : inc;
            }
        }
        if ((g.pflags & ndk::FREEHORIZ) != 0 && g.knob.x0 <= g.knob.x1) {
            if (wx < g.knob.x0) {
                const i32 dec = static_cast<i32>(g.hpot) - static_cast<i32>(g.hbody);
                new_hpot = dec < 0 ? 0 : static_cast<u32>(dec);
            } else if (wx > g.knob.x1) {
                const u32 inc = static_cast<u32>(g.hpot) + static_cast<u32>(g.hbody);
                new_hpot = inc > ndk::MAXPOT ? ndk::MAXPOT : inc;
            }
        }
        b.w16(g.pi + ndk::pi_HorizPot, new_hpot);
        b.w16(g.pi + ndk::pi_VertPot, new_vpot);
        WindowView draw_w = w;
        draw_w.rp = rec->rp;
        draw_prop_gadget(*graphics_, b, draw_w, gadget);
        call_gadget_hook(gadget, new_vpot);
        if (window_open(window)) {
            if ((activation & ndk::GACT_IMMEDIATE) != 0) {
                post_intuimessage(window, ndk::IDCMP_GADGETDOWN, 0, gadget);
            }
            if ((activation & ndk::GACT_RELVERIFY) != 0) {
                post_intuimessage(window, ndk::IDCMP_GADGETUP, 0, gadget);
            }
            pressed_gadget_window_ = window;
            pressed_gadget_ = GadgetKind::Prop;
        }
        return true;
    }
    return false;
}

void Intuition::drag_prop(i32 x, i32 y) {
    if (drag_window_ == 0 || drag_gadget_ == 0 || !window_open(drag_window_)) {
        drag_ = Drag::None;
        drag_window_ = 0;
        drag_gadget_ = 0;
        return;
    }
    const WindowRecord* rec = find_window_record(drag_window_);
    if (rec == nullptr) {
        drag_ = Drag::None;
        drag_window_ = 0;
        drag_gadget_ = 0;
        return;
    }
    Bus b(*machine_);
    const u32 gtype = b.r16(drag_gadget_ + ndk::gg_GadgetType) & ndk::GTYP_GTYPEMASK;
    const u32 gflags = b.r16(drag_gadget_ + ndk::gg_Flags);
    if (!b.ok() || gtype != ndk::GTYP_PROPGADGET || (gflags & ndk::GFLG_DISABLED) != 0) {
        return;
    }
    WindowView w = load_window(b, drag_window_);
    if (!w.ok) {
        return;
    }
    w.rp = rec->rp;
    const PropGeometry g = load_prop_geometry(b, w, drag_gadget_);
    const u32 activation = b.r16(drag_gadget_ + ndk::gg_Activation);
    if (!g.ok || !b.ok()) {
        return;
    }
    const i32 wx = x - w.left;
    const i32 wy = y - w.top;
    u16 new_hpot = g.hpot;
    u16 new_vpot = g.vpot;
    if ((g.pflags & ndk::FREEHORIZ) != 0 && g.travel_x > 0) {
        new_hpot = pot_from_offset((wx - grab_x_) - g.inner.x0, g.travel_x);
    }
    if ((g.pflags & ndk::FREEVERT) != 0 && g.travel_y > 0) {
        new_vpot = pot_from_offset((wy - grab_y_) - g.inner.y0, g.travel_y);
    }
    const u32 window = drag_window_;
    const u32 gadget = drag_gadget_;
    if (new_hpot != g.hpot || new_vpot != g.vpot) {
        b.w16(g.pi + ndk::pi_HorizPot, new_hpot);
        b.w16(g.pi + ndk::pi_VertPot, new_vpot);
        draw_prop_gadget(*graphics_, b, w, gadget);
        call_gadget_hook(gadget, new_vpot);
    }
    if (window_open(window) && (activation & ndk::GACT_FOLLOWMOUSE) != 0) {
        post_intuimessage(window, ndk::IDCMP_MOUSEMOVE, 0, gadget);
    }
}

void Intuition::end_prop_drag() {
    const u32 window = drag_window_;
    const u32 gadget = drag_gadget_;
    drag_ = Drag::None;
    drag_window_ = 0;
    drag_gadget_ = 0;
    if (window == 0 || gadget == 0 || !window_open(window)) {
        return;
    }
    Bus b(*machine_);
    const u32 gflags = b.r16(gadget + ndk::gg_Flags);
    const u32 activation = b.r16(gadget + ndk::gg_Activation);
    const u32 gtype = b.r16(gadget + ndk::gg_GadgetType) & ndk::GTYP_GTYPEMASK;
    const u32 pi = b.r32(gadget + ndk::gg_SpecialInfo);
    if (!b.ok()) {
        return;
    }
    b.w16(gadget + ndk::gg_Flags, gflags & ~ndk::GFLG_SELECTED);
    if (gtype == ndk::GTYP_PROPGADGET && pi != 0) {
        const u32 pflags = b.r16(pi + ndk::pi_Flags);
        if (b.ok()) {
            b.w16(pi + ndk::pi_Flags, pflags & ~ndk::KNOBHIT);
        }
        const WindowRecord* rec = find_window_record(window);
        if (rec != nullptr) {
            WindowView w = load_window(b, window);
            if (w.ok) {
                w.rp = rec->rp;
                draw_prop_gadget(*graphics_, b, w, gadget);
            }
        }
    }
    if ((gflags & ndk::GFLG_DISABLED) == 0 && (activation & ndk::GACT_RELVERIFY) != 0) {
        post_intuimessage(window, ndk::IDCMP_GADGETUP, 0, gadget);
    }
}

}
