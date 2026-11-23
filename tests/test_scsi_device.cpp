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

#include "libs/scsi_device.hpp"
#include "check.hpp"
#include "test_guest.hpp"

#include <cstring>
#include <initializer_list>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace hombre;
using namespace hombre::m68k;
using namespace hombre::ndk;

namespace {

constexpr u32 kCode = 0x1000;
constexpr u32 kStack = 0x8000;
constexpr u32 kData = 0x9000;
constexpr u32 kBuffer = 0x9200;
constexpr u32 kGeometry = 0x9600;
constexpr u32 kNames = 0x9800;
constexpr u32 kCdb = 0x9900;
constexpr u32 kScsi = 0x9A00;
constexpr u32 kSense = 0x9B00;
constexpr u32 kQuery = 0x9C00;
constexpr u32 kFreeStart = 0x10000;
constexpr usize kRegion = 1 << 20;
constexpr u32 kSectorSize = 512;

Jit g_jit;
Exec g_exec;
ScsiDevice g_scsi;
DiskUnits g_units;

struct FakeHost {
    u64 now_us = 0;
};

bool fake_create(void*, u32) { return true; }
void fake_switch(void*, u32, u32) {}
void fake_destroy(void*, u32) {}
bool fake_slice_over(void*) { return false; }
void fake_poll(void*) {}
bool fake_idle(void*) { return false; }
u64 fake_now_us(void* ctx) { return static_cast<FakeHost*>(ctx)->now_us; }

struct TestDisk {
    u64 sectors = 0;
    bool writable = true;
    bool fail_reads = false;
    bool fail_writes = false;
    bool fail_flush = false;
    u32 flush_calls = 0;
    u64 fail_sector = ~0ull;
    u64 last_write_sector = ~0ull;
    std::vector<u8> bytes;

    static bool read(void* ctx, u64 sector, u8* out) {
        TestDisk& disk = *static_cast<TestDisk*>(ctx);
        if (sector >= disk.sectors || sector == disk.fail_sector || disk.fail_reads) {
            return false;
        }
        if (disk.bytes.empty()) {
            for (u32 i = 0; i < kSectorSize; ++i) {
                out[i] = static_cast<u8>(sector + i);
            }
            return true;
        }
        std::memcpy(out, &disk.bytes[sector * kSectorSize], kSectorSize);
        return true;
    }

    static bool write(void* ctx, u64 sector, const u8* data) {
        TestDisk& disk = *static_cast<TestDisk*>(ctx);
        if (sector >= disk.sectors || sector == disk.fail_sector || disk.fail_writes) {
            return false;
        }
        disk.last_write_sector = sector;
        if (!disk.bytes.empty()) {
            std::memcpy(&disk.bytes[sector * kSectorSize], data, kSectorSize);
        }
        return true;
    }

