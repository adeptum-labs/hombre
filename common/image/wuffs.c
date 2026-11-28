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

// Wuffs' public declarations only; the implementation is compiled once per
// target in wuffs_impl.c, without our warning flags.

#include "wuffs_wrap.h"
#include "wuffs_config.h"
#include "wuffs_state.h"

#include "../../third_party/wuffs/release/c/wuffs-v0.4.c"

#define IMAGE_WUFFS_CHUNK 65536u
#define IMAGE_WUFFS_CLEAR_STEP (1024u * 1024u)
#define IMAGE_WUFFS_SIDE_LIMIT 16383u
#define IMAGE_WUFFS_PIXEL_LIMIT (64u * 1024u * 1024u)

enum { INPUT_MORE, INPUT_END, INPUT_STUCK };

// The decoder sees the file through a window that grows by a chunk per
// step. One byte range may be hidden from it: the window then covers the
// bytes before the range and, once the decoder has read all of those, the
// bytes after it, at positions that continue as if the range were not
// there.
typedef struct image_wuffs_input {
    wuffs_base__io_buffer io;
    const uint8_t* file;
    size_t bytes;
    size_t hidden_start;
    size_t hidden_end;
    size_t chunk;
    int past_hidden;
    uint8_t tail[8];
} image_wuffs_input;

typedef struct image_wuffs_decoder {
    void* block;
    size_t bytes;
    image_wuffs_format format;
} image_wuffs_decoder;

// The start of frame segment of a JPEG.
typedef struct image_wuffs_jpeg_frame {
    uint8_t marker;
    uint8_t precision;
    uint16_t height;
    uint16_t width;
    uint8_t components;
} image_wuffs_jpeg_frame;

static int text_has(const char* text, const char* word) {
    for (; *text != '\0'; ++text) {
        const char* a = text;
        const char* b = word;
        while ((*a != '\0') && (*b != '\0') && (*a == *b)) {
            ++a;
            ++b;
        }
        if (*b == '\0') {
            return 1;
        }
    }
    return 0;
}

// A PNG's picture data can end inside its zlib or deflate stream, which then
// reports the cut itself.
static int is_truncation(wuffs_base__status status) {
    return (status.repr == wuffs_png__error__truncated_input) ||
           (status.repr == wuffs_zlib__error__truncated_input) ||
           (status.repr == wuffs_deflate__error__truncated_input) ||
           (status.repr == wuffs_jpeg__error__truncated_input);
}

static image_wuffs_status status_of(wuffs_base__status status) {
    if (status.repr == 0) {
        return IMAGE_WUFFS_OK;
    }
    if (status.repr == wuffs_base__error__unsupported_image_dimension) {
        return IMAGE_WUFFS_TOO_LARGE;
    }
    if (is_truncation(status)) {
        return IMAGE_WUFFS_TRUNCATED;
    }
    if (text_has(status.repr, "unsupported") || text_has(status.repr, "rejected")) {
        return IMAGE_WUFFS_UNSUPPORTED;
    }
    return IMAGE_WUFFS_CORRUPT;
}

static image_wuffs_status check_dimensions(uint32_t width, uint32_t height) {
    if ((width == 0u) || (height == 0u)) {
        return IMAGE_WUFFS_CORRUPT;
    }
    if ((width > IMAGE_WUFFS_SIDE_LIMIT) || (height > IMAGE_WUFFS_SIDE_LIMIT)) {
        return IMAGE_WUFFS_TOO_LARGE;
    }
    if ((uint64_t)width * (uint64_t)height > (uint64_t)IMAGE_WUFFS_PIXEL_LIMIT) {
        return IMAGE_WUFFS_TOO_LARGE;
    }
    return IMAGE_WUFFS_OK;
}

