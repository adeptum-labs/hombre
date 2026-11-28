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

// png.datatype and jpeg.datatype over the real picture.datatype, a small
// datatypesclass and a dos volume holding the decoder's fixtures and a few
// larger pictures: loads through a dos file handle, the header and the
// pixels they write, and the failures, ended runs, removed tasks and
// shortages of memory a load meets.

#include "gfx_rig.hpp"
#include "image_fixtures.hpp"
#include "libs/dos.hpp"
#include "libs/image_dt.hpp"
#include "libs/picture_dt.hpp"
#include "ofs_writer.hpp"

#include <vector>

using namespace hombre;
using namespace hombre::m68k;
using namespace hombre::ndk;
using gfx_test::Base;
using gfx_test::GfxRig;
using gfx_test::kData;
using native::Bus;

namespace {

constexpr u32 kTags = kData;
constexpr u32 kName = kData + 0x400;
constexpr u32 kStore = kData + 0x500;
constexpr u32 kPath = kData + 0x600;
constexpr u32 kPixels = kData + 0x1000;
constexpr u32 kLoaderTask = kData + 0x4800;
constexpr u32 kYielderTask = kData + 0x4900;
constexpr u32 kLoaderStack = kData + 0x5800;
constexpr u32 kYielderStack = kData + 0x6800;

constexpr u32 DTA_Dummy = 0x80001000;
constexpr u32 DTA_Name = DTA_Dummy + 100;
constexpr u32 DTA_SourceType = DTA_Dummy + 101;
constexpr u32 DTA_Handle = DTA_Dummy + 102;
constexpr u32 DTA_ObjName = DTA_Dummy + 109;
constexpr u32 DTA_Domain = DTA_Dummy + 104;
constexpr u32 DTA_FrameInfo = DTA_Dummy + 116;
constexpr u32 DTA_SelectDomain = DTA_Dummy + 121;
constexpr u32 DTA_NominalVert = DTA_Dummy + 124;
constexpr u32 DTA_NominalHoriz = DTA_Dummy + 125;
constexpr u32 PDTA_ModeID = DTA_Dummy + 200;
constexpr u32 PDTA_BitMapHeader = DTA_Dummy + 201;
constexpr u32 PDTA_SourceMode = DTA_Dummy + 250;
constexpr u32 DTST_RAM = 1;
constexpr u32 DTST_FILE = 2;
constexpr u32 DTST_CLIPBOARD = 3;
constexpr u32 PMODE_V42 = 0;
constexpr u32 PMODE_V43 = 1;
constexpr u32 PBPAFMT_RGBA = 1;
constexpr u32 PDTM_READPIXELARRAY = 0x661;

// The stand-in datatypesclass's instance data, laid out as the real one's.
constexpr u32 dt_Domain = 22;
constexpr u32 dt_NominalHoriz = 54;
constexpr u32 dt_NominalVert = 58;
constexpr u32 dt_ObjName = 62;
constexpr u32 dt_FrameInfo = 90;
constexpr u32 dt_Special = 130;
constexpr u32 kStandInBytes = 246;
// The depth in struct FrameInfo, and what the display database says of a
// mode's deepest one.
constexpr u32 fri_Depth = 20;
constexpr u32 DTAG_DIMS = 0x80001000;
constexpr u32 dim_MaxDepth = 16;
constexpr u32 DimensionInfo_SIZEOF = 88;
constexpr u32 kGetDisplayInfoData = 756;

constexpr u32 DTERROR_UNKNOWN_COMPRESSION = 2006;
constexpr u32 DTERROR_NOT_ENOUGH_DATA = 2007;
constexpr u32 DTERROR_INVALID_DATA = 2008;
// What an application's OM_NEW tags carry as DTA_Handle: the lock the file
// was identified through, which is not the handle datatypesclass opens.
constexpr u32 kLock = 0x00123456;

// What reached the stand-in datatypesclass and what it answers.
struct StandIn {
    Machine* m = nullptr;
    Boopsi* bo = nullptr;
    Exec* exec = nullptr;
    Dos* dos = nullptr;
    u32 cl = 0;
    // What OM_GET answers for DTA_Handle, the file opened for the object,
    // and for DTA_SourceType, from the OM_NEW tags.
    u32 handle = 0;
    u32 source = DTST_FILE;

