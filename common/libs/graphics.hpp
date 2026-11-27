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

#pragma once

#include "../types.hpp"
#include "deep_bitmaps.hpp"
#include "gfx.hpp"
#include "native.hpp"
#include "text_blend.hpp"

#include <vector>

// graphics.library written in C++ for code run by the 68000 translator.
//
// It draws on the planar BitMap of a RastPort and has the colour table
// calls, the raster allocator and the fonts kept in the ROM: the built-in
// 8x8 topaz and those of rom_fonts.hpp. Text is drawn from the tables of
// whichever TextFont the RastPort has. The C++ methods are the same
// drawing code the native vectors use, for the Intuition windows and the
// shell console. Every other vector logs
// "graphics: unimplemented LVO -<n>" and ends the run with Unimplemented.
//
// Every guest pointer goes through native::Bus, so a bad one ends the run
// with BusError or AddressError.

namespace hombre {

class Exec;
struct RomFont;

namespace raster {
struct Bitmap;
struct Clip;
struct Font;
struct RastPortView;
struct TextStyle;
}

// Hombre's own tags of a TextFontExtension, beside those ExtendFont takes:
// the guest address of a font's 4-bit coverage plane and its bytes a row.
constexpr u32 kTagCoverage = 0x80484D01;
constexpr u32 kTagCoverageModulo = 0x80484D02;

// Library vector offsets of graphics.library, n of LVO -n, as the 3.1
// function description numbers them.
namespace graphics_lvo {

constexpr u32 BltBitMap = 30;
constexpr u32 BltBitMapRastPort = 606;
constexpr u32 ClearScreen = 48;
constexpr u32 TextLength = 54;
constexpr u32 Text = 60;
constexpr u32 SetFont = 66;
constexpr u32 OpenFont = 72;
constexpr u32 CloseFont = 78;
constexpr u32 AskSoftStyle = 84;
constexpr u32 SetSoftStyle = 90;
constexpr u32 LoadRGB4 = 192;
constexpr u32 InitRastPort = 198;
constexpr u32 DrawEllipse = 180;
constexpr u32 AreaEllipse = 186;
constexpr u32 WaitBlit = 228;
constexpr u32 SetRast = 234;
constexpr u32 Move = 240;
constexpr u32 Draw = 246;
constexpr u32 AreaMove = 252;
constexpr u32 AreaDraw = 258;
constexpr u32 AreaEnd = 264;
constexpr u32 InitArea = 282;
constexpr u32 SetRGB4 = 288;
constexpr u32 BltClear = 300;
constexpr u32 RectFill = 306;
constexpr u32 ReadPixel = 318;
constexpr u32 WritePixel = 324;
constexpr u32 Flood = 330;
constexpr u32 SetAPen = 342;
constexpr u32 SetBPen = 348;
constexpr u32 SetDrMd = 354;
constexpr u32 InitBitMap = 390;
constexpr u32 LockLayerRom = 432;
constexpr u32 UnlockLayerRom = 438;
constexpr u32 SyncSBitMap = 444;
constexpr u32 CopySBitMap = 450;
constexpr u32 AttemptLockLayerRom = 654;
constexpr u32 ScrollRaster = 396;
constexpr u32 InitTmpRas = 468;
constexpr u32 AskFont = 474;
constexpr u32 AddFont = 480;
constexpr u32 RemFont = 486;
constexpr u32 AndRectRegion = 504;
constexpr u32 OrRectRegion = 510;
constexpr u32 NewRegion = 516;
constexpr u32 ClearRectRegion = 522;
constexpr u32 ClearRegion = 528;
constexpr u32 DisposeRegion = 534;
constexpr u32 XorRectRegion = 558;
constexpr u32 OrRegionRegion = 612;
constexpr u32 AndRegionRegion = 624;
constexpr u32 AllocRaster = 492;
constexpr u32 TextExtent = 690;
constexpr u32 TextFit = 696;
constexpr u32 FreeRaster = 498;
constexpr u32 GetRGB4 = 582;
constexpr u32 BltTemplate = 36;
constexpr u32 PolyDraw = 336;
constexpr u32 ClipBlit = 552;
constexpr u32 SetChipRev = 888;
constexpr u32 VideoControl = 708;
constexpr u32 OpenMonitor = 714;
constexpr u32 CloseMonitor = 720;
constexpr u32 FindDisplayInfo = 726;
constexpr u32 NextDisplayInfo = 732;
constexpr u32 GetDisplayInfoData = 756;
constexpr u32 GetVPModeID = 792;
constexpr u32 ModeNotAvailable = 798;
constexpr u32 WeighTAMatch = 804;
constexpr u32 ExtendFont = 816;
constexpr u32 StripFont = 822;
constexpr u32 ObtainBestPenA = 840;
constexpr u32 SetRGB32 = 852;
constexpr u32 ReleasePen = 948;
constexpr u32 ObtainPen = 954;
constexpr u32 FindColor = 1008;
constexpr u32 GetAPen = 858;
constexpr u32 GetBPen = 864;
constexpr u32 GetDrMd = 870;
constexpr u32 GetOutlinePen = 876;
constexpr u32 SetABPenDrMd = 894;
constexpr u32 AllocBitMap = 918;
constexpr u32 FreeBitMap = 924;
constexpr u32 GetBitMapAttr = 960;
constexpr u32 SetOutlinePen = 978;
constexpr u32 SetWriteMask = 984;
constexpr u32 SetMaxPen = 990;
constexpr u32 ScrollRasterBF = 1002;
constexpr u32 SetRPAttrsA = 1038;
constexpr u32 GetRPAttrsA = 1044;
constexpr u32 BestModeIDA = 1050;
constexpr u32 LoadRGB32 = 882;
constexpr u32 GetRGB32 = 900;
constexpr u32 SetRGB32CM = 996;
constexpr u32 ReadPixelLine8 = 768;
constexpr u32 WritePixelLine8 = 774;
constexpr u32 ReadPixelArray8 = 780;
constexpr u32 WritePixelArray8 = 786;
constexpr u32 WriteChunkyPixels = 1056;
// The last vector of the 3.9 library; the table reaches down to it.
constexpr u32 Last = 1056;

}

class Graphics {
public:
    // Makes graphics.library with its vectors and its list of the fonts in
    // memory: topaz 8, then the fonts of rom_fonts.hpp. exec must be
    // initialised. False if exec runs out of memory or native ids.
    bool init(m68k::Machine& m, Exec& exec, DisplayState& display);

