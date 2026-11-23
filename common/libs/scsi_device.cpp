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

#include "scsi_device.hpp"

#include "../scsi_disk.hpp"

namespace hombre {

using m68k::Machine;
using native::Bus;
using namespace ndk;

namespace {

constexpr u32 kSectorSize = 512;

void fault_null(Bus& b) {
    b.bytes_at(0, b.machine().mem.size + 1);
}

}

struct ScsiOps {
    static ScsiDevice& of(void* ctx) { return *static_cast<ScsiDevice*>(ctx); }

    static bool decode_unit(const ScsiDevice& d, u32 unit_val, u32& unit_out) {
        if (d.units_ == nullptr || unit_val < d.units_base_) {
            return false;
        }
        const u32 offset = unit_val - d.units_base_;
        if (offset >= ScsiDevice::kMaxUnits * ScsiDevice::kUnitSize ||
            (offset % ScsiDevice::kUnitSize) != 0) {
            return false;
        }
        const u32 unit = offset / ScsiDevice::kUnitSize;
        if (unit >= d.units_->count) {
            return false;
        }
        unit_out = unit;
        return true;
    }

    static void complete_now(ScsiDevice& d, Bus& b, u32 io_raw, u8 flags, i8 err) {
        b.w8(io_raw + io_Error, static_cast<u8>(err));
        if (!b.ok()) {
            return;
        }
        if ((flags & IOF_QUICK) != 0) {
            return;
        }
        d.exec_->reply_message(io_raw);
    }

    static void open(void* ctx, Machine& m) {
        ScsiDevice& d = of(ctx);
        Bus b(m);
        const u32 unit = m.cpu.d[0];
        const u32 io_raw = m.cpu.a[1];
        const u32 dev = m.cpu.a[6];
        if (native::guest_address(m, io_raw) == 0) {
            fault_null(b);
            return;
        }
        if (d.units_ == nullptr || unit >= d.units_->count || unit >= DiskUnits::kMax) {
            b.w32(io_raw + io_Unit, 0);
            b.w32(io_raw + io_Device, 0);
            b.w8(io_raw + io_Error, static_cast<u8>(IOERR_OPENFAIL));
            if (b.ok()) {
                native::set_result(m, static_cast<u32>(-1));
            }
            return;
        }
        b.w16(dev + LIB_OPENCNT, b.r16(dev + LIB_OPENCNT) + 1);
        b.w8(dev + LIB_FLAGS, b.r8(dev + LIB_FLAGS) & ~LIBF_DELEXP);
        b.w32(io_raw + io_Unit, d.units_base_ + unit * ScsiDevice::kUnitSize);
        b.w8(io_raw + io_Error, 0);
        if (b.ok()) {
            native::set_result(m, 0);
        }
    }

    static void close(void*, Machine& m) {
        Bus b(m);
        const u32 io_raw = m.cpu.a[1];
        const u32 dev = m.cpu.a[6];
        const u32 count = b.r16(dev + LIB_OPENCNT);
        if (count != 0) {
            b.w16(dev + LIB_OPENCNT, count - 1);
        }
        if (native::guest_address(m, io_raw) != 0) {
            b.w32(io_raw + io_Unit, 0);
        }
        native::set_result(m, 0);
    }

    static void read_write(ScsiDevice& d, Bus& b, u32 io_raw, u8 flags, const DiskUnit& unit,
                           bool writing, u64 offset) {
        const u32 length = b.r32(io_raw + io_Length);
        const u32 data_addr = b.r32(io_raw + io_Data);
        if (!b.ok()) {
            return;
        }
        if ((offset % kSectorSize) != 0 || (length % kSectorSize) != 0) {
            complete_now(d, b, io_raw, flags, IOERR_BADLENGTH);
            return;
        }
        const u64 start = offset / kSectorSize;
        const u64 count = length / kSectorSize;
        if (start > unit.sectors || count > unit.sectors - start) {
            complete_now(d, b, io_raw, flags, IOERR_BADADDRESS);
            return;
        }
        if (writing && unit.write == nullptr) {
            complete_now(d, b, io_raw, flags, TDERR_WriteProt);
            return;
        }
        u8* data = length == 0 ? nullptr : b.bytes_at(data_addr, length);
        if (!b.ok()) {
            return;
        }
        u32 done = 0;
        for (u64 i = 0; i < count; ++i) {
            const bool ok = writing ? unit.write(unit.ctx, start + i, data + i * kSectorSize)
                                    : unit.read(unit.ctx, start + i, data + i * kSectorSize);
            if (!ok) {
                b.w32(io_raw + io_Actual, done);
                complete_now(d, b, io_raw, flags, TDERR_NotSpecified);
                return;
            }
            done += kSectorSize;
        }
        if (writing && unit.flush != nullptr && !unit.flush(unit.ctx)) {
            b.w32(io_raw + io_Actual, done);
            complete_now(d, b, io_raw, flags, TDERR_NotSpecified);
            return;
        }
        b.w32(io_raw + io_Actual, done);
        complete_now(d, b, io_raw, flags, 0);
    }