    u32 data(u32 object) const {
        Bus b(*m);
        return object + b.r16(cl + cl_InstOffset);
    }
};

u32 stand_in_dispatch(void* ctx, u32 cl, u32 object, u32 msg) {
    StandIn& s = *static_cast<StandIn*>(ctx);
    Bus b(*s.m);
    const u32 method = b.r32(msg);
    switch (method) {
    case OM_NEW: {
        s.source = DTST_FILE;
        for (u32 item = b.r32(msg + 4); item != 0 && b.r32(item) != 0; item += 8) {
            if (b.r32(item) == DTA_SourceType) {
                s.source = b.r32(item + 4);
            }
        }
        const u32 made = s.bo->call_super(cl, object, msg);
        if (made == 0) {
            // What datatypesclass leaves when its superclass made nothing.
            s.dos->set_io_err(ERROR_NO_FREE_STORE);
        } else {
            const u32 data = s.data(made);
            b.w32(made + gg_SpecialInfo, data + dt_Special);
            LibraryCall args;
            args.a[0] = data + dt_Special;
            u32 d0 = 0;
            CHECK(s.exec->call_library(s.exec->exec_base(), exec_lvo::InitSemaphore, args, d0));
        }
        return made;
    }
    case OM_GET: {
        const u32 data = s.data(object);
        const u32 id = b.r32(msg + 4);
        const u32 storage = b.r32(msg + 8);
        u32 value = 0;
        switch (id) {
        case DTA_Handle: value = s.handle; break;
        case DTA_SourceType: value = s.source; break;
        case DTA_Domain: value = data + dt_Domain; break;
        case DTA_SelectDomain: value = 0; break;
        case DTA_FrameInfo: value = data + dt_FrameInfo; break;
        case DTA_ObjName: value = b.r32(data + dt_ObjName); break;
        case DTA_NominalHoriz: value = b.r32(data + dt_NominalHoriz); break;
        case DTA_NominalVert: value = b.r32(data + dt_NominalVert); break;
        default: return s.bo->call_super(cl, object, msg);
        }
        b.w32(storage, value);
        return 1;
    }
    case OM_SET:
    case OM_UPDATE: {
        const u32 result = s.bo->call_super(cl, object, msg);
        const u32 data = s.data(object);
        for (u32 item = b.r32(msg + 4); item != 0 && b.r32(item) != 0; item += 8) {
            switch (b.r32(item)) {
            case DTA_ObjName: b.w32(data + dt_ObjName, b.r32(item + 4)); break;
            case DTA_NominalHoriz: b.w32(data + dt_NominalHoriz, b.r32(item + 4)); break;
            case DTA_NominalVert: b.w32(data + dt_NominalVert, b.r32(item + 4)); break;
            default: break;
            }
        }
        return result;
    }
    default:
        return s.bo->call_super(cl, object, msg);
    }
}

void task_body(void*) {}

void be32(std::vector<u8>& out, u32 value) {
    for (u32 shift = 32; shift != 0; shift -= 8) {
        out.push_back(static_cast<u8>(value >> (shift - 8)));
    }
}

void png_chunk(std::vector<u8>& png, const char* type, const std::vector<u8>& data) {
    be32(png, static_cast<u32>(data.size()));
    const usize start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    u32 crc = 0xFFFFFFFFu;
    for (usize i = start; i < png.size(); ++i) {
        crc ^= png[i];
        for (u32 bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) != 0 ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
        }
    }
    be32(png, ~crc);
}

// The four colours of the generated pictures, opaque.
constexpr u8 kPalette[4][3] = {{0, 0, 0}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}};

u32 palette_index(u32 x, u32 y) { return (x / 8 + y / 8) % 4; }

// A palette PNG of two bits a pixel whose zlib stream is stored blocks, so
// that the file is as large as its pixels. With trns it has a tRNS chunk,
// and may carry alpha although all of its colours are opaque. A corrupt one
// holds half of the pixel rows, then a block of the reserved type.
std::vector<u8> palette_png(u32 width, u32 height, bool trns, bool corrupt) {
    const u32 row_bytes = (width + 3) / 4;
    std::vector<u8> raw;
    for (u32 y = 0; y < height; ++y) {
        raw.push_back(0);
        for (u32 i = 0; i < row_bytes; ++i) {
            u32 byte = 0;
            for (u32 k = 0; k < 4; ++k) {
                const u32 x = i * 4 + k;
                byte = byte << 2 | (x < width ? palette_index(x, y) : 0);
            }
            raw.push_back(static_cast<u8>(byte));
        }
    }
    const usize stored = corrupt ? raw.size() / 2 : raw.size();
    std::vector<u8> zlib = {0x78, 0x01};
    for (usize at = 0; at < stored;) {
        const usize length = stored - at < 65535 ? stored - at : 65535;
        zlib.push_back(!corrupt && at + length == stored ? 1 : 0);
        zlib.push_back(static_cast<u8>(length));
        zlib.push_back(static_cast<u8>(length >> 8));
        zlib.push_back(static_cast<u8>(~length));
        zlib.push_back(static_cast<u8>(~length >> 8));
        zlib.insert(zlib.end(), raw.begin() + static_cast<long>(at),
                    raw.begin() + static_cast<long>(at + length));
        at += length;
    }
    if (corrupt) {
        zlib.insert(zlib.end(), {0x07, 0, 0, 0, 0, 0, 0, 0});
    }
    u32 a = 1;
    u32 b = 0;
    for (const u8 c : raw) {
        a = (a + c) % 65521u;
        b = (b + a) % 65521u;
    }
    be32(zlib, b << 16 | a);
    std::vector<u8> header;
    be32(header, width);
    be32(header, height);
    header.insert(header.end(), {2, 3, 0, 0, 0});
    std::vector<u8> palette;
    for (const auto& colour : kPalette) {
        palette.insert(palette.end(), colour, colour + 3);
    }
    std::vector<u8> png = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    png_chunk(png, "IHDR", header);
    png_chunk(png, "PLTE", palette);
    if (trns) {
        png_chunk(png, "tRNS", {255, 255, 255, 255});
    }
    png_chunk(png, "IDAT", zlib);
    png_chunk(png, "IEND", {});
    return png;
}

// The generated pictures large enough for a load to yield while it reads
// the file, scans the pixels for alpha and writes them into the object.
constexpr u32 kBigWidth = 1024;
constexpr u32 kBigHeight = 768;

std::vector<AdfFile> test_files() {
    const auto bytes = [](const unsigned char* data, usize size) {
        return std::vector<u8>(data, data + size);
    };
    const std::vector<u8> rgb8 = bytes(image_fixtures::rgb8_png_file,
                                       sizeof image_fixtures::rgb8_png_file);
    return {
        {"Big/Alpha.png", palette_png(kBigWidth, kBigHeight, true, false)},
        {"Big/Opaque.png", palette_png(kBigWidth, kBigHeight, false, false)},
        {"Bad/Corrupt.png", palette_png(64, 64, false, true)},
        {"Bad/Arithmetic.jpg", bytes(image_fixtures::arithmetic_jpg_file,
                                     sizeof image_fixtures::arithmetic_jpg_file)},
        {"Png/Rgb8.png", rgb8},
        {"Png/Rgba8.png",
         bytes(image_fixtures::rgba8_png_file, sizeof image_fixtures::rgba8_png_file)},
        {"Jpeg/B444.jpg",
         bytes(image_fixtures::b444_jpg_file, sizeof image_fixtures::b444_jpg_file)},
        {"Bad/Signature.png", std::vector<u8>(rgb8.begin(), rgb8.begin() + 8)},
        {"Bad/Partial.png", std::vector<u8>(rgb8.begin(), rgb8.begin() + 141)},
        {"Bad/Empty.png", {}},
        {"Bad/Huge.png", std::vector<u8>(256 * 1024, 0)},
    };
}