    static bool flush(void* ctx) {
        TestDisk& disk = *static_cast<TestDisk*>(ctx);
        ++disk.flush_calls;
        return !disk.fail_flush;
    }
};

TestDisk make_test_disk(u64 sectors, bool writable = true) {
    TestDisk disk;
    disk.sectors = sectors;
    disk.writable = writable;
    disk.bytes.resize(sectors * kSectorSize);
    for (u64 sector = 0; sector < sectors; ++sector) {
        for (u32 i = 0; i < kSectorSize; ++i) {
            disk.bytes[sector * kSectorSize + i] = static_cast<u8>(sector * 13 + i);
        }
    }
    return disk;
}

TestDisk make_sparse_disk(u64 sectors) {
    TestDisk disk;
    disk.sectors = sectors;
    return disk;
}

DiskUnit unit_of(TestDisk& disk) {
    DiskUnit unit{};
    unit.ctx = &disk;
    unit.read = TestDisk::read;
    unit.write = disk.writable ? TestDisk::write : nullptr;
    unit.flush = TestDisk::flush;
    unit.sectors = disk.sectors;
    std::strcpy(unit.product, "HOMBRE DISK");
    return unit;
}

class Rig {
public:
    Rig() : memory_(kTestGuestBytes) {
        fd_ = memfd_create("scsi_device", 0);
        CHECK(fd_ >= 0);
        CHECK(ftruncate(fd_, static_cast<off_t>(kRegion)) == 0);
        void* rw = mmap(nullptr, kRegion, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        void* rx = mmap(nullptr, kRegion, PROT_READ | PROT_EXEC, MAP_SHARED, fd_, 0);
        CHECK(rw != MAP_FAILED);
        CHECK(rx != MAP_FAILED);
        rw_ = static_cast<u8*>(rw);
        rx_ = static_cast<const u8*>(rx);
        m.cpu = CpuState{};
        m.mem = GuestMemory{memory_.data(), kTestGuestBytes};
        m.jit = nullptr;
        CHECK(g_jit.init(CodeRegion{rw_, rx_, kRegion}, m));
        g_log = nullptr;
        g_exec.set_task_host(TaskHost{fake_create, fake_switch, fake_destroy, fake_slice_over,
                                      fake_poll, fake_idle, fake_now_us, &host});
        CHECK(g_exec.init(m, kFreeStart, kTestChipEnd, kTestGuestBytes));
        CHECK(g_scsi.init(m, g_exec, &g_units));
        m.cpu.a[7] = kStack;
        make_task();
    }

    ~Rig() {
        g_exec.set_this_task(0);
        g_exec.set_task_host(TaskHost{});
        munmap(rw_, kRegion);
        munmap(const_cast<u8*>(rx_), kRegion);
        close(fd_);
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    Machine m;
    FakeHost host{};

    void put8(u32 at, u32 v) { memory_[at] = static_cast<u8>(v); }
    void put16(u32 at, u32 v) {
        put8(at, v >> 8);
        put8(at + 1, v);
    }
    void put32(u32 at, u32 v) {
        put16(at, v >> 16);
        put16(at + 2, v);
    }
    u32 get8(u32 at) const { return memory_[at]; }
    u32 get16(u32 at) const { return (get8(at) << 8) | get8(at + 1); }
    u32 get32(u32 at) const { return (get16(at) << 16) | get16(at + 2); }

    u32 str(u32 at, const char* s) {
        for (usize i = 0; i <= std::strlen(s); ++i) {
            put8(at + static_cast<u32>(i), static_cast<u8>(s[i]));
        }
        return at;
    }

    ExitReason call_exec(u32 lvo) { return call_vec(g_exec.exec_base(), lvo); }

    ExitReason call_scsi(u32 lvo) { return call_vec(g_scsi.device_base(), lvo); }

    u32 create_request(u32& port) {
        CHECK_EQ(call_exec(exec_lvo::CreateMsgPort), ExitReason::Returned);
        port = m.cpu.d[0];
        CHECK(port != 0);
        m.cpu.a[0] = port;
        m.cpu.d[0] = IOSTD_SIZE;
        CHECK_EQ(call_exec(exec_lvo::CreateIORequest), ExitReason::Returned);
        const u32 req = m.cpu.d[0];
        CHECK(req != 0);
        return req;
    }

    u32 open_device(u32 req, u32 unit) {
        m.cpu.a[0] = str(kNames, "scsi.device");
        m.cpu.d[0] = unit;
        m.cpu.a[1] = req;
        m.cpu.d[1] = 0;
        CHECK_EQ(call_exec(exec_lvo::OpenDevice), ExitReason::Returned);
        return m.cpu.d[0];
    }

    void do_io(u32 req) {
        m.cpu.a[1] = req;
        CHECK_EQ(call_exec(exec_lvo::DoIO), ExitReason::Returned);
    }

    void close_request(u32 req, u32 port) {
        m.cpu.a[1] = req;
        CHECK_EQ(call_exec(exec_lvo::CloseDevice), ExitReason::Returned);
        m.cpu.a[0] = req;
        CHECK_EQ(call_exec(exec_lvo::DeleteIORequest), ExitReason::Returned);
        m.cpu.a[0] = port;
        CHECK_EQ(call_exec(exec_lvo::DeleteMsgPort), ExitReason::Returned);
    }

private:
    void make_task() {
        const u32 task = kData;
        for (u32 i = 0; i < TC_SIZE; ++i) {
            put8(task + i, 0);
        }
        put8(task + LN_TYPE, NT_TASK);
        put32(task + TC_SIGALLOC, 0x0000FFFF);
        g_exec.set_this_task(task);
    }

    ExitReason call_vec(u32 base, u32 lvo) {
        put16(kCode, 0x4EAE);  // JSR d16(A6)
        put16(kCode + 2, static_cast<u16>(0x10000 - lvo));
        put16(kCode + 4, 0x4E75);
        g_jit.flush();
        m.cpu.a[6] = base;
        m.cpu.a[7] = kStack;
        m.cpu.pc = kCode;
        put32(kStack - 4, kReturnStub);
        m.cpu.a[7] = kStack - 4;
        return g_jit.run(100000);
    }

    std::vector<u8> memory_;
    int fd_ = -1;
    u8* rw_ = nullptr;
    const u8* rx_ = nullptr;
};

void opens_units_and_fails_for_missing_ones() {
    TestDisk first = make_test_disk(2048);
    TestDisk second = make_test_disk(64, false);
    g_units = DiskUnits{};
    g_units.count = 2;
    g_units.unit[0] = unit_of(first);
    g_units.unit[1] = unit_of(second);

    Rig r;
    u32 port = 0;
    const u32 req = r.create_request(port);

    CHECK_EQ(r.open_device(req, 0), 0u);
    CHECK(r.get32(req + io_Unit) != 0);
    CHECK_EQ(r.get32(req + io_Device), g_scsi.device_base());
    CHECK_EQ(r.get16(g_scsi.device_base() + LIB_OPENCNT), 1u);

    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_exec(exec_lvo::CloseDevice), ExitReason::Returned);
    CHECK_EQ(r.get16(g_scsi.device_base() + LIB_OPENCNT), 0u);
    CHECK_EQ(r.get32(req + io_Unit), 0u);

    CHECK_EQ(static_cast<i32>(r.open_device(req, 2)), static_cast<i32>(IOERR_OPENFAIL));
    CHECK_EQ(r.get8(req + io_Error), static_cast<u8>(IOERR_OPENFAIL));
    CHECK_EQ(r.get32(req + io_Unit), 0u);

    CHECK_EQ(r.open_device(req, 1), 0u);
    r.close_request(req, port);
}

void reads_and_writes_through_byte_offsets() {
    TestDisk disk = make_test_disk(2048);
    g_units = DiskUnits{};
    g_units.count = 1;
    g_units.unit[0] = unit_of(disk);

    Rig r;
    u32 port = 0;
    const u32 req = r.create_request(port);
    CHECK_EQ(r.open_device(req, 0), 0u);

    r.put16(req + io_Command, CMD_READ);
    r.put32(req + io_Offset, 1024);
    r.put32(req + io_Length, 1024);
    r.put32(req + io_Data, kBuffer);
    r.do_io(req);
    CHECK_EQ(r.m.cpu.d[0], 0u);
    CHECK_EQ(r.get32(req + io_Actual), 1024u);
    for (u32 i = 0; i < 1024; ++i) {
        CHECK_EQ(r.get8(kBuffer + i), disk.bytes[1024 + i]);
    }

    for (u32 i = 0; i < 1024; ++i) {
        r.put8(kBuffer + i, static_cast<u8>(i * 3));
    }
    r.put16(req + io_Command, CMD_WRITE);
    r.put32(req + io_Offset, 4096);
    r.put32(req + io_Length, 1024);
    r.put32(req + io_Data, kBuffer);
    r.do_io(req);
    CHECK_EQ(r.m.cpu.d[0], 0u);
    CHECK_EQ(r.get32(req + io_Actual), 1024u);
    CHECK_EQ(disk.flush_calls, 1u);
    for (u32 i = 0; i < 1024; ++i) {
        CHECK_EQ(disk.bytes[4096 + i], static_cast<u8>(i * 3));
    }

    r.put16(req + io_Command, CMD_READ);
    r.put32(req + io_Offset, 100);
    r.put32(req + io_Length, 512);
    r.do_io(req);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), static_cast<i32>(IOERR_BADLENGTH));

