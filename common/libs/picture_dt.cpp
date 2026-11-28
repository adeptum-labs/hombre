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
#include "../image/remap.hpp"
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
namespace remap = image::remap;

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
constexpr u32 PDTA_SourceMode = DTA_Dummy + 250;
constexpr u32 PDTA_DestMode = DTA_Dummy + 251;
constexpr u32 PDTA_UseFriendBitMap = DTA_Dummy + 255;
constexpr u32 PDTA_AlphaChannel = DTA_Dummy + 256;
constexpr u32 PDTA_MaskPlane = DTA_Dummy + 258;
constexpr u32 PDTA_ObtainPixelBuffer = DTA_Dummy + 260;
constexpr u32 PDTA_SubClassRendersAll = DTA_Dummy + 261;
constexpr u32 OBP_Precision = 0x84000000;

// The interface modes of PDTA_SourceMode and PDTA_DestMode, and the formats
// and masking kinds of the V43 pixel array messages.
constexpr u32 PMODE_V42 = 0;
constexpr u32 PMODE_V43 = 1;
constexpr u32 PBPAFMT_RGB = 0;
constexpr u32 PBPAFMT_RGBA = 1;
constexpr u32 PBPAFMT_ARGB = 2;
constexpr u32 PBPAFMT_LUT8 = 3;
constexpr u32 PBPAFMT_GREY8 = 4;
constexpr u32 mskHasTransparentColor = 2;
constexpr u32 mskHasAlpha = 4;

// Methods.
constexpr u32 DTM_FRAMEBOX = 0x601;
constexpr u32 DTM_PROCLAYOUT = 0x602;
constexpr u32 DTM_ASYNCLAYOUT = 0x603;
constexpr u32 DTM_SELECT = 0x605;
constexpr u32 DTM_CLEARSELECTED = 0x606;
constexpr u32 DTM_WRITE = 0x650;
constexpr u32 PDTM_WRITEPIXELARRAY = 0x660;
constexpr u32 PDTM_READPIXELARRAY = 0x661;