// A machine, exec, dos over an OFS volume, intuition, a datatypesclass in
// C++ and picture.datatype under the two image classes.
class Rig : public GfxRig {
public:
    // The image classes are the rig's own, or shared, kept from one rig to
    // the next as the kernel keeps them over a restart.
    explicit Rig(bool with_picture = true, ImageDataType* shared = nullptr)
        : image_(build_adf("Pics", false, test_files())),
          images_(shared != nullptr ? *shared : own_images_) {
        CHECK(adf_mount(image_.data(), image_.size(), volume_) == FsError::Ok);
        CHECK(dos_.init(machine, exec, volume_, nullptr, nullptr));
        stand_in_.m = &machine;
        stand_in_.bo = &intuition.boopsi();
        stand_in_.exec = &exec;
        stand_in_.dos = &dos_;
        stand_in_.cl = intuition.boopsi().make_native_class(
            "datatypesclass", intuition.boopsi().gadget_class(), kStandInBytes, stand_in_dispatch,
            &stand_in_);
        CHECK(stand_in_.cl != 0);
        if (with_picture) {
            CHECK(picture_.init(machine, exec, graphics, intuition));
        }
        CHECK(images_.init(machine, exec, intuition, dos_));
        png = exec.open_library("datatypes/png.datatype", 0);
        jpeg = exec.open_library("datatypes/jpeg.datatype", 0);
        if (with_picture) {
            CHECK(png != 0);
            CHECK(jpeg != 0);
        }
    }

    ~Rig() {
        exec.set_task_host(TaskHost{});
        exec.set_this_task(0);
        exec.set_segment_loader(SegmentLoader{nullptr, nullptr});
    }

    Boopsi& bo() { return intuition.boopsi(); }

    u32 get(u32 object, u32 attr) {
        put32(kStore, 0xDEADBEEF);
        CHECK(bo().get_attr(attr, object, kStore) != 0);
        return get32(kStore);
    }

    u32 send(u32 object, std::initializer_list<u32> longs) {
        std::vector<u32> values(longs);
        return bo().send(bo().class_of(object), object, values.data(),
                         static_cast<u32>(values.size()));
    }

    u32 open(const char* path) {
        str(kPath, path);
        machine.cpu.d[1] = kPath;
        machine.cpu.d[2] = MODE_OLDFILE;
        CHECK_EQ(call_at(dos_.dos_base(), dos_lvo::Open), ExitReason::Halt);
        machine.cpu.exit = ExitReason::None;
        return machine.cpu.d[0];
    }

    void close(u32 fh) {
        machine.cpu.d[1] = fh;
        CHECK_EQ(call_at(dos_.dos_base(), dos_lvo::Close), ExitReason::Halt);
        machine.cpu.exit = ExitReason::None;
    }

    // The class a library of the two made when it was opened.
    u32 class_of_library(u32 base) {
        return bo().find_class_named(base == png ? "png.datatype" : "jpeg.datatype");
    }

    u32 load(u32 library, u32 handle, const char* name, u32 source = DTST_FILE) {
        const u32 at = str(kName, name);
        stand_in_.handle = handle;
        const u32 tags = this->tags(kTags, {{DTA_SourceType, source},
                                            {DTA_Handle, kLock},
                                            {DTA_Name, at},
                                            {0, 0}});
        return bo().new_object(class_of_library(library), 0, tags);
    }

    u32 make_task(u32 addr, u32 stack_top) {
        for (u32 i = 0; i < TC_SIZE; ++i) {
            put8(addr + i, 0);
        }
        put32(addr + TC_SPLOWER, stack_top - 0x1000);
        put32(addr + TC_SPUPPER, stack_top);
        put32(addr + TC_SPREG, stack_top);
        return addr;
    }

    // The memory a DTST_RAM object of the png class takes, which a load
    // that cannot finish leaves behind.
    u32 object_cost() {
        const u32 before = exec.free_bytes();
        const u32 tags = this->tags(kTags, {{DTA_SourceType, DTST_RAM}, {0, 0}});
        const u32 object = bo().new_object(class_of_library(png), 0, tags);
        CHECK(object != 0);
        const u32 cost = before - exec.free_bytes();
        bo().dispose_object(object);
        CHECK_EQ(exec.free_bytes(), before);
        return cost;
    }

    // What a block of bytes takes from the free memory.
    u32 block_cost(u32 bytes) {
        const u32 before = exec.free_bytes();
        const u32 block = exec.alloc(bytes, MEMF_PUBLIC);
        CHECK(block != 0);
        const u32 cost = before - exec.free_bytes();
        exec.free(block, bytes);
        return cost;
    }

    // The boot shell's CLI, whose cli_Result2 SetIoErr leaves alone.
    u32 cli() { return get32(dos_.process() + pr_CLI) << 2; }

    // The deepest depth the display database gives the mode.
    u32 deepest_depth(u32 mode) {
        machine.cpu.a[0] = 0;
        machine.cpu.a[1] = kStore;
        machine.cpu.d[0] = DimensionInfo_SIZEOF;
        machine.cpu.d[1] = DTAG_DIMS;
        machine.cpu.d[2] = mode;
        CHECK_EQ(call(Base::Graphics, kGetDisplayInfoData), ExitReason::Halt);
        machine.cpu.exit = ExitReason::None;
        CHECK(machine.cpu.d[0] != 0);
        return get16(kStore + dim_MaxDepth);
    }

