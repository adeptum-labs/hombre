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

#include "gadtools.hpp"

#include "gfx.hpp"
#include "graphics.hpp"
#include "utility.hpp"

namespace hombre {

using m68k::Machine;
using native::Bus;
using native::LogLine;

namespace {

constexpr char kGadToolsName[] = "gadtools.library";
constexpr char kIdString[] = "gadtools 60.0 (Hombre)\r\n";

// The private VisualInfo: the screen and its font.
constexpr u32 vi_Screen = 0;
constexpr u32 vi_TextAttr = 4;
constexpr u32 kVisualInfoBytes = 16;

// struct Gadget
constexpr u32 gg_NextGadget = 0;
constexpr u32 gg_Flags = 12;
constexpr u32 kGadgetBytes = 44;
constexpr u16 GFLG_GADGHNONE = 3;

// The 3.1 default pens of a bevel box: SHINEPEN and SHADOWPEN.
constexpr u32 kShinePen = 2;
constexpr u32 kShadowPen = 1;

// The DrawBevelBoxA tag that asks for a recessed box.
constexpr u32 GTBB_Recessed = 0x80080033;

// Gadget chains a walk visits at most.
constexpr u32 kMaxChain = 1024;
constexpr u32 kMaxTags = 256;

// struct NewMenu and its types and flags.
constexpr u32 nm_Type = 0;
constexpr u32 nm_Label = 2;
constexpr u32 nm_CommKey = 6;
constexpr u32 nm_Flags = 10;
constexpr u32 nm_MutualExclude = 12;
constexpr u32 nm_UserData = 16;
constexpr u32 NewMenu_SIZEOF = 20;
constexpr u8 NM_END = 0;
constexpr u8 NM_TITLE = 1;
constexpr u8 NM_ITEM = 2;
constexpr u8 NM_SUB = 3;
constexpr u8 MENU_IMAGE = 0x80;
constexpr u32 NM_BARLABEL = 0xFFFFFFFF;
constexpr u16 NM_MENUDISABLED = 0x0001;
constexpr u16 NM_ITEMDISABLED = 0x0010;

// struct Menu, with the user data gadtools keeps after it.
constexpr u32 mu_NextMenu = 0;
constexpr u32 mu_LeftEdge = 4;
constexpr u32 mu_TopEdge = 6;
constexpr u32 mu_Width = 8;
constexpr u32 mu_Height = 10;
constexpr u32 mu_Flags = 12;
constexpr u32 mu_MenuName = 14;
constexpr u32 mu_FirstItem = 18;
constexpr u32 Menu_SIZEOF = 30;
constexpr u16 MENUENABLED = 0x0001;

// struct MenuItem, with the user data gadtools keeps after it.
constexpr u32 mi_NextItem = 0;
constexpr u32 mi_LeftEdge = 4;
constexpr u32 mi_TopEdge = 6;
constexpr u32 mi_Width = 8;
constexpr u32 mi_Height = 10;
constexpr u32 mi_Flags = 12;
constexpr u32 mi_MutualExclude = 14;
constexpr u32 mi_ItemFill = 18;
constexpr u32 mi_Command = 26;
constexpr u32 mi_SubItem = 28;
constexpr u32 MenuItem_SIZEOF = 34;
constexpr u16 CHECKIT = 0x0001;
constexpr u16 ITEMTEXT = 0x0002;
constexpr u16 COMMSEQ = 0x0004;
constexpr u16 MENUTOGGLE = 0x0008;
constexpr u16 ITEMENABLED = 0x0010;
constexpr u16 HIGHCOMP = 0x0040;
constexpr u16 CHECKED = 0x0100;

// struct IntuiText and struct Image.
constexpr u32 it_FrontPen = 0;
constexpr u32 it_DrawMode = 2;
constexpr u32 it_LeftEdge = 4;
constexpr u32 it_TopEdge = 6;
constexpr u32 it_IText = 12;
constexpr u32 IntuiText_SIZEOF = 20;
constexpr u32 im_Width = 4;
constexpr u32 im_Height = 6;
constexpr u32 Image_SIZEOF = 20;

// Every menu strip gadtools makes lives in one block: its size first,
// then the structures. FreeMenus frees the block from the first Menu.
constexpr u32 kMenuBlockHeader = 8;
constexpr u32 kMenuBlockMagic = 0x474D4E55;  // "GMNU"
constexpr u32 kMaxNewMenus = 512;
constexpr u32 kMenuText = 8;  // pixels per character of the 8x8 font
constexpr u32 kItemHeight = 9;

}

struct GadToolsNatives {
    static GadTools& of(void* ctx) { return *static_cast<GadTools*>(ctx); }

