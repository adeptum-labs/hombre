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

namespace hombre::image {

enum class Format : u32 { Unknown, Png, Jpeg };

// decode tells what each status leaves in the pixel buffer.
enum class Status : u32 {
    Ok,
    Partial,
    NotThisFormat,
    Truncated,
    Corrupt,
    Unsupported,
    TooLarge,
    BufferTooSmall,
    NoMemory,
    Stopped,
};

struct Info {
    Format format = Format::Unknown;
    u32 width = 0;
    u32 height = 0;
    // Bits per pixel of the file: a PNG's bit depth times its channels (24
    // for RGB, 4 for a 16-colour palette), 8 for every JPEG component.
    u32 depth = 0;
    // The picture may carry alpha: it has an alpha channel or a tRNS chunk.
    // Its pixels can still all be opaque.
    bool alpha = false;
};

// The decoder keeps its function pointers in host memory, so the allocator a
// probe or decode is given decides where those objects live.
struct Allocator {
    void* ctx;
    void* (*alloc)(void* ctx, usize bytes);
    void (*release)(void* ctx, void* block, usize bytes);
};

// Called before each decode step; false ends the decode with
// Status::Stopped.
struct Progress {
    void* ctx;
    bool (*keep_going)(void* ctx);
};

// Reads the picture header and reports its format, size, depth and
// transparency. Either side may be at most 16383 pixels and the picture at
// most 64 Mi pixels; a larger one is TooLarge.
Status probe(const u8* file, usize bytes, const Allocator& host, Info& out);

// Writes width * height pixels as R, G, B, A bytes with no row padding. Only
// the first frame is decoded: of an animated PNG, its IDAT picture. A buffer
// that is null or smaller than width * height * 4 bytes is BufferTooSmall.
//
// The work is done in steps, with a progress call before each one: reading
// the header and decoding the file 64 KiB at a time, and clearing the
// pixels 1 MiB at a time. A progressive JPEG is decoded from the whole file
// in one step, since its decoder converts the whole picture again each time
// it returns after a finished scan. Steps follow the file's bytes, not the
// picture's pixels, so their length varies: the step that ends a PNG or a
// baseline JPEG converts the whole picture, and a file that compresses well
// packs many pixels into 64 KiB. Under KVM the longest step of a 1920 x 1080
// picture takes about 15-20 ms for a PNG, 25-30 ms for a baseline JPEG and
// 100-130 ms for a progressive one, while a 16383 x 4096 one-bit PNG of
// 8 KiB is a single step of about a second. A caller that must keep its
// steps short refuses a large width * height after probe.
//
// What the pixels hold afterwards:
// - Ok: the whole picture. A JPEG whose only scan has no end of image
//   marker after it is Ok when the decoder can tell the scan's data was all
//   in the file.
// - Partial: some of the picture, and the rest clear (all four bytes zero).
//   The file ended early, or a JPEG had more than the 32 scans the decoder
//   takes. A cut PNG keeps the rows it holds whole, and an interlaced one
//   its finished passes too; a cut baseline JPEG keeps its rows down to
//   near the cut; a progressive JPEG keeps its whole area, in the detail of
//   the scans it holds.
// - NotThisFormat, BufferTooSmall, TooLarge, NoMemory, and whatever the
//   header shows to be wrong: the buffer is untouched.
// - Stopped: no picture. The buffer may be partly cleared and hold some
//   decoded pixels; the rest is what the caller left in it.
// - Truncated, Corrupt and Unsupported found once decoding is under way: no
//   picture to show. The buffer is cleared and may hold rows of the picture,
//   of grey or of wrongly decoded pixels.
Status decode(const u8* file, usize bytes, const Allocator& host, const Allocator& work,
              u8* pixels, usize pixel_bytes, const Progress& progress);

}
