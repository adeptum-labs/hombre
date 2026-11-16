#pragma once

#include "../common/types.hpp"
#include "uefi.hpp"

namespace hombre {

struct LoadedKernel {
    u64 entry_virt;
    // Page-rounded physical range claimed for the kernel: [phys_start, phys_end).
    u64 phys_start;
    u64 phys_end;
};

// Reads \KERNEL.ELF from the volume the loader was started from and places
// its PT_LOAD segments at their physical addresses. Every failure is logged
// to the serial port and the firmware console, and its status is returned.
EfiStatus load_kernel(EfiHandle image, EfiSystemTable* st, LoadedKernel& out);

}
