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

#include "acpi.hpp"

#include "../common/serial.hpp"

namespace hombre {

namespace {

constinit u64 direct_map_base = 0;
constinit AcpiProcessorList processors{};

// The tables are physical; the direct map makes all of physical memory
// readable, so this only has to refuse a range that wraps.
const u8* read_phys(u64 phys, u32 bytes) {
    if (bytes == 0 || direct_map_base == 0 || bytes - 1 > ~phys) {
        return nullptr;
    }
    return reinterpret_cast<const u8*>(direct_map_base + phys);
}

}

void acpi_init(const BootInfo& info) {
    direct_map_base = info.direct_map_base;
    processors = acpi_read_processors(info.rsdp_phys, &read_phys);
    serial::write("acpi: ");
    serial::write_dec(processors.count);
    serial::write(" cpus, 1 started\n");
}

const AcpiProcessorList& acpi_processors() {
    return processors;
}

}
