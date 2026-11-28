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

// The Wuffs implementation, compiled once per target without our warning
// flags, and the readers of the decoder state declared in wuffs_state.h.
// They use private fields, under the names the Wuffs release in
// third_party/wuffs gives them.

#define WUFFS_IMPLEMENTATION
#include "wuffs_config.h"

#include "../../third_party/wuffs/release/c/wuffs-v0.4.c"

#include "wuffs_state.h"

// The zero bytes the JPEG decoder appends to a scan's data once its input
// is closed, and those it may append in all where a marker ends the data
// of a scan or of one of its restart intervals.
#define IMAGE_WUFFS_JPEG_CLOSED_PADDING 264u
#define IMAGE_WUFFS_JPEG_MARKER_PADDING 12345u

bool image_wuffs_png_wrote_rows(const wuffs_png__decoder* png) {
    if (png->private_impl.f_interlace_pass > 1u) {
        return true;
    }
    return png->private_impl.f_workbuf_wi >= 1u + png->private_impl.f_pass_bytes_per_row;
}

uint32_t image_wuffs_jpeg_components(const wuffs_jpeg__decoder* jpeg) {
    return jpeg->private_impl.f_num_components;
}

uint32_t image_wuffs_jpeg_scans(const wuffs_jpeg__decoder* jpeg) {
    return jpeg->private_impl.f_scan_count;
}

bool image_wuffs_jpeg_more_scans(const wuffs_jpeg__decoder* jpeg) {
    return jpeg->private_impl.f_expect_multiple_scans;
}

bool image_wuffs_jpeg_in_scan_data(const wuffs_jpeg__decoder* jpeg) {
    // Suspension points 2 and 3 of decode_sos: refilling the bits of a
    // block, and looking for a restart marker.
    return jpeg->private_impl.p_decode_sos >= 2u;
}

bool image_wuffs_jpeg_cut_may_end_scan(const wuffs_jpeg__decoder* jpeg) {
    if (jpeg->private_impl.f_restart_interval == 0u) {
        return true;
    }
    // The decoder starts the data of each restart interval afresh, and
    // looks for a marker after the last block of a full one, so what it
    // holds at the end tells only about a last interval the scan ends
    // inside. Otherwise the cut must lie after the scan's last block.
    uint64_t width = jpeg->private_impl.f_scan_width_in_mcus;
    uint64_t total = width * jpeg->private_impl.f_scan_height_in_mcus;
    uint64_t current = (uint64_t)jpeg->private_data.s_decode_sos.v_my * width +
                       jpeg->private_data.s_decode_sos.v_mx;
    if (jpeg->private_impl.p_decode_sos == 3u) {
        return current + 1u == total;
    }
    return (current < total) && (total - current < jpeg->private_impl.f_restarts_remaining);
}

uint32_t image_wuffs_jpeg_rows_before_cut(const wuffs_jpeg__decoder* jpeg, uint32_t height) {
    if (jpeg->private_impl.p_decode_sos == 0u) {
        return 0;
    }
    uint64_t rows = (uint64_t)jpeg->private_data.s_decode_sos.v_my * 8u *
                    jpeg->private_impl.f_max_incl_components_v;
    if (jpeg->private_impl.f_scan_num_components == 1u) {
        // A scan of one component has a block row for every 8 of its rows.
        uint8_t component = jpeg->private_impl.f_scan_comps_cselector[0] & 3u;
        uint32_t v = jpeg->private_impl.f_components_v[component];
        rows = (v != 0u) ? rows / v : 0u;
    }
    return (rows < height) ? (uint32_t)rows : height;
}

bool image_wuffs_jpeg_scan_within_file(const wuffs_jpeg__decoder* jpeg) {
    // The zero bytes all come after the last byte of the file in the data
    // of the scan's last interval, and the decoder holds [ri, wi) of that
    // data. Those it has read past, less the bits it has not used yet, are
    // the zero bits it decoded.
    uint64_t zeros = IMAGE_WUFFS_JPEG_MARKER_PADDING - jpeg->private_impl.f_bitstream_padding;
    if (jpeg->private_impl.f_bitstream_is_closed) {
        zeros += IMAGE_WUFFS_JPEG_CLOSED_PADDING;
    }
    uint64_t unread =
        jpeg->private_impl.f_bitstream_wi - jpeg->private_impl.f_bitstream_ri;
    if (zeros <= unread) {
        return true;
    }
    return (zeros - unread) * 8u <= jpeg->private_impl.f_bitstream_n_bits;
}
