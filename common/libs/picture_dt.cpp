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

#include "picture_dt.hpp"

#include "../image/ilbm.hpp"
#include "boopsi.hpp"
#include "dos.hpp"
#include "graphics.hpp"
#include "intuition.hpp"
#include "tags.hpp"

namespace hombre {

using m68k::Machine;
using native::Bus;
using native::LogLine;
using namespace ndk;
namespace ilbm = image::ilbm;

namespace {

constexpr char kIdString[] = "picture.datatype 60.0 (Hombre)\r\n";

// datatypes.library's DoAsyncLayout(object A0, gpLayout A1).
constexpr u32 kDoAsyncLayout = 84;

// Attributes (datatypes/datatypesclass.h, datatypes/pictureclass.h).
constexpr u32 DTA_Dummy = 0x80001000;
constexpr u32 DTA_TopVert = DTA_Dummy + 11;
constexpr u32 DTA_VisibleVert = DTA_Dummy + 12;
constexpr u32 DTA_TotalVert = DTA_Dummy + 13;
constexpr u32 DTA_TopHoriz = DTA_Dummy + 15;
constexpr u32 DTA_VisibleHoriz = DTA_Dummy + 16;
constexpr u32 DTA_TotalHoriz = DTA_Dummy + 17;
constexpr u32 DTA_Title = DTA_Dummy + 20;
constexpr u32 DTA_Methods = DTA_Dummy + 24;
constexpr u32 DTA_Busy = DTA_Dummy + 28;
constexpr u32 DTA_ErrorLevel = DTA_Dummy + 32;
constexpr u32 DTA_ErrorNumber = DTA_Dummy + 33;
constexpr u32 DTA_Domain = DTA_Dummy + 104;
constexpr u32 DTA_ObjName = DTA_Dummy + 109;
constexpr u32 DTA_FrameInfo = DTA_Dummy + 116;
constexpr u32 DTA_SelectDomain = DTA_Dummy + 121;
constexpr u32 DTA_NominalVert = DTA_Dummy + 124;
constexpr u32 DTA_NominalHoriz = DTA_Dummy + 125;

constexpr u32 PDTA_ModeID = DTA_Dummy + 200;
constexpr u32 PDTA_BitMapHeader = DTA_Dummy + 201;
constexpr u32 PDTA_BitMap = DTA_Dummy + 202;
constexpr u32 PDTA_ColorRegisters = DTA_Dummy + 203;
constexpr u32 PDTA_CRegs = DTA_Dummy + 204;
constexpr u32 PDTA_GRegs = DTA_Dummy + 205;
constexpr u32 PDTA_ColorTable = DTA_Dummy + 206;
constexpr u32 PDTA_ColorTable2 = DTA_Dummy + 207;
constexpr u32 PDTA_Allocated = DTA_Dummy + 208;
constexpr u32 PDTA_NumColors = DTA_Dummy + 209;
constexpr u32 PDTA_NumAlloc = DTA_Dummy + 210;
constexpr u32 PDTA_Remap = DTA_Dummy + 211;
constexpr u32 PDTA_Screen = DTA_Dummy + 212;
constexpr u32 PDTA_FreeSourceBitMap = DTA_Dummy + 213;
constexpr u32 PDTA_Grab = DTA_Dummy + 214;
constexpr u32 PDTA_DestBitMap = DTA_Dummy + 215;
constexpr u32 PDTA_ClassBitMap = DTA_Dummy + 216;
constexpr u32 PDTA_NumSparse = DTA_Dummy + 217;
constexpr u32 PDTA_SparseTable = DTA_Dummy + 218;
constexpr u32 OBP_Precision = 0x84000000;

// Methods.
constexpr u32 DTM_FRAMEBOX = 0x601;
constexpr u32 DTM_PROCLAYOUT = 0x602;
constexpr u32 DTM_ASYNCLAYOUT = 0x603;
constexpr u32 DTM_SELECT = 0x605;
constexpr u32 DTM_CLEARSELECTED = 0x606;
constexpr u32 DTM_WRITE = 0x650;

// The methods DTA_Methods lists, ~0 last.
constexpr u32 kMethods[] = {
    OM_NEW, OM_GET, OM_SET, OM_UPDATE, OM_DISPOSE,
    GM_LAYOUT, GM_HITTEST, GM_GOACTIVE, GM_HANDLEINPUT, GM_RENDER,
    DTM_FRAMEBOX, DTM_SELECT, DTM_CLEARSELECTED, DTM_WRITE, 0xFFFFFFFF,
};

// The stipple a selection is marked with: four rows, a pixel in four.
constexpr u16 kStipple[] = {0x4444, 0x1111, 0x4444, 0x1111};
constexpr u32 kStippleRowsLog2 = 2;
constexpr u32 kSelectPen = 2;

// struct DTSpecialInfo, which datatypesclass keeps for every object.
constexpr u32 si_Lock = 0;
constexpr u32 si_Flags = 46;
constexpr u32 si_TopVert = 50;
constexpr u32 si_VisVert = 54;
constexpr u32 si_TotVert = 58;
constexpr u32 si_OTopVert = 62;
constexpr u32 si_VertUnit = 66;
constexpr u32 si_TopHoriz = 70;
constexpr u32 si_VisHoriz = 74;
constexpr u32 si_TotHoriz = 78;
constexpr u32 si_OTopHoriz = 82;
constexpr u32 si_HorizUnit = 86;
constexpr u32 DTSIF_LAYOUT = 1;
constexpr u32 DTSIF_HIGHLIGHT = 0x10;

// struct FrameInfo.
constexpr u32 fri_PropertyFlags = 0;
constexpr u32 fri_Resolution = 4;
constexpr u32 fri_RedBits = 8;
constexpr u32 fri_Width = 12;
constexpr u32 fri_Height = 16;
constexpr u32 fri_Depth = 20;
constexpr u32 fri_ColorMap = 28;
constexpr u32 FrameInfo_SIZEOF = 36;
constexpr u32 FRAMEF_SPECIFY = 1;

// struct DisplayInfo and struct DimensionInfo (graphics/displayinfo.h).
constexpr u32 DTAG_DISP = 0x80000000;
constexpr u32 DTAG_DIMS = 0x80001000;
constexpr u32 dis_NotAvailable = 16;
constexpr u32 dis_PropertyFlags = 18;
constexpr u32 dis_Resolution = 22;
constexpr u32 dis_RedBits = 40;
constexpr u32 DisplayInfo_SIZEOF = 56;
constexpr u32 dim_MaxDepth = 16;
constexpr u32 DimensionInfo_SIZEOF = 88;
constexpr u32 BIDTAG_DIPFMustHave = 0x80000001;
constexpr u32 BIDTAG_NominalWidth = 0x80000004;
constexpr u32 BIDTAG_NominalHeight = 0x80000005;
constexpr u32 BIDTAG_DesiredWidth = 0x80000006;
constexpr u32 BIDTAG_DesiredHeight = 0x80000007;
constexpr u32 BIDTAG_Depth = 0x80000008;
constexpr u32 BIDTAG_SourceID = 0x8000000A;
constexpr u32 DIPF_IS_HAM = 0x8;
constexpr u32 DIPF_IS_EXTRAHALFBRITE = 0x1000;
constexpr u32 kHamMode = 0x800;
constexpr u32 kHalfBriteMode = 0x80;
constexpr u32 kInvalidId = 0xFFFFFFFF;

// GetBitMapAttr's attributes, AllocBitMap's BMF_CLEAR, the draw modes and
// the blit's copy minterm.
constexpr u32 BMA_HEIGHT = 0;
constexpr u32 BMA_DEPTH = 4;
constexpr u32 BMA_WIDTH = 8;
constexpr u32 BMF_CLEAR = 1;
constexpr u32 kCopy = 0xC0;

// The struct BitMapHeader the class keeps and its subclass fills.
constexpr u32 bmh_Width = 0;
constexpr u32 bmh_Height = 2;
constexpr u32 bmh_Left = 4;
constexpr u32 bmh_Top = 6;
constexpr u32 bmh_Depth = 8;
constexpr u32 bmh_Masking = 9;
constexpr u32 bmh_Transparent = 12;
constexpr u32 bmh_XAspect = 14;
constexpr u32 bmh_YAspect = 15;
constexpr u32 bmh_PageWidth = 16;
constexpr u32 bmh_PageHeight = 18;
constexpr u32 BitMapHeader_SIZEOF = 20;

// The instance data, in guest memory after datatypesclass's.
constexpr u32 kModeId = 0;
constexpr u32 kHeader = 4;
constexpr u32 kGrab = 24;
// The bitmap PDTA_BitMap handed over, which the class frees; the source the
// layout reads (PDTA_BitMap's or PDTA_ClassBitMap's); the destination.
constexpr u32 kOwnedSource = 28;
constexpr u32 kSource = 32;
constexpr u32 kDest = 36;
constexpr u32 kScreen = 40;
// One block of exec's memory for all colour tables, and its size.
constexpr u32 kColours = 44;
constexpr u32 kColoursBytes = 48;
constexpr u32 kCRegs = 52;
constexpr u32 kGRegs = 56;
constexpr u32 kColorTable = 60;
constexpr u32 kColorTable2 = 64;
constexpr u32 kAllocated = 68;
constexpr u32 kNumColors = 72;  // word
constexpr u32 kNumAlloc = 74;   // word
// The ColorMap the pens of the allocated list came from.
constexpr u32 kColorMap = 76;
constexpr u32 kFlags = 80;
constexpr u32 kPrecision = 84;
constexpr u32 kNumSparse = 88;  // word
constexpr u32 kSparseTable = 92;
// From 96 on: room for the source and destination modes, pixel buffers,
// masks and alpha of the V43 interface, which the class does not offer yet.
constexpr u32 kInstBytes = 160;

// kFlags.
constexpr u32 kRemap = 1;
constexpr u32 kFreeSource = 2;
constexpr u32 kSparse = 4;
// The destination is the source bitmap itself.
constexpr u32 kAlias = 8;

// A colour index takes this many entries of the tables at most.
constexpr u32 kMaxPens = 256;
// Remapped pixels are worked on in runs of this many, and other tasks get
// a turn after about every kYieldPixels of them.
constexpr u32 kRun = 256;
constexpr u32 kYieldPixels = 65536;
// What DTM_WRITE gathers before it writes, in guest memory.
constexpr u32 kWriteBuffer = 16384;
constexpr u32 kMaxName = 256;

// Squared distances of colours above this get a second pen to mix with.
constexpr u32 kMixThreshold = 2000;

constexpr u32 kErrorLevelFail = 20;
constexpr u32 DTERROR_INVALID_DATA = 2008;

// Guest memory for the arguments of a call, freed when it goes.
class Scratch {
public:
    Scratch(Exec& exec, u32 bytes) : exec_(exec), bytes_(bytes) {
        addr_ = exec.alloc(bytes, MEMF_PUBLIC | MEMF_CLEAR);
    }
    ~Scratch() {
        if (addr_ != 0) {
            exec_.free(addr_, bytes_);
        }
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    u32 at() const { return addr_; }

private:
    Exec& exec_;
    u32 bytes_;
    u32 addr_ = 0;
};

struct Rgb {
    u8 r = 0;
    u8 g = 0;
    u8 b = 0;
};

u32 distance(const Rgb& a, const Rgb& b) {
    const i32 dr = i32{a.r} - b.r;
    const i32 dg = i32{a.g} - b.g;
    const i32 db = i32{a.b} - b.b;
    return static_cast<u32>(dr * dr + dg * dg + db * db);
}

Rgb mix(const Rgb& a, const Rgb& b) {
    return Rgb{static_cast<u8>((a.r + b.r) / 2), static_cast<u8>((a.g + b.g) / 2),
               static_cast<u8>((a.b + b.b) / 2)};
}

// A planar bitmap as the class reads and writes it.
struct Planar {
    u32 bytes_per_row = 0;
    u32 rows = 0;
    u32 depth = 0;
    u32 planes[8]{};
};

bool load_planar(Bus& b, u32 bitmap, Planar& out) {
    out.bytes_per_row = b.r16(bitmap + bm_BytesPerRow);
    out.rows = b.r16(bitmap + bm_Rows);
    out.depth = b.r8(bitmap + bm_Depth);
    if (!b.ok() || out.depth == 0 || out.depth > 8) {
        return false;
    }
    for (u32 p = 0; p < out.depth; ++p) {
        out.planes[p] = b.r32(bitmap + bm_Planes + p * 4);
    }
    return b.ok();
}

// The pixels x0 to x0 + count - 1 of row y as colour indexes.
bool read_indexes(Bus& b, const Planar& src, u32 y, u32 x0, u32 count, u8* out) {
    for (u32 i = 0; i < count; ++i) {
        out[i] = 0;
    }
    const u32 first = x0 >> 3;
    const u32 last = (x0 + count - 1) >> 3;
    for (u32 p = 0; p < src.depth; ++p) {
        const u8* row = b.bytes_at(src.planes[p] + y * src.bytes_per_row + first,
                                   last - first + 1);
        if (row == nullptr) {
            return false;
        }
        for (u32 i = 0; i < count; ++i) {
            const u32 x = x0 + i;
            out[i] |= static_cast<u8>(((row[(x >> 3) - first] >> (7 - (x & 7))) & 1) << p);
        }
    }
    return true;
}

}

struct PictureDataType::Ops {
    PictureDataType& p;
    Bus b;
    u32 cl;
    u32 object;
    u32 data;