    static void drive_geometry(ScsiDevice& d, Bus& b, u32 io_raw, u8 flags,
                               const DiskUnit& unit) {
        const u32 length = b.r32(io_raw + io_Length);
        const u32 data_addr = b.r32(io_raw + io_Data);
        if (!b.ok()) {
            return;
        }
        if (length < DG_SIZE) {
            complete_now(d, b, io_raw, flags, IOERR_BADLENGTH);
            return;
        }
        const DiskGeometry geometry = disk_geometry(unit.sectors);
        b.w32(data_addr + dg_SectorSize, kSectorSize);
        b.w32(data_addr + dg_TotalSectors,
              unit.sectors > 0xFFFFFFFF ? 0xFFFFFFFF : static_cast<u32>(unit.sectors));
        b.w32(data_addr + dg_Cylinders, geometry.cylinders);
        b.w32(data_addr + dg_CylSectors, geometry.heads * geometry.track_sectors);
        b.w32(data_addr + dg_Heads, geometry.heads);
        b.w32(data_addr + dg_TrackSectors, geometry.track_sectors);
        b.w32(data_addr + dg_BufMemType, MEMF_PUBLIC);
        b.w8(data_addr + dg_DeviceType, 0);
        b.w8(data_addr + dg_Flags, 0);
        b.w16(data_addr + dg_Reserved, 0);
        b.w32(io_raw + io_Actual, DG_SIZE);
        complete_now(d, b, io_raw, flags, 0);
    }

    static void scsi_command(ScsiDevice& d, Bus& b, u32 io_raw, u8 flags, const DiskUnit& unit) {
        const u32 length = b.r32(io_raw + io_Length);
        const u32 struct_addr = b.r32(io_raw + io_Data);
        if (!b.ok()) {
            return;
        }
        if (length < SCSICMD_SIZE) {
            complete_now(d, b, io_raw, flags, IOERR_BADLENGTH);
            return;
        }
        const u32 data_addr = b.r32(struct_addr + scsi_Data);
        const u32 data_len = b.r32(struct_addr + scsi_Length);
        const u32 cdb_addr = b.r32(struct_addr + scsi_Command);
        const u16 cdb_len = static_cast<u16>(b.r16(struct_addr + scsi_CmdLength));
        const u8 scsi_flags = static_cast<u8>(b.r8(struct_addr + scsi_Flags));
        const u32 sense_addr = b.r32(struct_addr + scsi_SenseData);
        const u16 sense_len = static_cast<u16>(b.r16(struct_addr + scsi_SenseLength));
        if (!b.ok()) {
            return;
        }
        if (cdb_len == 0 || cdb_len > 16) {
            complete_now(d, b, io_raw, flags, IOERR_BADLENGTH);
            return;
        }
        const u8* cdb = b.bytes_at(cdb_addr, cdb_len);
        if (!b.ok()) {
            return;
        }
        u8* data = data_len == 0 ? nullptr : b.bytes_at(data_addr, data_len);
        if (!b.ok()) {
            return;
        }
        const ScsiResult result = scsi_execute(unit, cdb, cdb_len, data, data_len);
        b.w32(struct_addr + scsi_Actual, result.actual);
        b.w16(struct_addr + scsi_CmdActual, cdb_len);
        b.w8(struct_addr + scsi_Status, result.status);
        u32 sense_actual = 0;
        if (result.status != 0 && (scsi_flags & SCSIF_AUTOSENSE) != 0 && sense_addr != 0 &&
            sense_len != 0) {
            u8* sense = b.bytes_at(sense_addr, sense_len);
            if (!b.ok()) {
                return;
            }
            scsi_sense(result, sense, sense_len, sense_actual);
        }
        b.w16(struct_addr + scsi_SenseActual, static_cast<u16>(sense_actual));
        b.w32(io_raw + io_Actual, length);
        complete_now(d, b, io_raw, flags, result.status != 0 ? HFERR_BadStatus : 0);
    }

