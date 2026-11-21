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
#include "sysimage.hpp"
#include "utility.hpp"
#include "version.hpp"

namespace hombre {

using m68k::Machine;
using native::Bus;
using i16 = __INT16_TYPE__;

namespace {

// Two clicks this close together make a double click.
constexpr u64 kDoubleClickUs = 1000000;

// struct InputEvent (devices/inputevent.h) and the mouse codes a custom
// gadget gets.
constexpr u32 ie_Class = 4;
constexpr u32 ie_Code = 6;
constexpr u32 ie_Qualifier = 8;
constexpr u32 kEventBytes = 24;  // the InputEvent, then the termination long
constexpr u32 kTermination = 24;
constexpr u32 IECLASS_RAWMOUSE = 2;
constexpr u32 IECODE_LBUTTON = 0x68;
constexpr u32 IECODE_UP_PREFIX = 0x80;
constexpr u32 IECODE_NOBUTTON = 0xFF;
constexpr u32 IEQUALIFIER_LEFTBUTTON = 0x4000;

u32 pack_point(i32 x, i32 y) {
    return (static_cast<u32>(x) << 16) | (static_cast<u32>(y) & 0xFFFF);
}

}

namespace {

constexpr char kIntuitionName[] = "intuition.library";

// struct Screen's sc_NextScreen, the pointer before sc_FirstWindow.
constexpr u32 kScNextScreen = 0;
constexpr u32 kWBENCHSCREEN = 1;
constexpr u32 kMaxTitleChars = 255;

// struct Image (intuition/intuition.h), as .info files and DrawImage lay it
// out.
namespace image {
constexpr u32 LeftEdge = 0;
constexpr u32 TopEdge = 2;
constexpr u32 Width = 4;
constexpr u32 Height = 6;
constexpr u32 Depth = 8;
constexpr u32 ImageData = 10;
constexpr u32 PlanePick = 14;
constexpr u32 PlaneOnOff = 15;
constexpr u32 NextImage = 16;
}

constexpr u32 kMaxImageChain = 64;

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
    if (pen < chrome::kMenuPaperPen) {
        return chrome::screen_bar_rgb(kBasePalette[2], pen - chrome::kScreenBar0);
    }
    if (pen < chrome::kChromeBase) {
        return chrome::kMenuPalette[pen - chrome::kMenuPaperPen];
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

constexpr char kSystemRequestTitle[] = "System Request";

// Formats the guest format string at format, taking the arguments from the
// stream at data, into out: at most capacity - 1 characters, NUL included.
// A null format gives an empty string.
bool format_requester_text(Bus& b, u32 format, u32& data, char* out, u32 capacity) {
    out[0] = '\0';
    if (format == 0) {
        return true;
    }
    struct Sink {
        char* out;
        u32 capacity;
        u32 length = 0;

        bool put(u32 c) {
            if (length + 1 >= capacity) {
                return false;
            }
            out[length++] = static_cast<char>(c);
            return true;
        }
    } sink{out, capacity};
    native::raw_format(b, format, data, sink);
    out[sink.length] = '\0';
    return b.ok();
}

struct RequesterLine {
    const char* text;
    u32 length;
};

u32 split_requester_lines(const char* text, RequesterLine* lines, u32 max) {
    u32 count = 0;
    const char* at = text;
    for (;;) {
        const char* end = at;
        while (*end != '\0' && *end != '\n') {
            ++end;
        }
        if (count < max) {
            lines[count++] = RequesterLine{at, static_cast<u32>(end - at)};
        }
        if (*end == '\0') {
            return count;
        }
        at = end + 1;
    }
}

constexpr i32 smaller(i32 a, i32 b) { return a < b ? a : b; }

constexpr i32 larger(i32 a, i32 b) { return a > b ? a : b; }

// The pens of a requester on screens of eight planes, and the classic
// white, black and grey of the shallower ones.
struct RequesterPens {
    u8 light;
    u8 dark;
    u8 paper;
    u8 text;
};

RequesterPens requester_pens(u32 depth) {
    if (depth >= 8) {
        return RequesterPens{chrome::kFrameLight, chrome::kFrameDark, chrome::kInactiveBar0,
                             chrome::kFrameDark};
    }
    return RequesterPens{2, 1, 2, 1};
}

// A bevel of two pixels around the paper: the top and left edges in the
// light pen and the bottom and right in the dark, or the other way round
// for a recessed box.
void draw_requester_box(Graphics& graphics, u32 rp, const Rect& box, const RequesterPens& pens,
                        bool recessed) {
    if (box.x1 < box.x0 || box.y1 < box.y0) {
        return;
    }
    graphics.set_pens(rp, pens.paper, 0, ndk::JAM2);
    graphics.rect_fill(rp, box.x0, box.y0, box.x1, box.y1);
    const u8 top = recessed ? pens.dark : pens.light;
    const u8 bottom = recessed ? pens.light : pens.dark;
    for (i32 inset = 0; inset < 2; ++inset) {
        const i32 x0 = box.x0 + inset;
        const i32 y0 = box.y0 + inset;
        const i32 x1 = box.x1 - inset;
        const i32 y1 = box.y1 - inset;
        if (x0 > x1 || y0 > y1) {
            break;
        }
        graphics.set_pens(rp, top, 0, ndk::JAM2);
        graphics.rect_fill(rp, x0, y0, x1, y0);
        graphics.rect_fill(rp, x0, y0, x0, y1);
        graphics.set_pens(rp, bottom, 0, ndk::JAM2);
        graphics.rect_fill(rp, x0, y1, x1, y1);
        graphics.rect_fill(rp, x1, y0, x1, y1);
    }
}

void draw_requester_text(Graphics& graphics, u32 rp, i32 x, i32 baseline, const char* text,
                         u32 length, u32 room, u8 pen) {
    if (length > room) {
        length = room;
    }
    if (length == 0) {
        return;
    }
    graphics.set_pens(rp, pen, 0, ndk::JAM1);
    graphics.move(rp, x, baseline);
    graphics.text(rp, reinterpret_cast<const u8*>(text), length);
}

void draw_requester_label(Graphics& graphics, u32 rp, const Rect& box, const char* text,
                          u32 length, u8 pen) {
    const i32 width = box.x1 - box.x0 + 1;
    const i32 height = box.y1 - box.y0 + 1;
    const i32 room = (width - 4) / 8;
    if (room <= 0 || height < 8) {
        return;
    }
    if (length > static_cast<u32>(room)) {
        length = static_cast<u32>(room);
    }
    const i32 x = box.x0 + (width - static_cast<i32>(length) * 8) / 2;
    const i32 baseline = box.y0 + (height - 8) / 2 + 6;
    draw_requester_text(graphics, rp, x, baseline, text, length, static_cast<u32>(room), pen);
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
    u32 inner_width = 0;   // the size inside the borders, when given instead
    u32 inner_height = 0;
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
    case ndk::WA_InnerWidth:
        a.inner_width = data;
        break;
    case ndk::WA_InnerHeight:
        a.inner_height = data;
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

// The part of the rectangle with its top-left corner at x0, y0 that lies
// inside the view's clip and bitmap, as bitmap coordinates.
bool clip_image_rect(const raster::RastPortView& view, i64 x0, i64 y0, i32 width, i32 height,
                     Rect& out) {
    i64 left = x0 > view.clip.x0 ? x0 : view.clip.x0;
    i64 top = y0 > view.clip.y0 ? y0 : view.clip.y0;
    i64 right = x0 + width - 1;
    i64 bottom = y0 + height - 1;
    if (right > view.clip.x1) {
        right = view.clip.x1;
    }
    if (bottom > view.clip.y1) {
        bottom = view.clip.y1;
    }
    const i64 bitmap_width = static_cast<i64>(view.bitmap.bytes_per_row) << 3;
    const i64 bitmap_height = static_cast<i64>(view.bitmap.rows);
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right >= bitmap_width) {
        right = bitmap_width - 1;
    }
    if (bottom >= bitmap_height) {
        bottom = bitmap_height - 1;
    }
    if (left > right || top > bottom) {
        return false;
    }
    out = Rect{static_cast<i32>(left), static_cast<i32>(top), static_cast<i32>(right),
               static_cast<i32>(bottom)};
    return true;
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

chrome::WindowChrome to_chrome(const WindowView& w, bool active, chrome::BarButton pressed) {
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
        pressed,
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

// struct DrawInfo (intuition/screens.h) and the 3.1 default pens:
// DETAILPEN, BLOCKPEN, TEXTPEN, SHINEPEN, SHADOWPEN, FILLPEN, FILLTEXTPEN,
// BACKGROUNDPEN, HIGHLIGHTTEXTPEN, BARDETAILPEN, BARBLOCKPEN, BARTRIMPEN.
constexpr u32 kDriVersion = 0;
constexpr u32 kDriNumPens = 2;
constexpr u32 kDriPens = 4;
constexpr u32 kDriFont = 8;
constexpr u32 kDriDepth = 12;
constexpr u32 kDriResolutionX = 14;
constexpr u32 kDriResolutionY = 16;
constexpr u32 kDriFlags = 18;
// Window.MenuStrip and the fields of Menu, MenuItem and IntuiText menu
// strips carry.
constexpr u32 kWdMenuStrip = 28;
constexpr u32 kMuNextMenu = 0;
constexpr u32 kMuLeftEdge = 4;
constexpr u32 kMuWidth = 8;
constexpr u32 kMuFlags = 12;
constexpr u32 kMuMenuName = 14;
constexpr u32 kMuFirstItem = 18;
constexpr u32 kMiNextItem = 0;
constexpr u32 kMiLeftEdge = 4;
constexpr u32 kMiTopEdge = 6;
constexpr u32 kMiWidth = 8;
constexpr u32 kMiHeight = 10;
constexpr u32 kMiFlags = 12;
constexpr u32 kMiMutualExclude = 14;
constexpr u32 kMiItemFill = 18;
constexpr u32 kMiCommand = 26;
constexpr u32 kMiSubItem = 28;
constexpr u32 kMiNextSelect = 32;
constexpr u32 kItText = 12;
constexpr u16 kMenuNull = 0xFFFF;
constexpr u32 kIdcmpMenuPick = 0x00000100;
constexpr u32 kDriNewLook = 1;
constexpr u32 kDriPenCount = chrome::kDrawInfoPens;
constexpr u16 kDefaultDriPens[kDriPenCount] = {0, 1, 1, 2, 1, 3, 1, 0, 2, 1, 2, 1};
constexpr u32 kDriBackgroundPen = 7;  // BACKGROUNDPEN in that order
constexpr u32 kDrawInfoPensAt = 52;
constexpr u32 kDrawInfoBytes = kDrawInfoPensAt + (kDriPenCount + 1) * 2;

namespace {

u32 read_menu_titles(Bus& b, u32 strip, menu_layout::Title* titles, u32 max) {
    u32 count = 0;
    for (u32 at = strip; at != 0 && count < max && b.ok();) {
        menu_layout::Title& title = titles[count];
        title.left = static_cast<i16>(b.r16(at + kMuLeftEdge));
        title.width = static_cast<i16>(b.r16(at + kMuWidth));
        title.first = b.r32(at + kMuFirstItem);
        title.name = b.r32(at + kMuMenuName);
        title.flags = static_cast<u16>(b.r16(at + kMuFlags));
        at = b.r32(at + kMuNextMenu);
        ++count;
    }
    return b.ok() ? count : 0;
}

u32 read_menu_items(Bus& b, u32 first, menu_layout::Item* items, u32 max) {
    u32 count = 0;
    for (u32 at = first; at != 0 && count < max && b.ok();) {
        menu_layout::Item& item = items[count];
        item.left = static_cast<i16>(b.r16(at + kMiLeftEdge));
        item.top = static_cast<i16>(b.r16(at + kMiTopEdge));
        item.width = static_cast<i16>(b.r16(at + kMiWidth));
        item.height = static_cast<i16>(b.r16(at + kMiHeight));
        item.flags = static_cast<u16>(b.r16(at + kMiFlags));
        item.command = static_cast<u8>(b.r8(at + kMiCommand));
        item.sub = b.r32(at + kMiSubItem);
        item.addr = at;
        const u32 fill = b.r32(at + kMiItemFill);
        item.label = fill != 0 ? b.r32(fill + kItText) : 0;
        at = b.r32(at + kMiNextItem);
        ++count;
    }
    return b.ok() ? count : 0;
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

    // The one public screen is the default screen, under the name
    // "Workbench"; a null name asks for the default public screen.
    static bool is_default_pub_name(Bus& b, u32 name) {
        if (name == 0) {
            return true;
        }
        const char* want = "Workbench";
        for (u32 i = 0;; ++i) {
            const u32 c = b.r8(name + i);
            if (!b.ok()) {
                return false;
            }
            const char w = want[i];
            const u32 lc = (c >= 'A' && c <= 'Z') ? c + 32 : c;
            const u32 lw = (w >= 'A' && w <= 'Z') ? static_cast<u32>(w) + 32 : static_cast<u32>(w);
            if (lc != lw) {
                return false;
            }
            if (c == 0) {
                return true;
            }
        }
    }

    static void lock_pub_screen(void* ctx, Machine& m) {
        Intuition& g = of(ctx);
        Bus b(m);
        const u32 screen = g.default_screen_ != 0 ? g.default_screen_ : g.front_;
        if (!is_default_pub_name(b, m.cpu.a[0]) || screen == 0) {
            native::set_result(m, 0);
            return;
        }
        native::set_result(m, screen);
    }

    static void unlock_pub_screen(void*, Machine&) {}

    // LockPubScreenList: a List of the public screens, which is the one
    // PubScreenNode named "Workbench" for the default screen. The list
    // lives as long as intuition; 0 when there is no screen.
    static void lock_pub_screen_list(void* ctx, Machine& m) {
        Intuition& g = of(ctx);
        Bus b(m);
        constexpr u32 kListBytes = 14;
        constexpr u32 kNodeBytes = 30;
        constexpr char kName[] = "Workbench";
        const u32 screen = g.default_screen_ != 0 ? g.default_screen_ : g.front_;
        if (screen == 0) {
            native::set_result(m, 0);
            return;
        }
        if (g.pub_screen_list_ == 0) {
            const u32 list = g.exec_->alloc(kListBytes + kNodeBytes + sizeof kName,
                                            ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
            if (list == 0) {
                native::set_result(m, 0);
                return;
            }
            const u32 node = list + kListBytes;
            const u32 name = node + kNodeBytes;
            for (u32 i = 0; i < sizeof kName; ++i) {
                b.w8(name + i, static_cast<u8>(kName[i]));
            }
            // lh_Head, lh_Tail, lh_TailPred around the one node.
            b.w32(list + 0, node);
            b.w32(list + 4, 0);
            b.w32(list + 8, node);
            b.w32(node + 0, list + 4);
            b.w32(node + 4, list);
            b.w32(node + 10, name);
            g.pub_screen_list_ = list;
        }
        // psn_Screen
        b.w32(g.pub_screen_list_ + kListBytes + 14, screen);
        native::set_result(m, b.ok() ? g.pub_screen_list_ : 0);
    }

    static void unlock_pub_screen_list(void*, Machine&) {}

    static void get_screen_draw_info(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).screen_draw_info(m.cpu.a[0]));
    }

    static void free_screen_draw_info(void*, Machine&) {}

    static void set_menu_strip(void* ctx, Machine& m) {
        const u32 window = m.cpu.a[0];
        of(ctx).set_menu_strip(window, m.cpu.a[1]);
        native::set_result(m, window != 0 ? 1 : 0);
    }

    static void clear_menu_strip(void* ctx, Machine& m) {
        of(ctx).clear_menu_strip(m.cpu.a[0]);
    }

    // The MenuItem a menu number names: menu in bits 0-4, item in bits
    // 5-10 and subitem in bits 11-15, 31 for none; 0 for MENUNULL or a
    // number past the strip.
    static void item_address(void*, Machine& m) {
        Bus b(m);
        const u32 number = m.cpu.d[0] & 0xFFFF;
        if (number == 0xFFFF || m.cpu.a[0] == 0) {
            native::set_result(m, 0);
            return;
        }
        auto nth = [&](u32 first, u32 next_at, u32 index) {
            u32 at = first;
            for (u32 i = 0; at != 0 && i < index && b.ok(); ++i) {
                at = b.r32(at + next_at);
            }
            return at;
        };
        const u32 menu = nth(m.cpu.a[0], 0, number & 0x1F);
        const u32 item_number = (number >> 5) & 0x3F;
        if (menu == 0 || item_number == 0x3F) {
            native::set_result(m, 0);
            return;
        }
        u32 item = nth(b.r32(menu + kMuFirstItem), 0, item_number);
        const u32 sub_number = (number >> 11) & 0x1F;
        if (item != 0 && sub_number != 0x1F) {
            item = nth(b.r32(item + kMiSubItem), 0, sub_number);
        }
        native::set_result(m, b.ok() ? item : 0);
    }

    // Windows keep their own bitmaps, so there is no damage to repair.
    static void begin_refresh(void*, Machine&) {}

    static void end_refresh(void*, Machine&) {}

    // Copies "Workbench" into the buffer at a0, which must hold
    // MAXPUBSCREENNAME + 1 bytes.
    static void get_default_pub_screen(void*, Machine& m) {
        Bus b(m);
        const u32 buffer = m.cpu.a[0];
        if (buffer == 0) {
            return;
        }
        const char* name = "Workbench";
        for (u32 i = 0;; ++i) {
            b.w8(buffer + i, static_cast<u8>(name[i]));
            if (name[i] == '\0' || !b.ok()) {
                break;
            }
        }
    }

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

    // CloseWorkBench: the Workbench leaves the default screen so IPrefs can
    // reset it. Every window still open on it must be the Workbench's own.
    static void close_workbench(void* ctx, Machine& m) {
        Intuition& in = of(ctx);
        if (in.default_screen_ == 0) {
            native::set_result(m, 1);
            return;
        }
        for (const Intuition::WindowRecord& window : in.window_records_) {
            if (window.addr != 0 && window.screen == in.default_screen_ &&
                (window.flags & ndk::WFLG_WBENCHWINDOW) == 0) {
                native::set_result(m, 0);
                return;
            }
        }
        if (in.workbench_hooks_.close != nullptr &&
            !in.workbench_hooks_.close(in.workbench_hooks_.ctx)) {
            native::set_result(m, 0);
            return;
        }
        in.workbench_closed_ = true;
        LogLine().text("intuition: Workbench closed").send();
        native::set_result(m, 1);
    }

    // OpenWorkBench: the default screen takes the palette and pens IPrefs
    // sent, starts over, and the Workbench opens its windows again.
    static void open_workbench(void* ctx, Machine& m) {
        Intuition& in = of(ctx);
        const u32 screen = in.default_screen_;
        if (screen == 0) {
            native::set_result(m, 0);
            return;
        }
        Intuition::ScreenRecord* rec = in.find_screen_record(screen);
        if (rec != nullptr) {
            in.apply_palette_prefs(*rec);
            in.apply_pen_prefs(screen);
            in.update_chrome_palette(screen);
            in.reset_screen(screen);
        }
        if (in.workbench_closed_) {
            in.workbench_closed_ = false;
            if (in.workbench_hooks_.open != nullptr) {
                in.workbench_hook_task_ = in.workbench_task_;
                in.workbench_hooks_.open(in.workbench_hooks_.ctx);
                in.workbench_hook_task_ = 0;
            }
        }
        LogLine().text("intuition: Workbench opened").send();
        native::set_result(m, screen);
    }

    static void free_sys_request(void* ctx, Machine& m) {
        of(ctx).free_sys_request(m.cpu.a[0]);
    }

    static void easy_request_args(void* ctx, Machine& m) {
        native::set_result(m, static_cast<u32>(of(ctx).easy_request(
                                  m.cpu.a[0], m.cpu.a[1], m.cpu.a[2], m.cpu.a[3])));
    }

    static void build_easy_request_args(void* ctx, Machine& m) {
        native::set_result(
            m, of(ctx).build_easy_request(m.cpu.a[0], m.cpu.a[1], m.cpu.d[0], m.cpu.a[3]));
    }

    static void sys_req_handler(void* ctx, Machine& m) {
        native::set_result(
            m, static_cast<u32>(of(ctx).sys_req_handler(m.cpu.a[0], m.cpu.a[1],
                                                        (m.cpu.d[0] & 0xFFFF) != 0)));
    }

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

    static void window_limits(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).window_limits(
                                  m.cpu.a[0], static_cast<i16>(m.cpu.d[0] & 0xFFFF),
                                  static_cast<i16>(m.cpu.d[1] & 0xFFFF), m.cpu.d[2] & 0xFFFF,
                                  m.cpu.d[3] & 0xFFFF)
                                  ? 1
                                  : 0);
    }

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

    // struct IntuiText: its pens, draw mode, place, font, text and the next
    // one of the chain. Text is in the topaz font, 8 pixels to a character.
    static constexpr u32 kItFrontPen = 0;
    static constexpr u32 kItBackPen = 1;
    static constexpr u32 kItDrawMode = 2;
    static constexpr u32 kItLeftEdge = 4;
    static constexpr u32 kItTopEdge = 6;
    static constexpr u32 kItNextText = 16;
    static constexpr u32 kItChain = 64;
    static constexpr u32 kItMaxChars = 255;

    static u32 itext_chars(Bus& b, u32 text) {
        u32 n = 0;
        while (text != 0 && n < kItMaxChars && b.r8(text + n) != 0 && b.ok()) {
            ++n;
        }
        return n;
    }

    // IntuiTextLength: the width in pixels of the IntuiText at A0.
    static void intui_text_length(void*, Machine& m) {
        Bus b(m);
        const u32 it = m.cpu.a[0];
        const u32 chars = it != 0 ? itext_chars(b, b.r32(it + kItText)) : 0;
        if (b.ok()) {
            native::set_result(m, chars * 8);
        }
    }

    // PrintIText(rp A0, itext A1, left D0, top D1): each IntuiText of the
    // chain at its place from left, top, the top of its text at that line.
    static void print_itext(void* ctx, Machine& m) {
        Intuition& in = of(ctx);
        Bus b(m);
        const u32 rp = m.cpu.a[0];
        const i32 left = static_cast<i16>(m.cpu.d[0] & 0xFFFF);
        const i32 top = static_cast<i16>(m.cpu.d[1] & 0xFFFF);
        u32 it = m.cpu.a[1];
        for (u32 step = 0; rp != 0 && it != 0 && step < kItChain; ++step) {
            const u32 text = b.r32(it + kItText);
            const u32 chars = itext_chars(b, text);
            const i32 x = left + static_cast<i16>(b.r16(it + kItLeftEdge));
            const i32 y = top + static_cast<i16>(b.r16(it + kItTopEdge));
            const u32 front = b.r8(it + kItFrontPen);
            const u32 back = b.r8(it + kItBackPen);
            const u32 mode = b.r8(it + kItDrawMode);
            const u32 next = b.r32(it + kItNextText);
            if (!b.ok()) {
                return;
            }
            if (chars != 0) {
                u8 buffer[kItMaxChars];
                for (u32 i = 0; i < chars; ++i) {
                    buffer[i] = static_cast<u8>(b.r8(text + i));
                }
                in.graphics_->set_pens(rp, front, back, mode);
                in.graphics_->move(rp, x, y + 6);
                in.graphics_->text(rp, buffer, chars);
            }
            it = next;
        }
    }

    // Request(requester A0, window A1): the window takes no clicks on its
    // gadgets until EndRequest. The requester itself is not drawn.
    static void request(void* ctx, Machine& m) {
        Intuition& in = of(ctx);
        const u32 window = m.cpu.a[1];
        if (window == 0 || !in.window_open(window)) {
            native::set_result(m, 0);
            return;
        }
        for (Intuition::Block& block : in.blocks_) {
            if (block.window == 0) {
                block = Intuition::Block{window, m.cpu.a[0]};
                native::set_result(m, 1);
                return;
            }
        }
        native::set_result(m, 0);
    }

    static void end_request(void* ctx, Machine& m) {
        for (Intuition::Block& block : of(ctx).blocks_) {
            if (block.requester == m.cpu.a[0] && block.window == m.cpu.a[1]) {
                block = Intuition::Block{};
                return;
            }
        }
    }

    // SetPointer, ClearPointer and SetWindowPointerA: a window's own
    // pointer, a busy one included, is not shown; the screen's stays.
    static void keep_pointer(void*, Machine&) {}

    // ActivateGadget: a string gadget becomes the one being edited.
    static void activate_gadget(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).activate_string(m.cpu.a[0], m.cpu.a[1]) ? 1 : 0);
    }