    r.put32(req + io_Offset, 0);
    r.put32(req + io_Length, 500);
    r.do_io(req);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), static_cast<i32>(IOERR_BADLENGTH));

    r.put32(req + io_Length, 2048 * kSectorSize);
    r.put32(req + io_Offset, 2048 * kSectorSize);
    r.do_io(req);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), static_cast<i32>(IOERR_BADADDRESS));

    r.put32(req + io_Offset, 0);
    r.put32(req + io_Length, 2048 * kSectorSize + kSectorSize);
    r.do_io(req);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), static_cast<i32>(IOERR_BADADDRESS));

    disk.fail_sector = 3;
    r.put16(req + io_Command, CMD_READ);
    r.put32(req + io_Offset, 1024);
    r.put32(req + io_Length, 1024);
    r.do_io(req);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), static_cast<i32>(TDERR_NotSpecified));
    CHECK_EQ(r.get32(req + io_Actual), kSectorSize);

    disk.fail_sector = 5;
    disk.flush_calls = 0;
    r.put16(req + io_Command, CMD_WRITE);
    r.put32(req + io_Offset, 2048);
    r.put32(req + io_Length, 1024);
    r.do_io(req);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), static_cast<i32>(TDERR_NotSpecified));
    CHECK_EQ(r.get32(req + io_Actual), kSectorSize);
    CHECK_EQ(disk.flush_calls, 0u);

    disk.fail_sector = ~0ull;
    disk.flush_calls = 0;
    r.put16(req + io_Command, CMD_UPDATE);
    r.do_io(req);
    CHECK_EQ(r.m.cpu.d[0], 0u);
    CHECK_EQ(disk.flush_calls, 1u);

    disk.fail_flush = true;
    r.do_io(req);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), static_cast<i32>(TDERR_NotSpecified));

    r.close_request(req, port);
}

