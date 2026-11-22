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

#include "types.hpp"

#include <memory>
#include <vector>

namespace hombre {

// Records that come and go at run time: each one lives in its own
// allocation, so a pointer to it stays good until that record is removed,
// however many are added or removed around it. Records keep the order they
// were added in. The list grows as needed up to its cap, which keeps a
// runaway program from taking all memory; 0 means no cap.
template <typename T>
class SlotList {
public:
    constexpr SlotList() = default;
    constexpr explicit SlotList(usize cap) : cap_(cap) {}

    // A new default record at the end, or nullptr at the cap.
    T* add() {
        if (cap_ != 0 && items_.size() >= cap_) {
            return nullptr;
        }
        items_.push_back(std::make_unique<T>());
        return items_.back().get();
    }

    // Removes the record, freeing it; one not in the list is ignored.
    void remove(const T* record) {
        for (auto it = items_.begin(); it != items_.end(); ++it) {
            if (it->get() == record) {
                items_.erase(it);
                return;
            }
        }
    }

    template <typename Pred>
    T* find_if(Pred pred) {
        for (const std::unique_ptr<T>& item : items_) {
            if (pred(*item)) {
                return item.get();
            }
        }
        return nullptr;
    }

    template <typename Pred>
    const T* find_if(Pred pred) const {
        for (const std::unique_ptr<T>& item : items_) {
            if (pred(*item)) {
                return item.get();
            }
        }
        return nullptr;
    }

    // Removes every record pred accepts.
    template <typename Pred>
    void remove_if(Pred pred) {
        std::erase_if(items_, [&](const std::unique_ptr<T>& item) { return pred(*item); });
    }

    void clear() { items_.clear(); }
    usize size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    bool full() const { return cap_ != 0 && items_.size() >= cap_; }
    usize cap() const { return cap_; }

    T& operator[](usize i) { return *items_[i]; }
    const T& operator[](usize i) const { return *items_[i]; }

    // Iterates the records themselves. Adding or removing records while
    // iterating invalidates the iterators, not the records.
    template <typename Inner, typename Value>
    class Iter {
    public:
        explicit Iter(Inner at) : at_(at) {}
        Value& operator*() const { return **at_; }
        Value* operator->() const { return at_->get(); }
        Iter& operator++() {
            ++at_;
            return *this;
        }
        bool operator==(const Iter& other) const { return at_ == other.at_; }

    private:
        Inner at_;
    };

    using iterator = Iter<typename std::vector<std::unique_ptr<T>>::iterator, T>;
    using const_iterator =
        Iter<typename std::vector<std::unique_ptr<T>>::const_iterator, const T>;

    iterator begin() { return iterator(items_.begin()); }
    iterator end() { return iterator(items_.end()); }
    const_iterator begin() const { return const_iterator(items_.begin()); }
    const_iterator end() const { return const_iterator(items_.end()); }

private:
    std::vector<std::unique_ptr<T>> items_;
    usize cap_ = 0;
};

}  // namespace hombre