    // ScrollWindowRaster(window A1, dx D0, dy D1, xmin D2, ymin D3, xmax D4,
    // ymax D5): the box of the window's raster scrolls; what it uncovers
    // takes pen 0 for the program to draw again.
    static void scroll_window_raster(void* ctx, Machine& m) {
        Intuition& in = of(ctx);
        const Intuition::WindowRecord* rec = in.find_window_record(m.cpu.a[1]);
        if (rec == nullptr) {
            return;
        }
        const auto word = [&](u32 r) { return static_cast<i32>(static_cast<i16>(r & 0xFFFF)); };
        in.graphics_->scroll(rec->rp, word(m.cpu.d[0]), word(m.cpu.d[1]), word(m.cpu.d[2]),
                             word(m.cpu.d[3]), word(m.cpu.d[4]), word(m.cpu.d[5]));
    }

    // DoubleClick(seconds D0, micros D1, seconds D2, micros D3): whether
    // the second click came within a second of the first, the Workbench's
    // double-click time.
    // DoubleClick: whether the two times lie no further apart than the
    // double-click time of the current Preferences.
    static void double_click(void* ctx, Machine& m) {
        const Intuition& in = of(ctx);
        constexpr u32 kDoubleClick = 20;  // struct timeval: seconds, micros
        u32 seconds = 0;
        u32 micros = 0;
        for (u32 i = 0; i < 4; ++i) {
            seconds = (seconds << 8) | in.prefs_[kDoubleClick + i];
            micros = (micros << 8) | in.prefs_[kDoubleClick + 4 + i];
        }
        const u64 limit =
            in.prefs_set_ ? static_cast<u64>(seconds) * 1000000 + micros : kDoubleClickUs;
        const u64 first = static_cast<u64>(m.cpu.d[0]) * 1000000 + m.cpu.d[1];
        const u64 second = static_cast<u64>(m.cpu.d[2]) * 1000000 + m.cpu.d[3];
        native::set_result(m, second >= first && second - first <= limit ? 1 : 0);
    }

    // The default struct Preferences: topaz 8 and the double-click time;
    // the rest stays 0 until SetPrefs gives more.
    static void default_prefs(u8* prefs) {
        for (u32 i = 0; i < Intuition::kPrefsBytes; ++i) {
            prefs[i] = 0;
        }
        constexpr u32 kFontHeight = 0;
        constexpr u32 kDoubleClick = 20;  // struct timeval: seconds, micros
        prefs[kFontHeight] = 8;
        const u32 seconds = static_cast<u32>(kDoubleClickUs / 1000000);
        const u32 micros = static_cast<u32>(kDoubleClickUs % 1000000);
        for (u32 i = 0; i < 4; ++i) {
            prefs[kDoubleClick + i] = static_cast<u8>(seconds >> (24 - 8 * i));
            prefs[kDoubleClick + 4 + i] = static_cast<u8>(micros >> (24 - 8 * i));
        }
    }

    static void copy_prefs_out(Machine& m, const u8* prefs) {
        Bus b(m);
        const u32 to = m.cpu.a[0];
        const u32 size = m.cpu.d[0] < Intuition::kPrefsBytes ? m.cpu.d[0] : Intuition::kPrefsBytes;
        for (u32 i = 0; to != 0 && i < size && b.ok(); ++i) {
            b.w8(to + i, prefs[i]);
        }
        native::set_result(m, to);
    }

    // GetDefPrefs(prefs A0, size D0): the defaults into prefs.
    static void get_def_prefs(void*, Machine& m) {
        u8 prefs[Intuition::kPrefsBytes];
        default_prefs(prefs);
        copy_prefs_out(m, prefs);
    }

    // GetPrefs(prefs A0, size D0): the preferences in use into prefs.
    static void get_prefs(void* ctx, Machine& m) {
        Intuition& in = of(ctx);
        if (!in.prefs_set_) {
            default_prefs(in.prefs_);
            in.prefs_set_ = true;
        }
        copy_prefs_out(m, in.prefs_);
    }

    // SetPrefs(prefs A0, size D0, inform D1): keeps prefs for GetPrefs.
    // Nothing on the screen follows them yet. TODO: apply them.
    static void set_prefs(void* ctx, Machine& m) {
        Intuition& in = of(ctx);
        Bus b(m);
        if (!in.prefs_set_) {
            default_prefs(in.prefs_);
            in.prefs_set_ = true;
        }
        const u32 from = m.cpu.a[0];
        const u32 size = m.cpu.d[0] < Intuition::kPrefsBytes ? m.cpu.d[0] : Intuition::kPrefsBytes;
        for (u32 i = 0; from != 0 && i < size && b.ok(); ++i) {
            in.prefs_[i] = static_cast<u8>(b.r8(from + i));
        }
        native::set_result(m, from);
    }

    // SetIPrefs(prefs A0, size D0, type D1), private to IPrefs: type 8 is
    // the palette and type 9 the two DrawInfo pen sets. Other types are
    // preferences this does not use yet. 0 back.
    static void set_iprefs(void* ctx, Machine& m) {
        Intuition& in = of(ctx);
        const u32 prefs = m.cpu.a[0];
        const u32 size = m.cpu.d[0];
        if (m.cpu.d[1] == 8) {
            in.store_palette_prefs(prefs, size);
            if (in.default_screen_ != 0) {
                auto* rec = in.find_screen_record(in.default_screen_);
                if (rec != nullptr) {
                    in.apply_palette_prefs(*rec);
                }
                in.update_chrome_palette(in.default_screen_);
                in.flush_palette();
            }
        } else if (m.cpu.d[1] == 9) {
            in.store_pen_prefs(prefs, size);
        }
        native::set_result(m, 0);
    }

    // PubScreenStatus(screen A0, flags D0): public screens are not tracked
    // yet, so no visitor ever holds one up; 0 back.
    static void pub_screen_status(void*, Machine& m) { native::set_result(m, 0); }

    // InitRequester: clears the struct Requester at A0.
    static void init_requester(void*, Machine& m) {
        Bus b(m);
        constexpr u32 kRequesterBytes = 112;
        for (u32 i = 0; m.cpu.a[0] != 0 && i < kRequesterBytes && b.ok(); ++i) {
            b.w8(m.cpu.a[0] + i, 0);
        }
    }

    // DrawBorder(rp A0, border A1, left D0, top D1): each Border of the
    // chain as lines through its points from left, top.
    static void draw_border(void* ctx, Machine& m) {
        Intuition& in = of(ctx);
        Bus b(m);
        const u32 rp = m.cpu.a[0];
        const i32 left = static_cast<i16>(m.cpu.d[0] & 0xFFFF);
        const i32 top = static_cast<i16>(m.cpu.d[1] & 0xFFFF);
        u32 border = m.cpu.a[1];
        for (u32 step = 0; rp != 0 && border != 0 && step < kItChain; ++step) {
            const i32 x = left + static_cast<i16>(b.r16(border));
            const i32 y = top + static_cast<i16>(b.r16(border + 2));
            const u32 front = b.r8(border + 4);
            const u32 back = b.r8(border + 5);
            const u32 mode = b.r8(border + 6);
            const u32 count = b.r8(border + 7);
            const u32 points = b.r32(border + 8);
            const u32 next = b.r32(border + 12);
            if (!b.ok()) {
                return;
            }
            in.graphics_->set_pens(rp, front, back, mode);
            for (u32 i = 0; i < count && points != 0; ++i) {
                const i32 px = x + static_cast<i16>(b.r16(points + i * 4));
                const i32 py = y + static_cast<i16>(b.r16(points + i * 4 + 2));
                if (!b.ok()) {
                    return;
                }
                if (i == 0) {
                    in.graphics_->move(rp, px, py);
                    // A single point is drawn as one.
                    if (count == 1) {
                        in.graphics_->draw_line(rp, px, py);
                    }
                } else {
                    in.graphics_->draw_line(rp, px, py);
                }
            }
            border = next;
        }
    }