    // A deep bitmap of width by height pixels in fmt: its BitMap in public
    // memory, its pixels cleared in fast memory, rows 16-byte aligned. 0,
    // with nothing kept, when either does not fit.
    u32 alloc_deep_bitmap(u32 width, u32 height, PixFmt fmt);
    // Frees a bitmap alloc_deep_bitmap made; any other is left alone.
    void free_deep_bitmap(u32 bitmap);
    // Makes the BitMap at bitmap, which the caller owns, a deep one over
    // pixels it allocated, of width by height in fmt with rows of
    // bytes_per_row; an earlier entry for it is replaced. Its pens take the
    // palette's colours. False when the table is full.
    bool attach_deep(u32 bitmap, u32 pixels, u32 pixel_bytes, u32 bytes_per_row, u32 width,
                     u32 height, PixFmt fmt, const u32* palette, u32 colormap);
    // Tells the display, or the damage sink, that rect of the bitmap
    // changed; rect counts in the bitmap's own pixels.
    void report_bitmap(u32 bitmap, const Rect& rect);
    // Forgets that bitmap is deep; its memory stays the caller's.
    void detach_deep(u32 bitmap) { deep_bitmaps_.remove(bitmap); }
    // The deep bitmap at that address, null for a planar one.
    const DeepBitmap* deep(u32 bitmap) const { return deep_bitmaps_.find(bitmap); }
    DeepBitmap* deep(u32 bitmap) { return deep_bitmaps_.find(bitmap); }
    const DeepBitmaps& deep_bitmaps() const { return deep_bitmaps_; }
    // Room left in the table of deep bitmaps.
    u32 deep_room() const { return deep_bitmaps_.free_count(); }

    // The 256 colours of a ColorMap as 0x00RRGGBB, kept in step with it
    // from then on: what pens draw in on a deep bitmap of that screen. Up
    // to kMaxPalettes ColorMaps; null past that, or for 0.
    const u32* palette_for(u32 colormap);
    // Lets go of a ColorMap's palette when its screen closes.
    void release_palette(u32 colormap);
    // A colour of some ColorMap changed: the palettes follow.
    void palettes_changed();

