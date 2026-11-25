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

#include "acpi_madt.hpp"
#include "check.hpp"

using namespace hombre;

namespace {

// Above zero, which the walk reads as there being no RSDP at all.
constexpr u32 kRsdp = 0x040;
constexpr u32 kRsdt = 0x100;
constexpr u32 kXsdt = 0x200;
constexpr u32 kMadt = 0x300;

// The whole physical memory the synthetic tables live in.
u8 memory[0x800]{};

const u8* read_memory(u64 phys, u32 bytes) {
    if (bytes == 0 || phys > sizeof memory || bytes > sizeof memory - phys) {
        return nullptr;
    }
    return memory + phys;
}

void clear_memory() {
    for (u8& byte : memory) {
        byte = 0;
    }
}

void put_u32(u32 at, u32 value) {
    for (u32 i = 0; i < 4; ++i) {
        memory[at + i] = static_cast<u8>(value >> (8 * i));
    }
}

void put_u64(u32 at, u64 value) {
    for (u32 i = 0; i < 8; ++i) {
        memory[at + i] = static_cast<u8>(value >> (8 * i));
    }
}

void put_text(u32 at, const char* text, u32 length) {
    for (u32 i = 0; i < length; ++i) {
        memory[at + i] = static_cast<u8>(text[i]);
    }
}

// Puts the byte that makes the region from `from` sum to zero.
void seal(u32 checksum_at, u32 from, u32 length) {
    memory[checksum_at] = 0;
    u8 sum = 0;
    for (u32 i = from; i < from + length; ++i) {
        sum = static_cast<u8>(sum + memory[i]);
    }
    memory[checksum_at] = static_cast<u8>(0 - sum);
}

void put_header(u32 at, const char* signature, u32 length) {
    put_text(at, signature, 4);
    put_u32(at + 4, length);
    memory[at + 8] = 2;
}

void seal_table(u32 at, u32 length) {
    seal(at + 9, at, length);
}

u32 put_local_apic(u32 at, u32 processor_id, u32 apic_id, u32 flags) {
    memory[at] = 0;
    memory[at + 1] = 8;
    memory[at + 2] = static_cast<u8>(processor_id);
    memory[at + 3] = static_cast<u8>(apic_id);
    put_u32(at + 4, flags);
    return 8;
}

u32 put_x2_apic(u32 at, u32 apic_id, u32 flags) {
    memory[at] = 9;
    memory[at + 1] = 16;
    put_u32(at + 4, apic_id);
    put_u32(at + 8, flags);
    put_u32(at + 12, apic_id);
    return 16;
}

// An empty MADT, whose entries the caller appends; returns where they start.
u32 open_madt() {
    put_header(kMadt, "APIC", 0);
    put_u32(kMadt + 36, 0xFEE00000);
    put_u32(kMadt + 40, 1);
    return kMadt + 44;
}

void close_madt(u32 entries_end) {
    const u32 length = entries_end - kMadt;
    put_u32(kMadt + 4, length);
    seal_table(kMadt, length);
}

void put_root(u32 at, const char* signature, bool extended, u32 madt) {
    const u32 length = 36 + (extended ? 8 : 4);
    put_header(at, signature, length);
    if (extended) {
        put_u64(at + 36, madt);
    } else {
        put_u32(at + 36, madt);
    }
    seal_table(at, length);
}

void put_rsdp(u32 revision, bool extended, u32 root) {
    put_text(kRsdp, "RSD PTR ", 8);
    memory[kRsdp + 15] = static_cast<u8>(revision);
    put_u32(kRsdp + 16, root);
    if (extended) {
        put_u32(kRsdp + 20, 36);
        put_u64(kRsdp + 24, root);
    }
    seal(kRsdp + 8, kRsdp, 20);
    if (extended) {
        seal(kRsdp + 32, kRsdp, 36);
    }
}

void xsdt_lists_both_entry_kinds() {
    clear_memory();
    u32 at = open_madt();
    at += put_local_apic(at, 0, 0, 1);
    at += put_x2_apic(at, 4, 3);
    at += put_local_apic(at, 1, 1, 0);
    at += put_local_apic(at, 2, 2, 2);
    close_madt(at);
    put_root(kXsdt, "XSDT", true, kMadt);
    put_rsdp(2, true, kXsdt);

    const AcpiProcessorList list = acpi_read_processors(kRsdp, &read_memory);
    CHECK_EQ(list.count, 3u);
    CHECK_EQ(list.apic_id[0], 0u);
    CHECK_EQ(list.apic_id[1], 4u);
    CHECK_EQ(list.apic_id[2], 2u);
}

void rsdt_lists_processors() {
    clear_memory();
    u32 at = open_madt();
    at += put_local_apic(at, 0, 0, 1);
    at += put_local_apic(at, 1, 9, 1);
    close_madt(at);
    put_root(kRsdt, "RSDT", false, kMadt);
    put_rsdp(1, false, kRsdt);

    const AcpiProcessorList list = acpi_read_processors(kRsdp, &read_memory);
    CHECK_EQ(list.count, 2u);
    CHECK_EQ(list.apic_id[0], 0u);
    CHECK_EQ(list.apic_id[1], 9u);
}

void bad_checksum_gives_empty_list() {
    clear_memory();
    u32 at = open_madt();
    at += put_local_apic(at, 0, 0, 1);
    close_madt(at);
    memory[kMadt + 9] = static_cast<u8>(memory[kMadt + 9] ^ 0xFF);
    put_root(kXsdt, "XSDT", true, kMadt);
    put_rsdp(2, true, kXsdt);

    CHECK_EQ(acpi_read_processors(kRsdp, &read_memory).count, 0u);
}

void truncated_entry_ends_the_walk() {
    clear_memory();
    u32 at = open_madt();
    at += put_local_apic(at, 0, 7, 1);
    memory[at] = 0;
    memory[at + 1] = 16;
    close_madt(at + 2);
    put_root(kXsdt, "XSDT", true, kMadt);
    put_rsdp(2, true, kXsdt);

    const AcpiProcessorList list = acpi_read_processors(kRsdp, &read_memory);
    CHECK_EQ(list.count, 1u);
    CHECK_EQ(list.apic_id[0], 7u);
}

void no_madt_gives_empty_list() {
    clear_memory();
    put_header(kMadt, "FACP", 36);
    seal_table(kMadt, 36);
    put_root(kXsdt, "XSDT", true, kMadt);
    put_rsdp(2, true, kXsdt);

    CHECK_EQ(acpi_read_processors(kRsdp, &read_memory).count, 0u);
}

}  // namespace

int main() {
    xsdt_lists_both_entry_kinds();
    rsdt_lists_processors();
    bad_checksum_gives_empty_list();
    truncated_entry_ends_the_walk();
    no_madt_gives_empty_list();
    return check::finish("test_acpi_madt");
}