    std::vector<u8> image_;
    AdfVolume volume_{};
    Dos dos_{};
    StandIn stand_in_{};
    PictureDataType picture_{};
    ImageDataType own_images_{};
    ImageDataType& images_;
    u32 png = 0;
    u32 jpeg = 0;
};

void warm(Rig& t) {
    const u32 fh = t.open("Png/Rgb8.png");
    CHECK(fh != 0);
    const u32 object = t.load(t.png, fh, "warm.png");
    CHECK(object != 0);
    t.bo().dispose_object(object);
    t.close(fh);
    gfx_test::g_lines.clear();
}

void check_header(Rig& t, u32 object, u32 width, u32 height, u32 masking) {
    const u32 bmh = t.get(object, PDTA_BitMapHeader);
    CHECK(bmh != 0);
    CHECK_EQ(t.get16(bmh + 0), width);
    CHECK_EQ(t.get16(bmh + 2), height);
    CHECK_EQ(t.get16(bmh + 4), 0u);
    CHECK_EQ(t.get16(bmh + 6), 0u);
    CHECK_EQ(t.get8(bmh + 8), 24u);
    CHECK_EQ(t.get8(bmh + 9), masking);
    CHECK_EQ(t.get8(bmh + 10), 0u);
    CHECK_EQ(t.get16(bmh + 12), 0u);
    CHECK_EQ(t.get8(bmh + 14), 1u);
    CHECK_EQ(t.get8(bmh + 15), 1u);
    CHECK_EQ(t.get16(bmh + 16), width);
    CHECK_EQ(t.get16(bmh + 18), height);
}

// The pixels the object gives back, against the fixture's, within a
// channel tolerance; alpha is compared as the fixture has it.
void check_pixels(Rig& t, u32 object, u32 width, u32 height, const unsigned char* expected,
                  u32 tolerance) {
    CHECK_EQ(t.send(object, {PDTM_READPIXELARRAY, kPixels, PBPAFMT_RGBA, width * 4, 0, 0, width,
                             height}),
             1u);
    for (u32 i = 0; i < width * height; ++i) {
        for (u32 c = 0; c < 4; ++c) {
            const u32 got = t.get8(kPixels + i * 4 + c);
            const u32 want = expected[i * 4 + c];
            const u32 difference = got > want ? got - want : want - got;
            CHECK(difference <= (c == 3 ? 0u : tolerance));
        }
    }
}

void opens_both_libraries_over_picture_datatype() {
    Rig t;
    CHECK(t.png != 0);
    CHECK(t.jpeg != 0);
    CHECK_EQ(t.get16(t.png + LIB_VERSION), kHombreLibVersion);
    CHECK(t.read_str(t.get32(t.png + LN_NAME)) == "png.datatype");
    CHECK(t.read_str(t.get32(t.jpeg + LN_NAME)) == "jpeg.datatype");
    const u32 picture = t.bo().find_class_named("picture.datatype");
    CHECK(picture != 0);
    for (const u32 base : {t.png, t.jpeg}) {
        CHECK_EQ(t.call_at(base, 30), ExitReason::Halt);
        const u32 cl = t.machine.cpu.d[0];
        CHECK(cl != 0);
        CHECK_EQ(t.get32(cl + cl_UserData), base);
        CHECK_EQ(t.get32(cl + cl_Super), picture);
        CHECK_EQ(t.get16(cl + cl_InstSize), 0u);
    }
}

void png_without_alpha_loads() {
    Rig t;
    warm(t);
    const u32 before = t.exec.free_bytes();
    const u32 fh = t.open("Png/Rgb8.png");
    const u32 object = t.load(t.png, fh, "rgb8.png");
    CHECK(object != 0);
    check_header(t, object, 23, 17, 0);
    CHECK_EQ(t.get(object, PDTA_SourceMode), PMODE_V43);
    CHECK_EQ(t.get(object, DTA_NominalHoriz), 23u);
    CHECK_EQ(t.get(object, DTA_NominalVert), 17u);
    CHECK_EQ(t.get(object, DTA_ObjName), kName);
    check_pixels(t, object, 23, 17, image_fixtures::rgb8_png_rgba, 0);
    CHECK(gfx_test::logged("png.datatype: 23x17 loaded"));
    // The mode came after the pixels, so that the frame's depth is the
    // mode's deepest.
    CHECK_EQ(t.get(object, PDTA_ModeID), 0u);
    const u32 depth = t.get32(t.stand_in_.data(object) + dt_FrameInfo + fri_Depth);
    CHECK(depth < 24);
    CHECK_EQ(depth, t.deepest_depth(0));
    t.bo().dispose_object(object);
    t.close(fh);
    CHECK_EQ(t.exec.free_bytes(), before);
}

void png_with_alpha_loads() {
    Rig t;
    warm(t);
    const u32 before = t.exec.free_bytes();
    const u32 fh = t.open("Png/Rgba8.png");
    const u32 object = t.load(t.png, fh, "rgba8.png");
    CHECK(object != 0);
    check_header(t, object, 23, 17, 4);
    check_pixels(t, object, 23, 17, image_fixtures::rgba8_png_rgba, 0);
    t.bo().dispose_object(object);
    t.close(fh);
    CHECK_EQ(t.exec.free_bytes(), before);
}

void jpeg_loads() {
    Rig t;
    warm(t);
    const u32 before = t.exec.free_bytes();
    const u32 fh = t.open("Jpeg/B444.jpg");
    const u32 object = t.load(t.jpeg, fh, "b444.jpg");
    CHECK(object != 0);
    check_header(t, object, 41, 33, 0);
    CHECK_EQ(t.get(object, PDTA_SourceMode), PMODE_V43);
    check_pixels(t, object, 41, 33, image_fixtures::b444_jpg_rgba, 3);
    CHECK(gfx_test::logged("jpeg.datatype: 41x33 loaded"));
    t.bo().dispose_object(object);
    t.close(fh);
    CHECK_EQ(t.exec.free_bytes(), before);
}