void reads_and_writes_at_64_bit_offsets() {
    TestDisk disk = make_sparse_disk(0x2000000);
    g_units = DiskUnits{};
    g_units.count = 1;
    g_units.unit[0] = unit_of(disk);

    Rig r;
    u32 port = 0;
    const u32 req = r.create_request(port);
    CHECK_EQ(r.open_device(req, 0), 0u);

    r.put16(req + io_Command, TD_READ64);
    r.put32(req + io_Actual, 2);
    r.put32(req + io_Offset, 0x10000);
    r.put32(req + io_Length, kSectorSize);
    r.put32(req + io_Data, kBuffer);
    r.do_io(req);
    CHECK_EQ(r.m.cpu.d[0], 0u);
    CHECK_EQ(r.get32(req + io_Actual), kSectorSize);
    CHECK_EQ(r.get8(kBuffer), 0x80u);
    CHECK_EQ(r.get8(kBuffer + 1), 0x81u);

    r.put16(req + io_Command, NSCMD_TD_READ64);
    r.put32(req + io_Actual, 2);
    r.put32(req + io_Offset, 0x20000);
    r.put32(req + io_Length, kSectorSize);
    r.put32(req + io_Data, kBuffer);
    r.do_io(req);
    CHECK_EQ(r.m.cpu.d[0], 0u);
    CHECK_EQ(r.get8(kBuffer), 0x00u);
    CHECK_EQ(r.get8(kBuffer + 1), 0x01u);

    r.put16(req + io_Command, NSCMD_TD_WRITE64);
    r.put32(req + io_Actual, 3);
    r.put32(req + io_Offset, 0);
    r.put32(req + io_Length, kSectorSize);
    r.put32(req + io_Data, kBuffer);
    r.do_io(req);
    CHECK_EQ(r.m.cpu.d[0], 0u);
    CHECK_EQ(disk.last_write_sector, 0x1800000u);

    r.put16(req + io_Command, TD_SEEK64);
    r.put32(req + io_Actual, 3);
    r.put32(req + io_Offset, 0);
    r.do_io(req);
    CHECK_EQ(r.m.cpu.d[0], 0u);

    r.close_request(req, port);
}

