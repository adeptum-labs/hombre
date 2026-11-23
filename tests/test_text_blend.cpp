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

#include "libs/text_blend.hpp"
#include "check.hpp"

#include <initializer_list>

using namespace hombre;
using hombre::text_blend::Cache;
using hombre::text_blend::kLevels;

namespace {

constexpr u32 kGreyCount = 16;

// Sixteen greys from black to white, seventeen apart.
constexpr u8 kGreys[kGreyCount][3] = {
    {0, 0, 0},       {17, 17, 17},   {34, 34, 34},    {51, 51, 51},
    {68, 68, 68},    {85, 85, 85},   {102, 102, 102}, {119, 119, 119},
    {136, 136, 136}, {153, 153, 153}, {170, 170, 170}, {187, 187, 187},
    {204, 204, 204}, {221, 221, 221}, {238, 238, 238}, {255, 255, 255},
};

void cache_without_palette_has_no_ramps() {
    Cache cache;
    CHECK(!cache.has_palette());
    CHECK(cache.ramp(0, 0) == nullptr);
}

void endpoints_are_the_pens_asked_for() {
    Cache cache;
    cache.set_palette(kGreys, kGreyCount);
    CHECK(cache.has_palette());
    const u8 pairs[][2] = {{0, 15}, {15, 0}, {3, 12}, {7, 9}, {12, 3}};
    for (const auto& pair : pairs) {
        const u8* ramp = cache.ramp(pair[0], pair[1]);
        CHECK(ramp != nullptr);
        CHECK_EQ(ramp[0], pair[1]);
        CHECK_EQ(ramp[kLevels - 1], pair[0]);
    }
}

void black_to_white_ramp_rises_through_half_light() {
    Cache cache;
    cache.set_palette(kGreys, kGreyCount);
    const u8* ramp = cache.ramp(kGreyCount - 1, 0);
    CHECK(ramp != nullptr);
    for (u32 level = 1; level < kLevels; ++level) {
        CHECK(ramp[level - 1] <= ramp[level]);
    }
    // Linear light reaches half of white's intensity between levels 7 and 8.
    const u32 before = kGreys[ramp[7]][0];
    const u32 after = kGreys[ramp[8]][0];
    CHECK(before * before < 32513);
    CHECK(after * after > 32512);
}

void full_grey_palette_blends_exactly() {
    u8 palette[256][3];
    for (u32 i = 0; i < 256; ++i) {
        palette[i][0] = static_cast<u8>(i);
        palette[i][1] = static_cast<u8>(i);
        palette[i][2] = static_cast<u8>(i);
    }
    Cache cache;
    cache.set_palette(palette, 256);
    const u8* ramp = cache.ramp(255, 0);
    CHECK(ramp != nullptr);
    CHECK_EQ(ramp[0], 0);
    CHECK_EQ(ramp[7], 174);
    CHECK_EQ(ramp[8], 186);
    CHECK_EQ(ramp[kLevels - 1], 255);
}

void colliding_ramps_are_recomputed() {
    constexpr u32 kCount = 64;
    u8 palette[kCount][3];
    for (u32 i = 0; i < kCount; ++i) {
        palette[i][0] = static_cast<u8>(i * 4);
        palette[i][1] = static_cast<u8>(255 - i * 4);
        palette[i][2] = static_cast<u8>((i * 37) & 0xFF);
    }
    Cache cache;
    cache.set_palette(palette, kCount);
    Cache reference;
    reference.set_palette(palette, kCount);
    // Far more pairs than the table has slots, so most slots are reused.
    for (u32 fg = 0; fg < kCount; ++fg) {
        for (u32 bg = 0; bg < kCount; ++bg) {
            const u8* ramp = cache.ramp(static_cast<u8>(fg), static_cast<u8>(bg));
            reference.invalidate();
            const u8* fresh = reference.ramp(static_cast<u8>(fg), static_cast<u8>(bg));
            CHECK(ramp != nullptr);
            CHECK(fresh != nullptr);
            for (u32 level = 0; level < kLevels; ++level) {
                CHECK_EQ(ramp[level], fresh[level]);
            }
        }
    }
}

void changed_palette_colour_changes_the_ramp() {
    u8 palette[3][3] = {{0, 0, 0}, {255, 255, 255}, {128, 128, 128}};
    Cache cache;
    cache.set_palette(palette, 3);
    const u8* ramp = cache.ramp(1, 0);
    u8 before[kLevels];
    for (u32 level = 0; level < kLevels; ++level) {
        before[level] = ramp[level];
    }
    CHECK_EQ(before[8], 2);
    palette[2][0] = 255;
    palette[2][1] = 0;
    palette[2][2] = 0;
    cache.set_palette(palette, 3);
    ramp = cache.ramp(1, 0);
    CHECK_EQ(ramp[8], 1);
    CHECK(before[8] != ramp[8]);
}

void invalidate_keeps_the_palette() {
    Cache cache;
    cache.set_palette(kGreys, kGreyCount);
    cache.invalidate();
    CHECK(cache.has_palette());
    const u8* ramp = cache.ramp(kGreyCount - 1, 0);
    CHECK(ramp != nullptr);
    CHECK_EQ(ramp[kLevels - 1], kGreyCount - 1);
}

// The Workbench palette case: between dark text and a grey background the
// nearest colour to a mid blend is a teal, but the ramp keeps to greys.
void ramp_keeps_the_hue_of_its_pens() {
    const u8 palette[4][3] = {
        {160, 166, 176}, {28, 30, 34}, {51, 102, 102}, {102, 102, 102}};
    Cache cache;
    cache.set_palette(palette, 4);
    const u8* ramp = cache.ramp(1, 0);
    for (u32 level = 1; level + 1 < kLevels; ++level) {
        CHECK(ramp[level] != 2);
    }
    CHECK_EQ(ramp[12], 3);
}

void nearest_breaks_ties_towards_the_lower_pen() {
    const u8 palette[3][3] = {{0, 0, 0}, {4, 0, 0}, {8, 0, 0}};
    CHECK_EQ(text_blend::nearest(palette, 3, 2, 0, 0), 0);
    CHECK_EQ(text_blend::nearest(palette, 3, 6, 0, 0), 1);
    CHECK_EQ(text_blend::nearest(palette, 3, 8, 0, 0), 2);
    CHECK_EQ(text_blend::nearest(palette, 1, 8, 0, 0), 0);
    const u8 duplicate[2][3] = {{5, 5, 5}, {5, 5, 5}};
    CHECK_EQ(text_blend::nearest(duplicate, 2, 5, 5, 5), 0);
}

// Text is blended from 32 pens, the five planes of the modern look, to 256;
// the cache of a palette of that size ramps between its own pens only.
void blending_needs_between_five_and_eight_planes() {
    for (u32 planes = 0; planes <= 9; ++planes) {
        CHECK_EQ(text_blend::blends_on(planes), planes >= 5 && planes <= 8);
    }
    for (const u32 planes : {5u, 6u, 7u}) {
        const u32 pens = 1u << planes;
        u8 palette[128][3];
        for (u32 i = 0; i < pens; ++i) {
            const u8 grey = static_cast<u8>(i * 255 / (pens - 1));
            palette[i][0] = grey;
            palette[i][1] = grey;
            palette[i][2] = grey;
        }
        Cache cache;
        cache.set_palette(palette, pens);
        const u8* ramp = cache.ramp(static_cast<u8>(pens - 1), 0);
        CHECK(ramp != nullptr);
        CHECK_EQ(static_cast<u32>(ramp[0]), 0u);
        CHECK_EQ(static_cast<u32>(ramp[kLevels - 1]), pens - 1);
        for (u32 level = 1; level < kLevels; ++level) {
            CHECK(ramp[level - 1] <= ramp[level]);
            CHECK(static_cast<u32>(ramp[level]) < pens);
        }
        CHECK(cache.ramp(static_cast<u8>(pens), 0) == nullptr);
        CHECK(cache.ramp(0, static_cast<u8>(pens)) == nullptr);
    }
}

}

int main() {
    cache_without_palette_has_no_ramps();
    endpoints_are_the_pens_asked_for();
    black_to_white_ramp_rises_through_half_light();
    full_grey_palette_blends_exactly();
    colliding_ramps_are_recomputed();
    changed_palette_colour_changes_the_ramp();
    invalidate_keeps_the_palette();
    ramp_keeps_the_hue_of_its_pens();
    nearest_breaks_ties_towards_the_lower_pen();
    blending_needs_between_five_and_eight_planes();
    return check::finish("test_text_blend");
}
