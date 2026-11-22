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

#include "chrome.hpp"
#include "intuition.hpp"
#include "sysimage.hpp"

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

// The private VisualInfo: the screen, the TextAttr of its font, that font
// held open, and a mark that tells one from any other pointer.
constexpr u32 vi_Screen = 0;
constexpr u32 vi_TextAttr = 4;
constexpr u32 vi_Font = 8;
constexpr u32 vi_Magic = 12;
constexpr u32 kVisualInfoBytes = 16;
constexpr u32 kVisualInfoMagic = 0x47545649;  // "GTVI"

// struct Gadget
constexpr u32 gg_NextGadget = 0;
constexpr u32 gg_Flags = 12;
constexpr u16 GFLG_GADGHNONE = 3;

// Every gadget gadtools makes is a struct Gadget followed by what gadtools
// keeps of it: its kind, the label and its placement, the text or number
// it shows, whether it has a frame, and a copy of its text it owns.
constexpr u32 gt_Kind = 44;
constexpr u32 gt_Place = 46;
constexpr u32 gt_Label = 48;
constexpr u32 gt_Text = 52;
constexpr u32 gt_Number = 56;
constexpr u32 gt_Options = 60;
constexpr u32 gt_Underscore = 62;
constexpr u32 gt_CopyBytes = 64;
// A cycle gadget's number of labels, a palette's number of colours, the
// most characters a string or integer gadget takes.
constexpr u32 gt_Count = 68;
constexpr u32 gt_ColorOffset = 70;
constexpr u32 gt_ColorTable = 72;
constexpr u32 gt_IndicatorWidth = 76;
constexpr u32 gt_IndicatorHeight = 78;
// A string gadget's StringInfo, whose buffer is the text copy.
constexpr u32 gt_StringInfo = 80;
// A scroller's PropInfo, which Intuition moves.
constexpr u32 gt_PropInfo = gt_StringInfo + ndk::StringInfo_SIZEOF;
// A list view is four gadgets in a row: its scroller, the arrow that moves
// it up, the one that moves it down, and last the list itself, which is
// the gadget CreateGadgetA returns. Each of the three parts knows the list
// it belongs to; the list knows its scroller, the string gadget that shows
// its selected label, an ordinal still to be scrolled into view, the height
// of a row and the width of the scroller.
constexpr u32 gt_Owner = gt_PropInfo + ndk::PropInfo_SIZEOF + 2;
constexpr u32 gt_Scroller = gt_Owner + 4;
constexpr u32 gt_Show = gt_Scroller + 4;
constexpr u32 gt_MakeVisible = gt_Show + 4;
constexpr u32 gt_RowHeight = gt_MakeVisible + 4;
constexpr u32 gt_Spacing = gt_RowHeight + 2;
constexpr u32 gt_ScrollWidth = gt_Spacing + 2;
// The font of the gadget's label and text, held open until FreeGadgets;
// 0 for the built-in one. A string gadget's StringExtend names it to
// Intuition, which draws the cursor.
constexpr u32 gt_Font = gt_ScrollWidth + 2;
constexpr u32 gt_StringExtend = gt_Font + 4;
// How a slider shows its level: the format string and the display function
// the program gave, the one argument the format takes, the characters its
// field holds and where the field is placed.
constexpr u32 gt_LevelFormat = gt_StringExtend + ndk::StringExtend_SIZEOF;
constexpr u32 gt_LevelFunc = gt_LevelFormat + 4;
constexpr u32 gt_LevelArg = gt_LevelFunc + 4;
constexpr u32 gt_LevelLen = gt_LevelArg + 4;
constexpr u32 gt_LevelPlace = gt_LevelLen + 2;
constexpr u32 kGadgetBytes = gt_LevelPlace + 2;
constexpr u32 kMaxCycleLabels = 256;
constexpr u32 kMaxPaletteColors = 256;
constexpr u16 kOptBorder = 0x1;
constexpr u16 kOptScaled = 0x4;

// Gadget kinds (libraries/gadtools.h).
constexpr u32 GENERIC_KIND = 0;
constexpr u32 BUTTON_KIND = 1;
constexpr u32 CHECKBOX_KIND = 2;
constexpr u32 INTEGER_KIND = 3;
constexpr u32 LISTVIEW_KIND = 4;
constexpr u32 NUMBER_KIND = 6;
constexpr u32 CYCLE_KIND = 7;
constexpr u32 PALETTE_KIND = 8;
constexpr u32 SCROLLER_KIND = 9;
constexpr u32 SLIDER_KIND = 11;
constexpr u32 STRING_KIND = 12;
constexpr u32 TEXT_KIND = 13;
constexpr u32 kKinds = 14;
// The parts of a list view, which no program asks for.
constexpr u32 kListScrollerKind = 0x80;
constexpr u32 kListUpKind = 0x81;
constexpr u32 kListDownKind = 0x82;

// struct NewGadget.
constexpr u32 ng_LeftEdge = 0;
constexpr u32 ng_TopEdge = 2;
constexpr u32 ng_Width = 4;
constexpr u32 ng_Height = 6;
constexpr u32 ng_GadgetText = 8;
constexpr u32 ng_TextAttr = 12;
constexpr u32 ng_GadgetID = 16;
constexpr u32 ng_Flags = 18;
constexpr u32 ng_VisualInfo = 22;
constexpr u32 ng_UserData = 26;

// Label placement, ng_Flags.
constexpr u32 PLACETEXT_LEFT = 0x1;
constexpr u32 PLACETEXT_RIGHT = 0x2;
constexpr u32 PLACETEXT_ABOVE = 0x4;
constexpr u32 PLACETEXT_BELOW = 0x8;
constexpr u32 PLACETEXT_IN = 0x10;

// The tags of the gadgets gadtools makes.
constexpr u32 GA_Disabled = 0x8003000E;
constexpr u32 GTTX_Text = 0x8008000B;
constexpr u32 GTTX_CopyText = 0x8008000C;
constexpr u32 GTNM_Number = 0x8008000D;
constexpr u32 GTCB_Checked = 0x80080004;
constexpr u32 GTCB_Scaled = 0x80080044;
constexpr u32 GTCY_Labels = 0x8008000E;
constexpr u32 GTCY_Active = 0x8008000F;
constexpr u32 GTPA_Depth = 0x80080010;
constexpr u32 GTPA_Color = 0x80080011;
constexpr u32 GTPA_ColorOffset = 0x80080012;
constexpr u32 GTPA_IndicatorWidth = 0x80080013;
constexpr u32 GTPA_IndicatorHeight = 0x80080014;
constexpr u32 GTPA_NumColors = 0x80080046;
constexpr u32 GTPA_ColorTable = 0x80080052;
constexpr u32 GTSC_Top = 0x80080015;
constexpr u32 GTSC_Total = 0x80080016;
constexpr u32 GTSC_Visible = 0x80080017;
constexpr u32 GTST_String = 0x8008002D;
constexpr u32 GTST_MaxChars = 0x8008002E;
constexpr u32 GTIN_Number = 0x8008002F;
constexpr u32 GTIN_MaxChars = 0x80080030;
constexpr u32 PGA_Freedom = 0x80031001;
constexpr u32 LORIENT_HORIZ = 1;
// A scroller keeps its total where a palette keeps its colour table, how
// much shows where a palette keeps its count, and its orientation where a
// palette keeps its colour offset.
constexpr u32 gt_Total = 72;
constexpr u32 gt_Visible = 68;
constexpr u32 gt_Freedom = 70;
constexpr u32 kDefaultMaxChars = 64;
constexpr u32 kDefaultIntMaxChars = 10;
// A slider is a scroller over its levels: its level where a scroller keeps
// its top, its maximum where a scroller keeps its total, and its minimum,
// a WORD, where a palette keeps its indicator width.
constexpr u32 gt_Min = 76;
constexpr u32 GTSL_Min = 0x80080026;
constexpr u32 GTSL_Max = 0x80080027;
constexpr u32 GTSL_Level = 0x80080028;
constexpr u32 GTSL_MaxLevelLen = 0x80080029;
constexpr u32 GTSL_LevelFormat = 0x8008002A;
constexpr u32 GTSL_LevelPlace = 0x8008002B;
constexpr u32 GTSL_DispFunc = 0x8008002C;
constexpr u32 kDefaultSliderMax = 15;
// Blocks of 68000 code a display function may run for one level.
constexpr u64 kLevelFuncBlocks = 1ull << 20;
// The characters of a level's text kept, as many as a format can usefully give.
constexpr u32 kMaxLevelText = 128;
// A list view keeps its struct List where a text display keeps its text,
// its selected ordinal, ~0 for none, where a number keeps its number, its
// top ordinal where a scroller keeps its total, and the number of rows it
// shows where a scroller keeps how much shows.
constexpr u32 GTLV_Top = 0x80080005;
constexpr u32 GTLV_Labels = 0x80080006;
constexpr u32 GTLV_ReadOnly = 0x80080007;
constexpr u32 GTLV_ScrollWidth = 0x80080008;
constexpr u32 GTLV_ShowSelected = 0x80080035;
constexpr u32 GTLV_Selected = 0x80080036;
constexpr u32 GTLV_ItemHeight = 0x8008004D;
constexpr u32 GTLV_MakeVisible = 0x8008004E;
constexpr u32 LAYOUTA_Spacing = 0x80038002;
constexpr u16 kOptReadOnly = 0x2;
// GTLV_ShowSelected was given: the selected row shows in the fill pens.
constexpr u16 kOptShowSelected = 0x8;
constexpr u32 kMaxListNodes = 4096;
constexpr u32 kNoOrdinal = 0xFFFFFFFF;
constexpr u32 kDefaultScrollWidth = 16;
// The height of each of a list view's two arrows, and the frame around
// its rows.
constexpr i32 kListArrowHeight = 10;
constexpr i32 kListFrame = 2;
// The largest arrow, in pixels, painted in the look of the sliders' knobs.
constexpr u32 kMaxArrowPixels = 64 * 16;
constexpr u32 GTTX_Border = 0x80080039;
constexpr u32 GTNM_Border = 0x8008003A;
constexpr u32 GT_Underscore = 0x80080040;

// The 3.1 default pens of a pressed button: FILLPEN and FILLTEXTPEN.
constexpr u32 kFillPen = 3;
constexpr u32 kFillTextPen = 2;
constexpr u32 kTextPen = 1;
constexpr u32 kBackgroundPen = 0;
// The gap between a gadget and a label beside it.
constexpr i32 kLabelGap = 8;
constexpr u32 kCheckBoxWidth = 26;
constexpr u32 kCheckBoxHeight = 11;

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
constexpr u32 it_ITextFont = 8;
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
constexpr u32 kItemLeading = 2;  // a text item: the font and a row of space above and below
constexpr u32 kBarItemHeight = 6;  // a separator bar
constexpr u32 kItemPadding = 8;  // columns around an item's label
constexpr u32 kTitlePadding = 8;  // columns on either side of a title's name
// Beside the two cells a shortcut takes: its key glyph and the gaps.
constexpr u32 kShortcutSpace = 16;
constexpr u32 kCheckSpace = 16;  // the column of the check marks
constexpr u32 GTMN_TextAttr = 0x80080031;

}

