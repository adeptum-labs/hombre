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

#include "amiga_fs.hpp"
#include "check.hpp"
#include "image/decode.hpp"
#include "image_fixtures.hpp"
#include "pc_partitions.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace hombre;
using hombre::image::Allocator;
using hombre::image::decode;
using hombre::image::Format;
using hombre::image::Info;
using hombre::image::probe;
using hombre::image::Progress;
using hombre::image::Status;

namespace {

constexpr usize kGuard = 32;
constexpr usize kMutationCount = 4000;
constexpr usize kInputStep = 65536;
constexpr usize kClearStep = 1024 * 1024;

struct AllocCounts {
    usize live = 0;
    usize bytes = 0;
    usize calls = 0;
    usize fail_at = ~usize{0};
};

void* counted_alloc(void* ctx, usize bytes) {
    AllocCounts& counts = *static_cast<AllocCounts*>(ctx);
    const usize call = counts.calls++;
    if (call == counts.fail_at) {
        return nullptr;
    }
    void* block = std::malloc(bytes);
    if (block != nullptr) {
        ++counts.live;
        counts.bytes += bytes;
    }
    return block;
}

void counted_release(void* ctx, void* block, usize bytes) {
    AllocCounts& counts = *static_cast<AllocCounts*>(ctx);
    CHECK(block != nullptr);
    CHECK(counts.live != 0);
    CHECK(counts.bytes >= bytes);
    --counts.live;
    counts.bytes -= bytes;
    std::free(block);
}

Allocator allocator(AllocCounts& counts) {
    return Allocator{&counts, counted_alloc, counted_release};
}

struct PixelBuffer {
    explicit PixelBuffer(usize bytes)
        : storage(bytes + 2 * kGuard, 0xA5), pixels(storage.data() + kGuard), bytes(bytes) {}