    Ops(PictureDataType& self, u32 cls, u32 obj)
        : p(self), b(*self.machine_), cl(cls), object(obj), data(0) {
        const u32 own = self.class_ptr() != 0 ? self.class_ptr() : cls;
        data = obj + b.r16(own + cl_InstOffset);
    }

    Boopsi& boopsi() { return p.intuition_->boopsi(); }
    bool stopped() const { return !b.ok() || p.machine_->cpu.exit != m68k::ExitReason::None; }

    u32 get(u32 offset) { return b.r32(data + offset); }
    void put(u32 offset, u32 value) { b.w32(data + offset, value); }
    u32 header16(u32 field) { return b.r16(data + kHeader + field); }
    u32 flags() { return get(kFlags); }
    u32 special_info() { return b.r32(object + gg_SpecialInfo); }
    u32 gadget_id() { return b.r16(object + gg_GadgetID); }

    // A library function through its vector; 0 when the call failed, which
    // has ended the run.
    u32 call(u32 base, u32 lvo, const LibraryCall& args) {
        u32 d0 = 0;
        return base != 0 && p.exec_->call_library(base, lvo, args, d0) ? d0 : 0;
    }
    u32 gfx(u32 lvo, const LibraryCall& args) {
        return call(p.graphics_->graphics_base(), lvo, args);
    }
    u32 intuition(u32 lvo, const LibraryCall& args) {
        return call(p.intuition_->intuition_base(), lvo, args);
    }
    u32 exec(u32 lvo, u32 a0) {
        LibraryCall args;
        args.a[0] = a0;
        return call(p.exec_->exec_base(), lvo, args);
    }

    // GetAttr on the object as its own class answers it; 0 if it does not.
    u32 attr(u32 id) {
        Scratch storage(*p.exec_, 4);
        if (storage.at() == 0) {
            return 0;
        }
        return boopsi().get_attr(id, object, storage.at()) != 0 ? b.r32(storage.at()) : 0;
    }

    void notify(const u32* pairs, u32 count, u32 ginfo) {
        boopsi().notify_attrs(object, pairs, count, ginfo, 0);
    }

    // GM_RENDER to the object, as DoMethod sends it, through the RastPort of
    // the GadgetInfo.
    void render(u32 ginfo, u32 redraw) {
        if (ginfo == 0) {
            return;
        }
        LibraryCall args;
        args.a[0] = ginfo;
        const u32 rp = intuition(intuition_lvo::ObtainGIRPort, args);
        if (rp == 0 || stopped()) {
            return;
        }
        const u32 msg[] = {GM_RENDER, ginfo, rp, redraw};
        boopsi().send(boopsi().class_of(object), object, msg, 4);
        LibraryCall release;
        release.a[0] = rp;
        intuition(intuition_lvo::ReleaseGIRPort, release);
    }