void geometry_protection_and_change_state() {
    TestDisk disk = make_test_disk(2048);
    TestDisk protected_disk = make_test_disk(64, false);
    g_units = DiskUnits{};
    g_units.count = 2;
    g_units.unit[0] = unit_of(disk);
    g_units.unit[1] = unit_of(protected_disk);

    Rig r;
    u32 port = 0;
    const u32 req = r.create_request(port);
    CHECK_EQ(r.open_device(req, 0), 0u);

    r.put16(req + io_Command, TD_GETGEOMETRY);
    r.put32(req + io_Length, 32);
    r.put32(req + io_Data, kGeometry);
    r.do_io(req);
    CHECK_EQ(r.m.cpu.d[0], 0u);
    CHECK_EQ(r.get32(req + io_Actual), 32u);
    CHECK_EQ(r.get32(kGeometry + dg_SectorSize), kSectorSize);
    CHECK_EQ(r.get32(kGeometry + dg_TotalSectors), 2048u);
    CHECK_EQ(r.get32(kGeometry + dg_Cylinders), 2u);
    CHECK_EQ(r.get32(kGeometry + dg_CylSectors), 1008u);
    CHECK_EQ(r.get32(kGeometry + dg_Heads), 16u);
    CHECK_EQ(r.get32(kGeometry + dg_TrackSectors), 63u);
    CHECK_EQ(r.get32(kGeometry + dg_BufMemType), MEMF_PUBLIC);
    CHECK_EQ(r.get8(kGeometry + dg_DeviceType), 0u);
    CHECK_EQ(r.get8(kGeometry + dg_Flags), 0u);
    CHECK_EQ(r.get16(kGeometry + dg_Reserved), 0u);

    r.put32(req + io_Length, 16);
    r.do_io(req);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), static_cast<i32>(IOERR_BADLENGTH));

    r.put16(req + io_Command, TD_GETNUMTRACKS);
    r.do_io(req);
    CHECK_EQ(r.get32(req + io_Actual), 32u);

    r.put16(req + io_Command, TD_PROTSTATUS);
    r.do_io(req);
    CHECK_EQ(r.get32(req + io_Actual), 0u);

    r.put16(req + io_Command, TD_CHANGENUM);
    r.do_io(req);
    CHECK_EQ(r.get32(req + io_Actual), 1u);

    r.put16(req + io_Command, TD_CHANGESTATE);
    r.do_io(req);
    CHECK_EQ(r.get32(req + io_Actual), 0u);

    r.put16(req + io_Command, TD_GETDRIVETYPE);
    r.do_io(req);
    CHECK_EQ(r.get32(req + io_Actual), 1u);

    r.put16(req + io_Command, TD_MOTOR);
    r.do_io(req);
    CHECK_EQ(r.get32(req + io_Actual), 1u);

    r.put16(req + io_Command, TD_SEEK);
    r.put32(req + io_Offset, 512);
    r.do_io(req);
    CHECK_EQ(r.m.cpu.d[0], 0u);

    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_exec(exec_lvo::CloseDevice), ExitReason::Returned);
    CHECK_EQ(r.open_device(req, 1), 0u);

    r.put16(req + io_Command, TD_PROTSTATUS);
    r.do_io(req);
    CHECK_EQ(r.get32(req + io_Actual), 1u);

    r.put16(req + io_Command, CMD_WRITE);
    r.put32(req + io_Offset, 0);
    r.put32(req + io_Length, kSectorSize);
    r.put32(req + io_Data, kBuffer);
    r.do_io(req);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), static_cast<i32>(TDERR_WriteProt));

    r.close_request(req, port);
}

void put_scsi_command(Rig& r, u32 data, u32 data_len, u16 cdb_len, u8 flags, u32 sense,
                      u16 sense_len) {
    for (u32 i = 0; i < SCSICMD_SIZE; ++i) {
        r.put8(kScsi + i, 0);
    }
    r.put32(kScsi + scsi_Data, data);
    r.put32(kScsi + scsi_Length, data_len);
    r.put32(kScsi + scsi_Actual, 0xDEADBEEF);
    r.put32(kScsi + scsi_Command, kCdb);
    r.put16(kScsi + scsi_CmdLength, cdb_len);
    r.put16(kScsi + scsi_CmdActual, 0xFFFF);
    r.put8(kScsi + scsi_Flags, flags);
    r.put8(kScsi + scsi_Status, 0xFF);
    r.put32(kScsi + scsi_SenseData, sense);
    r.put16(kScsi + scsi_SenseLength, sense_len);
    r.put16(kScsi + scsi_SenseActual, 0xFFFF);
}

void put_cdb(Rig& r, std::initializer_list<u8> cdb) {
    u32 at = kCdb;
    for (u8 byte : cdb) {
        r.put8(at++, byte);
    }
}

