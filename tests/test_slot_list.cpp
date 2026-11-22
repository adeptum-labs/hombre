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

#include "slot_list.hpp"
#include "check.hpp"

using namespace hombre;

namespace {

struct Record {
    u32 addr = 0;
    u32 value = 0;
};

void records_keep_their_address_while_others_come_and_go() {
    SlotList<Record> list;
    Record* first = list.add();
    first->addr = 1;
    std::vector<Record*> more;
    for (u32 i = 0; i < 100; ++i) {
        Record* r = list.add();
        r->addr = 100 + i;
        more.push_back(r);
    }
    CHECK_EQ(list.size(), 101u);
    for (u32 i = 0; i < 100; i += 2) {
        list.remove(more[i]);
    }
    CHECK_EQ(list.size(), 51u);
    CHECK_EQ(first->addr, 1u);
    CHECK_EQ(more[1]->addr, 101u);
    CHECK(list.find_if([](const Record& r) { return r.addr == 101; }) == more[1]);
    CHECK(list.find_if([](const Record& r) { return r.addr == 100; }) == nullptr);
}

void records_keep_the_order_they_were_added_in() {
    SlotList<Record> list;
    for (u32 i = 1; i <= 5; ++i) {
        list.add()->addr = i;
    }
    list.remove(&list[1]);
    u32 seen[4]{};
    u32 n = 0;
    for (const Record& r : list) {
        seen[n++] = r.addr;
    }
    CHECK_EQ(n, 4u);
    CHECK_EQ(seen[0], 1u);
    CHECK_EQ(seen[1], 3u);
    CHECK_EQ(seen[2], 4u);
    CHECK_EQ(seen[3], 5u);
}

void the_cap_stops_adding() {
    SlotList<Record> list(3);
    CHECK(list.add() != nullptr);
    CHECK(list.add() != nullptr);
    Record* last = list.add();
    CHECK(last != nullptr);
    CHECK(list.full());
    CHECK(list.add() == nullptr);
    list.remove(last);
    CHECK(!list.full());
    CHECK(list.add() != nullptr);
}

void remove_if_and_clear_empty_the_list() {
    SlotList<Record> list;
    for (u32 i = 0; i < 10; ++i) {
        list.add()->value = i % 2;
    }
    list.remove_if([](const Record& r) { return r.value == 1; });
    CHECK_EQ(list.size(), 5u);
    for (const Record& r : list) {
        CHECK_EQ(r.value, 0u);
    }
    Record stranger;
    list.remove(&stranger);
    CHECK_EQ(list.size(), 5u);
    list.clear();
    CHECK(list.empty());
}

}  // namespace

int main() {
    records_keep_their_address_while_others_come_and_go();
    records_keep_the_order_they_were_added_in();
    the_cap_stops_adding();
    remove_if_and_clear_empty_the_list();
    return check::finish("test_slot_list");
}