static image_wuffs_format format_of(const uint8_t* file, size_t bytes) {
    if ((bytes >= 8u) && (file[0] == 0x89u) && (file[1] == 0x50u) && (file[2] == 0x4Eu) &&
        (file[3] == 0x47u) && (file[4] == 0x0Du) && (file[5] == 0x0Au) &&
        (file[6] == 0x1Au) && (file[7] == 0x0Au)) {
        return IMAGE_WUFFS_FORMAT_PNG;
    }
    if ((bytes >= 2u) && (file[0] == 0xFFu) && (file[1] == 0xD8u)) {
        return IMAGE_WUFFS_FORMAT_JPEG;
    }
    return IMAGE_WUFFS_FORMAT_UNKNOWN;
}

static uint32_t be16(const uint8_t* at) {
    return ((uint32_t)at[0] << 8) | (uint32_t)at[1];
}

static uint32_t be32(const uint8_t* at) {
    return ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16) | ((uint32_t)at[2] << 8) |
           (uint32_t)at[3];
}

// Walks the segments in front of the first scan the way the decoder does,
// bytes other than 0xFF between them included, and fills in the start of
// frame segment. Its marker stays 0 when none lies wholly inside the file.
static void jpeg_find_frame(const uint8_t* file, size_t bytes, image_wuffs_jpeg_frame* frame) {
    frame->marker = 0;
    size_t at = 2;
    for (;;) {
        while ((at < bytes) && (file[at] != 0xFFu)) {
            ++at;
        }
        while ((at < bytes) && (file[at] == 0xFFu)) {
            ++at;
        }
        if (at >= bytes) {
            return;
        }
        uint8_t marker = file[at++];
        if ((marker == 0x00u) || (marker == 0x01u) || ((marker >= 0xD0u) && (marker <= 0xD8u))) {
            continue;
        }
        if ((marker == 0xD9u) || (marker == 0xDAu) || (bytes - at < 2u)) {
            return;
        }
        size_t length = be16(file + at);
        if ((length < 2u) || (length > bytes - at)) {
            return;
        }
        if ((marker >= 0xC0u) && (marker <= 0xCFu) && (marker != 0xC4u) && (marker != 0xC8u) &&
            (marker != 0xCCu)) {
            if (length < 8u) {
                return;
            }
            frame->marker = marker;
            frame->precision = file[at + 2u];
            frame->height = (uint16_t)be16(file + at + 3u);
            frame->width = (uint16_t)be16(file + at + 5u);
            frame->components = file[at + 7u];
            return;
        }
        at += length;
    }
}

// An animated PNG with an acTL chunk but no fcTL before its IDAT keeps its
// still picture in the IDAT, outside the animation, and the decoder would
// return the first animation frame instead. Hiding the acTL chunk from it
// leaves a plain PNG. Sets *start and *end to the chunk's byte range, or
// both to `bytes` when nothing needs hiding.
static void png_find_hidden(const uint8_t* file, size_t bytes, size_t* start, size_t* end) {
    size_t at = 8u;
    size_t actl_start = bytes;
    size_t actl_end = bytes;
    *start = bytes;
    *end = bytes;
    while (bytes - at >= 8u) {
        uint32_t length = be32(file + at);
        uint32_t type = be32(file + at + 4u);
        if (type == 0x49444154u) {  // IDAT
            *start = actl_start;
            *end = actl_end;
            return;
        }
        if ((type == 0x6663544Cu) || (length > 0x7FFFFFFFu) ||  // fcTL
            ((uint64_t)length + 12u > (uint64_t)(bytes - at))) {
            return;
        }
        if ((type == 0x6163544Cu) && (actl_start == bytes)) {  // acTL
            actl_start = at;
            actl_end = at + 12u + length;
        }
        at += 12u + length;
    }
}

static void input_window(image_wuffs_input* input, size_t start, size_t end,
                         uint64_t position) {
    size_t length = end - start;
    size_t filled = (length < input->chunk) ? length : input->chunk;
    input->io.data = wuffs_base__make_slice_u8((uint8_t*)input->file + start, length);
    input->io.meta = wuffs_base__make_io_buffer_meta(filled, 0, position, false);
}

