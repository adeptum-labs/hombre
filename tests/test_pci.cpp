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

#include "check.hpp"
#include "pci.hpp"

using namespace hombre;

namespace {

void config_address_encoding() {
    CHECK_EQ(pci_config_address({0, 0, 0}, 0), 0x80000000u);
    CHECK_EQ(pci_config_address({0, 0, 0}, 4), 0x80000004u);
    CHECK_EQ(pci_config_address({1, 0, 0}, 0), 0x80010000u);
    CHECK_EQ(pci_config_address({0, 1, 0}, 0), 0x80000800u);
    CHECK_EQ(pci_config_address({0, 0, 1}, 0), 0x80000100u);
    CHECK_EQ(pci_config_address({0xFF, 0x1F, 7}, 0xFC), 0x80FFFFFCu);
    CHECK_EQ(pci_config_address({0x12, 0x03, 5}, 0x10), 0x80121D10u);
    // The two low offset bits select nothing: they are masked off.
    CHECK_EQ(pci_config_address({0, 2, 0}, 0x07), 0x80001004u);
    CHECK_EQ(pci_config_address({0, 2, 0}, 0x03), 0x80001000u);
    CHECK_EQ(pci_config_address({0, 2, 0}, 0xFF), 0x800010FCu);
}

void decode_bar_memory32() {
    const PciBar bar = pci_decode_bar(0xFEBD1000, 0);
    CHECK_EQ(bar.phys, 0xFEBD1000ull);
    CHECK(!bar.is_io);
    CHECK(!bar.is_64bit);
}

void decode_bar_memory64_above_4gib() {
    // Memory BAR, 64-bit type (bits 2:1 = 2), prefetchable.
    const PciBar bar = pci_decode_bar(0x0000000C | 0x80000000u, 0x00000004);
    CHECK_EQ(bar.phys, 0x0000000480000000ull);
    CHECK(!bar.is_io);
    CHECK(bar.is_64bit);
}

void decode_bar_io() {
    const PciBar bar = pci_decode_bar(0x0000C041, 0xFFFFFFFF);
    CHECK_EQ(bar.phys, 0xC040ull);
    CHECK(bar.is_io);
    CHECK(!bar.is_64bit);
}

void decode_bar_masks_flag_bits() {
    // Prefetchable bit and the 32-bit type must not leak into the address.
    const PciBar prefetchable = pci_decode_bar(0xE0000008, 0);
    CHECK_EQ(prefetchable.phys, 0xE0000000ull);
    CHECK(!prefetchable.is_64bit);
    // Type 1 (below 1 MiB) is neither 64-bit nor changes the address.
    const PciBar below_1m = pci_decode_bar(0x00080002, 0);
    CHECK_EQ(below_1m.phys, 0x00080000ull);
    CHECK(!below_1m.is_64bit);
    const PciBar wide = pci_decode_bar(0xFEC0000C, 0);
    CHECK_EQ(wide.phys, 0xFEC00000ull);
    CHECK(wide.is_64bit);
    // I/O: bit 1 is reserved and masked along with the indicator bit.
    const PciBar io = pci_decode_bar(0x0000D003, 0);
    CHECK_EQ(io.phys, 0xD000ull);
}

void decode_bar_memory32_ignores_high() {
    const PciBar bar = pci_decode_bar(0xFEBD1000, 0x00000007);
    CHECK_EQ(bar.phys, 0xFEBD1000ull);
    CHECK(!bar.is_64bit);
}

}

int main() {
    config_address_encoding();
    decode_bar_memory32();
    decode_bar_memory64_above_4gib();
    decode_bar_io();
    decode_bar_masks_flag_bits();
    decode_bar_memory32_ignores_high();
    return check::finish("test_pci");
}
