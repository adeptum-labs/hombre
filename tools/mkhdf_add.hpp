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

// Host only: the --add option of mkhdf, working out its argument and copying a
// host file into a partition. The tool and its test share this file.

#include "amiga_fs.hpp"
#include "amiga_fs_write.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace hombre {

namespace mkhdf_add {

enum class AddError : u32 {
    Ok,
    BadArgument,
    ColonInPath,
    NameTooLong,
    TooLarge,
    UnknownVolume,
    HostUnreadable,
    Exists,
    NotDir,
    DiskFull,
    Fs,
};

inline const char* add_error_name(AddError error) {
    switch (error) {
    case AddError::Ok: return "ok";
    case AddError::BadArgument: return "expected <volume>:<path>=<hostfile>";
    case AddError::ColonInPath: return "a ':' in the path";
    case AddError::NameTooLong: return "a name longer than 30 bytes";
    case AddError::TooLarge: return "the file is larger than 4 GiB";
    case AddError::UnknownVolume: return "unknown volume";
    case AddError::HostUnreadable: return "cannot read the host file";
    case AddError::Exists: return "a file or folder already has that path";
    case AddError::NotDir: return "a folder on the path is a file";
    case AddError::DiskFull: return "the volume is full";
    case AddError::Fs: return "file system error";
    }
    return "unknown";
}

// One --add argument split up: the volume's name, the path of the file in it,
// and the host file to copy. `argument` is kept so an error can name it.
struct AddSpec {
    std::string argument;
    std::string volume;
    std::vector<std::string> path;
    std::string host_file;
};

inline char folded(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        if (folded(a[i]) != folded(b[i])) {
            return false;
        }
    }
    return true;
}

// The index of the partition named `name`, or -1. Volume names compare
// without regard to case, as the file system compares them.
inline int find_volume(const std::vector<std::string>& volumes, const std::string& name) {
    for (usize i = 0; i < volumes.size(); ++i) {
        if (same_name(volumes[i], name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Splits `<volume>:<path>=<hostfile>`, the path one or more '/'-separated
// names of at most 30 bytes each. Empty components are skipped, as the
// reader skips them.
inline AddError parse_add(const std::string& argument, AddSpec& out) {
    out = AddSpec{};
    out.argument = argument;
    const usize colon = argument.find(':');
    if (colon == std::string::npos) {
        return AddError::BadArgument;
    }
    const usize equals = argument.find('=', colon + 1);
    if (equals == std::string::npos) {
        return AddError::BadArgument;
    }
    out.volume = argument.substr(0, colon);
    out.host_file = argument.substr(equals + 1);
    if (out.volume.empty() || out.host_file.empty()) {
        return AddError::BadArgument;
    }
    const std::string path = argument.substr(colon + 1, equals - colon - 1);
    if (path.find(':') != std::string::npos) {
        return AddError::ColonInPath;
    }
    for (usize at = 0; at < path.size();) {
        const usize slash = path.find('/', at);
        const usize end = slash == std::string::npos ? path.size() : slash;
        const std::string component = path.substr(at, end - at);
        if (!component.empty()) {
            if (component.size() > amiga_fs_detail::name_max) {
                return AddError::NameTooLong;
            }
            out.path.push_back(component);
        }
        at = end + 1;
    }
    return out.path.empty() ? AddError::BadArgument : AddError::Ok;
}

inline AddError read_host_file(const std::string& path, std::vector<u8>& data) {
    data.clear();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return AddError::HostUnreadable;
    }
    u8 chunk[65536];
    usize got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        data.insert(data.end(), chunk, chunk + got);
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok ? AddError::Ok : AddError::HostUnreadable;
}

inline AddError to_add_error(FsError error) {
    switch (error) {
    case FsError::Ok: return AddError::Ok;
    case FsError::Exists: return AddError::Exists;
    case FsError::NotDir: return AddError::NotDir;
    case FsError::DiskFull: return AddError::DiskFull;
    case FsError::NameTooLong: return AddError::NameTooLong;
    case FsError::BadName: return AddError::BadArgument;
    default: return AddError::Fs;
    }
}

// Creates the folders of `path` that are missing, then the file, and writes
// `data` into it. A file or folder already at the path is Exists.
inline AddError add_file(const AdfVolume& v, const std::vector<std::string>& path,
                         const std::vector<u8>& data, const AmigaDate& now) {
    if (path.empty()) {
        return AddError::BadArgument;
    }
    if (data.size() > 0xFFFFFFFFu) {
        return AddError::TooLarge;
    }
    u32 dir = v.root_block;
    std::string at;
    for (usize i = 0; i + 1 < path.size(); ++i) {
        at += (at.empty() ? "" : "/") + path[i];
        DirEntry entry{};
        FsError error = adf_find(v, at.c_str(), entry);
        if (error == FsError::NotFound) {
            error = adf_create_dir(v, dir, path[i].c_str(), now, entry);
        } else if (error == FsError::Ok && !entry.is_dir) {
            return AddError::NotDir;
        }
        if (error != FsError::Ok) {
            return to_add_error(error);
        }
        dir = entry.header_block;
    }
    const std::string& name = path.back();
    const std::string target = at.empty() ? name : at + "/" + name;
    DirEntry existing{};
    const FsError found = adf_find(v, target.c_str(), existing);
    if (found == FsError::Ok) {
        return AddError::Exists;
    }
    if (found != FsError::NotFound) {
        return to_add_error(found);
    }
    DirEntry made{};
    FsError error = adf_create_file(v, dir, name.c_str(), now, made);
    if (error != FsError::Ok) {
        return to_add_error(error);
    }
    u32 written = 0;
    error = adf_write(v, made.header_block, 0, data.data(), static_cast<u32>(data.size()), now,
                      written);
    if (error == FsError::DiskFull) {
        return AddError::DiskFull;
    }
    if (error != FsError::Ok) {
        return to_add_error(error);
    }
    return written == data.size() ? AddError::Ok : AddError::DiskFull;
}

}

}