static void input_init(image_wuffs_input* input, const uint8_t* file, size_t bytes,
                       image_wuffs_format format) {
    input->file = file;
    input->bytes = bytes;
    input->hidden_start = bytes;
    input->hidden_end = bytes;
    input->chunk = IMAGE_WUFFS_CHUNK;
    if (format == IMAGE_WUFFS_FORMAT_PNG) {
        png_find_hidden(file, bytes, &input->hidden_start, &input->hidden_end);
    } else {
        image_wuffs_jpeg_frame frame;
        jpeg_find_frame(file, bytes, &frame);
        if (frame.marker == 0xC2u) {
            input->chunk = bytes;
        }
    }
    input->past_hidden = 0;
    input_window(input, 0, input->hidden_start, 0);
}

// Gives the decoder the next chunk of the file.
static int input_more(image_wuffs_input* input) {
    wuffs_base__io_buffer* io = &input->io;
    if (io->meta.wi < io->data.len) {
        size_t remaining = io->data.len - io->meta.wi;
        io->meta.wi += (remaining > input->chunk) ? input->chunk : remaining;
        return INPUT_MORE;
    }
    if (!input->past_hidden && (input->hidden_end < input->bytes)) {
        if (io->meta.ri != io->meta.wi) {
            return INPUT_STUCK;
        }
        input->past_hidden = 1;
        input_window(input, input->hidden_end, input->bytes, input->hidden_start);
        return INPUT_MORE;
    }
    return INPUT_END;
}

static int step_allowed(const image_wuffs_progress* progress) {
    return (progress == 0) || progress->keep_going(progress->ctx);
}

static image_wuffs_status clear_bytes(uint8_t* bytes, size_t count,
                                      const image_wuffs_progress* progress) {
    while (count != 0u) {
        if (!step_allowed(progress)) {
            return IMAGE_WUFFS_STOPPED;
        }
        size_t step = (count < IMAGE_WUFFS_CLEAR_STEP) ? count : IMAGE_WUFFS_CLEAR_STEP;
        memset(bytes, 0, step);
        bytes += step;
        count -= step;
    }
    return IMAGE_WUFFS_OK;
}

static wuffs_base__status decoder_config(image_wuffs_decoder* decoder,
                                         wuffs_base__image_config* config,
                                         wuffs_base__io_buffer* io) {
    if (decoder->format == IMAGE_WUFFS_FORMAT_PNG) {
        return wuffs_png__decoder__decode_image_config((wuffs_png__decoder*)decoder->block,
                                                       config, io);
    }
    return wuffs_jpeg__decoder__decode_image_config((wuffs_jpeg__decoder*)decoder->block,
                                                    config, io);
}

static wuffs_base__range_ii_u64 decoder_workbuf_len(const image_wuffs_decoder* decoder) {
    if (decoder->format == IMAGE_WUFFS_FORMAT_PNG) {
        return wuffs_png__decoder__workbuf_len((const wuffs_png__decoder*)decoder->block);
    }
    return wuffs_jpeg__decoder__workbuf_len((const wuffs_jpeg__decoder*)decoder->block);
}

static wuffs_base__status decoder_frame(image_wuffs_decoder* decoder,
                                        wuffs_base__pixel_buffer* pixels,
                                        wuffs_base__io_buffer* io,
                                        wuffs_base__slice_u8 workbuf) {
    if (decoder->format == IMAGE_WUFFS_FORMAT_PNG) {
        return wuffs_png__decoder__decode_frame((wuffs_png__decoder*)decoder->block, pixels,
                                                io, WUFFS_BASE__PIXEL_BLEND__SRC, workbuf, 0);
    }
    return wuffs_jpeg__decoder__decode_frame((wuffs_jpeg__decoder*)decoder->block, pixels,
                                             io, WUFFS_BASE__PIXEL_BLEND__SRC, workbuf, 0);
}

