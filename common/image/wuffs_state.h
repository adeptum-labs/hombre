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

// What wuffs.c needs to know about a decoder after it stopped, read from
// fields Wuffs keeps private. Only wuffs_impl.c sees the decoder structs,
// so these live there.

#ifndef HOMBRE_IMAGE_WUFFS_STATE_H
#define HOMBRE_IMAGE_WUFFS_STATE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct wuffs_png__decoder__struct wuffs_png__decoder;
typedef struct wuffs_jpeg__decoder__struct wuffs_jpeg__decoder;

// Whether the PNG decoder has written rows of the picture: every earlier
// pass of an interlaced one, or a whole row of the pass it was in.
bool image_wuffs_png_wrote_rows(const wuffs_png__decoder* png);

// The JPEG's components, as its frame header gives them.
uint32_t image_wuffs_jpeg_components(const wuffs_jpeg__decoder* jpeg);

// The scans the JPEG decoder has finished.
uint32_t image_wuffs_jpeg_scans(const wuffs_jpeg__decoder* jpeg);

// Whether more scans may follow the first: a progressive JPEG, or a
// baseline one whose first scan lacks some of the components.
bool image_wuffs_jpeg_more_scans(const wuffs_jpeg__decoder* jpeg);

// Whether the JPEG decoder waits inside a scan's entropy-coded data.
bool image_wuffs_jpeg_in_scan_data(const wuffs_jpeg__decoder* jpeg);

// The picture rows a JPEG's first scan holds once the input has run out:
// the decoder then waits inside the scan, before the block row it lacks
// bytes for. 0 when it waits anywhere else.
uint32_t image_wuffs_jpeg_rows_before_cut(const wuffs_jpeg__decoder* jpeg, uint32_t height);

// Whether a JPEG decoder that waits inside a scan's data, the file read to
// its end, can tell afterwards that the scan's data was all there. A scan
// that can only end after further restart markers was cut, and one whose
// last restart interval is full cannot be told from a cut one.
bool image_wuffs_jpeg_cut_may_end_scan(const wuffs_jpeg__decoder* jpeg);

// Whether the scan the JPEG decoder finished last took its bits from the
// file alone. Where the data of a scan ends, at a marker or at the end of
// the input, the decoder goes on with zero bytes so it can read the last
// blocks; a scan that needed any of their bits was cut short. The answer
// covers the scan's last restart interval only.
bool image_wuffs_jpeg_scan_within_file(const wuffs_jpeg__decoder* jpeg);

#endif