    void free_bitmap(u32 bitmap) {
        if (bitmap == 0) {
            return;
        }
        LibraryCall args;
        args.a[0] = bitmap;
        gfx(graphics_lvo::FreeBitMap, args);
    }

    void wait_blit() { gfx(graphics_lvo::WaitBlit, LibraryCall{}); }

    // Gives back the pens of the last remap and the destination it made.
    void release_remap() {
        release_pens();
        const u32 dest = get(kDest);
        if (dest != 0 && (flags() & kAlias) == 0) {
            wait_blit();
            free_bitmap(dest);
        }
        put(kDest, 0);
        put(kFlags, flags() & ~kAlias);
    }

    // The colour tables for count colours, all in one block: ColorRegisters
    // (3 bytes a colour), CRegs and GRegs (3 longs), ColorTable,
    // ColorTable2 and the allocated pens (a byte).
    void make_colour_tables(u32 count) {
        release_pens();
        const u32 old = get(kColours);
        if (old != 0) {
            p.exec_->free(old, get(kColoursBytes));
        }
        const u32 registers = (count * 3 + 3) & ~3u;
        const u32 bytes = registers + count * 24 + ((count + 3) & ~3u) * 3;
        const u32 block = count != 0 ? p.exec_->alloc(bytes, MEMF_PUBLIC | MEMF_CLEAR) : 0;
        const u32 bytes_table = (count + 3) & ~3u;
        put(kColours, block);
        put(kColoursBytes, block != 0 ? bytes : 0);
        put(kCRegs, block != 0 ? block + registers : 0);
        put(kGRegs, block != 0 ? block + registers + count * 12 : 0);
        put(kColorTable, block != 0 ? block + registers + count * 24 : 0);
        put(kColorTable2, block != 0 ? block + registers + count * 24 + bytes_table : 0);
        put(kAllocated, block != 0 ? block + registers + count * 24 + bytes_table * 2 : 0);
        if (block == 0 && count != 0) {
            LogLine().text("picture.datatype: no memory for ").dec(count).text(" colours").send();
        }
    }

    // The pens go back before the list that names them does.
    void release_pens() {
        const u32 count = b.r16(data + kNumAlloc);
        const u32 list = get(kAllocated);
        const u32 map = get(kColorMap);
        for (u32 i = count; i > 0 && list != 0 && map != 0; --i) {
            LibraryCall args;
            args.a[0] = map;
            args.d[0] = b.r8(list + i - 1);
            gfx(graphics_lvo::ReleasePen, args);
        }
        b.w16(data + kNumAlloc, 0);
    }

    // The FrameInfo datatypesclass keeps, which DTM_FRAMEBOX hands out.
    u32 frame_info() { return attr(DTA_FrameInfo); }

    // What the display mode says of the picture's frame: its properties,
    // resolution and colour bits, and at most its deepest depth. A mode the
    // display database does not have is replaced by the best one there is
    // for the picture.
    void mode_changed() {
        const u32 info = frame_info();
        Scratch disp(*p.exec_, DimensionInfo_SIZEOF);
        Scratch tags(*p.exec_, 8 * 8);
        if (info == 0 || disp.at() == 0 || tags.at() == 0 || stopped()) {
            return;
        }
        const auto display_info = [&](u32 id) {
            b.w16(disp.at() + dis_NotAvailable, 1);
            LibraryCall args;
            args.a[0] = 0;
            args.a[1] = disp.at();
            args.d[0] = DisplayInfo_SIZEOF;
            args.d[1] = DTAG_DISP;
            args.d[2] = id;
            return gfx(graphics_lvo::GetDisplayInfoData, args) != 0 &&
                   b.r16(disp.at() + dis_NotAvailable) == 0;
        };
        u32 id = get(kModeId);
        if (!display_info(id)) {
            const u32 must = (id & kHamMode) != 0         ? DIPF_IS_HAM
                             : (id & kHalfBriteMode) != 0 ? DIPF_IS_EXTRAHALFBRITE
                                                          : 0;
            const u32 pairs[] = {BIDTAG_SourceID,      id,
                                 BIDTAG_DIPFMustHave,  must,
                                 BIDTAG_NominalWidth,  header16(bmh_PageWidth),
                                 BIDTAG_NominalHeight, header16(bmh_PageHeight),
                                 BIDTAG_DesiredWidth,  header16(bmh_Width),
                                 BIDTAG_DesiredHeight, header16(bmh_Height),
                                 BIDTAG_Depth,         b.r8(data + kHeader + bmh_Depth)};
            for (u32 i = 0; i < 14; ++i) {
                b.w32(tags.at() + i * 4, pairs[i]);
            }
            b.w32(tags.at() + 56, tags::kTagDone);
            LibraryCall args;
            args.a[0] = tags.at();
            const u32 best = gfx(graphics_lvo::BestModeIDA, args);
            if (stopped() || best == kInvalidId) {
                return;
            }
            id = best;
            put(kModeId, id);
            if (!display_info(id)) {
                return;
            }
        }
        const u32 property = b.r32(disp.at() + dis_PropertyFlags);
        const u32 resolution = b.r32(disp.at() + dis_Resolution);
        const u32 bits = b.r32(disp.at() + dis_RedBits) & 0xFFFFFF00;
        b.w32(info + fri_PropertyFlags, property);
        b.w32(info + fri_Resolution, resolution);
        b.w8(info + fri_RedBits, bits >> 24);
        b.w8(info + fri_RedBits + 1, bits >> 16);
        b.w8(info + fri_RedBits + 2, bits >> 8);
        LibraryCall args;
        args.a[1] = disp.at();
        args.d[0] = DimensionInfo_SIZEOF;
        args.d[1] = DTAG_DIMS;
        args.d[2] = id;
        if (gfx(graphics_lvo::GetDisplayInfoData, args) != 0) {
            const u32 deepest = b.r16(disp.at() + dim_MaxDepth);
            if (b.r32(info + fri_Depth) > deepest) {
                b.w32(info + fri_Depth, deepest);
            }
        }
    }

    // The frame's size and depth are the source bitmap's.
    void source_changed() {
        const u32 info = frame_info();
        const u32 source = get(kSource);
        if (info == 0 || source == 0 || stopped()) {
            return;
        }
        const auto bitmap_attr = [&](u32 which) {
            LibraryCall args;
            args.a[0] = source;
            args.d[1] = which;
            return gfx(graphics_lvo::GetBitMapAttr, args);
        };
        b.w32(info + fri_Width, bitmap_attr(BMA_WIDTH));
        b.w32(info + fri_Height, bitmap_attr(BMA_HEIGHT));
        b.w32(info + fri_Depth, bitmap_attr(BMA_DEPTH));
    }

    // A bitmap the class owned and that PDTA_BitMap replaces is freed; a
    // destination that was that bitmap itself goes with it.
    void replace_owned(u32 bitmap) {
        const u32 old = get(kOwnedSource);
        if (old == 0 || old == bitmap) {
            return;
        }
        if (get(kDest) == old) {
            put(kDest, 0);
            put(kFlags, flags() & ~kAlias);
        }
        wait_blit();
        free_bitmap(old);
        put(kOwnedSource, 0);
    }

