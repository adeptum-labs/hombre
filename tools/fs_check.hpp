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

// Host only: checks an OFS or FFS volume from end to end, for the tests of
// the writer. The kernel never includes this file.

#include "amiga_fs.hpp"
#include "amiga_fs_write.hpp"

#include <string>
#include <utility>
#include <vector>

namespace hombre {

namespace fs_check_detail {

// The block layout constants of the reader and the writer.
using namespace amiga_fs_write_detail;

using Block = std::vector<u8>;

inline std::string num(u32 value) { return std::to_string(value); }

struct Checker {
    const AdfVolume& volume;
    std::vector<bool> reached;  // the blocks the tree and the bitmap use
    std::vector<std::string> problems;

    void report(u32 block, const std::string& what) {
        problems.push_back("block " + num(block) + ": " + what);
    }

    // Empty when the block cannot be read.
    Block read(u32 number) {
        Block buf(block_bytes);
        const u8* block = block_at(volume, number, buf.data());
        return block == nullptr ? Block{} : Block(block, block + block_bytes);
    }

    // Marks a block used; false, with a report, when it is not one of the
    // volume's or is used already, which also ends every loop.
    bool claim(u32 number, const std::string& what) {
        if (number < volume.reserved || number >= volume.block_count) {
            report(number, what + " is outside the volume");
            return false;
        }
        if (reached[number]) {
            report(number, what + " is used twice");
            return false;
        }
        reached[number] = true;
        return true;
    }

    // A header or extension block: its checksum, type and own number.
    Block header(u32 number, u32 type, const std::string& what) {
        Block b = read(number);
        if (b.empty()) {
            report(number, what + " cannot be read");
            return b;
        }
        if (!block_sums_to_zero(b.data())) {
            report(number, what + " has a bad checksum");
        }
        if (long_at(b.data(), l_type) != type) {
            report(number, what + " has type " + num(long_at(b.data(), l_type)));
        }
        if (number != volume.root_block && long_at(b.data(), l_own) != number) {
            report(number, what + " names itself " + num(long_at(b.data(), l_own)));
        }
        return b;
    }

    // The bitmap pages, each claimed with the extension blocks that list
    // them; empty when the bitmap cannot be used.
    std::vector<Block> bitmap(const Block& root) {
        if (long_at(root.data(), l_bm_flag) != bm_valid) {
            report(volume.root_block, "says the bitmap is not valid");
            return {};
        }
        const u32 count = (volume.block_count - volume.reserved - 1) / bm_span + 1;
        std::vector<u32> numbers;
        for (u32 i = 0; i < count && i < bm_pages_max; ++i) {
            numbers.push_back(long_at(root.data(), l_bm_pages + i));
        }
        for (u32 extension = long_at(root.data(), l_bm_ext); numbers.size() < count;) {
            if (!claim(extension, "bitmap extension block")) {
                return {};
            }
            const Block b = read(extension);
            if (b.empty()) {
                report(extension, "bitmap extension block cannot be read");
                return {};
            }
            for (u32 i = 0; i < bm_ext_pages && numbers.size() < count; ++i) {
                numbers.push_back(long_at(b.data(), i));
            }
            extension = long_at(b.data(), block_longs - 1);
        }
        std::vector<Block> pages;
        for (const u32 number : numbers) {
            pages.push_back(read(number));
            if (!claim(number, "bitmap page") || pages.back().empty()) {
                report(number, "bitmap page cannot be used");
                return {};
            }
            if (!block_sums_to_zero(pages.back().data())) {
                report(number, "bitmap page has a bad checksum");
            }
        }
        return pages;
    }

    void file(u32 number, const Block& head) {
        const u32 size = long_at(head.data(), l_size);
        const u32 payload = volume.ffs ? block_bytes : block_bytes - ofs_header_bytes;
        const u64 expected = (u64{size} + payload - 1) / payload;
        std::vector<u32> data;
        Block table = head;
        for (u32 table_number = number;;) {
            const u32 count = long_at(table.data(), l_count);
            if (count > hash_slots) {
                report(table_number, "holds " + num(count) + " data pointers");
                break;
            }
            for (u32 slot = 0; slot < hash_slots; ++slot) {
                const u32 pointer = long_at(table.data(), l_last_pointer - slot);
                if (slot < count) {
                    data.push_back(pointer);
                } else if (pointer != 0) {
                    report(table_number, "has a data pointer past its count");
                }
            }
            const u32 next = long_at(table.data(), l_extension);
            if (next == 0) {
                break;
            }
            if (count != hash_slots) {
                report(table_number, "is not full but has an extension block");
            }
            if (!claim(next, "extension block")) {
                break;
            }
            table = header(next, t_list, "extension block");
            if (table.empty()) {
                break;
            }
            table_number = next;
            if (long_at(table.data(), l_parent) != number) {
                report(next, "extension block belongs to " + num(long_at(table.data(), l_parent)));
            }
            if (long_at(table.data(), l_sec_type) != st_file) {
                report(next, "extension block has the wrong secondary type");
            }
            if (long_at(table.data(), l_count) == 0) {
                report(next, "extension block is empty");
            }
        }
        if (data.size() != expected) {
            report(number, "has " + num(static_cast<u32>(data.size())) + " data blocks for " +
                               num(size) + " bytes");
        }
        if (long_at(head.data(), l_first_data) != (data.empty() ? 0 : data[0])) {
            report(number, "names the wrong first data block");
        }
        for (u32 i = 0; i < data.size(); ++i) {
            if (!claim(data[i], "data block of " + num(number)) || volume.ffs) {
                continue;
            }
            const Block d = read(data[i]);
            if (d.empty()) {
                report(data[i], "data block cannot be read");
                continue;
            }
            const u64 done = u64{i} * payload;
            const u64 left = done >= size ? 0 : size - done;
            const u32 bytes = static_cast<u32>(left < payload ? left : payload);
            const u32 next = i + 1 < data.size() ? data[i + 1] : 0;
            if (!block_sums_to_zero(d.data())) {
                report(data[i], "data block has a bad checksum");
            }
            if (long_at(d.data(), l_type) != t_data || long_at(d.data(), l_own) != number) {
                report(data[i], "data block has the wrong type or header key");
            }
            if (long_at(d.data(), l_sequence) != i + 1) {
                report(data[i], "data block has sequence number " +
                                    num(long_at(d.data(), l_sequence)) + ", not " + num(i + 1));
            }
            if (long_at(d.data(), l_data_bytes) != bytes) {
                report(data[i], "data block holds " + num(long_at(d.data(), l_data_bytes)) +
                                    " bytes, not " + num(bytes));
            }
            if (long_at(d.data(), l_next_data) != next) {
                report(data[i], "data block points on to " + num(long_at(d.data(), l_next_data)) +
                                    ", not " + num(next));
            }
        }
    }

