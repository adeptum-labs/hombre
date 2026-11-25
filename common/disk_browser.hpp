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
#include "monitor_snapshot.hpp"
#include "types.hpp"

// The monitor's disk browser: the directory listings it shows and the paths
// its requests carry. Header only, so the host tests and the kernel share it.

namespace hombre {

inline constexpr u32 kDiskPartitionBytes = 32;
inline constexpr u32 kDiskPathBytes = sizeof(MonitorListing::path);

namespace disk_browser_detail {

inline char fold_ascii(char c) {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

// Names are compared without regard to ASCII case, as AmigaDOS compares
// device names and as the listing's order asks.
inline bool same_name(const char* a, const char* b) {
    while (*a != '\0' && fold_ascii(*a) == fold_ascii(*b)) {
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

inline bool has_disk_suffix(const char* name) {
    u32 length = 0;
    while (name[length] != '\0') {
        ++length;
    }
    if (length < 4 || name[length - 4] != '.') {
        return false;
    }
    const char* suffix = name + length - 3;
    return same_name(suffix, "adf") || same_name(suffix, "dms");
}

inline void copy_name(char (&out)[sizeof(MonitorEntry::name)], const char* name) {
    u32 i = 0;
    for (; name[i] != '\0' && i + 1 < sizeof out; ++i) {
        out[i] = name[i];
    }
    out[i] = '\0';
}

inline bool entry_before(const MonitorEntry& a, const MonitorEntry& b) {
    if (a.is_dir != b.is_dir) {
        return a.is_dir;
    }
    for (u32 i = 0;; ++i) {
        const char fa = fold_ascii(a.name[i]);
        const char fb = fold_ascii(b.name[i]);
        if (fa != fb || fa == '\0') {
            return fa < fb;
        }
    }
}

// Keeps the entries the listing has room for, in order, so a directory with
// more entries than that still lists its first ones rather than those the
// file system happened to visit first.
inline void add_entry(MonitorListing& listing, const MonitorEntry& entry, bool& truncated) {
    u32 at = 0;
    while (at < listing.count && !entry_before(entry, listing.entries[at])) {
        ++at;
    }
    if (at >= MonitorSnapshot::kMaxEntries) {
        truncated = true;
        return;
    }
    const u32 last = listing.count < MonitorSnapshot::kMaxEntries
                         ? listing.count
                         : MonitorSnapshot::kMaxEntries - 1;
    for (u32 i = last; i > at; --i) {
        listing.entries[i] = listing.entries[i - 1];
    }
    listing.entries[at] = entry;
    if (listing.count < MonitorSnapshot::kMaxEntries) {
        ++listing.count;
    } else {
        truncated = true;
    }
}

inline void add_name(MonitorListing& listing, const char* name, bool is_dir, bool& truncated) {
    MonitorEntry entry;
    copy_name(entry.name, name);
    entry.is_dir = is_dir;
    add_entry(listing, entry, truncated);
}

}

// "DH0:Disks/x.adf" gives partition "DH0" and path "Disks/x.adf"; false
// when the path names no partition.
inline bool disk_browser_split(const char* path, char (&partition)[kDiskPartitionBytes],
                               char (&rest)[kDiskPathBytes]) {
    partition[0] = '\0';
    rest[0] = '\0';
    u32 colon = 0;
    while (path[colon] != '\0' && path[colon] != ':' && path[colon] != '/') {
        ++colon;
    }
    if (colon == 0 || path[colon] != ':') {
        return false;
    }
    u32 i = 0;
    for (; i < colon; ++i) {
        if (i + 1 >= kDiskPartitionBytes) {
            return false;
        }
        partition[i] = path[i];
    }
    partition[i] = '\0';
    const char* p = path + colon + 1;
    while (*p == '/') {
        ++p;
    }
    u32 n = 0;
    for (; p[n] != '\0' && n + 1 < kDiskPathBytes; ++n) {
        rest[n] = p[n];
    }
    rest[n] = '\0';
    return true;
}

// The partition's device names, without their colons, as directories
// "DH0:" and on.
inline void disk_browser_list_partitions(const char* const* devices, u32 count,
                                         MonitorListing& listing) {
    listing = MonitorListing{};
    bool truncated = false;
    for (u32 i = 0; i < count; ++i) {
        const char* device = devices[i];
        if (device == nullptr || device[0] == '\0') {
            continue;
        }
        char name[sizeof(MonitorEntry::name)];
        u32 n = 0;
        // One byte stays for the colon, one for the terminator.
        for (; device[n] != '\0' && n + 2 < sizeof name; ++n) {
            name[n] = device[n];
        }
        name[n++] = ':';
        name[n] = '\0';
        disk_browser_detail::add_name(listing, name, true, truncated);
    }
    listing.truncated = truncated;
}

// Lists the directory path in volume: its directories and the files whose
// name ends in .adf or .dms, directories first and otherwise by name
// without regard to case.
inline void disk_browser_list_directory(const AdfVolume& volume, const char* path,
                                        MonitorListing& listing) {
    listing = MonitorListing{};
    u32 n = 0;
    for (; path[n] != '\0' && n + 1 < sizeof listing.path; ++n) {
        listing.path[n] = path[n];
    }
    listing.path[n] = '\0';

    struct Context {
        MonitorListing* listing;
        bool truncated;
    } context{&listing, false};

    const auto visit = [](void* ctx, const DirEntry& entry) {
        Context& c = *static_cast<Context*>(ctx);
        if (entry.is_dir || disk_browser_detail::has_disk_suffix(entry.name)) {
            disk_browser_detail::add_name(*c.listing, entry.name, entry.is_dir, c.truncated);
        }
    };
    const FsError error = adf_list(volume, path, visit, &context);
    if (error != FsError::Ok) {
        listing.count = 0;
        listing.truncated = false;
        u32 i = 0;
        for (const char* text = fs_error_name(error); text[i] != '\0' && i + 1 < sizeof listing.error;
             ++i) {
            listing.error[i] = text[i];
        }
        listing.error[i] = '\0';
        return;
    }
    listing.truncated = context.truncated;
}

}
