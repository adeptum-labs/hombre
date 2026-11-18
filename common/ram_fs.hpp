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

#include "dos_errors.hpp"
#include "types.hpp"

namespace hombre {

// In-memory volume over a caller-provided store: the node table is fixed and
// the contents live entirely in guest memory, so no heap is used.
class RamFs {
public:
    static constexpr u32 kMaxNodes = 1024;
    static constexpr u32 kMaxName = 30;
    static constexpr u32 kMaxComment = 79;
    static constexpr u32 kNone = 0xFFFFFFFF;

    struct Date {
        u32 days = 0;
        u32 minute = 0;
        u32 tick = 0;
    };

    struct Node {
        bool used = false;
        bool is_dir = false;
        u32 parent = kNone;
        char name[kMaxName + 1]{};
        u32 protection = 0;  // Amiga semantics: a set low bit denies access
        Date date;
        char comment[kMaxComment + 1]{};
        u32 size = 0;
        u32 data = 0;
        u32 capacity = 0;
        u32 open_count = 0;
    };

    struct Store {
        void* ctx = nullptr;
        u32 (*alloc)(void* ctx, u32 bytes) = nullptr;
        void (*free)(void* ctx, u32 addr, u32 bytes) = nullptr;
        void (*read)(void* ctx, u32 addr, u8* dst, u32 n) = nullptr;
        void (*write)(void* ctx, u32 addr, const u8* src, u32 n) = nullptr;
        void (*now)(void* ctx, u32& days, u32& minute, u32& tick) = nullptr;
    };

    constexpr RamFs() = default;

    void init(const Store& store) {
        if (store_.free != nullptr) {
            for (u32 i = 0; i < kMaxNodes; ++i) {
                free_data(nodes_[i]);
            }
        }
        for (u32 i = 0; i < kMaxNodes; ++i) {
            nodes_[i] = Node{};
        }
        store_ = store;
        Node& root = nodes_[0];
        root.used = true;
        root.is_dir = true;
        root.parent = kNone;
        root.protection = 0;
        copy_text(root.name, kMaxName, "Ram Disk");
        stamp(root);
    }

    u32 find(u32 dir, const char* name, u32& out) const {
        const u32 error = check_parent(dir, name);
        return error != 0 ? error : find_child(dir, name, out);
    }

    u32 find_path(const char* path, u32& out) const {
        if (path == nullptr) {
            return ERROR_INVALID_COMPONENT_NAME;
        }
        if (path[0] == '\0') {
            out = 0;
            return 0;
        }
        u32 dir = 0;
        const char* component = path;
        while (true) {
            const char* end = component;
            while (*end != '\0' && *end != '/') {
                ++end;
            }
            char name[kMaxName + 1]{};
            const u32 length = static_cast<u32>(end - component);
            if (length == 0 || length > kMaxName) {
                return ERROR_INVALID_COMPONENT_NAME;
            }
            for (u32 i = 0; i < length; ++i) {
                if (component[i] == ':') {
                    return ERROR_INVALID_COMPONENT_NAME;
                }
                name[i] = component[i];
            }
            u32 child = kNone;
            const u32 error = find(dir, name, child);
            if (*end == '\0') {
                if (error != 0) {
                    return error;
                }
                out = child;
                return 0;
            }
            if (error != 0 || !nodes_[child].is_dir) {
                return ERROR_DIR_NOT_FOUND;
            }
            dir = child;
            component = end + 1;
        }
    }

    u32 create_file(u32 dir, const char* name, u32& out) {
        const u32 error = check_parent(dir, name);
        if (error != 0) {
            return error;
        }
        u32 existing = kNone;
        if (find_child(dir, name, existing) == 0) {
            Node& f = nodes_[existing];
            if (f.is_dir) {
                return ERROR_OBJECT_EXISTS;
            }
            if (f.open_count > 0) {
                return ERROR_OBJECT_IN_USE;
            }
            free_data(f);
            f.size = 0;
            stamp(f);
            out = existing;
            return 0;
        }
        u32 index = kNone;
        if (!take_free_node(index)) {
            return ERROR_DISK_FULL;
        }
        Node& f = nodes_[index];
        f.used = true;
        f.parent = dir;
        copy_text(f.name, kMaxName, name);
        stamp(f);
        out = index;
        return 0;
    }

    u32 create_dir(u32 dir, const char* name, u32& out) {
        const u32 error = check_parent(dir, name);
        if (error != 0) {
            return error;
        }
        u32 existing = kNone;
        if (find_child(dir, name, existing) == 0) {
            return ERROR_OBJECT_EXISTS;
        }
        u32 index = kNone;
        if (!take_free_node(index)) {
            return ERROR_DISK_FULL;
        }
        Node& d = nodes_[index];
        d.used = true;
        d.is_dir = true;
        d.parent = dir;
        copy_text(d.name, kMaxName, name);
        stamp(d);
        out = index;
        return 0;
    }

