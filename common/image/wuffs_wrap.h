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

// The C side of common/image/decode.hpp: Wuffs decodes PNG and JPEG through
// this wrapper, which keeps the third-party types out of the C++ code.

#ifndef HOMBRE_IMAGE_WUFFS_WRAP_H
#define HOMBRE_IMAGE_WUFFS_WRAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum image_wuffs_format {
    IMAGE_WUFFS_FORMAT_UNKNOWN = 0,
    IMAGE_WUFFS_FORMAT_PNG = 1,
    IMAGE_WUFFS_FORMAT_JPEG = 2,
} image_wuffs_format;

typedef enum image_wuffs_status {
    IMAGE_WUFFS_OK = 0,
    IMAGE_WUFFS_PARTIAL,
    IMAGE_WUFFS_NOT_THIS_FORMAT,
    IMAGE_WUFFS_TRUNCATED,
    IMAGE_WUFFS_CORRUPT,
    IMAGE_WUFFS_UNSUPPORTED,
    IMAGE_WUFFS_TOO_LARGE,
    IMAGE_WUFFS_BUFFER_TOO_SMALL,
    IMAGE_WUFFS_NO_MEMORY,
    IMAGE_WUFFS_STOPPED,
} image_wuffs_status;

typedef struct image_wuffs_allocator {
    void* ctx;
    void* (*alloc)(void* ctx, size_t bytes);
    void (*release)(void* ctx, void* block, size_t bytes);
} image_wuffs_allocator;

typedef struct image_wuffs_progress {
    void* ctx;
    bool (*keep_going)(void* ctx);
} image_wuffs_progress;

typedef struct image_wuffs_info {
    image_wuffs_format format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    bool alpha;
} image_wuffs_info;

image_wuffs_status image_wuffs_probe(const uint8_t* file, size_t bytes,
                                     const image_wuffs_allocator* host,
                                     image_wuffs_info* out);

image_wuffs_status image_wuffs_decode(const uint8_t* file, size_t bytes,
                                      const image_wuffs_allocator* host,
                                      const image_wuffs_allocator* work, uint8_t* pixels,
                                      size_t pixel_bytes,
                                      const image_wuffs_progress* progress);

#ifdef __cplusplus
}
#endif

#endif