    // The attributes of OM_NEW, OM_SET and OM_UPDATE: 1 when one that moves
    // the view changed, so the picture is drawn again.
    u32 set_attrs(u32 list) {
        const u32 old_colours = b.r16(data + kNumColors);
        const u32 old_source = get(kSource);
        const u32 old_mode = get(kModeId);
        u32 changed = 0;
        u32 budget = tags::kMaxTagItems;
        for (u32 item = tags::next(b, list, budget); item != 0;
             item = tags::next(b, list, budget)) {
            const u32 tag = b.r32(item);
            const u32 value = b.r32(item + 4);
            switch (tag) {
            case DTA_VisibleVert:
            case DTA_TotalVert:
            case DTA_VisibleHoriz:
            case DTA_TotalHoriz:
                changed = 1;
                break;
            case PDTA_ModeID:
                put(kModeId, value);
                break;
            case PDTA_BitMap:
                replace_owned(value);
                put(kOwnedSource, value);
                put(kSource, value);
                break;
            case PDTA_ClassBitMap:
                put(kSource, value);
                break;
            case PDTA_NumColors:
                b.w16(data + kNumColors, value & 0xFFFF);
                break;
            case PDTA_Remap:
                put(kFlags, value != 0 ? flags() | kRemap : flags() & ~kRemap);
                break;
            case PDTA_Screen:
                put(kScreen, value);
                break;
            case PDTA_FreeSourceBitMap:
                put(kFlags, value != 0 ? flags() | kFreeSource : flags() & ~kFreeSource);
                break;
            case PDTA_Grab:
                if (value != 0) {
                    put(kGrab, b.r32(value));
                }
                break;
            case PDTA_NumSparse:
                b.w16(data + kNumSparse, value & 0xFFFF);
                put(kFlags, value != 0 ? flags() | kSparse : flags() & ~kSparse);
                break;
            case PDTA_SparseTable:
                put(kSparseTable, value);
                break;
            case OBP_Precision:
                put(kPrecision, value);
                break;
            default:
                break;
            }
        }
        if (!b.ok()) {
            return 0;
        }
        const u32 colours = b.r16(data + kNumColors);
        if (colours != old_colours) {
            make_colour_tables(colours);
            const u32 info = frame_info();
            if (info != 0) {
                b.w32(info + fri_ColorMap, get(kCRegs));
            }
        }
        if (get(kSource) != old_source) {
            source_changed();
        }
        if (get(kModeId) != old_mode) {
            mode_changed();
        }
        return changed;
    }

    u32 om_new(u32 msg) {
        const u32 made = p.intuition_->boopsi().call_super(cl, object, msg);
        if (made == 0 || stopped()) {
            return 0;
        }
        object = made;
        data = made + b.r16(p.class_ptr() + cl_InstOffset);
        put(kModeId, kInvalidId);
        put(kFlags, kRemap);
        put(kPrecision, 0);
        const u32 si = special_info();
        if (si != 0) {
            b.w32(si + si_HorizUnit, 1);
            b.w32(si + si_VertUnit, 1);
            b.w32(si + si_OTopHoriz, 0xFFFFFFFF);
            b.w32(si + si_OTopVert, 0xFFFFFFFF);
        }
        set_attrs(b.r32(msg + 4));
        return b.ok() ? made : 0;
    }

    u32 om_dispose(u32 msg) {
        wait_blit();
        release_remap();
        const u32 block = get(kColours);
        if (block != 0) {
            p.exec_->free(block, get(kColoursBytes));
            put(kColours, 0);
        }
        free_bitmap(get(kOwnedSource));
        put(kOwnedSource, 0);
        put(kSource, 0);
        return stopped() ? 0 : boopsi().pass_on(cl, object, msg);
    }

    u32 om_set(u32 msg) {
        const u32 result = boopsi().call_super(cl, object, msg);
        if (stopped()) {
            return 0;
        }
        if (result + set_attrs(b.r32(msg + 4)) != 0 && !stopped()) {
            render(b.r32(msg + 8), GREDRAW_UPDATE);
        }
        return 0;
    }

    u32 om_get(u32 msg) {
        const u32 id = b.r32(msg + 4);
        const u32 storage = b.r32(msg + 8);
        u32 value = 0;
        switch (id) {
        case DTA_Methods:
            value = p.methods_;
            break;
        case PDTA_ModeID:
            value = get(kModeId);
            break;
        case PDTA_BitMapHeader:
            value = data + kHeader;
            break;
        case PDTA_BitMap:
        case PDTA_DestBitMap:
        case PDTA_ClassBitMap:
            value = get(kDest);
            break;
        case PDTA_ColorRegisters:
            value = get(kColours);
            break;
        case PDTA_CRegs:
            value = get(kCRegs);
            break;
        case PDTA_GRegs:
            value = get(kGRegs);
            break;
        case PDTA_ColorTable:
            value = get(kColorTable);
            break;
        case PDTA_ColorTable2:
            value = get(kColorTable2);
            break;
        case PDTA_Allocated:
            value = get(kAllocated);
            break;
        case PDTA_NumColors:
            value = static_cast<u32>(static_cast<i32>(static_cast<i16>(b.r16(data + kNumColors))));
            break;
        case PDTA_NumAlloc:
            value = static_cast<u32>(static_cast<i32>(static_cast<i16>(b.r16(data + kNumAlloc))));
            break;
        case PDTA_Grab:
            value = data + kGrab;
            break;
        default:
            return boopsi().pass_on(cl, object, msg);
        }
        b.w32(storage, value);
        return b.ok() ? 1 : 0;
    }

    void notify_busy(u32 ginfo) {
        const u32 pairs[] = {GA_ID, gadget_id(), DTA_Busy, 1};
        notify(pairs, 2, ginfo);
    }

    u32 gm_layout(u32 msg) {
        const u32 ginfo = b.r32(msg + 4);
        notify_busy(ginfo);
        const u32 result = boopsi().call_super(cl, object, msg);
        if (stopped()) {
            return 0;
        }
        // datatypes.library's layout process needs the GadgetInfo; without
        // one the layout runs here.
        if (ginfo == 0) {
            return result + layout(msg);
        }
        LibraryCall args;
        args.a[0] = object;
        args.a[1] = msg;
        return result + call(p.datatypes_, kDoAsyncLayout, args);
    }

    u32 proc_layout(u32 msg) {
        notify_busy(b.r32(msg + 4));
        const u32 result = boopsi().call_super(cl, object, msg);
        return stopped() ? 0 : result + layout(msg);
    }

    // The screen's colours, when the source has its tables: the pen each
    // colour index becomes (pens) and the one it alternates with (second),
    // or on a deep destination of true colour, each index's pixel value.
    struct Mapping {
        u32 value[kMaxPens];
        u32 second[kMaxPens];
        bool converts = false;
    };

    // Lets other tasks run now and then while a large picture is worked on:
    // every kYieldPixels pixels, counted by whole rows of width. What the
    // layout holds then is the DTSpecialInfo's lock, which drawing only
    // attempts, and the pens it obtained.
    void pause(u32 y, u32 width) {
        const u32 rows = width >= kYieldPixels ? 1 : kYieldPixels / (width != 0 ? width : 1);
        if (y != 0 && y % rows == 0) {
            p.exec_->yield_in_native();
        }
    }

    // Which colour indexes the source's pixels use.
    bool used_indexes(const Planar& src, u32 width, u32 height, bool (&used)[kMaxPens]) {
        for (bool& u : used) {
            u = false;
        }
        u8 run[kRun];
        for (u32 y = 0; y < height; ++y) {
            pause(y, width);
            for (u32 x = 0; x < width; x += kRun) {
                const u32 count = width - x < kRun ? width - x : kRun;
                if (!read_indexes(b, src, y, x, count, run)) {
                    return false;
                }
                for (u32 i = 0; i < count; ++i) {
                    used[run[i]] = true;
                }
            }
        }
        return !stopped();
    }

    Rgb creg(u32 index) {
        const u32 at = get(kCRegs) + index * 12;
        return Rgb{static_cast<u8>(b.r32(at) >> 24), static_cast<u8>(b.r32(at + 4) >> 24),
                   static_cast<u8>(b.r32(at + 8) >> 24)};
    }

