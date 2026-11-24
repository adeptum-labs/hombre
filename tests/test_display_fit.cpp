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


#include "display_fit.hpp"
#include "check.hpp"

using namespace hombre;

namespace {

void same_size_is_one_to_one() {
    const Fit f = fit_square(1920, 1080, 1920, 1080);
    CHECK_EQ(f.x, 0u);
    CHECK_EQ(f.y, 0u);
    CHECK_EQ(f.width, 1920u);
    CHECK_EQ(f.height, 1080u);
}

void shrink_1440p_on_1080p_keeps_the_shape() {
    const Fit f = fit_square(1920, 1080, 2560, 1440);
    CHECK_EQ(f.width, 1920u);
    CHECK_EQ(f.height, 1080u);
    // 16:10 on 16:9 gets bars at the sides.
    const Fit g = fit_square(1920, 1080, 2560, 1600);
    CHECK_EQ(g.height, 1080u);
    CHECK_EQ(g.width, 1728u);
    CHECK_EQ(g.x, 96u);
    CHECK_EQ(g.y, 0u);
}

void four_by_three_on_wide_screen_is_pillarboxed() {
    const Fit f = fit_square(1920, 1080, 1024, 768);
    CHECK_EQ(f.height, 1080u);
    CHECK_EQ(f.width, 1440u);
    CHECK_EQ(f.x, 240u);
}

void small_mode_scaled_up_centred() {
    const Fit f = fit_square(2560, 1600, 800, 600);
    CHECK_EQ(f.width, 2133u);
    CHECK_EQ(f.height, 1600u);
    CHECK_EQ(f.x, 213u);
    CHECK_EQ(f.y, 0u);
    const Fit wide = fit_square(1280, 1024, 1280, 720);
    CHECK_EQ(wide.width, 1280u);
    CHECK_EQ(wide.height, 720u);
    CHECK_EQ(wide.y, 152u);
}

}

int main() {
    same_size_is_one_to_one();
    shrink_1440p_on_1080p_keeps_the_shape();
    four_by_three_on_wide_screen_is_pillarboxed();
    small_mode_scaled_up_centred();
    return check::finish("test_display_fit");
}