    static void device_query(ScsiDevice& d, Bus& b, u32 io_raw, u8 flags) {
        const u32 length = b.r32(io_raw + io_Length);
        const u32 data_addr = b.r32(io_raw + io_Data);
        if (!b.ok()) {
            return;
        }
        if (length < NSDQR_SIZE) {
            complete_now(d, b, io_raw, flags, IOERR_BADLENGTH);
            return;
        }
        b.w32(data_addr + nsdqr_DevQueryFormat, 0);
        b.w32(data_addr + nsdqr_SizeAvailable, NSDQR_SIZE);
        b.w16(data_addr + nsdqr_DeviceType, NSDEVTYPE_TRACKDISK);
        b.w16(data_addr + nsdqr_DeviceSubType, 0);
        b.w32(data_addr + nsdqr_SupportedCommands, d.supported_commands_);
        b.w32(io_raw + io_Actual, NSDQR_SIZE);
        complete_now(d, b, io_raw, flags, 0);
    }

    static void begin_io(void* ctx, Machine& m) {
        ScsiDevice& d = of(ctx);
        Bus b(m);
        const u32 io_raw = m.cpu.a[1];
        if (native::guest_address(m, io_raw) == 0) {
            fault_null(b);
            return;
        }
        const u16 cmd = static_cast<u16>(b.r16(io_raw + io_Command));
        const u8 flags = static_cast<u8>(b.r8(io_raw + io_Flags));
        const u32 unit_val = b.r32(io_raw + io_Unit);
        if (!b.ok()) {
            return;
        }
        u32 unit = 0;
        if (!decode_unit(d, unit_val, unit)) {
            complete_now(d, b, io_raw, flags, TDERR_BadUnitNum);
            return;
        }
        const DiskUnit& u = d.units_->unit[unit];

        switch (cmd) {
        case CMD_RESET:
        case CMD_CLEAR:
        case CMD_STOP:
        case CMD_START:
        case CMD_FLUSH:
        case TD_SEEK:
        case TD_SEEK64:
        case NSCMD_TD_SEEK64:
        case TD_REMOVE:
        case TD_EJECT:
        case TD_REMCHANGEINT:
            complete_now(d, b, io_raw, flags, 0);
            return;
        case TD_MOTOR:
        case TD_CHANGENUM:
        case TD_GETDRIVETYPE:
            b.w32(io_raw + io_Actual, 1);
            complete_now(d, b, io_raw, flags, 0);
            return;
        case TD_CHANGESTATE:
            b.w32(io_raw + io_Actual, 0);
            complete_now(d, b, io_raw, flags, 0);
            return;
        case TD_PROTSTATUS:
            b.w32(io_raw + io_Actual, u.write == nullptr ? 1 : 0);
            complete_now(d, b, io_raw, flags, 0);
            return;
        case TD_GETNUMTRACKS: {
            const DiskGeometry geometry = disk_geometry(u.sectors);
            b.w32(io_raw + io_Actual, geometry.cylinders * geometry.heads);
            complete_now(d, b, io_raw, flags, 0);
            return;
        }
        case TD_ADDCHANGEINT:
            b.w8(io_raw + io_Error, 0);
            b.w8(io_raw + io_Flags, flags & ~IOF_QUICK);
            return;
        case CMD_UPDATE:
            if (u.flush != nullptr && !u.flush(u.ctx)) {
                complete_now(d, b, io_raw, flags, TDERR_NotSpecified);
            } else {
                complete_now(d, b, io_raw, flags, 0);
            }
            return;
        case CMD_READ:
        case TD_READ64:
        case NSCMD_TD_READ64: {
            const u32 high = cmd == CMD_READ ? 0 : b.r32(io_raw + io_Actual);
            const u32 low = b.r32(io_raw + io_Offset);
            if (!b.ok()) {
                return;
            }
            read_write(d, b, io_raw, flags, u, false, (static_cast<u64>(high) << 32) | low);
            return;
        }
        case CMD_WRITE:
        case TD_FORMAT:
        case TD_WRITE64:
        case TD_FORMAT64:
        case NSCMD_TD_WRITE64:
        case NSCMD_TD_FORMAT64: {
            const bool wide = cmd != CMD_WRITE && cmd != TD_FORMAT;
            const u32 high = wide ? b.r32(io_raw + io_Actual) : 0;
            const u32 low = b.r32(io_raw + io_Offset);
            if (!b.ok()) {
                return;
            }
            read_write(d, b, io_raw, flags, u, true, (static_cast<u64>(high) << 32) | low);
            return;
        }
        case TD_GETGEOMETRY:
            drive_geometry(d, b, io_raw, flags, u);
            return;
        case HD_SCSICMD:
            scsi_command(d, b, io_raw, flags, u);
            return;
        case NSCMD_DEVICEQUERY:
            device_query(d, b, io_raw, flags);
            return;
        default:
            complete_now(d, b, io_raw, flags, IOERR_NOCMD);
            return;
        }
    }

