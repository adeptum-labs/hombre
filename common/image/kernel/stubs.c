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

// Wuffs' allocating decoder helpers are compiled but never called here: the
// decoders come from our own allocators. The helpers still reference calloc
// and free, which these stubs satisfy for the kernel link under the names
// the build gives them: calloc fails, and free has nothing to give back.

#include <stdlib.h>

void* calloc(size_t count, size_t bytes) {
    (void)count;
    (void)bytes;
    return 0;
}

void free(void* block) {
    (void)block;
}