void scsi_cmd_inquiry_read_and_autosense() {
    TestDisk disk = make_test_disk(2048);
    g_units = DiskUnits{};
    g_units.count = 1;
    g_units.unit[0] = unit_of(disk);

    Rig r;
    u32 port = 0;
    const u32 req = r.create_request(port);
    CHECK_EQ(r.open_device(req, 0), 0u);

    put_scsi_command(r, kBuffer, 96, 6, 1, kSense, 18);
    put_cdb(r, {0x12, 0, 0, 0, 36, 0});
    r.put16(req + io_Command, HD_SCSICMD);
    r.put32(req + io_Length, SCSICMD_SIZE);
    r.put32(req + io_Data, kScsi);
    r.do_io(req);
    CHECK_EQ(r.m.cpu.d[0], 0u);
    CHECK_EQ(r.get32(req + io_Actual), SCSICMD_SIZE);
    CHECK_EQ(r.get32(kScsi + scsi_Actual), 36u);
    CHECK_EQ(r.get16(kScsi + scsi_CmdActual), 6u);
    CHECK_EQ(r.get8(kScsi + scsi_Status), 0u);
    CHECK_EQ(r.get16(kScsi + scsi_SenseActual), 0u);
    CHECK_EQ(r.get8(kBuffer + 8), static_cast<u8>('H'));
    CHECK_EQ(r.get8(kBuffer + 35), static_cast<u8>(' '));

    put_scsi_command(r, kBuffer, 1024, 10, 1, kSense, 18);
    put_cdb(r, {0x28, 0, 0, 0, 0, 2, 0, 0, 2, 0});
    r.put16(req + io_Command, HD_SCSICMD);
    r.put32(req + io_Length, SCSICMD_SIZE);
    r.put32(req + io_Data, kScsi);
    r.do_io(req);
    CHECK_EQ(r.m.cpu.d[0], 0u);
    CHECK_EQ(r.get32(kScsi + scsi_Actual), 1024u);
    CHECK_EQ(r.get16(kScsi + scsi_CmdActual), 10u);
    CHECK_EQ(r.get8(kScsi + scsi_Status), 0u);
    CHECK_EQ(r.get8(kBuffer), disk.bytes[2 * kSectorSize]);
    CHECK_EQ(r.get8(kBuffer + 1023), disk.bytes[3 * kSectorSize + 511]);

    put_scsi_command(r, kBuffer, 1024, 10, SCSIF_AUTOSENSE, kSense, 18);
    put_cdb(r, {0x28, 0, 0, 0, 0x08, 0, 0, 0, 1, 0});
    r.put16(req + io_Command, HD_SCSICMD);
    r.put32(req + io_Length, SCSICMD_SIZE);
    r.put32(req + io_Data, kScsi);
    r.do_io(req);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), static_cast<i32>(HFERR_BadStatus));
    CHECK_EQ(static_cast<i8>(r.get8(req + io_Error)), HFERR_BadStatus);
    CHECK_EQ(r.get32(kScsi + scsi_Actual), 0u);
    CHECK_EQ(r.get8(kScsi + scsi_Status), 2u);
    CHECK_EQ(r.get16(kScsi + scsi_SenseActual), 18u);
    CHECK_EQ(r.get8(kSense), 0x70u);
    CHECK_EQ(r.get8(kSense + 2), 5u);
    CHECK_EQ(r.get8(kSense + 7), 10u);
    CHECK_EQ(r.get8(kSense + 12), 0x21u);

    put_scsi_command(r, kBuffer, 1024, 10, 1, kSense, 18);
    put_cdb(r, {0x28, 0, 0, 0, 0x08, 0, 0, 0, 1, 0});
    r.put16(req + io_Command, HD_SCSICMD);
    r.put32(req + io_Length, SCSICMD_SIZE);
    r.put32(req + io_Data, kScsi);
    r.do_io(req);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), static_cast<i32>(HFERR_BadStatus));
    CHECK_EQ(r.get16(kScsi + scsi_SenseActual), 0u);

    put_scsi_command(r, kBuffer, 512, 6, 1, kSense, 18);
    put_cdb(r, {0x12, 0, 0, 0, 36, 0});
    r.put16(req + io_Command, HD_SCSICMD);
    r.put32(req + io_Length, 29);
    r.put32(req + io_Data, kScsi);
    r.do_io(req);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), static_cast<i32>(IOERR_BADLENGTH));

    put_scsi_command(r, kBuffer, 512, 17, 1, kSense, 18);
    r.put16(req + io_Command, HD_SCSICMD);
    r.put32(req + io_Length, SCSICMD_SIZE);
    r.put32(req + io_Data, kScsi);
    r.do_io(req);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), static_cast<i32>(IOERR_BADLENGTH));

    r.close_request(req, port);
}

