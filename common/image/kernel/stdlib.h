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

// The kernel's C code has no C library. Wuffs' allocating helpers need these
// two functions, and the kernel stubs supply them; nothing calls the
// helpers. The build renames both for the Wuffs code (see KCFLAGS), so the
// kernel exports no calloc or free.

#ifndef HOMBRE_IMAGE_KERNEL_STDLIB_H
#define HOMBRE_IMAGE_KERNEL_STDLIB_H

#include <stddef.h>

void* calloc(size_t count, size_t bytes);
void free(void* block);

#endif
