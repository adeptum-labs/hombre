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

#include "../common/types.hpp"

namespace hombre {

using EfiHandle = void*;
using EfiStatus = usize;

struct EfiTableHeader {
    u64 signature;
    u32 revision;
    u32 header_size;
    u32 crc32;
    u32 reserved;
};

struct EfiSimpleTextOutput {
    void* reset;
    EfiStatus (*output_string)(EfiSimpleTextOutput* self, const char16_t* text);
};

struct EfiGuid {
    u32 data1;
    u16 data2;
    u16 data3;
    u8 data4[8];
};

enum class EfiAllocateType : u32 {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
};

enum class EfiMemoryType : u32 {
    Reserved,
    LoaderCode,
    LoaderData,
};

// Services the loader does not call are kept as placeholders so the offsets
// of the used entries stay right.
struct EfiBootServices {
    EfiTableHeader header;
    void* raise_tpl;
    void* restore_tpl;
    EfiStatus (*allocate_pages)(EfiAllocateType type, EfiMemoryType memory_type, usize pages,
                                u64* address);
    EfiStatus (*free_pages)(u64 address, usize pages);
    EfiStatus (*get_memory_map)(usize* map_size, void* map, usize* map_key,
                                usize* descriptor_size, u32* descriptor_version);
    EfiStatus (*allocate_pool)(EfiMemoryType memory_type, usize size, void** buffer);
    EfiStatus (*free_pool)(void* buffer);
    void* create_event;
    void* set_timer;
    void* wait_for_event;
    void* signal_event;
    void* close_event;
    void* check_event;
    void* install_protocol_interface;
    void* reinstall_protocol_interface;
    void* uninstall_protocol_interface;
    EfiStatus (*handle_protocol)(EfiHandle handle, const EfiGuid* protocol, void** interface);
    void* reserved;
    void* register_protocol_notify;
    void* locate_handle;
    void* locate_device_path;
    void* install_configuration_table;
    void* load_image;
    void* start_image;
    void* exit;
    void* unload_image;
    EfiStatus (*exit_boot_services)(EfiHandle image, usize map_key);
    void* get_next_monotonic_count;
    void* stall;
    EfiStatus (*set_watchdog_timer)(usize timeout, u64 code, usize data_size,
                                    const char16_t* data);
    void* connect_controller;
    void* disconnect_controller;
    void* open_protocol;
    void* close_protocol;
    void* open_protocol_information;
    void* protocols_per_handle;
    void* locate_handle_buffer;
    EfiStatus (*locate_protocol)(const EfiGuid* protocol, void* registration, void** interface);
};

static_assert(__builtin_offsetof(EfiBootServices, allocate_pages) == 40);
static_assert(__builtin_offsetof(EfiBootServices, get_memory_map) == 56);
static_assert(__builtin_offsetof(EfiBootServices, handle_protocol) == 152);
static_assert(__builtin_offsetof(EfiBootServices, exit_boot_services) == 232);
static_assert(__builtin_offsetof(EfiBootServices, set_watchdog_timer) == 256);
static_assert(__builtin_offsetof(EfiBootServices, locate_protocol) == 320);

struct EfiConfigurationTable {
    EfiGuid vendor_guid;
    void* vendor_table;
};

struct EfiSystemTable {
    EfiTableHeader header;
    char16_t* firmware_vendor;
    u32 firmware_revision;
    EfiHandle console_in_handle;
    void* con_in;
    EfiHandle console_out_handle;
    EfiSimpleTextOutput* con_out;
    EfiHandle standard_error_handle;
    void* std_err;
    void* runtime_services;
    EfiBootServices* boot_services;
    usize number_of_table_entries;
    EfiConfigurationTable* configuration_table;
};

static_assert(__builtin_offsetof(EfiSystemTable, boot_services) == 96);
static_assert(__builtin_offsetof(EfiSystemTable, configuration_table) == 112);

constexpr EfiStatus efi_error_bit = 1ull << 63;

constexpr EfiStatus efi_success = 0;
constexpr EfiStatus efi_load_error = efi_error_bit | 1;
constexpr EfiStatus efi_invalid_parameter = efi_error_bit | 2;
constexpr EfiStatus efi_buffer_too_small = efi_error_bit | 5;
constexpr EfiStatus efi_not_found = efi_error_bit | 14;

constexpr usize efi_page_size = 4096;

struct EfiFileProtocol {
    u64 revision;
    EfiStatus (*open)(EfiFileProtocol* self, EfiFileProtocol** file, const char16_t* name,
                      u64 mode, u64 attributes);
    EfiStatus (*close)(EfiFileProtocol* self);
    void* remove;
    EfiStatus (*read)(EfiFileProtocol* self, usize* size, void* buffer);
    void* write;
    EfiStatus (*get_position)(EfiFileProtocol* self, u64* position);
    EfiStatus (*set_position)(EfiFileProtocol* self, u64 position);
};

constexpr u64 efi_file_mode_read = 1;
// SetPosition with this value moves to the end of the file.
constexpr u64 efi_file_end_position = ~0ull;

struct EfiSimpleFileSystem {
    u64 revision;
    EfiStatus (*open_volume)(EfiSimpleFileSystem* self, EfiFileProtocol** root);
};

struct EfiLoadedImage {
    u32 revision;
    EfiHandle parent_handle;
    EfiSystemTable* system_table;
    EfiHandle device_handle;
};

constexpr EfiGuid efi_loaded_image_guid = {
    0x5B1B31A1, 0x9562, 0x11D2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
constexpr EfiGuid efi_simple_file_system_guid = {
    0x964E5B22, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
constexpr EfiGuid efi_graphics_output_guid = {
    0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}};
constexpr EfiGuid efi_acpi_20_table_guid = {
    0x8868E871, 0xE4F1, 0x11D3, {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}};

enum class EfiPixelFormat : u32 {
    RedGreenBlueReserved8,
    BlueGreenRedReserved8,
    BitMask,
    BltOnly,
};

struct EfiGraphicsModeInfo {
    u32 version;
    u32 horizontal_resolution;
    u32 vertical_resolution;
    EfiPixelFormat pixel_format;
    u32 pixel_bitmask[4];
    u32 pixels_per_scan_line;
};

struct EfiGraphicsMode {
    u32 max_mode;
    u32 mode;
    EfiGraphicsModeInfo* info;
    usize size_of_info;
    u64 frame_buffer_base;
    usize frame_buffer_size;
};

struct EfiGraphicsOutput {
    void* query_mode;
    void* set_mode;
    void* blt;
    EfiGraphicsMode* mode;
};

}