    // The entries of a directory: every header in the bucket its name hashes
    // to, in ascending order, naming the directory as its parent. The
    // directories found are added to `dirs`.
    void directory(u32 number, const Block& dir, std::vector<std::pair<u32, Block>>& dirs) {
        for (u32 slot = 0; slot < hash_slots; ++slot) {
            u32 previous = 0;
            for (u32 entry = long_at(dir.data(), l_table + slot); entry != 0;) {
                if (entry <= previous) {
                    report(entry, "breaks the ascending order of its hash chain");
                }
                previous = entry;
                if (!claim(entry, "header")) {
                    break;
                }
                const Block h = header(entry, t_header, "header");
                if (h.empty()) {
                    break;
                }
                const u32 length = h[l_name * 4];
                if (length == 0 || length > name_max) {
                    report(entry, "has a name of " + num(length) + " characters");
                } else if (hash_name(h.data() + l_name * 4 + 1, length, volume.intl) != slot) {
                    report(entry, "is in hash bucket " + num(slot) +
                                      ", not the one its name hashes to");
                }
                if (long_at(h.data(), l_parent) != number) {
                    report(entry, "names parent " + num(long_at(h.data(), l_parent)) +
                                      " but is in directory " + num(number));
                }
                const u32 sec_type = long_at(h.data(), l_sec_type);
                if (sec_type == st_dir) {
                    dirs.emplace_back(entry, h);
                } else if (sec_type == st_file) {
                    file(entry, h);
                } else {
                    report(entry, "has secondary type " + num(sec_type));
                }
                entry = long_at(h.data(), l_hash_next);
            }
        }
    }

    // Both ways the bitmap can disagree with the blocks in use.
    void compare(const std::vector<Block>& pages) {
        for (u32 number = volume.reserved; number < volume.block_count; ++number) {
            const u32 bit = number - volume.reserved;
            const bool marked_free = (long_at(pages[bit / bm_span].data(), 1 + bit % bm_span / 32) &
                                      (1u << (bit % 32))) != 0;
            if (reached[number] && marked_free) {
                report(number, "is in use but marked free");
            } else if (!reached[number] && !marked_free) {
                report(number, "is marked in use but nothing uses it");
            }
        }
    }
};

}

// Every problem found, one line each; empty for a consistent volume. Checks
// the checksums, own numbers and types of the headers, that each entry sits
// in the bucket its name hashes to, names its directory as parent and keeps
// its chain ascending, that each file has the data blocks its size needs
// through a sound extension chain, the OFS data block fields, that no block
// is used twice, and that the bitmap marks exactly the used blocks.
inline std::vector<std::string> fs_check(const AdfVolume& volume) {
    using namespace fs_check_detail;
    Checker c{volume, std::vector<bool>(volume.block_count, false), {}};
    if (volume.block_count <= volume.reserved || volume.root_block < volume.reserved ||
        volume.root_block >= volume.block_count) {
        c.problems.push_back("the volume has no room for its root block");
        return c.problems;
    }
    c.claim(volume.root_block, "root");
    const Block root = c.header(volume.root_block, t_header, "root");
    if (root.empty()) {
        return c.problems;
    }
    if (long_at(root.data(), l_sec_type) != st_root ||
        long_at(root.data(), l_hash_size) != hash_slots) {
        c.report(volume.root_block, "is not a root block");
    }
    const std::vector<Block> pages = c.bitmap(root);
    std::vector<std::pair<u32, Block>> dirs{{volume.root_block, root}};
    while (!dirs.empty()) {
        const std::pair<u32, Block> dir = dirs.back();
        dirs.pop_back();
        c.directory(dir.first, dir.second, dirs);
    }
    if (!pages.empty()) {
        c.compare(pages);
    }
    return c.problems;
}

}