    u32 graphics_base() const { return base_; }
    u32 topaz_font() const { return topaz_; }
    // The TextAttr that names the built-in font, as Screen.Font points to one.
    u32 topaz_attr() const { return topaz_attr_; }

    // The font InitRastPort gives a RastPort: topaz until set_default_font
    // names another.
    u32 default_font() const;
    // Makes font the default font, topaz for 0, opening it and closing the
    // default it replaces as OpenFont and CloseFont count accessors.
    void set_default_font(u32 font);
    // OpenFont(textAttr) and CloseFont(textFont) for code that is not the
    // guest's: the font in memory nearest the TextAttr at text_attr, with
    // one more accessor or none, and one accessor fewer.
    u32 open_font(u32 text_attr);
    void close_font(u32 font);
    // The height, baseline and width of a font, for laying text out in it.
    struct FontMetrics {
        i32 y_size;
        i32 baseline;
        i32 x_size;
    };
    FontMetrics font_metrics(u32 font);
    // TextLength in font, 0 for the built-in font: how far Text would move
    // the pen for the characters.
    u32 text_width(u32 font, const u8* chars, u32 count);
    // How many of the characters, from the first on, Text in font would
    // draw within width pixels.
    u32 text_fit(u32 font, const u8* chars, u32 count, i32 width);
    // One more accessor of a font that is open, as OpenFont for its own
    // TextAttr would count it.
    void hold_font(u32 font);
    // Draws the characters in font, 0 for the built-in one, in the
    // RastPort's pens, draw mode, spacing and styles, with the
    // antialiasing Text has, and moves its pen past them. How far the pen
    // moved.
    u32 text_in(u32 rp, u32 font, const u8* chars, u32 count);
    // The same without the RastPort's spacing and styles: they are its
    // program's, and the system labels its windows and gadgets plainly.
    u32 plain_text_in(u32 rp, u32 font, const u8* chars, u32 count);
    // Draws the characters in font, 0 for the built-in one, in pen on the
    // BitMap at bitmap_addr, loaded as bitmap, from x with their baseline
    // at baseline, antialiased as Text is. How far a pen would move.
    u32 text_on(native::Bus& b, const raster::Bitmap& bitmap, u32 bitmap_addr,
                const raster::Clip& clip, u8 pen, u32 font, i32 x, i32 baseline,
                const u8* chars, u32 count);

    void init_rastport(u32 rp);
    void set_pens(u32 rp, u32 fg, u32 bg, u32 mode);
    void move(u32 rp, i32 x, i32 y);
    void rect_fill(u32 rp, i32 x0, i32 y0, i32 x1, i32 y1);
    // BltBitMap: copies a rectangle between bitmaps, or within one, plane
    // by plane through the minterm and the plane mask. The number of
    // planes written.
    u32 blt_bitmap(u32 src, i32 sx, i32 sy, u32 dst, i32 dx, i32 dy, i32 width, i32 height,
                   u32 minterm, u32 mask);
    void blt_bitmap_rastport(u32 src, i32 sx, i32 sy, u32 rp, i32 dx, i32 dy, i32 width,
                             i32 height, u32 minterm);
    // CopySBitMap: the layer shows its super bitmap again, from the scroll
    // offset on. SyncSBitMap: the super bitmap takes what the layer shows,
    // so it holds all of the layer's drawing. Neither does anything for a
    // layer without a super bitmap of its own.
    void copy_super_bitmap(u32 layer);
    void sync_super_bitmap(u32 layer);
    // The colour of pen index of the ColorMap, eight bits a gun. False for
    // a bad map or a pen it has not.
    bool color_of(u32 colormap, u32 index, u8& red, u8& green, u8& blue);
    void draw_line(u32 rp, i32 x1, i32 y1);
    // Draws the characters in the built-in font, whatever font the
    // RastPort has, and moves its pen past them, for text laid out in
    // that font's cells. How far the pen moved.
    u32 text(u32 rp, const u8* chars, u32 count);
    void scroll(u32 rp, i32 dx, i32 dy, i32 x0, i32 y0, i32 x1, i32 y1);
    void set_font(u32 rp, u32 font);
    // Records the display mode of a ViewPort, for GetVPModeID, and
    // forgets it.
    void set_vp_mode_id(u32 vp, u32 id);
    void clear_vp_mode_id(u32 vp);

private:
    friend struct GraphicsNatives;

    static constexpr u32 kVectors = graphics_lvo::Last / 6;