    std::vector<u8> storage;
    u8* pixels;
    usize bytes;
};

void check_guards(const PixelBuffer& buffer) {
    for (usize i = 0; i < kGuard; ++i) {
        CHECK_EQ(buffer.storage[i], 0xA5u);
        CHECK_EQ(buffer.storage[kGuard + buffer.bytes + i], 0xA5u);
    }
}

struct ProgressCounts {
    usize calls = 0;
    usize stop_at = ~usize{0};
};

bool counted_progress(void* ctx) {
    ProgressCounts& counts = *static_cast<ProgressCounts*>(ctx);
    const bool keep_going = counts.calls != counts.stop_at;
    ++counts.calls;
    return keep_going;
}

Progress progress_for(ProgressCounts& counts) {
    return Progress{&counts, counted_progress};
}

const Progress no_progress{nullptr, nullptr};

// Decodes with fresh counting allocators and checks that nothing leaked and
// nothing was written outside the pixels.
Status decode_checked(const u8* file, usize bytes, PixelBuffer& pixels,
                      const Progress& progress = no_progress) {
    AllocCounts host_counts;
    AllocCounts work_counts;
    const Status status = decode(file, bytes, allocator(host_counts), allocator(work_counts),
                                 pixels.pixels, pixels.bytes, progress);
    check_guards(pixels);
    CHECK_EQ(host_counts.live, 0u);
    CHECK_EQ(work_counts.live, 0u);
    return status;
}

Status probe_checked(const u8* file, usize bytes, Info& info) {
    AllocCounts counts;
    const Status status = probe(file, bytes, allocator(counts), info);
    CHECK_EQ(counts.live, 0u);
    return status;
}

bool named_png(const char* name) {
    const usize length = std::strlen(name);
    return length >= 4 && std::strcmp(name + length - 4, ".png") == 0;
}

bool is_16_bit(const char* name) {
    return std::strstr(name, "16") != nullptr;
}

const image_fixtures::Picture& fixture(const char* name) {
    for (const image_fixtures::Picture& picture : image_fixtures::kPictures) {
        if (std::strcmp(picture.name, name) == 0) {
            return picture;
        }
    }
    std::fprintf(stderr, "no fixture %s\n", name);
    std::abort();
}

usize picture_bytes(const image_fixtures::Picture& picture) {
    return usize{picture.width} * picture.height * 4;
}

unsigned difference(unsigned a, unsigned b) {
    return a > b ? a - b : b - a;
}

unsigned tolerance_of(const image_fixtures::Picture& picture) {
    return picture.tolerance + (is_16_bit(picture.name) ? 1u : 0u);
}

// True when the pixel at `at` matches the fixture. A pixel that is fully
// transparent matches whatever its colour: the colour carries nothing.
bool pixel_matches(const image_fixtures::Picture& picture, const u8* pixels, usize at) {
    const unsigned tolerance = tolerance_of(picture);
    if (picture.rgba[at + 3] == 0 && pixels[at + 3] == 0) {
        return true;
    }
    for (usize i = 0; i < 4; ++i) {
        if (difference(picture.rgba[at + i], pixels[at + i]) > tolerance) {
            return false;
        }
    }
    return true;
}

bool pixel_is_clear(const u8* pixels, usize at) {
    return pixels[at] == 0 && pixels[at + 1] == 0 && pixels[at + 2] == 0 && pixels[at + 3] == 0;
}

void check_pixels(const image_fixtures::Picture& picture, const u8* pixels) {
    const usize bytes = picture_bytes(picture);
    for (usize at = 0; at < bytes; at += 4) {
        if (!pixel_matches(picture, pixels, at)) {
            std::fprintf(stderr, "%s: pixel %zu differs\n", picture.name, at / 4);
            CHECK(false);
        }
    }
}

u32 read_be32(const u8* at) {
    return u32{at[0]} << 24 | u32{at[1]} << 16 | u32{at[2]} << 8 | u32{at[3]};
}

void put_be32(u8* at, u32 value) {
    at[0] = static_cast<u8>(value >> 24);
    at[1] = static_cast<u8>(value >> 16);
    at[2] = static_cast<u8>(value >> 8);
    at[3] = static_cast<u8>(value);
}

void append_be32(std::vector<u8>& out, u32 value) {
    u8 bytes[4];
    put_be32(bytes, value);
    out.insert(out.end(), bytes, bytes + 4);
}

u32 crc32_of(const u8* data, usize bytes) {
    u32 crc = 0xFFFFFFFFu;
    for (usize i = 0; i < bytes; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

void append_chunk(std::vector<u8>& png, const char* type, const std::vector<u8>& data) {
    append_be32(png, static_cast<u32>(data.size()));
    const usize start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    append_be32(png, crc32_of(png.data() + start, png.size() - start));
}

// A zlib stream of stored deflate blocks, so the file is as large as the
// pixels it holds.
std::vector<u8> stored_zlib(const std::vector<u8>& raw) {
    std::vector<u8> out = {0x78, 0x01};
    usize at = 0;
    do {
        const usize length = raw.size() - at < 65535 ? raw.size() - at : 65535;
        const bool last = at + length == raw.size();
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<u8>(length));
        out.push_back(static_cast<u8>(length >> 8));
        out.push_back(static_cast<u8>(~length));
        out.push_back(static_cast<u8>(~length >> 8));
        out.insert(out.end(), raw.begin() + static_cast<long>(at),
                   raw.begin() + static_cast<long>(at + length));
        at += length;
    } while (at < raw.size());
    u32 a = 1;
    u32 b = 0;
    for (const u8 byte : raw) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    append_be32(out, b << 16 | a);
    return out;
}

struct BuiltPng {
    std::vector<u8> file;
    std::vector<u8> rgba;
    u32 width;
    u32 height;
};

// Noise in an 8-bit RGB or a 1-bit grey PNG, its IDAT split in chunks of
// `idat_bytes`.
BuiltPng noise_png(u32 width, u32 height, bool one_bit, usize idat_bytes) {
    BuiltPng built{{}, {}, width, height};
    u64 state = 0x2545F4914F6CDD1Dull;
    const usize row_bytes = one_bit ? (width + 7) / 8 : usize{width} * 3;
    std::vector<u8> raw;
    built.rgba.reserve(usize{width} * height * 4);
    for (u32 y = 0; y < height; ++y) {
        raw.push_back(0);
        const usize row = raw.size();
        for (usize i = 0; i < row_bytes; ++i) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            raw.push_back(static_cast<u8>(state));
        }
        for (u32 x = 0; x < width; ++x) {
            if (one_bit) {
                const u8 grey = (raw[row + x / 8] >> (7 - x % 8)) & 1 ? 255 : 0;
                built.rgba.insert(built.rgba.end(), {grey, grey, grey, 255});
            } else {
                const u8* rgb = &raw[row + usize{x} * 3];
                built.rgba.insert(built.rgba.end(), {rgb[0], rgb[1], rgb[2], 255});
            }
        }
    }
    const std::vector<u8> zlib = stored_zlib(raw);
    built.file = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<u8> header;
    append_be32(header, width);
    append_be32(header, height);
    header.insert(header.end(), {static_cast<u8>(one_bit ? 1 : 8),
                                 static_cast<u8>(one_bit ? 0 : 2), 0, 0, 0});
    append_chunk(built.file, "IHDR", header);
    for (usize at = 0; at < zlib.size(); at += idat_bytes) {
        const usize end = zlib.size() - at < idat_bytes ? zlib.size() : at + idat_bytes;
        append_chunk(built.file, "IDAT",
                     std::vector<u8>(zlib.begin() + static_cast<long>(at),
                                     zlib.begin() + static_cast<long>(end)));
    }
    append_chunk(built.file, "IEND", {});
    return built;
}

void png_set_size(std::vector<u8>& png, u32 width, u32 height) {
    put_be32(&png[16], width);
    put_be32(&png[20], height);
    put_be32(&png[29], crc32_of(&png[12], 17));
}

// The offset of the frame header's FF, found by walking the segments.
usize jpeg_sof_at(const std::vector<u8>& jpeg) {
    usize at = 2;
    while (at + 4 <= jpeg.size() && jpeg[at] == 0xFF) {
        const u8 marker = jpeg[at + 1];
        if (marker >= 0xC0 && marker <= 0xC3) {
            return at;
        }
        at += 2 + (usize{jpeg[at + 2]} << 8 | jpeg[at + 3]);
    }
    CHECK(false);
    return 0;
}

void jpeg_set_size(std::vector<u8>& jpeg, u32 width, u32 height) {
    const usize sof = jpeg_sof_at(jpeg);
    jpeg[sof + 5] = static_cast<u8>(height >> 8);
    jpeg[sof + 6] = static_cast<u8>(height);
    jpeg[sof + 7] = static_cast<u8>(width >> 8);
    jpeg[sof + 8] = static_cast<u8>(width);
}

// Inserts comment segments after the start of image until the file has
// grown by at least `extra` bytes.
std::vector<u8> with_comments(const image_fixtures::Picture& picture, usize extra) {
    std::vector<u8> out(picture.file, picture.file + 2);
    usize added = 0;
    while (added < extra) {
        const usize payload = 60000;
        out.insert(out.end(), {0xFF, 0xFE, static_cast<u8>((payload + 2) >> 8),
                               static_cast<u8>(payload + 2)});
        out.insert(out.end(), payload, 'c');
        added += payload + 4;
    }
    out.insert(out.end(), picture.file + 2, picture.file + picture.size);
    return out;
}

bool is_progressive(const image_fixtures::Picture& picture) {
    const std::vector<u8> data(picture.file, picture.file + picture.size);
    return data[jpeg_sof_at(data) + 1] == 0xC2;
}

void pictures_decode() {
    for (const image_fixtures::Picture& picture : image_fixtures::kPictures) {
        Info info{};
        CHECK_EQ(probe_checked(picture.file, picture.size, info), Status::Ok);
        CHECK_EQ(info.format, named_png(picture.name) ? Format::Png : Format::Jpeg);
        CHECK_EQ(info.width, picture.width);
        CHECK_EQ(info.height, picture.height);
        CHECK_EQ(info.depth, picture.depth);
        CHECK_EQ(info.alpha, picture.alpha);
        const std::vector<u8> file(picture.file, picture.file + picture.size);
        PixelBuffer pixels(picture_bytes(picture));
        CHECK_EQ(decode_checked(file.data(), file.size(), pixels), Status::Ok);
        check_pixels(picture, pixels.pixels);
        CHECK(std::memcmp(file.data(), picture.file, picture.size) == 0);
    }
}

void trailing_garbage_is_accepted() {
    const image_fixtures::Picture& picture = image_fixtures::kPictures[0];
    std::vector<u8> data(picture.file, picture.file + picture.size);
    data.insert(data.end(), 16, 0xAA);
    Info info{};
    CHECK_EQ(probe_checked(data.data(), data.size(), info), Status::Ok);
    CHECK_EQ(info.width, picture.width);
    CHECK_EQ(info.height, picture.height);
    PixelBuffer pixels(picture_bytes(picture));
    CHECK_EQ(decode_checked(data.data(), data.size(), pixels), Status::Ok);
    check_pixels(picture, pixels.pixels);
}

void check_unsupported(const std::vector<u8>& file) {
    Info info{};
    CHECK_EQ(probe_checked(file.data(), file.size(), info), Status::Unsupported);
    PixelBuffer pixels(64 * 64 * 4);
    CHECK_EQ(decode_checked(file.data(), file.size(), pixels), Status::Unsupported);
}

void unsupported_jpegs_are_rejected() {
    check_unsupported(std::vector<u8>(
        image_fixtures::arithmetic_jpg_file,
        image_fixtures::arithmetic_jpg_file + sizeof image_fixtures::arithmetic_jpg_file));
    const image_fixtures::Picture& colour = fixture("b444.jpg");
    const std::vector<u8> baseline(colour.file, colour.file + colour.size);
    const usize sof = jpeg_sof_at(baseline);
    std::vector<u8> twelve_bit = baseline;
    twelve_bit[sof + 4] = 12;
    check_unsupported(twelve_bit);
    std::vector<u8> lossless = baseline;
    lossless[sof + 1] = 0xC3;
    check_unsupported(lossless);
    // A grey picture whose frame header lists a second component.
    const image_fixtures::Picture& grey = fixture("grey.jpg");
    std::vector<u8> two = std::vector<u8>(grey.file, grey.file + grey.size);
    const usize grey_sof = jpeg_sof_at(two);
    CHECK_EQ(two[grey_sof + 9], 1u);
    two[grey_sof + 3] = static_cast<u8>(two[grey_sof + 3] + 3);
    two[grey_sof + 9] = 2;
    const u8 second[] = {2, 0x11, 0};
    two.insert(two.begin() + static_cast<long>(grey_sof + 13), second, second + 3);
    check_unsupported(two);
}

void too_many_scans_are_partial() {
    const u8* file = image_fixtures::scans33_jpg_file;
    const usize size = sizeof image_fixtures::scans33_jpg_file;
    Info info{};
    CHECK_EQ(probe_checked(file, size, info), Status::Ok);
    PixelBuffer pixels(usize{info.width} * info.height * 4);
    CHECK_EQ(decode_checked(file, size, pixels), Status::Partial);
    // The 32 scans the decoder takes cover the whole picture.
    for (usize at = 0; at < pixels.bytes; at += 4) {
        CHECK_EQ(pixels.pixels[at + 3], 255u);
    }
}

void size_limits_hold() {
    const image_fixtures::Picture& png = fixture("rgb8.png");
    struct Case {
        u32 width;
        u32 height;
        Status status;
    };
    const Case cases[] = {
        {16383, 1, Status::Ok},          {16384, 1, Status::TooLarge},
        {1, 16383, Status::Ok},          {1, 16384, Status::TooLarge},
        {16383, 4096, Status::Ok},       {16383, 4097, Status::TooLarge},
        {4097, 16383, Status::TooLarge}, {8192, 8192, Status::Ok},
    };
    for (const Case& test : cases) {
        std::vector<u8> data(png.file, png.file + png.size);
        png_set_size(data, test.width, test.height);
        Info info{};
        CHECK_EQ(probe_checked(data.data(), data.size(), info), test.status);
        if (test.status == Status::Ok) {
            CHECK_EQ(info.width, test.width);
            CHECK_EQ(info.height, test.height);
        }
    }
    const image_fixtures::Picture& jpeg = fixture("b444.jpg");
    for (const Case& test : cases) {
        std::vector<u8> data(jpeg.file, jpeg.file + jpeg.size);
        jpeg_set_size(data, test.width, test.height);
        Info info{};
        CHECK_EQ(probe_checked(data.data(), data.size(), info), test.status);
    }
}

void zero_sizes_are_corrupt() {
    const image_fixtures::Picture& png = fixture("rgb8.png");
    std::vector<u8> data(png.file, png.file + png.size);
    png_set_size(data, 0, png.height);
    Info info{};
    CHECK_EQ(probe_checked(data.data(), data.size(), info), Status::Corrupt);
    const image_fixtures::Picture& jpeg = fixture("b444.jpg");
    data.assign(jpeg.file, jpeg.file + jpeg.size);
    jpeg_set_size(data, 0, jpeg.height);
    CHECK_EQ(probe_checked(data.data(), data.size(), info), Status::Corrupt);
    PixelBuffer pixels(picture_bytes(jpeg));
    CHECK_EQ(decode_checked(data.data(), data.size(), pixels), Status::Corrupt);
}

void non_pictures_are_unknown() {
    const u8 text[] = {'h', 'e', 'l', 'l', 'o'};
    const u8 partial_png[] = {0x89, 0x50, 0x4E, 0x47};
    const u8 png_magic[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    const u8 partial_jpeg[] = {0xFF};
    const u8 truncated_jpeg[] = {0xFF, 0xD8};
    Info info{};
    CHECK_EQ(probe_checked(nullptr, 0, info), Status::NotThisFormat);
    CHECK_EQ(probe_checked(text, sizeof text, info), Status::NotThisFormat);
    CHECK_EQ(probe_checked(partial_png, sizeof partial_png, info), Status::NotThisFormat);
    CHECK_EQ(probe_checked(png_magic, sizeof png_magic, info), Status::Truncated);
    CHECK_EQ(probe_checked(partial_jpeg, sizeof partial_jpeg, info), Status::NotThisFormat);
    CHECK_EQ(probe_checked(truncated_jpeg, sizeof truncated_jpeg, info), Status::Truncated);
    PixelBuffer pixels(16);
    CHECK_EQ(decode_checked(nullptr, 0, pixels), Status::NotThisFormat);
    CHECK_EQ(decode_checked(text, sizeof text, pixels), Status::NotThisFormat);
}

void small_buffer_is_refused() {
    for (const image_fixtures::Picture& picture : image_fixtures::kPictures) {
        const usize bytes = picture_bytes(picture);
        for (const usize capacity : {usize{0}, bytes - 1}) {
            PixelBuffer pixels(capacity);
            CHECK_EQ(decode_checked(picture.file, picture.size, pixels),
                     Status::BufferTooSmall);
        }
        AllocCounts host_counts;
        AllocCounts work_counts;
        CHECK_EQ(decode(picture.file, picture.size, allocator(host_counts),
                        allocator(work_counts), nullptr, bytes, no_progress),
                 Status::BufferTooSmall);
        CHECK_EQ(host_counts.live, 0u);
        CHECK_EQ(work_counts.live, 0u);
    }
}

void allocator_failures_are_clean() {
    const image_fixtures::Picture& picture = image_fixtures::kPictures[0];
    const usize bytes = picture_bytes(picture);
    usize probe_allocations = 0;
    {
        AllocCounts counts;
        Info info{};
        CHECK_EQ(probe(picture.file, picture.size, allocator(counts), info), Status::Ok);
        CHECK_EQ(counts.live, 0u);
        probe_allocations = counts.calls;
    }
    CHECK(probe_allocations != 0);
    for (usize fail = 0; fail < probe_allocations; ++fail) {
        AllocCounts counts;
        counts.fail_at = fail;
        Info info{};
        CHECK_EQ(probe(picture.file, picture.size, allocator(counts), info),
                 Status::NoMemory);
        CHECK_EQ(counts.live, 0u);
    }
    usize decode_allocations = 0;
    {
        AllocCounts counts;
        PixelBuffer pixels(bytes);
        CHECK_EQ(decode(picture.file, picture.size, allocator(counts), allocator(counts),
                        pixels.pixels, bytes, no_progress),
                 Status::Ok);
        CHECK_EQ(counts.live, 0u);
        decode_allocations = counts.calls;
    }
    CHECK(decode_allocations >= 2);
    for (usize fail = 0; fail < decode_allocations; ++fail) {
        AllocCounts counts;
        counts.fail_at = fail;
        PixelBuffer pixels(bytes);
        CHECK_EQ(decode(picture.file, picture.size, allocator(counts), allocator(counts),
                        pixels.pixels, bytes, no_progress),
                 Status::NoMemory);
        check_guards(pixels);
        CHECK_EQ(counts.live, 0u);
    }
}

// Counts the steps of a whole decode, then stops it at every one of them.
usize check_stops(const u8* file, usize size, usize bytes) {
    ProgressCounts counts;
    {
        PixelBuffer pixels(bytes);
        CHECK_EQ(decode_checked(file, size, pixels, progress_for(counts)), Status::Ok);
    }
    CHECK(counts.calls != 0);
    for (usize stop = 0; stop < counts.calls; ++stop) {
        ProgressCounts stopping;
        stopping.stop_at = stop;
        PixelBuffer pixels(bytes);
        CHECK_EQ(decode_checked(file, size, pixels, progress_for(stopping)), Status::Stopped);
        CHECK_EQ(stopping.calls, stop + 1);
    }
    return counts.calls;
}

void progress_stops() {
    for (const image_fixtures::Picture& picture : image_fixtures::kPictures) {
        check_stops(picture.file, picture.size, picture_bytes(picture));
    }
}

void large_pngs_resume() {
    // Several input steps, the IDAT split across them.
    const BuiltPng rgb = noise_png(256, 192, false, 20000);
    CHECK(rgb.file.size() > 2 * kInputStep);
    Info info{};
    CHECK_EQ(probe_checked(rgb.file.data(), rgb.file.size(), info), Status::Ok);
    CHECK_EQ(info.width, rgb.width);
    CHECK_EQ(info.height, rgb.height);
    CHECK_EQ(info.depth, 24u);
    PixelBuffer pixels(rgb.rgba.size());
    CHECK_EQ(decode_checked(rgb.file.data(), rgb.file.size(), pixels), Status::Ok);
    CHECK(std::memcmp(pixels.pixels, rgb.rgba.data(), rgb.rgba.size()) == 0);
    const usize rgb_steps = check_stops(rgb.file.data(), rgb.file.size(), rgb.rgba.size());
    CHECK(rgb_steps > rgb.file.size() / kInputStep);

    // Four MiB of pixels from one IDAT: the clearing alone takes four steps.
    const BuiltPng grey = noise_png(1024, 1024, true, 1u << 20);
    CHECK(grey.file.size() > 2 * kInputStep);
    PixelBuffer grey_pixels(grey.rgba.size());
    CHECK_EQ(decode_checked(grey.file.data(), grey.file.size(), grey_pixels), Status::Ok);
    CHECK(std::memcmp(grey_pixels.pixels, grey.rgba.data(), grey.rgba.size()) == 0);
    const usize grey_steps = check_stops(grey.file.data(), grey.file.size(), grey.rgba.size());
    CHECK(grey_steps >= grey.rgba.size() / kClearStep + grey.file.size() / kInputStep);
}

usize steps_of(const std::vector<u8>& file, const image_fixtures::Picture& picture) {
    ProgressCounts counts;
    PixelBuffer pixels(picture_bytes(picture));
    CHECK_EQ(decode_checked(file.data(), file.size(), pixels, progress_for(counts)),
             Status::Ok);
    check_pixels(picture, pixels.pixels);
    return counts.calls;
}

void progressive_jpeg_is_one_step() {
    const image_fixtures::Picture& progressive = fixture("progressive.jpg");
    const std::vector<u8> plain(progressive.file, progressive.file + progressive.size);
    const std::vector<u8> padded = with_comments(progressive, 2 * kInputStep);
    CHECK(padded.size() > 2 * kInputStep);
    Info info{};
    CHECK_EQ(probe_checked(padded.data(), padded.size(), info), Status::Ok);
    CHECK_EQ(info.width, progressive.width);
    // The headers, the clearing and the whole file in one step.
    CHECK_EQ(steps_of(plain, progressive), 3u);
    CHECK_EQ(steps_of(padded, progressive), 3u);

    // A baseline picture is read a step at a time.
    const image_fixtures::Picture& baseline = fixture("b420.jpg");
    const std::vector<u8> baseline_plain(baseline.file, baseline.file + baseline.size);
    const std::vector<u8> baseline_padded = with_comments(baseline, 2 * kInputStep);
    CHECK(steps_of(baseline_padded, baseline) >=
          steps_of(baseline_plain, baseline) + baseline_padded.size() / kInputStep);
}

// The rows of a partly decoded picture: every pixel either holds the
// picture or is clear, and the first pixel was decoded.
void check_partial_png(const image_fixtures::Picture& picture, const u8* pixels) {
    CHECK(pixel_matches(picture, pixels, 0));
    CHECK(!pixel_is_clear(pixels, 0) || picture.rgba[3] == 0);
    for (usize at = 0; at < picture_bytes(picture); at += 4) {
        CHECK(pixel_matches(picture, pixels, at) || pixel_is_clear(pixels, at));
    }
}

// A partly decoded JPEG holds rows of the picture from the top, and every
// row below the first clear one is clear too. Rows of a progressive picture
// may hold it in less detail.
void check_partial_jpeg(const image_fixtures::Picture& picture, const u8* pixels,
                        bool progressive) {
    const usize row_bytes = usize{picture.width} * 4;
    CHECK_EQ(pixels[3], 255u);
    usize first_clear = picture.height;
    for (usize y = 0; y < picture.height; ++y) {
        bool clear = true;
        for (usize at = y * row_bytes; at < (y + 1) * row_bytes; at += 4) {
            clear = clear && pixel_is_clear(pixels, at);
        }
        if (clear && first_clear == picture.height) {
            first_clear = y;
        }
        if (first_clear != picture.height) {
            CHECK(clear);
        } else {
            for (usize at = y * row_bytes; at < (y + 1) * row_bytes; at += 4) {
                CHECK_EQ(pixels[at + 3], 255u);
            }
        }
    }
    // Rows well above the cut are the picture's own; the last row or two
    // before it may take colour from the rows the decoder had to guess.
    for (usize y = 0; !progressive && y + 16 < first_clear; ++y) {
        for (usize at = y * row_bytes; at < (y + 1) * row_bytes; at += 4) {
            CHECK(pixel_matches(picture, pixels, at));
        }
    }
}

void truncated_pngs_are_never_ok() {
    for (const image_fixtures::Picture& picture : image_fixtures::kPictures) {
        if (!named_png(picture.name)) {
            continue;
        }
        // The picture is complete once its last IDAT chunk is.
        usize complete = 0;
        for (usize at = 8; at + 8 <= picture.size;) {
            const usize length = read_be32(picture.file + at);
            if (std::memcmp(picture.file + at + 4, "IDAT", 4) == 0) {
                complete = at + 12 + length;
            }
            at += 12 + length;
        }
        CHECK(complete != 0);
        usize partials = 0;
        for (usize length = 0; length < picture.size; ++length) {
            const std::vector<u8> prefix(picture.file, picture.file + length);
            Info info{};
            const Status probed = probe_checked(prefix.data(), length, info);
            CHECK(probed == Status::Ok || probed == Status::Truncated ||
                  (length < 8 && probed == Status::NotThisFormat));
            PixelBuffer pixels(picture_bytes(picture));
            const Status decoded = decode_checked(prefix.data(), length, pixels);
            if (length >= complete) {
                CHECK_EQ(decoded, Status::Ok);
                check_pixels(picture, pixels.pixels);
            } else if (decoded == Status::Partial) {
                ++partials;
                check_partial_png(picture, pixels.pixels);
            } else {
                CHECK(decoded == Status::Truncated ||
                      (length < 8 && decoded == Status::NotThisFormat));
            }
        }
        CHECK(partials != 0);
    }
}

void truncated_jpegs_are_partial() {
    for (const image_fixtures::Picture& picture : image_fixtures::kPictures) {
        if (named_png(picture.name)) {
            continue;
        }
        const bool progressive = is_progressive(picture);
        usize partials = 0;
        for (usize length = 0; length < picture.size; ++length) {
            const std::vector<u8> prefix(picture.file, picture.file + length);
            Info info{};
            const Status probed = probe_checked(prefix.data(), length, info);
            CHECK(probed == Status::Ok || probed == Status::Truncated ||
                  (length < 2 && probed == Status::NotThisFormat));
            PixelBuffer pixels(picture_bytes(picture));
            const Status decoded = decode_checked(prefix.data(), length, pixels);
            if (decoded == Status::Ok) {
                // Only the end of image marker, or part of it, is missing.
                CHECK(!progressive);
                CHECK(length + 2 >= picture.size);
                check_pixels(picture, pixels.pixels);
            } else if (decoded == Status::Partial) {
                ++partials;
                check_partial_jpeg(picture, pixels.pixels, progressive);
            } else {
                CHECK(decoded == Status::Truncated ||
                      (length < 2 && decoded == Status::NotThisFormat));
            }
        }
        CHECK(partials != 0);
    }
}

void missing_end_of_image() {
    for (const image_fixtures::Picture& picture : image_fixtures::kPictures) {
        if (named_png(picture.name)) {
            continue;
        }
        CHECK_EQ(picture.file[picture.size - 2], 0xFFu);
        CHECK_EQ(picture.file[picture.size - 1], 0xD9u);
        for (const usize missing : {usize{1}, usize{2}}) {
            PixelBuffer pixels(picture_bytes(picture));
            const Status status = decode_checked(picture.file, picture.size - missing, pixels);
            if (is_progressive(picture)) {
                // More scans could have followed, and the decoder keeps the
                // last one's blocks only as far as it could read them.
                CHECK_EQ(status, Status::Partial);
                for (usize at = 0; at < pixels.bytes; at += 4) {
                    CHECK_EQ(pixels.pixels[at + 3], 255u);
                }
            } else if (std::strcmp(picture.name, "restart.jpg") == 0) {
                // Every restart interval is one MCU, so the last one is full
                // and the decoder cannot tell it was all there.
                CHECK_EQ(status, Status::Partial);
                check_partial_jpeg(picture, pixels.pixels, false);
            } else {
                CHECK_EQ(status, Status::Ok);
                check_pixels(picture, pixels.pixels);
            }
        }
    }
}

void mutations_are_safe() {
    constexpr usize kPictureCount =
        sizeof image_fixtures::kPictures / sizeof image_fixtures::kPictures[0];
    u64 state = 0x9E3779B97F4A7C15ull;
    for (usize i = 0; i < kMutationCount; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        const image_fixtures::Picture& picture =
            image_fixtures::kPictures[state % kPictureCount];
        std::vector<u8> data(picture.file, picture.file + picture.size);
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        const usize position = state % data.size();
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        data[position] ^= static_cast<u8>(state | 1u);
        Info info{};
        const Status probed = probe_checked(data.data(), data.size(), info);
        (void)probed;
        PixelBuffer pixels(picture_bytes(picture));
        const Status decoded = decode_checked(data.data(), data.size(), pixels);
        (void)decoded;
    }
}

// The HOMBRE_PICTURES_HDF picture check: the first Amiga partition of the
// image is mounted and every PNG and JPEG below Backdrops and Tiles is
// decoded, timed and printed.

struct DiskFile {
    std::FILE* file;
    u64 sectors;
    u64 volume_first;
};

bool read_disk(void* ctx, u64 sector, u8* out) {
    DiskFile& disk = *static_cast<DiskFile*>(ctx);
    if (sector >= disk.sectors) {
        return false;
    }
    if (std::fseek(disk.file, static_cast<long>(sector * 512u), SEEK_SET) != 0) {
        return false;
    }
    return std::fread(out, 1, 512, disk.file) == 512;
}

bool read_volume_block(void* ctx, u32 block, u8* out) {
    DiskFile& disk = *static_cast<DiskFile*>(ctx);
    return read_disk(&disk, disk.volume_first + block, out);
}

bool mount_hdf(DiskFile& disk, AdfVolume& volume) {
    AmigaPartitions partitions{};
    RdbError error = RdbError::NoPartitions;
    for (const u32 lba_bytes : {512u, 4096u}) {
        error = find_amiga_partitions(read_disk, &disk, disk.sectors, partitions, lba_bytes);
        if (error == RdbError::Ok && partitions.count != 0) {
            break;
        }
    }
    if (error != RdbError::Ok || partitions.count == 0) {
        return false;
    }
    const AmigaPartition& part = partitions.list[0];
    disk.volume_first = part.disk_first_sector;
    const BlockSource source{&disk, read_volume_block, nullptr};
    return adf_mount_partition(source, 0, part.part.block_count, part.part.reserved, volume) ==
           FsError::Ok;
}

void collect_entry(void* ctx, const DirEntry& entry) {
    if (!entry.is_dir) {
        static_cast<std::vector<DirEntry>*>(ctx)->push_back(entry);
    }
}

bool has_extension(const char* name, const char* extension) {
    const usize name_length = std::strlen(name);
    const usize extension_length = std::strlen(extension);
    if (name_length < extension_length) {
        return false;
    }
    for (usize i = 0; i < extension_length; ++i) {
        char c = name[name_length - extension_length + i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != extension[i]) {
            return false;
        }
    }
    return true;
}

bool read_drawer_file(const AdfVolume& volume, const DirEntry& entry,
                      std::vector<u8>& data) {
    data.assign(entry.size, 0);
    if (entry.size == 0) {
        return true;
    }
    u32 got = 0;
    const FsError error = adf_read(volume, entry, 0, data.data(), entry.size, got);
    return error == FsError::Ok && got == entry.size;
}

void decode_hdf_picture(const char* drawer, const AdfVolume& volume,
                        const DirEntry& entry) {
    std::vector<u8> data;
    CHECK(read_drawer_file(volume, entry, data));
    if (data.empty()) {
        return;
    }
    Info info{};
    CHECK_EQ(probe_checked(data.data(), data.size(), info), Status::Ok);
    CHECK(info.width != 0 && info.height != 0);
    CHECK(info.width <= 16383 && info.height <= 16383);
    CHECK(info.depth != 0);
    CHECK_EQ(info.format, has_extension(entry.name, ".png") ? Format::Png : Format::Jpeg);
    PixelBuffer pixels(usize{info.width} * info.height * 4);
    const auto start = std::chrono::steady_clock::now();
    const Status status = decode_checked(data.data(), data.size(), pixels);
    const auto finish = std::chrono::steady_clock::now();
    CHECK_EQ(status, Status::Ok);
    const auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count();
    std::printf("%s/%s: %ux%u, %u bits, %s, %lld us\n", drawer, entry.name, info.width,
                info.height, info.depth, info.alpha ? "alpha" : "opaque",
                static_cast<long long>(micros));
}

void pictures_from_hdf() {
    const char* path = std::getenv("HOMBRE_PICTURES_HDF");
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    std::FILE* file = std::fopen(path, "rb");
    CHECK(file != nullptr);
    if (file == nullptr) {
        return;
    }
    CHECK(std::fseek(file, 0, SEEK_END) == 0);
    const long length = std::ftell(file);
    CHECK(length > 0 && length % 512 == 0);
    DiskFile disk{file, static_cast<u64>(length) / 512u, 0};
    AdfVolume volume{};
    CHECK(mount_hdf(disk, volume));
    const char* drawers[] = {"Backdrops", "Tiles"};
    for (const char* drawer : drawers) {
        std::vector<DirEntry> entries;
        CHECK_EQ(adf_list(volume, drawer, collect_entry, &entries), FsError::Ok);
        for (const DirEntry& entry : entries) {
            if (has_extension(entry.name, ".png") || has_extension(entry.name, ".jpg")) {
                decode_hdf_picture(drawer, volume, entry);
            }
        }
    }
    std::fclose(file);
}

}

int main() {
    pictures_decode();
    trailing_garbage_is_accepted();
    unsupported_jpegs_are_rejected();
    too_many_scans_are_partial();
    size_limits_hold();
    zero_sizes_are_corrupt();
    non_pictures_are_unknown();
    small_buffer_is_refused();
    allocator_failures_are_clean();
    progress_stops();
    large_pngs_resume();
    progressive_jpeg_is_one_step();
    truncated_pngs_are_never_ok();
    truncated_jpegs_are_partial();
    missing_end_of_image();
    mutations_are_safe();
    pictures_from_hdf();
    return check::finish("test_image_decode");
}