    // Pens for the used colours on the screen's ColorMap: the best each one
    // ObtainBestPenA gives, or, when it gives none, the nearest FindColor
    // finds without holding it; with a sparse table only the nearest of its
    // pens. Then, where the best pen is far from the colour, a second pen
    // that mixed with it in a checkerboard comes nearer. False when there is
    // no memory for the call's tags.
    bool choose_pens(u32 screen, const bool (&used)[kMaxPens], Mapping& map) {
        const u32 cm = b.r32(screen + sc_ViewPort + vp_ColorMap);
        put(kColorMap, cm);
        const u32 colours = b.r16(data + kNumColors) < kMaxPens ? b.r16(data + kNumColors)
                                                                 : kMaxPens;
        for (u32 i = 0; i < kMaxPens; ++i) {
            map.value[i] = i;
        }
        if (get(kColours) == 0 || cm == 0) {
            for (u32 i = 0; i < kMaxPens; ++i) {
                map.second[i] = i;
            }
            return true;
        }
        const u32 ct = get(kColorTable);
        const u32 ct2 = get(kColorTable2);
        const u32 gregs = get(kGRegs);
        Rgb pen_rgb[kMaxPens];
        bool known[kMaxPens] = {};
        if ((flags() & kSparse) != 0 && get(kSparseTable) != 0) {
            const u32 sparse = get(kSparseTable);
            const u32 count = b.r16(data + kNumSparse) < kMaxPens ? b.r16(data + kNumSparse)
                                                                   : kMaxPens;
            for (u32 i = 0; i < colours; ++i) {
                if (!used[i]) {
                    continue;
                }
                const Rgb want = creg(i);
                u32 best = 0xFFFFFFFF;
                for (u32 k = 0; k < count; ++k) {
                    const u32 pen = b.r8(sparse + k);
                    Rgb have;
                    if (!p.graphics_->color_of(cm, pen, have.r, have.g, have.b)) {
                        continue;
                    }
                    const u32 d = distance(want, have);
                    if (d < best) {
                        best = d;
                        map.value[i] = pen;
                    }
                }
            }
        } else {
            Scratch tags(*p.exec_, 16);
            if (tags.at() == 0) {
                return false;
            }
            b.w32(tags.at(), OBP_Precision);
            b.w32(tags.at() + 4, get(kPrecision));
            b.w32(tags.at() + 8, tags::kTagDone);
            const u32 list = get(kAllocated);
            u32 held = 0;
            for (u32 i = 0; i < colours && !stopped(); ++i) {
                if (!used[i]) {
                    continue;
                }
                const u32 at = get(kCRegs) + i * 12;
                LibraryCall args;
                args.a[0] = cm;
                args.d[1] = b.r32(at);
                args.d[2] = b.r32(at + 4);
                args.d[3] = b.r32(at + 8);
                args.a[1] = tags.at();
                u32 pen = gfx(graphics_lvo::ObtainBestPenA, args);
                if (pen < kMaxPens) {
                    b.w8(list + held, pen);
                    b.w16(data + kNumAlloc, ++held);
                } else {
                    LibraryCall find;
                    find.a[3] = cm;
                    find.d[1] = args.d[1];
                    find.d[2] = args.d[2];
                    find.d[3] = args.d[3];
                    find.d[4] = 0xFFFFFFFF;
                    pen = gfx(graphics_lvo::FindColor, find);
                    if (pen >= kMaxPens || stopped()) {
                        continue;
                    }
                }
                map.value[i] = pen;
                LibraryCall rgb;
                rgb.a[0] = cm;
                rgb.d[0] = pen;
                rgb.d[1] = 1;
                rgb.a[1] = gregs + i * 12;
                gfx(graphics_lvo::GetRGB32, rgb);
                pen_rgb[i] = Rgb{static_cast<u8>(b.r32(gregs + i * 12) >> 24),
                                 static_cast<u8>(b.r32(gregs + i * 12 + 4) >> 24),
                                 static_cast<u8>(b.r32(gregs + i * 12 + 8) >> 24)};
                known[i] = true;
            }
        }
        for (u32 i = 0; i < kMaxPens; ++i) {
            map.second[i] = map.value[i];
            map.converts = map.converts || map.value[i] != i;
        }
        if (map.converts && (flags() & kSparse) == 0) {
            for (u32 i = 0; i < colours; ++i) {
                if (!used[i] || !known[i]) {
                    continue;
                }
                const Rgb want = creg(i);
                u32 best = distance(want, pen_rgb[i]);
                if (best <= kMixThreshold) {
                    continue;
                }
                for (u32 j = 0; j < colours; ++j) {
                    if (!used[j] || !known[j]) {
                        continue;
                    }
                    const u32 d = distance(want, mix(pen_rgb[i], pen_rgb[j]));
                    if (d < best) {
                        best = d;
                        map.second[i] = map.value[j];
                    }
                }
            }
        }
        for (u32 i = 0; i < colours; ++i) {
            if (used[i]) {
                b.w8(ct + i, map.value[i]);
                b.w8(ct2 + i, map.second[i]);
            }
        }
        return b.ok();
    }

    // A deep destination of true colour shows each index in its own colour;
    // GRegs then holds the colours of CRegs and ColorTable the indexes.
    void true_colours(PixFmt fmt, Mapping& map) {
        const u32 colours = b.r16(data + kNumColors) < kMaxPens ? b.r16(data + kNumColors)
                                                                 : kMaxPens;
        const bool tables = get(kColours) != 0;
        for (u32 i = 0; i < kMaxPens; ++i) {
            const Rgb c = tables && i < colours ? creg(i) : Rgb{};
            map.value[i] = encode_rgb(fmt, c.r, c.g, c.b);
            map.second[i] = map.value[i];
            if (tables && i < colours) {
                for (u32 k = 0; k < 12; k += 4) {
                    b.w32(get(kGRegs) + i * 12 + k, b.r32(get(kCRegs) + i * 12 + k));
                }
                b.w8(get(kColorTable) + i, i);
                b.w8(get(kColorTable2) + i, i);
            }
        }
        map.converts = true;
    }

    // The source's pixels through the mapping into the destination: planar
    // pens, LUT8 pens or true colour values, the second table's on every
    // other pixel of a checkerboard.
    bool convert(const Planar& src, u32 dest, u32 width, u32 height, const Mapping& map) {
        const DeepBitmap* deep = p.graphics_->deep(dest);
        Planar dst;
        if (deep == nullptr && !load_planar(b, dest, dst)) {
            return false;
        }
        u8 run[kRun];
        for (u32 y = 0; y < height; ++y) {
            pause(y, width);
            if (stopped()) {
                return false;
            }
            for (u32 x = 0; x < width; x += kRun) {
                const u32 count = width - x < kRun ? width - x : kRun;
                if (!read_indexes(b, src, y, x, count, run)) {
                    return false;
                }
                if (deep != nullptr) {
                    const u32 bpp = bytes_per_pixel(deep->fmt);
                    u8* row = b.bytes_at(deep->pixels + y * deep->bytes_per_row + x * bpp,
                                         count * bpp);
                    if (row == nullptr) {
                        return false;
                    }
                    for (u32 i = 0; i < count; ++i) {
                        const u32 v = ((x + i + y) & 1) == 0 ? map.value[run[i]]
                                                             : map.second[run[i]];
                        store_pixel(deep->fmt, row + i * bpp, v);
                    }
                    continue;
                }
                const u32 first = x >> 3;
                const u32 bytes = ((x + count - 1) >> 3) - first + 1;
                for (u32 plane = 0; plane < dst.depth; ++plane) {
                    u8* row = b.bytes_at(dst.planes[plane] + y * dst.bytes_per_row + first,
                                         bytes);
                    if (row == nullptr) {
                        return false;
                    }
                    for (u32 i = 0; i < count; ++i) {
                        const u32 at = x + i;
                        const u32 pen = ((at + y) & 1) == 0 ? map.value[run[i]]
                                                            : map.second[run[i]];
                        const u8 bit = static_cast<u8>(0x80 >> (at & 7));
                        u8& cell = row[(at >> 3) - first];
                        cell = static_cast<u8>(((pen >> plane) & 1) != 0 ? cell | bit
                                                                         : cell & ~bit);
                    }
                }
            }
        }
        return true;
    }

