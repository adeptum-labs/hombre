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

# Turns assets/splash.jpg into assets/splash.rle: the decorative progress
# bar band is painted black, the image is scaled down, quantised and stored
# as a palette followed by runs of palette indices in row order. The kernel
# draws its own progress blocks over the black band. Run by hand after
# changing the artwork; make does not call this.

import struct
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "assets" / "splash.jpg"
TARGET = ROOT / "assets" / "splash.rle"

OUTPUT_WIDTH = 1280
MAX_COLOURS = 128
BLOCKS = 20
BAR_LEFT, BAR_TOP, BAR_RIGHT, BAR_BOTTOM = 895, 1236, 1811, 1304
BAND_TOP, BAND_BOTTOM = 1220, 1320


# Pixels whose brightest channel is below this count as black.
DARK_LIMIT = 40


def scaled(value, source_size, target_size):
    return (value * target_size + source_size // 2) // source_size


def quantize(image):
    # One colour is kept back so pure black can take index 0.
    quantized = image.quantize(
        colors=MAX_COLOURS - 1,
        method=Image.Quantize.MEDIANCUT,
        dither=Image.Dither.NONE,
    )
    source_palette = quantized.getpalette()
    used = sorted(quantized.getcolors(MAX_COLOURS), key=lambda entry: entry[1])
    black = next(
        (
            index
            for _, index in used
            if tuple(source_palette[index * 3 : index * 3 + 3]) == (0, 0, 0)
        ),
        None,
    )
    palette = [(0, 0, 0)]
    mapping = [0] * 256
    for _, index in used:
        if index == black:
            continue
        mapping[index] = len(palette)
        palette.append(tuple(source_palette[index * 3 : index * 3 + 3]))
    return palette, quantized.tobytes().translate(bytes(mapping))


def encode_runs(indices):
    data = bytearray()
    count = 0
    at = 0
    total = len(indices)
    while at < total:
        end = at + 1
        while end < total and indices[end] == indices[at] and end - at < 256:
            end += 1
        data.append(end - at - 1)
        data.append(indices[at])
        at = end
        count += 1
    return data, count


def main():
    image = Image.open(SOURCE).convert("RGB")
    width, height = image.size
    ImageDraw.Draw(image).rectangle(
        (0, BAND_TOP, width - 1, BAND_BOTTOM), fill=(0, 0, 0)
    )
    output_height = max(1, (height * OUTPUT_WIDTH + width // 2) // width)
    image = image.resize(
        (OUTPUT_WIDTH, output_height), Image.Resampling.LANCZOS
    )
    # The JPEG's background is only nearly black: dark pixels become black,
    # so the background packs into long runs.
    pixels = image.load()
    for y in range(image.size[1]):
        for x in range(image.size[0]):
            r, g, b = pixels[x, y]
            if max(r, g, b) < DARK_LIMIT:
                pixels[x, y] = (0, 0, 0)
    palette, indices = quantize(image)

    data = bytearray(b"HSPL")
    data += struct.pack(
        "<HHHHHHBB",
        OUTPUT_WIDTH,
        output_height,
        scaled(BAR_LEFT, width, OUTPUT_WIDTH),
        scaled(BAR_TOP, height, output_height),
        scaled(BAR_RIGHT, width, OUTPUT_WIDTH),
        scaled(BAR_BOTTOM, height, output_height),
        BLOCKS,
        len(palette),
    )
    for red, green, blue in palette:
        data += bytes((red, green, blue))
    runs, run_count = encode_runs(indices)
    data += runs

    TARGET.parent.mkdir(parents=True, exist_ok=True)
    TARGET.write_bytes(data)
    print(
        f"{TARGET}: {OUTPUT_WIDTH}x{output_height}, {len(palette)} colours, "
        f"{run_count} runs, {len(data)} bytes"
    )


if __name__ == "__main__":
    main()
