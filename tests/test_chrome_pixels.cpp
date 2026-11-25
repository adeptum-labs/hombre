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
#include "hid_mouse.hpp"
#include "libs/chrome.hpp"
#include "libs/menu_layout.hpp"
#include "libs/raster.hpp"

#include <cstdio>
#include <cstring>

using namespace hombre;
using namespace hombre::m68k;
using namespace hombre::ndk;
using gfx_test::Base;
using gfx_test::GfxRig;
using gfx_test::kData;

namespace hombre {

// The requester state EasyRequestArgs keeps is not public; Intuition names
// this helper a friend so the tests can aim the pointer at a button.
struct IntuitionTest {
    static u32 button_count(const Intuition& g, u32 window) {
        const Intuition::Requester* req = g.find_requester(window);
        return req != nullptr ? req->button_count : 0;
    }

    static Rect button(const Intuition& g, u32 window, u32 index) {
        const Intuition::Requester* req = g.find_requester(window);
        if (req == nullptr || index >= req->button_count) {
            return Rect{0, 0, -1, -1};
        }
        return req->buttons[index];
    }
};

}

namespace {

constexpr u32 kRp = kData;
constexpr u32 kNewScreen = kData + 0x100;
constexpr u32 kScreenTitle = kData + 0x200;
constexpr u32 kWindowTitle = kData + 0x240;
constexpr u32 kNarrowTitle = kData + 0x280;
constexpr u32 kMenuTitle = kData + 0x2C0;
constexpr u32 kCheckedLabel = kData + 0x300;
constexpr u32 kDisabledLabel = kData + 0x340;
constexpr u32 kSubmenuLabel = kData + 0x380;
constexpr u32 kSubLabel0 = kData + 0x3C0;
constexpr u32 kSubLabel1 = kData + 0x400;
constexpr u32 kSubMenuTarget = kData + 0x480;
constexpr u32 kEasy = kData + 0x500;
constexpr u32 kEasyTitle = kData + 0x540;
constexpr u32 kEasyText = kData + 0x580;
constexpr u32 kEasyGadgets = kData + 0x5C0;

constexpr u16 kPens3_1[sysimage::kPenCount] = {0, 1, 1, 2, 1, 3, 1, 0, 2, 1, 2, 1};

struct Golden {
    const char* name;
    u32 value;
};

const Golden kGoldens[] = {
    {"window_chrome_active_depth8", 0xfa2e938b},
    {"window_chrome_active_depth2", 0x77a82872},
    {"window_chrome_inactive_depth8", 0x0aadb9e0},
    {"window_chrome_inactive_depth2", 0xe395f5b4},
    {"window_chrome_close_depth8", 0x3ae6348c},
    {"window_chrome_close_depth2", 0x00b85fd4},
    {"window_chrome_depth_depth8", 0xc788dda3},
    {"window_chrome_depth_depth2", 0x107cd31a},
    {"window_chrome_zoom_depth8", 0x12bc7955},
    {"window_chrome_zoom_depth2", 0xac64cdea},
    {"window_chrome_borderless_depth8", 0xa7223285},
    {"window_chrome_borderless_depth2", 0x3ca1cac5},
    {"window_chrome_keep_depth8", 0x0ece39bb},
    {"window_chrome_keep_depth2", 0x8e62cea2},
    {"window_chrome_narrow_depth8", 0xf19bca6f},
    {"window_chrome_narrow_depth2", 0x7a341801},
    {"window_bar_active_depth8", 0xe61fd055},
    {"window_bar_active_depth2", 0x77a82872},
    {"window_bar_inactive_depth8", 0x61d194d2},
    {"window_bar_inactive_depth2", 0xe395f5b4},
    {"screen_bar_depth8", 0x74a41654},
    {"screen_bar_depth2", 0x37e6640f},
    {"prop_vertical_depth8", 0xe137ebbc},
    {"prop_vertical_depth2", 0xa1333335},
    {"prop_horizontal_depth8", 0x6587601c},
    {"prop_horizontal_depth2", 0x14876f56},
    {"prop_knob_selected_depth8", 0x82b74931},
    {"prop_knob_selected_depth2", 0xe7d47941},
    {"prop_borderless_depth8", 0xd0f45cf0},
    {"prop_borderless_depth2", 0x0018deb2},
    {"menu_depth8", 0x0beab5b4},
    {"menu_depth2", 0x33d70d6c},
    {"requester_depth8", 0x4722ec15},
    {"requester_depth2", 0xae4bf5d3},
    {"requester_held_depth8", 0x5150cf69},
    {"requester_held_depth2", 0x5e9e0127},
};

u32 g_mismatches = 0;

u32 pixel_hash(GfxRig& r, u32 rastport, i32 width, i32 height) {
    u32 hash = 2166136261u;
    for (i32 y = 0; y < height; ++y) {
        for (i32 x = 0; x < width; ++x) {
            r.cpu().a[1] = rastport;
            r.cpu().d[0] = static_cast<u32>(x);
            r.cpu().d[1] = static_cast<u32>(y);
            hash ^= r.result(Base::Graphics, graphics_lvo::ReadPixel);
            hash *= 16777619u;
        }
    }
    return hash;
}

void expect_golden(const char* base, const char* suffix, u32 actual) {
    char name[96];
    std::snprintf(name, sizeof(name), "%s%s", base, suffix);
    for (const Golden& golden : kGoldens) {
        if (std::strcmp(name, golden.name) != 0) {
            continue;
        }
        if (golden.value != actual) {
            std::printf("%s: 0x%08x\n", name, static_cast<unsigned>(actual));
            std::fflush(stdout);
            ++g_mismatches;
        }
        return;
    }
    std::printf("%s: no golden\n", name);
    std::fflush(stdout);
    ++g_mismatches;
}

chrome::DrawInfoPens default_pens() {
    chrome::DrawInfoPens pens{};
    for (u32 i = 0; i < sysimage::kPenCount; ++i) {
        pens.pen[i] = kPens3_1[i];
    }
    return pens;
}

struct Canvas {
    u32 bitmap;
    u32 rp;
};

Canvas make_canvas(GfxRig& r, i32 width, i32 height, u32 depth) {
    r.cpu().d[0] = static_cast<u32>(width);
    r.cpu().d[1] = static_cast<u32>(height);
    r.cpu().d[2] = depth;
    r.cpu().d[3] = 0;
    r.cpu().a[0] = 0;
    Canvas canvas{r.result(Base::Graphics, graphics_lvo::AllocBitMap), kRp};
    CHECK(canvas.bitmap != 0);
    r.cpu().a[1] = canvas.rp;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::InitRastPort), ExitReason::Halt);
    r.put32(canvas.rp + rp_BitMap, canvas.bitmap);
    return canvas;
}

