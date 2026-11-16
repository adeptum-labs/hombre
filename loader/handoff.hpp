#pragma once

#include "kernel_load.hpp"
#include "uefi.hpp"

namespace hombre {

// Collects the memory map, the GOP framebuffer and the ACPI RSDP into a
// BootInfo, exits boot services, switches to page tables that identity map
// and direct map physical memory and map the kernel window into the higher
// half, and jumps to the kernel entry on a fresh stack with the BootInfo
// pointer in RDI. Every failure is logged and halts the machine.
[[noreturn]] void handoff_to_kernel(EfiHandle image, EfiSystemTable* st,
                                    const LoadedKernel& kernel);

}
