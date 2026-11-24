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


#include "libs/display_modes.hpp"
#include "check.hpp"

#include <cstring>

using namespace hombre;
using namespace hombre::display_modes;

namespace {

constexpr u64 kMiB = 1024 * 1024;
constexpr u32 kAmigaModes = 54;

void amiga_modes_unchanged() {
    configure_hombre(0, 0, 0);
    CHECK_EQ(count(), kAmigaModes);
    CHECK_EQ(at(0).id, 0u);
    CHECK(std::strcmp(at(0).name, "NTSC:Low Res") == 0);
    CHECK(!at(0).deep);
    CHECK(!is_hombre(0x29004));
}

void hombre_modes_listed_after_amiga() {
    configure_hombre(1280, 800, 512 * kMiB);
    CHECK_EQ(count(), kAmigaModes + 45);
    const Mode& first = at(kAmigaModes);
    CHECK_EQ(first.id, kHombreMonitorId);
    CHECK_EQ(first.width, 800u);
    CHECK_EQ(first.height, 600u);
    CHECK_EQ(first.max_depth, 8u);
    CHECK(first.deep);
    CHECK(first.square);
    CHECK(first.pixfmt == PixFmt::Lut8);
    CHECK(std::strcmp(first.name, "HOMBRE:800x600 8bit") == 0);
    CHECK_EQ(first.flags, kIsForeign | kIsWb);

    const u32 id1080 = kHombreMonitorId | (12u << 4) | 2u;
    const Mode* fhd = find(id1080);
    CHECK(fhd != nullptr);
    CHECK_EQ(fhd->width, 1920u);
    CHECK_EQ(fhd->height, 1080u);
    CHECK_EQ(fhd->max_depth, 24u);
    CHECK(fhd->pixfmt == PixFmt::Argb32);
    CHECK(std::strcmp(fhd->name, "HOMBRE:1920x1080 24bit") == 0);
    CHECK(is_hombre(id1080));
    const Mode* hd16 = find(kHombreMonitorId | (9u << 4) | 1u);
    CHECK(hd16 != nullptr);
    CHECK_EQ(hd16->width, 1280u);
    CHECK_EQ(hd16->height, 720u);
    CHECK(hd16->pixfmt == PixFmt::Rgb16);
    CHECK_EQ(hd16->max_depth, 16u);
    configure_hombre(0, 0, 0);
}

void native_mode_added_for_unlisted_size() {
    configure_hombre(1920, 1080, 512 * kMiB);
    CHECK_EQ(count(), kAmigaModes + 45);
    configure_hombre(1360, 768, 512 * kMiB);
    CHECK_EQ(count(), kAmigaModes + 48);
    const Mode* native = find(kHombreMonitorId | (15u << 4) | 2u);
    CHECK(native != nullptr);
    CHECK_EQ(native->width, 1360u);
    CHECK_EQ(native->height, 768u);
    CHECK(std::strcmp(native->name, "HOMBRE:1360x768 24bit") == 0);
    configure_hombre(0, 0, 0);
}

void large_modes_dropped_when_fast_is_small() {
    // A quarter of 64 MiB is 16 MiB: 3840x2160 at 4 bytes is 33 MB.
    configure_hombre(1280, 800, 64 * kMiB);
    CHECK(find(kHombreMonitorId | (14u << 4) | 2u) == nullptr);
    CHECK(find(kHombreMonitorId | (14u << 4) | 0u) != nullptr);
    CHECK(find(kHombreMonitorId | (12u << 4) | 2u) != nullptr);
    configure_hombre(0, 0, 0);
}

}

int main() {
    amiga_modes_unchanged();
    hombre_modes_listed_after_amiga();
    native_mode_added_for_unlisted_size();
    large_modes_dropped_when_fast_is_small();
    return check::finish("test_display_modes");
}