void a_partial_png_is_shown() {
    Rig t;
    warm(t);
    const u32 before = t.exec.free_bytes();
    const u32 fh = t.open("Bad/Partial.png");
    const u32 object = t.load(t.png, fh, "partial.png");
    CHECK(object != 0);
    CHECK_EQ(t.send(object, {PDTM_READPIXELARRAY, kPixels, PBPAFMT_RGBA, 23 * 4, 0, 0, 23, 17}),
             1u);
    u32 matched = 0;
    u32 other = 0;
    for (u32 i = 0; i < 23 * 17; ++i) {
        bool same = true;
        bool clear = true;
        for (u32 c = 0; c < 4; ++c) {
            const u32 got = t.get8(kPixels + i * 4 + c);
            same = same && got == image_fixtures::rgb8_png_rgba[i * 4 + c];
            clear = clear && got == 0;
        }
        if (same) {
            ++matched;
        } else if (!clear) {
            ++other;
        }
    }
    // Some of the picture, and the rest clear, which is transparent.
    CHECK(matched != 0);
    CHECK(matched < 23 * 17);
    CHECK_EQ(other, 0u);
    check_header(t, object, 23, 17, 4);
    CHECK(gfx_test::logged("png.datatype: 23x17 loaded"));
    t.bo().dispose_object(object);
    t.close(fh);
    CHECK_EQ(t.exec.free_bytes(), before);
}

void a_ram_object_is_returned_as_it_is() {
    Rig t;
    warm(t);
    const u32 before = t.exec.free_bytes();
    const u32 tags = t.tags(kTags, {{DTA_SourceType, DTST_RAM}, {0, 0}});
    const u32 object = t.bo().new_object(t.class_of_library(t.png), 0, tags);
    CHECK(object != 0);
    CHECK(t.get(object, PDTA_BitMapHeader) != 0);
    CHECK_EQ(t.get(object, PDTA_SourceMode), PMODE_V42);
    t.bo().dispose_object(object);
    CHECK_EQ(t.exec.free_bytes(), before);
}

void the_wrong_format_is_refused() {
    for (const bool png : {true, false}) {
        Rig t;
        warm(t);
        const u32 before = t.exec.free_bytes();
        const u32 fh = t.open(png ? "Jpeg/B444.jpg" : "Png/Rgb8.png");
        // The error goes where SetIoErr puts it, the CLI's is left alone.
        const u32 cli = t.cli();
        CHECK(cli != 0);
        t.put32(cli + cli_Result2, 0x1234);
        const u32 object = t.load(png ? t.png : t.jpeg, fh, "wrong");
        CHECK_EQ(object, 0u);
        CHECK_EQ(t.dos_.last_error(), ERROR_OBJECT_WRONG_TYPE);
        CHECK_EQ(t.get32(cli + cli_Result2), 0x1234u);
        CHECK(gfx_test::logged(png ? "png.datatype: could not load, error 212"
                                   : "jpeg.datatype: could not load, error 212"));
        t.close(fh);
        CHECK_EQ(t.exec.free_bytes(), before);
    }
}

void a_truncated_header_is_refused() {
    Rig t;
    warm(t);
    const u32 before = t.exec.free_bytes();
    const u32 fh = t.open("Bad/Signature.png");
    const u32 object = t.load(t.png, fh, "signature.png");
    CHECK_EQ(object, 0u);
    CHECK_EQ(t.dos_.last_error(), DTERROR_NOT_ENOUGH_DATA);
    CHECK(gfx_test::logged("png.datatype: could not load, error 2007"));
    t.close(fh);
    CHECK_EQ(t.exec.free_bytes(), before);
}

void an_unsupported_coding_is_refused() {
    Rig t;
    warm(t);
    const u32 before = t.exec.free_bytes();
    const u32 fh = t.open("Bad/Arithmetic.jpg");
    const u32 object = t.load(t.jpeg, fh, "arithmetic.jpg");
    CHECK_EQ(object, 0u);
    CHECK_EQ(t.dos_.last_error(), DTERROR_UNKNOWN_COMPRESSION);
    CHECK(gfx_test::logged("jpeg.datatype: could not load, error 2006"));
    t.close(fh);
    CHECK_EQ(t.exec.free_bytes(), before);
}

void data_corrupt_in_mid_stream_is_refused() {
    Rig t;
    warm(t);
    const u32 before = t.exec.free_bytes();
    const u32 fh = t.open("Bad/Corrupt.png");
    const u32 object = t.load(t.png, fh, "corrupt.png");
    CHECK_EQ(object, 0u);
    CHECK_EQ(t.dos_.last_error(), DTERROR_INVALID_DATA);
    CHECK(gfx_test::logged("png.datatype: could not load, error 2008"));
    t.close(fh);
    CHECK_EQ(t.exec.free_bytes(), before);
}

void a_source_other_than_a_file_is_refused() {
    Rig t;
    warm(t);
    const u32 before = t.exec.free_bytes();
    const u32 fh = t.open("Png/Rgb8.png");
    const u32 object = t.load(t.png, fh, "clip", DTST_CLIPBOARD);
    CHECK_EQ(object, 0u);
    CHECK_EQ(t.dos_.last_error(), ERROR_OBJECT_WRONG_TYPE);
    CHECK(gfx_test::logged("png.datatype: could not load, error 212"));
    t.close(fh);
    CHECK_EQ(t.exec.free_bytes(), before);
}

void an_empty_file_is_refused() {
    Rig t;
    warm(t);
    const u32 before = t.exec.free_bytes();
    const u32 fh = t.open("Bad/Empty.png");
    const u32 object = t.load(t.png, fh, "empty.png");
    CHECK_EQ(object, 0u);
    CHECK_EQ(t.dos_.last_error(), DTERROR_INVALID_DATA);
    t.close(fh);
    CHECK_EQ(t.exec.free_bytes(), before);
}