void free_canvas(GfxRig& r, const Canvas& canvas) {
    r.cpu().a[0] = canvas.bitmap;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::FreeBitMap), ExitReason::Halt);
}

template <typename Draw>
u32 run_scene(GfxRig& r, const char* base, const char* suffix, i32 width, i32 height, u32 depth,
              u32 fill, Draw draw) {
    const Canvas canvas = make_canvas(r, width, height, depth);
    if (fill != 0) {
        r.graphics.set_pens(canvas.rp, fill, 0, JAM2);
        r.graphics.rect_fill(canvas.rp, 0, 0, width - 1, height - 1);
    }
    const u32 untouched = pixel_hash(r, canvas.rp, width, height);
    draw(canvas);
    const u32 drawn = pixel_hash(r, canvas.rp, width, height);
    CHECK(drawn != untouched);
    expect_golden(base, suffix, drawn);
    free_canvas(r, canvas);
    return drawn;
}

constexpr u32 kAllGadgets = WFLG_CLOSEGADGET | WFLG_DEPTHGADGET | WFLG_HASZOOM |
                            WFLG_SIZEGADGET | WFLG_SIZEBRIGHT | WFLG_SIZEBBOTTOM;

chrome::WindowChrome make_window(i32 width, i32 height, u32 flags, u32 title) {
    chrome::WindowChrome w{};
    w.width = width;
    w.height = height;
    w.flags = flags;
    w.title = title;
    w.active = (flags & WFLG_ACTIVATE) != 0;
    w.pens = default_pens();
    const bool borderless = (flags & WFLG_BORDERLESS) != 0;
    const bool sized = (flags & WFLG_SIZEGADGET) != 0;
    w.border_left = borderless ? 1 : chrome::kBorderLeft;
    w.border_top = borderless ? 1 : static_cast<u32>(w.metrics.border_top());
    w.border_right =
        borderless ? 1 : (sized ? chrome::kSizeBorderRight : chrome::kBorderRight);
    w.border_bottom =
        borderless ? 1 : (sized ? chrome::kSizeBorderBottom : chrome::kBorderBottom);
    return w;
}