    // The destination of a first layout: the source itself, or a bitmap
    // made for the screen the source is remapped or copied into. 0, or the
    // error that left the picture without a remapped destination.
    u32 make_destination(u32 screen) {
        const u32 source = get(kSource);
        const bool remap = (flags() & kRemap) != 0;
        if (screen == 0) {
            if (!remap) {
                put(kFlags, flags() | kAlias);
            }
            return 0;
        }
        const u32 screen_bitmap = screen + sc_BitMap;
        const u32 screen_depth = b.r8(b.r32(screen + sc_RastPort + rp_BitMap) + bm_Depth);
        const u32 depth8 = b.r8(data + kHeader + bmh_Depth);
        const bool deeper = get(kScreen) != 0 && depth8 < screen_depth;
        Planar src;
        const bool planar_source = p.graphics_->deep(source) == nullptr &&
                                   load_planar(b, source, src);
        if ((!remap && !deeper) || !planar_source) {
            put(kFlags, flags() | kAlias);
            return 0;
        }
        const u32 width = header16(bmh_Width);
        const u32 height = header16(bmh_Height);
        u32 depth = screen_depth > depth8 ? screen_depth : depth8;
        if (p.graphics_->deep(screen_bitmap) == nullptr && depth > 8) {
            depth = 8;
        }
        LibraryCall args;
        args.d[0] = width;
        args.d[1] = height;
        args.d[2] = depth;
        args.d[3] = BMF_CLEAR;
        args.a[0] = screen_bitmap;
        const u32 dest = width != 0 && height != 0 ? gfx(graphics_lvo::AllocBitMap, args) : 0;
        if (dest == 0) {
            // A copy that cannot be had leaves the source to be shown.
            if (!remap) {
                put(kFlags, flags() | kAlias);
                return 0;
            }
            return ERROR_NO_FREE_STORE;
        }
        put(kDest, dest);
        const u32 w = width < src.bytes_per_row * 8 ? width : src.bytes_per_row * 8;
        const u32 h = height < src.rows ? height : src.rows;
        if (!remap) {
            LibraryCall blit;
            blit.a[0] = source;
            blit.a[1] = dest;
            blit.d[4] = w;
            blit.d[5] = h;
            blit.d[6] = kCopy;
            blit.d[7] = 0xFF;
            gfx(graphics_lvo::BltBitMap, blit);
            return 0;
        }
        bool used[kMaxPens];
        if (!used_indexes(src, w, h, used)) {
            return stopped() ? 0 : DTERROR_INVALID_DATA;
        }
        Mapping map;
        const DeepBitmap* deep = p.graphics_->deep(dest);
        if (deep != nullptr && deep->fmt != PixFmt::Lut8) {
            true_colours(deep->fmt, map);
        } else if (!choose_pens(screen, used, map)) {
            return ERROR_NO_FREE_STORE;
        }
        if (!convert(src, dest, w, h, map)) {
            return stopped() ? 0 : DTERROR_INVALID_DATA;
        }
        return 0;
    }

    // One line for each first layout: the picture and what shows it.
    void log_layout(u32 error) {
        LogLine line;
        line.text("picture.datatype: ").dec(header16(bmh_Width)).text("x")
            .dec(header16(bmh_Height)).text("x").dec(b.r8(data + kHeader + bmh_Depth));
        const u32 dest = get(kDest);
        const DeepBitmap* deep = p.graphics_->deep(dest);
        if (error != 0) {
            line.text(" not remapped, error ").dec(error);
        } else if (dest == 0) {
            line.text(" not shown");
        } else if ((flags() & kAlias) != 0) {
            line.text(" shown as it is");
        } else if (deep != nullptr) {
            line.text(" on a deep bitmap of ").dec(bytes_per_pixel(deep->fmt) * 8).text(" bits");
        } else {
            line.text(" on ").dec(b.r8(dest + bm_Depth)).text(" planes, ")
                .dec(b.r16(data + kNumAlloc)).text(" pens");
        }
        line.send();
    }

    // DTM_ASYNCLAYOUT, and the work of DTM_PROCLAYOUT: a first layout
    // remaps the source for the screen; every layout sets the visible and
    // total sizes from the domain and the picture. 1 when the destination
    // could be had.
    u32 layout(u32 msg) {
        const u32 ginfo = b.r32(msg + 4);
        const u32 initial = b.r32(msg + 8);
        const u32 si = special_info();
        if (si == 0 || !b.ok()) {
            return 0;
        }
        exec(exec_lvo::ObtainSemaphore, si + si_Lock);
        if (stopped()) {
            return 0;
        }
        b.w32(si + si_Flags, b.r32(si + si_Flags) | DTSIF_LAYOUT);
        const u32 name = attr(DTA_ObjName);
        const u32 domain = attr(DTA_Domain);
        if (stopped()) {
            return 0;
        }
        u32 screen = get(kScreen);
        if (screen == 0 && ginfo != 0) {
            screen = b.r32(ginfo + gi_Screen);
        }
        bool first = false;
        u32 error = 0;
        if (initial != 0 && get(kSource) != 0) {
            first = true;
            release_remap();
            error = make_destination(screen);
            if (stopped()) {
                return 0;
            }
            if ((flags() & kAlias) != 0) {
                put(kFlags, flags() & ~kFreeSource);
                put(kDest, get(kSource));
            }
            log_layout(error);
            wait_blit();
            if ((flags() & kFreeSource) != 0 && get(kOwnedSource) != 0) {
                free_bitmap(get(kOwnedSource));
                put(kOwnedSource, 0);
                put(kSource, 0);
            }
        }
        b.w32(si + si_Flags, b.r32(si + si_Flags) & ~DTSIF_LAYOUT);
        if (domain != 0) {
            b.w32(si + si_VisVert, static_cast<u32>(static_cast<i16>(b.r16(domain + 6))));
            b.w32(si + si_TotVert, header16(bmh_Height));
            b.w32(si + si_VisHoriz, static_cast<u32>(static_cast<i16>(b.r16(domain + 4))));
            b.w32(si + si_TotHoriz, header16(bmh_Width));
        }
        if (first && !stopped()) {
            const u32 pairs[] = {GA_ID,           gadget_id(),
                                 DTA_NominalVert, header16(bmh_Height),
                                 DTA_NominalHoriz, header16(bmh_Width),
                                 DTA_ErrorLevel,  error == 0 ? 0 : kErrorLevelFail,
                                 DTA_ErrorNumber, error,
                                 DTA_Title,       name};
            notify(pairs, 6, ginfo);
        }
        exec(exec_lvo::ReleaseSemaphore, si + si_Lock);
        return error == 0 && !stopped() ? 1 : 0;
    }

    // Draws the destination into the domain from the scroll offsets on, and
    // the selection over it, then lets datatypesclass have the method. The
    // destination is read under the DTSpecialInfo's lock, which a layout
    // holds while it makes a new one. The answer is datatypesclass's: no
    // caller reads it (Intuition, RefreshGList, the class's own redraws),
    // and the method goes on to datatypesclass without a run of its own.
    u32 gm_render(u32 msg) {
        const u32 si = special_info();
        if (si == 0 || (b.r32(si + si_Flags) & DTSIF_LAYOUT) != 0 || stopped()) {
            return stopped() ? 0 : boopsi().pass_on(cl, object, msg);
        }
        if (exec(exec_lvo::AttemptSemaphoreShared, si + si_Lock) == 0) {
            return stopped() ? 0 : boopsi().pass_on(cl, object, msg);
        }
        const u32 dest = get(kDest);
        const u32 rp = b.r32(msg + 8);
        if (dest != 0 && rp != 0) {
            const u32 domain = attr(DTA_Domain);
            const u32 select = attr(DTA_SelectDomain);
            if (domain != 0 && !stopped()) {
                draw(rp, domain, select, si, dest);
            }
            b.w32(si + si_OTopVert, b.r32(si + si_TopVert));
            b.w32(si + si_OTopHoriz, b.r32(si + si_TopHoriz));
        }
        exec(exec_lvo::ReleaseSemaphore, si + si_Lock);
        return stopped() ? 0 : boopsi().pass_on(cl, object, msg);
    }

