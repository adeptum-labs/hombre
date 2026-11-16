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

// Only the services up to HandleProtocol are declared; the rest of the table
// is kept as placeholders so the offsets of the used entries stay right.
struct EfiBootServices {
    EfiTableHeader header;
    void* raise_tpl;
    void* restore_tpl;
    EfiStatus (*allocate_pages)(EfiAllocateType type, EfiMemoryType memory_type, usize pages,
                                u64* address);
    void* free_pages;
    void* get_memory_map;
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
};

constexpr EfiStatus efi_error_bit = 1ull << 63;

constexpr EfiStatus efi_success = 0;
constexpr EfiStatus efi_load_error = efi_error_bit | 1;
constexpr EfiStatus efi_out_of_resources = efi_error_bit | 9;
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

}