    m68k::Machine* machine_ = nullptr;
    Exec* exec_ = nullptr;
    DisplayState* display_ = nullptr;
    u32 base_ = 0;
    u32 topaz_ = 0;
    u32 topaz_name_ = 0;
    u32 topaz_attr_ = 0;
    // The MonitorSpecs OpenMonitor hands out for the default, NTSC and PAL
    // monitors, made on first use.
    u32 monitor_ = 0;
    u32 ntsc_monitor_ = 0;
    u32 pal_monitor_ = 0;
    u32 hombre_monitor_ = 0;
    DeepBitmaps deep_bitmaps_{};
    struct HostPalette {
        u32 colormap = 0;
        u32 rgb[256]{};
    };
    static constexpr u32 kMaxPalettes = 8;
    HostPalette palettes_[kMaxPalettes]{};
    void sync_palette(HostPalette& palette);
    // The display modes of the ViewPorts registered for GetVPModeID.
    static constexpr u32 kVpModeIds = 256;
    struct VpModeId {
        u32 vp;
        u32 id;
    };
    bool vp_mode_id(u32 vp, u32& id) const;
    std::vector<VpModeId> vp_mode_ids_;
    NativeVector vectors_[kVectors]{};
    // Who holds each pen of the screen's colour map, for ObtainPen and
    // ReleasePen: how many programs share it, and whether one holds it alone.
    // Besides them the system holds the first kSystemPens and the chrome's
    // pens of a screen of that many planes, which stay shared.
    static constexpr u32 kPens = 256;
    static constexpr u32 kSystemPens = 8;
    u16 pen_users_[kPens]{};
    bool pen_exclusive_[kPens]{};
    // The palettes text has blended through lately: four ColorMaps by
    // their guest address, each with the palette_gen its cache holds, the
    // least recently used one reused first.
    static constexpr u32 kBlendCaches = 4;
    struct BlendCache {
        text_blend::Cache cache;
        u32 map = 0;
        u32 palette_gen = 0;
        u32 used = 0;
    };
    BlendCache blend_caches_[kBlendCaches]{};
    u32 blend_clock_ = 0;
    // A TextFont with its glyph tables, in one block of exec's memory and
    // at the end of the fonts in memory, named by the guest string at
    // name. 0 if exec has no memory for it.
    u32 add_rom_font(native::Bus& b, const RomFont& font, u32 name);
    // The glyph tables and the styles rp draws text with: those of its
    // font, or of the font at own, or of the built-in font for one without
    // glyph tables. False if the RastPort cannot be read.
    bool text_font(native::Bus& b, u32 rp, raster::Font& font, raster::TextStyle& style);
    bool text_font(native::Bus& b, u32 rp, u32 own, raster::Font& font,
                   raster::TextStyle& style);
    // Draws the characters in the RastPort's draw mode and moves its pen
    // past them; in its font and styles for own_font, else in the
    // built-in font. How far the pen moved.
    u32 draw_text(u32 rp, const u8* chars, u32 count, bool own_font);
    // Draws the characters in font through the RastPort's pens, draw mode
    // and style, with blend for the antialiasing Text has, and moves its
    // pen past them. How far the pen moved.
    u32 draw_run(native::Bus& b, u32 rp, const raster::Font& font,
                 const raster::TextStyle& style, const u8* chars, u32 count, bool blend);
    // One tf_Accessor more or fewer, as OpenFont and CloseFont count them.
    void acquire_font(native::Bus& b, u32 font);
    void release_font(native::Bus& b, u32 font);
    // The ramp cache of the palette of a bitmap of depth planes, which holds
    // the colours of its 2^depth pens, or nullptr when it has too few planes
    // for text to be blended or Intuition knows no ColorMap for it.
    text_blend::Cache* blend_for(u32 bitmap_addr, u32 depth);
    void blit_deep(native::Bus& b, const raster::Bitmap& src, i32 sx, i32 sy,
                   const raster::Bitmap& dst, const raster::Clip* clip, i32 dx, i32 dy,
                   i32 width, i32 height, u32 minterm, u32 mask);
    u32 blit(native::Bus& b, u32 src, i32 sx, i32 sy, const raster::RastPortView& v,
             const raster::Clip* clip, i32 dx, i32 dy, i32 width, i32 height, u32 minterm,
             u32 mask);
    // One row of BltBitMap's source plane, a byte a pixel.
    u8 blit_row_[4096]{};
};

}