static image_wuffs_status decoder_open(const image_wuffs_allocator* host,
                                       image_wuffs_format format,
                                       image_wuffs_decoder* decoder) {
    wuffs_base__status status;
    decoder->block = 0;
    decoder->format = format;
    decoder->bytes = (format == IMAGE_WUFFS_FORMAT_PNG) ? sizeof__wuffs_png__decoder()
                                                        : sizeof__wuffs_jpeg__decoder();
    void* block = host->alloc(host->ctx, decoder->bytes);
    if (block == 0) {
        return IMAGE_WUFFS_NO_MEMORY;
    }
    if (format == IMAGE_WUFFS_FORMAT_PNG) {
        status = wuffs_png__decoder__initialize(block, decoder->bytes, WUFFS_VERSION, 0);
    } else {
        status = wuffs_jpeg__decoder__initialize(block, decoder->bytes, WUFFS_VERSION, 0);
    }
    if (status.repr != 0) {
        image_wuffs_status code = status_of(status);
        host->release(host->ctx, block, decoder->bytes);
        return code;
    }
    decoder->block = block;
    return IMAGE_WUFFS_OK;
}

static void decoder_close(const image_wuffs_allocator* host, image_wuffs_decoder* decoder) {
    if (decoder->block != 0) {
        host->release(host->ctx, decoder->block, decoder->bytes);
        decoder->block = 0;
    }
}

// Bits per pixel of the file. The decoder has read the header by now, so a
// PNG's IHDR is whole and valid.
static uint32_t depth_of(const image_wuffs_decoder* decoder, const uint8_t* file,
                         size_t bytes) {
    if (decoder->format == IMAGE_WUFFS_FORMAT_PNG) {
        static const uint8_t channels[7] = {1, 0, 3, 1, 2, 0, 4};
        uint8_t colour_type = file[25];
        return (uint32_t)file[24] * ((colour_type < 7u) ? channels[colour_type] : 0u);
    }
    image_wuffs_jpeg_frame frame;
    jpeg_find_frame(file, bytes, &frame);
    if (frame.marker != 0) {
        return (uint32_t)frame.precision * frame.components;
    }
    return 8u * image_wuffs_jpeg_components((const wuffs_jpeg__decoder*)decoder->block);
}

static image_wuffs_status read_config(image_wuffs_decoder* decoder, image_wuffs_input* input,
                                      const image_wuffs_progress* progress,
                                      image_wuffs_info* info) {
    wuffs_base__image_config config = wuffs_base__null_image_config();
    for (;;) {
        if (!step_allowed(progress)) {
            return IMAGE_WUFFS_STOPPED;
        }
        wuffs_base__status status = decoder_config(decoder, &config, &input->io);
        if (status.repr == 0) {
            break;
        }
        if (status.repr == wuffs_base__suspension__short_read) {
            int more = input_more(input);
            if (more == INPUT_MORE) {
                continue;
            }
            return (more == INPUT_STUCK) ? IMAGE_WUFFS_UNSUPPORTED : IMAGE_WUFFS_TRUNCATED;
        }
        image_wuffs_status code = status_of(status);
        if ((code == IMAGE_WUFFS_TOO_LARGE) && (decoder->format == IMAGE_WUFFS_FORMAT_JPEG)) {
            // The JPEG decoder reports a width of 0 as a size it cannot take.
            image_wuffs_jpeg_frame frame;
            jpeg_find_frame(input->file, input->bytes, &frame);
            if ((frame.marker != 0) && (frame.width == 0)) {
                code = IMAGE_WUFFS_CORRUPT;
            }
        }
        return code;
    }
    uint32_t width = wuffs_base__pixel_config__width(&config.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&config.pixcfg);
    image_wuffs_status code = check_dimensions(width, height);
    if (code != IMAGE_WUFFS_OK) {
        return code;
    }
    info->format = decoder->format;
    info->width = width;
    info->height = height;
    info->depth = depth_of(decoder, input->file, input->bytes);
    info->alpha = !wuffs_base__image_config__first_frame_is_opaque(&config);
    return IMAGE_WUFFS_OK;
}