    // BOOPSI. A GadgetInfo goes with a method sent to a gadget of a window.
    static void new_object(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).boopsi_.new_object(m.cpu.a[0], m.cpu.a[1], m.cpu.a[2]));
    }

    static void dispose_object(void* ctx, Machine& m) {
        of(ctx).boopsi_.dispose_object(m.cpu.a[0]);
    }

    static void set_attrs(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).boopsi_.set_attrs(m.cpu.a[0], m.cpu.a[1], 0));
    }

    static void get_attr(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).boopsi_.get_attr(m.cpu.d[0], m.cpu.a[0], m.cpu.a[1]));
    }

    static void set_gadget_attrs(void* ctx, Machine& m) {
        Intuition& in = of(ctx);
        const u32 info = m.cpu.a[1] != 0 ? in.make_gadget_info(m.cpu.a[1]) : 0;
        const u32 result = in.boopsi_.set_attrs(m.cpu.a[0], m.cpu.a[3], info);
        in.free_gadget_info(info);
        native::set_result(m, result);
    }

    static void next_object(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).boopsi_.next_object(m.cpu.a[0]));
    }

    static void make_class(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).boopsi_.make_class(m.cpu.a[0], m.cpu.a[1], m.cpu.a[2],
                                                         m.cpu.d[0], m.cpu.d[1]));
    }

    static void add_class(void* ctx, Machine& m) { of(ctx).boopsi_.add_class(m.cpu.a[0]); }

    static void remove_class(void* ctx, Machine& m) {
        of(ctx).boopsi_.remove_class(m.cpu.a[0]);
    }

    static void free_class(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).boopsi_.free_class(m.cpu.a[0]) ? 1 : 0);
    }

    // The GadgetInfo goes where the method has it: after the MethodID for
    // a GM_ method, after the tag list for ndk::OM_NEW, ndk::OM_SET, ndk::OM_NOTIFY and
    // ndk::OM_UPDATE.
    static void do_gadget_method(void* ctx, Machine& m) {
        Intuition& in = of(ctx);
        Bus b(m);
        const u32 msg = m.cpu.a[3];
        const u32 method = b.r32(msg);
        if (!b.ok()) {
            return;
        }
        const u32 info = m.cpu.a[1] != 0 ? in.make_gadget_info(m.cpu.a[1]) : 0;
        if (method < 0x100) {
            b.w32(msg + 4, info);
        } else if (method == ndk::OM_NEW || method == ndk::OM_SET || method == ndk::OM_NOTIFY ||
                   method == ndk::OM_UPDATE) {
            b.w32(msg + 8, info);
        }
        const u32 result = b.ok() ? in.boopsi_.do_method(m.cpu.a[0], msg) : 0;
        in.free_gadget_info(info);
        native::set_result(m, result);
    }

    // The RastPort a custom gadget draws through, its window's own.
    static void obtain_gi_rport(void*, Machine& m) {
        Bus b(m);
        const u32 info = m.cpu.a[0];
        const u32 rp = info != 0 ? b.r32(info + ndk::gi_RastPort) : 0;
        if (b.ok()) {
            native::set_result(m, rp);
        }
    }

    static void release_gi_rport(void*, Machine&) {}

    // The pointer relative to the gadget's box, as two WORDs at A2.
    static void gadget_mouse(void* ctx, Machine& m) {
        Intuition& in = of(ctx);
        Bus b(m);
        const u32 gadget = m.cpu.a[0];
        const u32 window = m.cpu.a[1] != 0 ? b.r32(m.cpu.a[1] + ndk::gi_Window) : 0;
        const WindowView w = load_window(b, window);
        if (!w.ok) {
            return;
        }
        const Rect box = resolve_gadget_box(b, w, gadget);
        b.w16(m.cpu.a[2], static_cast<u32>(in.mouse_x_ - w.left - box.x0));
        b.w16(m.cpu.a[2] + 2, static_cast<u32>(in.mouse_y_ - w.top - box.y0));
    }

    static void refresh_gadgets(void* ctx, Machine& m) {
        of(ctx).refresh_glist(m.cpu.a[0], m.cpu.a[1], -1);
    }

    static void remove_gadget(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).remove_glist(m.cpu.a[0], m.cpu.a[1], 1));
    }

    // The offsets are WORDs: only the low half of D0 and D1 counts.
    static void draw_image(void* ctx, Machine& m) {
        of(ctx).draw_image(m.cpu.a[0], m.cpu.a[1], static_cast<i16>(m.cpu.d[0] & 0xFFFF),
                           static_cast<i16>(m.cpu.d[1] & 0xFFFF));
    }

    static void erase_image(void* ctx, Machine& m) {
        of(ctx).erase_image(m.cpu.a[0], m.cpu.a[1], static_cast<i16>(m.cpu.d[0] & 0xFFFF),
                            static_cast<i16>(m.cpu.d[1] & 0xFFFF));
    }

    static void draw_image_state(void* ctx, Machine& m) {
        of(ctx).draw_image_state(m.cpu.a[0], m.cpu.a[1], static_cast<i16>(m.cpu.d[0] & 0xFFFF),
                                 static_cast<i16>(m.cpu.d[1] & 0xFFFF), m.cpu.d[2], m.cpu.a[2]);
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
    {intuition_lvo::IntuiTextLength, IntuitionNatives::intui_text_length, "IntuiTextLength"},
    {intuition_lvo::InitRequester, IntuitionNatives::init_requester, "InitRequester"},
    {intuition_lvo::DoubleClick, IntuitionNatives::double_click, "DoubleClick"},
    {intuition_lvo::GetDefPrefs, IntuitionNatives::get_def_prefs, "GetDefPrefs"},
    {intuition_lvo::GetPrefs, IntuitionNatives::get_prefs, "GetPrefs"},
    {intuition_lvo::SetPrefs, IntuitionNatives::set_prefs, "SetPrefs"},
    {intuition_lvo::SetIPrefs, IntuitionNatives::set_iprefs, "SetIPrefs"},
    {intuition_lvo::PubScreenStatus, IntuitionNatives::pub_screen_status, "PubScreenStatus"},
    {intuition_lvo::ScrollWindowRaster, IntuitionNatives::scroll_window_raster,
     "ScrollWindowRaster"},
    {intuition_lvo::ActivateGadget, IntuitionNatives::activate_gadget, "ActivateGadget"},
    {intuition_lvo::ClearPointer, IntuitionNatives::keep_pointer, "ClearPointer"},
    {intuition_lvo::SetPointer, IntuitionNatives::keep_pointer, "SetPointer"},
    {intuition_lvo::SetWindowPointerA, IntuitionNatives::keep_pointer, "SetWindowPointerA"},
    {intuition_lvo::Request, IntuitionNatives::request, "Request"},
    {intuition_lvo::EndRequest, IntuitionNatives::end_request, "EndRequest"},
    {intuition_lvo::DrawBorder, IntuitionNatives::draw_border, "DrawBorder"},
    {intuition_lvo::PrintIText, IntuitionNatives::print_itext, "PrintIText"},
    {intuition_lvo::NewObjectA, IntuitionNatives::new_object, "NewObjectA"},
    {intuition_lvo::DisposeObject, IntuitionNatives::dispose_object, "DisposeObject"},
    {intuition_lvo::SetAttrsA, IntuitionNatives::set_attrs, "SetAttrsA"},
    {intuition_lvo::GetAttr, IntuitionNatives::get_attr, "GetAttr"},
    {intuition_lvo::SetGadgetAttrsA, IntuitionNatives::set_gadget_attrs, "SetGadgetAttrsA"},
    {intuition_lvo::NextObject, IntuitionNatives::next_object, "NextObject"},
    {intuition_lvo::MakeClass, IntuitionNatives::make_class, "MakeClass"},
    {intuition_lvo::AddClass, IntuitionNatives::add_class, "AddClass"},
    {intuition_lvo::RemoveClass, IntuitionNatives::remove_class, "RemoveClass"},
    {intuition_lvo::FreeClass, IntuitionNatives::free_class, "FreeClass"},
    {intuition_lvo::DoGadgetMethodA, IntuitionNatives::do_gadget_method, "DoGadgetMethodA"},
    {intuition_lvo::ObtainGIRPort, IntuitionNatives::obtain_gi_rport, "ObtainGIRPort"},
    {intuition_lvo::ReleaseGIRPort, IntuitionNatives::release_gi_rport, "ReleaseGIRPort"},
    {intuition_lvo::GadgetMouse, IntuitionNatives::gadget_mouse, "GadgetMouse"},
    {intuition_lvo::AddGadget, IntuitionNatives::add_gadget, "AddGadget"},
    {intuition_lvo::DrawImage, IntuitionNatives::draw_image, "DrawImage"},
    {intuition_lvo::EraseImage, IntuitionNatives::erase_image, "EraseImage"},
    {intuition_lvo::DrawImageState, IntuitionNatives::draw_image_state, "DrawImageState"},
    {intuition_lvo::CloseScreen, IntuitionNatives::close_screen, "CloseScreen"},
    {intuition_lvo::CloseWorkBench, IntuitionNatives::close_workbench, "CloseWorkBench"},
    {intuition_lvo::CloseWindow, IntuitionNatives::close_window, "CloseWindow"},
    {intuition_lvo::ModifyIDCMP, IntuitionNatives::modify_idcmp, "ModifyIDCMP"},
    {intuition_lvo::ModifyProp, IntuitionNatives::modify_prop, "ModifyProp"},
    {intuition_lvo::OffGadget, IntuitionNatives::off_gadget, "OffGadget"},
    {intuition_lvo::OnGadget, IntuitionNatives::on_gadget, "OnGadget"},
    {intuition_lvo::OpenScreen, IntuitionNatives::open_screen, "OpenScreen"},
    {intuition_lvo::OpenWindow, IntuitionNatives::open_window, "OpenWindow"},
    {intuition_lvo::OpenWorkBench, IntuitionNatives::open_workbench, "OpenWorkBench"},
    {intuition_lvo::RefreshGadgets, IntuitionNatives::refresh_gadgets, "RefreshGadgets"},
    {intuition_lvo::RemoveGadget, IntuitionNatives::remove_gadget, "RemoveGadget"},
    {intuition_lvo::ScreenToBack, IntuitionNatives::screen_to_back, "ScreenToBack"},
    {intuition_lvo::ScreenToFront, IntuitionNatives::screen_to_front, "ScreenToFront"},
    {intuition_lvo::SetWindowTitles, IntuitionNatives::set_window_titles, "SetWindowTitles"},
    {intuition_lvo::WindowToBack, IntuitionNatives::window_to_back, "WindowToBack"},
    {intuition_lvo::WindowToFront, IntuitionNatives::window_to_front, "WindowToFront"},
    {intuition_lvo::WindowLimits, IntuitionNatives::window_limits, "WindowLimits"},
    {intuition_lvo::LockIBase, IntuitionNatives::lock_i_base, "LockIBase"},
    {intuition_lvo::UnlockIBase, IntuitionNatives::unlock_i_base, "UnlockIBase"},
    {intuition_lvo::GetScreenData, IntuitionNatives::get_screen_data, "GetScreenData"},
    {intuition_lvo::RefreshGList, IntuitionNatives::refresh_glist, "RefreshGList"},
    {intuition_lvo::AddGList, IntuitionNatives::add_glist, "AddGList"},
    {intuition_lvo::RemoveGList, IntuitionNatives::remove_glist, "RemoveGList"},
    {intuition_lvo::NewModifyProp, IntuitionNatives::new_modify_prop, "NewModifyProp"},
    {intuition_lvo::LockPubScreen, IntuitionNatives::lock_pub_screen, "LockPubScreen"},
    {intuition_lvo::SetMenuStrip, IntuitionNatives::set_menu_strip, "SetMenuStrip"},
    {intuition_lvo::ResetMenuStrip, IntuitionNatives::set_menu_strip, "ResetMenuStrip"},
    {intuition_lvo::ClearMenuStrip, IntuitionNatives::clear_menu_strip, "ClearMenuStrip"},
    {intuition_lvo::ItemAddress, IntuitionNatives::item_address, "ItemAddress"},
    {intuition_lvo::BeginRefresh, IntuitionNatives::begin_refresh, "BeginRefresh"},
    {intuition_lvo::EndRefresh, IntuitionNatives::end_refresh, "EndRefresh"},
    {intuition_lvo::FreeSysRequest, IntuitionNatives::free_sys_request, "FreeSysRequest"},
    {intuition_lvo::EasyRequestArgs, IntuitionNatives::easy_request_args, "EasyRequestArgs"},
    {intuition_lvo::BuildEasyRequestArgs, IntuitionNatives::build_easy_request_args,
     "BuildEasyRequestArgs"},
    {intuition_lvo::SysReqHandler, IntuitionNatives::sys_req_handler, "SysReqHandler"},
    {intuition_lvo::GetScreenDrawInfo, IntuitionNatives::get_screen_draw_info,
     "GetScreenDrawInfo"},
    {intuition_lvo::FreeScreenDrawInfo, IntuitionNatives::free_screen_draw_info,
     "FreeScreenDrawInfo"},
    {intuition_lvo::UnlockPubScreen, IntuitionNatives::unlock_pub_screen, "UnlockPubScreen"},
    {intuition_lvo::LockPubScreenList, IntuitionNatives::lock_pub_screen_list,
     "LockPubScreenList"},
    {intuition_lvo::UnlockPubScreenList, IntuitionNatives::unlock_pub_screen_list,
     "UnlockPubScreenList"},
    {intuition_lvo::GetDefaultPubScreen, IntuitionNatives::get_default_pub_screen,
     "GetDefaultPubScreen"},
    {intuition_lvo::OpenWindowTagList, IntuitionNatives::open_window_tag_list,
     "OpenWindowTagList"},
    {intuition_lvo::OpenScreenTagList, IntuitionNatives::open_screen_tag_list,
     "OpenScreenTagList"},
};

}

Rect Intuition::close_box_rect(i32 left, i32 top) {
    const Rect box = chrome::close_button_box();
    return Rect{left + box.x0, top + box.y0, left + box.x1, top + box.y1};
}