    void draw(u32 rp, u32 domain, u32 select, u32 si, u32 dest) {
        const i32 left = static_cast<i16>(b.r16(domain));
        const i32 top = static_cast<i16>(b.r16(domain + 2));
        const i32 dw = static_cast<i16>(b.r16(domain + 4));
        const i32 dh = static_cast<i16>(b.r16(domain + 6));
        const i32 pw = static_cast<i32>(header16(bmh_Width));
        const i32 ph = static_cast<i32>(header16(bmh_Height));
        const i32 w = pw < dw ? pw : dw;
        const i32 h = ph < dh ? ph : dh;
        if (w <= 0 || h <= 0) {
            return;
        }
        // The offsets stay inside the picture.
        i32 sx = static_cast<i32>(b.r32(si + si_TopHoriz));
        i32 sy = static_cast<i32>(b.r32(si + si_TopVert));
        sx = sx < 0 ? 0 : (sx > pw - w ? pw - w : sx);
        sy = sy < 0 ? 0 : (sy > ph - h ? ph - h : sy);
        LibraryCall blit;
        blit.a[0] = dest;
        blit.d[0] = static_cast<u32>(sx);
        blit.d[1] = static_cast<u32>(sy);
        blit.a[1] = rp;
        blit.d[2] = static_cast<u32>(left);
        blit.d[3] = static_cast<u32>(top);
        blit.d[4] = static_cast<u32>(w);
        blit.d[5] = static_cast<u32>(h);
        blit.d[6] = kCopy;
        gfx(graphics_lvo::BltBitMapRastPort, blit);
        if (select == 0 || stopped()) {
            return;
        }
        i32 x0 = static_cast<i16>(b.r16(select)) - sx;
        i32 y0 = static_cast<i16>(b.r16(select + 2)) - sy;
        i32 x1 = x0 + static_cast<i16>(b.r16(select + 4)) - 1;
        i32 y1 = y0 + static_cast<i16>(b.r16(select + 6)) - 1;
        if (x0 >= w || y0 >= h || x1 < 0 || y1 < 0 || x1 < x0 || y1 < y0) {
            return;
        }
        x0 = x0 < 0 ? 0 : x0;
        y0 = y0 < 0 ? 0 : y0;
        x1 = x1 > w - 1 ? w - 1 : x1;
        y1 = y1 > h - 1 ? h - 1 : y1;
        // The RastPort's pen, draw mode and area pattern are put back after.
        const u32 pattern = b.r32(rp + rp_AreaPtrn);
        const u32 pattern_size = b.r8(rp + rp_AreaPtSz);
        const u32 pen = b.r8(rp + rp_FgPen);
        const u32 mode = b.r8(rp + rp_DrawMode);
        b.w32(rp + rp_AreaPtrn, p.stipple_);
        b.w8(rp + rp_AreaPtSz, kStippleRowsLog2);
        LibraryCall args;
        args.a[1] = rp;
        args.d[0] = ndk::JAM1;
        gfx(graphics_lvo::SetDrMd, args);
        args.d[0] = kSelectPen;
        gfx(graphics_lvo::SetAPen, args);
        LibraryCall fill;
        fill.a[1] = rp;
        fill.d[0] = static_cast<u32>(left + x0);
        fill.d[1] = static_cast<u32>(top + y0);
        fill.d[2] = static_cast<u32>(left + x1);
        fill.d[3] = static_cast<u32>(top + y1);
        gfx(graphics_lvo::RectFill, fill);
        args.d[0] = pen;
        gfx(graphics_lvo::SetAPen, args);
        args.d[0] = mode;
        gfx(graphics_lvo::SetDrMd, args);
        b.w32(rp + rp_AreaPtrn, pattern);
        b.w8(rp + rp_AreaPtSz, pattern_size);
    }

    // The FrameInfo datatypesclass keeps, as DTM_FRAMEBOX's answer; the
    // result is datatypesclass's plus one.
    u32 frame_box(u32 msg) {
        const u32 result = boopsi().call_super(cl, object, msg);
        if (stopped()) {
            return 0;
        }
        const u32 out = b.r32(msg + 12);
        const u32 size = b.r32(msg + 16);
        if ((b.r32(msg + 20) & FRAMEF_SPECIFY) == 0 && out != 0) {
            const u32 info = frame_info();
            const u32 bytes = size < FrameInfo_SIZEOF ? size : FrameInfo_SIZEOF;
            for (u32 i = 0; info != 0 && i < bytes; ++i) {
                b.w8(out + i, b.r8(info + i));
            }
        }
        return result + 1;
    }

    // DTM_SELECT and DTM_CLEARSELECTED: datatypesclass has the selection
    // already; the picture is drawn again with it, or without.
    u32 select(u32 msg, bool clear) {
        const u32 si = special_info();
        if (clear && si != 0) {
            b.w32(si + si_Flags, b.r32(si + si_Flags) & ~DTSIF_HIGHLIGHT);
        }
        render(b.r32(msg + 4), GREDRAW_REDRAW);
        return 0;
    }

    // DTM_WRITE: the source bitmap, or the part of it selected, as an ILBM
    // through the file handle, under the DTSpecialInfo's lock, so that no
    // layout frees the source meanwhile. 1 when it was all written.
    u32 write(u32 msg) {
        const u32 si = special_info();
        if (si != 0) {
            exec(exec_lvo::ObtainSemaphoreShared, si + si_Lock);
        }
        if (stopped()) {
            return 0;
        }
        const char* why = nullptr;
        const u32 result = write_locked(msg, why);
        if (si != 0) {
            exec(exec_lvo::ReleaseSemaphore, si + si_Lock);
        }
        if (why != nullptr) {
            LogLine().text("picture.datatype: could not write, ").text(why).send();
        }
        return result;
    }

