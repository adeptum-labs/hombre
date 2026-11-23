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
#include "ahci.hpp"

#include <cstddef>
#include <cstring>

using namespace hombre;

namespace {

void struct_layouts() {
    CHECK_EQ(sizeof(AhciCmdHeader), 32u);
    CHECK_EQ(offsetof(AhciCmdHeader, flags), 0u);
    CHECK_EQ(offsetof(AhciCmdHeader, prdt_count), 2u);
    CHECK_EQ(offsetof(AhciCmdHeader, prd_bytes), 4u);
    CHECK_EQ(offsetof(AhciCmdHeader, table_low), 8u);
    CHECK_EQ(offsetof(AhciCmdHeader, table_high), 12u);
    CHECK_EQ(sizeof(AhciPrdtEntry), 16u);
    CHECK_EQ(offsetof(AhciPrdtEntry, base_low), 0u);
    CHECK_EQ(offsetof(AhciPrdtEntry, base_high), 4u);
    CHECK_EQ(offsetof(AhciPrdtEntry, reserved), 8u);
    CHECK_EQ(offsetof(AhciPrdtEntry, byte_count), 12u);
    CHECK_EQ(kAhciPageBytes, 4096u);
    // The FIS area and command table sit whole and aligned in the page.
    CHECK(kAhciFisOffset + kAhciFisReceiveBytes <= kAhciCmdTableOffset);
    CHECK(kAhciCmdTableOffset + kAhciCmdTableBytes <= kAhciPageBytes);
    CHECK_EQ(kAhciFisOffset % 256u, 0u);
    CHECK_EQ(kAhciCmdTableOffset % 128u, 0u);
}

void identify_fis() {
    u8 fis[64];
    std::memset(fis, 0xAB, sizeof(fis));
    CHECK_EQ(ahci_build_h2d_fis(fis, kAhciCmdIdentify, 0, 0), 5u);
    CHECK_EQ(fis[0], kAhciFisTypeH2d);
    CHECK_EQ(fis[1], kAhciFisC);
    CHECK_EQ(fis[2], kAhciCmdIdentify);
    CHECK_EQ(fis[3], 0);
    CHECK_EQ(fis[4], 0);
    CHECK_EQ(fis[5], 0);
    CHECK_EQ(fis[6], 0);
    CHECK_EQ(fis[7], kAhciFisDeviceLba);
    CHECK_EQ(fis[8], 0);
    CHECK_EQ(fis[9], 0);
    CHECK_EQ(fis[10], 0);
    CHECK_EQ(fis[11], 0);
    CHECK_EQ(fis[12], 0);
    CHECK_EQ(fis[13], 0);
    CHECK_EQ(fis[14], 0);
    CHECK_EQ(fis[15], 0);
    CHECK_EQ(fis[16], 0);
    CHECK_EQ(fis[17], 0);
    CHECK_EQ(fis[18], 0);
    CHECK_EQ(fis[19], 0);
    // Only the FIS itself is written.
    CHECK_EQ(fis[20], 0xAB);
}

void read_fis_carries_lba48() {
    u8 fis[32];
    std::memset(fis, 0xAB, sizeof(fis));
    CHECK_EQ(ahci_build_h2d_fis(fis, kAhciCmdReadDmaExt, 0x0123456789ABull, 1), 5u);
    CHECK_EQ(fis[0], 0x27);
    CHECK_EQ(fis[1], 0x80);
    CHECK_EQ(fis[2], 0x25);
    CHECK_EQ(fis[4], 0xAB);
    CHECK_EQ(fis[5], 0x89);
    CHECK_EQ(fis[6], 0x67);
    CHECK_EQ(fis[7], 0x40);
    CHECK_EQ(fis[8], 0x45);
    CHECK_EQ(fis[9], 0x23);
    CHECK_EQ(fis[10], 0x01);
    CHECK_EQ(fis[12], 1);
    CHECK_EQ(fis[13], 0);
    CHECK_EQ(fis[20], 0xAB);
}

void write_fis_carries_count() {
    u8 fis[32];
    std::memset(fis, 0, sizeof(fis));
    ahci_build_h2d_fis(fis, kAhciCmdWriteDmaExt, 0, 0x1234);
    CHECK_EQ(fis[2], kAhciCmdWriteDmaExt);
    CHECK_EQ(fis[12], 0x34);
    CHECK_EQ(fis[13], 0x12);
}

void command_headers() {
    const AhciCmdHeader read = ahci_build_cmd_header(0x123456789ABull, false, 1);
    CHECK_EQ(read.flags, kAhciFisDwords);
    CHECK_EQ(read.prdt_count, 1);
    CHECK_EQ(read.prd_bytes, 0u);
    CHECK_EQ(read.table_low, 0x456789ABu);
    CHECK_EQ(read.table_high, 0x123u);
    for (u32 i = 0; i < 4; ++i) {
        CHECK_EQ(read.reserved[i], 0u);
    }

    const AhciCmdHeader write = ahci_build_cmd_header(0x1000, true, 0);
    CHECK_EQ(write.flags, kAhciFisDwords | kAhciCmdHeaderWrite);
    CHECK_EQ(write.prdt_count, 0);
    CHECK_EQ(write.table_low, 0x1000u);
    CHECK_EQ(write.table_high, 0u);
}

void prdt_building() {
    AhciPrdtEntry prdt[2];
    std::memset(prdt, 0xAB, sizeof(prdt));
    CHECK_EQ(ahci_build_prdt(prdt, 0x123456789000ull, 4096), 1u);
    CHECK_EQ(prdt[0].base_low, 0x56789000u);
    CHECK_EQ(prdt[0].base_high, 0x1234u);
    CHECK_EQ(prdt[0].reserved, 0u);
    CHECK_EQ(prdt[0].byte_count, 4095u);
    CHECK_EQ(prdt[1].base_low, 0xABABABABu);

    CHECK_EQ(ahci_build_prdt(prdt, 0x2000, 1), 1u);
    CHECK_EQ(prdt[0].byte_count, 0u);
    CHECK_EQ(prdt[0].base_low, 0x2000u);

    // A flush carries no data and gets no descriptor.
    CHECK_EQ(ahci_build_prdt(prdt, 0x3000, 0), 0u);
    CHECK_EQ(prdt[0].base_low, 0x2000u);
}

void signature_classification() {
    CHECK(ahci_classify_signature(0x00000101u) == AhciSignature::Ata);
    CHECK(ahci_classify_signature(0xEB140101u) == AhciSignature::Atapi);
    CHECK(ahci_classify_signature(0x96690101u) == AhciSignature::PortMultiplier);
    CHECK(ahci_classify_signature(0xC33C0101u) == AhciSignature::Enclosure);
    CHECK(ahci_classify_signature(0) == AhciSignature::Other);
    CHECK(ahci_classify_signature(0xFFFFFFFFu) == AhciSignature::Other);
}

void put_model(u16* words, const char* text) {
    char model[40];
    std::memset(model, ' ', sizeof(model));
    const usize length = std::strlen(text);
    std::memcpy(model, text, length < sizeof(model) ? length : sizeof(model));
    for (u32 i = 0; i < 20; ++i) {
        words[27 + i] = static_cast<u16>((static_cast<u8>(model[i * 2]) << 8) |
                                         static_cast<u8>(model[i * 2 + 1]));
    }
}

void identify_with_4k_sectors() {
    u16 words[256];
    std::memset(words, 0, sizeof(words));
    put_model(words, "Hombre Test Disk");
    words[83] = (1u << 10) | (1u << 13);
    words[100] = 0x89AB;
    words[101] = 0x4567;
    words[102] = 0x0123;
    words[103] = 0;
    words[106] = 0x4000 | 0x1000;
    words[117] = 2048;  // 4096 bytes
    words[118] = 0;

    AhciIdentify info{};
    CHECK(ahci_parse_identify(words, info));
    CHECK_EQ(info.sectors, 0x0123456789ABull);
    CHECK_EQ(info.sector_bytes, 4096u);
    CHECK(info.flush);
    CHECK_EQ(std::strcmp(info.model, "Hombre Test Disk"), 0);
    CHECK_EQ(ahci_disk_sectors(info.sectors, info.sector_bytes), info.sectors * 8);
}

void identify_falls_back_to_lba28() {
    u16 words[256];
    std::memset(words, 0, sizeof(words));
    words[60] = 0x1234;
    words[61] = 0x5678;

    AhciIdentify info{};
    CHECK(ahci_parse_identify(words, info));
    CHECK_EQ(info.sectors, 0x56781234ull);
    CHECK_EQ(info.sector_bytes, 512u);
    CHECK(!info.flush);
    CHECK_EQ(info.model[0], '\0');
}

void identify_rejects_unsupported_sector_size() {
    u16 words[256];
    std::memset(words, 0, sizeof(words));
    words[83] = 1u << 10;
    words[100] = 1;
    words[106] = 0x4000 | 0x1000;
    words[117] = 3;  // 6-byte sectors

    AhciIdentify info{};
    CHECK(!ahci_parse_identify(words, info));
    CHECK_EQ(info.sectors, 1u);
    CHECK_EQ(info.sector_bytes, 6u);

    // Bit 15 says the word count is not valid, so the size is 512.
    words[106] = 0x8000 | 0x4000 | 0x1000;
    words[117] = 2048;
    CHECK(ahci_parse_identify(words, info));
    CHECK_EQ(info.sector_bytes, 512u);

    // Bits 14 and 12 without both set is likewise 512.
    words[106] = 0x4000;
    CHECK(ahci_parse_identify(words, info));
    CHECK_EQ(info.sector_bytes, 512u);
}

void identify_rejects_empty_drive() {
    u16 words[256];
    std::memset(words, 0, sizeof(words));
    words[83] = (1u << 10) | (1u << 13);

    AhciIdentify info{};
    CHECK(!ahci_parse_identify(words, info));
    CHECK_EQ(info.sectors, 0u);
    CHECK_EQ(info.sector_bytes, 512u);
}

void sector_sizes() {
    CHECK(ahci_sector_size_ok(512));
    CHECK(ahci_sector_size_ok(1024));
    CHECK(ahci_sector_size_ok(1536));
    CHECK(ahci_sector_size_ok(2048));
    CHECK(ahci_sector_size_ok(4096));
    CHECK(!ahci_sector_size_ok(0));
    CHECK(!ahci_sector_size_ok(256));
    CHECK(!ahci_sector_size_ok(513));
    CHECK(!ahci_sector_size_ok(4608));
    CHECK(!ahci_sector_size_ok(8192));

    CHECK_EQ(ahci_disk_sectors(100, 512), 100u);
    CHECK_EQ(ahci_disk_sectors(100, 2048), 400u);
    CHECK_EQ(ahci_disk_sectors(100, 4096), 800u);
}

void sector_placement() {
    const AhciSectorPlace plain = ahci_sector_place(123, 512);
    CHECK_EQ(plain.logical, 123u);
    CHECK_EQ(plain.offset, 0u);

    const AhciSectorPlace first = ahci_sector_place(0, 4096);
    CHECK_EQ(first.logical, 0u);
    CHECK_EQ(first.offset, 0u);

    const AhciSectorPlace last = ahci_sector_place(7, 4096);
    CHECK_EQ(last.logical, 0u);
    CHECK_EQ(last.offset, 7u * 512u);

    const AhciSectorPlace next = ahci_sector_place(8, 4096);
    CHECK_EQ(next.logical, 1u);
    CHECK_EQ(next.offset, 0u);

    const AhciSectorPlace middle = ahci_sector_place(9, 2048);
    CHECK_EQ(middle.logical, 2u);
    CHECK_EQ(middle.offset, 512u);

    const AhciSectorPlace odd = ahci_sector_place(17, 1536);
    CHECK_EQ(odd.logical, 5u);
    CHECK_EQ(odd.offset, 2u * 512u);

    const AhciSectorPlace top = ahci_sector_place(0xFFFFFFFFull, 4096);
    CHECK_EQ(top.logical, 0x1FFFFFFFull);
    CHECK_EQ(top.offset, 7u * 512u);
}

}

int main() {
    struct_layouts();
    identify_fis();
    read_fis_carries_lba48();
    write_fis_carries_count();
    command_headers();
    prdt_building();
    signature_classification();
    identify_with_4k_sectors();
    identify_falls_back_to_lba28();
    identify_rejects_unsupported_sector_size();
    identify_rejects_empty_drive();
    sector_sizes();
    sector_placement();
    return check::finish("test_ahci");
}
