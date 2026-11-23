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
#include "nvme.hpp"

#include <cstddef>
#include <cstring>

using namespace hombre;

namespace {

void struct_layouts() {
    CHECK_EQ(sizeof(NvmeCommand), 64u);
    CHECK_EQ(offsetof(NvmeCommand, cdw0), 0u);
    CHECK_EQ(offsetof(NvmeCommand, nsid), 4u);
    CHECK_EQ(offsetof(NvmeCommand, cdw2), 8u);
    CHECK_EQ(offsetof(NvmeCommand, cdw3), 12u);
    CHECK_EQ(offsetof(NvmeCommand, mptr), 16u);
    CHECK_EQ(offsetof(NvmeCommand, prp1), 24u);
    CHECK_EQ(offsetof(NvmeCommand, prp2), 32u);
    CHECK_EQ(offsetof(NvmeCommand, cdw10), 40u);
    CHECK_EQ(offsetof(NvmeCommand, cdw11), 44u);
    CHECK_EQ(offsetof(NvmeCommand, cdw12), 48u);
    CHECK_EQ(offsetof(NvmeCommand, cdw13), 52u);
    CHECK_EQ(offsetof(NvmeCommand, cdw14), 56u);
    CHECK_EQ(offsetof(NvmeCommand, cdw15), 60u);

    CHECK_EQ(sizeof(NvmeCompletion), 16u);
    CHECK_EQ(offsetof(NvmeCompletion, result), 0u);
    CHECK_EQ(offsetof(NvmeCompletion, reserved), 4u);
    CHECK_EQ(offsetof(NvmeCompletion, sq_head), 8u);
    CHECK_EQ(offsetof(NvmeCompletion, status), 12u);

    // Both queues fit in one page at every depth the driver uses.
    CHECK_EQ(kNvmeAdminDepth * kNvmeCommandBytes, 1024u);
    CHECK_EQ(kNvmeAdminDepth * kNvmeCompletionBytes, 256u);
    CHECK(kNvmeAdminDepth * kNvmeCommandBytes <= kNvmePageBytes);
    CHECK(kNvmeIoDepthLimit * kNvmeCommandBytes <= kNvmePageBytes);
    CHECK(kNvmeNamespaceFormatOffset + kNvmeNamespaceFormatCount * kNvmeLbaFormatBytes <=
          kNvmePageBytes);
}

void cap_fields() {
    // MQES 0x3FF, DSTRD 4, TO 0x1E and MPSMIN 3.
    const u64 cap = 0x000301E4000003FFull;
    CHECK_EQ(nvme_cap_mqes(cap), 0x3FFu);
    CHECK_EQ(nvme_cap_dstrd(cap), 4u);
    CHECK_EQ(nvme_cap_timeout(cap), 0x1Eu);
    CHECK_EQ(nvme_cap_timeout_ms(cap), 15000u);
    CHECK_EQ(nvme_cap_mpsmin(cap), 3u);
    CHECK(!nvme_cap_supports_4k_pages(cap));

    // MQES 0x7F, DSTRD 0, TO 0x1E and MPSMIN 0.
    const u64 narrow = 0x000001E00000007Full;
    CHECK_EQ(nvme_cap_mqes(narrow), 0x7Fu);
    CHECK_EQ(nvme_cap_dstrd(narrow), 0u);
    CHECK_EQ(nvme_cap_timeout(narrow), 0x1Eu);
    CHECK_EQ(nvme_cap_mpsmin(narrow), 0u);
    CHECK(nvme_cap_supports_4k_pages(narrow));

    // A controller that names no timeout reads back as zero.
    const u64 no_timeout = 0xFFFFull;
    CHECK_EQ(nvme_cap_timeout(no_timeout), 0u);
    CHECK_EQ(nvme_cap_timeout_ms(no_timeout), 0u);
    CHECK(nvme_cap_supports_4k_pages(no_timeout));
}

void doorbell_offsets() {
    // DSTRD 0: neighbouring doorbells are one dword apart.
    CHECK_EQ(nvme_doorbell_offset(0, 0, false), 0x1000u);
    CHECK_EQ(nvme_doorbell_offset(0, 0, true), 0x1004u);
    CHECK_EQ(nvme_doorbell_offset(0, 1, false), 0x1008u);
    CHECK_EQ(nvme_doorbell_offset(0, 1, true), 0x100Cu);

    // DSTRD 1: eight bytes apart.
    CHECK_EQ(nvme_doorbell_offset(1, 0, false), 0x1000u);
    CHECK_EQ(nvme_doorbell_offset(1, 0, true), 0x1008u);
    CHECK_EQ(nvme_doorbell_offset(1, 1, false), 0x1010u);
    CHECK_EQ(nvme_doorbell_offset(1, 1, true), 0x1018u);

    // DSTRD 4: sixty-four bytes apart.
    CHECK_EQ(nvme_doorbell_offset(4, 0, false), 0x1000u);
    CHECK_EQ(nvme_doorbell_offset(4, 0, true), 0x1040u);
    CHECK_EQ(nvme_doorbell_offset(4, 1, false), 0x1080u);
    CHECK_EQ(nvme_doorbell_offset(4, 1, true), 0x10C0u);

    // DSTRD 15: 128 KiB between doorbells.
    CHECK_EQ(nvme_doorbell_offset(15, 0, false), 0x1000u);
    CHECK_EQ(nvme_doorbell_offset(15, 0, true), 0x21000u);
    CHECK_EQ(nvme_doorbell_offset(15, 1, false), 0x41000u);
    CHECK_EQ(nvme_doorbell_offset(15, 1, true), 0x61000u);
}

void completion_fields() {
    // Command 0x1234 completed with phase 1 and no error.
    const u32 done = 0x00011234u;
    CHECK_EQ(nvme_completion_id(done), 0x1234u);
    CHECK_EQ(nvme_completion_phase(done), 1u);
    CHECK_EQ(nvme_completion_status(done), 0u);

    // Status code 2, status code type 0, phase 1.
    const u32 failed = 0x00051234u;
    CHECK_EQ(nvme_completion_id(failed), 0x1234u);
    CHECK_EQ(nvme_completion_phase(failed), 1u);
    CHECK_EQ(nvme_completion_status(failed), 2u);

    // Status code 0x81 with status code type 1 is 0x181, phase 0.
    const u32 type = 0x030200AAu;
    CHECK_EQ(nvme_completion_id(type), 0x00AAu);
    CHECK_EQ(nvme_completion_phase(type), 0u);
    CHECK_EQ(nvme_completion_status(type), 0x181u);
}

void identify_commands() {
    const NvmeCommand controller =
        nvme_build_identify(7, kNvmeCnsController, 0, 0x0000000123456000ull);
    CHECK_EQ(controller.cdw0, 0x00070006u);
    CHECK_EQ(controller.nsid, 0u);
    CHECK_EQ(controller.cdw2, 0u);
    CHECK_EQ(controller.cdw3, 0u);
    CHECK_EQ(controller.mptr, 0u);
    CHECK_EQ(controller.prp1, 0x0000000123456000ull);
    CHECK_EQ(controller.prp2, 0u);
    CHECK_EQ(controller.cdw10, 1u);
    CHECK_EQ(controller.cdw11, 0u);
    CHECK_EQ(controller.cdw12, 0u);
    CHECK_EQ(controller.cdw13, 0u);
    CHECK_EQ(controller.cdw14, 0u);
    CHECK_EQ(controller.cdw15, 0u);

    const NvmeCommand ns =
        nvme_build_identify(0xABCD, kNvmeCnsNamespace, 0x01020304, 0x2000);
    CHECK_EQ(ns.cdw0, 0xABCD0006u);
    CHECK_EQ(ns.nsid, 0x01020304u);
    CHECK_EQ(ns.prp1, 0x2000u);
    CHECK_EQ(ns.cdw10, 0u);

    const NvmeCommand list =
        nvme_build_identify(1, kNvmeCnsActiveNamespaces, 0, 0x3000);
    CHECK_EQ(list.cdw0, 0x00010006u);
    CHECK_EQ(list.cdw10, 2u);
}

void queue_commands() {
    const NvmeCommand cq = nvme_build_create_cq(1, 1, 16, 0x3000);
    CHECK_EQ(cq.cdw0, 0x00010005u);
    CHECK_EQ(cq.nsid, 0u);
    CHECK_EQ(cq.prp1, 0x3000u);
    CHECK_EQ(cq.cdw10, 0x000F0001u);
    CHECK_EQ(cq.cdw11, 1u);
    CHECK_EQ(cq.cdw12, 0u);

    const NvmeCommand sq = nvme_build_create_sq(2, 1, 1, 16, 0x4000);
    CHECK_EQ(sq.cdw0, 0x00020001u);
    CHECK_EQ(sq.prp1, 0x4000u);
    CHECK_EQ(sq.cdw10, 0x000F0001u);
    CHECK_EQ(sq.cdw11, 0x00010001u);

    // The admin queues are sixteen entries each.
    CHECK_EQ(nvme_build_aqa(16, 16), 0x000F000Fu);
    CHECK_EQ(nvme_build_aqa(1, 1), 0u);
    CHECK_EQ(nvme_build_aqa(8, 4), 0x00030007u);
    CHECK_EQ(nvme_io_depth(0), 1u);
    CHECK_EQ(nvme_io_depth(15), 16u);
    CHECK_EQ(nvme_io_depth(1023), 16u);
}

void read_write_flush_commands() {
    const NvmeCommand read =
        nvme_build_rw(kNvmeCmdRead, 1, 3, 0x0000000123456789ull, 1, 0x5000);
    CHECK_EQ(read.cdw0, 0x00030002u);
    CHECK_EQ(read.nsid, 1u);
    CHECK_EQ(read.prp1, 0x5000u);
    CHECK_EQ(read.prp2, 0u);
    CHECK_EQ(read.cdw10, 0x23456789u);
    CHECK_EQ(read.cdw11, 0x00000001u);
    CHECK_EQ(read.cdw12, 0u);
    CHECK_EQ(read.cdw13, 0u);

    const NvmeCommand write =
        nvme_build_rw(kNvmeCmdWrite, 2, 4, 0x000000123456789Aull, 3, 0x6000);
    CHECK_EQ(write.cdw0, 0x00040001u);
    CHECK_EQ(write.nsid, 2u);
    CHECK_EQ(write.prp1, 0x6000u);
    CHECK_EQ(write.cdw10, 0x3456789Au);
    CHECK_EQ(write.cdw11, 0x00000012u);
    CHECK_EQ(write.cdw12, 2u);

    const NvmeCommand flush = nvme_build_flush(0x01020304, 5);
    CHECK_EQ(flush.cdw0, 0x00050000u);
    CHECK_EQ(flush.nsid, 0x01020304u);
    CHECK_EQ(flush.prp1, 0u);
    CHECK_EQ(flush.cdw10, 0u);
    CHECK_EQ(flush.cdw11, 0u);
    CHECK_EQ(flush.cdw12, 0u);
}

void fill_format(u8* data, u32 index, u32 metadata, u32 lbads) {
    u8* format = data + kNvmeNamespaceFormatOffset + index * kNvmeLbaFormatBytes;
    format[0] = static_cast<u8>(metadata);
    format[1] = static_cast<u8>(metadata >> 8);
    format[2] = static_cast<u8>(lbads);
    format[3] = 0;
}

void fill_sectors(u8* data, u64 sectors) {
    for (u32 i = 0; i < 8; ++i) {
        data[i] = static_cast<u8>(sectors >> (8 * i));
    }
}

void namespace_parsing() {
    u8 data[kNvmePageBytes];

    // A namespace with a 512-byte format.
    std::memset(data, 0, sizeof(data));
    fill_sectors(data, 0x1234);
    data[kNvmeNamespaceFlbasOffset] = 0;
    fill_format(data, 0, 0, 9);
    NvmeNamespaceInfo info = nvme_parse_namespace(data);
    CHECK(info.usable());
    CHECK_EQ(info.sectors, 0x1234u);
    CHECK_EQ(info.sector_bytes, 512u);
    CHECK(nvme_sector_size_ok(info.sector_bytes));

    // Format index 2 is the active one and holds 4096-byte blocks.
    std::memset(data, 0, sizeof(data));
    fill_sectors(data, 65536);
    data[kNvmeNamespaceFlbasOffset] = 2;
    fill_format(data, 2, 0, 12);
    info = nvme_parse_namespace(data);
    CHECK(info.usable());
    CHECK_EQ(info.sectors, 65536u);
    CHECK_EQ(info.sector_bytes, 4096u);
    CHECK_EQ(nvme_disk_sectors(info.sectors, info.sector_bytes), 524288u);

    // The low nibble of FLBAS picks the format.
    data[kNvmeNamespaceFlbasOffset] = 0x12;
    info = nvme_parse_namespace(data);
    CHECK(info.usable());
    CHECK_EQ(info.sector_bytes, 4096u);

    // A format with metadata cannot be served.
    std::memset(data, 0, sizeof(data));
    fill_sectors(data, 10);
    data[kNvmeNamespaceFlbasOffset] = 1;
    fill_format(data, 1, 8, 9);
    info = nvme_parse_namespace(data);
    CHECK(!info.usable());
    CHECK(info.problem == NvmeNamespaceProblem::Metadata);
    CHECK_EQ(info.sector_bytes, 512u);

    // Metadata occupies both bytes of the field.
    fill_format(data, 1, 0x0100, 9);
    info = nvme_parse_namespace(data);
    CHECK(info.problem == NvmeNamespaceProblem::Metadata);

    // 8192-byte blocks do not fit a page.
    std::memset(data, 0, sizeof(data));
    fill_sectors(data, 10);
    data[kNvmeNamespaceFlbasOffset] = 0;
    fill_format(data, 0, 0, 13);
    info = nvme_parse_namespace(data);
    CHECK(!info.usable());
    CHECK(info.problem == NvmeNamespaceProblem::SectorSize);
    CHECK_EQ(info.sector_bytes, 8192u);

    // 256-byte blocks are below the driver's floor.
    fill_format(data, 0, 0, 8);
    info = nvme_parse_namespace(data);
    CHECK(info.problem == NvmeNamespaceProblem::SectorSize);
    CHECK_EQ(info.sector_bytes, 256u);

    // An empty namespace is skipped before its format is looked at.
    std::memset(data, 0, sizeof(data));
    data[kNvmeNamespaceFlbasOffset] = 1;
    fill_format(data, 1, 8, 9);
    info = nvme_parse_namespace(data);
    CHECK(!info.usable());
    CHECK(info.problem == NvmeNamespaceProblem::NoSectors);

    // Bytes read little endian.
    const u8 bytes[8] = {0x78, 0x56, 0x34, 0x12, 0xF0, 0xDE, 0xBC, 0x9A};
    CHECK_EQ(nvme_read_u32(bytes, 0), 0x12345678u);
    CHECK_EQ(nvme_read_u32(bytes, 4), 0x9ABCDEF0u);
    CHECK_EQ(nvme_read_u64(bytes, 0), 0x9ABCDEF012345678ull);
}

void model_and_cache() {
    u8 identify[kNvmePageBytes];
    std::memset(identify, 0, sizeof(identify));
    std::memset(identify + kNvmeControllerModelOffset, ' ', kNvmeControllerModelBytes);
    std::memcpy(identify + kNvmeControllerModelOffset, "Hombre NVMe", 11);

    char model[kNvmeControllerModelBytes + 1];
    CHECK_EQ(nvme_model_string(identify, model, sizeof(model)), 11u);
    CHECK_EQ(std::strcmp(model, "Hombre NVMe"), 0);

    // A full forty-byte model keeps every character.
    std::memset(identify + kNvmeControllerModelOffset, 'A', kNvmeControllerModelBytes);
    CHECK_EQ(nvme_model_string(identify, model, sizeof(model)), 40u);
    CHECK_EQ(model[40], '\0');
    CHECK_EQ(model[39], 'A');

    // A short buffer truncates and still terminates.
    char small[5];
    std::memset(identify + kNvmeControllerModelOffset, ' ', kNvmeControllerModelBytes);
    std::memcpy(identify + kNvmeControllerModelOffset, "Hombre", 6);
    CHECK_EQ(nvme_model_string(identify, small, sizeof(small)), 4u);
    CHECK_EQ(std::strcmp(small, "Homb"), 0);

    // A NUL in the field ends the model.
    std::memset(identify + kNvmeControllerModelOffset, ' ', kNvmeControllerModelBytes);
    identify[kNvmeControllerModelOffset] = 'a';
    identify[kNvmeControllerModelOffset + 1] = 'b';
    identify[kNvmeControllerModelOffset + 2] = 'c';
    identify[kNvmeControllerModelOffset + 3] = '\0';
    identify[kNvmeControllerModelOffset + 4] = 'd';
    CHECK_EQ(nvme_model_string(identify, model, sizeof(model)), 3u);
    CHECK_EQ(std::strcmp(model, "abc"), 0);

    identify[kNvmeControllerVwcOffset] = 0;
    CHECK(!nvme_write_cache_present(identify));
    identify[kNvmeControllerVwcOffset] = 1;
    CHECK(nvme_write_cache_present(identify));
    identify[kNvmeControllerVwcOffset] = 3;
    CHECK(nvme_write_cache_present(identify));
    identify[kNvmeControllerVwcOffset] = 2;
    CHECK(!nvme_write_cache_present(identify));
}

void sector_sizes() {
    CHECK(nvme_sector_size_ok(512));
    CHECK(nvme_sector_size_ok(1024));
    CHECK(nvme_sector_size_ok(1536));
    CHECK(nvme_sector_size_ok(2048));
    CHECK(nvme_sector_size_ok(4096));
    CHECK(!nvme_sector_size_ok(0));
    CHECK(!nvme_sector_size_ok(256));
    CHECK(!nvme_sector_size_ok(513));
    CHECK(!nvme_sector_size_ok(4608));
    CHECK(!nvme_sector_size_ok(8192));

    CHECK_EQ(nvme_disk_sectors(100, 512), 100u);
    CHECK_EQ(nvme_disk_sectors(100, 2048), 400u);
    CHECK_EQ(nvme_disk_sectors(100, 4096), 800u);
}

void sector_placement() {
    const NvmeSectorPlace plain = nvme_sector_place(123, 512);
    CHECK_EQ(plain.logical, 123u);
    CHECK_EQ(plain.offset, 0u);

    const NvmeSectorPlace first = nvme_sector_place(0, 4096);
    CHECK_EQ(first.logical, 0u);
    CHECK_EQ(first.offset, 0u);

    const NvmeSectorPlace last = nvme_sector_place(7, 4096);
    CHECK_EQ(last.logical, 0u);
    CHECK_EQ(last.offset, 7u * 512u);

    const NvmeSectorPlace next = nvme_sector_place(8, 4096);
    CHECK_EQ(next.logical, 1u);
    CHECK_EQ(next.offset, 0u);

    const NvmeSectorPlace middle = nvme_sector_place(9, 2048);
    CHECK_EQ(middle.logical, 2u);
    CHECK_EQ(middle.offset, 512u);

    const NvmeSectorPlace odd = nvme_sector_place(17, 1536);
    CHECK_EQ(odd.logical, 5u);
    CHECK_EQ(odd.offset, 2u * 512u);

    const NvmeSectorPlace top = nvme_sector_place(0xFFFFFFFFull, 4096);
    CHECK_EQ(top.logical, 0x1FFFFFFFull);
    CHECK_EQ(top.offset, 7u * 512u);
}

}

int main() {
    struct_layouts();
    cap_fields();
    doorbell_offsets();
    completion_fields();
    identify_commands();
    queue_commands();
    read_write_flush_commands();
    namespace_parsing();
    model_and_cache();
    sector_sizes();
    sector_placement();
    return check::finish("test_nvme");
}
