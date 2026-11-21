#!/usr/bin/env python3
# The Hombre Operating System.
# Copyright © 2026 Adam Waldenberg and Adeptum AB.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# Website: https://www.adeptum.se
# Contact: info@adeptum.se

# Turns Hasklig-Regular into assets/fonts/hasklig/hasklig.romfont: the
# bitmap fonts the kernel keeps in memory as hasklig.font. Each size is the
# hinted outline cut to one colour, a character to a cell, every cell the
# same width, with the coverage of the grey rendering beside it. Run by
# hand with the path of Hasklig-Regular.ttf from the release named in
# assets/fonts/hasklig/README; make does not call this.
#
# The file is "HRF2", the number of fonts, then each font: its name in 32
# bytes, the rows and columns of a cell, the baseline row, the bold smear,
# the first and last character, the bytes of a row of glyph data and of a
# row of coverage, then that data: the one bit plane with the cells side by
# side, the cell after the last character being the one drawn for every
# character the font has none for, and the coverage plane, four bits a
# pixel, two pixels a byte, the left one in the high nibble.
#
#   tools/mkfont.py Hasklig-Regular.ttf [--preview preview.png]

import argparse
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
TARGET = ROOT / "assets" / "fonts" / "hasklig" / "hasklig.romfont"

NAME = "hasklig.font"
LO_CHAR = 32
HI_CHAR = 255
BOLD_SMEAR = 1

# A size of the font: the rows and columns of its cell, the pixels per em
# the outline is drawn at, the rows of the cell above the baseline, and the
# characters whose counters the one-colour rasteriser fills in, which are
# cut from the grey rendering instead. The em gives the capitals, an
# accent above them and the descenders the rows; the columns are the
# advance of the outline at that em, or a pixel more where its glyphs
# would touch.
SIZES = (
    {"rows": 8, "columns": 6, "em": 8, "ascent": 6, "grey": "m"},
    {"rows": 11, "columns": 6, "em": 10, "ascent": 9, "grey": ""},
    {"rows": 13, "columns": 7, "em": 11, "ascent": 10, "grey": ""},
)

# The least coverage, of 255, that sets a pixel of a grey rendering.
GREY_COVERAGE = 110

MARGIN = 16
CANVAS = MARGIN * 3
PREVIEW_TEXT = (
    "The quick brown fox jumps over the lazy dog. 0123456789",
    "HOMBRE [3.1] {~} <=> != -> :: /* ÅÄÖ åäö Éé Üü ß £ © ½ */",
    "".join(chr(code) for code in range(32, 127)),
    "".join(chr(code) for code in range(160, 256)),
)


def render_grey(font, text):
    # The coverage of the hinted outline, from 0 to 255, its origin MARGIN
    # rows down and MARGIN columns in.
    image = Image.new("L", (CANVAS, CANVAS), 0)
    ImageDraw.Draw(image).text(
        (MARGIN, MARGIN), text, fill=255, font=font, anchor="ls"
    )
    return [
        [image.getpixel((x, y)) for x in range(CANVAS)]
        for y in range(CANVAS)
    ]


def render_mono(font, text):
    # The hinted outline cut to one colour.
    image = Image.new("L", (CANVAS, CANVAS), 0)
    draw = ImageDraw.Draw(image)
    draw.fontmode = "1"
    draw.text((MARGIN, MARGIN), text, fill=255, font=font, anchor="ls")
    return [
        [image.getpixel((x, y)) >= 1 for x in range(CANVAS)]
        for y in range(CANVAS)
    ]


def render(font, text, size):
    # Both renderings: the one the one-bit plane keeps, taken from the grey
    # where the one-colour rasteriser fills the counters in, and the grey
    # itself for the coverage plane.
    grey = render_grey(font, text)
    if text in size["grey"]:
        mono = [[value >= GREY_COVERAGE for value in row] for row in grey]
    else:
        mono = render_mono(font, text)
    return mono, grey


def left_edge(rows, size):
    # The column the cell begins in: the glyph's origin, or further left
    # for ink that would pass the cell's right edge, as far as the blank
    # columns left of the ink allow.
    inked = [x for x in range(CANVAS) if any(row[x] for row in rows)]
    if not inked:
        return MARGIN
    past = max(inked[-1] - (MARGIN + size["columns"] - 1), 0)
    return MARGIN + min(past, max(inked[0] - MARGIN, 0))


def inked(rows):
    return any(any(row) for row in rows)


def close_gap(planes, first, last, step):
    # Takes out the blank row nearest the baseline among the rows from
    # first to last, so that everything beyond it moves a row towards the
    # baseline, as an accent does onto its letter, in every plane at once.
    # False if none is blank.
    union = planes[0]
    for at in range(first, last, step):
        if not any(union[at]):
            for rows in planes:
                shift(rows, at, step)
            return True
    return False


def shift(rows, at, step):
    # Moves the rows from at towards the near end over the blank row at,
    # leaving a blank at the far end.
    blank = [False] * len(rows[0])
    if step < 0:
        rows[1 : at + 1] = rows[0:at]
        rows[0] = blank
    else:
        rows[at : len(rows) - 1] = rows[at + 1 :]
        rows[-1] = blank


