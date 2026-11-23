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

#include "../disk_unit.hpp"
#include "../types.hpp"
#include "exec.hpp"

// scsi.device: a native Exec device that exposes every DiskUnit as one unit
// of a SCSI direct access disk. It answers the trackdisk commands and
// HD_SCSICMD synchronously, inside BeginIO.

namespace hombre {

namespace ndk {

// Commands of devices/trackdisk.h.
constexpr u16 TD_MOTOR = 9;
constexpr u16 TD_SEEK = 10;
constexpr u16 TD_FORMAT = 11;
constexpr u16 TD_REMOVE = 12;
constexpr u16 TD_CHANGENUM = 13;
constexpr u16 TD_CHANGESTATE = 14;
constexpr u16 TD_PROTSTATUS = 15;
constexpr u16 TD_GETDRIVETYPE = 18;
constexpr u16 TD_GETNUMTRACKS = 19;
constexpr u16 TD_ADDCHANGEINT = 20;
constexpr u16 TD_REMCHANGEINT = 21;
constexpr u16 TD_GETGEOMETRY = 22;
constexpr u16 TD_EJECT = 23;
constexpr u16 TD_READ64 = 24;
constexpr u16 TD_WRITE64 = 25;
constexpr u16 TD_SEEK64 = 26;
constexpr u16 TD_FORMAT64 = 27;

// struct DriveGeometry and its values.
constexpr u32 dg_SectorSize = 0;
constexpr u32 dg_TotalSectors = 4;
constexpr u32 dg_Cylinders = 8;
constexpr u32 dg_CylSectors = 12;
constexpr u32 dg_Heads = 16;
constexpr u32 dg_TrackSectors = 20;
constexpr u32 dg_BufMemType = 24;
constexpr u32 dg_DeviceType = 28;
constexpr u32 dg_Flags = 29;
constexpr u32 dg_Reserved = 30;
constexpr u32 DG_SIZE = 32;

// Driver errors of devices/trackdisk.h.
constexpr i8 TDERR_NotSpecified = 20;
constexpr i8 TDERR_WriteProt = 28;
constexpr i8 TDERR_BadUnitNum = 32;

// HD_SCSICMD and struct SCSICmd of devices/scsidisk.h.
constexpr u16 HD_SCSICMD = 28;
constexpr u32 scsi_Data = 0;
constexpr u32 scsi_Length = 4;
constexpr u32 scsi_Actual = 8;
constexpr u32 scsi_Command = 12;
constexpr u32 scsi_CmdLength = 16;
constexpr u32 scsi_CmdActual = 18;
constexpr u32 scsi_Flags = 20;
constexpr u32 scsi_Status = 21;
constexpr u32 scsi_SenseData = 22;
constexpr u32 scsi_SenseLength = 26;
constexpr u32 scsi_SenseActual = 28;
constexpr u32 SCSICMD_SIZE = 30;
constexpr u8 SCSIF_AUTOSENSE = 2;
constexpr i8 HFERR_BadStatus = 45;

// NSCMD_DEVICEQUERY, struct NSDeviceQueryResult and its values, of
// devices/newstyle.h.
constexpr u16 NSCMD_DEVICEQUERY = 0x4000;
constexpr u16 NSCMD_TD_READ64 = 0xC000;
constexpr u16 NSCMD_TD_WRITE64 = 0xC001;
constexpr u16 NSCMD_TD_SEEK64 = 0xC002;
constexpr u16 NSCMD_TD_FORMAT64 = 0xC003;
constexpr u32 nsdqr_DevQueryFormat = 0;
constexpr u32 nsdqr_SizeAvailable = 4;
constexpr u32 nsdqr_DeviceType = 8;
constexpr u32 nsdqr_DeviceSubType = 10;
constexpr u32 nsdqr_SupportedCommands = 12;
constexpr u32 NSDQR_SIZE = 16;
constexpr u16 NSDEVTYPE_TRACKDISK = 5;

// Device errors exec/io.h leaves out.
constexpr i8 IOERR_BADADDRESS = -5;

}

class ScsiDevice {
public:
    static constexpr u16 kVersion = 40;

    // Creates "scsi.device" version 40 on ExecBase->DeviceList. Unit n is
    // openable when units is not null and holds more than n units. exec
    // must be initialised.
    bool init(m68k::Machine& m, Exec& exec, const DiskUnits* units);

    u32 device_base() const { return base_; }

private:
    friend struct ScsiOps;

    // Each unit is a struct Unit of 38 bytes, kept long aligned, so a
    // program that looks into io_Unit finds one.
    static constexpr u32 kUnitSize = 40;
    static constexpr u32 kMaxUnits = DiskUnits::kMax;

    Exec* exec_ = nullptr;
    const DiskUnits* units_ = nullptr;
    u32 base_ = 0;
    u32 units_base_ = 0;
    u32 supported_commands_ = 0;
};

}
