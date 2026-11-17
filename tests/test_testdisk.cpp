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

// Runs C/Version of the generated test disk, the way the kernel will: the
// disk is unpacked with the project's DMS code, read with its Amiga file
// system code, and the command runs on the translator with the real exec and
// dos. exec loads LIBS:hombretest.library through dos.
//
// The path of the disk comes in the environment, as HOMBRE_TEST_DISK (make
// test sets it); without it the test says so and passes.

#include "libs/dos.hpp"
#include "check.hpp"
#include "dms.hpp"
#include "ofs_writer.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace hombre;
using namespace hombre::m68k;

namespace {

constexpr u32 kStack = 0x8000;
constexpr u32 kFreeStart = 0x10000;
constexpr u32 kFreeEnd = 0x400000;
constexpr u64 kBlocks = 1000000;
constexpr usize kRegion = 1 << 20;
constexpr usize kMaxImage = 80u * 22528u;  // an HD disk

Jit g_jit;
Exec g_exec;
Dos g_dos;
std::vector<std::string> g_lines;
std::string g_console;

void capture(const char* line) {
    g_lines.push_back(line);
}

void console(void*, const u8* data, u32 bytes) {
    g_console.append(reinterpret_cast<const char*>(data), bytes);
}

std::vector<u8> read_file(const char* path) {
    std::vector<u8> data;
    std::FILE* f = std::fopen(path, "rb");
    CHECK(f != nullptr);
    u8 chunk[65536];
    usize got = 0;
    while ((got = std::fread(chunk, 1, sizeof chunk, f)) > 0) {
        data.insert(data.end(), chunk, chunk + got);
    }
    std::fclose(f);
    return data;
}

std::vector<u8> unpack_disk(const std::vector<u8>& dms) {
    CHECK(detect_disk_format(dms.data(), dms.size()) == DiskFormat::Dms);
    auto work = std::make_unique<DmsWork>();
    std::vector<u8> image(kMaxImage, 0);
    DmsInfo info{};
    CHECK(dms_unpack(dms.data(), dms.size(), image.data(), image.size(), *work, info) ==
          DmsError::Ok);
    image.resize(info.image_bytes);
    return image;
}

// The bytes of a file on the volume.
std::vector<u8> file_bytes(const AdfVolume& volume, const char* path) {
    DirEntry entry{};
    CHECK(adf_find(volume, path, entry) == FsError::Ok);
    CHECK(!entry.is_dir);
    std::vector<u8> out(entry.size);
    u32 got = 0;
    CHECK(adf_read(volume, entry, 0, out.data(), entry.size, got) == FsError::Ok);
    CHECK_EQ(got, entry.size);
    return out;
}

// The translator, exec and dos over one volume, with the log and the
// console captured.
class Rig {
public:
    explicit Rig(const std::vector<u8>& image) : memory_(kGuestBytes), image_(image) {
        fd_ = memfd_create("testdisk", 0);
        CHECK(fd_ >= 0);
        CHECK(ftruncate(fd_, static_cast<off_t>(kRegion)) == 0);
        void* rw = mmap(nullptr, kRegion, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        void* rx = mmap(nullptr, kRegion, PROT_READ | PROT_EXEC, MAP_SHARED, fd_, 0);
        CHECK(rw != MAP_FAILED);
        CHECK(rx != MAP_FAILED);
        rw_ = static_cast<u8*>(rw);
        rx_ = static_cast<const u8*>(rx);
        m.cpu = CpuState{};
        m.mem = GuestMemory{memory_.data(), kGuestBytes};
        m.jit = nullptr;
        CHECK(g_jit.init(CodeRegion{rw_, rx_, kRegion}, m));
        g_lines.clear();
        g_console.clear();
        g_log = capture;
        CHECK(g_exec.init(m, kFreeStart, kFreeEnd));
        CHECK(adf_mount(image_.data(), image_.size(), volume) == FsError::Ok);
        CHECK(g_dos.init(m, g_exec, volume, console, nullptr));
        m.cpu.a[7] = kStack;
    }

    ~Rig() {
        g_exec.set_this_task(0);
        g_exec.set_segment_loader(SegmentLoader{nullptr, nullptr});
        g_log = nullptr;
        munmap(rw_, kRegion);
        munmap(const_cast<u8*>(rx_), kRegion);
        close(fd_);
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    Machine m;
    AdfVolume volume{};

private:
    std::vector<u8> memory_;
    std::vector<u8> image_;
    int fd_ = -1;
    u8* rw_ = nullptr;
    const u8* rx_ = nullptr;
};

void version_command_prints_the_library_version(const std::vector<u8>& image) {
    Rig r(image);
    u32 result = 99;
    CHECK(g_dos.run_command("C:Version", "", result, kBlocks));
    CHECK_EQ(result, 0u);
    CHECK(g_console == "Hombre test version 1\n");
    CHECK(g_lines.empty());
    // The library stays resident, and a second run finds it there.
    CHECK(g_dos.run_command("C:Version", "", result, kBlocks));
    CHECK_EQ(result, 0u);
    CHECK(g_console == "Hombre test version 1\nHombre test version 1\n");
    CHECK(g_lines.empty());
}

void version_command_fails_without_the_library(const std::vector<u8>& image) {
    AdfVolume source{};
    CHECK(adf_mount(image.data(), image.size(), source) == FsError::Ok);
    const std::vector<AdfFile> files = {{"C/Version", file_bytes(source, "C/Version")}};
    const std::vector<u8> bare = build_adf("HombreTest", false, files);
    Rig r(bare);
    u32 result = 0;
    CHECK(g_dos.run_command("C:Version", "", result, kBlocks));
    CHECK_EQ(result, 20u);
    CHECK(g_console == "Version: cannot open hombretest.library\n");
}

void the_other_files_are_unchanged(const std::vector<u8>& image) {
    Rig r(image);
    const std::vector<u8> startup = file_bytes(r.volume, "S/Startup-Sequence");
    CHECK(std::string(startup.begin(), startup.end()) == "Hombre test startup\n");
    const std::vector<u8> hello = file_bytes(r.volume, "Hello");
    CHECK(std::string(hello.begin(), hello.end()) == "Hello from the Hombre test disk.\n");
    const std::vector<u8> echo = file_bytes(r.volume, "C/Echo");
    CHECK_EQ(echo.size(), 600u);
    CHECK_EQ(echo[0], static_cast<u8>('A'));
    CHECK_EQ(echo[599], static_cast<u8>('A' + 599 % 26));
}

// What the corrupt-disk boot does: byte 100 of the file set to 0xFF. It
// lies in the first track's packed data, and the archive then fails with a
// track CRC error for track 0.
void the_corrupt_disk_byte_is_in_the_first_track(std::vector<u8> dms) {
    constexpr usize kFlipped = 100;
    constexpr usize kFirstTrack = 56;     // after the archive header
    constexpr usize kFirstTrackData = kFirstTrack + 20;  // after its header
    const usize packed = static_cast<usize>(dms[kFirstTrack + 6] << 8 | dms[kFirstTrack + 7]);
    CHECK(kFlipped >= kFirstTrackData);
    CHECK(kFlipped < kFirstTrackData + packed);
    CHECK(dms[kFlipped] != 0xFF);
    dms[kFlipped] = 0xFF;
    auto work = std::make_unique<DmsWork>();
    std::vector<u8> image(kMaxImage, 0);
    DmsInfo info{};
    CHECK(dms_unpack(dms.data(), dms.size(), image.data(), image.size(), *work, info) ==
          DmsError::TrackCrc);
    CHECK_EQ(info.failed_track, 0u);
}

}  // namespace

int main() {
    const char* path = std::getenv("HOMBRE_TEST_DISK");
    if (path == nullptr || *path == '\0') {
        std::printf("test_testdisk: skipping: set HOMBRE_TEST_DISK to the test disk\n");
        return 0;
    }
    const std::vector<u8> dms = read_file(path);
    const std::vector<u8> image = unpack_disk(dms);
    version_command_prints_the_library_version(image);
    version_command_fails_without_the_library(image);
    the_other_files_are_unchanged(image);
    the_corrupt_disk_byte_is_in_the_first_track(dms);
    return check::finish("test_testdisk");
}