    u32 remove(u32 node) {
        if (!valid(node)) {
            return ERROR_OBJECT_NOT_FOUND;
        }
        Node& n = nodes_[node];
        if (node == 0 || n.open_count > 0) {
            return ERROR_OBJECT_IN_USE;
        }
        if ((n.protection & 1u) != 0) {
            return ERROR_DELETE_PROTECTED;
        }
        if (n.is_dir && next_child(node, kNone) != kNone) {
            return ERROR_DIRECTORY_NOT_EMPTY;
        }
        free_data(n);
        nodes_[node] = Node{};
        return 0;
    }

    u32 rename(u32 node, u32 new_dir, const char* new_name) {
        if (!valid(node)) {
            return ERROR_OBJECT_NOT_FOUND;
        }
        if (node == 0) {
            return ERROR_OBJECT_IN_USE;
        }
        const u32 error = check_parent(new_dir, new_name);
        if (error != 0) {
            return error;
        }
        for (u32 dir = new_dir; dir != kNone; dir = nodes_[dir].parent) {
            if (dir == node) {
                return ERROR_OBJECT_IN_USE;
            }
        }
        u32 existing = kNone;
        if (find_child(new_dir, new_name, existing) == 0 && existing != node) {
            return ERROR_OBJECT_EXISTS;
        }
        Node& n = nodes_[node];
        n.parent = new_dir;
        copy_text(n.name, kMaxName, new_name);
        return 0;
    }

    u32 read(u32 node, u32 pos, u8* dst, u32 n, u32& got) const {
        got = 0;
        if (!valid(node)) {
            return ERROR_OBJECT_NOT_FOUND;
        }
        const Node& f = nodes_[node];
        if (f.is_dir) {
            return ERROR_OBJECT_WRONG_TYPE;
        }
        if (n == 0 || pos >= f.size) {
            return 0;
        }
        u32 count = f.size - pos;
        if (count > n) {
            count = n;
        }
        store_.read(store_.ctx, f.data + pos, dst, count);
        got = count;
        return 0;
    }

    u32 write(u32 node, u32 pos, const u8* src, u32 n) {
        if (!valid(node)) {
            return ERROR_OBJECT_NOT_FOUND;
        }
        Node& f = nodes_[node];
        if (f.is_dir) {
            return ERROR_OBJECT_WRONG_TYPE;
        }
        if (n == 0) {
            return 0;
        }
        if (n > kNone - pos) {
            return ERROR_DISK_FULL;
        }
        const u32 end = pos + n;
        const u32 needed = end > f.size ? end : f.size;
        const u32 error = reserve(f, needed);
        if (error != 0) {
            return error;
        }
        if (pos > f.size) {
            zero_fill(f, f.size, pos);
        }
        store_.write(store_.ctx, f.data + pos, src, n);
        f.size = needed;
        stamp(f);
        return 0;
    }

    u32 set_size(u32 node, u32 size) {
        if (!valid(node)) {
            return ERROR_OBJECT_NOT_FOUND;
        }
        Node& f = nodes_[node];
        if (f.is_dir) {
            return ERROR_OBJECT_WRONG_TYPE;
        }
        if (size == 0) {
            free_data(f);
            f.size = 0;
        } else if (size <= f.size) {
            f.size = size;
        } else {
            const u32 error = reserve(f, size);
            if (error != 0) {
                return error;
            }
            zero_fill(f, f.size, size);
            f.size = size;
        }
        stamp(f);
        return 0;
    }

    u32 set_protection(u32 node, u32 protection) {
        if (!valid(node)) {
            return ERROR_OBJECT_NOT_FOUND;
        }
        nodes_[node].protection = protection;
        return 0;
    }

    u32 set_comment(u32 node, const char* comment) {
        if (!valid(node)) {
            return ERROR_OBJECT_NOT_FOUND;
        }
        if (comment == nullptr) {
            comment = "";
        }
        u32 length = 0;
        while (comment[length] != '\0') {
            if (length >= kMaxComment) {
                return ERROR_COMMENT_TOO_BIG;
            }
            ++length;
        }
        copy_text(nodes_[node].comment, kMaxComment, comment);
        return 0;
    }

    u32 set_date(u32 node, u32 days, u32 minute, u32 tick) {
        if (!valid(node)) {
            return ERROR_OBJECT_NOT_FOUND;
        }
        Node& n = nodes_[node];
        n.date.days = days;
        n.date.minute = minute;
        n.date.tick = tick;
        return 0;
    }

    void retain(u32 node) {
        if (valid(node)) {
            ++nodes_[node].open_count;
        }
    }

    void release(u32 node) {
        if (valid(node) && nodes_[node].open_count > 0) {
            --nodes_[node].open_count;
        }
    }

    bool valid(u32 node) const { return node < kMaxNodes && nodes_[node].used; }

    const Node& node(u32 idx) const { return nodes_[idx]; }

    u32 parent(u32 node) const { return valid(node) ? nodes_[node].parent : kNone; }

    template <class F>
    void list(u32 dir, F&& visit) const {
        if (!valid(dir) || !nodes_[dir].is_dir) {
            return;
        }
        for (u32 i = 1; i < kMaxNodes; ++i) {
            if (nodes_[i].used && nodes_[i].parent == dir) {
                visit(i);
            }
        }
    }