    static void abort_io(void*, Machine& m) {
        Bus b(m);
        const u32 io_raw = m.cpu.a[1];
        if (native::guest_address(m, io_raw) == 0) {
            fault_null(b);
            return;
        }
        native::set_result(m, 0);
    }
};

namespace {

constexpr NativeVector kScsiVectors[] = {
    {ScsiOps::open, "Open"},
    {ScsiOps::close, "Close"},
    {nullptr, "Expunge"},
    {nullptr, "Reserved"},
    {ScsiOps::begin_io, "BeginIO"},
    {ScsiOps::abort_io, "AbortIO"},
};

constexpr u16 kSupportedCommands[] = {
    CMD_RESET,           CMD_READ,            CMD_WRITE,           CMD_UPDATE,
    CMD_CLEAR,           CMD_STOP,            CMD_START,           CMD_FLUSH,
    TD_MOTOR,            TD_SEEK,             TD_FORMAT,           TD_REMOVE,
    TD_CHANGENUM,        TD_CHANGESTATE,      TD_PROTSTATUS,       TD_GETDRIVETYPE,
    TD_GETNUMTRACKS,     TD_ADDCHANGEINT,     TD_REMCHANGEINT,     TD_GETGEOMETRY,
    TD_EJECT,            TD_READ64,           TD_WRITE64,          TD_SEEK64,
    TD_FORMAT64,         HD_SCSICMD,          NSCMD_DEVICEQUERY,   NSCMD_TD_READ64,
    NSCMD_TD_WRITE64,    NSCMD_TD_SEEK64,     NSCMD_TD_FORMAT64,
};

constexpr u32 kCommandListBytes =
    (sizeof kSupportedCommands / sizeof kSupportedCommands[0] + 1) * 2;

}

bool ScsiDevice::init(Machine& m, Exec& exec, const DiskUnits* units) {
    exec_ = &exec;
    units_ = units;
    base_ = 0;
    units_base_ = 0;
    supported_commands_ = 0;

    constexpr u32 pos_size = LIB_SIZE + kMaxUnits * kUnitSize + kCommandListBytes;
    base_ = exec.make_native_device("scsi.device", kVersion, kScsiVectors,
                                    sizeof kScsiVectors / sizeof kScsiVectors[0], this, pos_size);
    if (base_ == 0) {
        exec_ = nullptr;
        units_ = nullptr;
        return false;
    }
    units_base_ = base_ + LIB_SIZE;
    supported_commands_ = units_base_ + kMaxUnits * kUnitSize;

    Bus b(m);
    u32 at = supported_commands_;
    for (u16 cmd : kSupportedCommands) {
        b.w16(at, cmd);
        at += 2;
    }
    b.w16(at, 0);
    if (!b.ok()) {
        base_ = 0;
        units_base_ = 0;
        supported_commands_ = 0;
        exec_ = nullptr;
        units_ = nullptr;
        return false;
    }
    return true;
}

}
