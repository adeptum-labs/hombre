#include "../common/serial.hpp"
#include "uefi.hpp"

using namespace hombre;

extern "C" EfiStatus efi_main(EfiHandle, EfiSystemTable* system_table) {
    serial::init();
    serial::write("Hombre loader\n");
    system_table->con_out->output_string(system_table->con_out, u"Hombre loader\r\n");
    for (;;) {
        asm volatile("hlt");
    }
}
