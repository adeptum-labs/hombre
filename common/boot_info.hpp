#pragma once

#include "types.hpp"

namespace hombre {

// Virtual address layout shared by the loader and the kernel. All physical
// memory is mapped at kDirectMapBase + phys, and the first 64 MiB of physical
// memory, which holds the kernel image, at kKernelBase + phys.
inline constexpr u64 kDirectMapBase = 0xFFFF800000000000ull;
inline constexpr u64 kKernelBase = 0xFFFFFFFF80000000ull;
inline constexpr u64 kKernelWindowSize = 64ull << 20;

enum class MemType : u32 {
    Usable = 1,
    Reserved,
    AcpiReclaimable,
    AcpiNvs,
    Mmio,
    // Memory the loader handed over in use: the kernel image, this
    // structure, the region array, the kernel stack and the page tables.
    Loader,
};

struct MemoryRegion {
    u64 phys;
    u64 pages;  // 4 KiB pages
    MemType type;
    u32 reserved;
};

// format: 0 = BGRX8 (blue in the lowest byte), 1 = RGBX8.
// phys == 0 means there is no framebuffer.
struct FramebufferInfo {
    u64 phys;
    u32 width, height, pitch_bytes;
    u32 format;
};

inline constexpr u32 kFramebufferBgrx8 = 0;
inline constexpr u32 kFramebufferRgbx8 = 1;

inline constexpr u64 boot_info_magic = 0x484F4D4252450001ull;
inline constexpr u32 boot_info_version = 1;

// What the loader passes to kmain. Every pointer in it is a virtual address
// valid under the kernel's page tables; rsdp_phys == 0 means no ACPI RSDP.
struct BootInfo {
    u64 magic;
    u32 version;
    u32 region_count;
    MemoryRegion* regions;
    FramebufferInfo fb;
    u64 rsdp_phys;
    u64 direct_map_base;
    u64 kernel_phys_start;
    u64 kernel_phys_end;
};

static_assert(sizeof(MemoryRegion) == 24);
static_assert(__builtin_offsetof(MemoryRegion, type) == 16);
static_assert(sizeof(FramebufferInfo) == 24);
static_assert(sizeof(BootInfo) == 80);
static_assert(__builtin_offsetof(BootInfo, regions) == 16);
static_assert(__builtin_offsetof(BootInfo, fb) == 24);
static_assert(__builtin_offsetof(BootInfo, rsdp_phys) == 48);
static_assert(__builtin_offsetof(BootInfo, kernel_phys_end) == 72);

}
