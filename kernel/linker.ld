/* Loaded at physical 1 MiB and linked into the higher half. Every section
   starts on its own page so each segment gets its own permissions. */

ENTRY(kmain)

KERNEL_PHYS_BASE = 0x100000;
KERNEL_VIRT_BASE = 0xFFFFFFFF80000000;

SECTIONS
{
    . = KERNEL_VIRT_BASE + KERNEL_PHYS_BASE;

    .text ALIGN(4096) : AT(ADDR(.text) - KERNEL_VIRT_BASE) {
        *(.text .text.*)
    }

    .rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_VIRT_BASE) {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4096) : AT(ADDR(.data) - KERNEL_VIRT_BASE) {
        *(.data .data.*)
    }

    .bss ALIGN(4096) : AT(ADDR(.bss) - KERNEL_VIRT_BASE) {
        *(.bss .bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