// Where the input of a decode has got to.
enum { FEEDING_FILE, END_MARKER_GIVEN, INPUT_CLOSED };

// A JPEG cut inside a scan gets an end of image marker after its last byte,
// in place of the input being closed: the decoder then reads the rest of
// the scan as zero bits, so a scan whose data was all there ends from the
// file's own bits. A closed input stops it before the last blocks instead.
// Returns false when more than a few bytes remain unread, which the decoder
// never leaves inside a scan.
static int input_end_marker(image_wuffs_input* input) {
    wuffs_base__io_buffer* io = &input->io;
    size_t unread = io->meta.wi - io->meta.ri;
    if (unread > sizeof input->tail - 2u) {
        return 0;
    }
    for (size_t i = 0; i < unread; ++i) {
        input->tail[i] = io->data.ptr[io->meta.ri + i];
    }
    input->tail[unread] = 0xFFu;
    input->tail[unread + 1u] = 0xD9u;
    uint64_t position = wuffs_base__u64__sat_add(io->meta.pos, io->meta.ri);
    io->data = wuffs_base__make_slice_u8(input->tail, unread + 2u);
    io->meta = wuffs_base__make_io_buffer_meta(unread + 2u, 0, position, false);
    return 1;
}

// Where a JPEG's first scan stood when the file ran out: the rows above
// `rows` were decoded from the file's bytes, and below them the decoder may
// have made do with zero bits. `may_end_scan` tells whether the file can
// hold all of the scan.
typedef struct image_wuffs_jpeg_cut {
    uint32_t rows;
    int may_end_scan;
} image_wuffs_jpeg_cut;

// What a JPEG cut inside its first scan left in the pixels.
static image_wuffs_status jpeg_after_cut(const wuffs_jpeg__decoder* jpeg,
                                         image_wuffs_jpeg_cut cut, uint8_t* pixels,
                                         const image_wuffs_info* info,
                                         const image_wuffs_progress* progress) {
    if (cut.may_end_scan && (image_wuffs_jpeg_scans(jpeg) != 0u) &&
        image_wuffs_jpeg_scan_within_file(jpeg)) {
        // Only what follows the scan is missing.
        return image_wuffs_jpeg_more_scans(jpeg) ? IMAGE_WUFFS_PARTIAL : IMAGE_WUFFS_OK;
    }
    if (cut.rows == 0u) {
        return IMAGE_WUFFS_TRUNCATED;
    }
    size_t row_bytes = (size_t)info->width * 4u;
    image_wuffs_status code = clear_bytes(pixels + (size_t)cut.rows * row_bytes,
                                          (size_t)(info->height - cut.rows) * row_bytes,
                                          progress);
    return (code == IMAGE_WUFFS_OK) ? IMAGE_WUFFS_PARTIAL : code;
}

// What a decode that ended in an error left in the pixels.
static image_wuffs_status frame_error(const image_wuffs_decoder* decoder,
                                      wuffs_base__status status, int state,
                                      image_wuffs_jpeg_cut cut,
                                      uint8_t* pixels, const image_wuffs_info* info,
                                      const image_wuffs_progress* progress) {
    if (decoder->format == IMAGE_WUFFS_FORMAT_PNG) {
        if (is_truncation(status) &&
            image_wuffs_png_wrote_rows((const wuffs_png__decoder*)decoder->block)) {
            return IMAGE_WUFFS_PARTIAL;
        }
        return status_of(status);
    }
    const wuffs_jpeg__decoder* jpeg = (const wuffs_jpeg__decoder*)decoder->block;
    if ((status.repr == wuffs_jpeg__error__unsupported_scan_count) ||
        (status.repr == wuffs_jpeg__error__bad_scan_count)) {
        // The decoder converted the scans it took before failing.
        return (image_wuffs_jpeg_scans(jpeg) != 0u) ? IMAGE_WUFFS_PARTIAL : status_of(status);
    }
    if ((state != FEEDING_FILE) && ((status.repr == wuffs_jpeg__error__truncated_input) ||
                                    (status.repr == wuffs_jpeg__error__short_sos_bitstream))) {
        return jpeg_after_cut(jpeg, cut, pixels, info, progress);
    }
    return status_of(status);
}