void a_file_without_a_handle_is_refused() {
    Rig t;
    warm(t);
    const u32 before = t.exec.free_bytes();
    const u32 object = t.load(t.png, 0, "none.png");
    CHECK_EQ(object, 0u);
    CHECK_EQ(t.dos_.last_error(), ERROR_OBJECT_WRONG_TYPE);
    CHECK_EQ(t.exec.free_bytes(), before);
}

void a_load_without_memory_is_refused() {
    constexpr u32 kChunk = 64 * 1024;
    constexpr u32 kReserve = 4096;
    // Enough left for the object the superclass makes, too little for the
    // file, whatever the two memory regions' remainders are.
    constexpr u32 kKeep = 80 * 1024;
    Rig t;
    warm(t);
    const u32 fh = t.open("Bad/Huge.png");
    const u32 before = t.exec.free_bytes();
    const u32 reserve = t.exec.alloc(kReserve, MEMF_PUBLIC);
    CHECK(reserve != 0);
    std::vector<std::pair<u32, u32>> held;
    while (t.exec.free_bytes() > kKeep) {
        const u32 block = t.exec.alloc(kChunk, MEMF_PUBLIC);
        if (block == 0) {
            break;
        }
        held.emplace_back(block, kChunk);
    }
    t.exec.free(reserve, kReserve);
    CHECK(t.exec.free_bytes() < 256u * 1024u);
    const u32 object = t.load(t.png, fh, "huge.png");
    CHECK_EQ(object, 0u);
    CHECK_EQ(t.dos_.last_error(), ERROR_NO_FREE_STORE);
    CHECK(gfx_test::logged("png.datatype: could not load, error 103"));
    for (const auto& [block, bytes] : held) {
        t.exec.free(block, bytes);
    }
    CHECK_EQ(t.exec.free_bytes(), before);
    t.close(fh);
}

// Takes all of the free memory but budget bytes in one block, and gives it
// back when it goes.
class Starve {
public:
    Starve(Rig& t, u32 budget) : t_(t) {
        const u32 keep = budget != 0 ? t.exec.alloc(budget, MEMF_PUBLIC) : 0;
        CHECK(budget == 0 || keep != 0);
        for (u32 size = 1u << 20; size >= 8; size /= 2) {
            for (u32 block = t.exec.alloc(size, MEMF_PUBLIC); block != 0;
                 block = t.exec.alloc(size, MEMF_PUBLIC)) {
                held_.emplace_back(block, size);
            }
        }
        if (keep != 0) {
            t.exec.free(keep, budget);
        }
        CHECK_EQ(t.exec.free_bytes(), (budget + 7) & ~7u);
    }
    ~Starve() {
        for (const auto& [block, bytes] : held_) {
            t_.exec.free(block, bytes);
        }
    }
    Starve(const Starve&) = delete;
    Starve& operator=(const Starve&) = delete;

private:
    Rig& t_;
    std::vector<std::pair<u32, u32>> held_;
};

// Every amount of memory from none to what a whole load takes: a load that
// cannot finish gives all of it back and says so in IoErr, unless not even
// the caller's message to the class could be made.
void a_load_short_of_memory_gives_all_of_it_back() {
    Rig t;
    warm(t);
    const u32 fh = t.open("Png/Rgb8.png");
    const u32 caller = t.block_cost(12);
    const u32 before = t.exec.free_bytes();
    u32 failures = 0;
    u32 object = 0;
    for (u32 budget = 0; object == 0 && budget < 256u * 1024u; budget += 8) {
        Starve starve(t, budget);
        t.dos_.set_io_err(0x1234);
        object = t.load(t.png, fh, "rgb8.png");
        if (object == 0) {
            ++failures;
            CHECK_EQ(t.exec.free_bytes(), budget);
            CHECK_EQ(t.dos_.last_error(), budget < caller ? 0x1234u : ERROR_NO_FREE_STORE);
        }
    }
    CHECK(object != 0);
    CHECK(failures > 16);
    check_header(t, object, 23, 17, 0);
    check_pixels(t, object, 23, 17, image_fixtures::rgb8_png_rgba, 0);
    t.bo().dispose_object(object);
    CHECK_EQ(t.exec.free_bytes(), before);
    t.close(fh);
}

// A task host whose chosen switch does what another task would: end the
// run, remove the task that is loading, or start a load of its own. running
// is the task whose C++ runs: a switch to it only tells the scheduler so,
// and a switch away from it comes straight back once the other task has
// had its turn, unless the run ended or the task is gone.
struct Host {
    Exec* exec = nullptr;
    Machine* machine = nullptr;
    u32 target = 0;
    u32 running = 1;
    u32 switches = 0;
    u32 act_at = 1;
    // Set when the target's load is over, after which nothing is done.
    const bool* done = nullptr;
    bool end_run = false;
    bool remove = false;
    bool removed = false;
    bool back = false;
    u32 free_before_remove = 0;
    u32 free_after_remove = 0;
    void (*during)(void* ctx) = nullptr;
    void* during_ctx = nullptr;
};

bool host_create(void*, u32) { return true; }
void host_destroy(void*, u32) {}

void host_switch(void* ctx, u32, u32 to) {
    Host& h = *static_cast<Host*>(ctx);
    if (h.back || h.removed) {
        return;
    }
    ++h.switches;
    if (h.switches == h.act_at && (h.done == nullptr || !*h.done)) {
        if (h.end_run) {
            h.machine->cpu.exit = ExitReason::BusError;
            return;
        }
        if (h.remove) {
            h.free_before_remove = h.exec->free_bytes();
            LibraryCall args;
            args.a[1] = h.target;
            u32 d0 = 0;
            h.exec->call_library(h.exec->exec_base(), exec_lvo::RemTask, args, d0);
            h.free_after_remove = h.exec->free_bytes();
            h.removed = true;
            return;
        }
        if (h.during != nullptr && to != h.running) {
            const u32 was = h.running;
            h.running = to;
            h.during(h.during_ctx);
            h.running = was;
        }
    }
    if (to != h.running) {
        h.back = true;
        h.exec->yield_in_native();
        h.back = false;
    }
}

