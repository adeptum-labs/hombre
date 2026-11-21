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

#pragma once

#include "../common/types.hpp"

namespace hombre {

class Framebuffer;

// The boot steps reported before the Startup-Sequence's own lines.
inline constexpr u32 kSplashKernelSteps = 10;

// Parses the embedded image and paints it, with every progress block empty.
// False when the image is missing or malformed, leaving the screen as it is.
bool splash_show(Framebuffer& fb);

// Fills done/total of the progress blocks, repainting only those that change.
void splash_progress(u32 done, u32 total);

// Marks the splash finished; later progress calls draw nothing.
void splash_end();

// Whether the splash still owns the screen.
bool splash_active();

}