// The methods DTA_Methods lists, ~0 last.
constexpr u32 kMethods[] = {
    OM_NEW, OM_GET, OM_SET, OM_UPDATE, OM_DISPOSE,
    GM_LAYOUT, GM_HITTEST, GM_GOACTIVE, GM_HANDLEINPUT, GM_RENDER,
    DTM_FRAMEBOX, DTM_SELECT, DTM_CLEARSELECTED, DTM_WRITE,
    PDTM_WRITEPIXELARRAY, PDTM_READPIXELARRAY, 0xFFFFFFFF,
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
// A LUT8 bitmap of a graphics card, as AllocBitMap's BMF_SPECIALFMT asks
// for one with PIXFMT_LUT8 in the flags' top byte.
constexpr u32 BMF_SPECIALFMT = 0x80;
constexpr u32 PIXFMT_LUT8 = 0;
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
// From 96 on: the V43 interface's source and destination modes, the friend
// bitmap choice, the mask plane and the chunky source buffer.
constexpr u32 kSourceMode = 96;
constexpr u32 kDestMode = 100;
constexpr u32 kUseFriend = 104;
constexpr u32 kMaskPlane = 108;
constexpr u32 kMaskBytes = 112;
constexpr u32 kPixelData = 116;
constexpr u32 kPixelWidth = 120;
constexpr u32 kPixelHeight = 124;
constexpr u32 kPixelDepth = 128;
constexpr u32 kPixelBytes = 132;
constexpr u32 kInstBytes = 160;

// kFlags.
constexpr u32 kRemap = 1;
constexpr u32 kFreeSource = 2;
constexpr u32 kSparse = 4;
// The destination is the source bitmap itself.
constexpr u32 kAlias = 8;
// The 256 colours of the indexed source are a grey ramp.
constexpr u32 kGrey = 16;
// What the first layout found: some pixel is translucent (an alpha below
// opaque, or the transparent index), and the source was true colour with
// an alpha channel.
constexpr u32 kTranslucent = 32;
constexpr u32 kAlphaSource = 64;

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

// The source the layout, the frame info and the pixel array messages read:
// a planar bitmap a subclass handed over, the chunky buffer a first V43
// write made, or neither.
enum class SourceKind { None, Planar, Indexed, Argb };

struct Source {
    SourceKind kind = SourceKind::None;
    Planar planar;
    u32 bitmap = 0;
    u32 pixels = 0;
    u32 width = 0;
    u32 height = 0;
    u32 depth = 0;
};

// One source pixel as the class uses it: its colour, its alpha and, for an
// indexed source, the index it came from.
struct Pixel {
    Rgb colour;
    u8 alpha = 255;
    u8 index = 0;
};

// What reading pixels takes from the object, once for a whole call: the
// colours of the indexes, black past those the tables hold, and how the
// header makes pixels transparent.
struct Colours {
    Rgb of[kMaxPens];
    u32 masking = 0;
    u32 transparent = 0;
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
    u32 masking() { return b.r8(data + kHeader + bmh_Masking); }
    u32 transparent() { return header16(bmh_Transparent); }

    // The chunky buffer a V43 write made is the source while it exists;
    // otherwise the bitmap a subclass handed over, when it is planar.
    Source load_source() {
        Source source;
        const u32 pixels = get(kPixelData);
        if (pixels != 0) {
            source.pixels = pixels;
            source.width = get(kPixelWidth);
            source.height = get(kPixelHeight);
            source.depth = get(kPixelDepth);
            source.kind = source.depth <= 8 ? SourceKind::Indexed : SourceKind::Argb;
            return source;
        }
        const u32 bitmap = get(kSource);
        if (bitmap != 0 && p.graphics_->deep(bitmap) == nullptr &&
            load_planar(b, bitmap, source.planar)) {
            source.kind = SourceKind::Planar;
            source.bitmap = bitmap;
            source.width = source.planar.bytes_per_row * 8;
            source.height = source.planar.rows;
            source.depth = source.planar.depth;
        }
        return source;
    }

    bool has_source() { return get(kPixelData) != 0 || get(kSource) != 0; }

    // Whether the source is still the one src describes: the same buffer
    // or bitmap, of the same size. Another task may have replaced or freed
    // it while this one let others run.
    bool same_source(const Source& src) {
        const Source now = load_source();
        if (now.kind != src.kind || now.pixels != src.pixels || now.bitmap != src.bitmap ||
            now.width != src.width || now.height != src.height || now.depth != src.depth) {
            return false;
        }
        for (u32 plane = 0; plane < now.planar.depth; ++plane) {
            if (now.planar.planes[plane] != src.planar.planes[plane]) {
                return false;
            }
        }
        return true;
    }

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

    // Gives back the pens of the last remap, the destination it made and
    // the mask that goes with it.
    void release_remap() {
        release_pens();
        const u32 mask = get(kMaskPlane);
        if (mask != 0) {
            p.exec_->free(mask, get(kMaskBytes));
        }
        put(kMaskPlane, 0);
        put(kMaskBytes, 0);
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
        // New tables hold no grey ramp, whatever the old ones did.
        put(kFlags, flags() & ~kGrey);
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

    // The chunky source buffer and what it was allocated with go; a mask
    // belongs to the destination and is freed with it.
    void free_pixel_data() {
        const u32 block = get(kPixelData);
        if (block != 0) {
            p.exec_->free(block, get(kPixelBytes));
        }
        put(kPixelData, 0);
        put(kPixelWidth, 0);
        put(kPixelHeight, 0);
        put(kPixelDepth, 0);
        put(kPixelBytes, 0);
        put(kFlags, flags() & ~kGrey);
    }

    // A first GREY8 write makes the indexed source's colours 256 greys.
    void make_grey_palette() {
        make_colour_tables(kMaxPens);
        b.w16(data + kNumColors, kMaxPens);
        const u32 registers = get(kColours);
        const u32 cregs = get(kCRegs);
        if (registers == 0 || cregs == 0) {
            return;
        }
        for (u32 i = 0; i < kMaxPens; ++i) {
            for (u32 k = 0; k < 3; ++k) {
                b.w8(registers + i * 3 + k, i);
            }
            for (u32 k = 0; k < 12; k += 4) {
                b.w32(cregs + i * 12 + k, i * 0x01010101u);
            }
        }
        const u32 info = frame_info();
        if (info != 0) {
            b.w32(info + fri_ColorMap, cregs);
        }
    }

    // The buffer a pixel array write fills, made from the header's size and
    // depth the first time and again when those changed: a byte a pixel
    // with colour tables up to eight planes, A, R, G, B deeper.
    bool ensure_pixel_buffer(u32 format) {
        const u32 width = header16(bmh_Width);
        const u32 height = header16(bmh_Height);
        const u32 depth = b.r8(data + kHeader + bmh_Depth);
        if (width == 0 || height == 0 || depth == 0) {
            return false;
        }
        const bool indexed = depth <= 8;
        if (indexed && format != PBPAFMT_LUT8 && format != PBPAFMT_GREY8) {
            return false;
        }
        const u32 bytes_per_pixel = indexed ? 1 : 4;
        const u64 bytes = u64{width} * height * bytes_per_pixel;
        if (bytes == 0 || bytes > 0xFFFFFFFFu) {
            return false;
        }
        if (get(kPixelData) != 0 && get(kPixelWidth) == width && get(kPixelHeight) == height &&
            get(kPixelDepth) == depth) {
            return true;
        }
        free_pixel_data();
        const u32 block = p.exec_->alloc(static_cast<u32>(bytes), MEMF_PUBLIC | MEMF_CLEAR);
        if (block == 0) {
            return false;
        }
        put(kPixelData, block);
        put(kPixelWidth, width);
        put(kPixelHeight, height);
        put(kPixelDepth, depth);
        put(kPixelBytes, static_cast<u32>(bytes));
        if (indexed && b.r16(data + kNumColors) == 0) {
            make_colour_tables(kMaxPens);
            b.w16(data + kNumColors, kMaxPens);
            const u32 info = frame_info();
            if (info != 0) {
                b.w32(info + fri_ColorMap, get(kCRegs));
            }
        }
        source_changed();
        return b.ok();
    }

    void load_colours(Colours& out) {
        const u32 count = b.r16(data + kNumColors);
        const u32 colours = get(kColours) == 0 ? 0 : count < kMaxPens ? count : kMaxPens;
        for (u32 i = 0; i < kMaxPens; ++i) {
            out.of[i] = i < colours ? creg(i) : Rgb{};
        }
        out.masking = masking();
        out.transparent = transparent();
    }

    // The colour indexes of count pixels of row y from x0 of a planar or
    // indexed source.
    bool read_index_run(const Source& src, u32 y, u32 x0, u32 count, u8* out) {
        if (src.kind == SourceKind::Planar) {
            return read_indexes(b, src.planar, y, x0, count, out);
        }
        if (src.kind != SourceKind::Indexed) {
            return false;
        }
        const u64 from = u64{src.pixels} + u64{y} * src.width + x0;
        if (from + count > 0x100000000ull) {
            return false;
        }
        const u8* at = b.bytes_at(static_cast<u32>(from), count);
        if (at == nullptr) {
            return false;
        }
        for (u32 i = 0; i < count; ++i) {
            out[i] = at[i];
        }
        return true;
    }

    // count pixels of row y from x of the source, with the alpha the header
    // gives them: the source's own on a true colour picture, none for the
    // transparent index, opaque otherwise.
    bool read_pixels(const Source& src, u32 y, u32 x0, u32 count, const Colours& colours,
                     Pixel* out) {
        if (src.kind == SourceKind::Argb) {
            const u64 from = u64{src.pixels} + (u64{y} * src.width + x0) * 4;
            if (from + u64{count} * 4 > 0x100000000ull) {
                return false;
            }
            const u8* at = b.bytes_at(static_cast<u32>(from), count * 4);
            if (at == nullptr) {
                return false;
            }
            for (u32 i = 0; i < count; ++i) {
                out[i].colour = Rgb{at[i * 4 + 1], at[i * 4 + 2], at[i * 4 + 3]};
                out[i].alpha = at[i * 4];
                out[i].index = 0;
            }
            return true;
        }
        u8 run[kRun];
        if (!read_index_run(src, y, x0, count, run)) {
            return false;
        }
        const bool keyed = colours.masking == mskHasTransparentColor;
        for (u32 i = 0; i < count; ++i) {
            const u32 index = run[i];
            const u8 alpha = keyed && index == colours.transparent ? u8{0} : u8{255};
            out[i] = Pixel{colours.of[index], alpha, run[i]};
        }
        return true;
    }

    // Whether a pixel of a true colour source has an alpha below opaque;
    // false when the source could not be read to the end.
    bool any_translucent(const Source& src, bool& found) {
        found = false;
        Colours colours;
        load_colours(colours);
        Pixel run[kRun];
        for (u32 y = 0; y < src.height; ++y) {
            if (!pause(y, src.width, src)) {
                return false;
            }
            for (u32 x = 0; x < src.width; x += kRun) {
                const u32 count = src.width - x < kRun ? src.width - x : kRun;
                if (!read_pixels(src, y, x, count, colours, run)) {
                    return false;
                }
                for (u32 i = 0; i < count; ++i) {
                    if (run[i].alpha < 255) {
                        found = true;
                        return true;
                    }
                }
            }
        }
        return true;
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

    // The frame's size and depth are the chunky buffer's, or the source
    // bitmap's.
    void source_changed() {
        const u32 info = frame_info();
        if (info == 0 || stopped()) {
            return;
        }
        if (get(kPixelData) != 0) {
            b.w32(info + fri_Width, get(kPixelWidth));
            b.w32(info + fri_Height, get(kPixelHeight));
            b.w32(info + fri_Depth, b.r8(data + kHeader + bmh_Depth));
            return;
        }
        const u32 source = get(kSource);
        if (source == 0) {
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
            case PDTA_SourceMode:
                put(kSourceMode, value);
                if (value != PMODE_V43) {
                    free_pixel_data();
                    source_changed();
                }
                break;
            case PDTA_DestMode:
                put(kDestMode, value);
                break;
            case PDTA_UseFriendBitMap:
                put(kUseFriend, value != 0 ? 1 : 0);
                break;
            // A subclass writing straight into the frame buffer, and one
            // that leaves it dirty, are not taken: the buffer is the
            // class's and starts cleared.
            case PDTA_ObtainPixelBuffer:
            case PDTA_SubClassRendersAll:
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
        put(kSourceMode, PMODE_V42);
        put(kDestMode, PMODE_V42);
        put(kUseFriend, 1);
        put(kMaskPlane, 0);
        put(kMaskBytes, 0);
        put(kPixelData, 0);
        put(kPixelWidth, 0);
        put(kPixelHeight, 0);
        put(kPixelDepth, 0);
        put(kPixelBytes, 0);
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
        free_pixel_data();
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
        case PDTA_SourceMode:
            value = get(kSourceMode);
            break;
        case PDTA_DestMode:
            value = get(kDestMode);
            break;
        case PDTA_UseFriendBitMap:
            value = get(kUseFriend);
            break;
        case PDTA_MaskPlane:
            value = get(kMaskPlane);
            break;
        // While the source is there it answers; after it was freed, what
        // the first layout found does.
        case PDTA_AlphaChannel: {
            const SourceKind kind = load_source().kind;
            value = kind != SourceKind::None
                        ? (kind == SourceKind::Argb && masking() == mskHasAlpha ? 1 : 0)
                        : ((flags() & kAlphaSource) != 0 ? 1 : 0);
            break;
        }
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
        // The levels of the colour cube a true colour source is dithered
        // into on a palette destination.
        u32 levels = 0;
    };

    // Lets other tasks run now and then while a large picture is worked on:
    // every kYieldPixels pixels, counted by whole rows of width. What the
    // layout holds then is the DTSpecialInfo's lock, which drawing only
    // attempts, and the pens it obtained; the source, and the destination
    // dest when one is named, may be gone when the others are done. False
    // when the run ended or the source or the destination is no longer the
    // one it was, which then is not touched again.
    bool pause(u32 y, u32 width, const Source& src, u32 dest = 0) {
        const u32 rows = width >= kYieldPixels ? 1 : kYieldPixels / (width != 0 ? width : 1);
        if (y == 0 || y % rows != 0) {
            return !stopped();
        }
        p.exec_->yield_in_native();
        return !stopped() && same_source(src) && (dest == 0 || get(kDest) == dest);
    }

    // Which colour indexes the pixels of a planar or indexed source use.
    bool used_indexes(const Source& src, u32 width, u32 height, bool (&used)[kMaxPens]) {
        for (bool& u : used) {
            u = false;
        }
        u8 run[kRun];
        for (u32 y = 0; y < height; ++y) {
            if (!pause(y, width, src)) {
                return false;
            }
            for (u32 x = 0; x < width; x += kRun) {
                const u32 count = width - x < kRun ? width - x : kRun;
                if (!read_index_run(src, y, x, count, run)) {
                    return false;
                }
                for (u32 i = 0; i < count; ++i) {
                    used[run[i]] = true;
                }
            }
        }
        return !stopped();
    }

    // Which colour cube indexes a true colour source dithered into levels
    // levels uses.
    bool used_cube(const Source& src, u32 levels, bool (&used)[kMaxPens]) {
        for (bool& u : used) {
            u = false;
        }
        Colours colours;
        load_colours(colours);
        Pixel run[kRun];
        for (u32 y = 0; y < src.height; ++y) {
            if (!pause(y, src.width, src)) {
                return false;
            }
            for (u32 x = 0; x < src.width; x += kRun) {
                const u32 count = src.width - x < kRun ? src.width - x : kRun;
                if (!read_pixels(src, y, x, count, colours, run)) {
                    return false;
                }
                for (u32 i = 0; i < count; ++i) {
                    used[remap::cube_index(run[i].colour.r, run[i].colour.g, run[i].colour.b,
                                           x + i, y, levels)] = true;
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

    // The pen of a true colour pixel on a palette destination: the colour
    // cube index the dither gives it, through the pens chosen for the
    // indexes. The second table is the first's, so no mixing comes on top
    // of the dither.
    u32 cube_pen(const Pixel& pixel, u32 x, u32 y, const Mapping& map) {
        const u32 index = remap::cube_index(pixel.colour.r, pixel.colour.g, pixel.colour.b, x, y,
                                            map.levels);
        return ((x + y) & 1) == 0 ? map.value[index] : map.second[index];
    }

    // The value a true colour pixel takes in a destination of fmt, or with
    // planar set in a planar one: its colour, with its alpha when alpha is
    // set, on ARGB32, dithered on RGB16, the pen of its cube index else.
    u32 true_colour_value(PixFmt fmt, bool planar, const Pixel& pixel, u32 x, u32 y,
                          const Mapping& map, bool alpha) {
        if (planar || fmt == PixFmt::Lut8) {
            return cube_pen(pixel, x, y, map);
        }
        if (fmt == PixFmt::Rgb16) {
            return remap::rgb16(pixel.colour.r, pixel.colour.g, pixel.colour.b, x, y);
        }
        return (alpha ? u32{pixel.alpha} << 24 : 0xFF000000u) | (u32{pixel.colour.r} << 16) |
               (u32{pixel.colour.g} << 8) | pixel.colour.b;
    }

    // The source's pixels through the mapping into the destination: planar
    // pens, LUT8 pens or true colour values, the second table's on every
    // other pixel of a checkerboard; a true colour source is dithered into
    // the destination's colours or colour cube. Each pixel's value is had
    // once and then set in every plane.
    bool convert(const Source& src, u32 dest, u32 width, u32 height, const Mapping& map) {
        const DeepBitmap* deep = p.graphics_->deep(dest);
        Planar dst;
        if (deep == nullptr && !load_planar(b, dest, dst)) {
            return false;
        }
        const PixFmt fmt = deep != nullptr ? deep->fmt : PixFmt::Lut8;
        const u32 bpp = deep != nullptr ? bytes_per_pixel(fmt) : 0;
        const u32 deep_pixels = deep != nullptr ? deep->pixels : 0;
        const u32 deep_row = deep != nullptr ? deep->bytes_per_row : 0;
        const bool true_colour = src.kind == SourceKind::Argb;
        const bool alpha = masking() == mskHasAlpha;
        Colours colours;
        if (true_colour) {
            load_colours(colours);
        }
        Pixel pixels[kRun];
        u8 indexes[kRun];
        u32 values[kRun];
        for (u32 y = 0; y < height; ++y) {
            if (!pause(y, width, src, dest)) {
                return false;
            }
            for (u32 x = 0; x < width; x += kRun) {
                const u32 count = width - x < kRun ? width - x : kRun;
                if (true_colour) {
                    if (!read_pixels(src, y, x, count, colours, pixels)) {
                        return false;
                    }
                    for (u32 i = 0; i < count; ++i) {
                        values[i] = true_colour_value(fmt, deep == nullptr, pixels[i], x + i, y,
                                                      map, alpha);
                    }
                } else {
                    if (!read_index_run(src, y, x, count, indexes)) {
                        return false;
                    }
                    for (u32 i = 0; i < count; ++i) {
                        values[i] = ((x + i + y) & 1) == 0 ? map.value[indexes[i]]
                                                           : map.second[indexes[i]];
                    }
                }
                if (deep != nullptr) {
                    u8* row = b.bytes_at(deep_pixels + y * deep_row + x * bpp, count * bpp);
                    if (row == nullptr) {
                        return false;
                    }
                    for (u32 i = 0; i < count; ++i) {
                        store_pixel(fmt, row + i * bpp, values[i]);
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
                        const u8 bit = static_cast<u8>(0x80 >> (at & 7));
                        u8& cell = row[(at >> 3) - first];
                        cell = static_cast<u8>(((values[i] >> plane) & 1) != 0 ? cell | bit
                                                                               : cell & ~bit);
                    }
                }
            }
        }
        return true;
    }

    // The table of the destination depth's colour cube, its colours in
    // ColorRegisters and CRegs, and the number of colours the frame names.
    bool make_cube(u32 levels) {
        const u32 colours = levels * levels * levels;
        make_colour_tables(colours);
        b.w16(data + kNumColors, colours);
        const u32 registers = get(kColours);
        const u32 cregs = get(kCRegs);
        if (registers == 0 || cregs == 0) {
            return false;
        }
        for (u32 i = 0; i < colours; ++i) {
            u8 red = 0, green = 0, blue = 0;
            remap::cube_colour(i, levels, red, green, blue);
            b.w8(registers + i * 3, red);
            b.w8(registers + i * 3 + 1, green);
            b.w8(registers + i * 3 + 2, blue);
            b.w32(cregs + i * 12, u32{red} * 0x01010101u);
            b.w32(cregs + i * 12 + 4, u32{green} * 0x01010101u);
            b.w32(cregs + i * 12 + 8, u32{blue} * 0x01010101u);
        }
        const u32 info = frame_info();
        if (info != 0) {
            b.w32(info + fri_ColorMap, cregs);
        }
        return b.ok();
    }

    // The mask plane a translucent chunky source is drawn through: a bit
    // set where the alpha is at least half, or where the index is not the
    // transparent one. It has the rows of a planar destination, or of a
    // bitmap as wide as the picture.
    u32 make_mask(const Source& src, u32 dest) {
        const bool true_colour = src.kind == SourceKind::Argb;
        Colours colours;
        load_colours(colours);
        u32 stride = ((src.width + 15) / 16) * 2;
        if (dest != 0 && p.graphics_->deep(dest) == nullptr) {
            const u32 rows = b.r16(dest + bm_BytesPerRow);
            if (rows != 0) {
                stride = rows;
            }
        }
        const u64 bytes = u64{stride} * src.height;
        if (bytes == 0 || bytes > 0xFFFFFFFFu || u64{stride} * 8 < src.width) {
            return ERROR_NO_FREE_STORE;
        }
        const u32 plane = p.exec_->alloc(static_cast<u32>(bytes), MEMF_CHIP | MEMF_CLEAR);
        if (plane == 0) {
            return ERROR_NO_FREE_STORE;
        }
        const auto fail = [&](u32 error) {
            p.exec_->free(plane, static_cast<u32>(bytes));
            return stopped() ? 0 : error;
        };
        Pixel pixels[kRun];
        u8 indexes[kRun];
        for (u32 y = 0; y < src.height; ++y) {
            if (!pause(y, src.width, src, dest)) {
                return fail(DTERROR_INVALID_DATA);
            }
            for (u32 x = 0; x < src.width; x += kRun) {
                const u32 count = src.width - x < kRun ? src.width - x : kRun;
                const bool read = true_colour
                                      ? read_pixels(src, y, x, count, colours, pixels)
                                      : read_index_run(src, y, x, count, indexes);
                u8* row = b.bytes_at(plane + y * stride + (x >> 3), (count + 7) >> 3);
                if (!read || row == nullptr) {
                    return fail(DTERROR_INVALID_DATA);
                }
                for (u32 i = 0; i < count; ++i) {
                    const bool set = true_colour ? pixels[i].alpha >= 128
                                                 : indexes[i] != colours.transparent;
                    if (set) {
                        row[i >> 3] = static_cast<u8>(row[i >> 3] | (0x80u >> (i & 7)));
                    }
                }
            }
        }
        if (!b.ok()) {
            return fail(0);
        }
        put(kMaskPlane, plane);
        put(kMaskBytes, static_cast<u32>(bytes));
        return 0;
    }

    // A bitmap of width by height and depth, for the screen's bitmap as its
    // friend when the object asks for one, of the pixel format flags name
    // when they have BMF_SPECIALFMT.
    u32 alloc_destination(u32 width, u32 height, u32 depth, u32 flags, u32 friend_bitmap) {
        if (width == 0 || height == 0 || depth == 0) {
            return 0;
        }
        LibraryCall args;
        args.d[0] = width;
        args.d[1] = height;
        args.d[2] = depth;
        args.d[3] = BMF_CLEAR | flags;
        args.a[0] = get(kUseFriend) != 0 ? friend_bitmap : 0;
        return gfx(graphics_lvo::AllocBitMap, args);
    }

    // The destination of a chunky source: a bitmap for the screen made the
    // way the planar path makes one, its pixels taken from the buffer. An
    // indexed source keeps its indexes, in a planar bitmap of its depth or
    // a LUT8 one on a deep screen, when it is not remapped or has no screen
    // to be remapped for; a true colour one without a screen keeps its
    // pixels in an ARGB32 bitmap. What is translucent is recorded first.
    u32 make_pixel_destination(const Source& src, u32 screen) {
        const u32 mask = masking();
        bool translucent = false;
        bool used[kMaxPens];
        if (src.kind == SourceKind::Indexed) {
            if (!used_indexes(src, src.width, src.height, used)) {
                return stopped() ? 0 : DTERROR_INVALID_DATA;
            }
            translucent = mask == mskHasTransparentColor && transparent() < kMaxPens &&
                          used[transparent()];
        } else if (mask == mskHasAlpha || mask == mskHasTransparentColor) {
            if (!any_translucent(src, translucent)) {
                return stopped() ? 0 : DTERROR_INVALID_DATA;
            }
        }
        u32 record = flags();
        record = translucent ? record | kTranslucent : record;
        record = src.kind == SourceKind::Argb && mask == mskHasAlpha ? record | kAlphaSource
                                                                     : record;
        put(kFlags, record);
        const bool keep_indexes =
            src.kind == SourceKind::Indexed && (screen == 0 || (flags() & kRemap) == 0);
        const u32 screen_bitmap = screen != 0 ? screen + sc_BitMap : 0;
        const u32 screen_depth =
            screen != 0 ? b.r8(b.r32(screen + sc_RastPort + rp_BitMap) + bm_Depth) : 0;
        DeepBitmap* screen_deep = screen != 0 ? p.graphics_->deep(screen_bitmap) : nullptr;
        u32 dest = 0;
        if (keep_indexes && screen_deep != nullptr) {
            const bool same = screen_deep->fmt == PixFmt::Lut8;
            dest = alloc_destination(src.width, src.height, 8,
                                     same ? 0 : BMF_SPECIALFMT | (PIXFMT_LUT8 << 24),
                                     same ? screen_bitmap : 0);
            // Its pens show in the screen's colours.
            if (DeepBitmap* made = p.graphics_->deep(dest)) {
                made->palette = screen_deep->palette;
                made->colormap = screen_deep->colormap;
            }
        } else if (keep_indexes) {
            dest = alloc_destination(src.width, src.height, src.depth, 0, screen_bitmap);
        } else if (screen == 0) {
            dest = alloc_destination(src.width, src.height, 32, 0, 0);
        } else {
            u32 depth = src.depth;
            if (src.kind == SourceKind::Argb) {
                depth = screen_deep != nullptr
                            ? (screen_deep->fmt == PixFmt::Argb32
                                   ? 24
                                   : (screen_deep->fmt == PixFmt::Rgb16 ? 16 : 8))
                            : screen_depth;
            } else {
                depth = screen_depth > src.depth ? screen_depth : src.depth;
                if (screen_deep == nullptr && depth > 8) {
                    depth = 8;
                }
            }
            dest = alloc_destination(src.width, src.height, depth, 0, screen_bitmap);
        }
        if (dest == 0) {
            return ERROR_NO_FREE_STORE;
        }
        put(kDest, dest);
        const DeepBitmap* deep = p.graphics_->deep(dest);
        Mapping map;
        if (keep_indexes) {
            for (u32 i = 0; i < kMaxPens; ++i) {
                map.value[i] = i;
                map.second[i] = i;
            }
        } else if (src.kind == SourceKind::Indexed) {
            if (deep != nullptr && deep->fmt != PixFmt::Lut8) {
                true_colours(deep->fmt, map);
            } else if (!choose_pens(screen, used, map)) {
                return ERROR_NO_FREE_STORE;
            }
        } else if (screen != 0 && (deep == nullptr || deep->fmt == PixFmt::Lut8)) {
            const u32 levels = remap::levels(deep != nullptr ? 8 : screen_depth);
            if (!make_cube(levels)) {
                return ERROR_NO_FREE_STORE;
            }
            if (!used_cube(src, levels, used)) {
                return stopped() ? 0 : DTERROR_INVALID_DATA;
            }
            if (!choose_pens(screen, used, map)) {
                return ERROR_NO_FREE_STORE;
            }
            for (u32 i = 0; i < kMaxPens; ++i) {
                map.second[i] = map.value[i];
            }
            const u32 first = get(kColorTable);
            const u32 second = get(kColorTable2);
            for (u32 i = 0; first != 0 && second != 0 && i < levels * levels * levels; ++i) {
                b.w8(first + i, map.value[i]);
                b.w8(second + i, map.value[i]);
            }
            map.levels = levels;
        }
        if (!convert(src, dest, src.width, src.height, map)) {
            return stopped() ? 0 : DTERROR_INVALID_DATA;
        }
        return translucent ? make_mask(src, dest) : 0;
    }

    // The destination of a first layout: the source itself, or a bitmap
    // made for the screen the source is remapped or copied into. 0, or the
    // error that left the picture without a remapped destination.
    u32 make_destination(u32 screen) {
        put(kFlags, flags() & ~(kTranslucent | kAlphaSource));
        const Source pixels = load_source();
        if (pixels.kind == SourceKind::Indexed || pixels.kind == SourceKind::Argb) {
            return make_pixel_destination(pixels, screen);
        }
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
        const bool planar_source = pixels.kind == SourceKind::Planar;
        if ((!remap && !deeper) || !planar_source) {
            put(kFlags, flags() | kAlias);
            return 0;
        }
        const Planar& src = pixels.planar;
        const u32 width = header16(bmh_Width);
        const u32 height = header16(bmh_Height);
        u32 depth = screen_depth > depth8 ? screen_depth : depth8;
        if (p.graphics_->deep(screen_bitmap) == nullptr && depth > 8) {
            depth = 8;
        }
        const u32 dest = alloc_destination(width, height, depth, 0, screen_bitmap);
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
        if (!used_indexes(pixels, w, h, used)) {
            return stopped() ? 0 : DTERROR_INVALID_DATA;
        }
        Mapping map;
        const DeepBitmap* deep = p.graphics_->deep(dest);
        if (deep != nullptr && deep->fmt != PixFmt::Lut8) {
            true_colours(deep->fmt, map);
        } else if (!choose_pens(screen, used, map)) {
            return ERROR_NO_FREE_STORE;
        }
        if (!convert(pixels, dest, w, h, map)) {
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
        if (initial != 0 && has_source()) {
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
            if ((flags() & kFreeSource) != 0) {
                if (get(kOwnedSource) != 0) {
                    free_bitmap(get(kOwnedSource));
                    put(kOwnedSource, 0);
                    put(kSource, 0);
                }
                free_pixel_data();
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
        // A translucent picture with its alpha in an ARGB32 destination is
        // blended onto a true colour RastPort; onto others, and without an
        // alpha, it goes through its mask plane. Either is drawn over a
        // cleared area, so that what its transparency leaves out does not
        // show the last picture there. An opaque picture is copied.
        const u32 mask = get(kMaskPlane);
        const DeepBitmap* deep = p.graphics_->deep(dest);
        const DeepBitmap* target = p.graphics_->deep(b.r32(rp + rp_BitMap));
        const bool alpha = (flags() & (kTranslucent | kAlphaSource)) ==
                               (kTranslucent | kAlphaSource) &&
                           deep != nullptr && deep->fmt == PixFmt::Argb32 && target != nullptr &&
                           target->fmt != PixFmt::Lut8;
        if (mask != 0 || alpha) {
            LibraryCall erase;
            erase.a[1] = rp;
            erase.d[0] = static_cast<u32>(left);
            erase.d[1] = static_cast<u32>(top);
            erase.d[2] = static_cast<u32>(left + w - 1);
            erase.d[3] = static_cast<u32>(top + h - 1);
            gfx(graphics_lvo::EraseRect, erase);
        }
        if (alpha) {
            p.graphics_->blt_alpha_rastport(dest, sx, sy, rp, left, top, w, h);
        } else if (mask != 0) {
            LibraryCall blit;
            blit.a[0] = dest;
            blit.d[0] = static_cast<u32>(sx);
            blit.d[1] = static_cast<u32>(sy);
            blit.a[1] = rp;
            blit.d[2] = static_cast<u32>(left);
            blit.d[3] = static_cast<u32>(top);
            blit.d[4] = static_cast<u32>(w);
            blit.d[5] = static_cast<u32>(h);
            blit.d[6] = 0xE0;
            blit.a[2] = mask;
            gfx(graphics_lvo::BltMaskBitMapRastPort, blit);
        } else {
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
        }
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
        const Source src = load_source();
        if (fh == 0) {
            why = "no file";
            return 0;
        }
        if (src.kind == SourceKind::Argb) {
            why = "true colour cannot be saved yet";
            return 0;
        }
        if (src.kind != SourceKind::Planar && src.kind != SourceKind::Indexed) {
            why = "no planar source bitmap";
            return 0;
        }
        ilbm::Picture pic;
        ilbm::Header& h = pic.header;
        const u32 width = header16(bmh_Width);
        const u32 height = header16(bmh_Height);
        u32 x0 = 0;
        u32 y0 = 0;
        u32 w = width;
        u32 hgt = height;
        if (src.kind == SourceKind::Planar) {
            const u32 across = src.planar.bytes_per_row * 8;
            w = width < across ? width : across;
            hgt = height < src.planar.rows ? height : src.planar.rows;
        } else {
            w = width < src.width ? width : src.width;
            hgt = height < src.height ? height : src.height;
        }
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
            const u8* line =
                src.kind == SourceKind::Planar
                    ? b.bytes_at(src.planar.planes[plane] + (y0 + y) * src.planar.bytes_per_row,
                                 src.planar.bytes_per_row)
                    : b.bytes_at(src.pixels + (y0 + y) * src.width, src.width);
            if (line == nullptr) {
                return nullptr;
            }
            for (u32 i = 0; i < row_bytes; ++i) {
                row[i] = 0;
            }
            for (u32 x = 0; x < w; ++x) {
                const u32 at = x0 + x;
                const u32 bit = src.kind == SourceKind::Planar
                                    ? (line[at >> 3] >> (7 - (at & 7))) & 1
                                    : (line[at] >> plane) & 1;
                if (bit != 0) {
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

    // PDTM_WRITEPIXELARRAY: the caller's pixels into the chunky source
    // buffer, which the first write makes from the header's size and
    // depth. 1 when all of them were stored.
    u32 write_pixel_array(u32 msg) {
        if (get(kSourceMode) != PMODE_V43) {
            return 0;
        }
        const u32 pixels = b.r32(msg + 4);
        const u32 format = b.r32(msg + 8);
        const u32 mod = b.r32(msg + 12);
        const u32 left = b.r32(msg + 16);
        const u32 top = b.r32(msg + 20);
        const u32 width = b.r32(msg + 24);
        const u32 height = b.r32(msg + 28);
        if (pixels == 0 || width == 0 || height == 0 || format > PBPAFMT_GREY8) {
            return 0;
        }
        if (!ensure_pixel_buffer(format)) {
            return 0;
        }
        const u32 input_bytes = format == PBPAFMT_RGB ? 3 : format <= PBPAFMT_ARGB ? 4 : 1;
        const u64 row_bytes = u64{width} * input_bytes;
        if ((height > 1 && mod < row_bytes) || u64{left} + width > get(kPixelWidth) ||
            u64{top} + height > get(kPixelHeight)) {
            return 0;
        }
        const bool indexed = get(kPixelDepth) <= 8;
        if (indexed && format == PBPAFMT_GREY8 && (flags() & kGrey) == 0) {
            make_grey_palette();
            put(kFlags, flags() | kGrey);
        }
        // The buffer, which the loop checks is still there each time other
        // tasks had a turn.
        const Source target = load_source();
        const u32 buffer = get(kPixelData);
        const u32 stored_bytes = indexed ? 1 : 4;
        const u32 row_stored = get(kPixelWidth) * stored_bytes;
        Colours colours;
        load_colours(colours);
        const bool keyed = colours.masking == mskHasTransparentColor;
        for (u32 y = 0; y < height; ++y) {
            if (!pause(y, width, target)) {
                return 0;
            }
            const u64 from = u64{pixels} + u64{y} * mod;
            if (from + row_bytes > 0x100000000ull) {
                return 0;
            }
            const u64 to = u64{buffer} + u64{top + y} * row_stored + u64{left} * stored_bytes;
            if (to + u64{width} * stored_bytes > 0x100000000ull) {
                return 0;
            }
            const u8* in = b.bytes_at(static_cast<u32>(from), static_cast<u32>(row_bytes));
            u8* out = b.bytes_at(static_cast<u32>(to), width * stored_bytes);
            if (in == nullptr || out == nullptr) {
                return 0;
            }
            if (indexed) {
                for (u32 i = 0; i < width; ++i) {
                    out[i] = in[i];
                }
                continue;
            }
            for (u32 i = 0; i < width; ++i) {
                u32 red = 0, green = 0, blue = 0, alpha = 255;
                switch (format) {
                case PBPAFMT_RGB:
                    red = in[i * 3];
                    green = in[i * 3 + 1];
                    blue = in[i * 3 + 2];
                    break;
                case PBPAFMT_RGBA:
                    red = in[i * 4];
                    green = in[i * 4 + 1];
                    blue = in[i * 4 + 2];
                    alpha = in[i * 4 + 3];
                    break;
                case PBPAFMT_ARGB:
                    alpha = in[i * 4];
                    red = in[i * 4 + 1];
                    green = in[i * 4 + 2];
                    blue = in[i * 4 + 3];
                    break;
                case PBPAFMT_LUT8: {
                    const u32 index = in[i];
                    red = colours.of[index].r;
                    green = colours.of[index].g;
                    blue = colours.of[index].b;
                    alpha = keyed && index == colours.transparent ? 0 : 255;
                    break;
                }
                case PBPAFMT_GREY8:
                    red = green = blue = in[i];
                    break;
                }
                out[i * 4] = static_cast<u8>(alpha);
                out[i * 4 + 1] = static_cast<u8>(red);
                out[i * 4 + 2] = static_cast<u8>(green);
                out[i * 4 + 3] = static_cast<u8>(blue);
            }
        }
        return b.ok() ? 1 : 0;
    }

    // PDTM_READPIXELARRAY: the source's pixels, planar or chunky, in the
    // caller's format. LUT8 needs an indexed source to have an index to
    // give. 1 when all of them were read.
    u32 read_pixel_array(u32 msg) {
        const Source src = load_source();
        if (src.kind == SourceKind::None) {
            return 0;
        }
        const u32 pixels = b.r32(msg + 4);
        const u32 format = b.r32(msg + 8);
        const u32 mod = b.r32(msg + 12);
        const u32 left = b.r32(msg + 16);
        const u32 top = b.r32(msg + 20);
        const u32 width = b.r32(msg + 24);
        const u32 height = b.r32(msg + 28);
        if (pixels == 0 || width == 0 || height == 0 || format > PBPAFMT_GREY8 ||
            (format == PBPAFMT_LUT8 && src.kind == SourceKind::Argb)) {
            return 0;
        }
        u32 source_width = src.width;
        u32 source_height = src.height;
        if (src.kind == SourceKind::Planar) {
            const u32 across = header16(bmh_Width);
            const u32 down = header16(bmh_Height);
            source_width = across < source_width ? across : source_width;
            source_height = down < source_height ? down : source_height;
        }
        const u32 output_bytes = format == PBPAFMT_RGB ? 3 : format <= PBPAFMT_ARGB ? 4 : 1;
        const u64 row_bytes = u64{width} * output_bytes;
        if ((height > 1 && mod < row_bytes) || u64{left} + width > source_width ||
            u64{top} + height > source_height) {
            return 0;
        }
        Colours colours;
        load_colours(colours);
        const u32 mask = colours.masking;
        const u32 see_through = colours.transparent;
        Pixel run[kRun];
        for (u32 y = 0; y < height; ++y) {
            if (!pause(y, width, src)) {
                return 0;
            }
            for (u32 x = 0; x < width; x += kRun) {
                const u32 count = width - x < kRun ? width - x : kRun;
                if (!read_pixels(src, top + y, left + x, count, colours, run)) {
                    return 0;
                }
                const u64 to = u64{pixels} + u64{y} * mod + u64{x} * output_bytes;
                if (to + u64{count} * output_bytes > 0x100000000ull) {
                    return 0;
                }
                u8* out = b.bytes_at(static_cast<u32>(to), count * output_bytes);
                if (out == nullptr) {
                    return 0;
                }
                for (u32 i = 0; i < count; ++i) {
                    const u32 alpha = src.kind == SourceKind::Argb
                                          ? (mask == mskHasAlpha ? run[i].alpha : 255)
                                          : (mask == mskHasTransparentColor &&
                                                     run[i].index == see_through
                                                 ? 0
                                                 : 255);
                    u8* at = out + i * output_bytes;
                    switch (format) {
                    case PBPAFMT_RGB:
                        at[0] = run[i].colour.r;
                        at[1] = run[i].colour.g;
                        at[2] = run[i].colour.b;
                        break;
                    case PBPAFMT_RGBA:
                        at[0] = run[i].colour.r;
                        at[1] = run[i].colour.g;
                        at[2] = run[i].colour.b;
                        at[3] = static_cast<u8>(alpha);
                        break;
                    case PBPAFMT_ARGB:
                        at[0] = static_cast<u8>(alpha);
                        at[1] = run[i].colour.r;
                        at[2] = run[i].colour.g;
                        at[3] = run[i].colour.b;
                        break;
                    case PBPAFMT_GREY8:
                        at[0] = remap::luma(run[i].colour.r, run[i].colour.g,
                                            run[i].colour.b);
                        break;
                    case PBPAFMT_LUT8:
                        at[0] = run[i].index;
                        break;
                    }
                }
            }
        }
        return b.ok() ? 1 : 0;
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
    case PDTM_WRITEPIXELARRAY:
        return ops.write_pixel_array(msg);
    case PDTM_READPIXELARRAY:
        return ops.read_pixel_array(msg);
    default:
        return self.intuition_->boopsi().pass_on(cl, object, msg);
    }
}

}
