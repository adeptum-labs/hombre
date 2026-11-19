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

#include "check.hpp"
#include "libs/menu_layout.hpp"

using namespace hombre;
using namespace hombre::menu_layout;

namespace {

constexpr i32 kBarHeight = 15;

void rect_is(const Rect& r, i32 x0, i32 y0, i32 x1, i32 y1) {
    CHECK_EQ(r.x0, x0);
    CHECK_EQ(r.y0, y0);
    CHECK_EQ(r.x1, x1);
    CHECK_EQ(r.y1, y1);
}

// Titles sit kBarLeft columns in from the bar's left end.
void titles_span_the_bar_at_their_left_edges() {
    const Title titles[2] = {{0, 48, 0, 0, kTitleEnabled}, {48, 36, 0, 0, kTitleEnabled}};
    const Geometry g = compute(titles, 2, nullptr, 0, nullptr, 0, kBarHeight, 0, -1);
    CHECK_EQ(g.title_count, 2u);
    rect_is(g.title[0], 4, 0, 51, 14);
    rect_is(g.title[1], 52, 0, 87, 14);
    rect_is(g.box, 0, 0, -1, -1);
    CHECK_EQ(g.item_count, 0u);
}

void items_fill_a_box_below_the_open_title() {
    const Title titles[2] = {{0, 48, 0, 0, kTitleEnabled}, {48, 36, 0, 0, kTitleEnabled}};
    const Item items[3] = {
        {0, 0, 60, 9, 0, 0, 0, kItemText | kItemEnabled, 0},
        {0, 9, 60, 9, 0, 0, 0, kItemText | kItemEnabled, 0},
        {0, 18, 60, 4, 0, 0, 0, 0, 0},
    };
    const Geometry g = compute(titles, 2, items, 3, nullptr, 0, kBarHeight, 1, -1);
    // The items' 60x22 extent plus three columns and two rows of frame on
    // each side; the rows reach across the paper to the border.
    rect_is(g.box, 52, 15, 117, 40);
    CHECK_EQ(g.item_count, 3u);
    rect_is(g.item[0], 53, 17, 116, 25);
    rect_is(g.item[1], 53, 26, 116, 34);
    rect_is(g.item[2], 53, 35, 116, 38);
    rect_is(g.sub_box, 0, 0, -1, -1);

    const Title wide[1] = {{0, 80, 0, 0, kTitleEnabled}};
    const Item narrow[1] = {{0, 0, 40, 9, 0, 0, 0, kItemText | kItemEnabled, 0}};
    const Geometry w = compute(wide, 1, narrow, 1, nullptr, 0, kBarHeight, 0, -1);
    rect_is(w.box, 4, 15, 83, 27);
    rect_is(w.item[0], 5, 17, 82, 25);
}

void columns_keep_their_offsets_inside_the_frame() {
    const Title titles[1] = {{0, 48, 0, 0, kTitleEnabled}};
    const Item items[2] = {
        {0, 0, 40, 9, 0, 0, 0, kItemText | kItemEnabled, 0},
        {40, 0, 30, 9, 0, 0, 0, kItemText | kItemEnabled, 0},
    };
    const Geometry g = compute(titles, 1, items, 2, nullptr, 0, kBarHeight, 0, -1);
    rect_is(g.box, 4, 15, 79, 27);
    rect_is(g.item[0], 5, 17, 46, 25);
    rect_is(g.item[1], 47, 17, 78, 25);
}

void a_box_near_the_right_edge_moves_onto_the_screen() {
    const Title titles[1] = {{600, 40, 0, 0, kTitleEnabled}};
    const Item items[1] = {{0, 0, 60, 9, 0, 0, 0, kItemText | kItemEnabled, 0}};
    const Geometry g = compute(titles, 1, items, 1, nullptr, 0, kBarHeight, 0, -1, 640);
    rect_is(g.box, 574, 15, 639, 27);
    const Geometry free = compute(titles, 1, items, 1, nullptr, 0, kBarHeight, 0, -1);
    rect_is(free.box, 604, 15, 669, 27);
}

void submenu_opens_beside_the_highlighted_item() {
    const Title titles[1] = {{0, 48, 0, 0, kTitleEnabled}};
    const Item items[1] = {{0, 0, 60, 9, 0, 0, 1, kItemText | kItemEnabled, 0}};
    const Item subs[2] = {
        {4, 2, 30, 9, 0, 0, 0, kItemText | kItemEnabled, 0},
        {4, 11, 30, 9, 0, 0, 0, kItemText | kItemEnabled, 0},
    };
    const Geometry g = compute(titles, 1, items, 1, subs, 2, kBarHeight, 0, 0);
    rect_is(g.box, 4, 15, 69, 27);
    rect_is(g.item[0], 5, 17, 68, 25);
    // It shares the drop-down box's right border, its first row level with
    // the item.
    rect_is(g.sub_box, 69, 15, 104, 36);
    CHECK_EQ(g.sub_item_count, 2u);
    rect_is(g.sub_item[0], 70, 17, 103, 25);
    rect_is(g.sub_item[1], 70, 26, 103, 34);

    const Geometry closed = compute(titles, 1, items, 1, subs, 2, kBarHeight, 0, -1);
    CHECK_EQ(closed.sub_item_count, 0u);
    rect_is(closed.sub_box, 0, 0, -1, -1);
}

void submenu_opens_on_the_left_without_room_on_the_right() {
    const Title titles[1] = {{560, 48, 0, 0, kTitleEnabled}};
    const Item items[1] = {{0, 0, 60, 9, 0, 0, 1, kItemText | kItemEnabled, 0}};
    const Item subs[1] = {{0, 0, 30, 9, 0, 0, 0, kItemText | kItemEnabled, 0}};
    const Geometry g = compute(titles, 1, items, 1, subs, 1, kBarHeight, 0, 0, 640);
    rect_is(g.box, 564, 15, 629, 27);
    rect_is(g.sub_box, 529, 15, 564, 27);
    rect_is(g.sub_item[0], 530, 17, 563, 25);
}

void an_empty_or_closed_menu_has_no_box() {
    const Title titles[1] = {{0, 48, 0, 0, kTitleEnabled}};
    const Geometry none = compute(titles, 1, nullptr, 0, nullptr, 0, kBarHeight, 0, -1);
    CHECK_EQ(none.item_count, 0u);
    CHECK(!valid(none.box));

    const Item items[1] = {{0, 0, 40, 9, 0, 0, 0, kItemText | kItemEnabled, 0}};
    const Geometry no_title =
        compute(titles, 1, items, 1, nullptr, 0, kBarHeight, kNoTitle, -1);
    CHECK_EQ(no_title.item_count, 0u);
    CHECK(!valid(no_title.box));
}

void hit_testing_follows_the_computed_rectangles() {
    const Rect titles[2] = {{0, 0, 47, 14}, {48, 0, 83, 14}};
    CHECK_EQ(hit(titles, 2, 10, 5), 0);
    CHECK_EQ(hit(titles, 2, 50, 5), 1);
    CHECK_EQ(hit(titles, 2, 84, 5), -1);
    CHECK_EQ(hit(titles, 2, 10, 15), -1);
    CHECK_EQ(hit(titles, 0, 10, 5), -1);

    const Rect items[3] = {{0, 15, 59, 23}, {0, 24, 59, 32}, {0, 33, 59, 36}};
    CHECK_EQ(hit(items, 3, 10, 15), 0);
    CHECK_EQ(hit(items, 3, 10, 23), 0);
    CHECK_EQ(hit(items, 3, 10, 24), 1);
    CHECK_EQ(hit(items, 3, 10, 33), 2);
    CHECK_EQ(hit(items, 3, 10, 37), -1);
    CHECK_EQ(hit(items, 3, 60, 24), -1);

    const Rect subs[2] = {{60, 15, 89, 23}, {60, 24, 89, 32}};
    CHECK_EQ(hit(subs, 2, 60, 15), 0);
    CHECK_EQ(hit(subs, 2, 75, 28), 1);
    CHECK_EQ(hit(subs, 2, 59, 24), -1);
    CHECK_EQ(hit(subs, 2, 90, 15), -1);
}

}

int main() {
    titles_span_the_bar_at_their_left_edges();
    items_fill_a_box_below_the_open_title();
    columns_keep_their_offsets_inside_the_frame();
    a_box_near_the_right_edge_moves_onto_the_screen();
    submenu_opens_beside_the_highlighted_item();
    submenu_opens_on_the_left_without_room_on_the_right();
    an_empty_or_closed_menu_has_no_box();
    hit_testing_follows_the_computed_rectangles();
    return check::finish("test_menu_layout");
}