TaskHost host_task(Host& h) {
    return TaskHost{host_create, host_switch, host_destroy, nullptr,
                    nullptr,     nullptr,     nullptr,      &h};
}

void a_run_that_ends_during_a_yield_stops_the_load() {
    Rig t;
    warm(t);
    const u32 boot = t.exec.this_task();
    const u32 cost = t.object_cost();
    const u32 fh = t.open("Png/Rgb8.png");
    Host h;
    h.exec = &t.exec;
    h.machine = &t.machine;
    h.running = 0;
    h.end_run = true;
    t.exec.set_task_host(host_task(h));
    const u32 yielder = t.make_task(kYielderTask, kYielderStack);
    CHECK_EQ(t.exec.spawn_native(yielder, task_body, nullptr, 0, false), yielder);
    const u32 held = t.exec.free_bytes();
    t.dos_.set_last_error(0x1234);
    const u32 object = t.load(t.png, fh, "rgb8.png");
    CHECK_EQ(object, 0u);
    CHECK(h.switches != 0);
    CHECK_EQ(t.machine.cpu.exit, ExitReason::BusError);
    t.machine.cpu.exit = ExitReason::None;
    t.exec.set_task_host(TaskHost{});
    t.exec.set_this_task(boot);
    // The object the superclass made is left behind, nothing else is.
    CHECK_EQ(t.exec.free_bytes(), held - cost);
    // A run that ended is not told what the file was like.
    CHECK_EQ(t.dos_.last_error(), 0x1234u);
    t.close(fh);
}

struct LoadJob {
    Rig* rig = nullptr;
    u32 handle = 0;
    u32 object = 0;
    const char* name = "rgb8.png";
    bool done = false;
};

void loader_body(void* arg) {
    LoadJob& job = *static_cast<LoadJob*>(arg);
    job.object = job.rig->load(job.rig->png, job.handle, job.name);
    job.done = true;
}

void a_task_removed_during_a_yield_gives_its_buffers_back() {
    Rig t;
    warm(t);
    const u32 boot = t.exec.this_task();
    const u32 cost = t.object_cost();
    const u32 caller = t.block_cost(12);
    const u32 fh = t.open("Png/Rgb8.png");
    LoadJob job;
    job.rig = &t;
    job.handle = fh;
    job.object = 0xDEADBEEF;
    const u32 loader = t.make_task(kLoaderTask, kLoaderStack);
    CHECK_EQ(t.exec.spawn_native(loader, loader_body, &job, 0, false), loader);
    const u32 held = t.exec.free_bytes();
    Host h;
    h.exec = &t.exec;
    h.machine = &t.machine;
    h.target = loader;
    h.remove = true;
    t.exec.set_task_host(host_task(h));
    t.exec.task_main(1);
    t.exec.set_task_host(TaskHost{});
    t.exec.set_this_task(boot);
    CHECK_EQ(job.object, 0u);
    CHECK(h.removed);
    // At the removal the hook had already given the load's blocks back:
    // only the object and the caller's message are left.
    CHECK_EQ(h.free_after_remove, held - cost - caller);
    // The stack the removal left behind then gave the message back.
    CHECK_EQ(t.exec.free_bytes(), held - cost);
    t.close(fh);
}

// Where the loads that a removal stopped were.
struct Stops {
    u32 read = 0;
    u32 decode = 0;
    u32 scan = 0;
    u32 write = 0;
};

// Loads one of the large pictures in a task that is removed at its first
// yield, then at its second, and so on until a load ends first. What is
// left at each removal is the object and the caller's message, and the
// superclass's pixel buffer once the write has begun; what the hook gave
// back tells where the load was.
Stops remove_at_every_yield(const char* path, u32 file_bytes) {
    const u32 pixel_bytes = kBigWidth * kBigHeight * 4;
    Stops seen;
    for (u32 at = 1;; ++at) {
        CHECK(at < 200);
        if (at >= 200) {
            break;
        }
        Rig t;
        warm(t);
        const u32 boot = t.exec.this_task();
        const u32 cost = t.object_cost();
        const u32 caller = t.block_cost(12);
        const u32 fh = t.open(path);
        LoadJob job;
        job.rig = &t;
        job.handle = fh;
        job.object = 0xDEADBEEF;
        job.name = "big.png";
        const u32 loader = t.make_task(kLoaderTask, kLoaderStack);
        CHECK_EQ(t.exec.spawn_native(loader, loader_body, &job, 0, false), loader);
        const u32 held = t.exec.free_bytes();
        Host h;
        h.exec = &t.exec;
        h.machine = &t.machine;
        h.target = loader;
        h.remove = true;
        h.act_at = at;
        h.done = &job.done;
        t.exec.set_task_host(host_task(h));
        t.exec.task_main(1);
        t.exec.set_task_host(TaskHost{});
        t.exec.set_this_task(boot);
        if (!h.removed) {
            // The load was over before this yield.
            CHECK(at > 1);
            CHECK(job.object != 0);
            check_header(t, job.object, kBigWidth, kBigHeight, 0);
            t.bo().dispose_object(job.object);
            t.close(fh);
            CHECK_EQ(t.exec.free_bytes(), held);
            break;
        }
        CHECK_EQ(job.object, 0u);
        const u32 left = held - cost - caller;
        const u32 freed = h.free_after_remove - h.free_before_remove;
        if (h.free_after_remove == left - pixel_bytes) {
            ++seen.write;
        } else {
            CHECK_EQ(h.free_after_remove, left);
            if (freed < pixel_bytes) {
                ++seen.read;
            } else if (freed < pixel_bytes + file_bytes) {
                ++seen.scan;
            } else {
                ++seen.decode;
            }
        }
        CHECK_EQ(t.exec.free_bytes(), h.free_after_remove + caller);
        t.close(fh);
    }
    return seen;
}

