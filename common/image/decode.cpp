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

#include "decode.hpp"
#include "wuffs_wrap.h"

namespace hombre::image {
namespace {

void* alloc_thunk(void* ctx, size_t bytes) {
    const Allocator& allocator = *static_cast<const Allocator*>(ctx);
    return allocator.alloc(allocator.ctx, bytes);
}

void release_thunk(void* ctx, void* block, size_t bytes) {
    const Allocator& allocator = *static_cast<const Allocator*>(ctx);
    allocator.release(allocator.ctx, block, bytes);
}

bool progress_thunk(void* ctx) {
    const Progress& progress = *static_cast<const Progress*>(ctx);
    return progress.keep_going(progress.ctx);
}

image_wuffs_allocator to_c(const Allocator& allocator) {
    image_wuffs_allocator converted;
    converted.ctx = const_cast<Allocator*>(&allocator);
    converted.alloc = alloc_thunk;
    converted.release = release_thunk;
    return converted;
}

bool usable(const Allocator& allocator) {
    return allocator.alloc != nullptr && allocator.release != nullptr;
}

Status to_status(image_wuffs_status status) {
    switch (status) {
    case IMAGE_WUFFS_OK: return Status::Ok;
    case IMAGE_WUFFS_PARTIAL: return Status::Partial;
    case IMAGE_WUFFS_NOT_THIS_FORMAT: return Status::NotThisFormat;
    case IMAGE_WUFFS_TRUNCATED: return Status::Truncated;
    case IMAGE_WUFFS_CORRUPT: return Status::Corrupt;
    case IMAGE_WUFFS_UNSUPPORTED: return Status::Unsupported;
    case IMAGE_WUFFS_TOO_LARGE: return Status::TooLarge;
    case IMAGE_WUFFS_BUFFER_TOO_SMALL: return Status::BufferTooSmall;
    case IMAGE_WUFFS_NO_MEMORY: return Status::NoMemory;
    case IMAGE_WUFFS_STOPPED: return Status::Stopped;
    }
    return Status::Corrupt;
}

Format to_format(image_wuffs_format format) {
    switch (format) {
    case IMAGE_WUFFS_FORMAT_PNG: return Format::Png;
    case IMAGE_WUFFS_FORMAT_JPEG: return Format::Jpeg;
    case IMAGE_WUFFS_FORMAT_UNKNOWN: break;
    }
    return Format::Unknown;
}

}

Status probe(const u8* file, usize bytes, const Allocator& host, Info& out) {
    out = Info{};
    if (!usable(host)) {
        return Status::NoMemory;
    }
    const image_wuffs_allocator host_code = to_c(host);
    image_wuffs_info info{};
    const Status status =
        to_status(image_wuffs_probe(file, bytes, &host_code, &info));
    if (status == Status::Ok) {
        out.format = to_format(info.format);
        out.width = info.width;
        out.height = info.height;
        out.depth = info.depth;
        out.alpha = info.alpha;
    }
    return status;
}

Status decode(const u8* file, usize bytes, const Allocator& host, const Allocator& work,
              u8* pixels, usize pixel_bytes, const Progress& progress) {
    if (!usable(host) || !usable(work)) {
        return Status::NoMemory;
    }
    const image_wuffs_allocator host_code = to_c(host);
    const image_wuffs_allocator work_code = to_c(work);
    image_wuffs_progress progress_code{};
    const image_wuffs_progress* progress_ptr = nullptr;
    if (progress.keep_going != nullptr) {
        progress_code.ctx = const_cast<Progress*>(&progress);
        progress_code.keep_going = progress_thunk;
        progress_ptr = &progress_code;
    }
    return to_status(image_wuffs_decode(file, bytes, &host_code, &work_code, pixels,
                                        pixel_bytes, progress_ptr));
}

}
