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

#include "types.hpp"

namespace hombre {

// Read-only access to an Amiga OFS or FFS floppy image held in memory: 512
// byte blocks, big endian, a root block in the middle of the disk, and
// directories that are hash tables of header blocks chained through the
// header that follows in the same bucket. Every block number that is read is
// checked against the image, every header checksum is verified, and every
// chain is bounded, so a damaged image gives an error and never a stray read
// or a walk that does not end. Nothing is allocated and nothing is kept
// between calls.

enum class DiskFormat : u32 { Unknown, Dms, Adf };

// "DMS!" at the start is an archive; 901120 (DD) or 1802240 (HD) bytes is a
// raw image.
inline DiskFormat detect_disk_format(const u8* data, usize bytes) {
    if (bytes >= 4 && data[0] == 'D' && data[1] == 'M' && data[2] == 'S' && data[3] == '!') {
        return DiskFormat::Dms;
    }
    if (bytes == 901120 || bytes == 1802240) {
        return DiskFormat::Adf;
    }
    return DiskFormat::Unknown;
}

enum class FsError : u32 {
    Ok,
    ImageSize,      // not a DD or HD floppy image
    NotDos,         // the bootblock does not start with "DOS"
    UnsupportedFs,  // a DOS type this reader does not know
    BadChecksum,    // a header block fails its checksum
    BadBlock,       // a block number or a field of a block is out of range
    NotFound,
    NotFile,        // a read of something that is not a file
    NotDir,         // a path through, or a listing of, something that is not a directory
    Loop,           // a hash or extension chain that never ends
    NameTooLong,    // a path component of more than 30 characters
};

inline const char* fs_error_name(FsError error) {
    switch (error) {
    case FsError::Ok: return "ok";
    case FsError::ImageSize: return "not a floppy image size";
    case FsError::NotDos: return "not a DOS disk";
    case FsError::UnsupportedFs: return "unsupported file system";
    case FsError::BadChecksum: return "bad block checksum";
    case FsError::BadBlock: return "bad block";
    case FsError::NotFound: return "not found";
    case FsError::NotFile: return "not a file";
    case FsError::NotDir: return "not a directory";
    case FsError::Loop: return "chain loops";
    case FsError::NameTooLong: return "name too long";
    }
    return "unknown";
}

struct AdfVolume {
    const u8* image;
    u32 block_count;
    u32 root_block;
    bool ffs;
    // Names fold the Latin-1 letters as well as ASCII. Set for the INTL
    // types and for the directory cache types, which always do.
    bool intl;
    bool bootblock_checksum_ok;
    char name[32];
};

struct DirEntry {
    char name[32];
    bool is_dir;
    u32 size;
    u32 header_block;
    u8 volume;
};

using DirVisitor = void (*)(void* ctx, const DirEntry& entry);

namespace amiga_fs_detail {

inline constexpr u32 block_bytes = 512;
inline constexpr u32 block_longs = 128;
inline constexpr u32 hash_slots = 72;
inline constexpr u32 name_max = 30;

// Long indices; the trailing ones count back from the end of the block.
inline constexpr u32 l_type = 0;
inline constexpr u32 l_own = 1;
inline constexpr u32 l_count = 2;  // data pointers in this block
inline constexpr u32 l_table = 6;  // the 72 hash table longs of a directory
inline constexpr u32 l_last_pointer = block_longs - 51;  // data pointers run downwards from here
inline constexpr u32 l_size = block_longs - 47;
inline constexpr u32 l_name = block_longs - 20;  // length byte, then the name
inline constexpr u32 l_hash_next = block_longs - 4;
inline constexpr u32 l_parent = block_longs - 3;
inline constexpr u32 l_extension = block_longs - 2;
inline constexpr u32 l_sec_type = block_longs - 1;
// The root block's bitmap: the flag, -1 while the bitmap is valid, the
// first 25 bitmap block pointers and the first extension block of the rest.
inline constexpr u32 l_bm_flag = 78;
inline constexpr u32 l_bm_pages = 79;
inline constexpr u32 l_bm_ext = 104;
inline constexpr u32 bm_valid = 0xFFFFFFFFu;
inline constexpr u32 bm_pages_max = 25;
// A bitmap block is a checksum and 127 longs of bits, one bit a block from
// block 2 on, the lowest bit of each long first; a set bit is a free block.
inline constexpr u32 bm_longs = block_longs - 1;
inline constexpr u32 bm_span = bm_longs * 32;
// A bitmap extension block holds 127 more page pointers and the next
// extension block in its last long.
inline constexpr u32 bm_ext_pages = block_longs - 1;
// The bitmap pages the root block's own pointers and one extension block
// hold; a larger volume reports every block used.
inline constexpr u32 bm_page_slots = bm_pages_max + bm_ext_pages;

inline constexpr u32 t_header = 2;
inline constexpr u32 t_data = 8;
inline constexpr u32 t_list = 16;
inline constexpr u32 st_root = 1;
inline constexpr u32 st_dir = 2;
inline constexpr u32 st_file = 0xFFFFFFFDu;  // -3

inline constexpr u32 ofs_header_bytes = 24;

inline u32 be32(const u8* p) {
    return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | u32{p[3]};
}

inline u32 long_at(const u8* block, u32 index) { return be32(block + index * 4); }

// The block, or null when the number is not one a header or data block can
// have: the first two blocks are the bootblock.
inline const u8* block_at(const AdfVolume& v, u32 number) {
    if (number < 2 || number >= v.block_count) {
        return nullptr;
    }
    return v.image + static_cast<usize>(number) * block_bytes;
}

inline bool block_sums_to_zero(const u8* block) {
    u32 sum = 0;
    for (u32 i = 0; i < block_longs; ++i) {
        sum += long_at(block, i);
    }
    return sum == 0;
}

// The bootblock checksum adds with the carry brought round, over both
// blocks, with the stored sum counted as zero, and stores the complement.
inline bool bootblock_sum_ok(const u8* image) {
    u32 sum = 0;
    for (u32 i = 0; i < 2 * block_longs; ++i) {
        if (i == 1) {
            continue;
        }
        const u32 d = long_at(image, i);
        if (0xFFFFFFFFu - sum < d) {
            ++sum;
        }
        sum += d;
    }
    return ~sum == long_at(image, 1);
}

inline u8 fold(u8 c, bool intl) {
    if (c >= 'a' && c <= 'z') {
        return static_cast<u8>(c - 32);
    }
    if (intl && c >= 224 && c <= 254 && c != 247) {
        return static_cast<u8>(c - 32);
    }
    return c;
}

inline u32 hash_name(const u8* name, u32 length, bool intl) {
    u32 h = length;
    for (u32 i = 0; i < length; ++i) {
        h = (h * 13 + fold(name[i], intl)) & 0x7FF;
    }
    return h % hash_slots;
}

inline bool name_matches(const u8* block, const u8* name, u32 length, bool intl) {
    if (block[l_name * 4] != length) {
        return false;
    }
    for (u32 i = 0; i < length; ++i) {
        if (fold(block[l_name * 4 + 1 + i], intl) != fold(name[i], intl)) {
            return false;
        }
    }
    return true;
}

// A file, directory or root header: checksum, type and own block number.
// The secondary type is left to the caller.
inline const u8* load_header(const AdfVolume& v, u32 number, FsError& error) {
    const u8* block = block_at(v, number);
    if (block == nullptr) {
        error = FsError::BadBlock;
        return nullptr;
    }
    if (!block_sums_to_zero(block)) {
        error = FsError::BadChecksum;
        return nullptr;
    }
    // The root block leaves its own number out; every other header has it.
    if (long_at(block, l_type) != t_header || block[l_name * 4] > name_max ||
        (long_at(block, l_sec_type) != st_root && long_at(block, l_own) != number)) {
        error = FsError::BadBlock;
        return nullptr;
    }
    return block;
}

inline bool is_dir_type(u32 sec_type) { return sec_type == st_root || sec_type == st_dir; }

inline void fill_entry(const u8* header, u32 number, DirEntry& out) {
    const u32 length = header[l_name * 4];
    for (u32 i = 0; i < length; ++i) {
        out.name[i] = static_cast<char>(header[l_name * 4 + 1 + i]);
    }
    out.name[length] = '\0';
    out.is_dir = is_dir_type(long_at(header, l_sec_type));
    out.size = out.is_dir ? 0 : long_at(header, l_size);
    out.header_block = number;
    out.volume = 0;
}

// The directory header `dir` and a name: walks the bucket's chain. The chain
// can pass through at most every block of the disk.
inline FsError find_in_dir(const AdfVolume& v, const u8* dir, const u8* name, u32 length,
                           DirEntry& out) {
    u32 number = long_at(dir, l_table + hash_name(name, length, v.intl));
    for (u32 step = 0; number != 0; ++step) {
        if (step >= v.block_count) {
            return FsError::Loop;
        }
        FsError error = FsError::Ok;
        const u8* header = load_header(v, number, error);
        if (header == nullptr) {
            return error;
        }
        if (name_matches(header, name, length, v.intl)) {
            fill_entry(header, number, out);
            return FsError::Ok;
        }
        number = long_at(header, l_hash_next);
    }
    return FsError::NotFound;
}

}

inline FsError adf_mount(const u8* image, usize bytes, AdfVolume& volume) {
    using namespace amiga_fs_detail;
    if (bytes != 901120 && bytes != 1802240) {
        return FsError::ImageSize;
    }
    if (image[0] != 'D' || image[1] != 'O' || image[2] != 'S') {
        return FsError::NotDos;
    }
    // Bit 0 is FFS, bit 1 INTL, bit 2 the directory cache, which leaves the
    // hash table in place and adds blocks this reader does not look at. The
    // long name types (6 and 7) and anything above are not read.
    const u8 flags = image[3];
    if (flags > 5) {
        return FsError::UnsupportedFs;
    }
    volume.image = image;
    volume.block_count = static_cast<u32>(bytes / block_bytes);
    volume.root_block = volume.block_count / 2;
    volume.ffs = (flags & 1) != 0;
    volume.intl = (flags & 6) != 0;
    volume.bootblock_checksum_ok = bootblock_sum_ok(image);
    volume.name[0] = '\0';

    FsError error = FsError::Ok;
    const u8* root = load_header(volume, volume.root_block, error);
    if (root == nullptr) {
        return error;
    }
    if (long_at(root, l_sec_type) != st_root || long_at(root, 3) != hash_slots) {
        return FsError::BadBlock;
    }
    DirEntry entry;
    fill_entry(root, volume.root_block, entry);
    u32 i = 0;
    for (; entry.name[i] != '\0'; ++i) {
        volume.name[i] = entry.name[i];
    }
    volume.name[i] = '\0';
    return FsError::Ok;
}

// How many of the volume's blocks are in use, counted from the bitmap its
// root block names: every block from 2 to block_count - 1 has a bit, set
// while the block is free, and the root block and the bitmap blocks
// themselves always count as used. A volume whose bitmap is not marked
// valid, with bitmap pages that are not blocks of the volume, or with more
// pages than the root block and one extension block hold, reports every
// block used.
inline u32 adf_used_blocks(const AdfVolume& volume) {
    using namespace amiga_fs_detail;
    if (volume.block_count < 3) {
        return 0;
    }
    const u32 blocks = volume.block_count - 2;
    const u8* root = block_at(volume, volume.root_block);
    if (root == nullptr || long_at(root, l_bm_flag) != bm_valid) {
        return blocks;
    }
    const u32 pages_needed = (blocks - 1) / bm_span + 1;
    if (pages_needed > bm_page_slots) {
        return blocks;
    }
    u32 pages[bm_page_slots]{};
    for (u32 i = 0; i < pages_needed && i < bm_pages_max; ++i) {
        pages[i] = long_at(root, l_bm_pages + i);
    }
    if (pages_needed > bm_pages_max) {
        const u8* extension = block_at(volume, long_at(root, l_bm_ext));
        if (extension == nullptr) {
            return blocks;
        }
        for (u32 i = bm_pages_max; i < pages_needed; ++i) {
            pages[i] = long_at(extension, i - bm_pages_max);
        }
    }
    for (u32 i = 0; i < pages_needed; ++i) {
        if (block_at(volume, pages[i]) == nullptr) {
            return blocks;
        }
    }
    const auto is_free = [&](u32 block) {
        const u32 index = block - 2;
        const u8* page = block_at(volume, pages[index / bm_span]);
        const u32 bit = index % bm_span;
        return (long_at(page, 1 + bit / 32) & (1u << (bit % 32))) != 0;
    };
    u32 used = 0;
    for (u32 block = 2; block < volume.block_count; ++block) {
        if (!is_free(block)) {
            ++used;
        }
    }
    // The root and the bitmap blocks are in use even when their own bits
    // say otherwise.
    if (volume.root_block >= 2 && is_free(volume.root_block)) {
        ++used;
    }
    for (u32 i = 0; i < pages_needed; ++i) {
        if (pages[i] != volume.root_block && pages[i] >= 2 && is_free(pages[i])) {
            ++used;
        }
    }
    return used;
}

// Looks a path up: components separated by '/', names compared without
// regard to case, an optional leading "Volume:" (any case, and empty is the
// volume itself), and "" for the root. Empty components are skipped.
inline FsError adf_find(const AdfVolume& volume, const char* path, DirEntry& out) {
    using namespace amiga_fs_detail;
    const u8* p = reinterpret_cast<const u8*>(path);

    // A colon before the first slash ends a volume name.
    u32 prefix = 0;
    while (p[prefix] != '\0' && p[prefix] != '/' && p[prefix] != ':') {
        ++prefix;
    }
    if (p[prefix] == ':') {
        if (prefix > name_max) {
            return FsError::NameTooLong;
        }
        const u8* volume_name = reinterpret_cast<const u8*>(volume.name);
        u32 length = 0;
        while (volume.name[length] != '\0') {
            ++length;
        }
        if (prefix != 0) {
            if (prefix != length) {
                return FsError::NotFound;
            }
            for (u32 i = 0; i < length; ++i) {
                if (fold(p[i], volume.intl) != fold(volume_name[i], volume.intl)) {
                    return FsError::NotFound;
                }
            }
        }
        p += prefix + 1;
    }

    FsError error = FsError::Ok;
    const u8* root = load_header(volume, volume.root_block, error);
    if (root == nullptr) {
        return error;
    }
    fill_entry(root, volume.root_block, out);

    for (;;) {
        while (*p == '/') {
            ++p;
        }
        if (*p == '\0') {
            return FsError::Ok;
        }
        u32 length = 0;
        while (p[length] != '\0' && p[length] != '/') {
            ++length;
        }
        if (length > name_max) {
            return FsError::NameTooLong;
        }
        if (!out.is_dir) {
            return FsError::NotDir;
        }
        const u8* dir = load_header(volume, out.header_block, error);
        if (dir == nullptr) {
            return error;
        }
        const FsError found = find_in_dir(volume, dir, p, length, out);
        if (found != FsError::Ok) {
            return found;
        }
        p += length;
    }
}

// Calls the visitor once for each entry of the directory at `path`, in hash
// table order. A bucket whose chain is damaged reports nothing from that
// bucket, but earlier buckets have been reported by then.
inline FsError adf_list(const AdfVolume& volume, const char* path, DirVisitor visitor, void* ctx) {
    using namespace amiga_fs_detail;
    DirEntry dir_entry;
    FsError error = adf_find(volume, path, dir_entry);
    if (error != FsError::Ok) {
        return error;
    }
    if (!dir_entry.is_dir) {
        return FsError::NotDir;
    }
    const u8* dir = load_header(volume, dir_entry.header_block, error);
    if (dir == nullptr) {
        return error;
    }
    for (u32 slot = 0; slot < hash_slots; ++slot) {
        const u32 first = long_at(dir, l_table + slot);
        // First walk the whole chain to see that it is sound, then report it.
        u32 count = 0;
        for (u32 number = first; number != 0;) {
            if (count >= volume.block_count) {
                return FsError::Loop;
            }
            const u8* header = load_header(volume, number, error);
            if (header == nullptr) {
                return error;
            }
            ++count;
            number = long_at(header, l_hash_next);
        }
        u32 number = first;
        for (u32 i = 0; i < count; ++i) {
            const u8* header = block_at(volume, number);
            DirEntry entry;
            fill_entry(header, number, entry);
            if (visitor != nullptr) {
                visitor(ctx, entry);
            }
            number = long_at(header, l_hash_next);
        }
    }
    return FsError::Ok;
}

namespace amiga_fs_detail {

// A block that holds data pointers: the file header, or an extension block
// reached through the chain that starts there.
struct PointerBlocks {
    const u8* table;  // the block pointers are being read from
    u32 block;        // its number
    u32 header;       // the file header's number
    u32 hops;         // extension blocks moved over so far
    // Brent's cycle detection: the block remembered at a power of two hops.
    u32 mark;
    u32 power;
    u32 since_mark;
};

inline FsError start_pointer_blocks(PointerBlocks& w, const u8* header, u32 header_number) {
    if (long_at(header, l_count) > hash_slots) {
        return FsError::BadBlock;
    }
    w.table = header;
    w.block = header_number;
    w.header = header_number;
    w.hops = 0;
    w.mark = header_number;
    w.power = 1;
    w.since_mark = 0;
    return FsError::Ok;
}

// Moves to the next extension block of the file.
inline FsError next_pointer_block(const AdfVolume& v, PointerBlocks& w) {
    const u32 next = long_at(w.table, l_extension);
    if (next == 0) {
        return FsError::BadBlock;  // the file claims more data than it has pointers for
    }
    const u8* block = block_at(v, next);
    if (block == nullptr) {
        return FsError::BadBlock;
    }
    if (!block_sums_to_zero(block)) {
        return FsError::BadChecksum;
    }
    if (long_at(block, l_type) != t_list || long_at(block, l_own) != next ||
        long_at(block, l_count) > hash_slots || long_at(block, l_parent) != w.header) {
        return FsError::BadBlock;
    }
    if (next == w.block || next == w.mark || ++w.hops > v.block_count) {
        return FsError::Loop;
    }
    w.table = block;
    w.block = next;
    if (++w.since_mark == w.power) {
        w.mark = next;
        w.power *= 2;
        w.since_mark = 0;
    }
    return FsError::Ok;
}

}

// Reads up to `capacity` bytes of a file from `offset`. `read` is the number
// of bytes stored, less than `capacity` only at the end of the file. The
// entry is looked at again, so a stale or damaged one is an error.
//
// An OFS data block must say it is a data block, or the read fails with
// BadBlock. Its checksum, its header key, its sequence number and its data
// size (at most 488) are only counted: real disks carry blocks that fail
// each of the first three on their own (a patched block, a file made of
// another file's tail, a copy that kept an old key) and still read right.
// `inconsistent`, when given, receives the number of blocks read that failed
// any of the four, so a caller can warn or refuse.
inline FsError adf_read(const AdfVolume& volume, const DirEntry& file, u32 offset, u8* out,
                        u32 capacity, u32& read, u32* inconsistent = nullptr) {
    using namespace amiga_fs_detail;
    read = 0;
    u32 suspect = 0;
    if (inconsistent != nullptr) {
        *inconsistent = 0;
    }
    FsError error = FsError::Ok;
    const u8* header = load_header(volume, file.header_block, error);
    if (header == nullptr) {
        return error;
    }
    if (long_at(header, l_sec_type) != st_file) {
        return FsError::NotFile;
    }
    const u32 size = long_at(header, l_size);
    if (offset >= size || capacity == 0) {
        return FsError::Ok;
    }
    const u32 want = (size - offset < capacity) ? size - offset : capacity;
    const u32 data_offset = volume.ffs ? 0 : ofs_header_bytes;
    const u32 payload = volume.ffs ? block_bytes : block_bytes - ofs_header_bytes;

    PointerBlocks blocks;
    error = start_pointer_blocks(blocks, header, file.header_block);
    if (error != FsError::Ok) {
        return error;
    }
    const u32 first_block = offset / payload;
    for (u32 skip = first_block / hash_slots; skip > 0; --skip) {
        error = next_pointer_block(volume, blocks);
        if (error != FsError::Ok) {
            return error;
        }
    }
    u32 slot = first_block % hash_slots;
    u32 skip_bytes = offset % payload;
    u32 index = first_block;  // the block's place in the file, from 0
    u32 done = 0;
    while (done < want) {
        if (slot == hash_slots) {
            error = next_pointer_block(volume, blocks);
            if (error != FsError::Ok) {
                return error;
            }
            slot = 0;
        }
        if (slot >= long_at(blocks.table, l_count)) {
            return FsError::BadBlock;  // the size claims more data than the pointers cover
        }
        const u8* data = block_at(volume, long_at(blocks.table, l_last_pointer - slot));
        if (data == nullptr) {
            return FsError::BadBlock;
        }
        if (!volume.ffs) {
            // An OFS data block names its file, its place in it and how
            // much of it is data.
            if (long_at(data, l_type) != t_data) {
                return FsError::BadBlock;
            }
            if (!block_sums_to_zero(data) || long_at(data, l_own) != file.header_block ||
                long_at(data, 2) != index + 1 || long_at(data, 3) > payload) {
                ++suspect;
            }
        }
        u32 chunk = payload - skip_bytes;
        if (want - done < chunk) {
            chunk = want - done;
        }
        __builtin_memcpy(out + done, data + data_offset + skip_bytes, chunk);
        done += chunk;
        skip_bytes = 0;
        ++slot;
        ++index;
    }
    read = done;
    if (inconsistent != nullptr) {
        *inconsistent = suspect;
    }
    return FsError::Ok;
}

}
