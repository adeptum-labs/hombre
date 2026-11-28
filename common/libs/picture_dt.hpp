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

#include "../m68k_cpu.hpp"
#include "../types.hpp"
#include "exec.hpp"
#include "native_class_lib.hpp"

// picture.datatype, the superclass of the picture datatypes (ilbm.datatype
// and the like), written in C++ over datatypes.library's datatypesclass.
// exec offers it as datatypes/picture.datatype at Hombre's version; a disk
// copy replaces it only when its version is higher.
//
// A subclass loads the picture in its OM_NEW: it writes the BitMapHeader
// whose address PDTA_BitMapHeader gives, hands over a planar bitmap of up to
// eight planes with PDTA_BitMap (the class owns it from then on) or lends
// one with PDTA_ClassBitMap, and sets PDTA_NumColors, which makes the colour
// tables PDTA_ColorRegisters and PDTA_CRegs point at, for it to fill.
//
// The first layout of an object (GM_LAYOUT or DTM_PROCLAYOUT with
// gpl_Initial set) remaps it for its screen, the one PDTA_Screen names or
// the GadgetInfo's, in datatypes.library's layout process: one pen
// ObtainBestPenA gives (with the OBP_Precision of OM_NEW) for every colour
// the picture uses, or only pens of PDTA_SparseTable, and a second table
// whose pens, mixed in a checkerboard with the first's, come nearer a colour
// the first matches badly. The destination is a bitmap of the screen's
// depth, at least the picture's, a friend of the screen's bitmap.
//
// On a deep screen (a graphics card mode, more than eight planes or a
// chunky one) the destination is a deep bitmap in the screen's pixel
// format: on RGB16 and ARGB32 it holds every pixel's own colour from
// PDTA_CRegs and no pens are obtained; on LUT8 it holds the pens, as a
// planar one does. A picture saved from there is the same ILBM: DTM_WRITE
// always writes the source bitmap.
//
// GM_RENDER blits the destination into the gadget's domain from the
// scroll offsets on and marks a selection with a stipple of pen 2.
// DTM_WRITE writes the picture, or its selection with its corner in the
// header, as an ILBM with BMHD, CAMG, CMAP, NAME and a ByteRun1 BODY; a
// side of more than 16383 pixels cannot be written. Methods the class
// leaves to datatypesclass go on to it without a call of their own.
//
// Only the V42 interface is there: PDTA_SourceMode and PDTA_DestMode are
// neither taken nor answered, so a subclass that asks for them learns that
// pixel arrays are not offered, whatever the version says. DTM_COPY and
// DTM_PRINT are not offered either.

namespace hombre {

class Graphics;
class Intuition;

class PictureDataType {
public:
    // The library's file in LIBS:, the leaf a datatypes.library open names.
    static constexpr const char* kLeaf = "picture.datatype";
    static constexpr const char* kDir = "datatypes/";

    // Registers the library with exec's providers. False if exec has no
    // provider left.
    bool init(m68k::Machine& m, Exec& exec, Graphics& graphics, Intuition& intuition);

    // The library base, 0 until exec has made it.
    u32 base() const { return library_.base(); }
    // The class, 0 until the library's first open made it.
    u32 class_ptr() const { return library_.class_ptr(); }

private:
    struct Ops;

    static u32 make_library(void* ctx);
    static u32 make_class(void* ctx, u32 base);
    static u32 dispatch(void* ctx, u32 cl, u32 object, u32 msg);

    m68k::Machine* machine_ = nullptr;
    Exec* exec_ = nullptr;
    Graphics* graphics_ = nullptr;
    Intuition* intuition_ = nullptr;
    NativeClassLibrary library_{};
    // The libraries the class calls, opened when it is made; dos.library
    // when an object is first written.
    u32 datatypes_ = 0;
    u32 dos_ = 0;
    // Guest memory the class keeps: the DTA_Methods list and the stipple
    // of a selection.
    u32 methods_ = 0;
    u32 stipple_ = 0;
};

}
