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


#include "../loader/gop_choice.hpp"
#include "check.hpp"

using namespace hombre;

namespace {

void picks_most_pixels() {
    const GopModeSize modes[] = {{1280, 800, true}, {1920, 1080, true}, {1024, 768, true}};
    CHECK_EQ(best_gop_mode(modes, 3, 0), 1u);
}

void tie_prefers_wider() {
    const GopModeSize modes[] = {{1200, 1600, true}, {1600, 1200, true}};
    CHECK_EQ(best_gop_mode(modes, 2, 0), 1u);
}

void skips_unusable_formats() {
    const GopModeSize modes[] = {{1280, 800, true}, {3840, 2160, false}};
    CHECK_EQ(best_gop_mode(modes, 2, 0), 0u);
}

void monitor_shapes_win_over_square_ones() {
    const GopModeSize modes[] = {{2048, 2048, true}, {1920, 1080, true}, {1280, 1024, true}};
    CHECK_EQ(best_gop_mode(modes, 3, 0), 1u);
    // Only odd shapes: the largest of them.
    const GopModeSize odd[] = {{800, 800, true}, {2048, 2048, true}};
    CHECK_EQ(best_gop_mode(odd, 2, 0), 1u);
}

void falls_back_to_current() {
    const GopModeSize modes[] = {{1280, 800, false}, {640, 480, false}};
    CHECK_EQ(best_gop_mode(modes, 2, 1), 1u);
    CHECK_EQ(best_gop_mode(modes, 0, 3), 3u);
}

}

int main() {
    picks_most_pixels();
    tie_prefers_wider();
    skips_unusable_formats();
    monitor_shapes_win_over_square_ones();
    falls_back_to_current();
    return check::finish("test_gop_choice");
}