def cell_of(mono, grey, size):
    # The rows of the cell, both planes cut the same way: ink above or
    # below the cell moves in where a blank row gives way and is cut off
    # where none does, decided on the one-colour ink alone so the one-bit
    # cell comes out as it always has. The grey cell is four bits a pixel.
    union = [list(row) for row in mono]
    planes = (union, mono, grey)
    top = MARGIN - size["ascent"]
    bottom = top + size["rows"]
    while inked(union[:top]) and close_gap(planes, MARGIN - 1, top - 1, -1):
        pass
    while inked(union[bottom:]) and close_gap(planes, MARGIN, bottom, 1):
        pass
    left = left_edge(union, size)
    columns = size["columns"]
    return (
        [row[left : left + columns] for row in mono[top:bottom]],
        [
            [(value * 15 + 127) // 255 for value in row[left : left + columns]]
            for row in grey[top:bottom]
        ],
    )


def missing_cell(size):
    # A box, as tall as a capital, for the characters without a glyph, its
    # coverage full where the box is set.
    columns = size["columns"]
    height = (size["em"] * 2 + 2) // 3
    blank = [False] * columns
    edge = [True] * (columns - 1) + [False]
    side = [True] + [False] * (columns - 3) + [True, False]
    box = [edge] + [side] * (height - 2) + [edge]
    above = size["ascent"] - height
    mono = [blank] * above + box + [blank] * (size["rows"] - size["ascent"])
    return mono, [[15 if bit else 0 for bit in row] for row in mono]


def cells_of(path, size):
    font = ImageFont.truetype(
        str(path), size["em"], layout_engine=ImageFont.Layout.BASIC
    )
    undefined = render(font, "\uffff", size)[0]
    missing = missing_cell(size)
    cells = []
    for code in range(LO_CHAR, HI_CHAR + 1):
        mono, grey = render(font, bytes((code,)).decode("latin-1"), size)
        has_glyph = code in (0x20, 0xA0) or mono != undefined
        cells.append(cell_of(mono, grey, size) if has_glyph else missing)
    cells.append(missing)
    return cells


def image_of(cell):
    image = Image.new("1", (len(cell[0]), len(cell)), 0)
    image.putdata([1 if bit else 0 for row in cell for bit in row])
    return image


def grey_image_of(cell):
    image = Image.new("L", (len(cell[0]), len(cell)), 0)
    image.putdata([value * 17 for row in cell for value in row])
    return image


def coverage_plane(cells, coverage_modulo, size):
    # The cells' coverage side by side, two pixels a byte, the left one in
    # the high nibble, padded to the row's width with blank pixels.
    width = coverage_modulo * 2
    plane = bytearray()
    for y in range(size["rows"]):
        row = [value for _, cell in cells for value in cell[y]]
        row += [0] * (width - len(row))
        plane += bytes((row[x] << 4) | row[x + 1] for x in range(0, width, 2))
    return bytes(plane)


def pack(cells, size):
    columns = size["columns"]
    modulo = (len(cells) * columns + 15) // 16 * 2
    coverage_modulo = ((len(cells) * columns + 1) // 2 + 1) // 2 * 2
    strip = Image.new("1", (modulo * 8, size["rows"]), 0)
    for index, (cell, _) in enumerate(cells):
        strip.paste(image_of(cell), (index * columns, 0))
    header = struct.pack(
        "<32sHHHHBBHH",
        NAME.encode("ascii"),
        size["rows"],
        columns,
        size["ascent"] - 1,
        BOLD_SMEAR,
        LO_CHAR,
        HI_CHAR,
        modulo,
        coverage_modulo,
    )
    return header + strip.tobytes() + coverage_plane(cells, coverage_modulo, size)


def preview(fonts, target):
    scale = 4
    width = max(len(line) for line in PREVIEW_TEXT) * max(
        size["columns"] for size, _ in fonts
    )
    height = sum((size["rows"] + 2) * len(PREVIEW_TEXT) + 6 for size, _ in fonts)
    mono_sheet = Image.new("1", (width, height), 0)
    grey_sheet = Image.new("L", (width, height), 0)
    y = 2
    for size, cells in fonts:
        for line in PREVIEW_TEXT:
            for index, char in enumerate(line):
                code = ord(char)
                cell = cells[code - LO_CHAR if LO_CHAR <= code <= HI_CHAR else -1]
                mono_sheet.paste(image_of(cell[0]), (index * size["columns"], y))
                grey_sheet.paste(grey_image_of(cell[1]), (index * size["columns"], y))
            y += size["rows"] + 2
        y += 6
    size = (width * scale, height * scale)
    mono_sheet.resize(size, Image.Resampling.NEAREST).save(target)
    grey_sheet.resize(size, Image.Resampling.NEAREST).save(
        target.with_name(target.stem + "-grey" + target.suffix)
    )


def main():
    parser = argparse.ArgumentParser(
        description="Makes the bitmap fonts of hasklig.font from Hasklig-Regular."
    )
    parser.add_argument("source", type=Path, help="Hasklig-Regular.ttf")
    parser.add_argument("--preview", type=Path, help="write a sheet of sample text")
    arguments = parser.parse_args()

    fonts = [(size, cells_of(arguments.source, size)) for size in SIZES]
    data = bytearray(b"HRF2")
    data += struct.pack("<H", len(fonts))
    for size, cells in fonts:
        data += pack(cells, size)

    TARGET.parent.mkdir(parents=True, exist_ok=True)
    TARGET.write_bytes(data)
    sizes = ", ".join(f"{size['columns']}x{size['rows']}" for size, _ in fonts)
    print(f"{TARGET}: {NAME} {sizes}, {len(data)} bytes")
    if arguments.preview:
        preview(fonts, arguments.preview)


if __name__ == "__main__":
    main()