namespace {

// The pens, draw mode and pen position of a RastPort, put back when it
// goes: gadgets draw through the window's RastPort, whose state belongs
// to the program.
class RastPortState {
public:
    RastPortState(Bus& b, u32 rp)
        : b_(b),
          rp_(rp),
          fg_(b.r8(rp + ndk::rp_FgPen)),
          bg_(b.r8(rp + ndk::rp_BgPen)),
          mode_(b.r8(rp + ndk::rp_DrawMode)),
          x_(b.r16(rp + ndk::rp_cp_x)),
          y_(b.r16(rp + ndk::rp_cp_y)) {}
    ~RastPortState() {
        b_.w8(rp_ + ndk::rp_FgPen, fg_);
        b_.w8(rp_ + ndk::rp_BgPen, bg_);
        b_.w8(rp_ + ndk::rp_DrawMode, mode_);
        b_.w16(rp_ + ndk::rp_cp_x, x_);
        b_.w16(rp_ + ndk::rp_cp_y, y_);
    }
    RastPortState(const RastPortState&) = delete;
    RastPortState& operator=(const RastPortState&) = delete;

private:
    Bus& b_;
    u32 rp_;
    u32 fg_;
    u32 bg_;
    u32 mode_;
    u32 x_;
    u32 y_;
};

// The nodes of the struct List at list, at most kMaxListNodes.
u32 list_count(Bus& b, u32 list) {
    u32 count = 0;
    if (list == 0) {
        return 0;
    }
    for (u32 node = b.r32(list); node != 0 && b.r32(node) != 0 && count < kMaxListNodes &&
                                 b.ok();
         node = b.r32(node)) {
        ++count;
    }
    return count;
}

// The node of the given ordinal of the struct List at list, 0 past its end.
u32 list_node(Bus& b, u32 list, u32 ordinal) {
    if (list == 0 || ordinal >= kMaxListNodes) {
        return 0;
    }
    u32 node = b.r32(list);
    for (u32 i = 0; node != 0 && b.r32(node) != 0 && b.ok(); node = b.r32(node), ++i) {
        if (i == ordinal) {
            return node;
        }
    }
    return 0;
}

}  // namespace

struct GadToolsNatives {
    using MenuFont = GadTools::MenuFont;

    static GadTools& of(void* ctx) { return *static_cast<GadTools*>(ctx); }