void device_query_lists_supported_commands() {
    TestDisk disk = make_test_disk(2048);
    g_units = DiskUnits{};
    g_units.count = 1;
    g_units.unit[0] = unit_of(disk);

    Rig r;
    u32 port = 0;
    const u32 req = r.create_request(port);
    CHECK_EQ(r.open_device(req, 0), 0u);

    r.put16(req + io_Command, NSCMD_DEVICEQUERY);
    r.put32(req + io_Length, NSDQR_SIZE);
    r.put32(req + io_Data, kQuery);
    r.do_io(req);
    CHECK_EQ(r.m.cpu.d[0], 0u);
    CHECK_EQ(r.get32(req + io_Actual), NSDQR_SIZE);
    CHECK_EQ(r.get32(kQuery + nsdqr_DevQueryFormat), 0u);
    CHECK_EQ(r.get32(kQuery + nsdqr_SizeAvailable), NSDQR_SIZE);
    CHECK_EQ(r.get16(kQuery + nsdqr_DeviceType), NSDEVTYPE_TRACKDISK);
    CHECK_EQ(r.get16(kQuery + nsdqr_DeviceSubType), 0u);

    const u32 list = r.get32(kQuery + nsdqr_SupportedCommands);
    CHECK(list != 0);
    bool has_read = false;
    bool has_write = false;
    bool has_scsi = false;
    bool has_query = false;
    bool terminated = false;
    for (u32 i = 0; i < 64; ++i) {
        const u16 command = static_cast<u16>(r.get16(list + i * 2));
        if (command == 0) {
            terminated = true;
            break;
        }
        has_read = has_read || command == CMD_READ;
        has_write = has_write || command == CMD_WRITE;
        has_scsi = has_scsi || command == HD_SCSICMD;
        has_query = has_query || command == NSCMD_DEVICEQUERY;
    }
    CHECK(terminated);
    CHECK(has_read);
    CHECK(has_write);
    CHECK(has_scsi);
    CHECK(has_query);

    r.put32(req + io_Length, 8);
    r.do_io(req);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), static_cast<i32>(IOERR_BADLENGTH));

    r.close_request(req, port);
}

void invalid_units_unknown_commands_and_abort() {
    TestDisk disk = make_test_disk(2048);
    g_units = DiskUnits{};
    g_units.count = 1;
    g_units.unit[0] = unit_of(disk);

    Rig r;
    u32 port = 0;
    const u32 req = r.create_request(port);
    CHECK_EQ(r.open_device(req, 0), 0u);
    const u32 unit = r.get32(req + io_Unit);

    r.put16(req + io_Command, CMD_READ);
    r.put32(req + io_Unit, 0);
    r.put32(req + io_Length, kSectorSize);
    r.do_io(req);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), static_cast<i32>(TDERR_BadUnitNum));

    r.put32(req + io_Unit, unit);
    r.put16(req + io_Command, 99);
    r.do_io(req);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), static_cast<i32>(IOERR_NOCMD));

    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_exec(exec_lvo::AbortIO), ExitReason::Returned);
    CHECK_EQ(r.m.cpu.d[0], 0u);

    r.put16(req + io_Command, TD_ADDCHANGEINT);
    r.put8(req + io_Flags, IOF_QUICK);
    r.put8(req + io_Error, 0x7F);
    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_scsi(dev_lvo::BeginIO), ExitReason::Returned);
    CHECK_EQ(r.get8(req + io_Flags) & IOF_QUICK, 0u);
    CHECK_EQ(r.get8(req + io_Error), 0u);
    CHECK_EQ(g_exec.get_message(port), 0u);

    r.close_request(req, port);
}

}

int main() {
    opens_units_and_fails_for_missing_ones();
    reads_and_writes_through_byte_offsets();
    reads_and_writes_at_64_bit_offsets();
    geometry_protection_and_change_state();
    scsi_cmd_inquiry_read_and_autosense();
    device_query_lists_supported_commands();
    invalid_units_unknown_commands_and_abort();
    return check::finish("test_scsi_device");
}
