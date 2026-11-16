#include "../common/serial.hpp"

namespace hombre {
struct BootInfo;
}

extern "C" [[noreturn]] void kmain(hombre::BootInfo*) {
    hombre::serial::write("kernel stub\n");
    for (;;) {
        asm volatile("hlt");
    }
}