Rect Intuition::depth_box_rect(i32 left, i32 top, i32 width) {
    const Rect box = chrome::depth_button_box(width);
    return Rect{left + box.x0, top + box.y0, left + box.x1, top + box.y1};
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
    workbench_hooks_ = WorkbenchHooks{};
    workbench_closed_ = false;
    workbench_task_ = 0;
    workbench_hook_task_ = 0;
    pub_screen_list_ = 0;
    reply_port_ = 0;
    active_window_ = 0;
    pressed_gadget_window_ = 0;
    pressed_gadget_ = GadgetKind::None;
    bar_button_lit_ = false;
    drag_ = Drag::None;
    drag_window_ = 0;
    drag_gadget_ = 0;
    icon_drag_ = IconDrag{};
    menu_.active = false;
    menu_.screen = 0;
    menu_.window = 0;
    menu_.strip = 0;
    menu_.title = menu_layout::kNoTitle;
    menu_.item = -1;
    menu_.sub = -1;
    menu_.sub_hover = false;
    menu_.title_count = 0;
    menu_.item_count = 0;
    menu_.sub_count = 0;
    menu_.overlay = Rect{0, 0, -1, -1};
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
    for (u32& update : message_updates_) {
        update = 0;
    }
    for (u32& bytes : message_update_bytes_) {
        bytes = 0;
    }
    for (ScreenRecord& rec : screen_records_) {
        rec = ScreenRecord{};
    }
    for (WindowRecord& rec : window_records_) {
        rec = WindowRecord{};
    }
    for (Requester& req : requesters_) {
        req = Requester{};
    }
    for (GadgetHookEntry& entry : gadget_hooks_) {
        entry = GadgetHookEntry{};
    }
    for (bool& set : palette_prefs_set_) {
        set = false;
    }
    for (u32& rgb : palette_prefs_) {
        rgb = 0;
    }
    for (PenPrefs& prefs : pen_prefs_) {
        prefs = PenPrefs{};
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
    if (!boopsi_.init(m, exec)) {
        return false;
    }
    boopsi_.set_idcmp_update_hook(notify_idcmp_update, this);
    boopsi_.set_sys_image_renderer(draw_sys_image, this);
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

void Intuition::set_workbench_hooks(const WorkbenchHooks& hooks) {
    workbench_hooks_ = hooks;
    workbench_task_ = exec_ != nullptr ? exec_->this_task() : 0;
}

void Intuition::set_default_screen(u32 screen) {
    default_screen_ = screen;
    if (screen == 0) {
        return;
    }
    ScreenRecord* rec = find_screen_record(screen);
    if (rec == nullptr) {
        return;
    }
    apply_palette_prefs(*rec);
    apply_pen_prefs(screen);
    update_chrome_palette(screen);
    flush_palette();
}

void Intuition::apply_pen_prefs(u32 screen) {
    ScreenRecord* rec = find_screen_record(screen);
    if (rec == nullptr || rec->draw_info == 0 || machine_ == nullptr) {
        return;
    }
    const u16* pens = pen_prefs_for(rec->depth);
    if (pens == nullptr) {
        return;
    }
    Bus b(*machine_);
    for (u32 i = 0; i < kDriPenCount; ++i) {
        b.w16(rec->draw_info + kDrawInfoPensAt + i * 2, pens[i]);
    }
    if (default_candidate(screen)) {
        update_chrome_palette(screen);
    }
}

bool Intuition::default_candidate(u32 screen) const {
    return screen == default_screen_ || (default_screen_ == 0 && screen == front_);
}

const u16* Intuition::pen_prefs_for(u32 depth) const {
    const PenPrefs& prefs = pen_prefs_[depth > 2 ? 1 : 0];
    return prefs.set ? prefs.pen : nullptr;
}

// Type 8: records of pen, red, green and blue, each 16 bits, ended by
// 0xFFFF. The high byte of a colour is its 8 bits.
void Intuition::store_palette_prefs(u32 prefs, u32 size) {
    if (prefs == 0 || machine_ == nullptr) {
        return;
    }
    Bus b(*machine_);
    u32 at = prefs;
    u32 used = 0;
    u32 entries = 0;
    while (used + 2 <= size && entries < 256 && b.ok()) {
        const u32 pen = b.r16(at);
        if (!b.ok() || pen == 0xFFFF) {
            break;
        }
        if (used + 8 > size) {
            break;
        }
        const u32 rgb = ((b.r16(at + 2) >> 8) << 16) | ((b.r16(at + 4) >> 8) << 8) |
                        (b.r16(at + 6) >> 8);
        if (!b.ok()) {
            break;
        }
        if (pen < 256) {
            palette_prefs_[pen] = rgb;
            palette_prefs_set_[pen] = true;
        }
        at += 8;
        used += 8;
        ++entries;
    }
}

// Type 9: a count, a kind (0 for screens of four colours or fewer, 1 for
// the rest), four unused bytes and the pens, ended by 0xFFFF.
void Intuition::store_pen_prefs(u32 prefs, u32 size) {
    if (prefs == 0 || machine_ == nullptr || size < 8) {
        return;
    }
    Bus b(*machine_);
    const u32 count = b.r16(prefs);
    const u32 kind = b.r16(prefs + 2);
    if (!b.ok() || kind > 1) {
        return;
    }
    u32 n = count < kDriPenCount ? count : kDriPenCount;
    const u32 fit = (size - 8) / 2;
    if (n > fit) {
        n = fit;
    }
    PenPrefs& stored = pen_prefs_[kind];
    for (u32 i = 0; i < kDriPenCount; ++i) {
        stored.pen[i] = kDefaultDriPens[i];
    }
    for (u32 i = 0; i < n; ++i) {
        const u32 pen = b.r16(prefs + 8 + i * 2);
        if (!b.ok() || pen == 0xFFFF) {
            break;
        }
        // PEN_C3 to PEN_C0 name the palette's colours 4 to 7, which this
        // screen keeps in pens 4 to 7: its top pens are the chrome's.
        constexpr u32 kPenC3 = 0xFEFC;
        constexpr u32 kPenC0 = 0xFEFF;
        if (pen >= kPenC3 && pen <= kPenC0) {
            stored.pen[i] = static_cast<u16>(4 + (pen - kPenC3));
        } else if (pen < 256) {
            stored.pen[i] = static_cast<u16>(pen);
        }
    }
    stored.set = true;
}

void Intuition::write_colormap_rgb(ScreenRecord& rec, u32 pen, u32 rgb) {
    if (machine_ == nullptr || rec.colors == 0 || rec.depth == 0 ||
        pen >= (1u << rec.depth)) {
        return;
    }
    Bus b(*machine_);
    const u32 red = (rgb >> 16) & 0xFF;
    const u32 green = (rgb >> 8) & 0xFF;
    const u32 blue = rgb & 0xFF;
    b.w16(rec.colors + pen * 2, ((red >> 4) << 8) | ((green >> 4) << 4) | (blue >> 4));
    if (rec.low_colors != 0) {
        b.w16(rec.low_colors + pen * 2,
              ((red & 0xF) << 8) | ((green & 0xF) << 4) | (blue & 0xF));
    }
}

void Intuition::apply_palette_prefs(ScreenRecord& rec) {
    bool changed = false;
    for (u32 pen = 0; pen < 256; ++pen) {
        if (!palette_prefs_set_[pen]) {
            continue;
        }
        write_colormap_rgb(rec, pen, palette_prefs_[pen]);
        changed = true;
    }
    if (!changed || display_ == nullptr) {
        return;
    }
    ++display_->palette_gen;
    if (display_->front_screen == rec.addr) {
        display_->palette_dirty = true;
    }
}

void Intuition::update_chrome_palette(u32 screen) {
    ScreenRecord* rec = find_screen_record(screen);
    if (rec == nullptr || rec->depth < 8 || machine_ == nullptr || graphics_ == nullptr) {
        return;
    }
    const u32 info = screen_draw_info(screen);
    if (info == 0) {
        return;
    }
    Bus b(*machine_);
    const u32 pens = b.r32(info + kDriPens);
    if (!b.ok() || pens == 0) {
        return;
    }
    u32 pen_rgb[kDriPenCount]{};
    for (u32 i = 0; i < kDriPenCount; ++i) {
        // A pen the ColorMap does not have keeps the colour of the default
        // pen in its place.
        u32 pen = b.r16(pens + i * 2);
        u8 red = 0;
        u8 green = 0;
        u8 blue = 0;
        if (!b.ok()) {
            return;
        }
        if (!graphics_->color_of(rec->colormap, pen, red, green, blue)) {
            pen = kDefaultDriPens[i];
            if (!graphics_->color_of(rec->colormap, pen, red, green, blue)) {
                continue;
            }
        }
        pen_rgb[i] = (static_cast<u32>(red) << 16) | (static_cast<u32>(green) << 8) | blue;
    }
    u32 palette[chrome::kDerivedCount]{};
    chrome::palette_from_pens(pen_rgb, palette);
    for (u32 i = 0; i < chrome::kDerivedCount; ++i) {
        write_colormap_rgb(*rec, chrome::kDerivedBase + i, palette[i]);
    }
    if (display_ != nullptr) {
        ++display_->palette_gen;
        if (display_->front_screen == rec->addr) {
            display_->palette_dirty = true;
        }
    }
    flush_palette();
}

void Intuition::reset_screen(u32 screen) {
    ScreenRecord* rec = find_screen_record(screen);
    if (rec == nullptr || machine_ == nullptr || graphics_ == nullptr) {
        return;
    }
    u32 background = 0;
    const u32 info = screen_draw_info(screen);
    if (info != 0) {
        Bus b(*machine_);
        const u32 pens = b.r32(info + kDriPens);
        if (b.ok() && pens != 0) {
            background = b.r16(pens + kDriBackgroundPen * 2) & 0xFF;
        }
    }
    Bus b(*machine_);
    const i32 right = static_cast<i32>(rec->width) - 1;
    const i32 bottom = static_cast<i32>(rec->height) - 1;
    begin_batch();
    graphics_->set_pens(rec->backdrop_rp, background, background, ndk::JAM2);
    graphics_->rect_fill(rec->backdrop_rp, 0, 0, right, bottom);
    chrome::draw_screen_bar(*graphics_, b, rec->backdrop_rp, rec->width, rec->title);
    compose(screen, Rect{0, 0, right, bottom});
    end_batch();
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
    if (b.ok() && icon_drag_.image != 0 && icon_drag_.screen == screen) {
        raster::RastPortView view{};
        view.bitmap = dst;
        view.clip = raster::Clip{r.x0, r.y0, r.x1, r.y1};
        view.ok = true;
        draw_image_view(b, view, icon_drag_.image, icon_drag_.x, icon_drag_.y, false, false);
    }
    if (b.ok() && menu_.active && menu_.screen == screen) {
        draw_menu_overlay(b, dst, r);
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
    b.w8(screen + ndk::sc_WBorTop, chrome::kWBorTop);
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
    // The first screen is the one that becomes the default, so it takes the
    // palette IPrefs sent before it opened; other screens keep the default.
    const bool becomes_default = default_screen_ == 0;
    for (u32 i = 0; i < color_count; ++i) {
        const u32 rgb = becomes_default && palette_prefs_set_[i] ? palette_prefs_[i]
                                                                 : default_palette_rgb24(i);
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
    if (becomes_default && a.depth >= 8) {
        update_chrome_palette(screen);
    }
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
    if (rec->draw_info != 0) {
        exec_->free(rec->draw_info, kDrawInfoBytes);
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
            free_message_update(i);
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

void Intuition::free_message_update(u32 index) {
    if (message_updates_[index] != 0) {
        exec_->free(message_updates_[index], message_update_bytes_[index]);
        message_updates_[index] = 0;
        message_update_bytes_[index] = 0;
    }
}

void Intuition::notify_idcmp_update(void* ctx, u32 window, const u32* tags, u32 count,
                                    u32 flags) {
    (void)flags;
    static_cast<Intuition*>(ctx)->post_idcmp_update(window, tags, count);
}

// Puts a copy of the notification's attributes in guest memory as the
// IAddress of the window's IDCMP_IDCMPUPDATE, which frees it with the
// message; a message the window asks for but cannot get frees it at once.
void Intuition::post_idcmp_update(u32 window, const u32* tags, u32 count) {
    if (exec_ == nullptr || machine_ == nullptr) {
        return;
    }
    const u32 bytes = (count + 1) * ndk::ti_SIZEOF;
    const u32 list = exec_->alloc(bytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (list == 0) {
        return;
    }
    Bus b(*machine_);
    for (u32 i = 0; i < count; ++i) {
        b.w32(list + i * ndk::ti_SIZEOF, tags[i * 2]);
        b.w32(list + i * ndk::ti_SIZEOF + ndk::ti_Data, tags[i * 2 + 1]);
    }
    b.w32(list + count * ndk::ti_SIZEOF, ndk::TAG_DONE);
    if (!b.ok() || !post_intuimessage(window, ndk::IDCMP_IDCMPUPDATE, 0, list, bytes)) {
        exec_->free(list, bytes);
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

bool Intuition::post_intuimessage(u32 window, u32 idcmp_class, u16 code, u32 iaddress,
                                  u32 iaddress_bytes) {
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
    if (iaddress_bytes != 0) {
        message_updates_[slot] = iaddress;
        message_update_bytes_[slot] = iaddress_bytes;
    }
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
    chrome::draw_window_bar(*graphics_, b, rec->rp,
                            to_chrome(w, active_window_ == window, held_bar_button(window)));
    const i32 bar_bottom = static_cast<i32>(chrome::kBarHeight);
    const i32 y1 = rec->height - 1 < bar_bottom ? rec->height - 1 : bar_bottom;
    if (rec->width > 0 && y1 >= 0) {
        compose(rec->screen,
                Rect{rec->left, rec->top, rec->left + rec->width - 1, rec->top + y1});
    }
}

chrome::BarButton Intuition::held_bar_button(u32 window) const {
    if (!bar_button_lit_ || window == 0 || window != pressed_gadget_window_) {
        return chrome::BarButton::None;
    }
    if (pressed_gadget_ == GadgetKind::Close) {
        return chrome::BarButton::Close;
    }
    return pressed_gadget_ == GadgetKind::Depth ? chrome::BarButton::Depth
                                                : chrome::BarButton::None;
}

void Intuition::light_bar_button(bool lit) {
    if (lit == bar_button_lit_) {
        return;
    }
    bar_button_lit_ = lit;
    redraw_window_bar(pressed_gadget_window_);
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
    // WA_InnerWidth and WA_InnerHeight size the window by what lies inside
    // its borders.
    if (a.inner_width != 0) {
        a.width = a.inner_width + border_left + border_right;
    }
    if (a.inner_height != 0) {
        a.height = a.inner_height + border_top + border_bottom;
    }
    // A window asked for over the screen's title bar opens below it, as
    // far down as its height still fits; a backdrop stays where it asked.
    const i32 bar = static_cast<i32>(b.r8(screen + ndk::sc_BarHeight));
    if ((a.flags & ndk::WFLG_BACKDROP) == 0 && top < bar) {
        const i32 fits = a.height == 0 ? bar : screen_h - static_cast<i32>(a.height);
        const i32 lowest = fits < bar ? fits : bar;
        top = lowest > top ? lowest : top;
    }
    const u32 room_x = static_cast<u32>(screen_w - left);
    const u32 room_y = static_cast<u32>(screen_h - top);
    const i32 width =
        a.width == 0 || a.width > room_x ? static_cast<i32>(room_x) : static_cast<i32>(a.width);
    const i32 height =
        a.height == 0 || a.height > room_y ? static_cast<i32>(room_y) : static_cast<i32>(a.height);

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
    // The Workbench's windows stay the Workbench's even when IPrefs' task
    // runs the hook that opens them.
    rec->owner = workbench_hook_task_ != 0 ? workbench_hook_task_ : exec_->this_task();
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
    chrome::draw_window_chrome(*graphics_, b, rp,
                               to_chrome(w, active_window_ == window, held_bar_button(window)));
    if (a.gadgets != 0) {
        draw_gadget_list(b, window, a.gadgets, kMaxGadgets);
    }
    compose(screen, window_damage_rect(srec->depth, left, top, width, height, a.flags));
    end_batch();
    char name[41];
    name[0] = '\0';
    const u32 title_at = b.r32(window + ndk::wd_Title);
    for (u32 i = 0; title_at != 0 && i + 1 < sizeof name; ++i) {
        const u32 c = b.r8(title_at + i);
        name[i] = c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '\0';
        name[i + 1] = '\0';
        if (c == 0 || !b.ok()) {
            name[i] = '\0';
            break;
        }
    }
    LogLine().text("intuition: window ").text(name).text(" opened").send();
    return window;
}

void Intuition::close_window(u32 window) {
    const u32 index = find_window(window);
    if (index >= kMaxWindows) {
        return;
    }
    drop_requester(window);
    if (custom_.window == window) {
        drop_custom_gadget();
    }
    if (string_.window == window) {
        string_ = ActiveString{};
    }
    for (Block& block : blocks_) {
        if (block.window == window) {
            block = Block{};
        }
    }
    if (menu_.active && menu_.window == window) {
        leave_menu_mode();
    }
    if (active_window_ == window) {
        active_window_ = 0;
    }
    if (pressed_gadget_window_ == window) {
        pressed_gadget_window_ = 0;
        pressed_gadget_ = GadgetKind::None;
        bar_button_lit_ = false;
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

Intuition::Requester* Intuition::find_requester(u32 window) {
    if (window == 0) {
        return nullptr;
    }
    for (Requester& req : requesters_) {
        if (req.window == window) {
            return &req;
        }
    }
    return nullptr;
}

const Intuition::Requester* Intuition::find_requester(u32 window) const {
    if (window == 0) {
        return nullptr;
    }
    for (const Requester& req : requesters_) {
        if (req.window == window) {
            return &req;
        }
    }
    return nullptr;
}

Intuition::Requester* Intuition::free_requester() {
    for (Requester& req : requesters_) {
        if (req.window == 0) {
            return &req;
        }
    }
    return nullptr;
}

void Intuition::drop_requester(u32 window) {
    Requester* req = find_requester(window);
    if (req != nullptr) {
        *req = Requester{};
    }
}

u32 Intuition::build_easy_request(u32 window, u32 easy_struct, u32 idcmp, u32 args) {
    if (machine_ == nullptr || exec_ == nullptr || graphics_ == nullptr) {
        return 0;
    }
    Requester* slot = free_requester();
    if (slot == nullptr) {
        LogLine().text("intuition: requester limit reached").send();
        return 0;
    }
    Bus b(*machine_);
    const u32 easy_title = b.r32(easy_struct + ndk::es_Title);
    const u32 text_format = b.r32(easy_struct + ndk::es_TextFormat);
    const u32 gadget_format = b.r32(easy_struct + ndk::es_GadgetFormat);
    if (!b.ok()) {
        return 0;
    }
    u32 screen = 0;
    if (window != 0 && window_open(window)) {
        screen = b.r32(window + ndk::wd_WScreen);
    }
    if (screen == 0) {
        screen = default_screen_ != 0 ? default_screen_ : front_;
    }
    if (screen == 0 || find_screen(screen) >= kMaxScreens) {
        LogLine().text("intuition: requester without a screen").send();
        return 0;
    }
    const i32 screen_w = static_cast<i32>(b.r16(screen + ndk::sc_Width));
    const i32 screen_h = static_cast<i32>(b.r16(screen + ndk::sc_Height));
    if (!b.ok() || screen_w <= 0 || screen_h <= 0) {
        return 0;
    }
    if (gadget_format == 0) {
        LogLine().text("intuition: requester without gadgets").send();
        return 0;
    }

    char text[kMaxRequesterText];
    char gadgets[kMaxRequesterGadgetText];
    u32 stream = args;
    if (!format_requester_text(b, text_format, stream, text, sizeof text) ||
        !format_requester_text(b, gadget_format, stream, gadgets, sizeof gadgets)) {
        return 0;
    }

    Requester req;
    req.idcmp = idcmp;
    bool too_many_buttons = false;
    char* at = gadgets;
    for (;;) {
        char* end = at;
        while (*end != '\0' && *end != '|') {
            ++end;
        }
        if (req.button_count < kMaxRequesterButtons) {
            u32 length = static_cast<u32>(end - at);
            if (length >= kMaxRequesterLabel) {
                length = kMaxRequesterLabel - 1;
            }
            for (u32 i = 0; i < length; ++i) {
                req.labels[req.button_count][i] = at[i];
            }
            req.labels[req.button_count][length] = '\0';
            req.label_length[req.button_count] = static_cast<u8>(length);
            ++req.button_count;
        } else {
            too_many_buttons = true;
        }
        if (*end == '\0') {
            break;
        }
        at = end + 1;
    }
    if (too_many_buttons) {
        LogLine().text("intuition: requester has more than ").dec(kMaxRequesterButtons)
            .text(" buttons").send();
    }

    RequesterLine lines[kMaxRequesterLines];
    const u32 line_count = split_requester_lines(text, lines, kMaxRequesterLines);
    u32 max_chars = 0;
    for (u32 i = 0; i < line_count; ++i) {
        max_chars = lines[i].length > max_chars ? lines[i].length : max_chars;
    }
    u32 widest = 0;
    for (u32 i = 0; i < req.button_count; ++i) {
        widest = req.label_length[i] > widest ? req.label_length[i] : widest;
    }

    const i32 button_w = static_cast<i32>(widest) * 8 + kRequesterButtonPad;
    const i32 button_row_w =
        req.button_count <= 1
            ? button_w
            : static_cast<i32>(req.button_count) * button_w +
                  static_cast<i32>(req.button_count - 1) * kRequesterButtonGap;
    const i32 text_box_w = static_cast<i32>(max_chars) * 8 + 2 * kRequesterTextPad;
    const i32 text_box_h = static_cast<i32>(line_count) * 8 + 2 * kRequesterTextPad;
    const i32 content_w = larger(text_box_w, button_row_w);
    const i32 content_h = text_box_h + kRequesterGap + kRequesterButtonHeight;
    i32 width = content_w + 2 * kRequesterMargin + static_cast<i32>(chrome::kBorderLeft) +
                static_cast<i32>(chrome::kBorderRight);
    i32 height = content_h + 2 * kRequesterMargin + static_cast<i32>(chrome::kBorderTop) +
                 static_cast<i32>(chrome::kBorderBottom);
    width = smaller(width, screen_w);
    height = smaller(height, screen_h);
    const i32 left = (screen_w - width) / 2;
    const i32 top = (screen_h - height) / 2;

    u32 title = easy_title;
    if (title == 0 && window != 0 && window_open(window)) {
        title = b.r32(window + ndk::wd_Title);
    }
    const bool literal_title = title == 0;
    const u32 title_bytes = literal_title ? sizeof kSystemRequestTitle : 0;
    const u32 new_window_bytes = ndk::NewWindow_SIZEOF + title_bytes;
    const u32 new_window = exec_->alloc(new_window_bytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (new_window == 0) {
        LogLine().text("intuition: no memory for a requester").send();
        return 0;
    }
    if (literal_title) {
        title = new_window + ndk::NewWindow_SIZEOF;
        for (u32 i = 0; i < sizeof kSystemRequestTitle; ++i) {
            b.w8(title + i, static_cast<u8>(kSystemRequestTitle[i]));
        }
    }
    b.w16(new_window + ndk::nw_LeftEdge, static_cast<u32>(left));
    b.w16(new_window + ndk::nw_TopEdge, static_cast<u32>(top));
    b.w16(new_window + ndk::nw_Width, static_cast<u32>(width));
    b.w16(new_window + ndk::nw_Height, static_cast<u32>(height));
    b.w8(new_window + ndk::nw_DetailPen, 0);
    b.w8(new_window + ndk::nw_BlockPen, 1);
    b.w32(new_window + ndk::nw_IDCMPFlags, ndk::IDCMP_MOUSEBUTTONS | idcmp);
    b.w32(new_window + ndk::nw_Flags, ndk::WFLG_DRAGBAR | ndk::WFLG_DEPTHGADGET |
                                          ndk::WFLG_ACTIVATE | ndk::WFLG_RMBTRAP);
    b.w32(new_window + ndk::nw_Title, title);
    b.w32(new_window + ndk::nw_Screen, screen);
    b.w16(new_window + ndk::nw_Type, ndk::CUSTOMSCREEN);
    const u32 requester = b.ok() ? open_window(new_window, 0) : 0;
    exec_->free(new_window, new_window_bytes);
    if (!b.ok()) {
        return 0;
    }
    if (requester == 0) {
        LogLine().text("intuition: could not open the requester").send();
        return 0;
    }

    const i32 req_w = static_cast<i32>(b.r16(requester + ndk::wd_Width));
    const i32 req_h = static_cast<i32>(b.r16(requester + ndk::wd_Height));
    const i32 inner_x0 = static_cast<i32>(chrome::kBorderLeft) + kRequesterMargin;
    const i32 inner_x1 =
        req_w - static_cast<i32>(chrome::kBorderRight) - 1 - kRequesterMargin;
    const i32 inner_y0 = static_cast<i32>(chrome::kBorderTop) + kRequesterMargin;
    const i32 inner_y1 =
        req_h - static_cast<i32>(chrome::kBorderBottom) - 1 - kRequesterMargin;
    const i32 button_y1 = inner_y1;
    const i32 button_y0 = button_y1 - kRequesterButtonHeight + 1;
    const i32 span = larger(0, inner_x1 - inner_x0 + 1);
    const i32 last_x = larger(inner_x0, inner_x1 - button_w + 1);
    for (u32 i = 0; i < req.button_count; ++i) {
        i32 x = inner_x0;
        if (req.button_count == 1) {
            x = span > button_w ? inner_x0 + (span - button_w) / 2 : inner_x0;
        } else {
            x = inner_x0 + (last_x - inner_x0) * static_cast<i32>(i) /
                               static_cast<i32>(req.button_count - 1);
        }
        req.buttons[i] = Rect{x, button_y0, x + button_w - 1, button_y1};
    }
    req.window = requester;
    const Rect text_box{inner_x0, inner_y0,
                        smaller(inner_x0 + text_box_w - 1, inner_x1),
                        smaller(inner_y0 + text_box_h - 1, button_y0 - kRequesterGap - 1)};
    *slot = req;

    const u32 rp = b.r32(requester + ndk::wd_RPort);
    if (b.ok() && rp != 0) {
        const u32 depth = b.r8(b.r32(rp + ndk::rp_BitMap) + ndk::bm_Depth);
        const RequesterPens pens = requester_pens(depth);
        begin_batch();
        if (text_box.x1 >= text_box.x0 && text_box.y1 >= text_box.y0) {
            draw_requester_box(*graphics_, rp, text_box, pens, true);
            const u32 room = static_cast<u32>(
                larger(0, (text_box.x1 - text_box.x0 + 1 - 2 * kRequesterTextPad) / 8));
            i32 baseline = text_box.y0 + kRequesterTextPad + 6;
            for (u32 i = 0; i < line_count; ++i) {
                if (baseline + 1 > text_box.y1) {
                    break;
                }
                draw_requester_text(*graphics_, rp, text_box.x0 + kRequesterTextPad, baseline,
                                    lines[i].text, lines[i].length, room, pens.text);
                baseline += 8;
            }
        }
        for (u32 i = 0; i < req.button_count; ++i) {
            draw_requester_box(*graphics_, rp, req.buttons[i], pens, false);
            draw_requester_label(*graphics_, rp, req.buttons[i], req.labels[i],
                                 req.label_length[i], pens.text);
        }
        end_batch();
    }

    char name[41];
    name[0] = '\0';
    const u32 title_at = b.r32(requester + ndk::wd_Title);
    for (u32 i = 0; title_at != 0 && i + 1 < sizeof name; ++i) {
        const u32 c = b.r8(title_at + i);
        name[i] = c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '\0';
        name[i + 1] = '\0';
        if (c == 0 || !b.ok()) {
            name[i] = '\0';
            break;
        }
    }
    LogLine().text("intuition: requester ").text(name).text(" opened").send();
    return requester;
}

i32 Intuition::sys_req_handler(u32 window, u32 idcmp_ptr, bool wait) {
    Requester* req = find_requester(window);
    if (req == nullptr || machine_ == nullptr || exec_ == nullptr || graphics_ == nullptr) {
        return -2;
    }
    Bus b(*machine_);
    const u32 port = b.r32(window + ndk::wd_UserPort);
    if (!b.ok() || port == 0) {
        return -2;
    }
    for (;;) {
        const u32 message = exec_->get_message(port);
        if (message == 0) {
            if (!wait) {
                return -2;
            }
            const u32 bit = b.r8(port + ndk::mp_SigBit);
            if (!b.ok() || bit >= 32) {
                return -2;
            }
            if (exec_->wait_for(1u << bit, ~u64(0)) == 0) {
                return -2;
            }
            continue;
        }
        const u32 idcmp_class = b.r32(message + ndk::im_Class);
        const u32 code = b.r16(message + ndk::im_Code);
        const i32 mouse_x = static_cast<i16>(b.r16(message + ndk::im_MouseX));
        const i32 mouse_y = static_cast<i16>(b.r16(message + ndk::im_MouseY));
        exec_->reply_message(message);
        if (!b.ok()) {
            return -2;
        }
        if (idcmp_class != ndk::IDCMP_MOUSEBUTTONS && (idcmp_class & req->idcmp) != 0) {
            if (idcmp_ptr != 0) {
                Bus out(*machine_);
                out.w32(idcmp_ptr, idcmp_class);
            }
            return -1;
        }
        if (idcmp_class != ndk::IDCMP_MOUSEBUTTONS) {
            continue;
        }
        i32 hit = -1;
        for (u32 i = 0; i < req->button_count; ++i) {
            const Rect& box = req->buttons[i];
            if (mouse_x >= box.x0 && mouse_x <= box.x1 && mouse_y >= box.y0 &&
                mouse_y <= box.y1) {
                hit = static_cast<i32>(i);
                break;
            }
        }
        if (code == ndk::SELECTDOWN) {
            if (req->pressed >= 0 && req->pressed != hit) {
                draw_requester_button(window, *req, static_cast<u32>(req->pressed), false);
            }
            req->pressed = hit;
            if (hit >= 0) {
                draw_requester_button(window, *req, static_cast<u32>(hit), true);
            }
        } else if (code == ndk::SELECTUP && req->pressed >= 0) {
            const u32 pressed = static_cast<u32>(req->pressed);
            req->pressed = -1;
            if (hit == static_cast<i32>(pressed)) {
                return pressed + 1 == req->button_count ? 0 : static_cast<i32>(pressed + 1);
            }
            draw_requester_button(window, *req, pressed, false);
        }
    }
}

void Intuition::draw_requester_button(u32 window, const Requester& req, u32 index, bool pressed) {
    if (index >= req.button_count || machine_ == nullptr || graphics_ == nullptr) {
        return;
    }
    Bus b(*machine_);
    const u32 rp = b.r32(window + ndk::wd_RPort);
    if (!b.ok() || rp == 0) {
        return;
    }
    const u32 depth = b.r8(b.r32(rp + ndk::rp_BitMap) + ndk::bm_Depth);
    if (!b.ok()) {
        return;
    }
    const RequesterPens pens = requester_pens(depth);
    begin_batch();
    draw_requester_box(*graphics_, rp, req.buttons[index], pens, pressed);
    draw_requester_label(*graphics_, rp, req.buttons[index], req.labels[index],
                         req.label_length[index], pens.text);
    end_batch();
}

void Intuition::free_sys_request(u32 window) {
    if (window == 0 || window == 1 || find_requester(window) == nullptr) {
        return;
    }
    close_window(window);
}

i32 Intuition::easy_request(u32 window, u32 easy_struct, u32 idcmp_ptr, u32 args) {
    if (machine_ == nullptr) {
        return 0;
    }
    Bus b(*machine_);
    // The classes that end the requester early come in through *idcmp_ptr,
    // which also takes the class that ended it.
    const u32 idcmp = idcmp_ptr != 0 ? b.r32(idcmp_ptr) : 0;
    const u32 requester = b.ok() ? build_easy_request(window, easy_struct, idcmp, args) : 0;
    if (requester == 0) {
        return 0;
    }
    i32 result = -2;
    while (result == -2 && machine_->cpu.exit == m68k::ExitReason::None) {
        result = sys_req_handler(requester, idcmp_ptr, true);
    }
    free_sys_request(requester);
    return result;
}

void Intuition::set_menu_strip(u32 window, u32 menu) {
    if (window == 0 || machine_ == nullptr) {
        return;
    }
    Bus b(*machine_);
    b.w32(window + kWdMenuStrip, menu);
    if (b.ok() && menu_.active && menu_.window == window) {
        leave_menu_mode();
    }
}

void Intuition::clear_menu_strip(u32 window) {
    if (window == 0 || machine_ == nullptr) {
        return;
    }
    Bus b(*machine_);
    b.w32(window + kWdMenuStrip, 0);
    if (b.ok() && menu_.active && menu_.window == window) {
        leave_menu_mode();
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
            chrome::draw_window_bar(
                *graphics_, b, rec->rp,
                to_chrome(w, active_window_ == window, held_bar_button(window)));
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

// WindowLimits: 0 leaves a limit alone, a limit the window already breaks
// is left out, and the ones set are kept inside the screen.
bool Intuition::window_limits(u32 window, i32 width_min, i32 height_min, u32 width_max,
                              u32 height_max) {
    const u32 index = find_window(window);
    WindowRecord* rec = index < kMaxWindows ? find_window_record(window) : nullptr;
    const ScreenRecord* srec = rec != nullptr ? find_screen_record(rec->screen) : nullptr;
    if (srec == nullptr) {
        return false;
    }
    const i32 screen_w = static_cast<i32>(srec->width);
    const i32 screen_h = static_cast<i32>(srec->height);
    bool applied = true;
    if (width_min != 0) {
        if (width_min > rec->width) {
            applied = false;
        } else {
            rec->min_w = width_min < 0 ? 0 : width_min > screen_w ? screen_w : width_min;
        }
    }
    if (height_min != 0) {
        if (height_min > rec->height) {
            applied = false;
        } else {
            rec->min_h = height_min < 0 ? 0 : height_min > screen_h ? screen_h : height_min;
        }
    }
    if (width_max != 0) {
        if (static_cast<i32>(width_max) < rec->width) {
            applied = false;
        } else {
            rec->lim_w = width_max >= 0xFFFF ? screen_w
                         : static_cast<i32>(width_max) > screen_w ? screen_w
                                                                  : static_cast<i32>(width_max);
        }
    }
    if (height_max != 0) {
        if (static_cast<i32>(height_max) < rec->height) {
            applied = false;
        } else {
            rec->lim_h = height_max >= 0xFFFF ? screen_h
                         : static_cast<i32>(height_max) > screen_h ? screen_h
                                                                   : static_cast<i32>(height_max);
        }
    }
    Bus b(*machine_);
    b.w16(window + ndk::wd_MinWidth, static_cast<u32>(rec->min_w));
    b.w16(window + ndk::wd_MinHeight, static_cast<u32>(rec->min_h));
    b.w16(window + ndk::wd_MaxWidth, static_cast<u32>(rec->lim_w));
    b.w16(window + ndk::wd_MaxHeight, static_cast<u32>(rec->lim_h));
    return applied;
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

bool Intuition::menu_strip_ready(Bus& b) const {
    if (active_window_ == 0) {
        return false;
    }
    const WindowRecord* rec = find_window_record(active_window_);
    if (rec == nullptr || rec->screen != front_) {
        return false;
    }
    if ((b.r32(active_window_ + ndk::wd_Flags) & ndk::WFLG_RMBTRAP) != 0) {
        return false;
    }
    return b.r32(active_window_ + kWdMenuStrip) != 0 && b.ok();
}

void Intuition::reload_menu(Bus& b) {
    MenuMode& m = menu_;
    m.title_count = read_menu_titles(b, m.strip, m.titles, menu_layout::kMaxTitles);
    m.item_count = 0;
    m.sub_count = 0;
    if (m.title < m.title_count) {
        m.item_count =
            read_menu_items(b, m.titles[m.title].first, m.items, menu_layout::kMaxItems);
        if (m.item >= 0 && static_cast<u32>(m.item) < m.item_count &&
            m.items[m.item].sub != 0) {
            m.sub_count =
                read_menu_items(b, m.items[m.item].sub, m.subs, menu_layout::kMaxSubItems);
        }
    }
    if (m.item >= static_cast<i32>(m.item_count)) {
        m.item = -1;
    }
    if (m.sub >= static_cast<i32>(m.sub_count)) {
        m.sub = -1;
    }
    const ScreenRecord* srec = find_screen_record(m.screen);
    const i32 screen_width = srec != nullptr ? static_cast<i32>(srec->width) : 0;
    m.geometry = menu_layout::compute(
        m.titles, m.title_count, m.items, m.item_count, m.subs, m.sub_count,
        static_cast<i32>(chrome::kBarHeight) + 1, m.title, m.item, screen_width);
}

Rect Intuition::menu_overlay(const ScreenRecord& srec) const {
    Rect r{0, 0, static_cast<i32>(srec.width) - 1, static_cast<i32>(chrome::kBarHeight)};
    Rect box = menu_.item_count != 0 ? menu_.geometry.box : menu_layout::empty_rect();
    Rect sub = menu_.sub_count != 0 ? menu_.geometry.sub_box : menu_layout::empty_rect();
    if (srec.depth >= 8) {
        if (menu_layout::valid(box)) {
            box.x1 += kShadowSize;
            box.y1 += kShadowSize;
        }
        if (menu_layout::valid(sub)) {
            sub.x1 += kShadowSize;
            sub.y1 += kShadowSize;
        }
    }
    if (menu_layout::valid(box)) {
        r = unite_rect(r, box);
    }
    if (menu_layout::valid(sub)) {
        r = unite_rect(r, sub);
    }
    return r;
}

bool Intuition::enter_menu_mode(Bus& b, i32 x, i32 y) {
    if (!menu_strip_ready(b)) {
        return false;
    }
    const WindowRecord* rec = find_window_record(active_window_);
    if (rec == nullptr) {
        return false;
    }
    MenuMode& m = menu_;
    m.active = true;
    m.screen = rec->screen;
    m.window = active_window_;
    m.strip = b.r32(active_window_ + kWdMenuStrip);
    m.title = menu_layout::kNoTitle;
    m.item = -1;
    m.sub = -1;
    m.sub_hover = false;
    m.overlay = Rect{0, 0, -1, -1};
    reload_menu(b);
    hover_menu(b, x, y);
    return true;
}

void Intuition::leave_menu_mode() {
    if (!menu_.active) {
        return;
    }
    const Rect old = menu_.overlay;
    menu_.active = false;
    menu_.overlay = Rect{0, 0, -1, -1};
    if (old.x1 >= old.x0 && old.y1 >= old.y0) {
        compose(menu_.screen, old);
    }
}

void Intuition::hover_menu(Bus& b, i32 x, i32 y) {
    MenuMode& m = menu_;
    if (!m.active) {
        return;
    }
    const ScreenRecord* srec = find_screen_record(m.screen);
    if (srec == nullptr) {
        leave_menu_mode();
        return;
    }
    const Rect old_overlay = m.overlay;
    const u32 old_title = m.title;
    const i32 old_item = m.item;
    const i32 old_sub = m.sub;

    const i32 title_hit = menu_layout::hit(m.geometry.title, m.title_count, x, y);
    const bool title_on = title_hit >= 0 && m.titles[title_hit].enabled();
    if (title_on && static_cast<u32>(title_hit) != m.title) {
        m.title = static_cast<u32>(title_hit);
        m.item = -1;
        m.sub = -1;
        reload_menu(b);
    }

    m.sub_hover = false;
    if (m.title < m.title_count) {
        if (m.sub_count != 0 && in_rect(m.geometry.sub_box, x, y)) {
            m.sub_hover = true;
            const i32 hit = menu_layout::hit(m.geometry.sub_item, m.sub_count, x, y);
            m.sub = hit >= 0 && m.subs[hit].enabled() ? hit : -1;
        } else if (in_rect(m.geometry.box, x, y)) {
            const i32 hit = menu_layout::hit(m.geometry.item, m.item_count, x, y);
            const i32 item = hit >= 0 && m.items[hit].enabled() ? hit : -1;
            if (item != m.item) {
                m.item = item;
                m.sub = -1;
                reload_menu(b);
            }
            m.sub = -1;
        } else {
            if (m.item != -1 || m.sub != -1) {
                m.item = -1;
                m.sub = -1;
                reload_menu(b);
            }
        }
    } else {
        m.item = -1;
        m.sub = -1;
    }

    const Rect new_overlay = menu_overlay(*srec);
    if (m.title != old_title || m.item != old_item ||
        m.sub != old_sub || new_overlay.x0 != old_overlay.x0 ||
        new_overlay.y0 != old_overlay.y0 || new_overlay.x1 != old_overlay.x1 ||
        new_overlay.y1 != old_overlay.y1) {
        compose(m.screen, unite_rect(old_overlay, new_overlay));
        m.overlay = new_overlay;
    }
}

void Intuition::clear_menu_checked(Bus& b, const menu_layout::Item* items, u32 count, u32 mask) {
    for (u32 i = 0; i < count && i < 32; ++i) {
        if ((mask & (1u << i)) == 0 || items[i].addr == 0 ||
            (items[i].flags & menu_layout::kItemCheckIt) == 0) {
            continue;
        }
        const u16 flags = static_cast<u16>(b.r16(items[i].addr + kMiFlags));
        if ((flags & menu_layout::kItemChecked) != 0) {
            b.w16(items[i].addr + kMiFlags,
                  static_cast<u16>(flags & ~menu_layout::kItemChecked));
        }
    }
}

void Intuition::release_menu(Bus& b, i32 x, i32 y) {
    MenuMode& m = menu_;
    if (!m.active) {
        return;
    }
    hover_menu(b, x, y);
    u32 item_addr = 0;
    u16 code = kMenuNull;
    const menu_layout::Item* level = m.items;
    u32 level_count = m.item_count;
    if (m.sub_hover && m.sub >= 0 && static_cast<u32>(m.sub) < m.sub_count) {
        item_addr = m.subs[m.sub].addr;
        code = static_cast<u16>(m.title | (static_cast<u32>(m.item) << 5) |
                                (static_cast<u32>(m.sub) << 11));
        level = m.subs;
        level_count = m.sub_count;
    } else if (!m.sub_hover && m.item >= 0 && static_cast<u32>(m.item) < m.item_count) {
        item_addr = m.items[m.item].addr;
        code = static_cast<u16>(m.title | (static_cast<u32>(m.item) << 5) | (31u << 11));
    }
    const bool picked =
        item_addr != 0 && m.title < m.title_count && m.titles[m.title].enabled();
    if (picked) {
        u16 flags = static_cast<u16>(b.r16(item_addr + kMiFlags));
        if ((flags & menu_layout::kItemCheckIt) != 0) {
            if ((flags & menu_layout::kItemMenuToggle) != 0) {
                flags ^= menu_layout::kItemChecked;
            } else {
                flags |= menu_layout::kItemChecked;
            }
            b.w16(item_addr + kMiFlags, flags);
        }
        clear_menu_checked(b, level, level_count, b.r32(item_addr + kMiMutualExclude));
        b.w16(item_addr + kMiNextSelect, kMenuNull);
        post_intuimessage(m.window, kIdcmpMenuPick, code);
    } else {
        post_intuimessage(m.window, kIdcmpMenuPick, kMenuNull);
    }
    leave_menu_mode();
}

void Intuition::draw_menu_overlay(Bus& b, raster::Bitmap& bitmap, const Rect& clip) {
    const ScreenRecord* srec = find_screen_record(menu_.screen);
    if (srec == nullptr) {
        return;
    }
    chrome::MenuChrome m{};
    m.width = static_cast<i32>(srec->width);
    m.titles = menu_.titles;
    m.title_rects = menu_.geometry.title;
    m.title_count = menu_.title_count;
    m.selected_title = menu_.title < menu_.title_count ? static_cast<i32>(menu_.title) : -1;
    m.box = menu_.item_count != 0 ? menu_.geometry.box : menu_layout::empty_rect();
    m.items = menu_.items;
    m.item_rects = menu_.geometry.item;
    m.item_count = menu_.item_count;
    m.selected_item = menu_.item;
    m.sub_box = menu_.sub_count != 0 ? menu_.geometry.sub_box : menu_layout::empty_rect();
    m.sub_items = menu_.subs;
    m.sub_item_rects = menu_.geometry.sub_item;
    m.sub_item_count = menu_.sub_count;
    m.selected_sub = menu_.sub;
    m.depth = srec->depth;
    if (srec->depth >= 8) {
        m.strong_shade = srec->strong_shade;
        m.light_shade = srec->light_shade;
    }
    chrome::draw_menu(b, bitmap, clip, m);
}

void Intuition::pointer_event(i32 x, i32 y, u8 buttons, u8 pressed, u8 released, bool moved) {
    begin_batch();
    route_pointer(x, y, buttons, pressed, released, moved);
    end_batch();
}

void Intuition::route_pointer(i32 x, i32 y, u8 buttons, u8 pressed, u8 released, bool moved) {
    mouse_x_ = x;
    mouse_y_ = y;
    if (front_ == 0) {
        return;
    }
    Bus b(*machine_);
    // An active BOOPSI gadget owns the pointer until it lets go.
    if (custom_.gadget != 0) {
        if (!window_open(custom_.window)) {
            drop_custom_gadget();
        } else {
            // A move while the left button is down, or in the report that
            // lets it go, carries the button in its qualifier.
            if (moved) {
                const bool held = ((buttons | released) & kMouseLeft) != 0;
                custom_input(b, x, y, IECODE_NOBUTTON, held ? IEQUALIFIER_LEFTBUTTON : 0);
            }
            if (custom_.gadget != 0 && (released & kMouseLeft) != 0) {
                custom_input(b, x, y, IECODE_LBUTTON | IECODE_UP_PREFIX, 0);
            }
            return;
        }
    }
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
        // A press that follows a lost release leaves no bar button down.
        light_bar_button(false);
        // A press anywhere but on it ends the editing of a string gadget.
        if (string_.gadget != 0) {
            const u32 editing = string_.gadget;
            const WindowView sw = load_window(b, string_.window);
            if (!sw.ok || hit != string_.window ||
                !in_rect(resolve_gadget_box(b, sw, editing), x - sw.left, y - sw.top)) {
                end_string(false, 0);
            }
        }
        if (hit != 0) {
            activate_window(hit);
            const WindowView w = load_window(b, hit);
            if (w.ok) {
                const bool bordered = (w.flags & ndk::WFLG_BORDERLESS) == 0;
                if (bordered && (w.flags & ndk::WFLG_CLOSEGADGET) != 0 &&
                    in_rect(close_box_rect(w.left, w.top), x, y)) {
                    pressed_gadget_window_ = hit;
                    pressed_gadget_ = GadgetKind::Close;
                    light_bar_button(true);
                    left_press_consumed = true;
                } else if (bordered && (w.flags & ndk::WFLG_DEPTHGADGET) != 0 &&
                           in_rect(depth_box_rect(w.left, w.top, w.width), x, y)) {
                    pressed_gadget_window_ = hit;
                    pressed_gadget_ = GadgetKind::Depth;
                    light_bar_button(true);
                    left_press_consumed = true;
                } else if (!dragging && blocked(hit)) {
                    left_press_consumed = true;
                } else if (!dragging && hit_string_gadget(b, hit, x, y)) {
                    left_press_consumed = true;
                } else if (!dragging && hit_custom_gadget(b, hit, x, y)) {
                    left_press_consumed = true;
                } else if (!dragging && hit_bool_gadget(b, hit, x, y)) {
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

    // A held bar button shows pressed only while the pointer is over it.
    if (pressed_gadget_ == GadgetKind::Close || pressed_gadget_ == GadgetKind::Depth) {
        bool over = false;
        if (hit == pressed_gadget_window_) {
            const WindowView w = load_window(b, hit);
            const Rect box = pressed_gadget_ == GadgetKind::Close
                                 ? close_box_rect(w.left, w.top)
                                 : depth_box_rect(w.left, w.top, w.width);
            over = w.ok && in_rect(box, x, y);
        }
        light_bar_button(over && (released & kMouseLeft) == 0);
    }

    if ((released & kMouseLeft) != 0 && pressed_gadget_ != GadgetKind::None) {
        const u32 gad_window = pressed_gadget_window_;
        const GadgetKind gad_kind = pressed_gadget_;
        pressed_gadget_window_ = 0;
        pressed_gadget_ = GadgetKind::None;
        left_release_consumed = true;
        if (gad_kind == GadgetKind::Bool) {
            release_bool_gadget(b, gad_window, x, y);
        } else if (gad_window != 0 && hit == gad_window && gad_kind != GadgetKind::Prop) {
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

    bool menu_right = false;
    if ((pressed & kMouseRight) != 0 && !menu_.active && menu_strip_ready(b)) {
        menu_right = enter_menu_mode(b, x, y);
    }
    if (menu_.active) {
        menu_right = true;
        if ((released & kMouseRight) != 0) {
            release_menu(b, x, y);
        } else if (moved) {
            hover_menu(b, x, y);
        }
    }

    if (active_window_ != 0) {
        if ((pressed & kMouseLeft) != 0 && !left_press_consumed) {
            post_intuimessage(active_window_, ndk::IDCMP_MOUSEBUTTONS, ndk::SELECTDOWN);
        }
        if ((released & kMouseLeft) != 0 && !left_release_consumed) {
            post_intuimessage(active_window_, ndk::IDCMP_MOUSEBUTTONS, ndk::SELECTUP);
        }
        if ((pressed & kMouseRight) != 0 && !menu_right) {
            post_intuimessage(active_window_, ndk::IDCMP_MOUSEBUTTONS, ndk::MENUDOWN);
        }
        if ((released & kMouseRight) != 0 && !menu_right) {
            post_intuimessage(active_window_, ndk::IDCMP_MOUSEBUTTONS, ndk::MENUUP);
        }
        if ((pressed & kMouseMiddle) != 0) {
            post_intuimessage(active_window_, ndk::IDCMP_MOUSEBUTTONS, ndk::MIDDLEDOWN);
        }
        if ((released & kMouseMiddle) != 0) {
            post_intuimessage(active_window_, ndk::IDCMP_MOUSEBUTTONS, ndk::MIDDLEUP);
        }
    }

    if (moved && !dragging && !menu_right && active_window_ != 0) {
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
    chrome::draw_window_chrome(
        *graphics_, b, rec->rp,
        to_chrome(w, active_window_ == window, held_bar_button(window)), &keep);
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
                free_message_update(i);
                exec_->free(msg, ndk::IntuiMessage_SIZEOF);
            }
        }
    }

    if (active_window_ != 0 && find_window(active_window_) >= kMaxWindows) {
        active_window_ = 0;
    }
    pressed_gadget_window_ = 0;
    pressed_gadget_ = GadgetKind::None;
    bar_button_lit_ = false;
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

void Intuition::report_image_damage(const raster::RastPortView& view, const Rect& rect) {
    if (display_ == nullptr || rect.x1 < rect.x0 || rect.y1 < rect.y0) {
        return;
    }
    if (display_->front_screen != 0 &&
        view.bitmap_addr == display_->front_screen + ndk::sc_BitMap) {
        if (display_->sink.refresh != nullptr) {
            display_->sink.refresh(display_->sink.ctx, *machine_, display_->front_screen, rect);
        }
        return;
    }
    if (display_->damage.damaged != nullptr) {
        display_->damage.damaged(display_->damage.ctx, *machine_, view.bitmap_addr, rect);
    }
}

// A DrawInfo per screen, made on first use and freed with the screen: the
// pens IPrefs sent for the default screen (the 3.1 default pens before
// they arrive or on another screen), the screen's font, its depth and a
// 1:1 resolution. FreeScreenDrawInfo leaves it in place.
u32 Intuition::screen_draw_info(u32 screen) {
    ScreenRecord* found = screen != 0 ? find_screen_record(screen) : nullptr;
    if (found == nullptr) {
        return 0;
    }
    ScreenRecord& rec = *found;
    if (rec.draw_info == 0) {
        const u32 di = exec_->alloc(kDrawInfoBytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
        if (di == 0) {
            return 0;
        }
        Bus b(*machine_);
        const u16* source = kDefaultDriPens;
        if (default_candidate(screen)) {
            const u16* prefs = pen_prefs_for(rec.depth);
            if (prefs != nullptr) {
                source = prefs;
            }
        }
        const u32 pens = di + kDrawInfoPensAt;
        for (u32 i = 0; i < kDriPenCount; ++i) {
            b.w16(pens + i * 2, source[i]);
        }
        b.w16(pens + kDriPenCount * 2, 0xFFFF);
        b.w16(di + kDriVersion, 2);
        b.w16(di + kDriNumPens, kDriPenCount);
        b.w32(di + kDriPens, pens);
        b.w32(di + kDriFont, graphics_ != nullptr ? graphics_->topaz_font() : 0);
        b.w16(di + kDriDepth, static_cast<u16>(rec.depth));
        b.w16(di + kDriResolutionX, 44);
        b.w16(di + kDriResolutionY, 44);
        b.w32(di + kDriFlags, kDriNewLook);
        if (!b.ok()) {
            exec_->free(di, kDrawInfoBytes);
            return 0;
        }
        rec.draw_info = di;
    }
    return rec.draw_info;
}

u32 Intuition::make_gadget_info(u32 window) {
    const WindowRecord* rec = find_window_record(window);
    if (rec == nullptr || exec_ == nullptr) {
        return 0;
    }
    const u32 info = exec_->alloc(ndk::GadgetInfo_SIZEOF + 2, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (info == 0) {
        return 0;
    }
    Bus b(*machine_);
    const u32 screen = b.r32(window + ndk::wd_WScreen);
    b.w32(info + ndk::gi_Screen, screen);
    b.w32(info + ndk::gi_Window, window);
    b.w32(info + ndk::gi_RastPort, rec->rp);
    b.w32(info + ndk::gi_Layer, rec->layer);
    b.w16(info + ndk::gi_Domain, 0);
    b.w16(info + ndk::gi_Domain + 2, 0);
    b.w16(info + ndk::gi_Domain + 4, b.r16(window + ndk::wd_Width));
    b.w16(info + ndk::gi_Domain + 6, b.r16(window + ndk::wd_Height));
    b.w8(info + ndk::gi_Pens, b.r8(window + ndk::wd_DetailPen));
    b.w8(info + ndk::gi_Pens + 1, b.r8(window + ndk::wd_BlockPen));
    b.w32(info + ndk::gi_DrInfo, screen_draw_info(screen));
    if (!b.ok()) {
        exec_->free(info, ndk::GadgetInfo_SIZEOF + 2);
        return 0;
    }
    return info;
}

void Intuition::free_gadget_info(u32 info) {
    if (info != 0 && exec_ != nullptr) {
        exec_->free(info, ndk::GadgetInfo_SIZEOF + 2);
    }
}

void Intuition::draw_image(u32 rp, u32 image, i32 left, i32 top) {
    draw_image_mode(rp, image, left, top, ndk::IDS_NORMAL, 0);
}

void Intuition::draw_image_complemented(u32 rp, u32 image, i32 left, i32 top) {
    draw_image_mode(rp, image, left, top, ndk::IDS_SELECTED, 0);
}

void Intuition::draw_image_state(u32 rp, u32 image, i32 left, i32 top, u32 state,
                                 u32 draw_info) {
    draw_image_mode(rp, image, left, top, state, draw_info);
}

void Intuition::report_mouse(u32 window, bool on) {
    if (machine_ == nullptr || find_window(window) >= kMaxWindows) {
        return;
    }
    Bus b(*machine_);
    const u32 flags = b.r32(window + ndk::wd_Flags);
    b.w32(window + ndk::wd_Flags,
          on ? flags | ndk::WFLG_REPORTMOUSE : flags & ~ndk::WFLG_REPORTMOUSE);
}

// The box of the dragged image on its screen; empty when nothing is
// dragged.
Rect Intuition::icon_drag_rect(Bus& b) const {
    if (icon_drag_.image == 0) {
        return Rect{0, 0, -1, -1};
    }
    const u32 image = icon_drag_.image;
    const i32 left = icon_drag_.x + static_cast<i16>(b.r16(image + image::LeftEdge));
    const i32 top = icon_drag_.y + static_cast<i16>(b.r16(image + image::TopEdge));
    const i32 width = static_cast<i16>(b.r16(image + image::Width));
    const i32 height = static_cast<i16>(b.r16(image + image::Height));
    if (!b.ok() || width <= 0 || height <= 0) {
        return Rect{0, 0, -1, -1};
    }
    return Rect{left, top, left + width - 1, top + height - 1};
}

void Intuition::set_drag_image(u32 screen, u32 image, i32 x, i32 y) {
    if (machine_ == nullptr) {
        return;
    }
    Bus b(*machine_);
    const u32 old_screen = icon_drag_.screen;
    const Rect old_rect = icon_drag_rect(b);
    icon_drag_ = IconDrag{screen, image, x, y};
    const Rect new_rect = icon_drag_rect(b);
    if (old_screen != 0 && old_screen != screen) {
        compose(old_screen, old_rect);
        compose(screen, new_rect);
    } else {
        compose(screen, unite_rect(old_rect, new_rect));
    }
}

void Intuition::clear_drag_image() {
    if (machine_ == nullptr || icon_drag_.image == 0) {
        icon_drag_ = IconDrag{};
        return;
    }
    Bus b(*machine_);
    const u32 screen = icon_drag_.screen;
    const Rect rect = icon_drag_rect(b);
    icon_drag_ = IconDrag{};
    compose(screen, rect);
}

void Intuition::draw_sys_image(void* ctx, u32 rp, u32 which, i32 left, i32 top, i32 width,
                               i32 height, u32 state, u32 draw_info) {
    Intuition& in = *static_cast<Intuition*>(ctx);
    if (in.machine_ == nullptr || in.graphics_ == nullptr) {
        return;
    }
    Bus b(*in.machine_);
    u16 pens[sysimage::kPenCount];
    sysimage::read_pens(b, draw_info, pens);
    // The caller's pens and draw mode stay as they were: programs draw their
    // text after an image without setting them again.
    const u32 fg = b.r8(rp + ndk::rp_FgPen);
    const u32 bg = b.r8(rp + ndk::rp_BgPen);
    const u32 mode = b.r8(rp + ndk::rp_DrawMode);
    sysimage::draw(*in.graphics_, b, rp, which,
                   Rect{left, top, left + width - 1, top + height - 1}, state, pens);
    if (b.ok()) {
        in.graphics_->set_pens(rp, fg, bg, mode);
    }
}

void Intuition::draw_image_mode(u32 rp, u32 image, i32 left, i32 top, u32 state,
                                u32 draw_info) {
    if (machine_ == nullptr || rp == 0 || image == 0) {
        return;
    }
    Bus b(*machine_);
    const bool complement = state == ndk::IDS_SELECTED || state == ndk::IDS_INACTIVESELECTED;
    // A BOOPSI image draws itself.
    if (static_cast<u16>(b.r16(image + image::Depth)) == ndk::CUSTOMIMAGEDEPTH &&
        boopsi_.is_object(image)) {
        const u32 screen = display_ != nullptr ? display_->front_screen : 0;
        const u32 info = draw_info != 0
                             ? draw_info
                             : (screen != 0 ? screen_draw_info(screen) : 0);
        const u32 msg[] = {ndk::IM_DRAW, rp, pack_point(left, top), state, info};
        boopsi_.send(boopsi_.class_of(image), image, msg, 5);
        return;
    }
    const raster::RastPortView view = raster::load_rastport(b, rp);
    if (!view.ok) {
        return;
    }
    draw_image_view(b, view, image, left, top, complement, true);
}

void Intuition::draw_image_view(Bus& b, const raster::RastPortView& view, u32 image, i32 left,
                                i32 top, bool complement, bool damage) {
    const i64 origin_x = static_cast<i64>(left) + view.origin_x;
    const i64 origin_y = static_cast<i64>(top) + view.origin_y;
    u32 current = image;
    for (u32 step = 0; current != 0 && step < kMaxImageChain; ++step) {
        const i32 img_left = static_cast<i16>(b.r16(current + image::LeftEdge));
        const i32 img_top = static_cast<i16>(b.r16(current + image::TopEdge));
        const i32 width = static_cast<i16>(b.r16(current + image::Width));
        const i32 height = static_cast<i16>(b.r16(current + image::Height));
        const i32 depth = static_cast<i16>(b.r16(current + image::Depth));
        const u32 data = b.r32(current + image::ImageData);
        const u32 plane_pick = b.r8(current + image::PlanePick);
        const u32 plane_on_off = b.r8(current + image::PlaneOnOff);
        const u32 next = b.r32(current + image::NextImage);
        if (!b.ok()) {
            return;
        }
        Rect area{0, 0, -1, -1};
        if (width > 0 && height > 0 && depth >= 0 && depth <= 8 &&
            clip_image_rect(view, origin_x + img_left, origin_y + img_top, width, height, area)) {
            // A complemented image has every pen of its box inverted in the
            // planes the bitmap has.
            const u32 pen_mask =
                view.bitmap.depth >= 8 ? 0xFFu : (1u << view.bitmap.depth) - 1;
            if (data == 0 || depth == 0) {
                const Rect filled =
                    raster::fill_rect(b, view.bitmap, view.clip, area,
                                      complement ? ~plane_on_off & pen_mask : plane_on_off);
                if (b.ok() && damage) {
                    report_image_damage(view, filled);
                }
            } else {
                const u32 row_bytes = ((static_cast<u32>(width) + 15) / 16) * 2;
                const u32 plane_bytes = row_bytes * static_cast<u32>(height);
                const u64 all_bytes = static_cast<u64>(plane_bytes) * static_cast<u32>(depth);
                if (all_bytes <= 0xFFFFFFFFu &&
                    b.bytes_at(data, static_cast<u32>(all_bytes)) != nullptr) {
                    i32 plane_of_bit[8]{};
                    u32 fixed = 0;
                    u32 used = 0;
                    for (u32 bit = 0; bit < 8; ++bit) {
                        if ((plane_pick & (1u << bit)) != 0) {
                            plane_of_bit[bit] = static_cast<i32>(used++);
                        } else {
                            plane_of_bit[bit] = -1;
                            if ((plane_on_off & (1u << bit)) != 0) {
                                fixed |= 1u << bit;
                            }
                        }
                    }
                    const i64 image_x = origin_x + img_left;
                    const i64 image_y = origin_y + img_top;
                    for (i64 y = area.y0; y <= area.y1; ++y) {
                        const u32 row = static_cast<u32>(y - image_y);
                        const u8* planes[8]{};
                        for (u32 plane = 0; plane < static_cast<u32>(depth); ++plane) {
                            planes[plane] = b.bytes_at(
                                data + plane * plane_bytes + row * row_bytes, row_bytes);
                        }
                        if (!b.ok()) {
                            return;
                        }
                        for (i64 x = area.x0; x <= area.x1; ++x) {
                            const u32 column = static_cast<u32>(x - image_x);
                            const u8 mask = static_cast<u8>(0x80u >> (column & 7));
                            u32 pen = fixed;
                            for (u32 bit = 0; bit < 8; ++bit) {
                                const i32 plane = plane_of_bit[bit];
                                if (plane >= 0 && static_cast<u32>(plane) < static_cast<u32>(depth) &&
                                    (planes[plane][column >> 3] & mask) != 0) {
                                    pen |= 1u << bit;
                                }
                            }
                            if (view.clip.region != 0 &&
                                !raster::in_clip(b, view.clip, static_cast<i32>(x),
                                                 static_cast<i32>(y))) {
                                continue;
                            }
                            if (complement) {
                                pen = ~pen & pen_mask;
                            }
                            raster::write_pixel(b, view.bitmap, static_cast<i32>(x),
                                                static_cast<i32>(y), pen);
                        }
                    }
                    if (b.ok() && damage) {
                        report_image_damage(view, area);
                    }
                }
            }
        }
        current = next;
    }
}

void Intuition::erase_image(u32 rp, u32 image, i32 left, i32 top) {
    if (machine_ == nullptr || rp == 0 || image == 0) {
        return;
    }
    Bus b(*machine_);
    const raster::RastPortView view = raster::load_rastport(b, rp);
    if (!view.ok) {
        return;
    }
    // Erasing clears, as EraseRect does where no backfill hook draws.
    const u32 pen = 0;
    const i64 origin_x = static_cast<i64>(left) + view.origin_x;
    const i64 origin_y = static_cast<i64>(top) + view.origin_y;
    u32 current = image;
    for (u32 step = 0; current != 0 && step < kMaxImageChain; ++step) {
        const i32 img_left = static_cast<i16>(b.r16(current + image::LeftEdge));
        const i32 img_top = static_cast<i16>(b.r16(current + image::TopEdge));
        const i32 width = static_cast<i16>(b.r16(current + image::Width));
        const i32 height = static_cast<i16>(b.r16(current + image::Height));
        const u32 next = b.r32(current + image::NextImage);
        if (!b.ok()) {
            return;
        }
        Rect area{0, 0, -1, -1};
        if (width > 0 && height > 0 &&
            clip_image_rect(view, origin_x + img_left, origin_y + img_top, width, height, area)) {
            const Rect filled = raster::fill_rect(b, view.bitmap, view.clip, area, pen);
            if (b.ok()) {
                report_image_damage(view, filled);
            }
        }
        current = next;
    }
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
        } else if (gtype == ndk::GTYP_BOOLGADGET) {
            draw_bool_gadget(b, window, w.rp, gadget);
        } else if (gtype == ndk::GTYP_CUSTOMGADGET) {
            draw_custom_gadget(b, window, w.rp, gadget);
        } else if (gtype == ndk::GTYP_STRGADGET) {
            draw_string_gadget(b, window, w.rp, gadget);
        } else {
            log_unhandled_gadget_type(gtype);
        }
        gadget = next;
    }
}

// --- BOOPSI gadgets --------------------------------------------------------


// GM_RENDER: the gadget draws itself whole through the window's RastPort.
void Intuition::draw_custom_gadget(Bus& b, u32 window, u32 rp, u32 gadget) {
    (void)b;
    const u32 info = make_gadget_info(window);
    if (info == 0) {
        return;
    }
    const u32 msg[] = {ndk::GM_RENDER, info, rp, ndk::GREDRAW_REDRAW};
    boopsi_.send(boopsi_.class_of(gadget), gadget, msg, 4);
    free_gadget_info(info);
}

// A press on a BOOPSI gadget whose GM_HITTEST says it is hit: GM_GOACTIVE,
// and the gadget keeps the input while it answers GMR_MEACTIVE.
bool Intuition::hit_custom_gadget(Bus& b, u32 window, i32 x, i32 y) {
    const WindowView w = load_window(b, window);
    if (!w.ok || exec_ == nullptr) {
        return false;
    }
    const i32 wx = x - w.left;
    const i32 wy = y - w.top;
    u32 gadget = w.first_gadget;
    for (u32 step = 0; step < kMaxGadgets && gadget != 0; ++step) {
        const u32 gtype = b.r16(gadget + ndk::gg_GadgetType) & ndk::GTYP_GTYPEMASK;
        const u32 gflags = b.r16(gadget + ndk::gg_Flags);
        const u32 next = b.r32(gadget + ndk::gg_NextGadget);
        if (!b.ok()) {
            return false;
        }
        const Rect box = gtype == ndk::GTYP_CUSTOMGADGET && (gflags & ndk::GFLG_DISABLED) == 0
                             ? resolve_gadget_box(b, w, gadget)
                             : Rect{0, 0, -1, -1};
        if (!in_rect(box, wx, wy)) {
            gadget = next;
            continue;
        }
        const u32 info = make_gadget_info(window);
        const u32 point = pack_point(wx - box.x0, wy - box.y0);
        const u32 test[] = {ndk::GM_HITTEST, info, point};
        if (info == 0 ||
            boopsi_.send(boopsi_.class_of(gadget), gadget, test, 3) != ndk::GMR_GADGETHIT) {
            free_gadget_info(info);
            gadget = next;
            continue;
        }
        const u32 event = exec_->alloc(kEventBytes + 4, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
        if (event == 0) {
            free_gadget_info(info);
            return false;
        }
        custom_ = ActiveCustom{window, gadget, info, event};
        b.w8(event + ie_Class, IECLASS_RAWMOUSE);
        b.w16(event + ie_Code, IECODE_LBUTTON);
        b.w16(event + ie_Qualifier, IEQUALIFIER_LEFTBUTTON);
        if ((b.r16(gadget + ndk::gg_Activation) & ndk::GACT_IMMEDIATE) != 0) {
            post_intuimessage(window, ndk::IDCMP_GADGETDOWN, 0, gadget);
        }
        const u32 go[] = {ndk::GM_GOACTIVE, info, event, event + kTermination, point};
        custom_result(b, boopsi_.send(boopsi_.class_of(gadget), gadget, go, 5));
        return true;
    }
    return false;
}

// GM_HANDLEINPUT for the active BOOPSI gadget, with the pointer relative
// to its box.
void Intuition::custom_input(Bus& b, i32 x, i32 y, u32 code, u32 qualifier) {
    const ActiveCustom c = custom_;
    const WindowView w = load_window(b, c.window);
    if (!w.ok) {
        drop_custom_gadget();
        return;
    }
    const Rect box = resolve_gadget_box(b, w, c.gadget);
    b.w16(c.event + ie_Code, code);
    b.w16(c.event + ie_Qualifier, qualifier);
    const u32 msg[] = {ndk::GM_HANDLEINPUT, c.info, c.event, c.event + kTermination,
                       pack_point(x - w.left - box.x0, y - w.top - box.y0)};
    custom_result(b, boopsi_.send(boopsi_.class_of(c.gadget), c.gadget, msg, 5));
}

// What the active gadget answered: GMR_MEACTIVE keeps the input; any other
// answer ends it, with IDCMP_GADGETUP and the termination code for
// GMR_VERIFY on a GACT_RELVERIFY gadget, and GM_GOINACTIVE.
void Intuition::custom_result(Bus& b, u32 result) {
    const ActiveCustom c = custom_;
    if (c.gadget == 0 ||
        (result & (ndk::GMR_NOREUSE | ndk::GMR_REUSE | ndk::GMR_NEXTACTIVE |
                   ndk::GMR_PREVACTIVE)) == 0) {
        return;
    }
    if ((result & ndk::GMR_VERIFY) != 0 &&
        (b.r16(c.gadget + ndk::gg_Activation) & ndk::GACT_RELVERIFY) != 0) {
        const u32 code = b.r32(c.event + kTermination);
        post_intuimessage(c.window, ndk::IDCMP_GADGETUP, static_cast<u16>(code), c.gadget);
    }
    const u32 msg[] = {ndk::GM_GOINACTIVE, c.info, 0};
    boopsi_.send(boopsi_.class_of(c.gadget), c.gadget, msg, 3);
    drop_custom_gadget();
}

// --- string gadgets -------------------------------------------------------

// The part of a string gadget's box its text shows in, and how many
// characters of the topaz font fit there; gadtools frames its own.
Rect Intuition::string_text_box(Bus& b, u32 window, u32 gadget, u32& columns) {
    const WindowView w = load_window(b, window);
    Rect box = resolve_gadget_box(b, w, gadget);
    if ((b.r16(gadget + ndk::gg_GadgetType) & ndk::GTYP_GADTOOLS) != 0) {
        box.x0 += 4;
        box.x1 -= 4;
    }
    const i32 width = box.x1 - box.x0 + 1;
    columns = width > 0 ? static_cast<u32>(width) / 8 : 0;
    return box;
}

// The text from the display position on, and while it is being edited the
// cursor as a complemented character cell, which the display position
// keeps in view.
void Intuition::draw_string_gadget(Bus& b, u32 window, u32 rp, u32 gadget) {
    const u32 si = b.r32(gadget + ndk::gg_SpecialInfo);
    if (!b.ok() || rp == 0 || si == 0) {
        return;
    }
    const bool framed = gadtools_draws(b, window, rp, gadget);
    u32 columns = 0;
    const Rect box = string_text_box(b, window, gadget, columns);
    if (box.x1 < box.x0 || box.y1 < box.y0) {
        return;
    }
    const u32 buffer = b.r32(si + ndk::si_Buffer);
    const u32 count = b.r16(si + ndk::si_NumChars);
    const u32 pos = b.r16(si + ndk::si_BufferPos);
    u32 disp = b.r16(si + ndk::si_DispPos);
    const bool editing = string_.gadget == gadget;
    if (editing && columns != 0) {
        disp = pos < disp ? pos : (pos >= disp + columns ? pos - columns + 1 : disp);
        b.w16(si + ndk::si_DispPos, disp);
    }
    const u32 fg = b.r8(rp + ndk::rp_FgPen);
    const u32 bg = b.r8(rp + ndk::rp_BgPen);
    const u32 mode = b.r8(rp + ndk::rp_DrawMode);
    const u32 cx = b.r16(rp + ndk::rp_cp_x);
    const u32 cy = b.r16(rp + ndk::rp_cp_y);
    const i32 top = box.y0 + (box.y1 - box.y0 + 1 - 8) / 2;
    if (!framed) {
        graphics_->set_pens(rp, 0, 0, ndk::JAM1);
        graphics_->rect_fill(rp, box.x0, box.y0, box.x1, box.y1);
    }
    u8 text[256];
    u32 shown = 0;
    for (u32 i = disp; i < count && shown < columns && shown < sizeof text && buffer != 0; ++i) {
        text[shown++] = static_cast<u8>(b.r8(buffer + i));
    }
    if (shown != 0 && b.ok()) {
        graphics_->set_pens(rp, 1, 0, ndk::JAM1);
        graphics_->move(rp, box.x0, top + 6);
        graphics_->text(rp, text, shown);
    }
    if (editing && pos >= disp && pos - disp < columns) {
        const i32 x = box.x0 + static_cast<i32>((pos - disp) * 8);
        graphics_->set_pens(rp, 0xFF, 0, ndk::COMPLEMENT);
        graphics_->rect_fill(rp, x, top, x + 7, top + 7);
    }
    graphics_->set_pens(rp, fg, bg, mode);
    b.w16(rp + ndk::rp_cp_x, cx);
    b.w16(rp + ndk::rp_cp_y, cy);
}

bool Intuition::activate_string(u32 gadget, u32 window) {
    if (machine_ == nullptr || gadget == 0 || !window_open(window)) {
        return false;
    }
    Bus b(*machine_);
    const u32 type = b.r16(gadget + ndk::gg_GadgetType) & ndk::GTYP_GTYPEMASK;
    const u32 flags = b.r16(gadget + ndk::gg_Flags);
    if (!b.ok() || type != ndk::GTYP_STRGADGET || (flags & ndk::GFLG_DISABLED) != 0 ||
        b.r32(gadget + ndk::gg_SpecialInfo) == 0) {
        return false;
    }
    if (string_.gadget != 0 && string_.gadget != gadget) {
        end_string(false, 0);
    }
    string_ = ActiveString{window, gadget, 0};
    const WindowRecord* rec = find_window_record(window);
    if (rec != nullptr) {
        draw_string_gadget(b, window, rec->rp, gadget);
    }
    return true;
}

// A press on a string gadget makes it the one being edited, with the
// cursor at the character under the pointer.
bool Intuition::hit_string_gadget(Bus& b, u32 window, i32 x, i32 y) {
    const WindowView w = load_window(b, window);
    if (!w.ok) {
        return false;
    }
    u32 gadget = w.first_gadget;
    for (u32 step = 0; step < kMaxGadgets && gadget != 0; ++step) {
        const u32 type = b.r16(gadget + ndk::gg_GadgetType) & ndk::GTYP_GTYPEMASK;
        const u32 next = b.r32(gadget + ndk::gg_NextGadget);
        if (!b.ok()) {
            return false;
        }
        if (type == ndk::GTYP_STRGADGET &&
            in_rect(resolve_gadget_box(b, w, gadget), x - w.left, y - w.top)) {
            const u32 si = b.r32(gadget + ndk::gg_SpecialInfo);
            u32 columns = 0;
            const Rect text = string_text_box(b, window, gadget, columns);
            const i32 col = (x - w.left - text.x0) / 8;
            const u32 count = b.r16(si + ndk::si_NumChars);
            const u32 at = b.r16(si + ndk::si_DispPos) + static_cast<u32>(col > 0 ? col : 0);
            b.w16(si + ndk::si_BufferPos, at < count ? at : count);
            if (!activate_string(gadget, window)) {
                return false;
            }
            if ((b.r16(gadget + ndk::gg_Activation) & ndk::GACT_IMMEDIATE) != 0) {
                post_intuimessage(window, ndk::IDCMP_GADGETDOWN, 0, gadget);
            }
            return true;
        }
        gadget = next;
    }
    return false;
}

namespace {

// A LONGINT string gadget's text becomes the number of its leading digits,
// written back as that number's canonical decimal, which can be shorter
// than the text that was typed.
void normalize_long_int(Bus& b, u32 gadget) {
    const u32 si = b.r32(gadget + ndk::gg_SpecialInfo);
    if (!b.ok() || si == 0) {
        return;
    }
    const u32 buffer = b.r32(si + ndk::si_Buffer);
    const u32 count = b.r16(si + ndk::si_NumChars);
    if (!b.ok() || buffer == 0) {
        return;
    }
    u32 at = 0;
    bool negative = false;
    if (count != 0 && b.r8(buffer) == '-') {
        negative = true;
        at = 1;
    }
    i64 number = 0;
    for (; at < count; ++at) {
        const u8 c = static_cast<u8>(b.r8(buffer + at));
        if (c < '0' || c > '9') {
            break;
        }
        if (number <= 2147483647LL) {
            number = number * 10 + (c - '0');
        }
    }
    i64 signed_number = negative ? -number : number;
    signed_number = signed_number > 2147483647LL ? 2147483647LL
                    : signed_number < -2147483648LL ? -2147483647LL - 1
                                                    : signed_number;
    const i32 value = static_cast<i32>(signed_number);
    char text[12];
    u32 length = 0;
    u64 magnitude =
        value < 0 ? static_cast<u64>(-static_cast<i64>(value)) : static_cast<u64>(value);
    do {
        text[length++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    if (value < 0) {
        text[length++] = '-';
    }
    const u32 max = b.r16(si + ndk::si_MaxChars);
    const u32 room = max > 0 ? max - 1 : 0;
    const u32 written = length < room ? length : room;
    for (u32 i = 0; i < written; ++i) {
        b.w8(buffer + i, static_cast<u8>(text[length - 1 - i]));
    }
    b.w8(buffer + written, 0);
    b.w16(si + ndk::si_NumChars, written);
    const u32 pos = b.r16(si + ndk::si_BufferPos);
    b.w16(si + ndk::si_BufferPos, pos < written ? pos : written);
    b.w16(si + ndk::si_DispPos, 0);
    b.w32(si + ndk::si_LongInt, static_cast<u32>(value));
}

}

// Ends the editing; Return or Tab on a GACT_RELVERIFY gadget reports
// GADGETUP with the key that ended it.
void Intuition::end_string(bool verify, u16 code) {
    const ActiveString s = string_;
    string_ = ActiveString{};
    if (s.gadget == 0 || !window_open(s.window)) {
        return;
    }
    Bus b(*machine_);
    if ((b.r16(s.gadget + ndk::gg_Activation) & ndk::GACT_LONGINT) != 0) {
        normalize_long_int(b, s.gadget);
    }
    const WindowRecord* rec = find_window_record(s.window);
    if (rec != nullptr) {
        draw_string_gadget(b, s.window, rec->rp, s.gadget);
    }
    if (verify && (b.r16(s.gadget + ndk::gg_Activation) & ndk::GACT_RELVERIFY) != 0) {
        post_intuimessage(s.window, ndk::IDCMP_GADGETUP, code, s.gadget);
    }
}

// Edits the string with a key: characters go in at the cursor, Backspace
// and Del take one out, the cursor keys move it, Return and Tab end the
// editing with GADGETUP, and Esc ends it without.
void Intuition::string_key(u8 byte) {
    if (string_.gadget == 0) {
        return;
    }
    if (!window_open(string_.window)) {
        string_ = ActiveString{};
        return;
    }
    Bus b(*machine_);
    const u32 gadget = string_.gadget;
    const u32 si = b.r32(gadget + ndk::gg_SpecialInfo);
    const u32 buffer = b.r32(si + ndk::si_Buffer);
    const u32 max = b.r16(si + ndk::si_MaxChars);
    const bool long_int = (b.r16(gadget + ndk::gg_Activation) & ndk::GACT_LONGINT) != 0;
    u32 count = b.r16(si + ndk::si_NumChars);
    u32 pos = b.r16(si + ndk::si_BufferPos);
    if (!b.ok() || buffer == 0) {
        return;
    }
    pos = pos > count ? count : pos;
    if (string_.csi == 1) {
        string_.csi = 0;
        if (byte == 'C' && pos < count) {
            ++pos;
        } else if (byte == 'D' && pos > 0) {
            --pos;
        } else if (byte < 0x40 || byte > 0x7E) {
            string_.csi = 2;  // a longer sequence, passed over to its end
        }
    } else if (string_.csi == 2) {
        if (byte >= 0x40 && byte <= 0x7E) {
            string_.csi = 0;
        }
        return;
    } else if (byte == 0x9B) {
        string_.csi = 1;
        return;
    } else if (byte == '\n' || byte == '\r' || byte == '\t') {
        end_string(true, byte == '\t' ? 0x09 : 0x0D);
        return;
    } else if (byte == 0x1B) {
        end_string(false, 0);
        return;
    } else if (byte == 0x08 || byte == 0x7F) {
        const u32 at = byte == 0x08 ? (pos > 0 ? pos - 1 : count) : pos;
        if (at < count) {
            for (u32 i = at; i + 1 < count; ++i) {
                b.w8(buffer + i, b.r8(buffer + i + 1));
            }
            --count;
            pos = at;
        }
    } else if (byte >= 0x20 && (byte < 0x7F || byte >= 0xA0) && count + 1 < max &&
               (!long_int || byte == '-' || (byte >= '0' && byte <= '9'))) {
        for (u32 i = count; i > pos; --i) {
            b.w8(buffer + i, b.r8(buffer + i - 1));
        }
        b.w8(buffer + pos, byte);
        ++count;
        ++pos;
    }
    b.w8(buffer + count, 0);
    b.w16(si + ndk::si_NumChars, count);
    b.w16(si + ndk::si_BufferPos, pos);
    const WindowRecord* rec = find_window_record(string_.window);
    if (rec != nullptr && b.ok()) {
        draw_string_gadget(b, string_.window, rec->rp, gadget);
    }
}

// The code a prop gadget's messages carry: what gadtools makes of its
// pot for a scroller of its own, 0 for any other.
u16 Intuition::prop_code(Bus& b, u32 gadget) {
    if (gadget_prop_code_ == nullptr ||
        (b.r16(gadget + ndk::gg_GadgetType) & ndk::GTYP_GADTOOLS) == 0) {
        return 0;
    }
    return gadget_prop_code_(gadget_renderer_ctx_, gadget);
}

bool Intuition::blocked(u32 window) const {
    for (const Block& block : blocks_) {
        if (window != 0 && block.window == window) {
            return true;
        }
    }
    return false;
}

void Intuition::drop_custom_gadget() {
    if (custom_.info != 0) {
        free_gadget_info(custom_.info);
    }
    if (custom_.event != 0 && exec_ != nullptr) {
        exec_->free(custom_.event, kEventBytes + 4);
    }
    custom_ = ActiveCustom{};
}

// A boolean gadget as it stands: gadtools draws its own; any other shows
// its image, the select image while selected with GFLG_GADGHIMAGE, and
// its box complemented while selected with GFLG_GADGHCOMP.
void Intuition::draw_bool_gadget(Bus& b, u32 window, u32 rp, u32 gadget) {
    const u32 flags = b.r16(gadget + ndk::gg_Flags);
    if (!b.ok() || rp == 0 || gadtools_draws(b, window, rp, gadget)) {
        return;
    }
    const WindowView w = load_window(b, window);
    const Rect box = resolve_gadget_box(b, w, gadget);
    if (!w.ok || box.x1 < box.x0 || box.y1 < box.y0) {
        return;
    }
    const bool selected = (flags & ndk::GFLG_SELECTED) != 0;
    const u32 highlight = flags & ndk::GFLG_GADGHIGHBITS;
    if ((flags & ndk::GFLG_GADGIMAGE) != 0) {
        const u32 select = b.r32(gadget + ndk::gg_SelectRender);
        const u32 render = b.r32(gadget + ndk::gg_GadgetRender);
        const bool high = selected && highlight == ndk::GFLG_GADGHIMAGE && select != 0;
        const u32 image = high ? select : render;
        if (image != 0) {
            draw_image(rp, image, box.x0, box.y0);
        }
    }
    if (selected && highlight == ndk::GFLG_GADGHCOMP) {
        complement_gadget(b, window, rp, gadget);
    }
}

// A gadtools gadget gadtools draws; true when it did.
bool Intuition::gadtools_draws(Bus& b, u32 window, u32 rp, u32 gadget) {
    const u32 type = b.r16(gadget + ndk::gg_GadgetType);
    return b.ok() && (type & ndk::GTYP_GADTOOLS) != 0 && gadget_renderer_ != nullptr &&
           gadget_renderer_(gadget_renderer_ctx_, window, rp, gadget);
}

// Complements the gadget's box; the program's pens and draw mode stay as
// they were.
void Intuition::complement_gadget(Bus& b, u32 window, u32 rp, u32 gadget) {
    const WindowView w = load_window(b, window);
    const Rect box = resolve_gadget_box(b, w, gadget);
    if (!w.ok || box.x1 < box.x0 || box.y1 < box.y0) {
        return;
    }
    const u32 fg = b.r8(rp + ndk::rp_FgPen);
    const u32 bg = b.r8(rp + ndk::rp_BgPen);
    const u32 mode = b.r8(rp + ndk::rp_DrawMode);
    graphics_->set_pens(rp, 0xFF, 0, ndk::COMPLEMENT);
    graphics_->rect_fill(rp, box.x0, box.y0, box.x1, box.y1);
    graphics_->set_pens(rp, fg, bg, mode);
}

bool Intuition::hit_bool_gadget(Bus& b, u32 window, i32 x, i32 y) {
    const WindowRecord* rec = find_window_record(window);
    const WindowView w = load_window(b, window);
    if (rec == nullptr || !w.ok) {
        return false;
    }
    const i32 wx = x - w.left;
    const i32 wy = y - w.top;
    u32 gadget = w.first_gadget;
    for (u32 step = 0; step < kMaxGadgets && gadget != 0; ++step) {
        const u32 gflags = b.r16(gadget + ndk::gg_Flags);
        const u32 activation = b.r16(gadget + ndk::gg_Activation);
        const u32 type = b.r16(gadget + ndk::gg_GadgetType);
        const u32 gtype = type & ndk::GTYP_GTYPEMASK;
        const u32 next = b.r32(gadget + ndk::gg_NextGadget);
        if (!b.ok()) {
            return false;
        }
        if (gtype != ndk::GTYP_BOOLGADGET || (gflags & ndk::GFLG_DISABLED) != 0 ||
            (activation & (ndk::GACT_RELVERIFY | ndk::GACT_IMMEDIATE)) == 0 ||
            !in_rect(resolve_gadget_box(b, w, gadget), wx, wy)) {
            gadget = next;
            continue;
        }
        // A gadget that toggles its own state takes it when let go; its
        // release callback turns GFLG_SELECTED over.
        const bool toggles = (type & ndk::GTYP_GADTOOLS) != 0 &&
                             (activation & ndk::GACT_TOGGLESELECT) != 0;
        if (!toggles) {
            b.w16(gadget + ndk::gg_Flags, gflags | ndk::GFLG_SELECTED);
            draw_bool_gadget(b, window, rec->rp, gadget);
        }
        pressed_gadget_window_ = window;
        pressed_gadget_ = GadgetKind::Bool;
        pressed_bool_gadget_ = gadget;
        if ((activation & ndk::GACT_IMMEDIATE) != 0) {
            post_intuimessage(window, ndk::IDCMP_GADGETDOWN, 0, gadget);
        }
        return true;
    }
    return false;
}

// The left button let go of the boolean gadget it held: the gadget is
// drawn as it now stands, a release over it reports GADGETUP, and one that
// does not toggle its own state shows unselected.
void Intuition::release_bool_gadget(Bus& b, u32 window, i32 x, i32 y) {
    const u32 gadget = pressed_bool_gadget_;
    pressed_bool_gadget_ = 0;
    const WindowRecord* rec = find_window_record(window);
    if (gadget == 0 || rec == nullptr || !window_open(window)) {
        return;
    }
    const WindowView w = load_window(b, window);
    const u32 gflags = b.r16(gadget + ndk::gg_Flags);
    const u32 activation = b.r16(gadget + ndk::gg_Activation);
    const u32 type = b.r16(gadget + ndk::gg_GadgetType);
    if (!w.ok || !b.ok()) {
        return;
    }
    const Rect box = resolve_gadget_box(b, w, gadget);
    const bool inside = window_at(x, y) == window && in_rect(box, x - w.left, y - w.top);
    const bool toggles = (type & ndk::GTYP_GADTOOLS) != 0 &&
                         (activation & ndk::GACT_TOGGLESELECT) != 0;
    // A gadtools gadget takes its new state before it is drawn again.
    u16 code = 0;
    if (inside && (type & ndk::GTYP_GADTOOLS) != 0 && gadget_release_ != nullptr) {
        gadget_release_(gadget_renderer_ctx_, window, gadget, x - w.left - box.x0,
                        y - w.top - box.y0, code);
    }
    if (!toggles) {
        b.w16(gadget + ndk::gg_Flags, gflags & ~ndk::GFLG_SELECTED);
    }
    if (!gadtools_draws(b, window, rec->rp, gadget)) {
        if ((gflags & ndk::GFLG_GADGHIGHBITS) == ndk::GFLG_GADGHCOMP &&
            (gflags & ndk::GFLG_GADGIMAGE) == 0) {
            // Complementing the box again puts back what was under it.
            complement_gadget(b, window, rec->rp, gadget);
        } else {
            draw_bool_gadget(b, window, rec->rp, gadget);
        }
    }
    if (inside && (activation & ndk::GACT_RELVERIFY) != 0) {
        post_intuimessage(window, ndk::IDCMP_GADGETUP, code, gadget);
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
            if (gtype != ndk::GTYP_BOOLGADGET && gtype != ndk::GTYP_CUSTOMGADGET &&
                gtype != ndk::GTYP_STRGADGET) {
                log_unhandled_gadget_type(gtype);
            }
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
                post_intuimessage(window, ndk::IDCMP_GADGETDOWN, prop_code(b, gadget), gadget);
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
                post_intuimessage(window, ndk::IDCMP_GADGETDOWN, prop_code(b, gadget), gadget);
            }
            if ((activation & ndk::GACT_RELVERIFY) != 0) {
                post_intuimessage(window, ndk::IDCMP_GADGETUP, prop_code(b, gadget), gadget);
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
        post_intuimessage(window, ndk::IDCMP_MOUSEMOVE, prop_code(b, gadget), gadget);
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
        post_intuimessage(window, ndk::IDCMP_GADGETUP, prop_code(b, gadget), gadget);
    }
}

}
