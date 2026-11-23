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

#include "amiga_fs.hpp"

namespace hombre {

// Write access to the OFS and FFS volumes amiga_fs.hpp reads: formatting,
// and creating, writing, resizing, renaming and deleting files and
// directories. Only a volume read and written through a BlockSource can be
// written; one held as an image, a source without a write function, a
// directory cache volume and a volume whose bitmap is not marked valid are
// read-only, and every change to them fails before anything is written.
//
// The writes come in an order that keeps the volume sound without a
// validator: a block is taken in the bitmap, and the bitmap page written,
// before the block is used; a new block is written before anything points to
// it; a block is unlinked before the bitmap frees it. A change cut short can
// leave blocks taken that nothing uses, never a block two objects share, and
// the bitmap stays marked valid. A new block is the first free one from the
// block of the directory it goes in, so related blocks stay close. Every
// change sets the root's volume alteration date, and a change of a
// directory's entries sets that directory's date. A call keeps at most four
// blocks in buffers of its own and nothing between calls.

struct AmigaDate {
    u32 days;
    u32 minutes;
    u32 ticks;
};

namespace amiga_fs_write_detail {

using namespace amiga_fs_detail;

// Header longs beyond those the reader uses; the trailing ones count back
// from the end of the block.
inline constexpr u32 l_hash_size = 3;  // the root's hash table size
inline constexpr u32 l_first_data = 4;
inline constexpr u32 l_checksum = 5;
inline constexpr u32 l_protection = block_longs - 48;
inline constexpr u32 l_comment = block_longs - 46;  // length byte, then the comment
inline constexpr u32 l_date = block_longs - 23;     // days, minutes, ticks
inline constexpr u32 l_volume_date = block_longs - 10;
inline constexpr u32 l_creation_date = block_longs - 7;
// An OFS data block: its place in the file from 1, its data bytes and the
// next data block of the file.
inline constexpr u32 l_sequence = 2;
inline constexpr u32 l_data_bytes = 3;
inline constexpr u32 l_next_data = 4;

inline constexpr u32 name_field_bytes = 32;
inline constexpr u32 comment_max = 79;
inline constexpr u32 comment_field_bytes = comment_max + 1;
inline constexpr u8 last_plain_dos_type = 3;  // the directory cache types follow

inline void set_long(u8* block, u32 index, u32 value) {
    u8* p = block + index * 4;
    p[0] = static_cast<u8>(value >> 24);
    p[1] = static_cast<u8>(value >> 16);
    p[2] = static_cast<u8>(value >> 8);
    p[3] = static_cast<u8>(value);
}

inline void set_date(u8* block, u32 index, const AmigaDate& date) {
    set_long(block, index, date.days);
    set_long(block, index + 1, date.minutes);
    set_long(block, index + 2, date.ticks);
}

// Sets the long at `index` so that all the longs of the block add up to zero.
inline void seal(u8* block, u32 index) {
    set_long(block, index, 0);
    u32 sum = 0;
    for (u32 i = 0; i < block_longs; ++i) {
        sum += long_at(block, i);
    }
    set_long(block, index, 0u - sum);
}

inline FsError put_block(const AdfVolume& v, u32 number, const u8* data) {
    if (number < v.reserved || number >= v.block_count) {
        return FsError::BadBlock;
    }
    return v.source.write(v.source.ctx, v.first_block + number, data) ? FsError::Ok : FsError::Io;
}

inline FsError put_header(const AdfVolume& v, u32 number, u8* block) {
    seal(block, l_checksum);
    return put_block(v, number, block);
}

// Reads a header into `buf`, which a volume read through a source always fills.
inline FsError get_header(const AdfVolume& v, u32 number, u8* buf) {
    FsError error = FsError::Ok;
    return load_header(v, number, buf, error) != nullptr ? FsError::Ok : error;
}

inline FsError load_file(const AdfVolume& v, u32 number, u8* buf) {
    const FsError error = get_header(v, number, buf);
    if (error != FsError::Ok) {
        return error;
    }
    return long_at(buf, l_sec_type) == st_file ? FsError::Ok : FsError::NotFile;
}

// Checked before a change writes anything; reads the root into `buf`.
inline FsError check_writable(const AdfVolume& v, u8* buf) {
    if (v.image != nullptr || v.source.write == nullptr || v.dos_type > last_plain_dos_type) {
        return FsError::ReadOnly;
    }
    const FsError error = get_header(v, v.root_block, buf);
    if (error != FsError::Ok) {
        return error;
    }
    return long_at(buf, l_bm_flag) == bm_valid ? FsError::Ok : FsError::ReadOnly;
}

// The last write of every change.
inline FsError stamp_volume(const AdfVolume& v, u8* buf, const AmigaDate& now) {
    const FsError error = get_header(v, v.root_block, buf);
    if (error != FsError::Ok) {
        return error;
    }
    set_date(buf, l_volume_date, now);
    return put_header(v, v.root_block, buf);
}

// The length of `text`, counted no further than max + 1.
inline u32 bounded_length(const char* text, u32 max) {
    u32 length = 0;
    while (length <= max && text[length] != '\0') {
        ++length;
    }
    return length;
}

// The length of a name an object can have; 0 for an empty name, one of more
// than 30 characters, or one holding a ':' or a '/'.
inline u32 object_name_length(const char* name) {
    const u32 length = bounded_length(name, name_max);
    if (length > name_max) {
        return 0;
    }
    for (u32 i = 0; i < length; ++i) {
        if (name[i] == ':' || name[i] == '/') {
            return 0;
        }
    }
    return length;
}

// A length byte and the text, with the rest of the field cleared.
inline void put_text(u8* block, u32 index, const char* text, u32 length, u32 field_bytes) {
    u8* field = block + index * 4;
    __builtin_memset(field, 0, field_bytes);
    field[0] = static_cast<u8>(length);
    __builtin_memcpy(field + 1, text, length);
}

inline u32 payload_of(const AdfVolume& v) {
    return v.ffs ? block_bytes : block_bytes - ofs_header_bytes;
}

inline u32 blocks_for(u32 size, u32 payload) {
    return size / payload + (size % payload != 0 ? 1 : 0);
}

inline void set_free(u8* page, u32 bit, bool is_free) {
    const u32 index = 1 + bit / 32;
    const u32 mask = 1u << (bit % 32);
    set_long(page, index, is_free ? long_at(page, index) | mask : long_at(page, index) & ~mask);
}

inline constexpr u32 no_page = 0xFFFFFFFFu;

// The volume's bitmap, one page at a time. A block taken is written to its
// page at once, as the block is about to be used; blocks freed reach the
// disk when the page is left or flushed.
struct Bitmap {
    u32 cursor = 0;  // where the next search for a free block starts
    u32 page = no_page;
    u32 page_block = 0;
    bool dirty = false;
    u8 buf[block_bytes];
};

inline FsError flush_bitmap(const AdfVolume& v, Bitmap& bm) {
    if (!bm.dirty) {
        return FsError::Ok;
    }
    seal(bm.buf, 0);
    bm.dirty = false;
    return put_block(v, bm.page_block, bm.buf);
}

// Page `index` is found through the root, then through the chain of
// extension blocks for the pages past the root's 25.
inline FsError load_page(const AdfVolume& v, Bitmap& bm, u32 index) {
    if (bm.page == index) {
        return FsError::Ok;
    }
    FsError error = flush_bitmap(v, bm);
    bm.page = no_page;
    if (error == FsError::Ok) {
        error = get_header(v, v.root_block, bm.buf);
    }
    if (error != FsError::Ok) {
        return error;
    }
    u32 number = 0;
    if (index < bm_pages_max) {
        number = long_at(bm.buf, l_bm_pages + index);
    } else {
        u32 extension = long_at(bm.buf, l_bm_ext);
        for (u32 slot = index - bm_pages_max;; slot -= bm_ext_pages) {
            if (block_at_checked(v, extension, bm.buf, error) == nullptr) {
                return error;
            }
            if (slot < bm_ext_pages) {
                number = long_at(bm.buf, slot);
                break;
            }
            extension = long_at(bm.buf, block_longs - 1);
        }
    }
    if (block_at_checked(v, number, bm.buf, error) == nullptr) {
        return error;
    }
    if (!block_sums_to_zero(bm.buf)) {
        return FsError::BadChecksum;
    }
    bm.page = index;
    bm.page_block = number;
    return FsError::Ok;
}

// Takes the first free block from the cursor upwards, wrapping round to the
// first block after the reserved ones. A bit that calls the root or the page
// itself free is put right on the way.
inline FsError allocate_block(const AdfVolume& v, Bitmap& bm, u32& out) {
    const u32 bits = v.block_count - v.reserved;
    u32 bit = bm.cursor >= v.reserved && bm.cursor < v.block_count ? bm.cursor - v.reserved : 0;
    for (u32 left = bits; left > 0;) {
        if (bit == bits) {
            bit = 0;
        }
        const FsError error = load_page(v, bm, bit / bm_span);
        if (error != FsError::Ok) {
            return error;
        }
        const u32 in_page = bit % bm_span;
        u32 span = 32 - in_page % 32;  // the bits of this long from `bit` on
        span = span < bits - bit ? span : bits - bit;
        span = span < left ? span : left;
        u32 word = long_at(bm.buf, 1 + in_page / 32) >> (in_page % 32);
        if (span < 32) {
            word &= (1u << span) - 1;
        }
        if (word == 0) {
            bit += span;
            left -= span;
            continue;
        }
        u32 skip = 0;
        for (; (word & 1) == 0; word >>= 1) {
            ++skip;
        }
        set_free(bm.buf, in_page + skip, false);
        bm.dirty = true;
        const u32 number = v.reserved + bit + skip;
        bit += skip + 1;
        left -= skip + 1;
        if (number != v.root_block && number != bm.page_block) {
            bm.cursor = number + 1;
            out = number;
            return flush_bitmap(v, bm);
        }
    }
    return FsError::DiskFull;
}

inline FsError free_block(const AdfVolume& v, Bitmap& bm, u32 number) {
    if (number < v.reserved || number >= v.block_count || number == v.root_block) {
        return FsError::BadBlock;
    }
    const u32 bit = number - v.reserved;
    const FsError error = load_page(v, bm, bit / bm_span);
    if (error != FsError::Ok) {
        return error;
    }
    set_free(bm.buf, bit % bm_span, true);
    bm.dirty = true;
    return FsError::Ok;
}

// Frees the data blocks of `w`'s table from `slot` on, then every extension
// block after it with its data blocks. `buf` may be the table's own buffer.
inline FsError free_tail(const AdfVolume& v, Bitmap& bm, PointerBlocks& w, u32 slot, u8* buf) {
    for (;;) {
        for (const u32 count = long_at(w.table, l_count); slot < count; ++slot) {
            const FsError error = free_block(v, bm, long_at(w.table, l_last_pointer - slot));
            if (error != FsError::Ok) {
                return error;
            }
        }
        if (long_at(w.table, l_extension) == 0) {
            return FsError::Ok;
        }
        FsError error = next_pointer_block(v, w, buf);
        if (error == FsError::Ok) {
            error = free_block(v, bm, w.block);
        }
        if (error != FsError::Ok) {
            return error;
        }
        slot = 0;
    }
}

// Where `number` goes in the bucket's chain to keep it ascending: after
// `prev`, 0 when it goes first, and before `next`, 0 when it goes last.
inline FsError find_chain_place(const AdfVolume& v, const u8* dir, u32 slot, u32 number, u8* buf,
                                u32& prev, u32& next) {
    prev = 0;
    next = long_at(dir, l_table + slot);
    for (u32 step = 0; next != 0 && next < number; ++step) {
        if (step >= v.block_count) {
            return FsError::Loop;
        }
        const FsError error = get_header(v, next, buf);
        if (error != FsError::Ok) {
            return error;
        }
        prev = next;
        next = long_at(buf, l_hash_next);
    }
    return FsError::Ok;
}

// The header whose hash_next is `number`, or 0 when the directory's table
// points at it.
inline FsError find_chain_prev(const AdfVolume& v, const u8* dir, u32 slot, u32 number, u8* buf,
                               u32& prev) {
    prev = 0;
    for (u32 at = long_at(dir, l_table + slot), step = 0; at != number; ++step) {
        if (at == 0) {
            return FsError::BadBlock;  // not in the directory its parent field names
        }
        if (step >= v.block_count) {
            return FsError::Loop;
        }
        const FsError error = get_header(v, at, buf);
        if (error != FsError::Ok) {
            return error;
        }
        prev = at;
        at = long_at(buf, l_hash_next);
    }
    return FsError::Ok;
}

// Points the bucket's chain at `target` from `prev`, or from the table when
// prev is 0, then dates and writes the directory.
inline FsError finish_link(const AdfVolume& v, u8* dir, u32 dir_block, u32 slot, u32 prev,
                           u32 target, u8* buf, const AmigaDate& now) {
    if (prev == 0) {
        set_long(dir, l_table + slot, target);
    } else {
        FsError error = get_header(v, prev, buf);
        if (error == FsError::Ok) {
            set_long(buf, l_hash_next, target);
            error = put_header(v, prev, buf);
        }
        if (error != FsError::Ok) {
            return error;
        }
    }
    set_date(dir, l_date, now);
    return put_header(v, dir_block, dir);
}

inline u32 name_slot(const AdfVolume& v, const u8* header) {
    return hash_name(header + l_name * 4 + 1, header[l_name * 4], v.intl);
}

// Takes `header`, block `number`, out of its bucket in `dir`.
inline FsError unlink_header(const AdfVolume& v, u8* dir, u32 dir_block, const u8* header,
                             u32 number, u8* buf, const AmigaDate& now) {
    const u32 slot = name_slot(v, header);
    u32 prev = 0;
    const FsError error = find_chain_prev(v, dir, slot, number, buf, prev);
    if (error != FsError::Ok) {
        return error;
    }
    return finish_link(v, dir, dir_block, slot, prev, long_at(header, l_hash_next), buf, now);
}

inline FsError create_object(const AdfVolume& v, u32 dir_block, const char* name, u32 sec_type,
                             const AmigaDate& now, DirEntry& out) {
    u8 dir[block_bytes];
    u8 header[block_bytes];
    FsError error = check_writable(v, dir);
    if (error != FsError::Ok) {
        return error;
    }
    const u32 length = object_name_length(name);
    if (length == 0) {
        return FsError::BadName;
    }
    error = get_header(v, dir_block, dir);
    if (error != FsError::Ok) {
        return error;
    }
    if (!is_dir_type(long_at(dir, l_sec_type))) {
        return FsError::NotDir;
    }
    const u8* key = reinterpret_cast<const u8*>(name);
    DirEntry existing{};
    error = find_in_dir(v, dir, key, length, existing);
    if (error != FsError::NotFound) {
        return error == FsError::Ok ? FsError::Exists : error;
    }
    Bitmap bitmap;
    bitmap.cursor = dir_block;
    u32 number = 0;
    const u32 slot = hash_name(key, length, v.intl);
    u32 prev = 0;
    u32 next = 0;
    error = allocate_block(v, bitmap, number);
    if (error == FsError::Ok) {
        error = find_chain_place(v, dir, slot, number, header, prev, next);
    }
    if (error != FsError::Ok) {
        return error;
    }
    __builtin_memset(header, 0, block_bytes);
    set_long(header, l_type, t_header);
    set_long(header, l_own, number);
    set_date(header, l_date, now);
    put_text(header, l_name, name, length, name_field_bytes);
    set_long(header, l_hash_next, next);
    set_long(header, l_parent, dir_block);
    set_long(header, l_sec_type, sec_type);
    error = put_header(v, number, header);
    if (error != FsError::Ok) {
        return error;
    }
    fill_entry(header, number, out);
    error = finish_link(v, dir, dir_block, slot, prev, number, header, now);
    return error == FsError::Ok ? stamp_volume(v, header, now) : error;
}

// Stores [start, end) of the file whose header, block `number`, is in
// `header`: zeros up to `offset`, then `data`. A data block is written once
// the block after it is known, so an OFS block gets its next pointer in the
// same write; a new extension block is written empty before the full table
// that links it. The table in hand, then the header with the size and date,
// go out last. `reached` is where the stored range ends: `end`, or less with
// DiskFull, the file then ending there.
inline FsError write_range(const AdfVolume& v, u8* header, u32 number, u32 start, u32 offset,
                           const u8* data, u32 end, const AmigaDate& now, u32& reached) {
    const u32 payload = payload_of(v);
    const u32 skip = v.ffs ? 0 : ofs_header_bytes;
    const u32 old_size = long_at(header, l_size);
    const u32 old_blocks = blocks_for(old_size, payload);
    u8 table_buf[block_bytes];
    u8 block_buf[block_bytes];
    Bitmap bitmap;
    bitmap.cursor = long_at(header, l_parent);
    PointerBlocks w;
    FsError error = start_pointer_blocks(w, header, number);
    if (error != FsError::Ok) {
        return error;
    }
    u8* table = header;
    u32 table_index = 0;
    bool table_dirty = false;  // an extension block in table_buf with pointers not written yet
    u32 pending = 0;           // the data block in block_buf, not written yet
    u32 pending_index = 0;
    reached = start;

    const auto advance = [&]() {
        FsError result = table_dirty ? put_header(v, w.block, table) : FsError::Ok;
        if (result == FsError::Ok) {
            result = next_pointer_block(v, w, table_buf);
        }
        table = table_buf;
        table_dirty = false;
        ++table_index;
        return result;
    };
    const auto link_extension = [&](u32 extension) {
        __builtin_memset(block_buf, 0, block_bytes);
        set_long(block_buf, l_type, t_list);
        set_long(block_buf, l_own, extension);
        set_long(block_buf, l_parent, number);
        set_long(block_buf, l_sec_type, st_file);
        FsError result = put_header(v, extension, block_buf);
        set_long(table, l_extension, extension);
        if (result == FsError::Ok) {
            result = put_header(v, w.block, table);
        }
        __builtin_memcpy(table_buf, block_buf, block_bytes);
        table = table_buf;
        w.table = table_buf;
        w.block = extension;
        table_dirty = false;
        ++table_index;
        return result;
    };
    // `successor` is the data block after the pending one, 0 for none.
    const auto finish = [&](u32 successor) {
        if (pending == 0) {
            return FsError::Ok;
        }
        if (!v.ffs) {
            const u32 left = (reached > old_size ? reached : old_size) - pending_index * payload;
            set_long(block_buf, l_data_bytes, (successor != 0 || left > payload) ? payload : left);
            if (successor != 0) {
                set_long(block_buf, l_next_data, successor);
            }
            seal(block_buf, l_checksum);
        }
        const u32 block = pending;
        pending = 0;
        return put_block(v, block, block_buf);
    };

    u32 first = start / payload;
    if (!v.ffs && first == old_blocks && first > 0) {
        --first;  // the old last block gains a next pointer
    }
    const u32 stop = start < end ? (end - 1) / payload + 1 : first;
    while (table_index < first / hash_slots && long_at(table, l_extension) != 0) {
        error = advance();
        if (error != FsError::Ok) {
            return error;
        }
    }
    bool full = false;
    for (u32 i = first; i < stop; ++i) {
        const bool fresh = i >= old_blocks;
        const bool new_table = i / hash_slots > table_index;
        const u32 slot = i % hash_slots;
        u32 block = 0;
        u32 extension = 0;
        if (!fresh) {
            error = new_table ? advance() : FsError::Ok;
            if (error != FsError::Ok) {
                return error;
            }
            if (slot >= long_at(table, l_count)) {
                return FsError::BadBlock;
            }
            block = long_at(table, l_last_pointer - slot);
        } else {
            // Only ever appended: to the table in hand, or as the first
            // pointer of the next once this one is full.
            if (long_at(table, l_count) != (new_table ? hash_slots : slot) ||
                i / hash_slots > table_index + 1) {
                return FsError::BadBlock;
            }
            if (new_table && long_at(table, l_extension) == 0) {
                error = allocate_block(v, bitmap, extension);
            }
            if (error == FsError::Ok) {
                error = allocate_block(v, bitmap, block);
            }
            if (error == FsError::DiskFull) {
                full = true;
                error = extension != 0 ? free_block(v, bitmap, extension) : FsError::Ok;
                break;
            }
            if (error != FsError::Ok) {
                return error;
            }
        }
        error = finish(block);
        if (error == FsError::Ok && fresh && new_table) {
            error = extension != 0 ? link_extension(extension) : advance();
        }
        if (error != FsError::Ok) {
            return error;
        }

        const u32 block_start = i * payload;
        const u32 from = start > block_start ? start : block_start;
        const u32 chunk_end = end - block_start < payload ? end : block_start + payload;
        if (fresh || (v.ffs && from == block_start && chunk_end == block_start + payload)) {
            __builtin_memset(block_buf, 0, block_bytes);
        } else if (block_at_checked(v, block, block_buf, error) == nullptr) {
            return error;
        } else if (!v.ffs && long_at(block_buf, l_type) != t_data) {
            return FsError::BadBlock;
        }
        if (!v.ffs) {
            set_long(block_buf, l_type, t_data);
            set_long(block_buf, l_own, number);
            set_long(block_buf, l_sequence, i + 1);
        }
        const u32 zero_to = offset < from ? from : (offset < chunk_end ? offset : chunk_end);
        __builtin_memset(block_buf + skip + (from - block_start), 0, zero_to - from);
        if (chunk_end > zero_to) {
            __builtin_memcpy(block_buf + skip + (zero_to - block_start), data + (zero_to - offset),
                             chunk_end - zero_to);
        }
        pending = block;
        pending_index = i;
        reached = chunk_end;
        if (fresh) {
            set_long(table, l_last_pointer - slot, block);
            set_long(table, l_count, slot + 1);
            table_dirty = table != header;
            if (i == 0) {
                set_long(header, l_first_data, block);
            }
        }
    }
    if (error == FsError::Ok) {
        error = finish(0);
    }
    if (error == FsError::Ok && table_dirty) {
        error = put_header(v, w.block, table);
    }
    if (error == FsError::Ok) {
        error = flush_bitmap(v, bitmap);
    }
    if (error != FsError::Ok) {
        return error;
    }
    if (reached > old_size) {
        set_long(header, l_size, reached);
    }
    set_date(header, l_date, now);
    error = put_header(v, number, header);
    if (error == FsError::Ok) {
        error = stamp_volume(v, table_buf, now);
    }
    if (error != FsError::Ok) {
        return error;
    }
    return full ? FsError::DiskFull : FsError::Ok;
}

// Cuts the file whose header, block `number`, is in `header` down to `size`
// bytes. The new size goes out first, then the table that loses pointers
// and, on OFS, the new last data block; only then are the blocks past the
// end freed.
inline FsError truncate_file(const AdfVolume& v, u8* header, u32 number, u32 size,
                             const AmigaDate& now) {
    const u32 payload = payload_of(v);
    const u32 keep = blocks_for(size, payload);
    const u32 last_table = keep == 0 ? 0 : (keep - 1) / hash_slots;
    const u32 slot = keep - last_table * hash_slots;  // the pointers the last table keeps
    u8 table_buf[block_bytes];
    u8 copy[block_bytes];
    Bitmap bitmap;
    PointerBlocks w;
    FsError error = start_pointer_blocks(w, header, number);
    for (u32 k = 0; k < last_table && error == FsError::Ok; ++k) {
        error = next_pointer_block(v, w, table_buf);
    }
    if (error != FsError::Ok) {
        return error;
    }
    if (long_at(w.table, l_count) < slot) {
        return FsError::BadBlock;
    }
    set_long(header, l_size, size);
    set_date(header, l_date, now);
    if (keep == 0) {
        set_long(header, l_first_data, 0);
    }
    if (last_table > 0) {
        error = put_header(v, number, header);
    }
    // The table keeps its old pointers for the frees; a copy loses them.
    __builtin_memcpy(copy, w.table, block_bytes);
    for (u32 s = slot; s < hash_slots; ++s) {
        set_long(copy, l_last_pointer - s, 0);
    }
    set_long(copy, l_count, slot);
    set_long(copy, l_extension, 0);
    if (error == FsError::Ok) {
        error = put_header(v, w.block, copy);
    }
    if (error == FsError::Ok && !v.ffs && keep > 0) {
        const u32 last = long_at(w.table, l_last_pointer - (slot - 1));
        if (block_at_checked(v, last, copy, error) != nullptr) {
            if (long_at(copy, l_type) != t_data) {
                return FsError::BadBlock;
            }
            set_long(copy, l_data_bytes, size - (keep - 1) * payload);
            set_long(copy, l_next_data, 0);
            seal(copy, l_checksum);
            error = put_block(v, last, copy);
        }
    }
    if (error == FsError::Ok) {
        error = free_tail(v, bitmap, w, slot, table_buf);
    }
    if (error == FsError::Ok) {
        error = flush_bitmap(v, bitmap);
    }
    return error == FsError::Ok ? stamp_volume(v, copy, now) : error;
}

// Loads header `block`, lets `change` alter it or refuse with an error, then
// writes it and stamps the volume.
template <typename Change>
inline FsError change_header(const AdfVolume& v, u32 block, const AmigaDate& now, Change change) {
    u8 header[block_bytes];
    FsError error = check_writable(v, header);
    if (error == FsError::Ok) {
        error = get_header(v, block, header);
    }
    if (error == FsError::Ok) {
        error = change(header);
    }
    if (error == FsError::Ok) {
        error = put_header(v, block, header);
    }
    return error == FsError::Ok ? stamp_volume(v, header, now) : error;
}

}

// Formats a volume of `block_count` blocks from `first_block` on the device:
// the `reserved` boot blocks, "DOS" and the type in the first and zeros
// otherwise, the root in the middle, and right after it the bitmap pages and
// then their extension blocks, with every block free but those. The boot
// blocks go out last, so a format cut short leaves no volume to mount. The
// directory cache types are refused.
inline FsError adf_format(const BlockSource& src, u32 first_block, u32 block_count, u32 reserved,
                          u8 dos_type, const char* name, const AmigaDate& now) {
    using namespace amiga_fs_write_detail;
    if (src.write == nullptr) {
        return FsError::ReadOnly;
    }
    if (dos_type > last_plain_dos_type) {
        return FsError::UnsupportedFs;
    }
    const u32 length = object_name_length(name);
    if (length == 0) {
        return FsError::BadName;
    }
    if (reserved == 0 || block_count <= reserved) {
        return FsError::BadBlock;
    }
    const u32 bits = block_count - reserved;
    const u32 pages = (bits - 1) / bm_span + 1;
    const u32 extensions = pages > bm_pages_max ? (pages - bm_pages_max - 1) / bm_ext_pages + 1 : 0;
    const u32 root = (block_count - 1 + reserved) / 2;
    const u32 first_page = root + 1;
    const u32 first_extension = first_page + pages;
    const u32 used_end = first_extension + extensions;  // the root up to here is in use
    if (used_end > block_count) {
        return FsError::BadBlock;
    }
    u8 block[block_bytes];
    const auto store = [&](u32 number) { return src.write(src.ctx, first_block + number, block); };

    for (u32 page = 0; page < pages; ++page) {
        __builtin_memset(block, 0, block_bytes);
        for (u32 bit = 0; bit < bm_span && page * bm_span + bit < bits; ++bit) {
            const u32 number = reserved + page * bm_span + bit;
            if (number < root || number >= used_end) {
                set_free(block, bit, true);
            }
        }
        seal(block, 0);
        if (!store(first_page + page)) {
            return FsError::Io;
        }
    }
    for (u32 extension = 0; extension < extensions; ++extension) {
        __builtin_memset(block, 0, block_bytes);
        for (u32 i = 0; i < bm_ext_pages; ++i) {
            const u32 page = bm_pages_max + extension * bm_ext_pages + i;
            if (page < pages) {
                set_long(block, i, first_page + page);
            }
        }
        if (extension + 1 < extensions) {
            set_long(block, block_longs - 1, first_extension + extension + 1);
        }
        if (!store(first_extension + extension)) {
            return FsError::Io;
        }
    }

    __builtin_memset(block, 0, block_bytes);
    set_long(block, l_type, t_header);
    set_long(block, l_hash_size, hash_slots);
    set_long(block, l_bm_flag, bm_valid);
    for (u32 i = 0; i < pages && i < bm_pages_max; ++i) {
        set_long(block, l_bm_pages + i, first_page + i);
    }
    set_long(block, l_bm_ext, extensions > 0 ? first_extension : 0);
    set_date(block, l_date, now);
    put_text(block, l_name, name, length, name_field_bytes);
    set_date(block, l_volume_date, now);
    set_date(block, l_creation_date, now);
    set_long(block, l_sec_type, st_root);
    seal(block, l_checksum);
    if (!store(root)) {
        return FsError::Io;
    }

    __builtin_memset(block, 0, block_bytes);
    for (u32 number = reserved; number-- > 0;) {
        if (number == 0) {
            block[0] = 'D';
            block[1] = 'O';
            block[2] = 'S';
            block[3] = dos_type;
        }
        if (!store(number)) {
            return FsError::Io;
        }
    }
    return FsError::Ok;
}

// Creates an empty file in the directory `dir_block` and fills `out` with
// its entry. A name another object of the directory has, in any case, is
// Exists; an empty name, one of more than 30 characters, or one with a ':'
// or a '/' is BadName.
inline FsError adf_create_file(const AdfVolume& v, u32 dir_block, const char* name,
                               const AmigaDate& now, DirEntry& out) {
    return amiga_fs_write_detail::create_object(v, dir_block, name, amiga_fs_detail::st_file, now,
                                                out);
}

// Creates an empty directory, as adf_create_file a file.
inline FsError adf_create_dir(const AdfVolume& v, u32 dir_block, const char* name,
                              const AmigaDate& now, DirEntry& out) {
    return amiga_fs_write_detail::create_object(v, dir_block, name, amiga_fs_detail::st_dir, now,
                                                out);
}

// Writes `bytes` bytes at `offset` of the file `file_block`, growing it as
// needed; an offset past the end fills the gap with zeros. `written` is the
// number of bytes of `data` stored: all of them, or with DiskFull as many as
// fitted, the file then ending after them. Nothing is written for 0 bytes.
inline FsError adf_write(const AdfVolume& v, u32 file_block, u32 offset, const u8* data, u32 bytes,
                         const AmigaDate& now, u32& written) {
    using namespace amiga_fs_write_detail;
    written = 0;
    u8 header[block_bytes];
    FsError error = check_writable(v, header);
    if (error == FsError::Ok) {
        error = load_file(v, file_block, header);
    }
    if (error != FsError::Ok || bytes == 0) {
        return error;
    }
    if (bytes > 0xFFFFFFFFu - offset) {
        return FsError::BadBlock;  // past the largest size a file can have
    }
    const u32 size = long_at(header, l_size);
    u32 reached = 0;
    error = write_range(v, header, file_block, offset < size ? offset : size, offset, data,
                        offset + bytes, now, reached);
    if ((error == FsError::Ok || error == FsError::DiskFull) && reached > offset) {
        written = reached - offset;
    }
    return error;
}

// Cuts the file down to `size` bytes, freeing the data blocks and the
// extension blocks past the end, or grows it with zeros. A file that cannot
// grow all the way ends where the disk filled up, with DiskFull.
inline FsError adf_set_size(const AdfVolume& v, u32 file_block, u32 size, const AmigaDate& now) {
    using namespace amiga_fs_write_detail;
    u8 header[block_bytes];
    FsError error = check_writable(v, header);
    if (error == FsError::Ok) {
        error = load_file(v, file_block, header);
    }
    if (error != FsError::Ok) {
        return error;
    }
    const u32 old_size = long_at(header, l_size);
    if (size < old_size) {
        return truncate_file(v, header, file_block, size, now);
    }
    u32 reached = 0;
    return write_range(v, header, file_block, old_size, size, nullptr, size, now, reached);
}

// Deletes a file, freeing its blocks, or an empty directory: NotEmpty when
// it has entries, BadName for the root.
inline FsError adf_delete(const AdfVolume& v, u32 block, const AmigaDate& now) {
    using namespace amiga_fs_write_detail;
    u8 header[block_bytes];
    u8 dir[block_bytes];
    u8 buf[block_bytes];
    FsError error = check_writable(v, header);
    if (error != FsError::Ok) {
        return error;
    }
    if (block == v.root_block) {
        return FsError::BadName;
    }
    error = get_header(v, block, header);
    if (error != FsError::Ok) {
        return error;
    }
    const u32 sec_type = long_at(header, l_sec_type);
    if (sec_type != st_file && sec_type != st_dir) {
        return FsError::BadBlock;
    }
    for (u32 slot = 0; sec_type == st_dir && slot < hash_slots; ++slot) {
        if (long_at(header, l_table + slot) != 0) {
            return FsError::NotEmpty;
        }
    }
    const u32 parent = long_at(header, l_parent);
    error = get_header(v, parent, dir);
    if (error == FsError::Ok) {
        error = unlink_header(v, dir, parent, header, block, buf, now);
    }
    Bitmap bitmap;
    PointerBlocks w;
    if (error == FsError::Ok && sec_type == st_file) {
        error = start_pointer_blocks(w, header, block);
        if (error == FsError::Ok) {
            error = free_tail(v, bitmap, w, 0, buf);
        }
    }
    if (error == FsError::Ok) {
        error = free_block(v, bitmap, block);
    }
    if (error == FsError::Ok) {
        error = flush_bitmap(v, bitmap);
    }
    return error == FsError::Ok ? stamp_volume(v, buf, now) : error;
}

// Moves the object `block` into the directory `new_dir_block` under
// `new_name`, both directories dated. Exists when another object there has
// the name, so a change of case alone is allowed; Loop for a directory moved
// into itself or below; BadName for the root or a name an object cannot have.
// The object leaves its old directory before it joins the new one.
inline FsError adf_rename(const AdfVolume& v, u32 block, u32 new_dir_block, const char* new_name,
                          const AmigaDate& now) {
    using namespace amiga_fs_write_detail;
    u8 object[block_bytes];
    u8 dir[block_bytes];
    u8 buf[block_bytes];
    FsError error = check_writable(v, object);
    if (error != FsError::Ok) {
        return error;
    }
    const u32 length = object_name_length(new_name);
    if (length == 0 || block == v.root_block) {
        return FsError::BadName;
    }
    error = get_header(v, block, object);
    if (error == FsError::Ok) {
        error = get_header(v, new_dir_block, dir);
    }
    if (error != FsError::Ok) {
        return error;
    }
    if (!is_dir_type(long_at(dir, l_sec_type))) {
        return FsError::NotDir;
    }
    const u8* key = reinterpret_cast<const u8*>(new_name);
    DirEntry existing{};
    error = find_in_dir(v, dir, key, length, existing);
    if (error == FsError::Ok && existing.header_block != block) {
        return FsError::Exists;
    }
    if (error != FsError::Ok && error != FsError::NotFound) {
        return error;
    }
    for (u32 at = new_dir_block, step = 0; at != v.root_block; ++step) {
        if (at == block || step >= v.block_count) {
            return FsError::Loop;
        }
        error = get_header(v, at, buf);
        if (error != FsError::Ok) {
            return error;
        }
        at = long_at(buf, l_parent);
    }

    const u32 old_dir_block = long_at(object, l_parent);
    error = get_header(v, old_dir_block, dir);
    if (error == FsError::Ok) {
        error = unlink_header(v, dir, old_dir_block, object, block, buf, now);
    }
    if (error == FsError::Ok) {
        error = get_header(v, new_dir_block, dir);
    }
    const u32 slot = hash_name(key, length, v.intl);
    u32 prev = 0;
    u32 next = 0;
    if (error == FsError::Ok) {
        error = find_chain_place(v, dir, slot, block, buf, prev, next);
    }
    if (error != FsError::Ok) {
        return error;
    }
    put_text(object, l_name, new_name, length, name_field_bytes);
    set_long(object, l_hash_next, next);
    set_long(object, l_parent, new_dir_block);
    error = put_header(v, block, object);
    if (error == FsError::Ok) {
        error = finish_link(v, dir, new_dir_block, slot, prev, block, buf, now);
    }
    return error == FsError::Ok ? stamp_volume(v, buf, now) : error;
}

// Sets the protection bits of a file or directory; BadName for the root,
// whose block holds bitmap pointers there.
inline FsError adf_set_protection(const AdfVolume& v, u32 block, u32 bits, const AmigaDate& now) {
    using namespace amiga_fs_write_detail;
    return change_header(v, block, now, [&](u8* header) {
        if (block == v.root_block) {
            return FsError::BadName;
        }
        set_long(header, l_protection, bits);
        return FsError::Ok;
    });
}

// Sets the comment of a file or directory; BadName for one of more than 79
// characters, and for the root, whose block holds bitmap pointers there.
inline FsError adf_set_comment(const AdfVolume& v, u32 block, const char* comment,
                               const AmigaDate& now) {
    using namespace amiga_fs_write_detail;
    const u32 length = bounded_length(comment, comment_max);
    return change_header(v, block, now, [&](u8* header) {
        if (block == v.root_block || length > comment_max) {
            return FsError::BadName;
        }
        put_text(header, l_comment, comment, length, comment_field_bytes);
        return FsError::Ok;
    });
}

// Sets the date of a file or directory, or the root's alteration date.
inline FsError adf_set_date(const AdfVolume& v, u32 block, const AmigaDate& date,
                            const AmigaDate& now) {
    using namespace amiga_fs_write_detail;
    return change_header(v, block, now, [&](u8* header) {
        set_date(header, l_date, date);
        return FsError::Ok;
    });
}

}
