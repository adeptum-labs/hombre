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

#include "icon.hpp"

#include "dos.hpp"
#include "gfx.hpp"

namespace hombre {

using m68k::Machine;
using native::Bus;
using i16 = __INT16_TYPE__;

namespace {

constexpr char kName[] = "icon.library";
constexpr char kIdString[] = "icon 60.0 (Hombre)\r\n";

constexpr u16 kMagic = 0xE310;
constexpr u16 kVersion = 1;
constexpr u16 kGflgGadghImage = 2;
constexpr u16 kGflgGadgImage = 4;
// The low byte of the gadget's UserData is the icon's revision; revision 1
// icons carry dd_Flags and dd_ViewModes after every other part.
constexpr u32 kDiskRevision = 1;
constexpr u32 kDiskRevisionMask = 255;
constexpr u32 kNoIconPosition = 0x80000000;
constexpr u32 kDefaultStack = 4096;

// workbench/workbench.h object types.
constexpr u8 kWBDisk = 1;
constexpr u8 kWBDrawer = 2;
constexpr u8 kWBTool = 3;
constexpr u8 kWBProject = 4;
constexpr u8 kWBGarbage = 5;
constexpr u8 kWBKick = 7;

// struct DiskObject (workbench/workbench.h).
namespace disk_object {
constexpr u32 Magic = 0;
constexpr u32 Version = 2;
constexpr u32 Gadget = 4;
constexpr u32 Type = 48;
constexpr u32 Pad = 49;
constexpr u32 DefaultTool = 50;
constexpr u32 ToolTypes = 54;
constexpr u32 CurrentX = 58;
constexpr u32 CurrentY = 62;
constexpr u32 DrawerData = 66;
constexpr u32 ToolWindow = 70;
constexpr u32 StackSize = 74;
constexpr u32 Bytes = 78;
}

// struct Image (intuition/intuition.h).
namespace image {
constexpr u32 LeftEdge = 0;
constexpr u32 TopEdge = 2;
constexpr u32 Width = 4;
constexpr u32 Height = 6;
constexpr u32 Depth = 8;
constexpr u32 ImageData = 10;
constexpr u32 PlanePick = 14;
constexpr u32 PlaneOnOff = 15;
constexpr u32 NextImage = 16;
constexpr u32 Bytes = 20;
}

// struct DrawerData: OldDrawerData and the OS 2 extension.
namespace drawer {
constexpr u32 CurrentX = 48;
constexpr u32 CurrentY = 52;
constexpr u32 Flags = 56;
constexpr u32 ViewModes = 60;
constexpr u32 Bytes = 62;
constexpr u32 OldBytes = 56;
}

// The NewWindow pointers gfx.hpp does not name, between FirstGadget and
// Title and after Screen.
constexpr u32 nw_check_mark = 22;
constexpr u32 nw_bit_map = 34;

constexpr u32 kMaxNameChars = 255;
constexpr u32 kNameBytes = kMaxNameChars + 6;  // ".info" and the NUL
constexpr u32 kMaxInfoBytes = Icon::kMaxInfoBytes;
// A tool type takes five bytes of an .info file at least, so no array a
// file can hold is longer than this.
constexpr u32 kMaxToolTypeEntries = kMaxInfoBytes / 5;
// The longest tool type name and value FindToolType and MatchToolValue take.
constexpr u32 kMaxToolNameChars = 255;
constexpr u32 kMaxToolValueChars = 1023;
constexpr u32 kBumpMax = 30;
constexpr u32 kArtBytes = 512;

constexpr char kInfoSuffix[] = ".info";

char lower_ascii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

bool starts_with_ci(const char* text, const char* prefix) {
    for (u32 i = 0; prefix[i] != '\0'; ++i) {
        if (lower_ascii(text[i]) != lower_ascii(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool same_ci(const char* a, const char* b, u32 length) {
    for (u32 i = 0; i < length; ++i) {
        if (lower_ascii(a[i]) != lower_ascii(b[i])) {
            return false;
        }
    }
    return true;
}

u32 read_be32(const u8* at) {
    return (u32{at[0]} << 24) | (u32{at[1]} << 16) | (u32{at[2]} << 8) | at[3];
}

void write_be16(u8* at, u32 value) {
    at[0] = static_cast<u8>(value >> 8);
    at[1] = static_cast<u8>(value);
}

void write_be32(u8* at, u32 value) {
    at[0] = static_cast<u8>(value >> 24);
    at[1] = static_cast<u8>(value >> 16);
    at[2] = static_cast<u8>(value >> 8);
    at[3] = static_cast<u8>(value);
}

}  // namespace

namespace icon_detail {

// The parts of an .info file a DiskObject is built from. Offsets point into
// the bytes the file was read from.
struct SourceImage {
    bool present = false;
    i32 left = 0;
    i32 top = 0;
    i32 width = 0;
    i32 height = 0;
    i32 depth = 0;
    u32 plane_pick = 0;
    u32 plane_on_off = 0;
    bool has_data = false;
    const u8* data = nullptr;
    u32 bytes = 0;
};

struct IconFile {
    i32 left = 0;
    i32 top = 0;
    i32 width = 0;
    i32 height = 0;
    u32 flags = 0;
    u32 activation = 0;
    u32 gadget_type = 0;
    u32 gadget_id = 0;
    u32 user_data = 0;
    u8 type = 0;
    i32 current_x = 0;
    i32 current_y = 0;
    i32 stack_size = 0;
    bool has_drawer = false;
    u8 drawer_window[drawer::OldBytes]{};
    u32 drawer_flags = 0;
    u32 drawer_view_modes = 0;
    SourceImage render;
    SourceImage select;
    bool has_default_tool = false;
    const char* default_tool = nullptr;
    u32 default_tool_bytes = 0;
    bool has_tool_types = false;
    u32 tool_type_count = 0;
    // The first tool type as the file holds it, a LONG length and the
    // bytes; the others follow it. tool_type_bytes counts the bytes of
    // every string.
    const u8* tool_types = nullptr;
    u32 tool_type_bytes = 0;
    bool has_tool_window = false;
    const char* tool_window = nullptr;
    u32 tool_window_bytes = 0;
};

}  // namespace icon_detail

namespace {

using icon_detail::IconFile;
using icon_detail::SourceImage;

// A big-endian reader over the bytes of an .info file.
struct Reader {
    const u8* data = nullptr;
    u32 size = 0;
    u32 pos = 0;
    bool bad = false;

    bool need(u32 bytes) {
        if (bad || bytes > size - pos) {
            bad = true;
            return false;
        }
        return true;
    }

    u32 u8v() {
        if (!need(1)) {
            return 0;
        }
        return data[pos++];
    }

    u32 u16v() {
        if (!need(2)) {
            return 0;
        }
        const u32 value = (u32{data[pos]} << 8) | data[pos + 1];
        pos += 2;
        return value;
    }

    u32 u32v() {
        if (!need(4)) {
            return 0;
        }
        const u32 value = (u32{data[pos]} << 24) | (u32{data[pos + 1]} << 16) |
                          (u32{data[pos + 2]} << 8) | data[pos + 3];
        pos += 4;
        return value;
    }

    const u8* take(u32 bytes) {
        if (!need(bytes)) {
            return nullptr;
        }
        const u8* at = data + pos;
        pos += bytes;
        return at;
    }
};

bool parse_string(Reader& r, const char*& text, u32& bytes) {
    const u32 length = r.u32v();
    if (r.bad || length == 0) {
        return false;
    }
    const u8* at = r.take(length);
    if (at == nullptr || at[length - 1] != 0) {
        return false;
    }
    text = reinterpret_cast<const char*>(at);
    bytes = length;
    return true;
}

bool parse_image(Reader& r, SourceImage& out) {
    out.present = true;
    out.left = static_cast<i16>(r.u16v());
    out.top = static_cast<i16>(r.u16v());
    out.width = static_cast<i16>(r.u16v());
    out.height = static_cast<i16>(r.u16v());
    out.depth = static_cast<i16>(r.u16v());
    const u32 data = r.u32v();
    out.plane_pick = r.u8v();
    out.plane_on_off = r.u8v();
    r.u32v();  // NextImage: the file holds one Image here
    if (r.bad) {
        return false;
    }
    if (out.width < 0 || out.height < 0 || out.depth < 0 || out.depth > 8) {
        return false;
    }
    if (data != 0) {
        const u64 row = (static_cast<u64>(static_cast<u32>(out.width)) + 15) / 16 * 2;
        const u64 bytes = row * static_cast<u32>(out.height) * static_cast<u32>(out.depth);
        if (bytes > kMaxInfoBytes) {
            return false;
        }
        const u8* at = r.take(static_cast<u32>(bytes));
        if (at == nullptr) {
            return false;
        }
        out.has_data = true;
        out.data = at;
        out.bytes = static_cast<u32>(bytes);
    }
    return true;
}

bool parse_info(const u8* data, u32 size, IconFile& out) {
    if (size < disk_object::Bytes) {
        return false;
    }
    Reader r{data, size};
    if (r.u16v() != kMagic) {
        return false;
    }
    r.u16v();  // do_Version
    r.u32v();  // NextGadget
    out.left = static_cast<i16>(r.u16v());
    out.top = static_cast<i16>(r.u16v());
    out.width = static_cast<i16>(r.u16v());
    out.height = static_cast<i16>(r.u16v());
    out.flags = r.u16v();
    out.activation = r.u16v();
    out.gadget_type = r.u16v();
    const u32 render = r.u32v();
    const u32 select = r.u32v();
    r.u32v();  // GadgetText
    r.u32v();  // MutualExclude
    r.u32v();  // SpecialInfo
    out.gadget_id = r.u16v();
    out.user_data = r.u32v();
    out.type = static_cast<u8>(r.u8v());
    r.u8v();  // pad
    const u32 default_tool = r.u32v();
    const u32 tool_types = r.u32v();
    out.current_x = static_cast<i32>(r.u32v());
    out.current_y = static_cast<i32>(r.u32v());
    const u32 drawer_data = r.u32v();
    const u32 tool_window = r.u32v();
    out.stack_size = static_cast<i32>(r.u32v());
    if (r.bad) {
        return false;
    }

    if (drawer_data != 0) {
        const u8* window = r.take(drawer::OldBytes);
        if (window == nullptr) {
            return false;
        }
        for (u32 i = 0; i < drawer::OldBytes; ++i) {
            out.drawer_window[i] = window[i];
        }
        out.has_drawer = true;
    }
    if (render != 0 && !parse_image(r, out.render)) {
        return false;
    }
    if ((out.flags & kGflgGadghImage) != 0 && select != 0 && !parse_image(r, out.select)) {
        return false;
    }
    if (default_tool != 0) {
        if (!parse_string(r, out.default_tool, out.default_tool_bytes)) {
            return false;
        }
        out.has_default_tool = true;
    }
    if (tool_types != 0) {
        const u32 table = r.u32v();
        if (r.bad || table < 4 || (table % 4) != 0) {
            return false;
        }
        // Every string takes file bytes, so the file's size bounds the
        // walk whatever count the table claims.
        const u32 count = table / 4 - 1;
        out.has_tool_types = true;
        out.tool_types = r.data + r.pos;
        for (u32 i = 0; i < count; ++i) {
            const char* text = nullptr;
            u32 bytes = 0;
            if (!parse_string(r, text, bytes)) {
                return false;
            }
            out.tool_type_bytes += bytes;
        }
        out.tool_type_count = count;
    }
    if (tool_window != 0) {
        if (!parse_string(r, out.tool_window, out.tool_window_bytes)) {
            return false;
        }
        out.has_tool_window = true;
    }
    // The OS 2 disk revision follows every other part.
    if (out.has_drawer && (out.user_data & kDiskRevisionMask) == kDiskRevision) {
        out.drawer_flags = r.u32v();
        out.drawer_view_modes = r.u16v();
    }
    return !r.bad;
}

bool info_path(const char* name, char (&path)[kNameBytes]) {
    u32 length = 0;
    while (name[length] != '\0' && length < kMaxNameChars) {
        path[length] = name[length];
        ++length;
    }
    if (name[length] != '\0') {
        return false;
    }
    for (u32 i = 0; i < sizeof kInfoSuffix; ++i) {
        path[length + i] = kInfoSuffix[i];
    }
    return true;
}

// The built-in default icons, drawn as boxes in pens 0-3: 1 black outline,
// 2 white, 3 blue.
struct ArtBox {
    u8 x0;
    u8 y0;
    u8 x1;
    u8 y1;
    u8 pen;
};

struct Art {
    u32 width;
    u32 height;
    const ArtBox* boxes;
    u32 count;
};

constexpr ArtBox kDiskArt[] = {
    {0, 0, 34, 17, 1}, {1, 1, 33, 16, 2}, {2, 3, 22, 5, 3}, {2, 9, 7, 15, 3},
    {9, 9, 20, 9, 1},  {9, 12, 20, 12, 1},
};
constexpr Art kDisk{35, 18, kDiskArt, sizeof kDiskArt / sizeof kDiskArt[0]};

constexpr ArtBox kDrawerArt[] = {
    {0, 0, 53, 22, 1}, {1, 1, 52, 21, 2}, {2, 2, 16, 5, 3},
    {20, 11, 34, 13, 3}, {4, 17, 49, 18, 3},
};
constexpr Art kDrawer{54, 23, kDrawerArt, sizeof kDrawerArt / sizeof kDrawerArt[0]};

constexpr ArtBox kToolArt[] = {
    {7, 2, 32, 8, 1}, {8, 3, 31, 7, 3}, {16, 8, 23, 21, 1}, {17, 9, 22, 20, 3},
};
constexpr Art kTool{40, 23, kToolArt, sizeof kToolArt / sizeof kToolArt[0]};

constexpr ArtBox kProjectArt[] = {
    {1, 1, 34, 21, 1}, {2, 2, 33, 20, 2}, {25, 2, 33, 10, 3},
    {5, 7, 20, 7, 3},  {5, 11, 20, 11, 3}, {5, 15, 20, 15, 3},
};
constexpr Art kProject{40, 23, kProjectArt, sizeof kProjectArt / sizeof kProjectArt[0]};

constexpr ArtBox kGarbageArt[] = {
    {21, 1, 32, 2, 1},  {7, 3, 46, 6, 1},  {8, 4, 45, 5, 3},  {10, 7, 43, 21, 1},
    {11, 8, 42, 20, 3}, {17, 9, 17, 19, 2}, {26, 9, 26, 19, 2}, {35, 9, 35, 19, 2},
};
constexpr Art kGarbage{54, 23, kGarbageArt, sizeof kGarbageArt / sizeof kGarbageArt[0]};

constexpr ArtBox kRamDiskArt[] = {
    {0, 0, 35, 13, 1}, {1, 1, 34, 12, 3}, {3, 3, 8, 9, 1}, {4, 4, 4, 4, 2},
    {11, 3, 16, 9, 1}, {12, 4, 12, 4, 2}, {19, 3, 24, 9, 1}, {20, 4, 20, 4, 2},
    {27, 3, 32, 9, 1}, {28, 4, 28, 4, 2}, {1, 14, 34, 17, 1}, {2, 14, 3, 16, 2},
    {5, 14, 6, 16, 2}, {8, 14, 9, 16, 2}, {11, 14, 12, 16, 2}, {14, 14, 15, 16, 2},
    {20, 14, 21, 16, 2}, {23, 14, 24, 16, 2}, {26, 14, 27, 16, 2}, {29, 14, 30, 16, 2},
    {32, 14, 33, 16, 2}, {16, 11, 19, 17, 0},
};
// The Ram Disk: a memory module, four chips on a board above its contacts,
// with the key notch between them.
constexpr Art kRamDisk{36, 18, kRamDiskArt, sizeof kRamDiskArt / sizeof kRamDiskArt[0]};

const Art* default_art(u32 type) {
    switch (type) {
    case kWBDisk:
        return &kDisk;
    case kWBDrawer:
        return &kDrawer;
    case kWBTool:
        return &kTool;
    case kWBProject:
        return &kProject;
    case kWBGarbage:
        return &kGarbage;
    case kWBKick:
        return &kDisk;
    default:
        return nullptr;
    }
}

void draw_art(const Art& art, u8* data) {
    const u32 row_bytes = ((art.width + 15) / 16) * 2;
    const u32 plane_bytes = row_bytes * art.height;
    __builtin_memset(data, 0, plane_bytes * 2);
    for (u32 i = 0; i < art.count; ++i) {
        const ArtBox& box = art.boxes[i];
        for (u32 y = box.y0; y <= box.y1; ++y) {
            for (u32 x = box.x0; x <= box.x1; ++x) {
                const u8 bit = static_cast<u8>(0x80u >> (x & 7));
                for (u32 plane = 0; plane < 2; ++plane) {
                    u8& byte = data[plane * plane_bytes + y * row_bytes + (x >> 3)];
                    if (((box.pen >> plane) & 1) != 0) {
                        byte = static_cast<u8>(byte | bit);
                    } else {
                        byte = static_cast<u8>(byte & ~bit);
                    }
                }
            }
        }
    }
}

// Where each part of the DiskObject block goes.
struct Layout {
    u32 total = 0;
    u32 drawer = 0;
    u32 render = 0;
    u32 select = 0;
    u32 render_data = 0;
    u32 select_data = 0;
    u32 default_tool = 0;
    u32 tool_strings = 0;
    u32 tool_types = 0;
    u32 tool_window = 0;
};

bool layout_info(const IconFile& info, Layout& out) {
    u64 at = disk_object::Bytes;
    if (info.has_drawer) {
        out.drawer = static_cast<u32>(at);
        at += drawer::Bytes;
    }
    if (info.render.present) {
        out.render = static_cast<u32>(at);
        at += image::Bytes;
    }
    if (info.select.present) {
        out.select = static_cast<u32>(at);
        at += image::Bytes;
    }
    if (info.render.has_data) {
        out.render_data = static_cast<u32>(at);
        at += info.render.bytes;
    }
    if (info.select.has_data) {
        out.select_data = static_cast<u32>(at);
        at += info.select.bytes;
    }
    if (info.has_default_tool) {
        out.default_tool = static_cast<u32>(at);
        at += info.default_tool_bytes;
    }
    if (info.has_tool_types) {
        out.tool_strings = static_cast<u32>(at);
        at += info.tool_type_bytes;
        at = (at + 3) & ~static_cast<u64>(3);
        out.tool_types = static_cast<u32>(at);
        at += (static_cast<u64>(info.tool_type_count) + 1) * 4;
    }
    if (info.has_tool_window) {
        out.tool_window = static_cast<u32>(at);
        at += info.tool_window_bytes;
    }
    at = (at + 3) & ~static_cast<u64>(3);
    out.total = static_cast<u32>(at);
    return at <= 0xFFFFFFFFu;
}

void write_image(Bus& b, u32 at, const SourceImage& img, u32 data_at) {
    b.w16(at + image::LeftEdge, static_cast<u32>(img.left));
    b.w16(at + image::TopEdge, static_cast<u32>(img.top));
    b.w16(at + image::Width, static_cast<u32>(img.width));
    b.w16(at + image::Height, static_cast<u32>(img.height));
    b.w16(at + image::Depth, static_cast<u32>(img.depth));
    b.w32(at + image::ImageData, img.has_data ? data_at : 0);
    b.w8(at + image::PlanePick, img.plane_pick);
    b.w8(at + image::PlaneOnOff, img.plane_on_off);
    b.w32(at + image::NextImage, 0);
    if (img.has_data && img.bytes != 0) {
        u8* to = b.bytes_at(data_at, img.bytes);
        if (to != nullptr) {
            __builtin_memcpy(to, img.data, img.bytes);
        }
    }
}

void copy_string(Bus& b, u32 at, const char* text, u32 bytes) {
    u8* to = b.bytes_at(at, bytes);
    if (to != nullptr) {
        __builtin_memcpy(to, text, bytes);
    }
}

// A big-endian writer for the bytes of an .info file.
struct Writer {
    u8* data = nullptr;
    u32 size = 0;
    u32 pos = 0;
    bool full = false;

    void u8v(u32 value) {
        if (full || pos >= size) {
            full = true;
            return;
        }
        data[pos++] = static_cast<u8>(value);
    }

    void u16v(u32 value) {
        u8v(value >> 8);
        u8v(value);
    }

    void u32v(u32 value) {
        u16v(value >> 16);
        u16v(value);
    }

    void bytes(const u8* src, u32 count) {
        if (full || count > size - pos) {
            full = true;
            return;
        }
        __builtin_memcpy(data + pos, src, count);
        pos += count;
    }
};

bool write_guest_string(Writer& w, Bus& b, u32 addr) {
    u32 length = 0;
    while (length < kMaxInfoBytes) {
        const u32 c = b.r8(addr + length);
        if (!b.ok()) {
            return false;
        }
        if (c == 0) {
            break;
        }
        ++length;
    }
    if (length >= kMaxInfoBytes) {
        return false;
    }
    w.u32v(length + 1);
    for (u32 i = 0; i < length; ++i) {
        w.u8v(b.r8(addr + i));
    }
    w.u8v(0);
    return b.ok() && !w.full;
}

bool write_guest_image(Writer& w, Bus& b, u32 at) {
    const i32 left = static_cast<i16>(b.r16(at + image::LeftEdge));
    const i32 top = static_cast<i16>(b.r16(at + image::TopEdge));
    const i32 width = static_cast<i16>(b.r16(at + image::Width));
    const i32 height = static_cast<i16>(b.r16(at + image::Height));
    const i32 depth = static_cast<i16>(b.r16(at + image::Depth));
    const u32 data = b.r32(at + image::ImageData);
    const u32 plane_pick = b.r8(at + image::PlanePick);
    const u32 plane_on_off = b.r8(at + image::PlaneOnOff);
    if (!b.ok() || width < 0 || height < 0 || depth < 0 || depth > 8) {
        return false;
    }
    w.u16v(static_cast<u32>(left));
    w.u16v(static_cast<u32>(top));
    w.u16v(static_cast<u32>(width));
    w.u16v(static_cast<u32>(height));
    w.u16v(static_cast<u32>(depth));
    w.u32v(data != 0 ? 1 : 0);
    w.u8v(plane_pick);
    w.u8v(plane_on_off);
    w.u32v(0);  // NextImage
    if (data != 0) {
        const u32 row = ((static_cast<u32>(width) + 15) / 16) * 2;
        const u64 bytes =
            static_cast<u64>(row) * static_cast<u32>(height) * static_cast<u32>(depth);
        if (bytes > w.size) {
            w.full = true;
            return true;
        }
        const u8* src = b.bytes_at(data, static_cast<u32>(bytes));
        if (src == nullptr) {
            return false;
        }
        w.bytes(src, static_cast<u32>(bytes));
    }
    return true;
}

// False when the array holds more entries than a file has room for, or a
// string does not fit.
bool write_guest_tool_types(Writer& w, Bus& b, u32 array) {
    u32 count = 0;
    for (;; ++count) {
        const u32 entry = b.r32(array + count * 4);
        if (!b.ok() || count > kMaxToolTypeEntries) {
            return false;
        }
        if (entry == 0) {
            break;
        }
    }
    w.u32v((count + 1) * 4);
    for (u32 i = 0; i < count; ++i) {
        if (!write_guest_string(w, b, b.r32(array + i * 4))) {
            return false;
        }
    }
    return true;
}

void write_guest_drawer(Writer& w, Bus& b, u32 at) {
    w.u16v(b.r16(at + ndk::nw_LeftEdge));
    w.u16v(b.r16(at + ndk::nw_TopEdge));
    w.u16v(b.r16(at + ndk::nw_Width));
    w.u16v(b.r16(at + ndk::nw_Height));
    w.u8v(b.r8(at + ndk::nw_DetailPen));
    w.u8v(b.r8(at + ndk::nw_BlockPen));
    w.u32v(b.r32(at + ndk::nw_IDCMPFlags));
    w.u32v(b.r32(at + ndk::nw_Flags));
    w.u32v(0);  // FirstGadget
    w.u32v(0);  // CheckMark
    w.u32v(0);  // Title
    w.u32v(0);  // Screen
    w.u32v(0);  // BitMap
    w.u16v(b.r16(at + ndk::nw_MinWidth));
    w.u16v(b.r16(at + ndk::nw_MinHeight));
    w.u16v(b.r16(at + ndk::nw_MaxWidth));
    w.u16v(b.r16(at + ndk::nw_MaxHeight));
    w.u16v(b.r16(at + ndk::nw_Type));
    w.u32v(b.r32(at + drawer::CurrentX));
    w.u32v(b.r32(at + drawer::CurrentY));
}

struct Function {
    u32 lvo;
    m68k::NativeFn fn;
    const char* name;
};

}  // namespace

struct IconNatives {
    static Icon& of(void* ctx) { return *static_cast<Icon*>(ctx); }

    static void get_disk_object(void* ctx, Machine& m) {
        Bus b(m);
        native::set_result(m, of(ctx).get_disk_object(b, m.cpu.a[0]));
    }

    static void get_disk_object_new(void* ctx, Machine& m) {
        Bus b(m);
        native::set_result(m, of(ctx).get_disk_object_new(b, m.cpu.a[0]));
    }

    static void put_disk_object(void* ctx, Machine& m) {
        Bus b(m);
        native::set_result(m, of(ctx).put_disk_object(b, m.cpu.a[0], m.cpu.a[1]));
    }

    static void put_icon(void* ctx, Machine& m) {
        Bus b(m);
        native::set_result(m, of(ctx).put_disk_object(b, m.cpu.a[0], m.cpu.a[1]));
    }

    static void free_disk_object(void* ctx, Machine& m) {
        of(ctx).free_disk_object(m.cpu.a[0]);
    }

    static void find_tool_type(void* ctx, Machine& m) {
        Bus b(m);
        native::set_result(m, of(ctx).find_tool_type(b, m.cpu.a[0], m.cpu.a[1]));
    }

    static void match_tool_value(void* ctx, Machine& m) {
        Bus b(m);
        native::set_result(m, of(ctx).match_tool_value(b, m.cpu.a[0], m.cpu.a[1]));
    }

    static void bump_revision(void* ctx, Machine& m) {
        Bus b(m);
        native::set_result(m, of(ctx).bump_revision(b, m.cpu.a[0], m.cpu.a[1]));
    }

    static void get_def_disk_object(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).get_default_disk_object(m.cpu.d[0]));
    }

    static void put_def_disk_object(void* ctx, Machine& m) {
        of(ctx).set_io_error(ndk::ERROR_DISK_WRITE_PROTECTED);
        native::set_result(m, 0);
    }

    static void delete_disk_object(void* ctx, Machine& m) {
        Bus b(m);
        native::set_result(m, of(ctx).delete_disk_object(b, m.cpu.a[0]));
    }

    // The obsolete free-list calls never hold memory of this library's.
    static void free_free_list(void*, Machine& m) { native::set_result(m, 0); }

    static void add_free_list(void*, Machine& m) { native::set_result(m, 0); }
};

namespace {

constexpr Function kFunctions[] = {
    {icon_lvo::PutIcon, IconNatives::put_icon, "PutIcon"},
    {icon_lvo::FreeFreeList, IconNatives::free_free_list, "FreeFreeList"},
    {icon_lvo::AddFreeList, IconNatives::add_free_list, "AddFreeList"},
    {icon_lvo::GetDiskObject, IconNatives::get_disk_object, "GetDiskObject"},
    {icon_lvo::PutDiskObject, IconNatives::put_disk_object, "PutDiskObject"},
    {icon_lvo::FreeDiskObject, IconNatives::free_disk_object, "FreeDiskObject"},
    {icon_lvo::FindToolType, IconNatives::find_tool_type, "FindToolType"},
    {icon_lvo::MatchToolValue, IconNatives::match_tool_value, "MatchToolValue"},
    {icon_lvo::BumpRevision, IconNatives::bump_revision, "BumpRevision"},
    {icon_lvo::GetDefDiskObject, IconNatives::get_def_disk_object, "GetDefDiskObject"},
    {icon_lvo::PutDefDiskObject, IconNatives::put_def_disk_object, "PutDefDiskObject"},
    {icon_lvo::GetDiskObjectNew, IconNatives::get_disk_object_new, "GetDiskObjectNew"},
    {icon_lvo::DeleteDiskObject, IconNatives::delete_disk_object, "DeleteDiskObject"},
};

}

bool Icon::init(Machine& m, Exec& exec, Dos& dos) {
    machine_ = &m;
    exec_ = &exec;
    dos_ = &dos;
    base_ = 0;
    for (Block& block : blocks_) {
        block = Block{};
    }
    for (NativeVector& v : vectors_) {
        v = NativeVector{nullptr, nullptr};
    }
    for (const Function& f : kFunctions) {
        vectors_[f.lvo / ndk::LIB_VECTSIZE - 1] = NativeVector{f.fn, f.name};
    }
    const u32 id_string = exec.alloc(sizeof kIdString, ndk::MEMF_PUBLIC);
    if (id_string == 0) {
        return false;
    }
    const u32 base =
        exec.make_native_library(kName, kHombreLibVersion, vectors_, kVectors, this);
    if (base == 0) {
        exec.free(id_string, sizeof kIdString);
        return false;
    }
    Bus b(m);
    for (u32 i = 0; i < sizeof kIdString; ++i) {
        b.w8(id_string + i, static_cast<u8>(kIdString[i]));
    }
    b.w16(base + ndk::LIB_REVISION, kHombreLibRevision);
    b.w32(base + ndk::LIB_IDSTRING, id_string);
    if (!b.ok()) {
        return false;
    }
    base_ = base;
    return true;
}

void Icon::set_io_error(u32 code) {
    if (dos_ != nullptr) {
        dos_->set_last_error(code);
    }
}

bool Icon::record(u32 addr, u32 bytes) {
    for (Block& block : blocks_) {
        if (block.addr == 0) {
            block.addr = addr;
            block.bytes = bytes;
            return true;
        }
    }
    return false;
}

void Icon::free_disk_object(u32 addr) {
    if (addr == 0 || machine_ == nullptr || exec_ == nullptr) {
        return;
    }
    const u32 at = native::guest_address(*machine_, addr);
    for (Block& block : blocks_) {
        if (block.addr == at) {
            exec_->free(block.addr, block.bytes);
            block = Block{};
            return;
        }
    }
    LogLine().text("icon: FreeDiskObject of memory not from icon.library").send();
}

u32 Icon::build_object(const icon_detail::IconFile& info) {
    Layout layout;
    if (!layout_info(info, layout)) {
        set_io_error(ndk::ERROR_NO_FREE_STORE);
        return 0;
    }
    const u32 base = exec_->alloc(layout.total, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (base == 0) {
        set_io_error(ndk::ERROR_NO_FREE_STORE);
        return 0;
    }
    Bus b(*machine_);
    const u32 gadget = base + disk_object::Gadget;
    b.w16(base + disk_object::Magic, kMagic);
    b.w16(base + disk_object::Version, kVersion);
    b.w32(gadget + ndk::gg_NextGadget, 0);
    b.w16(gadget + ndk::gg_LeftEdge, static_cast<u32>(info.left));
    b.w16(gadget + ndk::gg_TopEdge, static_cast<u32>(info.top));
    b.w16(gadget + ndk::gg_Width, static_cast<u32>(info.width));
    b.w16(gadget + ndk::gg_Height, static_cast<u32>(info.height));
    b.w16(gadget + ndk::gg_Flags, info.flags);
    b.w16(gadget + ndk::gg_Activation, info.activation);
    b.w16(gadget + ndk::gg_GadgetType, info.gadget_type);
    // The layout holds offsets in the block; the guest gets addresses.
    auto addr = [base](u32 offset) { return offset != 0 ? base + offset : 0; };
    b.w32(gadget + ndk::gg_GadgetRender, addr(layout.render));
    b.w32(gadget + ndk::gg_SelectRender, addr(layout.select));
    b.w32(gadget + ndk::gg_GadgetText, 0);
    b.w32(gadget + ndk::gg_MutualExclude, 0);
    b.w32(gadget + ndk::gg_SpecialInfo, 0);
    b.w16(gadget + ndk::gg_GadgetID, info.gadget_id);
    b.w32(gadget + ndk::gg_UserData, info.user_data);
    b.w8(base + disk_object::Type, info.type);
    b.w8(base + disk_object::Pad, 0);
    b.w32(base + disk_object::DefaultTool, addr(layout.default_tool));
    b.w32(base + disk_object::ToolTypes, addr(layout.tool_types));
    b.w32(base + disk_object::CurrentX, static_cast<u32>(info.current_x));
    b.w32(base + disk_object::CurrentY, static_cast<u32>(info.current_y));
    b.w32(base + disk_object::DrawerData, addr(layout.drawer));
    b.w32(base + disk_object::ToolWindow, addr(layout.tool_window));
    b.w32(base + disk_object::StackSize, static_cast<u32>(info.stack_size));

    if (info.has_drawer) {
        u8* window = b.bytes_at(base + layout.drawer, drawer::Bytes);
        if (window != nullptr) {
            for (u32 i = 0; i < drawer::OldBytes; ++i) {
                window[i] = info.drawer_window[i];
            }
            // The file's NewWindow pointers only say a part follows; none
            // does, so the guest window has none.
            write_be32(window + ndk::nw_FirstGadget, 0);
            write_be32(window + nw_check_mark, 0);
            write_be32(window + ndk::nw_Title, 0);
            write_be32(window + ndk::nw_Screen, 0);
            write_be32(window + nw_bit_map, 0);
            write_be32(window + drawer::Flags, info.drawer_flags);
            write_be16(window + drawer::ViewModes, info.drawer_view_modes);
        }
    }
    if (info.render.present) {
        write_image(b, base + layout.render, info.render, base + layout.render_data);
    }
    if (info.select.present) {
        write_image(b, base + layout.select, info.select, base + layout.select_data);
    }
    if (info.has_default_tool) {
        copy_string(b, base + layout.default_tool, info.default_tool, info.default_tool_bytes);
    }
    if (info.has_tool_types) {
        const u8* from = info.tool_types;
        u32 to = base + layout.tool_strings;
        for (u32 i = 0; i < info.tool_type_count; ++i) {
            const u32 bytes = read_be32(from);
            copy_string(b, to, reinterpret_cast<const char*>(from + 4), bytes);
            b.w32(base + layout.tool_types + i * 4, to);
            from += 4 + bytes;
            to += bytes;
        }
        b.w32(base + layout.tool_types + info.tool_type_count * 4, 0);
    }
    if (info.has_tool_window) {
        copy_string(b, base + layout.tool_window, info.tool_window, info.tool_window_bytes);
    }
    if (!b.ok()) {
        exec_->free(base, layout.total);
        return 0;
    }
    if (!record(base, layout.total)) {
        exec_->free(base, layout.total);
        set_io_error(ndk::ERROR_NO_FREE_STORE);
        return 0;
    }
    return base;
}

u32 Icon::load_object(const char* text) {
    char path[kNameBytes];
    if (!info_path(text, path)) {
        set_io_error(ndk::ERROR_INVALID_COMPONENT_NAME);
        return 0;
    }
    u32 got = 0;
    u32 error = 0;
    if (!dos_->read_file(path, file_, sizeof file_, got, error)) {
        return 0;
    }
    icon_detail::IconFile info;
    if (!parse_info(file_, got, info)) {
        set_io_error(ndk::ERROR_OBJECT_WRONG_TYPE);
        return 0;
    }
    return build_object(info);
}

u32 Icon::default_object(u32 type) { return get_default_disk_object(type); }

u32 Icon::get_disk_object(Bus& b, u32 name) {
    if (name == 0) {
        // No name asks for a DiskObject to fill in: one with no parts.
        return build_object(icon_detail::IconFile{});
    }
    char text[kMaxNameChars + 1];
    if (!b.string(name, text, sizeof text)) {
        if (b.ok()) {
            set_io_error(ndk::ERROR_INVALID_COMPONENT_NAME);
        }
        return 0;
    }
    return load_object(text);
}

u32 Icon::get_disk_object_new(Bus& b, u32 name) {
    const u32 object = get_disk_object(b, name);
    if (object != 0 || name == 0 || !b.ok()) {
        return object;
    }
    // Whatever kept the icon from loading, the object gets the default
    // icon of its type. A name too long to look up keeps the IoErr
    // GetDiskObject left.
    char text[kMaxNameChars + 1];
    if (!b.string(name, text, sizeof text)) {
        return 0;
    }
    const Dos::Target target = dos_->examine(text);
    u8 type = kWBProject;
    if (target.kind == Dos::TargetKind::Dir) {
        type = kWBDrawer;
    } else if (target.kind == Dos::TargetKind::VolumeRoot) {
        type = kWBDisk;
    }
    return get_default_disk_object(type);
}

u32 Icon::get_default_disk_object(u32 type) {
    const Art* art = default_art(type);
    if (art == nullptr) {
        set_io_error(ndk::ERROR_OBJECT_WRONG_TYPE);
        return 0;
    }
    return object_from_art(type, art);
}

u32 Icon::ram_disk_object() { return object_from_art(kWBDisk, &kRamDisk); }

u32 Icon::object_from_art(u32 type, const void* art_ptr) {
    const Art* art = static_cast<const Art*>(art_ptr);
    icon_detail::IconFile info;
    info.left = 0;
    info.top = 0;
    info.width = static_cast<i32>(art->width);
    info.height = static_cast<i32>(art->height);
    // An icon's gadget renders an Image and highlights by complement.
    info.flags = kGflgGadgImage;
    info.activation = ndk::GACT_RELVERIFY;
    info.gadget_type = 1;  // GTYP_BOOLGADGET
    info.user_data = kDiskRevision;
    info.type = static_cast<u8>(type);
    info.current_x = static_cast<i32>(kNoIconPosition);
    info.current_y = static_cast<i32>(kNoIconPosition);
    info.stack_size = static_cast<i32>(kDefaultStack);
    info.has_drawer = type == kWBDisk || type == kWBDrawer || type == kWBGarbage;
    if (info.has_drawer) {
        write_be16(info.drawer_window + ndk::nw_Width, 305);
        write_be16(info.drawer_window + ndk::nw_Height, 100);
    }
    u8 art_data[kArtBytes]{};
    const u32 row_bytes = ((art->width + 15) / 16) * 2;
    const u32 data_bytes = row_bytes * art->height * 2;
    draw_art(*art, art_data);
    info.render.present = true;
    info.render.width = static_cast<i32>(art->width);
    info.render.height = static_cast<i32>(art->height);
    info.render.depth = 2;
    info.render.plane_pick = 3;
    info.render.plane_on_off = 0;
    info.render.has_data = true;
    info.render.data = art_data;
    info.render.bytes = data_bytes;
    return build_object(info);
}

u32 Icon::put_disk_object(Bus& b, u32 name, u32 disk_object) {
    char text[kMaxNameChars + 1];
    if (name == 0 || !b.string(name, text, sizeof text)) {
        if (b.ok()) {
            set_io_error(ndk::ERROR_INVALID_COMPONENT_NAME);
        }
        return 0;
    }
    u32 error = 0;
    return write_object(b, text, disk_object, error);
}

bool Icon::save_object(const char* path, u32 object, u32& error) {
    error = 0;
    if (machine_ == nullptr || path == nullptr) {
        error = ndk::ERROR_REQUIRED_ARG_MISSING;
        return false;
    }
    Bus b(*machine_);
    const bool saved = write_object(b, path, object, error) != 0;
    if (!saved && error == 0) {
        error = b.ok() ? ndk::ERROR_OBJECT_WRONG_TYPE : ndk::ERROR_REQUIRED_ARG_MISSING;
    }
    return saved;
}

u32 Icon::write_object(Bus& b, const char* text, u32 disk_object, u32& error) {
    error = 0;
    char path[kNameBytes];
    if (!info_path(text, path)) {
        set_io_error(ndk::ERROR_INVALID_COMPONENT_NAME);
        error = ndk::ERROR_INVALID_COMPONENT_NAME;
        return 0;
    }
    if (disk_object == 0) {
        set_io_error(ndk::ERROR_REQUIRED_ARG_MISSING);
        error = ndk::ERROR_REQUIRED_ARG_MISSING;
        return 0;
    }
    const u32 gadget = disk_object + disk_object::Gadget;
    const u32 flags = b.r16(gadget + ndk::gg_Flags);
    const u32 render = b.r32(gadget + ndk::gg_GadgetRender);
    const u32 select = b.r32(gadget + ndk::gg_SelectRender);
    const u32 user_data = b.r32(gadget + ndk::gg_UserData);
    const u32 default_tool = b.r32(disk_object + disk_object::DefaultTool);
    const u32 tool_types = b.r32(disk_object + disk_object::ToolTypes);
    const u32 drawer = b.r32(disk_object + disk_object::DrawerData);
    const u32 tool_window = b.r32(disk_object + disk_object::ToolWindow);

    Writer w{file_, sizeof file_};
    w.u16v(kMagic);
    w.u16v(kVersion);
    w.u32v(0);  // NextGadget
    w.u16v(b.r16(gadget + ndk::gg_LeftEdge));
    w.u16v(b.r16(gadget + ndk::gg_TopEdge));
    w.u16v(b.r16(gadget + ndk::gg_Width));
    w.u16v(b.r16(gadget + ndk::gg_Height));
    w.u16v(b.r16(gadget + ndk::gg_Flags));
    w.u16v(b.r16(gadget + ndk::gg_Activation));
    w.u16v(b.r16(gadget + ndk::gg_GadgetType));
    w.u32v(render != 0 ? 1 : 0);
    w.u32v((flags & kGflgGadghImage) != 0 && select != 0 ? 1 : 0);
    w.u32v(0);  // GadgetText
    w.u32v(0);  // MutualExclude
    w.u32v(0);  // SpecialInfo
    w.u16v(b.r16(gadget + ndk::gg_GadgetID));
    w.u32v(user_data);
    w.u8v(b.r8(disk_object + disk_object::Type));
    w.u8v(0);  // pad
    w.u32v(default_tool != 0 ? 1 : 0);
    w.u32v(tool_types != 0 ? 1 : 0);
    w.u32v(b.r32(disk_object + disk_object::CurrentX));
    w.u32v(b.r32(disk_object + disk_object::CurrentY));
    w.u32v(drawer != 0 ? 1 : 0);
    w.u32v(tool_window != 0 ? 1 : 0);
    w.u32v(b.r32(disk_object + disk_object::StackSize));
    if (b.ok()) {
        if (drawer != 0) {
            write_guest_drawer(w, b, drawer);
        }
        if (render != 0 && !write_guest_image(w, b, render)) {
            set_io_error(ndk::ERROR_OBJECT_WRONG_TYPE);
            error = ndk::ERROR_OBJECT_WRONG_TYPE;
            return 0;
        }
        if ((flags & kGflgGadghImage) != 0 && select != 0 &&
            !write_guest_image(w, b, select)) {
            set_io_error(ndk::ERROR_OBJECT_WRONG_TYPE);
            error = ndk::ERROR_OBJECT_WRONG_TYPE;
            return 0;
        }
        if (default_tool != 0 && !write_guest_string(w, b, default_tool)) {
            set_io_error(ndk::ERROR_OBJECT_TOO_LARGE);
            error = ndk::ERROR_OBJECT_TOO_LARGE;
            return 0;
        }
        if (tool_types != 0 && !write_guest_tool_types(w, b, tool_types)) {
            set_io_error(ndk::ERROR_OBJECT_TOO_LARGE);
            error = ndk::ERROR_OBJECT_TOO_LARGE;
            return 0;
        }
        if (tool_window != 0 && !write_guest_string(w, b, tool_window)) {
            set_io_error(ndk::ERROR_OBJECT_TOO_LARGE);
            error = ndk::ERROR_OBJECT_TOO_LARGE;
            return 0;
        }
        if (drawer != 0 && (user_data & kDiskRevisionMask) == kDiskRevision) {
            w.u32v(b.r32(drawer + drawer::Flags));
            w.u16v(b.r16(drawer + drawer::ViewModes));
        }
    }
    if (!b.ok()) {
        return 0;
    }
    if (w.full) {
        set_io_error(ndk::ERROR_OBJECT_TOO_LARGE);
        error = ndk::ERROR_OBJECT_TOO_LARGE;
        return 0;
    }
    return dos_->write_file(path, file_, w.pos, error) ? 1 : 0;
}

u32 Icon::delete_disk_object(Bus& b, u32 name) {
    char text[kMaxNameChars + 1];
    if (name == 0 || !b.string(name, text, sizeof text)) {
        if (b.ok()) {
            set_io_error(ndk::ERROR_INVALID_COMPONENT_NAME);
        }
        return 0;
    }
    char path[kNameBytes];
    if (!info_path(text, path)) {
        set_io_error(ndk::ERROR_INVALID_COMPONENT_NAME);
        return 0;
    }
    u32 error = 0;
    return dos_->delete_file(path, error) ? 1 : 0;
}

u32 Icon::find_tool_type(Bus& b, u32 array, u32 name) {
    if (array == 0 || name == 0) {
        return 0;
    }
    char wanted[kMaxToolNameChars + 1]{};
    if (!b.string(name, wanted, sizeof wanted)) {
        return 0;
    }
    for (u32 i = 0; i <= kMaxToolTypeEntries; ++i) {
        const u32 entry = b.r32(array + i * 4);
        if (!b.ok() || entry == 0) {
            return 0;
        }
        // The entry's name is what stands before its '=' or its end, and
        // only as much of the entry is read as the wanted name is long.
        u32 length = 0;
        bool match = true;
        for (; match && wanted[length] != '\0'; ++length) {
            const u32 c = b.r8(entry + length);
            if (!b.ok()) {
                return 0;
            }
            match = c != '=' &&
                    lower_ascii(static_cast<char>(c)) == lower_ascii(wanted[length]);
        }
        if (!match) {
            continue;
        }
        const u32 terminator = b.r8(entry + length);
        if (!b.ok()) {
            return 0;
        }
        if (terminator == '=') {
            return entry + length + 1;
        }
        if (terminator == 0) {
            return entry + length;
        }
    }
    return 0;
}

u32 Icon::match_tool_value(Bus& b, u32 types, u32 value) {
    if (types == 0 || value == 0) {
        return 0;
    }
    char alternatives[kMaxToolValueChars + 1]{};
    char wanted[kMaxToolValueChars + 1]{};
    if (!b.string(types, alternatives, sizeof alternatives) ||
        !b.string(value, wanted, sizeof wanted)) {
        return 0;
    }
    const char* at = alternatives;
    for (;;) {
        const char* end = at;
        while (*end != '\0' && *end != '|') {
            ++end;
        }
        const u32 length = static_cast<u32>(end - at);
        bool match = wanted[length] == '\0';
        if (match && !same_ci(at, wanted, length)) {
            match = false;
        }
        if (match) {
            return 1;
        }
        if (*end == '\0') {
            return 0;
        }
        at = end + 1;
    }
}

u32 Icon::bump_revision(Bus& b, u32 new_name, u32 old_name) {
    if (new_name == 0) {
        return 0;
    }
    char old_text[kMaxNameChars + 1]{};
    if (old_name != 0 && !b.string(old_name, old_text, sizeof old_text)) {
        if (!b.ok()) {
            return new_name;
        }
        old_text[kMaxNameChars] = '\0';
    }
    // "copy_of_X" is copy 1 of X and "copy_N_of_X" copy N; any other name,
    // "copy X" included, is an original.
    const char* base = old_text;
    bool is_copy = false;
    u64 number = 0;
    if (starts_with_ci(old_text, "copy_")) {
        const char* rest = old_text + 5;
        if (starts_with_ci(rest, "of_")) {
            base = rest + 3;
            is_copy = true;
            number = 1;
        } else {
            u64 digits = 0;
            u32 count = 0;
            while (rest[count] >= '0' && rest[count] <= '9' && count < 9) {
                digits = digits * 10 + static_cast<u32>(rest[count] - '0');
                ++count;
            }
            if (count > 0 && starts_with_ci(rest + count, "_of_")) {
                base = rest + count + 4;
                is_copy = true;
                number = digits;
            }
        }
    }
    char result[kBumpMax + 1];
    u32 pos = 0;
    auto put = [&](const char* text) {
        while (*text != '\0' && pos < kBumpMax) {
            result[pos++] = *text++;
        }
    };
    if (!is_copy) {
        put("copy_of_");
    } else {
        put("copy_");
        char digits[24];
        if (format_u64(digits, sizeof digits, number + 1, 10) != 0) {
            put(digits);
        }
        put("_of_");
    }
    put(base);
    result[pos] = '\0';
    for (u32 i = 0; i < pos; ++i) {
        b.w8(new_name + i, static_cast<u8>(result[i]));
    }
    b.w8(new_name + pos, 0);
    return new_name;
}

}