    static void get_visual_info(void* ctx, Machine& m) {
        GadTools& g = of(ctx);
        const u32 screen = m.cpu.a[0];
        if (screen == 0) {
            native::set_result(m, 0);
            return;
        }
        const u32 vi = g.exec_->alloc(kVisualInfoBytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
        if (vi == 0) {
            native::set_result(m, 0);
            return;
        }
        Bus b(m);
        b.w32(vi + vi_Screen, screen);
        b.w32(vi + vi_TextAttr, b.r32(screen + ndk::sc_Font));
        native::set_result(m, b.ok() ? vi : 0);
    }

    static void free_visual_info(void* ctx, Machine& m) {
        if (m.cpu.a[0] != 0) {
            of(ctx).exec_->free(m.cpu.a[0], kVisualInfoBytes);
        }
    }

    // The context gadget: an inert, empty gadget at the head of the list.
    static void create_context(void* ctx, Machine& m) {
        GadTools& g = of(ctx);
        const u32 list = m.cpu.a[0];
        const u32 gadget = g.exec_->alloc(kGadgetBytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
        if (gadget == 0) {
            native::set_result(m, 0);
            return;
        }
        Bus b(m);
        b.w16(gadget + gg_Flags, GFLG_GADGHNONE);
        if (list != 0) {
            b.w32(list, gadget);
        }
        native::set_result(m, b.ok() ? gadget : 0);
    }

    // Frees a list made by CreateContext and CreateGadgetA.
    static void free_gadgets(void* ctx, Machine& m) {
        GadTools& g = of(ctx);
        Bus b(m);
        u32 gadget = m.cpu.a[0];
        for (u32 i = 0; gadget != 0 && i < kMaxChain && b.ok(); ++i) {
            const u32 next = b.r32(gadget + gg_NextGadget);
            g.exec_->free(gadget, kGadgetBytes);
            gadget = next;
        }
    }

    static u32 label_length(Bus& b, u32 label) {
        u32 n = 0;
        while (label != 0 && n < 255 && b.r8(label + n) != 0 && b.ok()) {
            ++n;
        }
        return n;
    }

    // Builds a Menu, MenuItem and IntuiText strip from the NewMenu array
    // at a0, ending at NM_END. Items follow the last title and subitems
    // the last item; bar labels become separator images. Positions come
    // from LayoutMenusA.
    static void create_menus(void* ctx, Machine& m) {
        GadTools& g = of(ctx);
        Bus b(m);
        const u32 array = m.cpu.a[0];
        if (array == 0) {
            native::set_result(m, 0);
            return;
        }
        u32 count = 0;
        u32 bytes = kMenuBlockHeader;
        for (; count < kMaxNewMenus; ++count) {
            const u8 type = static_cast<u8>(b.r8(array + count * NewMenu_SIZEOF + nm_Type));
            if (!b.ok()) {
                native::set_result(m, 0);
                return;
            }
            if (type == NM_END) {
                break;
            }
            const u8 kind = type & ~MENU_IMAGE;
            bytes += kind == NM_TITLE ? Menu_SIZEOF + 4
                                      : MenuItem_SIZEOF + 4 + IntuiText_SIZEOF + Image_SIZEOF;
        }
        if (count == kMaxNewMenus) {
            native::set_result(m, 0);
            return;
        }
        const u32 block = g.exec_->alloc(bytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
        if (block == 0) {
            native::set_result(m, 0);
            return;
        }
        b.w32(block, kMenuBlockMagic);
        b.w32(block + 4, bytes);
        u32 at = block + kMenuBlockHeader;
        u32 first_menu = 0;
        u32 menu = 0;
        u32 item = 0;
        u32 last_item = 0;
        u32 last_sub = 0;
        for (u32 i = 0; i < count && b.ok(); ++i) {
            const u32 nm = array + i * NewMenu_SIZEOF;
            const u8 kind = static_cast<u8>(b.r8(nm + nm_Type)) & ~MENU_IMAGE;
            const u32 label = b.r32(nm + nm_Label);
            const u32 comm = b.r32(nm + nm_CommKey);
            const u16 flags = static_cast<u16>(b.r16(nm + nm_Flags));
            const u32 exclude = b.r32(nm + nm_MutualExclude);
            const u32 user = b.r32(nm + nm_UserData);
            if (kind == NM_TITLE) {
                const u32 mu = at;
                at += Menu_SIZEOF + 4;
                b.w16(mu + mu_Flags, (flags & NM_MENUDISABLED) != 0 ? 0 : MENUENABLED);
                b.w32(mu + mu_MenuName, label);
                b.w32(mu + Menu_SIZEOF, user);
                if (menu != 0) {
                    b.w32(menu + mu_NextMenu, mu);
                } else {
                    first_menu = mu;
                }
                menu = mu;
                item = 0;
                last_item = 0;
                continue;
            }
            if (menu == 0 || (kind == NM_SUB && last_item == 0)) {
                continue;  // an item before any title, or a subitem before any item
            }
            const u32 mi = at;
            at += MenuItem_SIZEOF + 4;
            const u32 fill = at;
            at += IntuiText_SIZEOF + Image_SIZEOF;
            u16 item_flags = HIGHCOMP | ((flags & NM_ITEMDISABLED) != 0 ? 0 : ITEMENABLED);
            item_flags |= flags & (CHECKIT | CHECKED | MENUTOGGLE);
            if (label == NM_BARLABEL) {
                b.w32(mi + mi_ItemFill, fill + IntuiText_SIZEOF);
                item_flags &= static_cast<u16>(~ITEMENABLED);
            } else {
                item_flags |= ITEMTEXT;
                b.w8(fill + it_FrontPen, 0);
                b.w8(fill + it_DrawMode, ndk::JAM1);
                b.w16(fill + it_LeftEdge, 2);
                b.w16(fill + it_TopEdge, 1);
                b.w32(fill + it_IText, label);
                b.w32(mi + mi_ItemFill, fill);
            }
            if (comm != 0) {
                const u32 key = b.r8(comm);
                if (key != 0) {
                    item_flags |= COMMSEQ;
                    b.w8(mi + mi_Command, key);
                }
            }
            b.w16(mi + mi_Flags, item_flags);
            b.w32(mi + mi_MutualExclude, exclude);
            b.w32(mi + MenuItem_SIZEOF, user);
            if (kind == NM_ITEM) {
                if (item == 0) {
                    b.w32(menu + mu_FirstItem, mi);
                } else {
                    b.w32(item + mi_NextItem, mi);
                }
                item = mi;
                last_item = mi;
                last_sub = 0;
            } else {
                if (last_sub == 0) {
                    b.w32(last_item + mi_SubItem, mi);
                } else {
                    b.w32(last_sub + mi_NextItem, mi);
                }
                last_sub = mi;
            }
        }
        if (!b.ok() || first_menu == 0) {
            g.exec_->free(block, bytes);
            native::set_result(m, 0);
            return;
        }
        native::set_result(m, first_menu);
    }

    static void free_menus(void* ctx, Machine& m) {
        GadTools& g = of(ctx);
        Bus b(m);
        const u32 menu = m.cpu.a[0];
        if (menu == 0) {
            return;
        }
        const u32 block = menu - kMenuBlockHeader;
        if (b.r32(block) != kMenuBlockMagic || !b.ok()) {
            LogLine().text("gadtools: FreeMenus of a strip it did not make").send();
            return;
        }
        g.exec_->free(block, b.r32(block + 4));
    }

    // Lays out items (and their subitems) in a column below their title,
    // as wide as their widest label, and titles side by side.
    static u32 layout_items(Bus& b, u32 first, i32 left, i32 top) {
        u32 widest = 0;
        u32 count = 0;
        for (u32 it = first; it != 0 && count < kMaxNewMenus && b.ok();
             it = b.r32(it + mi_NextItem), ++count) {
            const u16 flags = static_cast<u16>(b.r16(it + mi_Flags));
            u32 w = 0;
            if ((flags & ITEMTEXT) != 0) {
                w = label_length(b, b.r32(b.r32(it + mi_ItemFill) + it_IText)) * kMenuText + 4;
            }
            if ((flags & COMMSEQ) != 0) {
                w += 4 * kMenuText;
            }
            if ((flags & CHECKIT) != 0) {
                w += 2 * kMenuText;
            }
            widest = w > widest ? w : widest;
        }
        widest = widest < 4 * kMenuText ? 4 * kMenuText : widest;
        i32 y = top;
        count = 0;
        for (u32 it = first; it != 0 && count < kMaxNewMenus && b.ok();
             it = b.r32(it + mi_NextItem), ++count) {
            const u16 flags = static_cast<u16>(b.r16(it + mi_Flags));
            const u32 h = (flags & ITEMTEXT) != 0 ? kItemHeight : 4;
            b.w16(it + mi_LeftEdge, static_cast<u16>(left));
            b.w16(it + mi_TopEdge, static_cast<u16>(y));
            b.w16(it + mi_Width, static_cast<u16>(widest));
            b.w16(it + mi_Height, static_cast<u16>(h));
            if ((flags & ITEMTEXT) == 0) {
                const u32 image = b.r32(it + mi_ItemFill);
                b.w16(image + im_Width, static_cast<u16>(widest - 4));
                b.w16(image + im_Height, 2);
            }
            const u32 sub = b.r32(it + mi_SubItem);
            if (sub != 0) {
                layout_items(b, sub, static_cast<i32>(widest) - 2 * static_cast<i32>(kMenuText), 0);
            }
            y += static_cast<i32>(h);
        }
        return widest;
    }

    static void layout_menus(void*, Machine& m) {
        Bus b(m);
        i32 left = 0;
        u32 count = 0;
        for (u32 mu = m.cpu.a[0]; mu != 0 && count < kMaxNewMenus && b.ok();
             mu = b.r32(mu + mu_NextMenu), ++count) {
            const u32 width = (label_length(b, b.r32(mu + mu_MenuName)) + 2) * kMenuText;
            b.w16(mu + mu_LeftEdge, static_cast<u16>(left));
            b.w16(mu + mu_TopEdge, 0);
            b.w16(mu + mu_Width, static_cast<u16>(width));
            b.w16(mu + mu_Height, static_cast<u16>(kItemHeight));
            layout_items(b, b.r32(mu + mu_FirstItem), 0, static_cast<i32>(kItemHeight) + 2);
            left += static_cast<i32>(width);
        }
        native::set_result(m, b.ok() ? 1 : 0);
    }

    static void layout_menu_items(void*, Machine& m) {
        Bus b(m);
        if (m.cpu.a[0] != 0) {
            layout_items(b, m.cpu.a[0], 0, static_cast<i32>(kItemHeight) + 2);
        }
        native::set_result(m, b.ok() ? 1 : 0);
    }

    static void get_imsg(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).exec_->get_message(m.cpu.a[0]));
    }

    static void reply_imsg(void* ctx, Machine& m) {
        if (m.cpu.a[1] != 0) {
            of(ctx).exec_->reply_message(m.cpu.a[1]);
        }
    }

    static void filter_imsg(void*, Machine& m) { native::set_result(m, m.cpu.a[1]); }

    static void post_filter_imsg(void*, Machine& m) { native::set_result(m, m.cpu.a[1]); }

    // Gadgets draw themselves through intuition, so there is nothing to
    // refresh here.
    static void refresh_window(void*, Machine&) {}

    static void begin_refresh(void*, Machine&) {}

    static void end_refresh(void*, Machine&) {}

    // A raised box: shine on the top and left edges, shadow on the bottom
    // and right; GTBB_Recessed swaps them.
    static void draw_bevel_box(void* ctx, Machine& m) {
        GadTools& g = of(ctx);
        const u32 rp = m.cpu.a[0];
        const i32 left = static_cast<i32>(static_cast<i16>(m.cpu.d[0]));
        const i32 top = static_cast<i32>(static_cast<i16>(m.cpu.d[1]));
        const i32 width = static_cast<i32>(static_cast<i16>(m.cpu.d[2]));
        const i32 height = static_cast<i32>(static_cast<i16>(m.cpu.d[3]));
        if (rp == 0 || width <= 0 || height <= 0) {
            return;
        }
        Bus b(m);
        bool recessed = false;
        u32 item = m.cpu.a[1];
        for (u32 i = 0; item != 0 && i < kMaxTags; ++i) {
            const u32 tag = b.r32(item);
            const u32 data = b.r32(item + 4);
            if (!b.ok() || tag == ndk::TAG_DONE) {
                break;
            }
            if (tag == ndk::TAG_MORE) {
                item = data;
                continue;
            }
            if (tag == GTBB_Recessed) {
                recessed = data != 0;
            }
            item += 8;
        }
        const u32 light = recessed ? kShadowPen : kShinePen;
        const u32 dark = recessed ? kShinePen : kShadowPen;
        const i32 right = left + width - 1;
        const i32 bottom = top + height - 1;
        Graphics& gfx = *g.graphics_;
        gfx.set_pens(rp, light, 0, ndk::JAM1);
        gfx.move(rp, left, bottom);
        gfx.draw_line(rp, left, top);
        gfx.draw_line(rp, right, top);
        gfx.set_pens(rp, dark, 0, ndk::JAM1);
        gfx.move(rp, right, top + 1);
        gfx.draw_line(rp, right, bottom);
        gfx.draw_line(rp, left + 1, bottom);
    }
};

namespace {

struct GadToolsFunction {
    u32 lvo;
    m68k::NativeFn fn;
    const char* name;
};

constexpr GadToolsFunction kGadToolsFunctions[] = {
    {gadtools_lvo::FreeGadgets, GadToolsNatives::free_gadgets, "FreeGadgets"},
    {gadtools_lvo::CreateMenusA, GadToolsNatives::create_menus, "CreateMenusA"},
    {gadtools_lvo::FreeMenus, GadToolsNatives::free_menus, "FreeMenus"},
    {gadtools_lvo::LayoutMenuItemsA, GadToolsNatives::layout_menu_items, "LayoutMenuItemsA"},
    {gadtools_lvo::LayoutMenusA, GadToolsNatives::layout_menus, "LayoutMenusA"},
    {gadtools_lvo::GT_GetIMsg, GadToolsNatives::get_imsg, "GT_GetIMsg"},
    {gadtools_lvo::GT_ReplyIMsg, GadToolsNatives::reply_imsg, "GT_ReplyIMsg"},
    {gadtools_lvo::GT_RefreshWindow, GadToolsNatives::refresh_window, "GT_RefreshWindow"},
    {gadtools_lvo::GT_BeginRefresh, GadToolsNatives::begin_refresh, "GT_BeginRefresh"},
    {gadtools_lvo::GT_EndRefresh, GadToolsNatives::end_refresh, "GT_EndRefresh"},
    {gadtools_lvo::GT_FilterIMsg, GadToolsNatives::filter_imsg, "GT_FilterIMsg"},
    {gadtools_lvo::GT_PostFilterIMsg, GadToolsNatives::post_filter_imsg, "GT_PostFilterIMsg"},
    {gadtools_lvo::CreateContext, GadToolsNatives::create_context, "CreateContext"},
    {gadtools_lvo::DrawBevelBoxA, GadToolsNatives::draw_bevel_box, "DrawBevelBoxA"},
    {gadtools_lvo::GetVisualInfoA, GadToolsNatives::get_visual_info, "GetVisualInfoA"},
    {gadtools_lvo::FreeVisualInfo, GadToolsNatives::free_visual_info, "FreeVisualInfo"},
};

}

bool GadTools::init(Machine& m, Exec& exec, Graphics& graphics) {
    machine_ = &m;
    exec_ = &exec;
    graphics_ = &graphics;
    base_ = 0;
    for (NativeVector& v : vectors_) {
        v = NativeVector{nullptr, nullptr};
    }
    for (const GadToolsFunction& f : kGadToolsFunctions) {
        vectors_[f.lvo / ndk::LIB_VECTSIZE - 1] = NativeVector{f.fn, f.name};
    }
    const u32 id_string = exec.alloc(sizeof kIdString, ndk::MEMF_PUBLIC);
    if (id_string == 0) {
        return false;
    }
    const u32 base = exec.make_native_library(kGadToolsName, kHombreLibVersion, vectors_,
                                              kVectors, this);
    if (base == 0) {
        exec.free(id_string, sizeof kIdString);
        return false;
    }
    Bus b(m);
    for (u32 i = 0; i < sizeof kIdString; ++i) {
        b.w8(id_string + i, static_cast<u8>(kIdString[i]));
    }
    b.w16(base + ndk::LIB_REVISION, kHombreLibRevision);
    b.w32(base + ndk::LIB_IDSTRING, id_string);
    if (!b.ok()) {
        return false;
    }
    base_ = base;
    return true;
}

}
