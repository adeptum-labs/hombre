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

#include "../font.hpp"
#include "raster.hpp"
#include "text_blend.hpp"

// Text drawn from the tables of a struct TextFont (graphics/text.h).
//
// tf_CharData is one bit plane holding every glyph side by side, tf_Modulo
// bytes a row and tf_YSize rows. tf_CharLoc gives each glyph's bit offset
// and bit width in a row. Before a glyph is drawn the pen moves by its
// tf_CharKern word, afterwards by its tf_CharSpace word, or by tf_XSize in
// a font without that table. The glyph after tf_HiChar is drawn for every
// character the font has none for.

namespace hombre::raster {

// The tallest font and the widest bold smear a draw works with.
constexpr u32 kMaxFontRows = 1024;
constexpr i32 kMaxBoldSmear = 8;

// A font's tables, each of them readable from end to end.
struct Font {
    const u8* data = nullptr;
    const u8* loc = nullptr;
    const u8* space = nullptr;
    const u8* kern = nullptr;
    u32 modulo = 0;
    // The glyphs' 4-bit coverage beside the 1-bit plane, a nibble a pixel
    // in the same cells, or none.
    const u8* coverage = nullptr;
    u32 coverage_modulo = 0;
    u32 lo_char = 0;
    // Every character from lo_char up, and the one drawn for all others.
    u32 glyphs = 0;
    i32 y_size = 0;
    i32 x_size = 0;
    i32 baseline = 0;
    i32 bold_smear = 0;
};

// What a RastPort adds to its font: rp_TxSpacing and rp_AlgoStyle.
struct TextStyle {
    i32 spacing = 0;
    u32 algo = 0;
};

// How far text moves the pen, and its leftmost and rightmost pixel column
// counted from where the pen began.
struct TextSpan {
    i64 advance = 0;
    i64 min_x = 0;
    i64 max_x = -1;
};

// The built-in 8x8 font as a TextFont's tables: a glyph for every
// character from space to 255 and the question mark for all others, which
// is also what glyph() gives the characters it has no bitmap for.
constexpr u32 kBuiltinLoChar = 0x20;
constexpr u32 kBuiltinHiChar = 0xFF;
constexpr u32 kBuiltinGlyphs = kBuiltinHiChar - kBuiltinLoChar + 2;
constexpr u32 kBuiltinSize = 8;
constexpr u32 kBuiltinBaseline = 6;
constexpr u32 kBuiltinModulo = (kBuiltinGlyphs + 1) & ~1u;
constexpr u32 kBuiltinDataBytes = kBuiltinModulo * kBuiltinSize;

// Fills tf_CharLoc, four bytes a glyph, for a font whose glyphs are cells
// of width bits side by side in its data.
constexpr void build_cell_loc(u8* loc, u32 glyphs, u32 width) {
    for (u32 i = 0; i < glyphs; ++i) {
        loc[i * 4] = static_cast<u8>((i * width) >> 8);
        loc[i * 4 + 1] = static_cast<u8>(i * width);
        loc[i * 4 + 2] = static_cast<u8>(width >> 8);
        loc[i * 4 + 3] = static_cast<u8>(width);
    }
}

struct BuiltinTables {
    u8 data[kBuiltinDataBytes];
    u8 loc[kBuiltinGlyphs * 4];
};

constexpr BuiltinTables builtin_tables() {
    BuiltinTables tables{};
    for (u32 i = 0; i < kBuiltinGlyphs; ++i) {
        const u32 code = kBuiltinLoChar + i;
        const u8* rows = glyph(code <= kBuiltinHiChar ? static_cast<char>(code) : '?');
        for (u32 row = 0; row < kBuiltinSize; ++row) {
            u8 bits = 0;
            for (u32 col = 0; col < 8; ++col) {
                bits |= ((rows[row] >> col) & 1) != 0 ? static_cast<u8>(0x80u >> col) : 0;
            }
            tables.data[row * kBuiltinModulo + i] = bits;
        }
    }
    build_cell_loc(tables.loc, kBuiltinGlyphs, kBuiltinSize);
    return tables;
}

inline constexpr BuiltinTables kBuiltinTables = builtin_tables();

// The built-in font, drawn from the tables above.
inline Font builtin_font() {
    Font font;
    font.data = kBuiltinTables.data;
    font.loc = kBuiltinTables.loc;
    font.modulo = kBuiltinModulo;
    font.lo_char = kBuiltinLoChar;
    font.glyphs = kBuiltinGlyphs;
    font.y_size = kBuiltinSize;
    font.x_size = kBuiltinSize;
    font.baseline = kBuiltinBaseline;
    font.bold_smear = 1;
    return font;
}

// Reads the TextFont at font. False for one without glyph tables or with
// a shape no font has; a table outside guest memory faults as well.
inline bool load_font(native::Bus& bus, u32 font, Font& out) {
    if (font == 0) {
        return false;
    }
    const u32 y_size = bus.r16(font + ndk::tf_YSize);
    const u32 lo = bus.r8(font + ndk::tf_LoChar);
    const u32 hi = bus.r8(font + ndk::tf_HiChar);
    const u32 data = bus.r32(font + ndk::tf_CharData);
    const u32 modulo = bus.r16(font + ndk::tf_Modulo);
    const u32 loc = bus.r32(font + ndk::tf_CharLoc);
    const u32 space = bus.r32(font + ndk::tf_CharSpace);
    const u32 kern = bus.r32(font + ndk::tf_CharKern);
    const i32 smear = static_cast<i16>(bus.r16(font + ndk::tf_BoldSmear));
    out.x_size = static_cast<i16>(bus.r16(font + ndk::tf_XSize));
    out.baseline = static_cast<i16>(bus.r16(font + ndk::tf_Baseline));
    if (!bus.ok() || data == 0 || loc == 0 || modulo == 0 || hi < lo || y_size == 0 ||
        y_size > kMaxFontRows) {
        return false;
    }
    out.lo_char = lo;
    out.glyphs = hi - lo + 2;
    out.modulo = modulo;
    out.y_size = static_cast<i32>(y_size);
    out.bold_smear = smear < 0 ? 0 : smear > kMaxBoldSmear ? kMaxBoldSmear : smear;
    out.data = bus.bytes_at(data, modulo * y_size);
    out.loc = bus.bytes_at(loc, out.glyphs * 4);
    out.space = space != 0 ? bus.bytes_at(space, out.glyphs * 2) : nullptr;
    out.kern = kern != 0 ? bus.bytes_at(kern, out.glyphs * 2) : nullptr;
    return bus.ok();
}

namespace {

u32 be16(const u8* p) {
    return (static_cast<u32>(p[0]) << 8) | p[1];
}

i32 signed16(const u8* p) {
    return static_cast<i16>(be16(p));
}

// Where a character's bits are in a row of the font's data, and how the
// pen moves around them.
struct Glyph {
    u32 offset;
    u32 width;
    i32 kern;
    i32 advance;
};

Glyph glyph_of(const Font& f, i32 spacing, u8 c) {
    const u32 i = c >= f.lo_char && c - f.lo_char + 1 < f.glyphs ? c - f.lo_char : f.glyphs - 1;
    Glyph g{be16(f.loc + i * 4), be16(f.loc + i * 4 + 2), 0, 0};
    if (g.offset + g.width > f.modulo * 8) {
        g.width = 0;
    }
    g.kern = f.kern != nullptr ? signed16(f.kern + i * 2) : 0;
    g.advance = g.kern + (f.space != nullptr ? signed16(f.space + i * 2) : f.x_size) + spacing;
    return g;
}

// How far the italic style moves a row of the font to the right: a pixel
// for every two rows above the baseline, to the left below it.
i32 italic_shift(const Font& f, const TextStyle& style, i32 row) {
    return (style.algo & ndk::FSF_ITALIC) != 0 ? (f.baseline - row) / 2 : 0;
}

i32 bold_smear(const Font& f, const TextStyle& style) {
    return (style.algo & ndk::FSF_BOLD) != 0 ? f.bold_smear : 0;
}

// The row the underline is drawn in, the one below the baseline.
i32 underline_row(const Font& f) {
    return f.baseline + 1 < f.y_size ? f.baseline + 1 : f.y_size - 1;
}

// ORs width bits of src, from its bit offset on, into the columns of line
// from col on. Columns outside the line are dropped.
void or_bits(u8* line, u32 line_bytes, i64 col, const u8* src, u32 src_bytes, u32 offset,
             u32 width) {
    const i64 columns = static_cast<i64>(line_bytes) * 8;
    for (u32 done = 0; done < width; done += 8, col += 8) {
        if (col <= -8 || col >= columns) {
            continue;
        }
        const u32 bit = offset + done;
        const u32 count = width - done < 8 ? width - done : 8;
        const u32 at = bit >> 3;
        const u32 pair = (static_cast<u32>(src[at]) << 8) | (at + 1 < src_bytes ? src[at + 1] : 0);
        const u32 bits = ((pair << (bit & 7)) >> 8) & (0xFF00u >> count) & 0xFF;
        const u32 spread = (bits << 8) >> (col & 7);
        const i64 byte = col >> 3;
        if (byte >= 0) {
            line[byte] |= static_cast<u8>(spread >> 8);
        }
        if (byte + 1 < static_cast<i64>(line_bytes)) {
            line[byte + 1] |= static_cast<u8>(spread);
        }
    }
}

void set_columns(u8* line, u32 line_bytes, i64 from, i64 to) {
    const i64 last = static_cast<i64>(line_bytes) * 8 - 1;
    from = from < 0 ? 0 : from;
    to = to > last ? last : to;
    if (from > to) {
        return;
    }
    const u32 first = static_cast<u32>(from >> 3);
    fill_span(line + first, static_cast<u32>(to >> 3) - first + 1, left_mask(from),
              right_mask(to), 0xFF);
}

// ORs the line into itself, moved smear columns to the right.
void smear_right(u8* line, u32 line_bytes, i32 smear) {
    for (u32 i = line_bytes; i-- > 0;) {
        const u32 pair = (i > 0 ? static_cast<u32>(line[i - 1]) << 8 : 0) | line[i];
        line[i] |= static_cast<u8>(pair >> smear);
    }
}

// Puts width pixels of coverage from src into line at col, the greater of
// the two kept. Pixels outside the line are dropped.
void or_coverage(u8* line, u32 line_pixels, i64 col, const u8* src, u32 offset, u32 width) {
    for (u32 i = 0; i < width; ++i, ++col) {
        if (col < 0 || col >= static_cast<i64>(line_pixels)) {
            continue;
        }
        const u32 pixel = offset + i;
        const u8 bits = src[pixel >> 1];
        const u8 value = (pixel & 1) == 0 ? static_cast<u8>(bits >> 4)
                                          : static_cast<u8>(bits & 0xF);
        line[col] = value > line[col] ? value : line[col];
    }
}

void set_coverage(u8* line, u32 line_pixels, i64 from, i64 to, u8 value) {
    from = from < 0 ? 0 : from;
    to = to >= static_cast<i64>(line_pixels) ? static_cast<i64>(line_pixels) - 1 : to;
    for (i64 i = from; i <= to; ++i) {
        line[i] = value;
    }
}

// Moves coverage pixels smear to the right, the greater of the two kept.
void smear_coverage(u8* line, u32 line_pixels, u32 smear) {
    for (u32 i = line_pixels; i-- > smear;) {
        line[i] = line[i - smear] > line[i] ? line[i - smear] : line[i];
    }
}

// The first and last set bit of the bytes, as columns. False if none is
// set.
bool inked_columns(const u8* bits, u32 bytes, u32& first, u32& last) {
    u32 lo = 0;
    while (lo < bytes && bits[lo] == 0) {
        ++lo;
    }
    if (lo == bytes) {
        return false;
    }
    u32 hi = bytes - 1;
    while (bits[hi] == 0) {
        --hi;
    }
    first = lo * 8 + static_cast<u32>(__builtin_clz(bits[lo])) - 24;
    last = hi * 8 + 7 - static_cast<u32>(__builtin_ctz(bits[hi]));
    return true;
}

Rect draw_text_part(native::Bus& bus, const Bitmap& bitmap, const Clip& clip, const Pens& pens,
                    const Font& f, const TextStyle& style, const TextSpan& span, i32 x,
                    i32 baseline_y, const u8* chars, u32 count,
                    text_blend::Cache* blend = nullptr) {
    Rect dirty = empty_rect();
    // The text's rectangle, cut to the bitmap and the clip.
    const i64 top = static_cast<i64>(baseline_y) - f.baseline;
    const i64 width = static_cast<i64>(bitmap.bytes_per_row) * 8;
    i64 x0 = x + span.min_x;
    i64 x1 = x + span.max_x;
    i64 y0 = top;
    i64 y1 = top + f.y_size - 1;
    x0 = x0 < clip.x0 ? clip.x0 : x0;
    x0 = x0 < 0 ? 0 : x0;
    x1 = x1 > clip.x1 ? clip.x1 : x1;
    x1 = x1 >= width ? width - 1 : x1;
    y0 = y0 < clip.y0 ? clip.y0 : y0;
    y0 = y0 < 0 ? 0 : y0;
    y1 = y1 > clip.y1 ? clip.y1 : y1;
    y1 = y1 >= static_cast<i64>(bitmap.rows) ? static_cast<i64>(bitmap.rows) - 1 : y1;
    if (x0 > x1 || y0 > y1) {
        return dirty;
    }

    // The characters with pixels in those columns, wherever the styles
    // move them.
    const i32 smear = bold_smear(f, style);
    const i64 reach_right = smear + italic_shift(f, style, 0);
    const i64 reach_left = -italic_shift(f, style, f.y_size - 1);
    u32 begin = count;
    u32 end = 0;
    i64 begin_pen = 0;
    i64 pen = x;
    for (u32 i = 0; i < count; ++i) {
        const Glyph g = glyph_of(f, style.spacing, chars[i]);
        const i64 left = pen + g.kern;
        if (g.width != 0 && left + g.width - 1 + reach_right >= x0 && left - reach_left <= x1) {
            if (begin == count) {
                begin = i;
                begin_pen = pen;
            }
            end = i + 1;
        }
        pen += g.advance;
    }

    // One row of the text at a time, as bits that lie over the bytes of
    // the bitmap's row. The byte before them catches what the bold smear
    // moves into view from the left.
    constexpr u32 kLead = 1;
    u8 line[kLead + kMaxBytesPerRow];
    const u32 first = static_cast<u32>(x0 >> 3);
    const u32 bytes = static_cast<u32>(x1 >> 3) - first + 1;
    if (bytes == 0 || bytes > kMaxBytesPerRow) {
        return dirty;
    }
    const i64 origin = (static_cast<i64>(first) - kLead) * 8;
    u8* const bits = line + kLead;
    const u8 lmask = left_mask(x0);
    const u8 rmask = right_mask(x1);
    const bool inverse = (pens.mode & ndk::INVERSVID) != 0;
    const bool underlined = (style.algo & ndk::FSF_UNDERLINED) != 0;
    const u32 mode = pens.mode & ~ndk::INVERSVID;
    // Only an eight-plane palette through a cache can take the coverage
    // levels; every other mode keeps the font's 1-bit glyphs.
    const bool can_blend = f.coverage != nullptr && blend != nullptr && bitmap.depth == 8 &&
                           !inverse && (mode == ndk::JAM1 || mode == ndk::JAM2);
    const u8* bg_ramp = can_blend && mode == ndk::JAM2 ? blend->ramp(pens.fg, pens.bg) : nullptr;
    const bool antialiased = can_blend && (mode == ndk::JAM1 || bg_ramp != nullptr);
    for (i64 y = y0; y <= y1; ++y) {
        const i32 row = static_cast<i32>(y - top);
        if (antialiased) {
            // A coverage pixel for every column from the left the bold
            // smear can reach into, from x0 on.
            const u32 line_pixels = static_cast<u32>(x1 - x0 + 1) + static_cast<u32>(smear);
            u8 cover[kMaxBoldSmear + kMaxBytesPerRow * 8];
            for (u32 i = 0; i < line_pixels; ++i) {
                cover[i] = 0;
            }
            const i64 cover_origin = static_cast<i64>(x0) - smear;
            const u8* coverage = f.coverage + static_cast<u32>(row) * f.coverage_modulo;
            const i64 shift = italic_shift(f, style, row) - cover_origin;
            pen = begin_pen;
            const u64 cover_pixels = static_cast<u64>(f.coverage_modulo) * 2;
            for (u32 i = begin; i < end; ++i) {
                Glyph g = glyph_of(f, style.spacing, chars[i]);
                if (g.offset < cover_pixels && g.width != 0) {
                    if (static_cast<u64>(g.offset) + g.width > cover_pixels) {
                        g.width = static_cast<u32>(cover_pixels - g.offset);
                    }
                    or_coverage(cover, line_pixels, pen + g.kern + shift, coverage, g.offset,
                                g.width);
                }
                pen += g.advance;
            }
            if (underlined && row == underline_row(f)) {
                set_coverage(cover, line_pixels, x - cover_origin,
                             x + span.advance - 1 - cover_origin, 15);
            }
            if (smear != 0) {
                smear_coverage(cover, line_pixels, static_cast<u32>(smear));
            }
            const u32 row_offset = static_cast<u32>(y) * bitmap.bytes_per_row + first;
            u8* planes[8];
            for (u32 plane = 0; plane < 8; ++plane) {
                planes[plane] = bus.bytes_at(bitmap.planes[plane] + row_offset, bytes);
                if (planes[plane] == nullptr) {
                    return dirty;
                }
            }
            u32 leftmost = 0;
            u32 rightmost = 0;
            bool inked = false;
            for (u32 i = 0; i < bytes; ++i) {
                const u32 base = first * 8 + i * 8;
                u8 old[8];
                u8 result[8];
                for (u32 plane = 0; plane < 8; ++plane) {
                    old[plane] = planes[plane][i];
                    result[plane] = old[plane];
                }
                for (u32 bit = 0; bit < 8; ++bit) {
                    const u32 at_x = base + bit;
                    if (at_x < static_cast<u32>(x0) || at_x > static_cast<u32>(x1)) {
                        continue;
                    }
                    const u8 level = cover[static_cast<u32>(at_x - cover_origin)];
                    u8 pixel_pen = 0;
                    if (mode == ndk::JAM2) {
                        pixel_pen = bg_ramp[level];
                    } else {
                        if (level == 0) {
                            continue;
                        }
                        if (static_cast<u32>(level) == text_blend::kLevels - 1) {
                            pixel_pen = pens.fg;
                        } else {
                            const u8 bit_mask = static_cast<u8>(0x80u >> bit);
                            u32 under = 0;
                            for (u32 plane = 0; plane < 8; ++plane) {
                                if ((old[plane] & bit_mask) != 0) {
                                    under |= 1u << plane;
                                }
                            }
                            const u8* ramp = blend->ramp(pens.fg, static_cast<u8>(under));
                            if (ramp == nullptr) {
                                continue;
                            }
                            pixel_pen = ramp[level];
                        }
                        if (!inked) {
                            leftmost = at_x;
                            inked = true;
                        }
                        rightmost = at_x;
                    }
                    const u8 bit_mask = static_cast<u8>(0x80u >> bit);
                    for (u32 plane = 0; plane < 8; ++plane) {
                        if (((pixel_pen >> plane) & 1) != 0) {
                            result[plane] |= bit_mask;
                        } else {
                            result[plane] &= static_cast<u8>(~bit_mask);
                        }
                    }
                }
                for (u32 plane = 0; plane < 8; ++plane) {
                    planes[plane][i] = result[plane];
                }
            }
            if (mode == ndk::JAM2) {
                grow(dirty, static_cast<i32>(x0), static_cast<i32>(y));
                grow(dirty, static_cast<i32>(x1), static_cast<i32>(y));
            } else if (inked) {
                grow(dirty, static_cast<i32>(leftmost), static_cast<i32>(y));
                grow(dirty, static_cast<i32>(rightmost), static_cast<i32>(y));
            }
            continue;
        }
        for (u32 i = 0; i < kLead + bytes; ++i) {
            line[i] = 0;
        }
        const u8* src = f.data + static_cast<u32>(row) * f.modulo;
        const i64 shift = italic_shift(f, style, row) - origin;
        pen = begin_pen;
        for (u32 i = begin; i < end; ++i) {
            const Glyph g = glyph_of(f, style.spacing, chars[i]);
            or_bits(line, kLead + bytes, pen + g.kern + shift, src, f.modulo, g.offset, g.width);
            pen += g.advance;
        }
        if (underlined && row == underline_row(f)) {
            set_columns(line, kLead + bytes, x - origin, x + span.advance - 1 - origin);
        }
        if (smear != 0) {
            smear_right(line, kLead + bytes, smear);
        }
        // INVERSVID turns the text's whole rectangle over.
        if (inverse) {
            for (u32 i = 0; i < bytes; ++i) {
                bits[i] = static_cast<u8>(~bits[i]);
            }
        }
        const u32 row_offset = static_cast<u32>(y) * bitmap.bytes_per_row + first;
        if (mode == ndk::JAM2) {
            for (u32 plane = 0; plane < bitmap.depth && plane < 8; ++plane) {
                u8* dst = bus.bytes_at(bitmap.planes[plane] + row_offset, bytes);
                if (dst == nullptr) {
                    return dirty;
                }
                const u8 fg = ((pens.fg >> plane) & 1) != 0 ? 0xFF : 0x00;
                const u8 bg = ((pens.bg >> plane) & 1) != 0 ? 0xFF : 0x00;
                for (u32 i = 0; i < bytes; ++i) {
                    const u8 mask = static_cast<u8>((i == 0 ? lmask : 0xFF) &
                                                    (i + 1 == bytes ? rmask : 0xFF));
                    dst[i] = merge(dst[i], static_cast<u8>((bits[i] & fg) | (~bits[i] & bg)),
                                   mask);
                }
            }
            grow(dirty, static_cast<i32>(x0), static_cast<i32>(y));
            grow(dirty, static_cast<i32>(x1), static_cast<i32>(y));
            continue;
        }
        // JAM1 and COMPLEMENT change the set bits only.
        bits[0] &= lmask;
        bits[bytes - 1] &= rmask;
        u32 leftmost = 0;
        u32 rightmost = 0;
        if (!inked_columns(bits, bytes, leftmost, rightmost)) {
            continue;
        }
        for (u32 plane = 0; plane < bitmap.depth && plane < 8; ++plane) {
            u8* dst = bus.bytes_at(bitmap.planes[plane] + row_offset, bytes);
            if (dst == nullptr) {
                return dirty;
            }
            const bool fg = ((pens.fg >> plane) & 1) != 0;
            if (mode == ndk::COMPLEMENT) {
                for (u32 i = 0; fg && i < bytes; ++i) {
                    dst[i] ^= bits[i];
                }
            } else {
                for (u32 i = 0; i < bytes; ++i) {
                    dst[i] = fg ? dst[i] | bits[i] : dst[i] & static_cast<u8>(~bits[i]);
                }
            }
        }
        grow(dirty, static_cast<i32>(first * 8 + leftmost), static_cast<i32>(y));
        grow(dirty, static_cast<i32>(first * 8 + rightmost), static_cast<i32>(y));
    }
    return dirty;
}

}

// Adds one more character to the span of the text before it.
inline void extend_span(TextSpan& span, const Font& f, const TextStyle& style, u8 c) {
    const Glyph g = glyph_of(f, style.spacing, c);
    if (g.width != 0) {
        const i64 left = span.advance + g.kern;
        span.min_x = left < span.min_x ? left : span.min_x;
        span.max_x = left + g.width - 1 > span.max_x ? left + g.width - 1 : span.max_x;
    }
    span.advance += g.advance;
    span.max_x = span.advance - 1 > span.max_x ? span.advance - 1 : span.max_x;
}

// The span with the columns the bold and italic styles add at its ends.
inline TextSpan styled_span(TextSpan span, const Font& f, const TextStyle& style) {
    if (span.max_x >= span.min_x) {
        span.max_x += bold_smear(f, style) + italic_shift(f, style, 0);
        span.min_x += italic_shift(f, style, f.y_size - 1);
    }
    return span;
}

inline TextSpan measure_text(const Font& f, const TextStyle& style, const u8* chars, u32 count) {
    TextSpan span{};
    for (u32 i = 0; i < count; ++i) {
        extend_span(span, f, style, chars[i]);
    }
    return styled_span(span, f, style);
}

// Draws count characters with the pen at x on the row baseline_y: the set
// bits of the glyphs in the foreground pen under JAM1, the rest of the
// text's rectangle in the background pen as well under JAM2, the set bits
// turned over through the foreground pen under COMPLEMENT. INVERSVID swaps
// set and clear bits throughout the rectangle. A font with a coverage plane
// drawn through blend on an eight-plane bitmap takes the pen of each
// pixel's level instead of its 1-bit glyph. advance is how far the pen
// moves; the answer is the rectangle that changed.
inline Rect draw_text(native::Bus& bus, const Bitmap& bitmap, const Clip& clip, const Pens& pens,
                      const Font& f, const TextStyle& style, i32 x, i32 baseline_y,
                      const u8* chars, u32 count, u32& advance,
                      text_blend::Cache* blend = nullptr) {
    const TextSpan span = measure_text(f, style, chars, count);
    advance = static_cast<u32>(span.advance);
    Rect dirty = empty_rect();
    walk_clip(bus, clip, [&](const Clip& part) {
        dirty = unite_rects(dirty, draw_text_part(bus, bitmap, part, pens, f, style, span, x,
                                                  baseline_y, chars, count, blend));
        return true;
    });
    return dirty;
}

}
