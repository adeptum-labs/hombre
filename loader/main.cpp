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

#include "../common/serial.hpp"
#include "handoff.hpp"
#include "kernel_load.hpp"
#include "uefi.hpp"

using namespace hombre;

extern "C" EfiStatus efi_main(EfiHandle image, EfiSystemTable* system_table) {
    serial::init();
    serial::write("Hombre loader\n");
    system_table->con_out->output_string(system_table->con_out, u"Hombre loader\r\n");

    LoadedKernel kernel{};
    if (load_kernel(image, system_table, kernel) == efi_success) {
        handoff_to_kernel(image, system_table, kernel);
    }

    // Keep the firmware's watchdog from resetting the machine and wiping the
    // error message. Firmware without a watchdog fails the call, which is
    // harmless.
    system_table->boot_services->set_watchdog_timer(0, 0, 0, nullptr);
    for (;;) {
        asm volatile("cli; hlt");
    }
}