    static void set_integer_number(GadTools& g, Bus& b, u32 gadget, i32 value);

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
        const u32 attr = b.r32(screen + ndk::sc_Font);
        b.w32(vi + vi_Screen, screen);
        b.w32(vi + vi_TextAttr, attr);
        b.w32(vi + vi_Font, attr != 0 && b.ok() ? g.graphics_->open_font(attr) : 0);
        b.w32(vi + vi_Magic, kVisualInfoMagic);
        native::set_result(m, b.ok() ? vi : 0);
    }

    static void free_visual_info(void* ctx, Machine& m) {
        GadTools& g = of(ctx);
        const u32 vi = m.cpu.a[0];
        if (vi == 0) {
            return;
        }
        Bus b(m);
        g.graphics_->close_font(b.r32(vi + vi_Font));
        b.w32(vi + vi_Magic, 0);
        g.exec_->free(vi, kVisualInfoBytes);
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
        b.w16(gadget + ndk::gg_GadgetType, ndk::GTYP_GADTOOLS | ndk::GTYP_BOOLGADGET);
        b.w16(gadget + gt_Kind, GENERIC_KIND);
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
            // A gadget of the program's own in the chain is its to free.
            if ((b.r16(gadget + ndk::gg_GadgetType) & ndk::GTYP_GADTOOLS) != 0) {
                drop_text_copy(g, b, gadget);
                g.graphics_->close_font(b.r32(gadget + gt_Font));
                g.exec_->free(gadget, kGadgetBytes);
            }
            gadget = next;
        }
    }

    static void drop_text_copy(GadTools& g, Bus& b, u32 gadget) {
        const u32 bytes = b.r32(gadget + gt_CopyBytes);
        const u32 text = b.r32(gadget + gt_Text);
        if (b.ok() && bytes != 0 && text != 0) {
            g.exec_->free(text, bytes);
        }
        b.w32(gadget + gt_CopyBytes, 0);
    }

    // GTTX_Text: the text the gadget shows; with GTTX_CopyText gadtools
    // keeps a copy of its own.
    // A string gadget's copy is its buffer, at least kDefaultMaxChars long
    // and there even for no text.
    static void set_text(GadTools& g, Bus& b, u32 gadget, u32 text, bool copy) {
        drop_text_copy(g, b, gadget);
        const u32 kind = b.r16(gadget + gt_Kind);
        const bool buffer = kind == STRING_KIND || kind == INTEGER_KIND;
        if (!copy || (text == 0 && !buffer)) {
            b.w32(gadget + gt_Text, text);
            return;
        }
        const u32 length = text != 0 ? label_length(b, text) : 0;
        const u32 max_chars = kind == INTEGER_KIND ? b.r16(gadget + gt_Count) + 1 : 0;
        const u32 undo = kind == INTEGER_KIND ? max_chars : 0;
        // A string gadget holds GTST_MaxChars characters and the NUL after
        // them, 64 with it unless the tag says otherwise.
        const u32 string_chars =
            kind == STRING_KIND && b.r16(gadget + gt_Count) != 0 ? b.r16(gadget + gt_Count) + 1
                                                                 : kDefaultMaxChars;
        u32 bytes = buffer && length + 1 < string_chars ? string_chars : length + 1;
        bytes = bytes < max_chars ? max_chars : bytes;
        const u32 at = g.exec_->alloc(bytes + undo, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
        if (at == 0) {
            b.w32(gadget + gt_Text, 0);
            return;
        }
        for (u32 i = 0; i < length; ++i) {
            b.w8(at + i, b.r8(text + i));
        }
        b.w32(gadget + gt_Text, at);
        b.w32(gadget + gt_CopyBytes, bytes + undo);
        if (buffer) {
            const u32 si = gadget + gt_StringInfo;
            b.w32(si + ndk::si_Buffer, at);
            b.w32(si + ndk::si_UndoBuffer, undo != 0 ? at + bytes : 0);
            b.w16(si + ndk::si_MaxChars, kind == INTEGER_KIND ? max_chars : bytes);
            b.w16(si + ndk::si_NumChars, length);
            b.w16(si + ndk::si_BufferPos, length);
            b.w16(si + ndk::si_DispPos, 0);
            b.w32(gadget + ndk::gg_SpecialInfo, si);
        }
    }

    // The tags a gadget takes at CreateGadgetA and GT_SetGadgetAttrsA.
    static void apply_tags(GadTools& g, Bus& b, u32 gadget, u32 tags, bool creating,
                           u32 window = 0) {
        bool copy = false;
        u32 text = 0;
        bool has_text = false;
        bool selection = false;
        for (u32 item = tags, i = 0; item != 0 && i < kMaxTags && b.ok(); ++i) {
            const u32 tag = b.r32(item);
            const u32 data = b.r32(item + 4);
            if (tag == ndk::TAG_DONE) {
                break;
            }
            if (tag == ndk::TAG_MORE) {
                item = data;
                continue;
            }
            const u16 options = static_cast<u16>(b.r16(gadget + gt_Options));
            switch (tag) {
            case GA_Disabled: {
                const u32 flags = b.r16(gadget + gg_Flags);
                b.w16(gadget + gg_Flags,
                      data != 0 ? flags | ndk::GFLG_DISABLED : flags & ~ndk::GFLG_DISABLED);
                break;
            }
            case GTTX_Text:
                text = data;
                has_text = true;
                break;
            case GTTX_CopyText:
                copy = creating && data != 0;
                break;
            case GTCB_Checked: {
                const u32 flags = b.r16(gadget + gg_Flags);
                b.w16(gadget + gg_Flags,
                      data != 0 ? flags | ndk::GFLG_SELECTED : flags & ~ndk::GFLG_SELECTED);
                break;
            }
            case GTCB_Scaled:
                if (creating) {
                    b.w16(gadget + gt_Options,
                          data != 0 ? options | kOptScaled : options & ~kOptScaled);
                }
                break;
            case GTNM_Number:
            case GTCY_Active:
            case GTPA_Color:
                b.w32(gadget + gt_Number, data);
                break;
            case GTCY_Labels: {
                u32 count = 0;
                while (data != 0 && count < kMaxCycleLabels && b.r32(data + count * 4) != 0 &&
                       b.ok()) {
                    ++count;
                }
                b.w32(gadget + gt_Text, data);
                b.w16(gadget + gt_Count, count);
                break;
            }
            case GTPA_Depth:
                b.w16(gadget + gt_Count, data < 8 ? 1u << data : kMaxPaletteColors);
                break;
            case GTPA_NumColors:
                b.w16(gadget + gt_Count, data < kMaxPaletteColors ? data : kMaxPaletteColors);
                break;
            case GTPA_ColorOffset:
                b.w16(gadget + gt_ColorOffset, data);
                break;
            case GTPA_ColorTable:
                b.w32(gadget + gt_ColorTable, data);
                break;
            case GTPA_IndicatorWidth:
                b.w16(gadget + gt_IndicatorWidth, data);
                break;
            case GTPA_IndicatorHeight:
                b.w16(gadget + gt_IndicatorHeight, data);
                break;
            case GTSC_Top:
                b.w32(gadget + gt_Number, data);
                break;
            case GTSC_Total:
                b.w32(gadget + gt_Total, data);
                break;
            case GTSL_Min:
                b.w16(gadget + gt_Min, data);
                break;
            case GTSL_Max:
                b.w32(gadget + gt_Total, data);
                break;
            case GTSL_Level:
                b.w32(gadget + gt_Number, data);
                break;
            case GTSL_MaxLevelLen:
                if (creating) {
                    b.w16(gadget + gt_LevelLen, data < 0xFFFF ? data : 0xFFFF);
                }
                break;
            case GTSL_LevelFormat:
                if (creating) {
                    b.w32(gadget + gt_LevelFormat, data);
                }
                break;
            case GTSL_LevelPlace:
                if (creating && (data & 0x1F) != 0) {
                    b.w16(gadget + gt_LevelPlace, data & 0x1F);
                }
                break;
            case GTSL_DispFunc:
                if (creating) {
                    b.w32(gadget + gt_LevelFunc, data);
                }
                break;
            case GTLV_Labels:
                // ~0 takes the list away while the caller changes it.
                b.w32(gadget + gt_Text, data == 0xFFFFFFFF ? 0 : data);
                break;
            case GTLV_Top:
                b.w32(gadget + gt_Total, data & 0xFFFF);
                break;
            case GTLV_MakeVisible:
                // Scrolled into view once the rows and the labels are known.
                b.w32(gadget + gt_MakeVisible, data & 0xFFFF);
                break;
            case GTLV_Selected:
                // An ordinal is a UWORD, and ~0 of one is no selection.
                b.w32(gadget + gt_Number, (data & 0xFFFF) == 0xFFFF ? kNoOrdinal : data & 0xFFFF);
                selection = true;
                break;
            case GTLV_ReadOnly:
                if (creating) {
                    b.w16(gadget + gt_Options,
                          data != 0 ? options | kOptReadOnly : options & ~kOptReadOnly);
                }
                break;
            case GTLV_ShowSelected:
                if (creating) {
                    b.w32(gadget + gt_Show, data);
                    b.w16(gadget + gt_Options, options | kOptShowSelected);
                }
                break;
            case GTLV_ScrollWidth:
                if (creating && data != 0) {
                    b.w16(gadget + gt_ScrollWidth, data > 0x7FFF ? 0x7FFF : data);
                }
                break;
            case GTLV_ItemHeight:
                if (creating && data != 0) {
                    b.w16(gadget + gt_RowHeight, data > 0x7FFF ? 0x7FFF : data);
                }
                break;
            case LAYOUTA_Spacing:
                if (creating) {
                    b.w16(gadget + gt_Spacing, data > 0x7FFF ? 0x7FFF : data);
                }
                break;
            case GTSC_Visible:
                b.w16(gadget + gt_Visible, data > 0xFFFF ? 0xFFFF : data);
                break;
            case PGA_Freedom:
                if (creating) {
                    b.w16(gadget + gt_Freedom, data);
                }
                break;
            case GTST_String:
                // The string goes into the gadget's own buffer.
                text = data;
                has_text = true;
                copy = true;
                break;
            case GTIN_Number:
                b.w32(gadget + gt_Number, data);
                if (!creating && b.r16(gadget + gt_Kind) == INTEGER_KIND) {
                    set_integer_number(g, b, gadget, static_cast<i32>(data));
                }
                break;
            case GTIN_MaxChars:
            case GTST_MaxChars:
                // A string and an integer gadget take either tag.
                if (creating) {
                    b.w16(gadget + gt_Count, data < 0xFFFF ? data : 0xFFFE);
                }
                break;
            case GTTX_Border:
            case GTNM_Border:
                if (creating) {
                    b.w16(gadget + gt_Options,
                          data != 0 ? options | kOptBorder : options & ~kOptBorder);
                }
                break;
            case GT_Underscore:
                if (creating) {
                    b.w8(gadget + gt_Underscore, data & 0xFF);
                }
                break;
            default:
                break;
            }
            item += 8;
        }
        if (has_text) {
            const bool keep_copy = copy || b.r32(gadget + gt_CopyBytes) != 0;
            set_text(g, b, gadget, text, keep_copy);
        }
        const u32 kind = b.r16(gadget + gt_Kind);
        if (kind == SCROLLER_KIND || kind == SLIDER_KIND) {
            sync_scroller(b, gadget);
        }
        // A list view being made has no rows to scroll or show yet.
        if (kind == LISTVIEW_KIND && !creating) {
            sync_list(b, gadget);
            if (selection) {
                show_selected(g, b, window, gadget);
            }
        }
    }

    // The top of a list view kept where its labels and rows allow, after
    // an ordinal asked for was scrolled into view with as little movement
    // as that takes, and its scroller made to match.
    static void sync_list(Bus& b, u32 gadget) {
        const u32 count = list_count(b, b.r32(gadget + gt_Text));
        const u32 rows = b.r16(gadget + gt_Visible);
        u32 top = b.r32(gadget + gt_Total);
        const u32 wanted = b.r32(gadget + gt_MakeVisible);
        if (wanted != kNoOrdinal) {
            if (wanted < top) {
                top = wanted;
            } else if (rows != 0 && wanted >= top + rows) {
                top = wanted - rows + 1;
            }
            b.w32(gadget + gt_MakeVisible, kNoOrdinal);
        }
        const u32 last_top = count > rows ? count - rows : 0;
        top = top > last_top ? last_top : top;
        b.w32(gadget + gt_Total, top);
        const u32 scroller = b.r32(gadget + gt_Scroller);
        if (scroller != 0) {
            b.w32(scroller + gt_Number, top);
            b.w32(scroller + gt_Total, count);
            b.w16(scroller + gt_Visible, rows);
            sync_scroller(b, scroller);
        }
        // A disabled list view's parts are disabled with it.
        const u32 disabled = b.r16(gadget + gg_Flags) & ndk::GFLG_DISABLED;
        u32 part = scroller;
        for (u32 i = 0; i < 3 && part != 0 && b.ok(); ++i) {
            if (b.r32(part + gt_Owner) != gadget) {
                break;
            }
            const u32 flags = b.r16(part + gg_Flags);
            b.w16(part + gg_Flags, (flags & ~ndk::GFLG_DISABLED) | disabled);
            part = b.r32(part + gg_NextGadget);
        }
    }

    // The label of a list view's selected row put into the string gadget
    // GTLV_ShowSelected named, as much of it as the gadget holds; an
    // integer gadget takes the number the label spells as well.
    static void show_selected(GadTools& g, Bus& b, u32 window, u32 gadget) {
        const u32 show = b.r32(gadget + gt_Show);
        const u32 node =
            list_node(b, b.r32(gadget + gt_Text), b.r32(gadget + gt_Number));
        if (show == 0 || node == 0 || !b.ok()) {
            return;
        }
        const u32 si = b.r32(show + ndk::gg_SpecialInfo);
        const u32 buffer = si != 0 ? b.r32(si + ndk::si_Buffer) : 0;
        const u32 max_chars = si != 0 ? b.r16(si + ndk::si_MaxChars) : 0;
        if (buffer == 0 || max_chars == 0 || !b.ok()) {
            return;
        }
        const u32 name = b.r32(node + ndk::LN_NAME);
        u32 length = label_length(b, name);
        length = length < max_chars ? length : max_chars - 1;
        for (u32 i = 0; i < length; ++i) {
            b.w8(buffer + i, b.r8(name + i));
        }
        b.w8(buffer + length, 0);
        b.w16(si + ndk::si_NumChars, length);
        b.w16(si + ndk::si_BufferPos, length);
        b.w16(si + ndk::si_DispPos, 0);
        if ((b.r16(show + ndk::gg_Activation) & ndk::GACT_LONGINT) != 0) {
            const bool negative = length != 0 && b.r8(buffer) == '-';
            u32 magnitude = 0;
            for (u32 at = negative ? 1 : 0; at < length; ++at) {
                const u32 c = b.r8(buffer + at);
                if (c < '0' || c > '9') {
                    break;
                }
                magnitude = magnitude * 10 + (c - '0');
            }
            const u32 value = negative ? 0 - magnitude : magnitude;
            b.w32(si + ndk::si_LongInt, value);
            if ((b.r16(show + ndk::gg_GadgetType) & ndk::GTYP_GADTOOLS) != 0 &&
                b.r16(show + gt_Kind) == INTEGER_KIND) {
                b.w32(show + gt_Number, value);
            }
        }
        if (window != 0 && g.intuition_ != nullptr && b.ok()) {
            g.intuition_->refresh_glist(show, window, 1);
        }
    }

    // Lays a list view out inside the box its NewGadget gave and makes its
    // parts. The rows take the box's width less the scroller's and as much
    // of its height as holds whole rows inside the frame; the string gadget
    // that shows the selected label goes directly beneath them, as wide as
    // the box, and its height comes out of the box's. The scroller stands
    // to the right of the rows with the two arrows below it. The first of
    // the parts, which the gadget before the list view links to; 0 when
    // memory ran out.
    static u32 make_list_parts(GadTools& g, Bus& b, u32 gadget) {
        const i32 left = static_cast<i16>(b.r16(gadget + ndk::gg_LeftEdge));
        const i32 top = static_cast<i16>(b.r16(gadget + ndk::gg_TopEdge));
        const i32 width = static_cast<i16>(b.r16(gadget + ndk::gg_Width));
        const i32 height = static_cast<i16>(b.r16(gadget + ndk::gg_Height));
        const u32 show = b.r32(gadget + gt_Show);
        const i32 show_h = show != 0 ? static_cast<i16>(b.r16(show + ndk::gg_Height)) : 0;
        if (b.r16(gadget + gt_RowHeight) == 0) {
            const i32 font_height = g.graphics_->font_metrics(b.r32(gadget + gt_Font)).y_size;
            b.w16(gadget + gt_RowHeight,
                  static_cast<u32>(font_height) + b.r16(gadget + gt_Spacing));
        }
        const i32 row_h = static_cast<i32>(b.r16(gadget + gt_RowHeight));
        i32 scroll_w = static_cast<i32>(b.r16(gadget + gt_ScrollWidth));
        scroll_w = scroll_w > width / 2 ? width / 2 : scroll_w;
        scroll_w = scroll_w < 0 ? 0 : scroll_w;
        i32 rows = (height - show_h - 2 * kListFrame) / row_h;
        rows = rows < 1 ? 1 : rows;
        const i32 list_h = rows * row_h + 2 * kListFrame;
        b.w16(gadget + ndk::gg_Width, static_cast<u16>(width - scroll_w));
        b.w16(gadget + ndk::gg_Height, static_cast<u16>(list_h));
        b.w16(gadget + gt_Visible, static_cast<u16>(rows));
        b.w16(gadget + gt_ScrollWidth, static_cast<u16>(scroll_w));
        if (show != 0) {
            b.w16(show + ndk::gg_LeftEdge, static_cast<u16>(left));
            b.w16(show + ndk::gg_TopEdge, static_cast<u16>(top + list_h));
            b.w16(show + ndk::gg_Width, static_cast<u16>(width));
        }
        const i32 arrow_h = list_h < 3 * kListArrowHeight ? list_h / 3 : kListArrowHeight;
        const i32 x = left + width - scroll_w;
        const i32 part_top[3] = {top, top + list_h - 2 * arrow_h, top + list_h - arrow_h};
        const i32 part_h[3] = {list_h - 2 * arrow_h, arrow_h, arrow_h};
        const u32 kinds[3] = {kListScrollerKind, kListUpKind, kListDownKind};
        u32 parts[3] = {0, 0, 0};
        for (u32 i = 0; i < 3; ++i) {
            parts[i] = g.exec_->alloc(kGadgetBytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
            if (parts[i] == 0) {
                for (u32 made = 0; made < i; ++made) {
                    g.exec_->free(parts[made], kGadgetBytes);
                }
                return 0;
            }
        }
        for (u32 i = 0; i < 3; ++i) {
            const u32 part = parts[i];
            const bool prop = i == 0;
            b.w32(part + gg_NextGadget, i + 1 < 3 ? parts[i + 1] : gadget);
            b.w16(part + ndk::gg_LeftEdge, static_cast<u16>(x));
            b.w16(part + ndk::gg_TopEdge, static_cast<u16>(part_top[i]));
            b.w16(part + ndk::gg_Width, static_cast<u16>(scroll_w));
            b.w16(part + ndk::gg_Height, static_cast<u16>(part_h[i]));
            b.w16(part + gg_Flags, GFLG_GADGHNONE);
            b.w16(part + ndk::gg_GadgetType,
                  ndk::GTYP_GADTOOLS | (prop ? ndk::GTYP_PROPGADGET : ndk::GTYP_BOOLGADGET));
            b.w16(part + ndk::gg_Activation,
                  prop ? ndk::GACT_RELVERIFY | ndk::GACT_IMMEDIATE | ndk::GACT_FOLLOWMOUSE
                       : ndk::GACT_RELVERIFY);
            if (prop) {
                b.w32(part + ndk::gg_SpecialInfo, part + gt_PropInfo);
            }
            b.w16(part + gt_Kind, kinds[i]);
            b.w32(part + gt_Owner, gadget);
        }
        b.w32(gadget + gt_Scroller, parts[0]);
        return parts[0];
    }

    // A scroller's top, total and visible part; a slider's level less its
    // minimum, its number of levels and one.
    static void scroller_span(Bus& b, u32 gadget, u64& top, u64& total, u64& visible) {
        if (b.r16(gadget + gt_Kind) == SLIDER_KIND) {
            const i64 min = static_cast<i16>(b.r16(gadget + gt_Min));
            const i64 max = static_cast<i32>(b.r32(gadget + gt_Total));
            const i64 level = static_cast<i32>(b.r32(gadget + gt_Number));
            const i64 clamped = level < min ? min : level > max ? max : level;
            top = static_cast<u64>(clamped - min);
            total = max >= min ? static_cast<u64>(max - min + 1) : 1;
            visible = 1;
            return;
        }
        top = b.r32(gadget + gt_Number);
        total = b.r32(gadget + gt_Total);
        visible = b.r16(gadget + gt_Visible);
    }

    // A scroller's PropInfo from its top, total and visible part: the
    // knob as long as the part that shows, as far along as the top.
    static void sync_scroller(Bus& b, u32 gadget) {
        const u32 pi = gadget + gt_PropInfo;
        const bool across = b.r16(gadget + gt_Freedom) == LORIENT_HORIZ;
        u64 top = 0;
        u64 total = 0;
        u64 visible = 0;
        scroller_span(b, gadget, top, total, visible);
        u32 body = ndk::MAXBODY;
        u32 pot = 0;
        if (total > visible) {
            body = static_cast<u32>(visible * ndk::MAXBODY / total);
            const u64 range = total - visible;
            pot = static_cast<u32>((top > range ? range : top) * ndk::MAXPOT / range);
        }
        b.w16(pi + ndk::pi_Flags, ndk::AUTOKNOB | ndk::PROPNEWLOOK |
                                      (across ? ndk::FREEHORIZ : ndk::FREEVERT));
        b.w16(pi + ndk::pi_HorizPot, across ? pot : 0);
        b.w16(pi + ndk::pi_VertPot, across ? 0 : pot);
        b.w16(pi + ndk::pi_HorizBody, across ? body : ndk::MAXBODY);
        b.w16(pi + ndk::pi_VertBody, across ? ndk::MAXBODY : body);
    }

    // CreateGadgetA: kind in D0, the gadget before it at A0, the NewGadget
    // at A1 and tags at A2. Buttons, text and number displays work; any
    // other kind makes a gadget that shows its frame and does nothing.
    static void create_gadget(void* ctx, Machine& m) {
        GadTools& g = of(ctx);
        const u32 kind = m.cpu.d[0];
        const u32 previous = m.cpu.a[0];
        const u32 ng = m.cpu.a[1];
        if (previous == 0 || ng == 0) {
            native::set_result(m, 0);
            return;
        }
        const u32 gadget = g.exec_->alloc(kGadgetBytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
        if (gadget == 0) {
            native::set_result(m, 0);
            return;
        }
        Bus b(m);
        b.w16(gadget + ndk::gg_LeftEdge, b.r16(ng + ng_LeftEdge));
        b.w16(gadget + ndk::gg_TopEdge, b.r16(ng + ng_TopEdge));
        b.w16(gadget + ndk::gg_Width, b.r16(ng + ng_Width));
        b.w16(gadget + ndk::gg_Height, b.r16(ng + ng_Height));
        b.w16(gadget + gg_Flags, GFLG_GADGHNONE);
        const bool clicks = kind == BUTTON_KIND || kind == CHECKBOX_KIND ||
                            kind == INTEGER_KIND || kind == CYCLE_KIND ||
                            kind == PALETTE_KIND || kind == STRING_KIND ||
                            kind == LISTVIEW_KIND;
        // A checkbox turns its own state over when it is let go, an integer
        // gadget takes only numbers and a minus sign.
        const u32 activation =
            kind == CHECKBOX_KIND ? ndk::GACT_RELVERIFY | ndk::GACT_TOGGLESELECT
            : clicks              ? ndk::GACT_RELVERIFY
                                  : 0;
        b.w16(gadget + ndk::gg_Activation,
              kind == INTEGER_KIND ? activation | ndk::GACT_LONGINT : activation);
        if (kind == PALETTE_KIND) {
            // One plane and colour 1, unless the tags say otherwise.
            b.w16(gadget + gt_Count, 2);
            b.w32(gadget + gt_Number, 1);
        }
        if (kind == INTEGER_KIND) {
            b.w16(gadget + gt_Count, kDefaultIntMaxChars);
        }
        const bool prop = kind == SCROLLER_KIND || kind == SLIDER_KIND;
        if (prop) {
            b.w16(gadget + gt_Freedom, LORIENT_HORIZ);
        }
        if (kind == SLIDER_KIND) {
            b.w32(gadget + gt_Total, kDefaultSliderMax);
            b.w16(gadget + gt_LevelPlace, PLACETEXT_LEFT);
        }
        if (kind == LISTVIEW_KIND) {
            b.w32(gadget + gt_Number, kNoOrdinal);
            b.w32(gadget + gt_MakeVisible, kNoOrdinal);
            b.w16(gadget + gt_ScrollWidth, kDefaultScrollWidth);
        }
        b.w16(gadget + ndk::gg_GadgetType,
              ndk::GTYP_GADTOOLS | (kind == STRING_KIND || kind == INTEGER_KIND
                                        ? ndk::GTYP_STRGADGET
                                    : prop ? ndk::GTYP_PROPGADGET
                                           : ndk::GTYP_BOOLGADGET));
        if (prop) {
            b.w16(gadget + ndk::gg_Activation,
                  ndk::GACT_RELVERIFY | ndk::GACT_IMMEDIATE | ndk::GACT_FOLLOWMOUSE);
            b.w32(gadget + ndk::gg_SpecialInfo, gadget + gt_PropInfo);
        }
        b.w16(gadget + ndk::gg_GadgetID, b.r16(ng + ng_GadgetID));
        b.w32(gadget + ndk::gg_UserData, b.r32(ng + ng_UserData));
        b.w16(gadget + gt_Kind, kind);
        const u32 place = b.r32(ng + ng_Flags);
        // A button's label goes inside it, a list view's above it, any
        // other's to its left.
        const u32 default_place = kind == BUTTON_KIND     ? PLACETEXT_IN
                                  : kind == LISTVIEW_KIND ? PLACETEXT_ABOVE
                                                          : PLACETEXT_LEFT;
        b.w16(gadget + gt_Place, (place & 0x1F) != 0 ? place & 0x1F : default_place);
        b.w32(gadget + gt_Label, b.r32(ng + ng_GadgetText));
        // The font its NewGadget names when that is in memory, else its
        // VisualInfo's.
        const u32 attr = b.r32(ng + ng_TextAttr);
        u32 font = attr != 0 && b.ok() ? g.graphics_->open_font(attr) : 0;
        if (font == 0) {
            font = g.visual_font(b, b.r32(ng + ng_VisualInfo));
            g.graphics_->hold_font(font);
        }
        b.w32(gadget + gt_Font, font);
        if (font != 0 && (kind == STRING_KIND || kind == INTEGER_KIND)) {
            const u32 extend = gadget + gt_StringExtend;
            b.w32(extend + ndk::sex_Font, font);
            b.w8(extend + ndk::sex_Pens, kTextPen);
            b.w8(extend + ndk::sex_ActivePens, kTextPen);
            b.w32(gadget + gt_StringInfo + ndk::si_Extension, extend);
            b.w16(gadget + gg_Flags, GFLG_GADGHNONE | ndk::GFLG_STRINGEXTEND);
        }
        apply_tags(g, b, gadget, m.cpu.a[2], true);
        // A checkbox is its own size unless GTCB_Scaled takes the
        // NewGadget's.
        if (kind == CHECKBOX_KIND && (b.r16(gadget + gt_Options) & kOptScaled) == 0) {
            b.w16(gadget + ndk::gg_Width, kCheckBoxWidth);
            b.w16(gadget + ndk::gg_Height, kCheckBoxHeight);
        }
        if (kind == LISTVIEW_KIND && (b.r16(gadget + gt_Options) & kOptReadOnly) != 0) {
            b.w16(gadget + ndk::gg_Activation, 0);
        }
        u32 first = gadget;
        if (kind == LISTVIEW_KIND) {
            // The parts come before the list itself, which is the gadget
            // the next one made links to.
            first = make_list_parts(g, b, gadget);
            if (first == 0) {
                g.graphics_->close_font(font);
                g.exec_->free(gadget, kGadgetBytes);
                native::set_result(m, 0);
                return;
            }
            sync_list(b, gadget);
            show_selected(g, b, 0, gadget);
        }
        b.w32(previous + gg_NextGadget, first);
        if (!b.ok()) {
            native::set_result(m, 0);
            return;
        }
        if (kind == INTEGER_KIND) {
            set_integer_number(g, b, gadget, static_cast<i32>(b.r32(gadget + gt_Number)));
        } else if (kind == STRING_KIND && b.r32(gadget + gt_Text) == 0) {
            set_text(g, b, gadget, 0, true);
        }
        if (kind != GENERIC_KIND && kind != BUTTON_KIND && kind != CHECKBOX_KIND &&
            kind != INTEGER_KIND && kind != TEXT_KIND && kind != NUMBER_KIND &&
            kind != CYCLE_KIND && kind != PALETTE_KIND &&
            kind != SCROLLER_KIND && kind != STRING_KIND && kind != SLIDER_KIND &&
            kind != LISTVIEW_KIND && kind < kKinds &&
            !g.unhandled_kinds_[kind]) {
            g.unhandled_kinds_[kind] = true;
            LogLine().text("gadtools: gadget kind ").dec(kind).text(" not handled").send();
        }
        native::set_result(m, gadget);
    }

    // GT_SetGadgetAttrsA: the gadget at A0 of the window at A1 takes the
    // tags at A3 and is drawn again.
    static void set_gadget_attrs(void* ctx, Machine& m) {
        GadTools& g = of(ctx);
        const u32 gadget = m.cpu.a[0];
        const u32 window = m.cpu.a[1];
        if (gadget == 0) {
            return;
        }
        Bus b(m);
        apply_tags(g, b, gadget, m.cpu.a[3], false, window);
        if (window != 0 && b.ok()) {
            const u32 kind = b.r16(gadget + gt_Kind);
            // Intuition draws a scroller's prop and a string's cursor.
            if ((kind == SCROLLER_KIND || kind == SLIDER_KIND || kind == STRING_KIND ||
                 kind == INTEGER_KIND) &&
                g.intuition_ != nullptr) {
                g.intuition_->refresh_glist(gadget, window, 1);
                if (kind == SLIDER_KIND) {
                    g.draw_slider_level(b, b.r32(window + ndk::wd_RPort), gadget);
                }
            } else if (kind == LISTVIEW_KIND) {
                g.draw_list_view(b, window, gadget);
            } else {
                g.render(window, b.r32(window + ndk::wd_RPort), gadget);
            }
        }
    }

    // GT_GetGadgetAttrsA: each tag at A3 gets the value at its ti_Data.
    // Returns the number of tags answered.
    static void get_gadget_attrs(void*, Machine& m) {
        const u32 gadget = m.cpu.a[0];
        Bus b(m);
        u32 answered = 0;
        for (u32 item = m.cpu.a[3], i = 0; gadget != 0 && item != 0 && i < kMaxTags && b.ok();
             ++i) {
            const u32 tag = b.r32(item);
            const u32 data = b.r32(item + 4);
            if (tag == ndk::TAG_DONE) {
                break;
            }
            if (tag == ndk::TAG_MORE) {
                item = data;
                continue;
            }
            if (tag == GA_Disabled) {
                b.w32(data, (b.r16(gadget + gg_Flags) & ndk::GFLG_DISABLED) != 0 ? 1 : 0);
                ++answered;
            } else if (tag == GTTX_Text) {
                b.w32(data, b.r32(gadget + gt_Text));
                ++answered;
            } else if (tag == GTNM_Number || tag == GTCY_Active || tag == GTPA_Color) {
                b.w32(data, b.r32(gadget + gt_Number));
                ++answered;
            } else if (tag == GTIN_Number) {
                b.w32(data, b.r32(gadget + gt_StringInfo + ndk::si_LongInt));
                ++answered;
            } else if (tag == GTCY_Labels || tag == GTST_String) {
                b.w32(data, b.r32(gadget + gt_Text));
                ++answered;
            } else if (tag == GTCB_Checked) {
                b.w32(data, (b.r16(gadget + gg_Flags) & ndk::GFLG_SELECTED) != 0 ? 1 : 0);
                ++answered;
            } else if (tag == GTSC_Top || tag == GTSL_Level || tag == GTLV_Selected) {
                b.w32(data, b.r32(gadget + gt_Number));
                ++answered;
            } else if (tag == GTLV_Top || tag == GTSL_Max) {
                b.w32(data, b.r32(gadget + gt_Total));
                ++answered;
            } else if (tag == GTLV_Labels) {
                b.w32(data, b.r32(gadget + gt_Text));
                ++answered;
            } else if (tag == GTSL_Min) {
                b.w32(data, static_cast<u32>(static_cast<i32>(static_cast<i16>(
                                b.r16(gadget + gt_Min)))));
                ++answered;
            } else if (tag == GTSC_Total) {
                b.w32(data, b.r32(gadget + gt_Total));
                ++answered;
            } else if (tag == GTSC_Visible) {
                b.w32(data, b.r16(gadget + gt_Visible));
                ++answered;
            }
            item += 8;
        }
        native::set_result(m, answered);
    }

    static u32 label_length(Bus& b, u32 label) {
        u32 n = 0;
        while (label != 0 && n < 255 && b.r8(label + n) != 0 && b.ok()) {
            ++n;
        }
        return n;
    }

    static void create_menus(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).create_menus(m.cpu.a[0]));
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

    // The width in pixels of the guest string at label in the font.
    static u32 label_width(GadTools& g, Bus& b, u32 label, u32 font) {
        u8 chars[255];
        const u32 length = label_length(b, label);
        for (u32 i = 0; i < length; ++i) {
            chars[i] = static_cast<u8>(b.r8(label + i));
        }
        return b.ok() ? g.graphics_->text_width(font, chars, length) : 0;
    }

    // Lays out items (and their subitems) in a column below their title,
    // as wide as their widest label in the font and two rows higher than
    // it. An item's IntuiText names the font's TextAttr when it has one.
    static u32 layout_items(GadTools& g, Bus& b, u32 first, i32 left, i32 top,
                            const MenuFont& font) {
        u32 widest = 0;
        u32 count = 0;
        bool checks = false;
        for (u32 it = first; it != 0 && count < kMaxNewMenus && b.ok();
             it = b.r32(it + mi_NextItem), ++count) {
            const u16 flags = static_cast<u16>(b.r16(it + mi_Flags));
            u32 w = 0;
            if ((flags & ITEMTEXT) != 0) {
                const u32 fill = b.r32(it + mi_ItemFill);
                w = label_width(g, b, b.r32(fill + it_IText), font.font) + kItemPadding;
                if (font.text_attr != 0) {
                    b.w32(fill + it_ITextFont, font.text_attr);
                }
            }
            if ((flags & COMMSEQ) != 0) {
                w += 2 * font.cell + kShortcutSpace;
            }
            if (b.r32(it + mi_SubItem) != 0) {
                w += 2 * font.cell;
            }
            checks = checks || (flags & CHECKIT) != 0;
            widest = w > widest ? w : widest;
        }
        // One CHECKIT item gives every item of the column room for a check
        // mark, so the labels line up.
        if (checks) {
            widest += kCheckSpace;
        }
        widest = widest < 4 * font.cell ? 4 * font.cell : widest;
        i32 y = top;
        count = 0;
        for (u32 it = first; it != 0 && count < kMaxNewMenus && b.ok();
             it = b.r32(it + mi_NextItem), ++count) {
            const u16 flags = static_cast<u16>(b.r16(it + mi_Flags));
            const u32 h = (flags & ITEMTEXT) != 0 ? font.height + kItemLeading : kBarItemHeight;
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
                layout_items(g, b, sub, static_cast<i32>(widest) - 2 * static_cast<i32>(font.cell),
                             0, font);
            }
            y += static_cast<i32>(h);
        }
        return widest;
    }

    // The font a strip is laid out for: the one GTMN_TextAttr names when
    // it is in memory, which the caller closes, else the VisualInfo's.
    static MenuFont menu_font(GadTools& g, Bus& b, u32 vi, u32 tags, u32& opened) {
        u32 attr = 0;
        for (u32 item = tags, i = 0; item != 0 && i < kMaxTags && b.ok(); ++i) {
            const u32 tag = b.r32(item);
            const u32 data = b.r32(item + 4);
            if (tag == ndk::TAG_DONE) {
                break;
            }
            if (tag == ndk::TAG_MORE) {
                item = data;
                continue;
            }
            item += tag == ndk::TAG_SKIP ? (data + 1) * 8 : 8;
            attr = tag == GTMN_TextAttr ? data : attr;
        }
        opened = attr != 0 && b.ok() ? g.graphics_->open_font(attr) : 0;
        return g.menu_font(opened != 0 ? opened : g.visual_font(b, vi),
                           opened != 0 ? attr : 0);
    }

    // LayoutMenusA(menu A0, vi A1, tags A2).
    static void layout_menus(void* ctx, Machine& m) {
        GadTools& g = of(ctx);
        Bus b(m);
        u32 opened = 0;
        const MenuFont font = menu_font(g, b, m.cpu.a[1], m.cpu.a[2], opened);
        native::set_result(m, g.layout_strip(m.cpu.a[0], font) ? 1 : 0);
        g.graphics_->close_font(opened);
    }

    // LayoutMenuItemsA(first item A0, vi A1, tags A2).
    static void layout_menu_items(void* ctx, Machine& m) {
        GadTools& g = of(ctx);
        Bus b(m);
        u32 opened = 0;
        const MenuFont font = menu_font(g, b, m.cpu.a[1], m.cpu.a[2], opened);
        if (m.cpu.a[0] != 0) {
            layout_items(g, b, m.cpu.a[0], 0, 0, font);
        }
        g.graphics_->close_font(opened);
        native::set_result(m, b.ok() ? 1 : 0);
    }

    static void get_imsg(void* ctx, Machine& m) {
        GadTools& g = of(ctx);
        Bus b(m);
        g.draw_pending_levels(b, m.cpu.a[0]);
        native::set_result(m, g.exec_->get_message(m.cpu.a[0]));
    }

    static void reply_imsg(void* ctx, Machine& m) {
        if (m.cpu.a[1] != 0) {
            of(ctx).exec_->reply_message(m.cpu.a[1]);
        }
    }

    static void filter_imsg(void*, Machine& m) { native::set_result(m, m.cpu.a[1]); }

    static void post_filter_imsg(void*, Machine& m) { native::set_result(m, m.cpu.a[1]); }

    // GT_RefreshWindow: draws every gadtools gadget of the window at A0.
    static void refresh_window(void* ctx, Machine& m) {
        GadTools& g = of(ctx);
        const u32 window = m.cpu.a[0];
        if (window == 0) {
            return;
        }
        Bus b(m);
        const u32 rp = b.r32(window + ndk::wd_RPort);
        u32 gadget = b.r32(window + ndk::wd_FirstGadget);
        for (u32 i = 0; gadget != 0 && i < kMaxChain && b.ok(); ++i) {
            const u32 next = b.r32(gadget + gg_NextGadget);
            if ((b.r16(gadget + ndk::gg_GadgetType) & ndk::GTYP_GADTOOLS) != 0) {
                g.render(window, rp, gadget);
            }
            gadget = next;
        }
    }

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
        const RastPortState keep(b, rp);
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

// Builds a Menu, MenuItem and IntuiText strip from the NewMenu array at
// array, ending at NM_END. Items follow the last title and subitems the
// last item; bar labels become separator images. Positions come from
// layout_menus.
u32 GadTools::create_menus(u32 array) {
    if (machine_ == nullptr || exec_ == nullptr || array == 0) {
        return 0;
    }
    Bus b(*machine_);
    u32 count = 0;
    u32 bytes = kMenuBlockHeader;
    for (; count < kMaxNewMenus; ++count) {
        const u8 type = static_cast<u8>(b.r8(array + count * NewMenu_SIZEOF + nm_Type));
        if (!b.ok()) {
            return 0;
        }
        if (type == NM_END) {
            break;
        }
        const u8 kind = type & ~MENU_IMAGE;
        bytes += kind == NM_TITLE ? Menu_SIZEOF + 4
                                  : MenuItem_SIZEOF + 4 + IntuiText_SIZEOF + Image_SIZEOF;
    }
    if (count == kMaxNewMenus) {
        return 0;
    }
    const u32 block = exec_->alloc(bytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (block == 0) {
        return 0;
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
        exec_->free(block, bytes);
        return 0;
    }
    return first_menu;
}

u32 GadTools::visual_font(Bus& b, u32 vi) const {
    if (vi != 0 && (vi & 1) == 0 && b.r32(vi + vi_Magic) == kVisualInfoMagic && b.ok()) {
        return b.r32(vi + vi_Font);
    }
    return intuition_ != nullptr ? intuition_->font_of_screen(intuition_->default_screen()) : 0;
}

GadTools::MenuFont GadTools::menu_font(u32 font, u32 text_attr) const {
    const Graphics::FontMetrics cell = graphics_->font_metrics(font);
    return MenuFont{font, text_attr, static_cast<u32>(cell.y_size),
                    static_cast<u32>(cell.x_size)};
}

bool GadTools::layout_menus(u32 menu, u32 font) {
    if (machine_ == nullptr) {
        return false;
    }
    Bus b(*machine_);
    return layout_strip(menu, menu_font(font != 0 ? font : visual_font(b, 0), 0));
}

// Lays the titles of the strip out side by side, a title's name with
// kTitlePadding columns on either side, and every item column below its
// title.
bool GadTools::layout_strip(u32 menu, const MenuFont& font) {
    if (machine_ == nullptr) {
        return false;
    }
    Bus b(*machine_);
    i32 left = 0;
    u32 count = 0;
    for (u32 mu = menu; mu != 0 && count < kMaxNewMenus && b.ok();
         mu = b.r32(mu + mu_NextMenu), ++count) {
        const u32 width =
            GadToolsNatives::label_width(*this, b, b.r32(mu + mu_MenuName), font.font) +
            2 * kTitlePadding;
        b.w16(mu + mu_LeftEdge, static_cast<u16>(left));
        b.w16(mu + mu_TopEdge, 0);
        b.w16(mu + mu_Width, static_cast<u16>(width));
        b.w16(mu + mu_Height, static_cast<u16>(font.height + kItemLeading));
        GadToolsNatives::layout_items(*this, b, b.r32(mu + mu_FirstItem), 0, 0, font);
        left += static_cast<i32>(width);
    }
    return b.ok();
}

namespace {

struct GadToolsFunction {
    u32 lvo;
    m68k::NativeFn fn;
    const char* name;
};

constexpr GadToolsFunction kGadToolsFunctions[] = {
    {gadtools_lvo::CreateGadgetA, GadToolsNatives::create_gadget, "CreateGadgetA"},
    {gadtools_lvo::FreeGadgets, GadToolsNatives::free_gadgets, "FreeGadgets"},
    {gadtools_lvo::GT_SetGadgetAttrsA, GadToolsNatives::set_gadget_attrs, "GT_SetGadgetAttrsA"},
    {gadtools_lvo::GT_GetGadgetAttrsA, GadToolsNatives::get_gadget_attrs, "GT_GetGadgetAttrsA"},
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

namespace {

// Up to max characters of the text at addr, without the underscore
// character that marks a keyboard shortcut.
u32 read_label(Bus& b, u32 addr, u8 underscore, char* out, u32 max) {
    u32 n = 0;
    for (u32 i = 0; addr != 0 && n < max && i < 255; ++i) {
        const u8 c = static_cast<u8>(b.r8(addr + i));
        if (!b.ok() || c == 0) {
            break;
        }
        if (underscore != 0 && c == underscore) {
            continue;
        }
        out[n++] = static_cast<char>(c);
    }
    return n;
}

// Where text starts, and its baseline, when it is placed per a PLACETEXT_
// flag on a gadget: beside the whole of it, or for PLACETEXT_IN centred in
// its box, which is inner_width wide.
struct TextOrigin {
    i32 x;
    i32 y;
};

TextOrigin text_origin(u32 place, i32 span, const Rect& whole, i32 inner_width, i32 baseline,
                       const Graphics::FontMetrics& cell) {
    if ((place & PLACETEXT_IN) != 0) {
        return {whole.x0 + (inner_width - span) / 2, baseline};
    }
    if ((place & PLACETEXT_RIGHT) != 0) {
        return {whole.x1 + 1 + kLabelGap, baseline};
    }
    const i32 centred = whole.x0 + (whole.x1 - whole.x0 + 1 - span) / 2;
    if ((place & PLACETEXT_ABOVE) != 0) {
        return {centred, whole.y0 - 4 - (cell.y_size - cell.baseline)};
    }
    if ((place & PLACETEXT_BELOW) != 0) {
        return {centred, whole.y1 + 3 + cell.baseline};
    }
    return {whole.x0 - span - kLabelGap, baseline};
}

// Whether the first conversion of the RawDoFmt format at fmt takes a word,
// as %d does, rather than a long, as %ld does.
bool format_takes_word(Bus& b, u32 fmt) {
    for (u32 c = b.r8(fmt); c != 0 && b.ok(); c = b.r8(++fmt)) {
        if (c != '%') {
            continue;
        }
        do {
            c = b.r8(++fmt);
        } while (c == '-' || c == '.' || (c >= '0' && c <= '9'));
        if (c != '%') {
            return c != 'l';
        }
    }
    return false;
}

u32 format_number(i32 value, char* out) {
    char digits[12];
    u32 n = 0;
    u32 magnitude = value < 0 ? static_cast<u32>(-(value + 1)) + 1 : static_cast<u32>(value);
    do {
        digits[n++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    u32 at = 0;
    if (value < 0) {
        out[at++] = '-';
    }
    while (n > 0) {
        out[at++] = digits[--n];
    }
    return at;
}

}  // namespace

// An integer gadget's text is the decimal of its number, in a buffer of
// MaxChars + 1 characters with as much again to undo with.
void GadToolsNatives::set_integer_number(GadTools& g, Bus& b, u32 gadget, i32 value) {
    drop_text_copy(g, b, gadget);
    const u32 si = gadget + gt_StringInfo;
    const u32 max_chars = static_cast<u32>(b.r16(gadget + gt_Count)) + 1;
    char text[12];
    const u32 length = format_number(value, text);
    const u32 bytes = length + 1 > max_chars ? length + 1 : max_chars;
    const u32 at = g.exec_->alloc(bytes + max_chars, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (at == 0) {
        b.w32(gadget + gt_Text, 0);
        return;
    }
    for (u32 i = 0; i < length; ++i) {
        b.w8(at + i, static_cast<u8>(text[i]));
    }
    b.w32(gadget + gt_Text, at);
    b.w32(gadget + gt_CopyBytes, bytes + max_chars);
    b.w32(si + ndk::si_Buffer, at);
    b.w32(si + ndk::si_UndoBuffer, at + bytes);
    b.w16(si + ndk::si_MaxChars, max_chars);
    b.w32(si + ndk::si_LongInt, static_cast<u32>(value));
    b.w16(si + ndk::si_NumChars, length);
    b.w16(si + ndk::si_BufferPos, length);
    b.w16(si + ndk::si_DispPos, 0);
    b.w32(gadget + ndk::gg_SpecialInfo, si);
}


namespace {

// The cycle glyph, a loop with its arrow, 12 by 7 pixels.
constexpr const char* kCycleGlyph[] = {
    "..XXXXXX....",
    ".X......X...",
    "X........X..",
    "X......XXXXX",
    "X.......XXX.",
    ".X.......X..",
    "..XXXX......",
};
constexpr i32 kCycleGlyphWidth = 12;
constexpr i32 kCycleGlyphHeight = 7;
// The part of a cycle gadget left of its separator.
constexpr i32 kCycleKnob = 20;

// A palette's cells: the area they fill and how many columns and rows,
// chosen so a cell is as near square as the area allows.
struct PaletteGrid {
    i32 x = 0;
    i32 y = 0;
    i32 columns = 1;
    i32 rows = 1;
    i32 cell_w = 0;
    i32 cell_h = 0;
    i32 colors = 0;
    Rect indicator{0, 0, -1, -1};
};

PaletteGrid palette_grid(Bus& b, u32 gadget, i32 left, i32 top, i32 right, i32 bottom) {
    PaletteGrid g;
    g.colors = static_cast<i32>(b.r16(gadget + gt_Count));
    const i32 ind_w = static_cast<i32>(b.r16(gadget + gt_IndicatorWidth));
    const i32 ind_h = static_cast<i32>(b.r16(gadget + gt_IndicatorHeight));
    i32 x0 = left + 2;
    i32 y0 = top + 2;
    const i32 x1 = right - 2;
    const i32 y1 = bottom - 2;
    if (ind_w > 0) {
        g.indicator = Rect{x0, y0, x0 + ind_w - 1, y1};
        x0 += ind_w + 2;
    } else if (ind_h > 0) {
        g.indicator = Rect{x0, y0, x1, y0 + ind_h - 1};
        y0 += ind_h + 2;
    }
    const i32 w = x1 - x0 + 1;
    const i32 h = y1 - y0 + 1;
    g.x = x0;
    g.y = y0;
    if (g.colors <= 0 || w <= 0 || h <= 0) {
        g.colors = 0;
        return g;
    }
    i32 best = -1;
    for (i32 columns = 1; columns <= g.colors; ++columns) {
        const i32 rows = (g.colors + columns - 1) / columns;
        const i32 cw = w / columns;
        const i32 ch = h / rows;
        if (cw < 1 || ch < 1) {
            continue;
        }
        const i32 score = cw > ch ? cw - ch : ch - cw;
        if (best < 0 || score < best) {
            best = score;
            g.columns = columns;
            g.rows = rows;
            g.cell_w = cw;
            g.cell_h = ch;
        }
    }
    if (best < 0) {
        g.colors = 0;
    }
    return g;
}

// The pen of the palette's colour number index.
u32 palette_pen(Bus& b, u32 gadget, i32 index) {
    const u32 table = b.r32(gadget + gt_ColorTable);
    return table != 0 ? b.r8(table + static_cast<u32>(index))
                      : b.r16(gadget + gt_ColorOffset) + static_cast<u32>(index);
}

}  // namespace

// A list view's rows: the names of its nodes a row each from its top
// ordinal on, as many as it has rows for. The selected one shows in the
// fill pens when GTLV_ShowSelected was given.
void GadTools::draw_list(Bus& b, u32 rp, u32 gadget, i32 left, i32 top, i32 right,
                         i32 bottom, u32 font) {
    Graphics& gfx = *graphics_;
    const u32 list = b.r32(gadget + gt_Text);
    const u32 first = b.r32(gadget + gt_Total);
    const bool shows = (b.r16(gadget + gt_Options) & kOptShowSelected) != 0;
    const u32 selected = shows ? b.r32(gadget + gt_Number) : kNoOrdinal;
    const i32 row_h = static_cast<i32>(b.r16(gadget + gt_RowHeight));
    const u32 rows = b.r16(gadget + gt_Visible);
    const Graphics::FontMetrics cell = gfx.font_metrics(font);
    const i32 room = right - left - 6;
    if (list == 0 || room < cell.x_size || row_h <= 0) {
        return;
    }
    u32 ordinal = 0;
    i32 y = top + kListFrame;
    for (u32 node = b.r32(list); node != 0 && b.r32(node) != 0 && ordinal < kMaxListNodes &&
                                 b.ok();
         node = b.r32(node), ++ordinal) {
        if (ordinal < first) {
            continue;
        }
        if (ordinal - first >= rows || y + row_h - 1 > bottom - kListFrame) {
            break;
        }
        const bool chosen = ordinal == selected;
        if (chosen) {
            gfx.set_pens(rp, kFillPen, 0, ndk::JAM1);
            gfx.rect_fill(rp, left + 2, y, right - 2, y + row_h - 1);
        }
        char name[256];
        const u32 length = read_label(b, b.r32(node + ndk::LN_NAME), 0, name, sizeof name);
        const u8* chars = reinterpret_cast<const u8*>(name);
        const u32 shown = gfx.text_fit(font, chars, length, room);
        if (shown > 0) {
            // The text sits in the middle of a row higher than the font.
            const i32 text_top = y + (row_h - cell.y_size) / 2;
            gfx.set_pens(rp, chosen ? kFillTextPen : kTextPen, 0, ndk::JAM1);
            gfx.move(rp, left + 4, text_top + cell.baseline);
            gfx.plain_text_in(rp, font, chars, shown);
        }
        y += row_h;
    }
}

// One of a list view's two arrows, in the look of the sliders' knobs; an
// arrow too large for that takes the system image's.
void GadTools::draw_list_arrow(Bus& b, u32 rp, i32 left, i32 top, i32 width, i32 height,
                               bool down, bool pressed) {
    Graphics& gfx = *graphics_;
    if (static_cast<u32>(width) * static_cast<u32>(height) > kMaxArrowPixels) {
        u16 pens[sysimage::kPenCount];
        sysimage::read_pens(b, 0, pens);
        sysimage::draw(gfx, b, rp, down ? ndk::DOWNIMAGE : ndk::UPIMAGE,
                       Rect{left, top, left + width - 1, top + height - 1},
                       pressed ? ndk::IDS_SELECTED : ndk::IDS_NORMAL, pens);
        return;
    }
    const u32 bitmap = b.r32(rp + ndk::rp_BitMap);
    const u32 depth = bitmap != 0 ? b.r8(bitmap + ndk::bm_Depth) : 0;
    u8 pens[kMaxArrowPixels];
    chrome::paint_arrow_button(pens, width, height, down ? chrome::Arrow::Down : chrome::Arrow::Up,
                               pressed, depth >= 8);
    // Row by row, each run of one pen a line.
    for (i32 y = 0; y < height; ++y) {
        for (i32 x = 0; x < width;) {
            const u8 pen = pens[y * width + x];
            i32 end = x;
            while (end + 1 < width && pens[y * width + end + 1] == pen) {
                ++end;
            }
            gfx.set_pens(rp, pen, 0, ndk::JAM1);
            gfx.rect_fill(rp, left + x, top + y, left + end, top + y);
            x = end + 1;
        }
    }
}

// A list view drawn again after its labels, top or selection changed: its
// rows, its scroller and its arrows.
void GadTools::draw_list_view(Bus& b, u32 window, u32 gadget) {
    if (window == 0) {
        return;
    }
    render(window, b.r32(window + ndk::wd_RPort), gadget);
    const u32 scroller = b.r32(gadget + gt_Scroller);
    if (scroller != 0 && intuition_ != nullptr && b.ok()) {
        intuition_->refresh_glist(scroller, window, 3);
    }
}

void GadTools::draw_cycle(Bus& b, u32 rp, u32 gadget, i32 left, i32 top, i32 right,
                          i32 bottom, i32 baseline, bool selected, u32 font) {
    Graphics& gfx = *graphics_;
    const i32 gx = left + 4;
    const i32 gy = top + (bottom - top + 1 - kCycleGlyphHeight) / 2;
    gfx.set_pens(rp, selected ? kFillTextPen : kTextPen, 0, ndk::JAM1);
    for (i32 row = 0; row < kCycleGlyphHeight; ++row) {
        for (i32 col = 0; col < kCycleGlyphWidth; ++col) {
            if (kCycleGlyph[row][col] == 'X') {
                gfx.rect_fill(rp, gx + col, gy + row, gx + col, gy + row);
            }
        }
    }
    // The separator, a groove between the glyph and the choice.
    const i32 sx = left + kCycleKnob - 2;
    gfx.set_pens(rp, kShadowPen, 0, ndk::JAM1);
    gfx.rect_fill(rp, sx, top + 2, sx, bottom - 2);
    gfx.set_pens(rp, kShinePen, 0, ndk::JAM1);
    gfx.rect_fill(rp, sx + 1, top + 2, sx + 1, bottom - 2);
    const u32 labels = b.r32(gadget + gt_Text);
    const u32 count = b.r16(gadget + gt_Count);
    const u32 active = b.r32(gadget + gt_Number);
    if (labels == 0 || active >= count) {
        return;
    }
    char text[128];
    const u32 length = read_label(b, b.r32(labels + active * 4), 0, text, sizeof text);
    const i32 x0 = left + kCycleKnob;
    const u8* chars = reinterpret_cast<const u8*>(text);
    const u32 shown = gfx.text_fit(font, chars, length, right - 3 - x0);
    if (shown == 0) {
        return;
    }
    const i32 span = static_cast<i32>(gfx.text_width(font, chars, shown));
    gfx.set_pens(rp, selected ? kFillTextPen : kTextPen, 0, ndk::JAM1);
    gfx.move(rp, x0 + (right - 3 - x0 - span) / 2, baseline);
    gfx.plain_text_in(rp, font, chars, shown);
}

void GadTools::draw_palette(Bus& b, u32 rp, u32 gadget, i32 left, i32 top, i32 right,
                            i32 bottom) {
    Graphics& gfx = *graphics_;
    const PaletteGrid g = palette_grid(b, gadget, left, top, right, bottom);
    const u32 color = b.r32(gadget + gt_Number);
    if (g.indicator.x1 >= g.indicator.x0) {
        gfx.set_pens(rp, color, 0, ndk::JAM1);
        gfx.rect_fill(rp, g.indicator.x0 + 1, g.indicator.y0 + 1, g.indicator.x1 - 1,
                      g.indicator.y1 - 1);
        gfx.set_pens(rp, kShadowPen, 0, ndk::JAM1);
        gfx.move(rp, g.indicator.x0, g.indicator.y1);
        gfx.draw_line(rp, g.indicator.x0, g.indicator.y0);
        gfx.draw_line(rp, g.indicator.x1, g.indicator.y0);
        gfx.set_pens(rp, kShinePen, 0, ndk::JAM1);
        gfx.move(rp, g.indicator.x1, g.indicator.y0 + 1);
        gfx.draw_line(rp, g.indicator.x1, g.indicator.y1);
        gfx.draw_line(rp, g.indicator.x0 + 1, g.indicator.y1);
    }
    for (i32 i = 0; i < g.colors; ++i) {
        const i32 cx = g.x + (i % g.columns) * g.cell_w;
        const i32 cy = g.y + (i / g.columns) * g.cell_h;
        const u32 pen = palette_pen(b, gadget, i);
        const i32 inset = g.cell_w > 4 && g.cell_h > 4 ? 1 : 0;
        gfx.set_pens(rp, pen, 0, ndk::JAM1);
        gfx.rect_fill(rp, cx + inset, cy + inset, cx + g.cell_w - 1 - inset,
                      cy + g.cell_h - 1 - inset);
        if (pen == color) {
            // The chosen colour is framed black inside white.
            gfx.set_pens(rp, kShinePen, 0, ndk::JAM1);
            gfx.move(rp, cx, cy);
            gfx.draw_line(rp, cx + g.cell_w - 1, cy);
            gfx.draw_line(rp, cx + g.cell_w - 1, cy + g.cell_h - 1);
            gfx.draw_line(rp, cx, cy + g.cell_h - 1);
            gfx.draw_line(rp, cx, cy);
            if (g.cell_w > 4 && g.cell_h > 4) {
                gfx.set_pens(rp, kTextPen, 0, ndk::JAM1);
                gfx.move(rp, cx + 1, cy + 1);
                gfx.draw_line(rp, cx + g.cell_w - 2, cy + 1);
                gfx.draw_line(rp, cx + g.cell_w - 2, cy + g.cell_h - 2);
                gfx.draw_line(rp, cx + 1, cy + g.cell_h - 2);
                gfx.draw_line(rp, cx + 1, cy + 1);
            }
        }
    }
}

// A checkbox: a raised box filled with the background, and a check mark
// in the text pen when selected.
void GadTools::draw_checkbox(u32 rp, i32 left, i32 top, i32 right, i32 bottom,
                             bool selected) {
    // The checkbox looks as sysiclass's CHECKIMAGE, in the default pens.
    Bus b(*machine_);
    u16 pens[sysimage::kPenCount];
    sysimage::read_pens(b, 0, pens);
    sysimage::draw(*graphics_, b, rp, ndk::CHECKIMAGE, Rect{left, top, right, bottom},
                   selected ? ndk::IDS_SELECTED : ndk::IDS_NORMAL, pens);
}

// A scroller Intuition moved: its top from the knob's place, which its
// messages carry.
u16 GadTools::prop_code(u32 gadget) {
    if (machine_ == nullptr || gadget == 0) {
        return 0;
    }
    Bus b(*machine_);
    const u32 kind = b.r16(gadget + gt_Kind);
    if (kind != SCROLLER_KIND && kind != SLIDER_KIND && kind != kListScrollerKind) {
        return 0;
    }
    const u32 pi = gadget + gt_PropInfo;
    const bool across = b.r16(gadget + gt_Freedom) == LORIENT_HORIZ;
    u64 old_top = 0;
    u64 total = 0;
    u64 visible = 0;
    GadToolsNatives::scroller_span(b, gadget, old_top, total, visible);
    const u64 pot = across ? b.r16(pi + ndk::pi_HorizPot) : b.r16(pi + ndk::pi_VertPot);
    const u64 range = total > visible ? total - visible : 0;
    const u32 top = static_cast<u32>((pot * range + ndk::MAXPOT / 2) / ndk::MAXPOT);
    if (kind == SLIDER_KIND) {
        // A slider's messages carry its level.
        const i32 level = static_cast<i16>(b.r16(gadget + gt_Min)) + static_cast<i32>(top);
        b.w32(gadget + gt_Number, static_cast<u32>(level));
        return static_cast<u16>(level);
    }
    b.w32(gadget + gt_Number, top);
    return static_cast<u16>(top);
}

// A prop gadget of the window Intuition moved: the code its message
// carries. False for a list view's scroller, which moves the list's rows
// and whose messages no program is to see.
bool GadTools::prop_moved(u32 window, u32 gadget, u16& code) {
    code = prop_code(gadget);
    if (machine_ == nullptr || gadget == 0) {
        return true;
    }
    Bus b(*machine_);
    const u32 kind = b.r16(gadget + gt_Kind);
    if (kind == SLIDER_KIND && window != 0) {
        if (b.r32(gadget + gt_LevelFunc) == 0) {
            draw_slider_level(b, b.r32(window + ndk::wd_RPort), gadget);
        } else {
            PendingLevel* queued = nullptr;
            for (PendingLevel& p : pending_levels_) {
                if (p.gadget == gadget) {
                    queued = &p;
                    break;
                }
            }
            if (queued != nullptr) {
                queued->window = window;
            } else if (pending_levels_.size() < kPendingLevels) {
                pending_levels_.push_back(PendingLevel{window, gadget});
            }
        }
    }
    if (kind != kListScrollerKind) {
        return true;
    }
    const u32 list = b.r32(gadget + gt_Owner);
    if (list != 0 && b.r32(list + gt_Total) != code && b.ok()) {
        b.w32(list + gt_Total, code);
        if (window != 0) {
            render(window, b.r32(window + ndk::wd_RPort), list);
        }
    }
    return false;
}

// A click let go over the gadget at x, y of its box: a cycle gadget moves
// on to its next choice, a palette takes the colour under the pointer, a
// checkbox turns over, a list view takes the row under the pointer and one
// of its arrows moves it a row. The code its IDCMP_GADGETUP carries; false
// when the click is to send none, as one on a list view's arrow or below
// its last label.
bool GadTools::release(u32 window, u32 gadget, i32 x, i32 y, u16& code) {
    if (machine_ == nullptr || gadget == 0) {
        return false;
    }
    Bus b(*machine_);
    const u32 kind = b.r16(gadget + gt_Kind);
    if (kind == CHECKBOX_KIND) {
        const u32 flags = b.r16(gadget + gg_Flags);
        const bool selected = (flags & ndk::GFLG_SELECTED) == 0;
        b.w16(gadget + gg_Flags,
              selected ? flags | ndk::GFLG_SELECTED : flags & ~ndk::GFLG_SELECTED);
        code = selected ? 1 : 0;
        if (window != 0) {
            render(window, b.r32(window + ndk::wd_RPort), gadget);
        }
        return b.ok();
    }
    if (kind == CYCLE_KIND) {
        const u32 count = b.r16(gadget + gt_Count);
        const u32 active = b.r32(gadget + gt_Number);
        const u32 next = count != 0 ? (active + 1) % count : 0;
        b.w32(gadget + gt_Number, next);
        code = static_cast<u16>(next);
        return b.ok();
    }
    if (kind == PALETTE_KIND) {
        const i32 width = static_cast<i16>(b.r16(gadget + ndk::gg_Width));
        const i32 height = static_cast<i16>(b.r16(gadget + ndk::gg_Height));
        const PaletteGrid g = palette_grid(b, gadget, 0, 0, width - 1, height - 1);
        u32 color = b.r32(gadget + gt_Number);
        if (g.colors > 0 && x >= g.x && y >= g.y) {
            const i32 col = (x - g.x) / g.cell_w;
            const i32 row = (y - g.y) / g.cell_h;
            const i32 index = row * g.columns + col;
            if (col < g.columns && index < g.colors) {
                color = palette_pen(b, gadget, index);
            }
        }
        b.w32(gadget + gt_Number, color);
        code = static_cast<u16>(color);
        return b.ok();
    }
    if (kind == LISTVIEW_KIND) {
        // The row under the pointer, counted from the list's top.
        const i32 row_h = static_cast<i32>(b.r16(gadget + gt_RowHeight));
        const i32 row = row_h > 0 && y >= kListFrame ? (y - kListFrame) / row_h : -1;
        const u32 count = list_count(b, b.r32(gadget + gt_Text));
        const u32 ordinal = b.r32(gadget + gt_Total) + static_cast<u32>(row < 0 ? 0 : row);
        if (row < 0 || static_cast<u32>(row) >= b.r16(gadget + gt_Visible) || ordinal >= count) {
            return false;
        }
        b.w32(gadget + gt_Number, ordinal);
        code = static_cast<u16>(ordinal);
        GadToolsNatives::show_selected(*this, b, window, gadget);
        if (window != 0) {
            render(window, b.r32(window + ndk::wd_RPort), gadget);
        }
        return b.ok();
    }
    if (kind == kListUpKind || kind == kListDownKind) {
        const u32 list = b.r32(gadget + gt_Owner);
        const u32 top = list != 0 ? b.r32(list + gt_Total) : 0;
        if (list != 0 && b.ok() && (kind == kListDownKind || top != 0)) {
            // Past the last row the list stays where its labels end.
            b.w32(list + gt_Total, kind == kListUpKind ? top - 1 : top + 1);
            GadToolsNatives::sync_list(b, list);
            if (b.r32(list + gt_Total) != top) {
                draw_list_view(b, window, list);
            }
        }
        return false;
    }
    return true;
}

// What GTSL_DispFunc makes of a level: the function is called as C code is,
// with the gadget and then the level on the stack, and answers in D0. The
// level itself shows when there is no function or the call does not return.
void GadTools::draw_pending_levels(Bus& b, u32 port) {
    // Levels queued while one draws wait for the next pass.
    usize end = pending_levels_.size();
    for (usize i = 0; i < end;) {
        const PendingLevel& p = pending_levels_[i];
        if (b.r32(p.window + ndk::wd_UserPort) != port) {
            ++i;
            continue;
        }
        const PendingLevel level = p;
        pending_levels_.erase(pending_levels_.begin() + i);
        --end;
        draw_slider_level(b, b.r32(level.window + ndk::wd_RPort), level.gadget);
    }
}

i32 GadTools::displayed_level(Bus& b, u32 gadget, i32 level) {
    const u32 func = b.r32(gadget + gt_LevelFunc);
    if (func == 0 || !b.ok() || !exec_->enter_nested_run()) {
        return level;
    }
    m68k::CpuState& cpu = machine_->cpu;
    u32 d[8];
    u32 a[8];
    for (u32 i = 0; i < 8; ++i) {
        d[i] = cpu.d[i];
        a[i] = cpu.a[i];
    }
    const u32 args = a[7] - 8;
    b.w32(args, gadget);
    b.w32(args + 4, static_cast<u32>(level));
    m68k::ExitReason why = m68k::ExitReason::BusError;
    if (b.ok()) {
        cpu.a[7] = args;
        why = machine_->jit->call_guest(func, kLevelFuncBlocks);
    }
    exec_->leave_nested_run();
    if (why != m68k::ExitReason::Returned) {
        return level;
    }
    const i32 shown = static_cast<i32>(cpu.d[0]);
    for (u32 i = 0; i < 8; ++i) {
        cpu.d[i] = d[i];
        cpu.a[i] = a[i];
    }
    return shown;
}

// A slider's level as text, formatted with GTSL_LevelFormat. The format
// takes the level as a word for %d and %u and as a long for %ld and %lu.
u32 GadTools::level_text(Bus& b, u32 gadget, i32 level, char* out, u32 capacity) {
    struct Sink {
        char* out;
        u32 capacity;
        u32 length = 0;

        bool put(u32 c) {
            if (length >= capacity) {
                return false;
            }
            out[length++] = static_cast<char>(c);
            return true;
        }
    } sink{out, capacity};
    const u32 format = b.r32(gadget + gt_LevelFormat);
    const u32 argument = static_cast<u32>(displayed_level(b, gadget, level));
    const u32 stream = gadget + gt_LevelArg;
    if (format_takes_word(b, format)) {
        b.w16(stream, argument);
    } else {
        b.w32(stream, argument);
    }
    u32 data = stream;
    native::raw_format(b, format, data, sink);
    return sink.length;
}

// A slider's level, when GTSL_LevelFormat was given, in a field of
// GTSL_MaxLevelLen characters beside its box. The field is cleared first,
// except for PLACETEXT_IN, whose field is the box itself, which Intuition
// has drawn again by the time the level is.
void GadTools::draw_slider_level(Bus& b, u32 rp, u32 gadget) {
    if (rp == 0 || machine_ == nullptr || graphics_ == nullptr ||
        b.r32(gadget + gt_LevelFormat) == 0 || !b.ok()) {
        return;
    }
    const i32 left = static_cast<i16>(b.r16(gadget + ndk::gg_LeftEdge));
    const i32 top = static_cast<i16>(b.r16(gadget + ndk::gg_TopEdge));
    const i32 width = static_cast<i16>(b.r16(gadget + ndk::gg_Width));
    const i32 height = static_cast<i16>(b.r16(gadget + ndk::gg_Height));
    const u32 place = b.r16(gadget + gt_LevelPlace);
    const u32 font = b.r32(gadget + gt_Font);
    Graphics& gfx = *graphics_;
    const Graphics::FontMetrics cell = gfx.font_metrics(font);
    u64 offset = 0;
    u64 total = 0;
    u64 visible = 0;
    GadToolsNatives::scroller_span(b, gadget, offset, total, visible);
    const i32 level = static_cast<i16>(b.r16(gadget + gt_Min)) + static_cast<i32>(offset);
    char text[kMaxLevelText];
    const u32 length = level_text(b, gadget, level, text, sizeof text);
    // GTSL_MaxLevelLen bounds the string; the text itself is what is drawn,
    // flush against the slider for LEFT, with the background pen behind
    // it so a fixed-width format such as "%3lu" covers the last level.
    const u8* chars = reinterpret_cast<const u8*>(text);
    const i32 limit = static_cast<i32>(b.r16(gadget + gt_LevelLen)) * cell.x_size;
    const u32 shown = gfx.text_fit(font, chars, length, limit);
    const i32 span = gfx.text_width(font, chars, shown);
    const i32 baseline = top + (height - cell.y_size) / 2 + cell.baseline;
    const Rect box{left, top, left + width - 1, top + height - 1};
    const TextOrigin at = text_origin(place, span, box, width, baseline, cell);
    if (!b.ok() || shown == 0) {
        return;
    }
    const RastPortState keep(b, rp);
    const bool inside = (place & PLACETEXT_IN) != 0;
    gfx.set_pens(rp, kTextPen, kBackgroundPen, inside ? ndk::JAM1 : ndk::JAM2);
    gfx.move(rp, at.x, at.y);
    gfx.plain_text_in(rp, font, chars, shown);
}

bool GadTools::render(u32 window, u32 rp, u32 gadget) {
    if (machine_ == nullptr || graphics_ == nullptr || rp == 0 || gadget == 0) {
        return true;
    }
    Bus b(*machine_);
    const i32 left = static_cast<i16>(b.r16(gadget + ndk::gg_LeftEdge));
    const i32 top = static_cast<i16>(b.r16(gadget + ndk::gg_TopEdge));
    const i32 width = static_cast<i16>(b.r16(gadget + ndk::gg_Width));
    const i32 height = static_cast<i16>(b.r16(gadget + ndk::gg_Height));
    const u32 flags = b.r16(gadget + gg_Flags);
    const u32 kind = b.r16(gadget + gt_Kind);
    if (kind == GENERIC_KIND) {
        return false;
    }
    const u32 place = b.r16(gadget + gt_Place);
    const u32 options = b.r16(gadget + gt_Options);
    const u8 underscore = static_cast<u8>(b.r8(gadget + gt_Underscore));
    if (!b.ok() || width <= 0 || height <= 0) {
        return true;
    }
    const RastPortState keep(b, rp);
    Graphics& gfx = *graphics_;
    const i32 right = left + width - 1;
    const i32 bottom = top + height - 1;
    if (kind == kListUpKind || kind == kListDownKind) {
        draw_list_arrow(b, rp, left, top, width, height, kind == kListDownKind,
                        (flags & ndk::GFLG_SELECTED) != 0);
        ghost(rp, flags, left, top, right, bottom);
        return true;
    }
    // A list view held down looks as it did: it has rows to pick, and is
    // no button.
    const bool selected = (flags & ndk::GFLG_SELECTED) != 0 && kind != LISTVIEW_KIND;
    const bool framed = kind == BUTTON_KIND || (options & kOptBorder) != 0 ||
                        (kind != TEXT_KIND && kind != NUMBER_KIND);
    // A list view to pick from is raised, one only to read recessed.
    const bool recessed = kind == TEXT_KIND || kind == NUMBER_KIND ||
                          kind == STRING_KIND || kind == INTEGER_KIND ||
                          kind == SCROLLER_KIND || kind == SLIDER_KIND ||
                          (kind == LISTVIEW_KIND && (options & kOptReadOnly) != 0) || selected;
    // The inside first, then the frame around it. Intuition draws a
    // scroller's or slider's box and knob, over which this would paint.
    const bool prop = kind == SCROLLER_KIND || kind == SLIDER_KIND || kind == kListScrollerKind;
    gfx.set_pens(rp, selected ? kFillPen : 0, 0, ndk::JAM1);
    if (prop) {
        if (intuition_ != nullptr && window != 0) {
            intuition_->refresh_glist(gadget, window, 1);
        }
    } else if (kind == CHECKBOX_KIND) {
        draw_checkbox(rp, left, top, right, bottom, selected);
    } else if (framed && width > 2 && height > 2) {
        gfx.rect_fill(rp, left + 1, top + 1, right - 1, bottom - 1);
        gfx.set_pens(rp, recessed ? kShadowPen : kShinePen, 0, ndk::JAM1);
        gfx.move(rp, left, bottom);
        gfx.draw_line(rp, left, top);
        gfx.draw_line(rp, right, top);
        gfx.set_pens(rp, recessed ? kShinePen : kShadowPen, 0, ndk::JAM1);
        gfx.move(rp, right, top + 1);
        gfx.draw_line(rp, right, bottom);
        gfx.draw_line(rp, left + 1, bottom);
    } else {
        gfx.rect_fill(rp, left, top, right, bottom);
    }
    const u32 font = b.r32(gadget + gt_Font);
    const Graphics::FontMetrics cell = gfx.font_metrics(font);
    const i32 baseline = top + (height - cell.y_size) / 2 + cell.baseline;
    const i32 room = width - (framed ? 8 : 0);

    // What a display shows, cut to fit inside it.
    char text[256];
    u32 length = 0;
    // A string gadget shows its text from its display position on;
    // Intuition draws the cursor over it while it is being edited.
    if (kind == TEXT_KIND) {
        length = read_label(b, b.r32(gadget + gt_Text), 0, text, sizeof text);
    } else if (kind == STRING_KIND || kind == INTEGER_KIND) {
        const u32 si = gadget + gt_StringInfo;
        const u32 disp = b.r16(si + ndk::si_DispPos);
        const u32 count = b.r16(si + ndk::si_NumChars);
        const u32 buffer = b.r32(si + ndk::si_Buffer);
        for (u32 i = disp; buffer != 0 && i < count && length < sizeof text; ++i) {
            text[length++] = static_cast<char>(b.r8(buffer + i));
        }
    } else if (kind == NUMBER_KIND) {
        length = format_number(static_cast<i32>(b.r32(gadget + gt_Number)), text);
    } else if (kind == CYCLE_KIND) {
        draw_cycle(b, rp, gadget, left, top, right, bottom, baseline, selected, font);
    } else if (kind == PALETTE_KIND) {
        draw_palette(b, rp, gadget, left, top, right, bottom);
    } else if (kind == LISTVIEW_KIND) {
        draw_list(b, rp, gadget, left, top, right, bottom, font);
    }
    const u32 shown = gfx.text_fit(font, reinterpret_cast<const u8*>(text), length, room);
    if (shown != 0) {
        gfx.set_pens(rp, kTextPen, 0, ndk::JAM1);
        gfx.move(rp, left + (framed ? 4 : 0), baseline);
        gfx.plain_text_in(rp, font, reinterpret_cast<const u8*>(text), shown);
    }
    if (kind == SLIDER_KIND) {
        draw_slider_level(b, rp, gadget);
    }

    // The label, inside a button or beside the gadget.
    char label[128];
    const u32 label_chars = read_label(b, b.r32(gadget + gt_Label), underscore, label,
                                       sizeof label);
    if (label_chars == 0) {
        ghost(rp, flags, left, top, right, bottom);
        return true;
    }
    const u8* label_text = reinterpret_cast<const u8*>(label);
    const i32 span = static_cast<i32>(gfx.text_width(font, label_text, label_chars));
    // A list view's label is placed by the whole of it, the scroller and
    // the gadget that shows its selected label included.
    i32 full_width = width;
    i32 full_bottom = bottom;
    if (kind == LISTVIEW_KIND) {
        const u32 show = b.r32(gadget + gt_Show);
        full_width += static_cast<i32>(b.r16(gadget + gt_ScrollWidth));
        full_bottom += show != 0 ? static_cast<i16>(b.r16(show + ndk::gg_Height)) : 0;
    }
    const Rect whole{left, top, left + full_width - 1, full_bottom};
    const TextOrigin at = text_origin(place, span, whole, width, baseline, cell);
    const bool inside = (place & PLACETEXT_IN) != 0;
    gfx.set_pens(rp, inside && selected ? kFillTextPen : kTextPen, 0, ndk::JAM1);
    gfx.move(rp, at.x, at.y);
    gfx.plain_text_in(rp, font, label_text, label_chars);
    ghost(rp, flags, left, top, right, bottom);
    return true;
}

// A disabled gadget is drawn over with a dot of the shadow pen at every
// other pixel, as gadtools ghosts it.
void GadTools::ghost(u32 rp, u32 flags, i32 left, i32 top, i32 right, i32 bottom) {
    if ((flags & ndk::GFLG_DISABLED) == 0) {
        return;
    }
    Graphics& gfx = *graphics_;
    gfx.set_pens(rp, kShadowPen, 0, ndk::JAM1);
    for (i32 y = top; y <= bottom; ++y) {
        for (i32 x = left + ((y - top) & 1); x <= right; x += 2) {
            gfx.rect_fill(rp, x, y, x, y);
        }
    }
}

bool GadTools::init(Machine& m, Exec& exec, Graphics& graphics) {
    machine_ = &m;
    exec_ = &exec;
    graphics_ = &graphics;
    base_ = 0;
    for (bool& seen : unhandled_kinds_) {
        seen = false;
    }
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