static image_wuffs_status run_frame(image_wuffs_decoder* decoder, image_wuffs_input* input,
                                    wuffs_base__pixel_buffer* buffer,
                                    wuffs_base__slice_u8 workbuf, uint8_t* pixels,
                                    const image_wuffs_info* info,
                                    const image_wuffs_progress* progress) {
    const wuffs_jpeg__decoder* jpeg = (decoder->format == IMAGE_WUFFS_FORMAT_JPEG)
                                          ? (const wuffs_jpeg__decoder*)decoder->block
                                          : 0;
    int state = FEEDING_FILE;
    image_wuffs_jpeg_cut cut = {0, 0};
    for (;;) {
        if (!step_allowed(progress)) {
            return IMAGE_WUFFS_STOPPED;
        }
        wuffs_base__status status = decoder_frame(decoder, buffer, &input->io, workbuf);
        if (status.repr == 0) {
            if (state == END_MARKER_GIVEN) {
                return jpeg_after_cut(jpeg, cut, pixels, info, progress);
            }
            return IMAGE_WUFFS_OK;
        }
        if (status.repr != wuffs_base__suspension__short_read) {
            return frame_error(decoder, status, state, cut, pixels, info, progress);
        }
        if (state == FEEDING_FILE) {
            int more = input_more(input);
            if (more == INPUT_MORE) {
                continue;
            }
            if (more == INPUT_STUCK) {
                return IMAGE_WUFFS_UNSUPPORTED;
            }
        }
        if ((state == FEEDING_FILE) && (jpeg != 0)) {
            if (image_wuffs_jpeg_scans(jpeg) != 0u) {
                // Every finished scan is in the pixels already. A picture of
                // one scan is whole; after more, others could have followed.
                return image_wuffs_jpeg_more_scans(jpeg) ? IMAGE_WUFFS_PARTIAL
                                                         : IMAGE_WUFFS_OK;
            }
            if (image_wuffs_jpeg_in_scan_data(jpeg) && input_end_marker(input)) {
                cut.rows = image_wuffs_jpeg_rows_before_cut(jpeg, info->height);
                cut.may_end_scan = image_wuffs_jpeg_cut_may_end_scan(jpeg);
                state = END_MARKER_GIVEN;
                continue;
            }
        }
        if (state == INPUT_CLOSED) {
            return IMAGE_WUFFS_TRUNCATED;
        }
        // The whole file has been read: tell the decoder there is no more,
        // so it ends the frame with what it has.
        input->io.meta.closed = true;
        state = INPUT_CLOSED;
    }
}