void window_chrome_scenes(GfxRig& r) {
    constexpr i32 kW = 160;
    constexpr i32 kH = 90;
    constexpr i32 kNarrowW = 48;
    constexpr i32 kNarrowH = 70;
    r.str(kWindowTitle, "Window");
    r.str(kNarrowTitle, "A title far wider than this window");
    for (u32 depth : {8u, 2u}) {
        const char* suffix = depth == 8 ? "_depth8" : "_depth2";
        const u32 fill = depth == 8 ? 7u : 3u;

        const u32 active = run_scene(r, "window_chrome_active", suffix, kW, kH, depth, 0,
            [&](const Canvas& c) {
                const chrome::WindowChrome w =
                    make_window(kW, kH, kAllGadgets | WFLG_ACTIVATE, kWindowTitle);
                native::Bus b(r.machine);
                chrome::draw_window_chrome(r.graphics, b, c.rp, w);
            });
        run_scene(r, "window_chrome_inactive", suffix, kW, kH, depth, 0,
            [&](const Canvas& c) {
                const chrome::WindowChrome w = make_window(kW, kH, kAllGadgets, kWindowTitle);
                native::Bus b(r.machine);
                chrome::draw_window_chrome(r.graphics, b, c.rp, w);
            });
        const u32 close = run_scene(r, "window_chrome_close", suffix, kW, kH, depth, 0,
            [&](const Canvas& c) {
                chrome::WindowChrome w =
                    make_window(kW, kH, kAllGadgets | WFLG_ACTIVATE, kWindowTitle);
                w.pressed = chrome::BarButton::Close;
                native::Bus b(r.machine);
                chrome::draw_window_chrome(r.graphics, b, c.rp, w);
            });
        CHECK(close != active);
        const u32 depth_button = run_scene(r, "window_chrome_depth", suffix, kW, kH, depth, 0,
            [&](const Canvas& c) {
                chrome::WindowChrome w =
                    make_window(kW, kH, kAllGadgets | WFLG_ACTIVATE, kWindowTitle);
                w.pressed = chrome::BarButton::Depth;
                native::Bus b(r.machine);
                chrome::draw_window_chrome(r.graphics, b, c.rp, w);
            });
        CHECK(depth_button != active);
        const u32 zoom = run_scene(r, "window_chrome_zoom", suffix, kW, kH, depth, 0,
            [&](const Canvas& c) {
                chrome::WindowChrome w =
                    make_window(kW, kH, kAllGadgets | WFLG_ACTIVATE, kWindowTitle);
                w.pressed = chrome::BarButton::Zoom;
                native::Bus b(r.machine);
                chrome::draw_window_chrome(r.graphics, b, c.rp, w);
            });
        CHECK(zoom != active);

        // A borderless window draws only by clearing its background, so the
        // bitmap starts inked for the clear to show.
        run_scene(r, "window_chrome_borderless", suffix, kW, kH, depth, fill,
            [&](const Canvas& c) {
                const chrome::WindowChrome w =
                    make_window(kW, kH, WFLG_BORDERLESS | WFLG_ACTIVATE, kWindowTitle);
                native::Bus b(r.machine);
                chrome::draw_window_chrome(r.graphics, b, c.rp, w);
            });
        run_scene(r, "window_chrome_keep", suffix, kW, kH, depth, fill,
            [&](const Canvas& c) {
                const chrome::WindowChrome w =
                    make_window(kW, kH, kAllGadgets | WFLG_ACTIVATE, kWindowTitle);
                const Rect keep{
                    static_cast<i32>(w.border_left), static_cast<i32>(w.border_top),
                    kW - static_cast<i32>(w.border_right) - 1,
                    kH - static_cast<i32>(w.border_bottom) - 1};
                native::Bus b(r.machine);
                chrome::draw_window_chrome(r.graphics, b, c.rp, w, &keep);
            });
        run_scene(r, "window_chrome_narrow", suffix, kNarrowW, kNarrowH, depth, 0,
            [&](const Canvas& c) {
                const chrome::WindowChrome w = make_window(
                    kNarrowW, kNarrowH, WFLG_CLOSEGADGET | WFLG_DEPTHGADGET | WFLG_ACTIVATE,
                    kNarrowTitle);
                native::Bus b(r.machine);
                chrome::draw_window_chrome(r.graphics, b, c.rp, w);
            });
    }
}

