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

struct EfiBootServices {
    EfiTableHeader header;
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

constexpr EfiStatus efi_success = 0;

}