static image_wuffs_status decode_first_frame(image_wuffs_decoder* decoder,
                                             image_wuffs_input* input,
                                             const image_wuffs_allocator* work,
                                             uint8_t* pixels, size_t pixel_bytes,
                                             const image_wuffs_info* info,
                                             const image_wuffs_progress* progress) {
    uint64_t needed = (uint64_t)info->width * (uint64_t)info->height * 4u;
    if ((needed > (uint64_t)pixel_bytes) || (pixels == 0)) {
        return IMAGE_WUFFS_BUFFER_TOO_SMALL;
    }
    wuffs_base__range_ii_u64 range = decoder_workbuf_len(decoder);
    if (wuffs_base__range_ii_u64__is_empty(&range)) {
        return IMAGE_WUFFS_UNSUPPORTED;
    }
#if SIZE_MAX < UINT64_MAX
    if (range.max_incl > (uint64_t)SIZE_MAX) {
        return IMAGE_WUFFS_TOO_LARGE;
    }
#endif
    size_t work_bytes = (size_t)range.max_incl;
    void* work_block = 0;
    if (work_bytes != 0) {
        work_block = work->alloc(work->ctx, work_bytes);
        if (work_block == 0) {
            return IMAGE_WUFFS_NO_MEMORY;
        }
    }

    wuffs_base__pixel_config config = wuffs_base__null_pixel_config();
    wuffs_base__pixel_config__set(&config, WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL,
                                  WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, info->width,
                                  info->height);
    wuffs_base__pixel_buffer buffer = wuffs_base__null_pixel_buffer();
    wuffs_base__status status = wuffs_base__pixel_buffer__set_from_slice(
        &buffer, &config, wuffs_base__make_slice_u8(pixels, (size_t)needed));
    image_wuffs_status code = status_of(status);
    if (code == IMAGE_WUFFS_OK) {
        // A picture cut short must never show what the buffer held before.
        code = clear_bytes(pixels, (size_t)needed, progress);
    }
    if (code == IMAGE_WUFFS_OK) {
        wuffs_base__slice_u8 workbuf =
            wuffs_base__make_slice_u8((uint8_t*)work_block, work_bytes);
        code = run_frame(decoder, input, &buffer, workbuf, pixels, info, progress);
    }
    if (work_block != 0) {
        work->release(work->ctx, work_block, work_bytes);
    }
    return code;
}

image_wuffs_status image_wuffs_probe(const uint8_t* file, size_t bytes,
                                     const image_wuffs_allocator* host,
                                     image_wuffs_info* out) {
    if ((host == 0) || (host->alloc == 0) || (host->release == 0) || (out == 0) ||
        ((file == 0) && (bytes != 0))) {
        return IMAGE_WUFFS_CORRUPT;
    }
    out->format = IMAGE_WUFFS_FORMAT_UNKNOWN;
    out->width = 0;
    out->height = 0;
    out->depth = 0;
    out->alpha = false;
    image_wuffs_format format = format_of(file, bytes);
    if (format == IMAGE_WUFFS_FORMAT_UNKNOWN) {
        return IMAGE_WUFFS_NOT_THIS_FORMAT;
    }
    image_wuffs_decoder decoder;
    image_wuffs_status code = decoder_open(host, format, &decoder);
    if (code == IMAGE_WUFFS_OK) {
        image_wuffs_input input;
        input_init(&input, file, bytes, format);
        code = read_config(&decoder, &input, 0, out);
        decoder_close(host, &decoder);
    }
    return code;
}

image_wuffs_status image_wuffs_decode(const uint8_t* file, size_t bytes,
                                      const image_wuffs_allocator* host,
                                      const image_wuffs_allocator* work, uint8_t* pixels,
                                      size_t pixel_bytes,
                                      const image_wuffs_progress* progress) {
    if ((host == 0) || (work == 0) || (host->alloc == 0) || (host->release == 0) ||
        (work->alloc == 0) || (work->release == 0) ||
        ((file == 0) && (bytes != 0))) {
        return IMAGE_WUFFS_CORRUPT;
    }
    image_wuffs_format format = format_of(file, bytes);
    if (format == IMAGE_WUFFS_FORMAT_UNKNOWN) {
        return IMAGE_WUFFS_NOT_THIS_FORMAT;
    }
    image_wuffs_decoder decoder;
    image_wuffs_status code = decoder_open(host, format, &decoder);
    if (code != IMAGE_WUFFS_OK) {
        return code;
    }
    image_wuffs_info info = {IMAGE_WUFFS_FORMAT_UNKNOWN, 0, 0, 0, false};
    image_wuffs_input input;
    input_init(&input, file, bytes, format);
    code = read_config(&decoder, &input, progress, &info);
    if (code == IMAGE_WUFFS_OK) {
        code = decode_first_frame(&decoder, &input, work, pixels, pixel_bytes, &info, progress);
    }
    decoder_close(host, &decoder);
    return code;
}