void a_task_removed_at_any_yield_gives_its_buffers_back() {
    const u32 file = static_cast<u32>(palette_png(kBigWidth, kBigHeight, true, false).size());
    // The file is read 64 KiB at a time.
    const u32 reads = (file + 65535) / 65536 - 1;
    const Stops alpha = remove_at_every_yield("Big/Alpha.png", file);
    CHECK_EQ(alpha.read, reads);
    CHECK(alpha.decode != 0);
    // A picture that may carry alpha is scanned 256 Ki pixels at a time.
    CHECK_EQ(alpha.scan, kBigWidth * kBigHeight / 262144 - 1);
    CHECK(alpha.write != 0);
    // One that cannot is not scanned.
    const Stops opaque = remove_at_every_yield(
        "Big/Opaque.png",
        static_cast<u32>(palette_png(kBigWidth, kBigHeight, false, false).size()));
    CHECK_EQ(opaque.read, reads);
    CHECK(opaque.decode != 0);
    CHECK_EQ(opaque.scan, 0u);
    CHECK(opaque.write != 0);
}

struct SecondLoad {
    Rig* rig = nullptr;
    u32 handle = 0;
    u32 object = 0;
};

void second_load(void* ctx) {
    SecondLoad& second = *static_cast<SecondLoad*>(ctx);
    second.object = second.rig->load(second.rig->png, second.handle, "rgb8.png");
}

// One load begins and yields while it reads its file; another, from the
// task that runs then, loads a picture of its own in the meantime.
void two_loads_in_flight_keep_apart() {
    Rig t;
    warm(t);
    const u32 boot = t.exec.this_task();
    const u32 big = t.open("Big/Alpha.png");
    const u32 small = t.open("Png/Rgb8.png");
    LoadJob job;
    job.rig = &t;
    job.handle = big;
    job.object = 0xDEADBEEF;
    job.name = "alpha.png";
    const u32 loader = t.make_task(kLoaderTask, kLoaderStack);
    CHECK_EQ(t.exec.spawn_native(loader, loader_body, &job, 0, false), loader);
    const u32 held = t.exec.free_bytes();
    SecondLoad second;
    second.rig = &t;
    second.handle = small;
    Host h;
    h.exec = &t.exec;
    h.machine = &t.machine;
    h.act_at = 2;
    h.done = &job.done;
    h.during = second_load;
    h.during_ctx = &second;
    t.exec.set_task_host(host_task(h));
    t.exec.task_main(1);
    t.exec.set_task_host(TaskHost{});
    t.exec.set_this_task(boot);
    CHECK(job.done);
    CHECK(second.object != 0);
    CHECK(job.object != 0);
    check_header(t, second.object, 23, 17, 0);
    check_pixels(t, second.object, 23, 17, image_fixtures::rgb8_png_rgba, 0);
    check_header(t, job.object, kBigWidth, kBigHeight, 0);
    for (const u32 y : {0u, kBigHeight / 2, kBigHeight - 1}) {
        CHECK_EQ(t.send(job.object, {PDTM_READPIXELARRAY, kPixels, PBPAFMT_RGBA, kBigWidth * 4,
                                     0, y, kBigWidth, 1}),
                 1u);
        for (u32 x = 0; x < kBigWidth; ++x) {
            const u8* colour = kPalette[palette_index(x, y)];
            CHECK_EQ(t.get8(kPixels + x * 4), colour[0]);
            CHECK_EQ(t.get8(kPixels + x * 4 + 1), colour[1]);
            CHECK_EQ(t.get8(kPixels + x * 4 + 2), colour[2]);
            CHECK_EQ(t.get8(kPixels + x * 4 + 3), 255u);
        }
    }
    t.bo().dispose_object(second.object);
    t.bo().dispose_object(job.object);
    t.close(small);
    t.close(big);
    CHECK_EQ(t.exec.free_bytes(), held);
}

// The classes kept over a restart open picture.datatype again in the new
// system, and their loads start afresh.
void init_starts_the_classes_afresh() {
    ImageDataType shared;
    {
        Rig first(true, &shared);
        warm(first);
    }
    Rig t(true, &shared);
    const u32 picture = t.exec.open_library("datatypes/picture.datatype", 0);
    CHECK(picture != 0);
    // The two classes' opens and this one.
    CHECK_EQ(t.get16(picture + LIB_OPENCNT), 3u);
    t.exec.close_library(picture);
    warm(t);
}

void without_picture_datatype_the_class_does_not_open() {
    Rig t(false);
    CHECK_EQ(t.png, 0u);
    CHECK_EQ(t.jpeg, 0u);
    CHECK(gfx_test::logged("png.datatype: no picture.datatype"));
    CHECK(gfx_test::logged("jpeg.datatype: no picture.datatype"));
}

}

int main() {
    opens_both_libraries_over_picture_datatype();
    png_without_alpha_loads();
    png_with_alpha_loads();
    jpeg_loads();
    a_partial_png_is_shown();
    a_ram_object_is_returned_as_it_is();
    the_wrong_format_is_refused();
    a_truncated_header_is_refused();
    an_unsupported_coding_is_refused();
    data_corrupt_in_mid_stream_is_refused();
    a_source_other_than_a_file_is_refused();
    an_empty_file_is_refused();
    a_file_without_a_handle_is_refused();
    a_load_without_memory_is_refused();
    a_load_short_of_memory_gives_all_of_it_back();
    a_run_that_ends_during_a_yield_stops_the_load();
    a_task_removed_during_a_yield_gives_its_buffers_back();
    a_task_removed_at_any_yield_gives_its_buffers_back();
    two_loads_in_flight_keep_apart();
    init_starts_the_classes_afresh();
    without_picture_datatype_the_class_does_not_open();
    return check::finish("test_image_dt");
}