    u32 next_child(u32 dir, u32 after) const {
        if (!valid(dir) || !nodes_[dir].is_dir) {
            return kNone;
        }
        for (u32 i = after == kNone ? 1u : after + 1u; i < kMaxNodes; ++i) {
            if (nodes_[i].used && nodes_[i].parent == dir) {
                return i;
            }
        }
        return kNone;
    }

    u32 used_nodes() const {
        u32 count = 0;
        for (u32 i = 0; i < kMaxNodes; ++i) {
            if (nodes_[i].used) {
                ++count;
            }
        }
        return count;
    }

    u32 bytes_used() const {
        u32 total = 0;
        for (u32 i = 0; i < kMaxNodes; ++i) {
            if (nodes_[i].used && !nodes_[i].is_dir) {
                total += nodes_[i].capacity;
            }
        }
        return total;
    }

private:
    static constexpr u32 kChunk = 256;
    static constexpr u32 kMinCapacity = 256;

    static char lower_ascii(char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
    }

    static bool valid_name(const char* name) {
        if (name == nullptr || name[0] == '\0') {
            return false;
        }
        for (u32 i = 0; name[i] != '\0'; ++i) {
            if (i >= kMaxName || name[i] == ':' || name[i] == '/') {
                return false;
            }
        }
        return true;
    }

    static bool same_name(const char* a, const char* b) {
        for (u32 i = 0;; ++i) {
            if (lower_ascii(a[i]) != lower_ascii(b[i])) {
                return false;
            }
            if (a[i] == '\0') {
                return true;
            }
        }
    }

    static void copy_text(char* dst, u32 limit, const char* src) {
        u32 i = 0;
        while (i < limit && src[i] != '\0') {
            dst[i] = src[i];
            ++i;
        }
        dst[i] = '\0';
    }

    u32 check_parent(u32 dir, const char* name) const {
        if (!valid(dir)) {
            return ERROR_OBJECT_NOT_FOUND;
        }
        if (!nodes_[dir].is_dir) {
            return ERROR_OBJECT_WRONG_TYPE;
        }
        if (!valid_name(name)) {
            return ERROR_INVALID_COMPONENT_NAME;
        }
        return 0;
    }

    u32 find_child(u32 dir, const char* name, u32& out) const {
        for (u32 i = 1; i < kMaxNodes; ++i) {
            if (nodes_[i].used && nodes_[i].parent == dir && same_name(nodes_[i].name, name)) {
                out = i;
                return 0;
            }
        }
        return ERROR_OBJECT_NOT_FOUND;
    }

    bool take_free_node(u32& index) const {
        for (u32 i = 1; i < kMaxNodes; ++i) {
            if (!nodes_[i].used) {
                index = i;
                return true;
            }
        }
        return false;
    }

    u32 reserve(Node& f, u32 needed) {
        if (needed <= f.capacity) {
            return 0;
        }
        const u32 doubled = f.capacity <= kNone / 2u ? f.capacity * 2u : kNone;
        u32 new_capacity = needed > doubled ? needed : doubled;
        if (new_capacity < kMinCapacity) {
            new_capacity = kMinCapacity;
        }
        if (store_.alloc == nullptr) {
            return ERROR_DISK_FULL;
        }
        const u32 addr = store_.alloc(store_.ctx, new_capacity);
        if (addr == 0) {
            return ERROR_DISK_FULL;
        }
        copy_data(addr, f);
        if (f.data != 0 && f.capacity > 0) {
            store_.free(store_.ctx, f.data, f.capacity);
        }
        f.data = addr;
        f.capacity = new_capacity;
        return 0;
    }

    void copy_data(u32 addr, const Node& f) const {
        u8 buf[kChunk]{};
        u32 offset = 0;
        while (offset < f.size) {
            u32 chunk = f.size - offset;
            if (chunk > kChunk) {
                chunk = kChunk;
            }
            store_.read(store_.ctx, f.data + offset, buf, chunk);
            store_.write(store_.ctx, addr + offset, buf, chunk);
            offset += chunk;
        }
    }

    void zero_fill(const Node& f, u32 from, u32 to) const {
        const u8 zeros[kChunk]{};
        while (from < to) {
            u32 chunk = to - from;
            if (chunk > kChunk) {
                chunk = kChunk;
            }
            store_.write(store_.ctx, f.data + from, zeros, chunk);
            from += chunk;
        }
    }

    void free_data(Node& f) {
        if (f.used && !f.is_dir && f.data != 0 && f.capacity > 0) {
            store_.free(store_.ctx, f.data, f.capacity);
        }
        f.data = 0;
        f.capacity = 0;
    }

    void stamp(Node& n) const {
        if (store_.now != nullptr) {
            store_.now(store_.ctx, n.date.days, n.date.minute, n.date.tick);
        }
    }

    Store store_{};
    Node nodes_[kMaxNodes]{};
};

}  // namespace hombre