    u32 write_locked(u32 msg, const char*& why) {
        const u32 fh = b.r32(msg + 8);
        const u32 source = get(kSource);
        Planar src;
        if (fh == 0) {
            why = "no file";
            return 0;
        }
        if (source == 0 || p.graphics_->deep(source) != nullptr ||
            !load_planar(b, source, src)) {
            why = "no planar source bitmap";
            return 0;
        }
        ilbm::Picture pic;
        ilbm::Header& h = pic.header;
        const u32 width = header16(bmh_Width);
        const u32 height = header16(bmh_Height);
        u32 x0 = 0;
        u32 y0 = 0;
        u32 w = width < src.bytes_per_row * 8 ? width : src.bytes_per_row * 8;
        u32 hgt = height < src.rows ? height : src.rows;
        const u32 select = attr(DTA_SelectDomain);
        if (select != 0) {
            const i32 sx = static_cast<i16>(b.r16(select));
            const i32 sy = static_cast<i16>(b.r16(select + 2));
            const i32 sw = static_cast<i16>(b.r16(select + 4));
            const i32 sh = static_cast<i16>(b.r16(select + 6));
            const i32 cx0 = sx < 0 ? 0 : sx;
            const i32 cy0 = sy < 0 ? 0 : sy;
            const i32 cx1 = sx + sw > static_cast<i32>(w) ? static_cast<i32>(w) : sx + sw;
            const i32 cy1 = sy + sh > static_cast<i32>(hgt) ? static_cast<i32>(hgt) : sy + sh;
            if (cx1 > cx0 && cy1 > cy0) {
                x0 = static_cast<u32>(cx0);
                y0 = static_cast<u32>(cy0);
                w = static_cast<u32>(cx1 - cx0);
                hgt = static_cast<u32>(cy1 - cy0);
            }
        }
        if (w == 0 || hgt == 0) {
            why = "an empty picture";
            return 0;
        }
        if (w > ilbm::kMaxSide || hgt > ilbm::kMaxSide) {
            why = "a side is past 16383 pixels";
            return 0;
        }
        h.width = static_cast<u16>(w);
        h.height = static_cast<u16>(hgt);
        // The corner of the part written, as the header records it.
        h.left = static_cast<i16>(x0);
        h.top = static_cast<i16>(y0);
        h.depth = static_cast<u8>(b.r8(data + kHeader + bmh_Depth));
        if (h.depth == 0 || h.depth > src.depth) {
            h.depth = static_cast<u8>(src.depth);
        }
        // A mask plane is not kept, so none is written.
        const u32 masking = b.r8(data + kHeader + bmh_Masking);
        h.masking = static_cast<u8>(masking == 1 ? 0 : masking);
        h.compression = ilbm::kCompressByteRun1;
        h.transparent = static_cast<u16>(header16(bmh_Transparent));
        h.x_aspect = static_cast<u8>(b.r8(data + kHeader + bmh_XAspect));
        h.y_aspect = static_cast<u8>(b.r8(data + kHeader + bmh_YAspect));
        h.page_width = static_cast<i16>(header16(bmh_PageWidth));
        h.page_height = static_cast<i16>(header16(bmh_PageHeight));
        const u32 mode = get(kModeId);
        pic.has_mode = mode != kInvalidId;
        pic.mode_id = mode;
        u8 colours[kMaxPens * 3];
        u32 count = b.r16(data + kNumColors);
        u32 limit = 1u << h.depth;
        if ((mode & kHamMode) != 0 && pic.has_mode && h.depth > 2) {
            limit = 1u << (h.depth - 2);
        }
        count = count < limit ? count : limit;
        const u32 registers = get(kColours);
        if (registers != 0 && count != 0) {
            const u8* from = b.bytes_at(registers, count * 3);
            if (from == nullptr) {
                return 0;
            }
            for (u32 i = 0; i < count * 3; ++i) {
                colours[i] = from[i];
            }
            pic.colours = colours;
            pic.colour_count = count;
        }
        u8 name[kMaxName];
        const u32 name_at = attr(DTA_ObjName);
        if (name_at != 0) {
            u32 n = 0;
            while (n + 1 < kMaxName) {
                name[n] = static_cast<u8>(b.r8(name_at + n));
                if (!b.ok() || name[n] == 0) {
                    break;
                }
                ++n;
            }
            name[n] = 0;
            pic.name = name;
            pic.name_length = n + 1;
        }
        if (stopped()) {
            return 0;
        }
        if (p.dos_ == 0) {
            p.dos_ = p.exec_->open_library("dos.library", 0);
        }
        Scratch buffer(*p.exec_, kWriteBuffer);
        if (p.dos_ == 0 || buffer.at() == 0) {
            why = p.dos_ == 0 ? "no dos.library" : "no memory";
            return 0;
        }
        u32 filled = 0;
        const auto flush = [&] {
            if (filled == 0) {
                return true;
            }
            LibraryCall args;
            args.d[1] = fh;
            args.d[2] = buffer.at();
            args.d[3] = filled;
            const u32 written = call(p.dos_, dos_lvo::Write, args);
            const bool ok = written == filled && !stopped();
            filled = 0;
            return ok;
        };
        u8 row[ilbm::row_bytes(ilbm::kMaxSide)];
        const u32 row_bytes = ilbm::row_bytes(w);
        const auto rows = [&](u32 plane, u32 y) -> const u8* {
            const u8* line = b.bytes_at(src.planes[plane] + (y0 + y) * src.bytes_per_row,
                                        src.bytes_per_row);
            if (line == nullptr) {
                return nullptr;
            }
            for (u32 i = 0; i < row_bytes; ++i) {
                row[i] = 0;
            }
            for (u32 x = 0; x < w; ++x) {
                const u32 at = x0 + x;
                if (((line[at >> 3] >> (7 - (at & 7))) & 1) != 0) {
                    row[x >> 3] |= static_cast<u8>(0x80 >> (x & 7));
                }
            }
            return row;
        };
        const auto sink = [&](const u8* bytes, u32 n) {
            while (n != 0) {
                const u32 room = kWriteBuffer - filled;
                const u32 part = n < room ? n : room;
                u8* to = b.bytes_at(buffer.at() + filled, part);
                if (to == nullptr) {
                    return false;
                }
                for (u32 i = 0; i < part; ++i) {
                    to[i] = bytes[i];
                }
                filled += part;
                bytes += part;
                n -= part;
                if (filled == kWriteBuffer && !flush()) {
                    return false;
                }
            }
            return true;
        };
        const bool ok = ilbm::write(pic, rows, sink) && flush();
        if (!ok) {
            why = "writing failed";
            return 0;
        }
        LogLine line;
        line.text("picture.datatype: wrote ").dec(w).text("x").dec(hgt).text("x").dec(h.depth);
        line.send();
        return 1;
    }
};

bool PictureDataType::init(Machine& m, Exec& exec, Graphics& graphics, Intuition& intuition) {
    machine_ = &m;
    exec_ = &exec;
    graphics_ = &graphics;
    intuition_ = &intuition;
    datatypes_ = 0;
    dos_ = 0;
    methods_ = 0;
    stipple_ = 0;
    return exec.add_library_provider(LibraryProvider{kLeaf, kHombreLibVersion, make_library, this,
                                                     kDir});
}

u32 PictureDataType::make_library(void* ctx) {
    PictureDataType& self = *static_cast<PictureDataType*>(ctx);
    return self.library_.make(*self.machine_, *self.exec_, kLeaf, kIdString, make_class, ctx);
}

// The class over datatypesclass, which datatypes.library makes when it is
// first opened; it stays open while the class lives.
u32 PictureDataType::make_class(void* ctx, u32) {
    PictureDataType& self = *static_cast<PictureDataType*>(ctx);
    Boopsi& boopsi = self.intuition_->boopsi();
    if (self.datatypes_ == 0) {
        self.datatypes_ = self.exec_->open_library("datatypes.library", 0);
    }
    const u32 super = boopsi.find_class_named("datatypesclass");
    if (super == 0) {
        LogLine().text("picture.datatype: no datatypesclass").send();
        self.exec_->close_library(self.datatypes_);
        self.datatypes_ = 0;
        return 0;
    }
    // The methods list and the stipple in one block, made once and kept.
    if (self.methods_ == 0) {
        const u32 block = self.exec_->alloc(sizeof kMethods + sizeof kStipple, MEMF_PUBLIC);
        if (block == 0) {
            return 0;
        }
        Bus b(*self.machine_);
        for (u32 i = 0; i < sizeof kMethods / sizeof kMethods[0]; ++i) {
            b.w32(block + i * 4, kMethods[i]);
        }
        for (u32 i = 0; i < sizeof kStipple / sizeof kStipple[0]; ++i) {
            b.w16(block + sizeof kMethods + i * 2, kStipple[i]);
        }
        if (!b.ok()) {
            self.exec_->free(block, sizeof kMethods + sizeof kStipple);
            return 0;
        }
        self.methods_ = block;
        self.stipple_ = block + sizeof kMethods;
    }
    return boopsi.make_native_class(kLeaf, super, kInstBytes, dispatch, ctx);
}

u32 PictureDataType::dispatch(void* ctx, u32 cl, u32 object, u32 msg) {
    PictureDataType& self = *static_cast<PictureDataType*>(ctx);
    Ops ops(self, cl, object);
    const u32 method = ops.b.r32(msg);
    if (!ops.b.ok()) {
        return 0;
    }
    switch (method) {
    case OM_NEW:
        return ops.om_new(msg);
    case OM_DISPOSE:
        return ops.om_dispose(msg);
    case OM_SET:
    case OM_UPDATE:
        return ops.om_set(msg);
    case OM_GET:
        return ops.om_get(msg);
    case GM_LAYOUT:
        return ops.gm_layout(msg);
    case GM_RENDER:
        return ops.gm_render(msg);
    case DTM_FRAMEBOX:
        return ops.frame_box(msg);
    case DTM_PROCLAYOUT:
        return ops.proc_layout(msg);
    case DTM_ASYNCLAYOUT:
        return ops.layout(msg);
    case DTM_SELECT:
        return ops.select(msg, false);
    case DTM_CLEARSELECTED:
        return ops.select(msg, true);
    case DTM_WRITE:
        return ops.write(msg);
    default:
        return self.intuition_->boopsi().pass_on(cl, object, msg);
    }
}

}