void window_bar_scenes(GfxRig& r) {
    constexpr i32 kW = 160;
    constexpr i32 kH = 90;
    r.str(kWindowTitle, "Window");
    for (u32 depth : {8u, 2u}) {
        const char* suffix = depth == 8 ? "_depth8" : "_depth2";
        run_scene(r, "window_bar_active", suffix, kW, kH, depth, 0, [&](const Canvas& c) {
            const chrome::WindowChrome w =
                make_window(kW, kH, kAllGadgets | WFLG_ACTIVATE, kWindowTitle);
            native::Bus b(r.machine);
            chrome::draw_window_bar(r.graphics, b, c.rp, w);
        });
        run_scene(r, "window_bar_inactive", suffix, kW, kH, depth, 0, [&](const Canvas& c) {
            const chrome::WindowChrome w = make_window(kW, kH, kAllGadgets, kWindowTitle);
            native::Bus b(r.machine);
            chrome::draw_window_bar(r.graphics, b, c.rp, w);
        });
    }
}

void screen_bar_scenes(GfxRig& r) {
    constexpr i32 kW = 160;
    constexpr i32 kH = chrome::Metrics{}.bar_height() + 1;
    r.str(kScreenTitle, "Screen");
    for (u32 depth : {8u, 2u}) {
        const char* suffix = depth == 8 ? "_depth8" : "_depth2";
        run_scene(r, "screen_bar", suffix, kW, kH, depth, 0, [&](const Canvas& c) {
            native::Bus b(r.machine);
            chrome::draw_screen_bar(r.graphics, b, c.rp, static_cast<u32>(kW), kScreenTitle,
                                    default_pens(), chrome::Metrics{});
        });
    }
}

chrome::PropChrome make_prop(const Rect& box, const Rect& knob, u16 pflags, u16 gflags) {
    chrome::PropChrome g{};
    g.box = box;
    g.knob = knob;
    g.pflags = pflags;
    g.gflags = gflags;
    g.pens = default_pens();
    return g;
}

void prop_scenes(GfxRig& r) {
    constexpr i32 kW = 60;
    constexpr i32 kH = 50;
    constexpr Rect kVertical{8, 8, 20, 41};
    constexpr Rect kVerticalKnob{9, 9, 19, 24};
    constexpr Rect kHorizontal{8, 8, 50, 20};
    constexpr Rect kHorizontalKnob{9, 9, 40, 19};
    for (u32 depth : {8u, 2u}) {
        const char* suffix = depth == 8 ? "_depth8" : "_depth2";
        run_scene(r, "prop_vertical", suffix, kW, kH, depth, 0, [&](const Canvas& c) {
            const chrome::PropChrome g = make_prop(
                kVertical, kVerticalKnob, static_cast<u16>(AUTOKNOB | FREEVERT), 0);
            native::Bus b(r.machine);
            chrome::draw_prop_gadget(r.graphics, b, c.rp, g);
        });
        run_scene(r, "prop_horizontal", suffix, kW, kH, depth, 0, [&](const Canvas& c) {
            const chrome::PropChrome g = make_prop(
                kHorizontal, kHorizontalKnob, static_cast<u16>(AUTOKNOB | FREEHORIZ), 0);
            native::Bus b(r.machine);
            chrome::draw_prop_gadget(r.graphics, b, c.rp, g);
        });
        run_scene(r, "prop_knob_selected", suffix, kW, kH, depth, 0, [&](const Canvas& c) {
            const chrome::PropChrome g =
                make_prop(kVertical, kVerticalKnob, static_cast<u16>(AUTOKNOB | FREEVERT),
                          static_cast<u16>(GFLG_SELECTED));
            native::Bus b(r.machine);
            chrome::draw_prop_gadget(r.graphics, b, c.rp, g);
        });
        run_scene(r, "prop_borderless", suffix, kW, kH, depth, 0, [&](const Canvas& c) {
            const chrome::PropChrome g =
                make_prop(kVertical, kVerticalKnob,
                          static_cast<u16>(AUTOKNOB | FREEVERT | PROPBORDERLESS), 0);
            native::Bus b(r.machine);
            chrome::draw_prop_gadget(r.graphics, b, c.rp, g);
        });
    }
}

