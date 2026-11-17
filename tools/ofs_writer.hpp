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

// Host only: builds OFS and FFS floppy images for tests and tools. The
// kernel never includes this file.

#include "types.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace hombre {

struct AdfFile {
    std::string path;  // "Dir/Sub/name"; the directories are created
    std::vector<u8> data;
};

namespace ofs_writer_detail {

inline constexpr u32 block_bytes = 512;
inline constexpr u32 disk_blocks = 1760;
inline constexpr u32 root_block = 880;
inline constexpr u32 hash_slots = 72;
inline constexpr u32 name_max = 30;

inline void put32(std::vector<u8>& image, u32 block, u32 index, u32 value) {
    u8* p = image.data() + static_cast<usize>(block) * block_bytes + index * 4;
    p[0] = static_cast<u8>(value >> 24);
    p[1] = static_cast<u8>(value >> 16);
    p[2] = static_cast<u8>(value >> 8);
    p[3] = static_cast<u8>(value);
}

inline u32 get32(const std::vector<u8>& image, u32 block, u32 index) {
    const u8* p = image.data() + static_cast<usize>(block) * block_bytes + index * 4;
    return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | u32{p[3]};
}

inline u32 upper(u8 c, bool intl) {
    if (c >= 'a' && c <= 'z') {
        return c - 32u;
    }
    if (intl && c >= 224 && c <= 254 && c != 247) {
        return c - 32u;
    }
    return c;
}

inline u32 hash_of(const std::string& name, bool intl) {
    u32 h = static_cast<u32>(name.size());
    for (const char c : name) {
        h = (h * 13 + upper(static_cast<u8>(c), intl)) & 0x7FF;
    }
    return h % hash_slots;
}

inline void put_name(std::vector<u8>& image, u32 block, const std::string& name) {
    if (name.empty() || name.size() > name_max) {
        throw std::invalid_argument("bad name: " + name);
    }
    u8* p = image.data() + static_cast<usize>(block) * block_bytes + 432;
    p[0] = static_cast<u8>(name.size());
    for (usize i = 0; i < name.size(); ++i) {
        p[1 + i] = static_cast<u8>(name[i]);
    }
}

inline std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : path) {
        if (c == '/') {
            if (!current.empty()) {
                parts.push_back(current);
            }
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

}

// Sets a block's checksum long so that its 128 longs add up to zero. Useful
// after a test changes a header on purpose.
inline void adf_fix_checksum(std::vector<u8>& image, u32 block) {
    using namespace ofs_writer_detail;
    put32(image, block, 5, 0);
    u32 sum = 0;
    for (u32 i = 0; i < 128; ++i) {
        sum += get32(image, block, i);
    }
    put32(image, block, 5, 0u - sum);
}

// An 880 KiB disk: a bootblock, the root at block 880 with a hash table of
// the files and directories below it, files with extension blocks when they
// need more than 72 data blocks. No bitmap blocks are written and the root
// says so. `intl` makes the hash fold the Latin-1 letters and sets the INTL
// bit of the bootblock.
inline std::vector<u8> build_adf(const char* volume_name, bool ffs, const std::vector<AdfFile>& files,
                                 bool intl = false) {
    using namespace ofs_writer_detail;
    std::vector<u8> image(static_cast<usize>(disk_blocks) * block_bytes, 0);
    std::vector<u32> headers;  // the blocks that carry a header checksum

    u32 next_free = 2;
    auto allocate = [&]() {
        if (next_free == root_block) {
            ++next_free;
        }
        if (next_free >= disk_blocks) {
            throw std::length_error("disk full");
        }
        return next_free++;
    };

    // The bootblock: "DOS" and the flags, and the sum that makes the whole
    // 1024 bytes add up, with the carry brought round.
    image[0] = 'D';
    image[1] = 'O';
    image[2] = 'S';
    image[3] = static_cast<u8>((ffs ? 1 : 0) | (intl ? 2 : 0));
    image[7] = 0;
    {
        u32 sum = 0;
        for (u32 i = 0; i < 256; ++i) {
            if (i == 1) {
                continue;
            }
            const u32 d = get32(image, 0, i);
            const u64 total = u64{sum} + d;
            sum = static_cast<u32>(total) + static_cast<u32>(total >> 32);
        }
        put32(image, 0, 1, ~sum);
    }

    put32(image, root_block, 0, 2);
    put32(image, root_block, 3, hash_slots);
    put32(image, root_block, 127, 1);
    put_name(image, root_block, volume_name);
    headers.push_back(root_block);

    std::map<std::string, u32> directories;
    directories[""] = root_block;

    // Adds a header to the front of its parent's hash chain.
    auto link = [&](u32 parent, const std::string& name, u32 block) {
        const u32 slot = 6 + hash_of(name, intl);
        put32(image, block, 124, get32(image, parent, slot));
        put32(image, parent, slot, block);
        put32(image, block, 125, parent);
    };

    auto ensure_directory = [&](const std::vector<std::string>& parts, usize count) {
        std::string key;
        u32 parent = root_block;
        for (usize i = 0; i < count; ++i) {
            key += parts[i] + "/";
            auto found = directories.find(key);
            if (found == directories.end()) {
                const u32 block = allocate();
                put32(image, block, 0, 2);
                put32(image, block, 1, block);
                put32(image, block, 127, 2);
                put_name(image, block, parts[i]);
                link(parent, parts[i], block);
                headers.push_back(block);
                found = directories.emplace(key, block).first;
            }
            parent = found->second;
        }
        return parent;
    };

    const u32 payload = ffs ? 512 : 488;
    for (const AdfFile& file : files) {
        const std::vector<std::string> parts = split_path(file.path);
        if (parts.empty()) {
            throw std::invalid_argument("empty path");
        }
        const u32 parent = ensure_directory(parts, parts.size() - 1);
        const u32 header = allocate();
        const u32 size = static_cast<u32>(file.data.size());
        const u32 count = (size + payload - 1) / payload;

        put32(image, header, 0, 2);
        put32(image, header, 1, header);
        put32(image, header, 81, size);
        put32(image, header, 127, 0xFFFFFFFDu);
        put_name(image, header, parts.back());
        link(parent, parts.back(), header);
        headers.push_back(header);

        std::vector<u32> data_blocks;
        for (u32 i = 0; i < count; ++i) {
            data_blocks.push_back(allocate());
        }
        for (u32 i = 0; i < count; ++i) {
            const u32 block = data_blocks[i];
            const u32 chunk = std::min(payload, size - i * payload);
            u8* dst = image.data() + static_cast<usize>(block) * block_bytes;
            if (ffs) {
                std::copy_n(file.data.begin() + i * payload, chunk, dst);
            } else {
                put32(image, block, 0, 8);
                put32(image, block, 1, header);
                put32(image, block, 2, i + 1);
                put32(image, block, 3, chunk);
                put32(image, block, 4, i + 1 < count ? data_blocks[i + 1] : 0);
                std::copy_n(file.data.begin() + i * payload, chunk, dst + 24);
                adf_fix_checksum(image, block);
            }
        }

        // The first 72 pointers live in the header, every further 72 in an
        // extension block, each pointing on to the next. Pointers are stored
        // from long 77 downwards.
        u32 table = header;
        for (u32 first = 0; first < count || first == 0; first += hash_slots) {
            const u32 here = std::min(hash_slots, count - std::min(count, first));
            put32(image, table, 2, here);
            for (u32 j = 0; j < here; ++j) {
                put32(image, table, 77 - j, data_blocks[first + j]);
            }
            if (first == 0) {
                put32(image, header, 4, count > 0 ? data_blocks[0] : 0);
            }
            if (first + hash_slots < count) {
                const u32 ext = allocate();
                put32(image, ext, 0, 16);
                put32(image, ext, 1, ext);
                put32(image, ext, 125, header);
                put32(image, ext, 127, 0xFFFFFFFDu);
                put32(image, table, 126, ext);
                headers.push_back(ext);
                table = ext;
            }
        }
    }

    for (const u32 block : headers) {
        adf_fix_checksum(image, block);
    }
    return image;
}

}
