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
# same width. Run by hand with the path of Hasklig-Regular.ttf from the
# release named in assets/fonts/hasklig/README; make does not call this.
#
# The file is "HRF1", the number of fonts, then each font: its name in 32
# bytes, the rows and columns of a cell, the baseline row, the bold smear,
# the first and last character, the bytes of a row of glyph data, and that
# data, one bit plane with the cells side by side, the cell after the last
# character being the one drawn for every character the font has none for.
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


def render(font, text, size):
    # The rows of the hinted outline, its origin MARGIN rows down and
    # MARGIN columns in.
    image = Image.new("L", (CANVAS, CANVAS), 0)
    draw = ImageDraw.Draw(image)
    grey = text in size["grey"]
    if not grey:
        draw.fontmode = "1"
    draw.text((MARGIN, MARGIN), text, fill=255, font=font, anchor="ls")
    least = GREY_COVERAGE if grey else 1
    return [
        [image.getpixel((x, y)) >= least for x in range(CANVAS)]
        for y in range(CANVAS)
    ]


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


def close_gap(rows, first, last, step):
    # Takes out the blank row nearest the baseline among the rows from
    # first to last, so that everything beyond it moves a row towards the
    # baseline, as an accent does onto its letter. False if none is blank.
    for at in range(first, last, step):
        if not any(rows[at]):
            del rows[at]
            rows.insert(0 if step < 0 else len(rows), [False] * len(rows[0]))
            return True
    return False


def cell_of(rows, size):
    # The rows of the cell. Ink above or below it moves in where a blank
    # row gives way and is cut off where none does.
    rows = [list(row) for row in rows]
    top = MARGIN - size["ascent"]
    bottom = top + size["rows"]
    while inked(rows[:top]) and close_gap(rows, MARGIN - 1, top - 1, -1):
        pass
    while inked(rows[bottom:]) and close_gap(rows, MARGIN, bottom, 1):
        pass
    left = left_edge(rows, size)
    return [row[left : left + size["columns"]] for row in rows[top:bottom]]


def missing_cell(size):
    # A box, as tall as a capital, for the characters without a glyph.
    columns = size["columns"]
    height = (size["em"] * 2 + 2) // 3
    blank = [False] * columns
    edge = [True] * (columns - 1) + [False]
    side = [True] + [False] * (columns - 3) + [True, False]
    box = [edge] + [side] * (height - 2) + [edge]
    above = size["ascent"] - height
    return [blank] * above + box + [blank] * (size["rows"] - size["ascent"])


def cells_of(path, size):
    font = ImageFont.truetype(
        str(path), size["em"], layout_engine=ImageFont.Layout.BASIC
    )
    undefined = render(font, "\uffff", size)
    missing = missing_cell(size)
    cells = []
    for code in range(LO_CHAR, HI_CHAR + 1):
        rows = render(font, bytes((code,)).decode("latin-1"), size)
        has_glyph = code in (0x20, 0xA0) or rows != undefined
        cells.append(cell_of(rows, size) if has_glyph else missing)
    cells.append(missing)
    return cells


def image_of(cell):
    image = Image.new("1", (len(cell[0]), len(cell)), 0)
    image.putdata([1 if bit else 0 for row in cell for bit in row])
    return image


def pack(cells, size):
    columns = size["columns"]
    modulo = (len(cells) * columns + 15) // 16 * 2
    strip = Image.new("1", (modulo * 8, size["rows"]), 0)
    for index, cell in enumerate(cells):
        strip.paste(image_of(cell), (index * columns, 0))
    header = struct.pack(
        "<32sHHHHBBH",
        NAME.encode("ascii"),
        size["rows"],
        columns,
        size["ascent"] - 1,
        BOLD_SMEAR,
        LO_CHAR,
        HI_CHAR,
        modulo,
    )
    return header + strip.tobytes()


def preview(fonts, target):
    scale = 4
    width = max(len(line) for line in PREVIEW_TEXT) * max(
        size["columns"] for size, _ in fonts
    )
    height = sum((size["rows"] + 2) * len(PREVIEW_TEXT) + 6 for size, _ in fonts)
    sheet = Image.new("1", (width, height), 0)
    y = 2
    for size, cells in fonts:
        for line in PREVIEW_TEXT:
            for index, char in enumerate(line):
                code = ord(char)
                cell = cells[code - LO_CHAR if LO_CHAR <= code <= HI_CHAR else -1]
                sheet.paste(image_of(cell), (index * size["columns"], y))
            y += size["rows"] + 2
        y += 6
    sheet.resize((width * scale, height * scale), Image.Resampling.NEAREST).save(target)


def main():
    parser = argparse.ArgumentParser(
        description="Makes the bitmap fonts of hasklig.font from Hasklig-Regular."
    )
    parser.add_argument("source", type=Path, help="Hasklig-Regular.ttf")
    parser.add_argument("--preview", type=Path, help="write a sheet of sample text")
    arguments = parser.parse_args()

    fonts = [(size, cells_of(arguments.source, size)) for size in SIZES]
    data = bytearray(b"HRF1")
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