void menu_scenes(GfxRig& r) {
    constexpr i32 kW = 220;
    constexpr i32 kH = 140;
    const u32 title_name = r.str(kMenuTitle, "File");
    const u32 checked = r.str(kCheckedLabel, "Checked");
    const u32 disabled = r.str(kDisabledLabel, "Disabled");
    const u32 submenu = r.str(kSubmenuLabel, "Submenu");
    const u32 sub0 = r.str(kSubLabel0, "First");
    const u32 sub1 = r.str(kSubLabel1, "Second");
    const menu_layout::Title titles[1] = {{0, 64, 0, title_name, menu_layout::kTitleEnabled}};
    const menu_layout::Item items[3] = {
        {0, 0, 110, 10, 0, checked, 0, 0,
         static_cast<u16>(menu_layout::kItemText | menu_layout::kItemEnabled |
                          menu_layout::kItemCheckIt | menu_layout::kItemChecked),
         0},
        {0, 10, 110, 10, 0, disabled, 0, 0, static_cast<u16>(menu_layout::kItemText), 0},
        {0, 20, 110, 10, 0, submenu, kSubMenuTarget, 0,
         static_cast<u16>(menu_layout::kItemText | menu_layout::kItemEnabled), 0},
    };
    const menu_layout::Item subs[2] = {
        {0, 0, 90, 10, 0, sub0, 0, 0,
         static_cast<u16>(menu_layout::kItemText | menu_layout::kItemEnabled), 0},
        {0, 10, 90, 10, 0, sub1, 0, 0,
         static_cast<u16>(menu_layout::kItemText | menu_layout::kItemEnabled), 0},
    };
    u8 shade[256];
    for (u32 i = 0; i < 256u; ++i) {
        shade[i] = static_cast<u8>(i);
    }
    for (u32 depth : {8u, 2u}) {
        const char* suffix = depth == 8 ? "_depth8" : "_depth2";
        run_scene(r, "menu", suffix, kW, kH, depth, 0, [&](const Canvas& c) {
            const menu_layout::Geometry g = menu_layout::compute(
                titles, 1, items, 3, subs, 2, chrome::Metrics{}.bar_height() + 1, 0, 2, kW);
            chrome::MenuChrome m{};
            m.width = kW;
            m.titles = titles;
            m.title_rects = g.title;
            m.title_count = 1;
            m.selected_title = 0;
            m.box = g.box;
            m.items = items;
            m.item_rects = g.item;
            m.item_count = 3;
            m.selected_item = 2;
            m.sub_box = g.sub_box;
            m.sub_items = subs;
            m.sub_item_rects = g.sub_item;
            m.sub_item_count = 2;
            m.selected_sub = 0;
            m.depth = depth;
            m.bar_height = chrome::Metrics{}.bar_height();
            m.metrics = chrome::Metrics{};
            m.bitmap_addr = c.bitmap;
            m.pens = default_pens();
            if (chrome::style_for_depth(depth) == chrome::Style::Modern) {
                m.strong_shade = shade;
                m.light_shade = shade;
            }
            native::Bus b(r.machine);
            raster::Bitmap bitmap{};
            CHECK(raster::load_bitmap(b, c.bitmap, bitmap));
            chrome::draw_menu(r.graphics, b, bitmap, Rect{0, 0, kW - 1, kH - 1}, m);
        });
    }
}

void put_new_screen(GfxRig& r, u32 at, u32 width, u32 height, u32 depth) {
    r.put16(at + ns_LeftEdge, 0);
    r.put16(at + ns_TopEdge, 0);
    r.put16(at + ns_Width, width);
    r.put16(at + ns_Height, height);
    r.put16(at + ns_Depth, depth);
    r.put8(at + ns_DetailPen, 0);
    r.put8(at + ns_BlockPen, 1);
    r.put16(at + ns_ViewModes, 0);
    r.put16(at + ns_Type, CUSTOMSCREEN);
    r.put32(at + ns_Font, 0);
    r.put32(at + ns_DefaultTitle, 0);
    r.put32(at + ns_CustomBitMap, 0);
}

