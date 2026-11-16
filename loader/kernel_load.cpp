#include "kernel_load.hpp"

#include "../common/boot_info.hpp"
#include "../common/elf.hpp"
#include "../common/serial.hpp"
#include "error_line.hpp"

namespace hombre {

namespace {

EfiStatus fail(EfiSystemTable* st, EfiStatus status, const char* text) {
    ErrorLine(text).emit(st);
    return status;
}

EfiStatus fail_status(EfiSystemTable* st, EfiStatus status, const char* text) {
    ErrorLine(text).append(" status=").append_hex(status).emit(st);
    return status;
}

// Reads the whole file into a pool buffer. On success *buffer must be freed
// with free_pool.
EfiStatus read_kernel_file(EfiHandle image, EfiSystemTable* st, u8*& buffer, u64& size) {
    EfiBootServices* bs = st->boot_services;

    EfiLoadedImage* loaded_image = nullptr;
    EfiStatus status = bs->handle_protocol(image, &efi_loaded_image_guid,
                                           reinterpret_cast<void**>(&loaded_image));
    if (status != efi_success) {
        return fail_status(st, status, "loaded image protocol unavailable");
    }

    EfiSimpleFileSystem* volume = nullptr;
    status = bs->handle_protocol(loaded_image->device_handle, &efi_simple_file_system_guid,
                                 reinterpret_cast<void**>(&volume));
    if (status != efi_success) {
        return fail_status(st, status, "boot volume has no file system");
    }

    EfiFileProtocol* root = nullptr;
    status = volume->open_volume(volume, &root);
    if (status != efi_success) {
        return fail_status(st, status, "cannot open boot volume");
    }

    EfiFileProtocol* file = nullptr;
    status = root->open(root, &file, u"\\KERNEL.ELF", efi_file_mode_read, 0);
    root->close(root);
    if (status == efi_not_found) {
        return fail(st, status, "KERNEL.ELF not found");
    }
    if (status != efi_success) {
        return fail_status(st, status, "cannot open KERNEL.ELF");
    }

    u64 file_size = 0;
    status = file->set_position(file, efi_file_end_position);
    if (status == efi_success) {
        status = file->get_position(file, &file_size);
    }
    if (status == efi_success) {
        status = file->set_position(file, 0);
    }
    if (status != efi_success) {
        file->close(file);
        return fail_status(st, status, "cannot size KERNEL.ELF");
    }

    void* memory = nullptr;
    status = bs->allocate_pool(EfiMemoryType::LoaderData, file_size, &memory);
    if (status != efi_success) {
        file->close(file);
        return fail_status(st, status, "cannot allocate buffer for KERNEL.ELF");
    }

    usize read_size = file_size;
    status = file->read(file, &read_size, memory);
    file->close(file);
    if (status != efi_success || read_size != file_size) {
        bs->free_pool(memory);
        return fail_status(st, status != efi_success ? status : efi_load_error,
                           "cannot read KERNEL.ELF");
    }

    buffer = static_cast<u8*>(memory);
    size = file_size;
    return efi_success;
}

// The page-aligned range [start, end) covering [address, address + length),
// computed without wrapping. Fails when the range does not fit below the top
// of the address space.
bool page_range(u64 address, u64 length, u64& start, u64& end) {
    constexpr u64 page_mask = efi_page_size - 1;
    if (length > ~address) {
        return false;
    }
    const u64 exact_end = address + length;
    if (exact_end > ~page_mask) {
        return false;
    }
    start = address & ~page_mask;
    end = (exact_end + page_mask) & ~page_mask;
    return true;
}

bool entry_in_segments(const ElfImage& image) {
    for (u32 i = 0; i < image.segment_count; ++i) {
        const Segment& seg = image.segments[i];
        // vaddr <= entry first, so the subtraction cannot wrap.
        if (image.entry >= seg.vaddr && image.entry - seg.vaddr < seg.mem_size) {
            return true;
        }
    }
    return false;
}

}

EfiStatus load_kernel(EfiHandle image, EfiSystemTable* st, LoadedKernel& out) {
    EfiBootServices* bs = st->boot_services;

    u8* file = nullptr;
    u64 file_size = 0;
    EfiStatus status = read_kernel_file(image, st, file, file_size);
    if (status != efi_success) {
        return status;
    }

    ElfImage elf{};
    const ElfError error = parse_elf(file, file_size, elf);
    if (error != ElfError::Ok) {
        bs->free_pool(file);
        ErrorLine("KERNEL.ELF invalid: ").append(to_string(error)).emit(st);
        return efi_load_error;
    }
    if (elf.segment_count == 0) {
        bs->free_pool(file);
        return fail(st, efi_load_error, "KERNEL.ELF invalid: no loadable segments");
    }
    if (!entry_in_segments(elf)) {
        bs->free_pool(file);
        return fail(st, efi_load_error, "KERNEL.ELF invalid: entry outside loaded segments");
    }

    // Check every range before claiming anything. The kernel is only reachable
    // through the mapping of kKernelBase + x to x for the first
    // kKernelWindowSize bytes, so every segment must sit inside it.
    for (u32 i = 0; i < elf.segment_count; ++i) {
        const Segment& seg = elf.segments[i];
        u64 start = 0;
        u64 end = 0;
        if (!page_range(seg.paddr, seg.mem_size, start, end)) {
            bs->free_pool(file);
            return fail(st, efi_load_error, "KERNEL.ELF invalid: segment address overflow");
        }
        if (seg.vaddr - seg.paddr != kKernelBase) {
            bs->free_pool(file);
            return fail(st, efi_load_error,
                        "KERNEL.ELF invalid: segment not at the kernel base offset");
        }
        if (end > kKernelWindowSize) {
            bs->free_pool(file);
            return fail(st, efi_load_error,
                        "KERNEL.ELF invalid: segment beyond the kernel window");
        }
    }

    u64 phys_start = ~0ull;
    u64 phys_end = 0;
    for (u32 i = 0; i < elf.segment_count; ++i) {
        const Segment& seg = elf.segments[i];
        if (seg.mem_size == 0) {
            continue;
        }

        u64 start = 0;
        u64 end = 0;
        page_range(seg.paddr, seg.mem_size, start, end);

        u64 claimed = start;
        status = bs->allocate_pages(EfiAllocateType::AllocateAddress, EfiMemoryType::LoaderData,
                                    (end - start) / efi_page_size, &claimed);
        if (status != efi_success) {
            bs->free_pool(file);
            ErrorLine("cannot claim memory at ").append_hex(start).append(" status=")
                .append_hex(status).emit(st);
            return status;
        }

        // The pages are identity mapped while boot services run.
        u8* destination = reinterpret_cast<u8*>(seg.paddr);
        __builtin_memcpy(destination, file + seg.file_offset, seg.file_size);
        __builtin_memset(destination + seg.file_size, 0, seg.mem_size - seg.file_size);

        if (start < phys_start) {
            phys_start = start;
        }
        if (end > phys_end) {
            phys_end = end;
        }
    }

    bs->free_pool(file);

    out.entry_virt = elf.entry;
    out.phys_start = phys_start;
    out.phys_end = phys_end;

    serial::write("kernel loaded: entry=0x");
    serial::write_hex(elf.entry);
    serial::write(" segments=");
    serial::write_dec(elf.segment_count);
    serial::write("\n");
    return efi_success;
}

}