void put_easy(GfxRig& r, u32 at, u32 flags, u32 title, u32 text, u32 gadgets) {
    r.put32(at + es_StructSize, EasyStruct_SIZEOF);
    r.put32(at + es_Flags, flags);
    r.put32(at + es_Title, title);
    r.put32(at + es_TextFormat, text);
    r.put32(at + es_GadgetFormat, gadgets);
}

u32 build_requester(GfxRig& r, u32 easy) {
    r.cpu().a[0] = 0;
    r.cpu().a[1] = easy;
    r.cpu().d[0] = 0;
    r.cpu().a[3] = 0;
    return r.result(Base::Intuition, intuition_lvo::BuildEasyRequestArgs);
}

void free_requester(GfxRig& r, u32 window) {
    r.cpu().a[0] = window;
    CHECK_EQ(r.call(Base::Intuition, intuition_lvo::FreeSysRequest), ExitReason::Halt);
}

void press(GfxRig& r, i32 x, i32 y) {
    r.intuition.pointer_event(x, y, kMouseLeft, kMouseLeft, 0, false);
}

void release(GfxRig& r, i32 x, i32 y) {
    r.intuition.pointer_event(x, y, 0, 0, kMouseLeft, false);
}

void clear_bitmap(GfxRig& r, u32 rp) {
    const u32 bitmap = r.get32(rp + rp_BitMap);
    CHECK(bitmap != 0);
    const u32 bytes = r.get16(bitmap + bm_BytesPerRow) * r.get16(bitmap + bm_Rows);
    const u32 depth = r.get8(bitmap + bm_Depth);
    CHECK(depth <= 8u);
    for (u32 plane = 0; plane < depth; ++plane) {
        const u32 base = r.get32(bitmap + bm_Planes + plane * 4);
        for (u32 i = 0; i < bytes; ++i) {
            r.put8(base + i, 0);
        }
    }
}

void requester_scenes(GfxRig& r) {
    const u32 title = r.str(kEasyTitle, "Requester");
    const u32 text = r.str(kEasyText, "first line\nsecond line");
    const u32 gadgets = r.str(kEasyGadgets, "Yes|No");
    for (u32 depth : {8u, 2u}) {
        const char* suffix = depth == 8 ? "_depth8" : "_depth2";
        put_new_screen(r, kNewScreen, 320, 200, depth);
        r.cpu().a[0] = kNewScreen;
        const u32 screen = r.result(Base::Intuition, intuition_lvo::OpenScreen);
        CHECK(screen != 0);
        r.intuition.set_default_screen(screen);
        put_easy(r, kEasy, 0, title, text, gadgets);
        const u32 window = build_requester(r, kEasy);
        CHECK(window != 0);
        const u32 rp = r.get32(window + wd_RPort);
        const i32 width = static_cast<i32>(r.get16(window + wd_Width));
        const i32 height = static_cast<i32>(r.get16(window + wd_Height));
        const u32 normal = pixel_hash(r, rp, width, height);
        expect_golden("requester", suffix, normal);

        CHECK(IntuitionTest::button_count(r.intuition, window) != 0);
        const Rect button = IntuitionTest::button(r.intuition, window, 0);
        CHECK(button.x1 >= button.x0 && button.y1 >= button.y0);
        const i32 left = static_cast<i16>(r.get16(window + wd_LeftEdge));
        const i32 top = static_cast<i16>(r.get16(window + wd_TopEdge));
        const i32 x = left + (button.x0 + button.x1) / 2;
        const i32 y = top + (button.y0 + button.y1) / 2;
        press(r, x, y);
        // The requester draws a held button when it handles the press.
        r.cpu().a[0] = window;
        r.cpu().a[1] = 0;
        r.cpu().d[0] = 0;
        CHECK_EQ(r.call(Base::Intuition, intuition_lvo::SysReqHandler), ExitReason::Halt);
        const u32 held = pixel_hash(r, rp, width, height);
        CHECK(held != normal);
        expect_golden("requester_held", suffix, held);
        release(r, x, y);

        clear_bitmap(r, rp);
        CHECK(normal != pixel_hash(r, rp, width, height));

        free_requester(r, window);
    }
}

}

int main() {
    GfxRig r;
    window_chrome_scenes(r);
    window_bar_scenes(r);
    screen_bar_scenes(r);
    prop_scenes(r);
    menu_scenes(r);
    requester_scenes(r);
    CHECK_EQ(g_mismatches, 0u);
    return check::finish("test_chrome_pixels");
}
