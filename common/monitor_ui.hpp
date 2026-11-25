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

#include "format.hpp"
#include "hid_keyboard.hpp"
#include "libs/chrome.hpp"
#include "monitor_snapshot.hpp"
#include "monitor_view.hpp"
#include "types.hpp"

namespace hombre {

enum class MonitorOp : u8 {
    None,
    Close,
    Suspend,
    Resume,
    End,
    Inspect,
    Reboot,
    ListDirectory,
    Insert,
    Eject,
};

struct MonitorRequest {
    MonitorOp op = MonitorOp::None;
    u32 slot = 0;
    u32 task_addr = 0;
    u32 drive = 0;
    char path[256]{};
};

enum class MonitorStyle : u8 {
    Normal,
    Title,
    Selected,
    Dim,
    Warning,
};

inline constexpr u8 monitor_style(MonitorStyle style) {
    return static_cast<u8>(style);
}

struct MonitorCell {
    char ch = ' ';
    u8 style = monitor_style(MonitorStyle::Normal);
};

struct MonitorGrid {
    static constexpr u32 kMaxColumns = 160;
    static constexpr u32 kMaxRows = 60;

    u32 columns = 0;
    u32 rows = 0;
    MonitorCell cells[kMaxColumns * kMaxRows]{};

    MonitorCell& at(u32 column, u32 row) { return cells[row * kMaxColumns + column]; }
    const MonitorCell& at(u32 column, u32 row) const {
        return cells[row * kMaxColumns + column];
    }
};

template <typename Visitor>
void visit_changed_cells(const MonitorGrid& previous, const MonitorGrid& current, Visitor visit) {
    u32 columns = previous.columns < current.columns ? previous.columns : current.columns;
    u32 rows = previous.rows < current.rows ? previous.rows : current.rows;
    if (columns > MonitorGrid::kMaxColumns) {
        columns = MonitorGrid::kMaxColumns;
    }
    if (rows > MonitorGrid::kMaxRows) {
        rows = MonitorGrid::kMaxRows;
    }
    for (u32 row = 0; row < rows; ++row) {
        for (u32 column = 0; column < columns; ++column) {
            const MonitorCell& before = previous.at(column, row);
            const MonitorCell& now = current.at(column, row);
            if (before.ch != now.ch || before.style != now.style) {
                visit(column, row, now);
            }
        }
    }
}

// The full-screen monitor model: it keeps the selection and view state, turns
// keyboard reports into requests and lays the snapshot out into a character
// grid. Everything here runs in interrupt context.
class MonitorUi {
public:
    static constexpr u32 kPathBytes = 256;
    static constexpr u32 kTextColumns = 80;

    void reset();
    MonitorRequest key_report(const u8 report[8], const MonitorSnapshot& snapshot);
    void render(const MonitorSnapshot& snapshot, MonitorGrid& grid);
    void layout(const MonitorSnapshot& snapshot, MonitorView& view);
    void start_mouse(i32 x, i32 y, u8 buttons);
    MonitorRequest mouse(const MonitorMouse& mouse_state, const MonitorSnapshot& snapshot,
                         const MonitorView& view);

private:
    enum class Mode : u8 { Main, Browser, Inspect };
    enum class Focus : u8 { Tasks, Drives };
    enum class Confirm : u8 { None, Suspend, End, Reboot, Eject };
    enum class KeyCode : u8 { Up, Down, Left, Right, Tab, Enter, Escape, Backspace, Character };

    static constexpr i32 kWindowMargin = 10;
    static constexpr i32 kWindowWidth = static_cast<i32>(kMonitorWidth) - 2 * kWindowMargin;
    static constexpr i32 kWindowHeight = static_cast<i32>(kMonitorHeight) - 2 * kWindowMargin;
    static constexpr i32 kContentInset = 8;
    static constexpr i32 kGroupGap = 6;
    static constexpr i32 kTextHeight = 10;
    static constexpr i32 kCpuGroupHeight = 54;
    static constexpr i32 kMemoryGroupHeight = 36;
    static constexpr i32 kCpuColumns = 7;
    static constexpr i32 kCpuRows = 2;
    static constexpr i32 kCpuCellWidth = 84;
    static constexpr i32 kCpuCellHeight = 20;
    static constexpr i32 kCpuGaugeWidth = 46;
    static constexpr i32 kCpuGaugeHeight = 12;
    static constexpr i32 kCpuLabelOffset = 50;
    static constexpr i32 kRowPitch = 11;
    static constexpr i32 kScrollerWidth = 14;
    static constexpr i32 kScrollerGap = 6;
    static constexpr i32 kButtonWidth = 94;
    static constexpr i32 kButtonHeight = 18;
    static constexpr i32 kButtonGap = 6;
    static constexpr i32 kDriveWidth = 145;
    static constexpr i32 kDriveHeight = 15;
    static constexpr i32 kDriveGap = 6;
    static constexpr i32 kDialogWidth = 420;
    static constexpr i32 kDialogHeight = 80;
    static constexpr i32 kDialogButtonWidth = 100;
    static constexpr i32 kDialogButtonHeight = 18;
    static constexpr u64 kDoubleClickUs = 500000;

    static constexpr Rect window_box() {
        return Rect{kWindowMargin, kWindowMargin, kWindowMargin + kWindowWidth - 1,
                    kWindowMargin + kWindowHeight - 1};
    }
    static constexpr i32 content_left() { return kWindowMargin + kContentInset; }
    static constexpr i32 content_right() {
        return kWindowMargin + kWindowWidth - 1 - kContentInset;
    }
    static constexpr i32 content_top() {
        return kWindowMargin + chrome::Metrics{}.border_top() + kGroupGap;
    }
    static constexpr i32 content_bottom() {
        return kWindowMargin + kWindowHeight - 1 - kContentInset;
    }
    static constexpr i32 header_top() {
        return content_top() + kCpuGroupHeight + kGroupGap + kMemoryGroupHeight + kGroupGap;
    }
    static constexpr i32 rows_top() { return header_top() + kTextHeight; }
    static constexpr i32 status_top() { return content_bottom() - kTextHeight + 1; }
    static constexpr i32 drives_top() { return status_top() - kGroupGap - kDriveHeight; }
    static constexpr i32 buttons_top() { return drives_top() - kGroupGap - kButtonHeight; }
    static constexpr i32 rows_bottom() { return buttons_top() - kGroupGap - 1; }
    static constexpr u32 list_rows() {
        return static_cast<u32>((rows_bottom() - rows_top() + 1) / kRowPitch);
    }
    static constexpr Rect list_row_box(u32 row) {
        const i32 y = rows_top() + static_cast<i32>(row) * kRowPitch;
        return Rect{content_left(), y, content_right() - kScrollerWidth - kScrollerGap,
                    y + kTextHeight - 1};
    }
    static constexpr i32 button_left(u32 button) {
        return content_left() + static_cast<i32>(button) * (kButtonWidth + kButtonGap);
    }

    struct Key {
        KeyCode code = KeyCode::Character;
        char ch = '\0';
    };

    template <u32 Capacity>
    struct Text {
        char text[Capacity]{};
        u32 length = 0;

        void add(char c) {
            if (length + 1 < Capacity) {
                text[length++] = c;
                text[length] = '\0';
            }
        }

        void add(const char* s) {
            for (; *s != '\0'; ++s) {
                add(*s);
            }
        }

        void add_padded(u64 value, u32 width, char pad) {
            char digits[21]{};
            const usize count = format_u64(digits, sizeof digits, value, 10);
            for (u32 i = static_cast<u32>(count); i < width; ++i) {
                add(pad);
            }
            add(digits);
        }

        void add_signed(i32 value, u32 width, char pad) {
            const bool negative = value < 0;
            const u64 magnitude = negative ? static_cast<u64>(-static_cast<i64>(value))
                                           : static_cast<u64>(value);
            char digits[21]{};
            const usize count = format_u64(digits, sizeof digits, magnitude, 10);
            u32 total = static_cast<u32>(count) + (negative ? 1u : 0u);
            for (; total < width; ++total) {
                add(pad);
            }
            if (negative) {
                add('-');
            }
            add(digits);
        }

        void add_hex(u64 value, u32 width) {
            char digits[21]{};
            const usize count = format_u64(digits, sizeof digits, value, 16);
            for (u32 i = static_cast<u32>(count); i < width; ++i) {
                add('0');
            }
            add(digits);
        }

        void add_field(const char* s, u32 width) {
            bool ended = false;
            for (u32 i = 0; i < width; ++i) {
                char c = ' ';
                if (!ended) {
                    c = s[i];
                    if (c == '\0') {
                        ended = true;
                        c = ' ';
                    }
                }
                add(c);
            }
        }
    };

    static Key decode_key(const u8* data, usize count, usize& index);
    static bool is_char(const Key& key, char lower);
    static const MonitorTask* find_task(const MonitorSnapshot& snapshot, u32 task_addr);
    static u32 task_count(const MonitorSnapshot& snapshot);
    static u32 entry_count(const MonitorSnapshot& snapshot);
    static const char* state_text(const MonitorTask& task);
    static void copy_text(char* destination, const char* source);
    static u32 text_length(const char* text);
    static void parent_path(char* path);

    template <u32 Capacity>
    static void join_path(Text<Capacity>& text, const char* parent, const char* name);

    template <u32 Capacity>
    static void add_human(Text<Capacity>& text, u64 bytes);

    template <u32 Capacity>
    static void add_cpu(Text<Capacity>& text, u32 permille);

    template <u32 Capacity>
    static void add_task_name(const MonitorSnapshot& snapshot, Text<Capacity>& text, u32 task_addr);

    static MonitorRequest task_request(MonitorOp op, u32 slot, u32 task_addr);
    static MonitorRequest drive_request(MonitorOp op, u32 drive);
    static MonitorRequest path_request(MonitorOp op, u32 drive, const char* path);

    void sync(const MonitorSnapshot& snapshot);
    MonitorRequest handle_key(const Key& key, const MonitorSnapshot& snapshot);
    MonitorRequest handle_main(const Key& key, const MonitorSnapshot& snapshot);
    MonitorRequest handle_browser(const Key& key, const MonitorSnapshot& snapshot);
    MonitorRequest finish_confirm(const Key& key);
    void move_task(i32 delta, const MonitorSnapshot& snapshot);
    void move_drive(i32 delta);
    MonitorRequest inspect_selected(const MonitorSnapshot& snapshot);
    MonitorRequest suspend_or_resume(const MonitorSnapshot& snapshot);
    void begin_task_confirm(Confirm confirm, const MonitorSnapshot& snapshot);
    MonitorRequest eject_selected(const MonitorSnapshot& snapshot);
    MonitorRequest open_browser();
    MonitorRequest browser_enter(const MonitorSnapshot& snapshot);
    MonitorRequest browser_parent();
    void browser_cancel();

    template <u32 Capacity>
    static void copy_into(char (&destination)[Capacity], const char* source);

    template <u32 Capacity>
    static void add_memory_chip(const MonitorSnapshot& snapshot, Text<Capacity>& text);

    template <u32 Capacity>
    static void add_memory_kernel(const MonitorSnapshot& snapshot, Text<Capacity>& text);

    template <u32 Capacity>
    void add_confirm_question(const MonitorSnapshot& snapshot, Text<Capacity>& text);

    static void add_task_fields(const MonitorTask& task, Text<80>& text);
    static MonitorPart* add_part(MonitorView& view, MonitorPartKind kind, const Rect& box);
    static void add_group(MonitorView& view, const Rect& box, const char* title);
    static void add_text_part(MonitorView& view, const Rect& box, const char* text, bool warning);
    static void add_button(MonitorView& view, const Rect& box, const char* label,
                           MonitorAction action, bool disabled);
    static void add_scroller(MonitorView& view, const Rect& box, u32 total, u32 scroll);
    void add_drive(MonitorView& view, u32 drive, const MonitorSnapshot& snapshot);
    static void clamp_scroll(u32& scroll, u32 total, u32 visible);
    static void scroll_by(u32& scroll, i64 delta, u32 total);

    MonitorRequest mouse_press(const MonitorMouse& mouse_state, const MonitorSnapshot& snapshot,
                               const MonitorView& view);
    MonitorRequest mouse_release(const MonitorMouse& mouse_state, const MonitorSnapshot& snapshot,
                                 const MonitorView& view);
    MonitorRequest press_indexed(const MonitorMouse& mouse_state, MonitorAction action, u32 index,
                                 const MonitorSnapshot& snapshot);
    void press_scroller(const MonitorMouse& mouse_state, const MonitorSnapshot& snapshot,
                        const MonitorPart& part);
    void drag_scroller(i32 y, const MonitorSnapshot& snapshot, const MonitorView& view);
    void scroll_by_wheel(i32 wheel, const MonitorSnapshot& snapshot);

    static u32 scrolled(u32 scroll, u32 index, u32 total, u32 visible);
    static u32 visible_columns(const MonitorGrid& grid);
    static u32 visible_rows(const MonitorGrid& grid);
    static void fill(MonitorGrid& grid);
    static void fill_row(MonitorGrid& grid, i32 row, u32 first, u32 last, u8 cell_style);
    static void put_cell(MonitorGrid& grid, i32 row, u32 column, char ch, u8 cell_style);
    static void put_text(MonitorGrid& grid, i32 row, u32 column, const char* text, u8 cell_style);
    static void put_field(MonitorGrid& grid, i32 row, u32 column, const char* text, u32 width,
                          u8 cell_style);

    void render_title(MonitorGrid& grid, const MonitorSnapshot& snapshot, i32 row) const;
    void render_cpu(MonitorGrid& grid, const MonitorSnapshot& snapshot, i32 row) const;
    void render_memory(MonitorGrid& grid, const MonitorSnapshot& snapshot, i32 chip_row,
                       i32 kernel_row) const;
    void render_header(MonitorGrid& grid, const MonitorSnapshot& snapshot, i32 row) const;
    void render_tasks(const MonitorSnapshot& snapshot, MonitorGrid& grid, i32 first, i32 count);
    void render_task_row(const MonitorTask& task, MonitorGrid& grid, i32 row, bool selected) const;
    void render_browser(const MonitorSnapshot& snapshot, MonitorGrid& grid, i32 first, i32 count);
    void render_inspect(const MonitorSnapshot& snapshot, MonitorGrid& grid, i32 first, i32 count) const;
    void render_drives(MonitorGrid& grid, const MonitorSnapshot& snapshot, i32 first_row) const;
    void render_status(MonitorGrid& grid, const MonitorSnapshot& snapshot, i32 row) const;
    void render_help(MonitorGrid& grid, const MonitorSnapshot& snapshot, i32 row) const;

    KeyDecoder decoder_{};
    Mode mode_ = Mode::Main;
    Focus focus_ = Focus::Tasks;
    Confirm confirm_ = Confirm::None;
    u32 confirm_slot_ = 0;
    u32 confirm_addr_ = 0;
    u32 confirm_drive_ = 0;
    u32 selected_addr_ = 0;
    bool has_selection_ = false;
    u32 task_index_ = 0;
    u32 task_scroll_ = 0;
    u32 drive_index_ = 0;
    u32 browser_drive_ = 0;
    u32 browser_index_ = 0;
    u32 browser_scroll_ = 0;
    char browser_path_[kPathBytes]{};
    u32 inspect_addr_ = 0;
    bool follow_selection_ = false;
    i32 mouse_x_ = 0;
    i32 mouse_y_ = 0;
    u8 mouse_buttons_ = 0;
    bool mouse_pressed_ = false;
    MonitorAction press_action_ = MonitorAction::None;
    u32 press_index_ = 0;
    bool scroll_drag_ = false;
    i32 drag_offset_ = 0;
    bool click_valid_ = false;
    MonitorAction click_action_ = MonitorAction::None;
    u32 click_key_ = 0;
    u64 click_us_ = 0;
};

inline void MonitorUi::reset() {
    decoder_.reset();
    mode_ = Mode::Main;
    focus_ = Focus::Tasks;
    confirm_ = Confirm::None;
    confirm_slot_ = 0;
    confirm_addr_ = 0;
    confirm_drive_ = 0;
    selected_addr_ = 0;
    has_selection_ = false;
    task_index_ = 0;
    task_scroll_ = 0;
    drive_index_ = 0;
    browser_drive_ = 0;
    browser_index_ = 0;
    browser_scroll_ = 0;
    browser_path_[0] = '\0';
    inspect_addr_ = 0;
    follow_selection_ = false;
    mouse_x_ = 0;
    mouse_y_ = 0;
    mouse_buttons_ = 0;
    mouse_pressed_ = false;
    press_action_ = MonitorAction::None;
    press_index_ = 0;
    scroll_drag_ = false;
    drag_offset_ = 0;
    click_valid_ = false;
    click_action_ = MonitorAction::None;
    click_key_ = 0;
    click_us_ = 0;
}

inline MonitorUi::Key MonitorUi::decode_key(const u8* data, usize count, usize& index) {
    const u8 byte = data[index++];
    if (byte == 0x9B && index < count) {
        switch (data[index++]) {
        case 'A':
        case 'T':
            return {KeyCode::Up, 0};
        case 'B':
        case 'S':
            return {KeyCode::Down, 0};
        case 'C':
            return {KeyCode::Right, 0};
        case 'D':
            return {KeyCode::Left, 0};
        default:
            return {};
        }
    }
    switch (byte) {
    case 0x1B:
        return {KeyCode::Escape, 0};
    case '\t':
        return {KeyCode::Tab, 0};
    case '\n':
    case '\r':
        return {KeyCode::Enter, 0};
    case 0x08:
    case 0x7F:
        return {KeyCode::Backspace, 0};
    default:
        return {KeyCode::Character, static_cast<char>(byte)};
    }
}

inline bool MonitorUi::is_char(const Key& key, char lower) {
    return key.code == KeyCode::Character &&
           (key.ch == lower || key.ch == static_cast<char>(lower - 'a' + 'A'));
}

inline const MonitorTask* MonitorUi::find_task(const MonitorSnapshot& snapshot, u32 task_addr) {
    const u32 total = task_count(snapshot);
    for (u32 i = 0; i < total; ++i) {
        if (snapshot.tasks[i].task_addr == task_addr) {
            return &snapshot.tasks[i];
        }
    }
    return nullptr;
}

inline u32 MonitorUi::task_count(const MonitorSnapshot& snapshot) {
    return snapshot.task_count < MonitorSnapshot::kMaxTasks ? snapshot.task_count
                                                            : MonitorSnapshot::kMaxTasks;
}

inline u32 MonitorUi::entry_count(const MonitorSnapshot& snapshot) {
    return snapshot.listing.count < MonitorSnapshot::kMaxEntries ? snapshot.listing.count
                                                                 : MonitorSnapshot::kMaxEntries;
}

inline const char* MonitorUi::state_text(const MonitorTask& task) {
    if (task.suspended) {
        return "susp";
    }
    switch (task.state) {
    case TaskState::Running:
        return "run";
    case TaskState::Ready:
        return "ready";
    case TaskState::Waiting:
        return "wait";
    case TaskState::Free:
        return "free";
    case TaskState::Removed:
        return "rem";
    }
    return "?";
}

inline void MonitorUi::copy_text(char* destination, const char* source) {
    u32 i = 0;
    for (; source[i] != '\0' && i + 1 < kPathBytes; ++i) {
        destination[i] = source[i];
    }
    destination[i] = '\0';
}

inline u32 MonitorUi::text_length(const char* text) {
    u32 length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

inline void MonitorUi::parent_path(char* path) {
    const u32 length = text_length(path);
    u32 slash = length;
    u32 colon = length;
    for (u32 i = 0; i < length; ++i) {
        if (path[i] == '/') {
            slash = i;
        } else if (path[i] == ':') {
            colon = i;
        }
    }
    u32 keep = 0;
    if (slash != length) {
        keep = slash;
    } else if (length != 0 && path[length - 1] == ':') {
        keep = 0;
    } else if (colon != length) {
        keep = colon + 1;
    }
    path[keep] = '\0';
}

template <u32 Capacity>
inline void MonitorUi::join_path(Text<Capacity>& text, const char* parent, const char* name) {
    text.add(parent);
    if (parent[0] != '\0' && parent[text_length(parent) - 1] != ':') {
        text.add('/');
    }
    text.add(name);
}

template <u32 Capacity>
inline void MonitorUi::add_human(Text<Capacity>& text, u64 bytes) {
    if (bytes >= (1ull << 30)) {
        u64 whole = bytes >> 30;
        u64 tenths = ((bytes & ((1ull << 30) - 1)) * 10 + (1ull << 29)) >> 30;
        if (tenths == 10) {
            ++whole;
            tenths = 0;
        }
        text.add_padded(whole, 1, ' ');
        text.add('.');
        text.add(static_cast<char>('0' + tenths));
        text.add(" GiB");
    } else if (bytes >= (1ull << 20)) {
        text.add_padded(bytes >> 20, 1, ' ');
        text.add(" MiB");
    } else if (bytes >= 1024) {
        text.add_padded(bytes >> 10, 1, ' ');
        text.add(" KiB");
    } else {
        text.add_padded(bytes, 1, ' ');
        text.add(" B");
    }
}

template <u32 Capacity>
inline void MonitorUi::add_cpu(Text<Capacity>& text, u32 permille) {
    const u32 clamped = permille > 1000 ? 1000 : permille;
    const u32 whole = clamped / 10;
    if (whole < 10) {
        text.add(' ');
    }
    text.add_padded(whole, 1, ' ');
    text.add('.');
    text.add(static_cast<char>('0' + clamped % 10));
}

template <u32 Capacity>
inline void MonitorUi::add_task_name(const MonitorSnapshot& snapshot, Text<Capacity>& text,
                                     u32 task_addr) {
    const MonitorTask* task = find_task(snapshot, task_addr);
    text.add(task != nullptr ? task->name : "(gone)");
}

inline MonitorRequest MonitorUi::task_request(MonitorOp op, u32 slot, u32 task_addr) {
    MonitorRequest request{};
    request.op = op;
    request.slot = slot;
    request.task_addr = task_addr;
    return request;
}

inline MonitorRequest MonitorUi::drive_request(MonitorOp op, u32 drive) {
    MonitorRequest request{};
    request.op = op;
    request.drive = drive;
    return request;
}

inline MonitorRequest MonitorUi::path_request(MonitorOp op, u32 drive, const char* path) {
    MonitorRequest request{};
    request.op = op;
    request.drive = drive;
    copy_text(request.path, path);
    return request;
}

inline void MonitorUi::sync(const MonitorSnapshot& snapshot) {
    const u32 total = task_count(snapshot);
    if (total == 0) {
        task_index_ = 0;
        has_selection_ = false;
    } else {
        u32 index = total;
        if (has_selection_) {
            for (u32 i = 0; i < total; ++i) {
                if (snapshot.tasks[i].task_addr == selected_addr_) {
                    index = i;
                    break;
                }
            }
        }
        if (index == total) {
            index = task_index_ < total ? task_index_ : total - 1;
        }
        task_index_ = index;
        selected_addr_ = snapshot.tasks[index].task_addr;
        has_selection_ = true;
    }
    if (mode_ == Mode::Inspect && find_task(snapshot, inspect_addr_) == nullptr) {
        mode_ = Mode::Main;
        focus_ = Focus::Tasks;
        follow_selection_ = true;
    }
    if ((confirm_ == Confirm::Suspend || confirm_ == Confirm::End) &&
        find_task(snapshot, confirm_addr_) == nullptr) {
        confirm_ = Confirm::None;
    }
    if (confirm_ == Confirm::Eject && !snapshot.drives[confirm_drive_].inserted) {
        confirm_ = Confirm::None;
    }
    if (mode_ == Mode::Browser) {
        const u32 entries = entry_count(snapshot);
        if (entries == 0) {
            browser_index_ = 0;
        } else if (browser_index_ >= entries) {
            browser_index_ = entries - 1;
        }
    }
}

inline MonitorRequest MonitorUi::key_report(const u8 report[8],
                                             const MonitorSnapshot& snapshot) {
    sync(snapshot);
    // A key must not leave an earlier mouse press to fire on its release.
    mouse_pressed_ = false;
    scroll_drag_ = false;
    click_valid_ = false;
    u8 decoded[32];
    const usize count = decoder_.feed(report, decoded, sizeof decoded);
    usize index = 0;
    if (confirm_ != Confirm::None) {
        if (index >= count) {
            return MonitorRequest{};
        }
        return finish_confirm(decode_key(decoded, count, index));
    }
    while (index < count) {
        MonitorRequest request = handle_key(decode_key(decoded, count, index), snapshot);
        if (request.op != MonitorOp::None) {
            return request;
        }
        if (confirm_ != Confirm::None && index < count) {
            return finish_confirm(decode_key(decoded, count, index));
        }
    }
    return MonitorRequest{};
}

inline MonitorRequest MonitorUi::handle_key(const Key& key, const MonitorSnapshot& snapshot) {
    switch (mode_) {
    case Mode::Main:
        return handle_main(key, snapshot);
    case Mode::Browser:
        return handle_browser(key, snapshot);
    case Mode::Inspect:
        if (key.code == KeyCode::Escape) {
            mode_ = Mode::Main;
            focus_ = Focus::Tasks;
            follow_selection_ = true;
        }
        return MonitorRequest{};
    }
    return MonitorRequest{};
}

inline MonitorRequest MonitorUi::handle_main(const Key& key, const MonitorSnapshot& snapshot) {
    if (key.code == KeyCode::Escape) {
        return task_request(MonitorOp::Close, 0, 0);
    }
    if (key.code == KeyCode::Tab) {
        focus_ = focus_ == Focus::Tasks ? Focus::Drives : Focus::Tasks;
        return MonitorRequest{};
    }
    if (is_char(key, 'r')) {
        confirm_ = Confirm::Reboot;
        return MonitorRequest{};
    }
    if (focus_ == Focus::Tasks) {
        switch (key.code) {
        case KeyCode::Up:
            move_task(-1, snapshot);
            return MonitorRequest{};
        case KeyCode::Down:
            move_task(1, snapshot);
            return MonitorRequest{};
        case KeyCode::Enter:
            return inspect_selected(snapshot);
        default:
            break;
        }
        if (is_char(key, 's')) {
            return suspend_or_resume(snapshot);
        }
        if (is_char(key, 'k')) {
            begin_task_confirm(Confirm::End, snapshot);
        }
        return MonitorRequest{};
    }
    switch (key.code) {
    case KeyCode::Up:
    case KeyCode::Left:
        move_drive(-1);
        return MonitorRequest{};
    case KeyCode::Down:
    case KeyCode::Right:
        move_drive(1);
        return MonitorRequest{};
    case KeyCode::Enter:
        return open_browser();
    default:
        break;
    }
    if (is_char(key, 'e')) {
        return eject_selected(snapshot);
    }
    return MonitorRequest{};
}

inline MonitorRequest MonitorUi::handle_browser(const Key& key, const MonitorSnapshot& snapshot) {
    if (key.code == KeyCode::Escape) {
        browser_cancel();
        return MonitorRequest{};
    }
    if (key.code == KeyCode::Up) {
        if (browser_index_ > 0) {
            --browser_index_;
            follow_selection_ = true;
        }
        return MonitorRequest{};
    }
    if (key.code == KeyCode::Down) {
        if (browser_index_ + 1 < entry_count(snapshot)) {
            ++browser_index_;
            follow_selection_ = true;
        }
        return MonitorRequest{};
    }
    if (key.code == KeyCode::Backspace) {
        return browser_parent();
    }
    if (key.code == KeyCode::Enter) {
        return browser_enter(snapshot);
    }
    return MonitorRequest{};
}

inline MonitorRequest MonitorUi::browser_enter(const MonitorSnapshot& snapshot) {
    if (browser_index_ >= entry_count(snapshot)) {
        return MonitorRequest{};
    }
    const MonitorEntry& entry = snapshot.listing.entries[browser_index_];
    Text<kPathBytes> path;
    join_path(path, browser_path_, entry.name);
    follow_selection_ = true;
    if (entry.is_dir) {
        copy_text(browser_path_, path.text);
        browser_index_ = 0;
        browser_scroll_ = 0;
        return path_request(MonitorOp::ListDirectory, browser_drive_, path.text);
    }
    mode_ = Mode::Main;
    focus_ = Focus::Drives;
    return path_request(MonitorOp::Insert, browser_drive_, path.text);
}

inline MonitorRequest MonitorUi::browser_parent() {
    if (browser_path_[0] == '\0') {
        return MonitorRequest{};
    }
    parent_path(browser_path_);
    browser_index_ = 0;
    browser_scroll_ = 0;
    follow_selection_ = true;
    return path_request(MonitorOp::ListDirectory, browser_drive_, browser_path_);
}

inline void MonitorUi::browser_cancel() {
    mode_ = Mode::Main;
    focus_ = Focus::Drives;
    follow_selection_ = true;
}

inline MonitorRequest MonitorUi::finish_confirm(const Key& key) {
    const Confirm pending = confirm_;
    confirm_ = Confirm::None;
    if (!is_char(key, 'y')) {
        return MonitorRequest{};
    }
    switch (pending) {
    case Confirm::Suspend:
        return task_request(MonitorOp::Suspend, confirm_slot_, confirm_addr_);
    case Confirm::End:
        return task_request(MonitorOp::End, confirm_slot_, confirm_addr_);
    case Confirm::Reboot:
        return task_request(MonitorOp::Reboot, 0, 0);
    case Confirm::Eject:
        return drive_request(MonitorOp::Eject, confirm_drive_);
    case Confirm::None:
        break;
    }
    return MonitorRequest{};
}

inline void MonitorUi::move_task(i32 delta, const MonitorSnapshot& snapshot) {
    const u32 total = task_count(snapshot);
    if (total == 0) {
        return;
    }
    if (delta < 0) {
        if (task_index_ > 0) {
            --task_index_;
        }
    } else if (task_index_ + 1 < total) {
        ++task_index_;
    }
    selected_addr_ = snapshot.tasks[task_index_].task_addr;
    has_selection_ = true;
    follow_selection_ = true;
}

inline void MonitorUi::move_drive(i32 delta) {
    if (delta < 0) {
        if (drive_index_ > 0) {
            --drive_index_;
        }
    } else if (drive_index_ + 1 < MonitorSnapshot::kDriveCount) {
        ++drive_index_;
    }
}

inline MonitorRequest MonitorUi::inspect_selected(const MonitorSnapshot& snapshot) {
    if (task_count(snapshot) == 0) {
        return MonitorRequest{};
    }
    const MonitorTask& task = snapshot.tasks[task_index_];
    inspect_addr_ = task.task_addr;
    mode_ = Mode::Inspect;
    focus_ = Focus::Tasks;
    follow_selection_ = true;
    return task_request(MonitorOp::Inspect, task.slot, task.task_addr);
}

inline MonitorRequest MonitorUi::suspend_or_resume(const MonitorSnapshot& snapshot) {
    if (task_count(snapshot) == 0) {
        return MonitorRequest{};
    }
    const MonitorTask& task = snapshot.tasks[task_index_];
    if (task.suspended) {
        return task_request(MonitorOp::Resume, task.slot, task.task_addr);
    }
    begin_task_confirm(Confirm::Suspend, snapshot);
    return MonitorRequest{};
}

inline void MonitorUi::begin_task_confirm(Confirm confirm, const MonitorSnapshot& snapshot) {
    if (task_count(snapshot) == 0) {
        return;
    }
    const MonitorTask& task = snapshot.tasks[task_index_];
    confirm_ = confirm;
    confirm_slot_ = task.slot;
    confirm_addr_ = task.task_addr;
}

inline MonitorRequest MonitorUi::eject_selected(const MonitorSnapshot& snapshot) {
    if (!snapshot.drives[drive_index_].inserted) {
        return MonitorRequest{};
    }
    if (drive_index_ == 0) {
        confirm_ = Confirm::Eject;
        confirm_drive_ = 0;
        return MonitorRequest{};
    }
    return drive_request(MonitorOp::Eject, drive_index_);
}

inline MonitorRequest MonitorUi::open_browser() {
    browser_drive_ = drive_index_;
    browser_path_[0] = '\0';
    browser_index_ = 0;
    browser_scroll_ = 0;
    mode_ = Mode::Browser;
    focus_ = Focus::Drives;
    follow_selection_ = true;
    return path_request(MonitorOp::ListDirectory, browser_drive_, "");
}

inline u32 MonitorUi::visible_columns(const MonitorGrid& grid) {
    return grid.columns < MonitorGrid::kMaxColumns ? grid.columns : MonitorGrid::kMaxColumns;
}

inline u32 MonitorUi::visible_rows(const MonitorGrid& grid) {
    return grid.rows < MonitorGrid::kMaxRows ? grid.rows : MonitorGrid::kMaxRows;
}

inline void MonitorUi::fill(MonitorGrid& grid) {
    const u32 columns = visible_columns(grid);
    const u32 rows = visible_rows(grid);
    for (u32 row = 0; row < rows; ++row) {
        for (u32 column = 0; column < columns; ++column) {
            MonitorCell& cell = grid.at(column, row);
            cell.ch = ' ';
            cell.style = monitor_style(MonitorStyle::Normal);
        }
    }
}

inline void MonitorUi::fill_row(MonitorGrid& grid, i32 row, u32 first, u32 last, u8 cell_style) {
    if (first > last) {
        return;
    }
    for (u32 column = first; column <= last; ++column) {
        put_cell(grid, row, column, ' ', cell_style);
    }
}

inline void MonitorUi::put_cell(MonitorGrid& grid, i32 row, u32 column, char ch, u8 cell_style) {
    if (row < 0 || column >= kTextColumns || static_cast<u32>(row) >= visible_rows(grid) ||
        column >= visible_columns(grid)) {
        return;
    }
    MonitorCell& cell = grid.at(column, static_cast<u32>(row));
    cell.ch = ch;
    cell.style = cell_style;
}

inline void MonitorUi::put_text(MonitorGrid& grid, i32 row, u32 column, const char* text,
                                u8 cell_style) {
    for (; *text != '\0'; ++text, ++column) {
        put_cell(grid, row, column, *text, cell_style);
    }
}

inline void MonitorUi::put_field(MonitorGrid& grid, i32 row, u32 column, const char* text,
                                 u32 width, u8 cell_style) {
    bool ended = false;
    for (u32 i = 0; i < width; ++i) {
        char ch = ' ';
        if (!ended) {
            ch = text[i];
            if (ch == '\0') {
                ended = true;
                ch = ' ';
            }
        }
        put_cell(grid, row, column + i, ch, cell_style);
    }
}

inline void MonitorUi::render(const MonitorSnapshot& snapshot, MonitorGrid& grid) {
    sync(snapshot);
    fill(grid);
    const i32 height = static_cast<i32>(visible_rows(grid));
    render_title(grid, snapshot, 0);
    render_cpu(grid, snapshot, 1);
    render_memory(grid, snapshot, 2, 3);
    render_header(grid, snapshot, 4);
    const i32 first = 5;
    const i32 last = height - 5;
    const i32 count = last >= first ? last - first + 1 : 0;
    switch (mode_) {
    case Mode::Main:
        render_tasks(snapshot, grid, first, count);
        break;
    case Mode::Browser:
        render_browser(snapshot, grid, first, count);
        break;
    case Mode::Inspect:
        render_inspect(snapshot, grid, first, count);
        break;
    }
    render_drives(grid, snapshot, height - 4);
    render_status(grid, snapshot, height - 2);
    render_help(grid, snapshot, height - 1);
}

inline void MonitorUi::render_title(MonitorGrid& grid, const MonitorSnapshot& snapshot,
                                    i32 row) const {
    put_text(grid, row, 0, "HOMBRE MONITOR", monitor_style(MonitorStyle::Title));
    const u64 seconds = snapshot.uptime_us / 1000000;
    Text<32> uptime;
    uptime.add("up ");
    uptime.add_padded(seconds / 3600, 2, '0');
    uptime.add(':');
    uptime.add_padded((seconds / 60) % 60, 2, '0');
    uptime.add(':');
    uptime.add_padded(seconds % 60, 2, '0');
    put_text(grid, row, 22, uptime.text, monitor_style(MonitorStyle::Dim));
    put_text(grid, row, 38, "F12 close", monitor_style(MonitorStyle::Dim));
}

inline void MonitorUi::render_cpu(MonitorGrid& grid, const MonitorSnapshot& snapshot,
                                  i32 row) const {
    put_text(grid, row, 0, "CPU", monitor_style(MonitorStyle::Title));
    const u32 cores = snapshot.cpu_count < MonitorSnapshot::kMaxCpus ? snapshot.cpu_count
                                                                     : MonitorSnapshot::kMaxCpus;
    const u32 columns = visible_columns(grid);
    const u32 limit = columns < kTextColumns ? columns : kTextColumns;
    u32 column = 5;
    for (u32 i = 0; i < cores; ++i) {
        const MonitorCpu& cpu = snapshot.cpus[i];
        Text<32> token;
        token.add_padded(cpu.apic_id, 1, ' ');
        token.add(' ');
        if (cpu.started) {
            const u32 permille = cpu.busy_permille > 1000 ? 1000 : cpu.busy_permille;
            const u32 filled = (permille * 10 + 500) / 1000;
            for (u32 cell = 0; cell < 10; ++cell) {
                token.add(cell < filled ? '#' : '.');
            }
            token.add(' ');
            token.add_padded(permille / 10, 2, ' ');
            token.add('%');
        } else {
            token.add("off");
        }
        const u32 remaining = cores - i - 1;
        Text<16> hidden;
        hidden.add('+');
        hidden.add_padded(remaining, 1, ' ');
        const u32 reserve = remaining != 0 ? hidden.length + 3 : 0;
        if (column + token.length + reserve > limit) {
            Text<16> summary;
            summary.add('+');
            summary.add_padded(cores - i, 1, ' ');
            if (column + summary.length <= limit) {
                put_text(grid, row, column, summary.text, monitor_style(MonitorStyle::Dim));
            }
            break;
        }
        put_text(grid, row, column, token.text,
                 monitor_style(cpu.started ? MonitorStyle::Normal : MonitorStyle::Dim));
        column += token.length + 3;
    }
}

template <u32 Capacity>
inline void MonitorUi::add_memory_chip(const MonitorSnapshot& snapshot, Text<Capacity>& text) {
    text.add("chip ");
    text.add_padded(snapshot.chip_free >> 20, 1, ' ');
    text.add('/');
    text.add_padded(snapshot.chip_total >> 20, 1, ' ');
    text.add(" MiB   fast ");
    text.add_padded(snapshot.fast_free >> 20, 1, ' ');
    text.add('/');
    text.add_padded(snapshot.fast_total >> 20, 1, ' ');
    text.add(" MiB");
}

template <u32 Capacity>
inline void MonitorUi::add_memory_kernel(const MonitorSnapshot& snapshot, Text<Capacity>& text) {
    text.add("kernel ");
    add_human(text, snapshot.heap_used);
    text.add('/');
    add_human(text, snapshot.heap_size);
    text.add("     free ");
    add_human(text, snapshot.physical_free);
    text.add("     code ");
    text.add_padded(snapshot.code_used >> 10, 1, ' ');
    text.add('/');
    text.add_padded(snapshot.code_total >> 10, 1, ' ');
    text.add(" KiB");
}

inline void MonitorUi::render_memory(MonitorGrid& grid, const MonitorSnapshot& snapshot,
                                     i32 chip_row, i32 kernel_row) const {
    put_text(grid, chip_row, 0, "MEM", monitor_style(MonitorStyle::Title));
    Text<80> chip;
    add_memory_chip(snapshot, chip);
    put_text(grid, chip_row, 5, chip.text, monitor_style(MonitorStyle::Normal));

    Text<96> kernel;
    add_memory_kernel(snapshot, kernel);
    put_text(grid, kernel_row, 5, kernel.text, monitor_style(MonitorStyle::Normal));
}

inline void MonitorUi::render_header(MonitorGrid& grid, const MonitorSnapshot& snapshot,
                                     i32 row) const {
    switch (mode_) {
    case Mode::Main:
        put_text(grid, row, 0, "TASKS", monitor_style(MonitorStyle::Title));
        put_text(grid, row, 24, "state  pri  cpu%", monitor_style(MonitorStyle::Normal));
        break;
    case Mode::Browser: {
        Text<272> header;
        header.add("BROWSE  ");
        header.add(browser_path_[0] != '\0' ? browser_path_ : "partitions");
        put_text(grid, row, 0, header.text, monitor_style(MonitorStyle::Title));
        break;
    }
    case Mode::Inspect: {
        Text<64> header;
        header.add("INSPECT  ");
        const MonitorTask* task = find_task(snapshot, inspect_addr_);
        header.add(task != nullptr ? task->name : "(gone)");
        put_text(grid, row, 0, header.text, monitor_style(MonitorStyle::Title));
        break;
    }
    }
}

// The first row a list shows, moved no further than it takes to keep the
// selection in view.
inline u32 MonitorUi::scrolled(u32 scroll, u32 index, u32 total, u32 visible) {
    if (total <= visible) {
        return 0;
    }
    if (index < scroll) {
        scroll = index;
    } else if (index >= scroll + visible) {
        scroll = index - visible + 1;
    }
    return scroll > total - visible ? total - visible : scroll;
}

inline void MonitorUi::render_tasks(const MonitorSnapshot& snapshot, MonitorGrid& grid,
                                    i32 first, i32 count) {
    if (count <= 0) {
        return;
    }
    const u32 visible = static_cast<u32>(count);
    const u32 total = task_count(snapshot);
    const u32 scroll = scrolled(task_scroll_, task_index_, total, visible);
    task_scroll_ = scroll;
    for (u32 i = 0; i < visible && scroll + i < total; ++i) {
        render_task_row(snapshot.tasks[scroll + i], grid, first + static_cast<i32>(i),
                        scroll + i == task_index_);
    }
}

inline void MonitorUi::render_task_row(const MonitorTask& task, MonitorGrid& grid, i32 row,
                                       bool selected) const {
    const u8 row_style =
        monitor_style(selected ? MonitorStyle::Selected : MonitorStyle::Normal);
    fill_row(grid, row, 0, kTextColumns - 1, row_style);
    if (selected) {
        put_cell(grid, row, 0, '>', row_style);
    }
    put_field(grid, row, 2, task.name, 20, row_style);
    if (task.forbid) {
        put_cell(grid, row, 22, 'F', monitor_style(MonitorStyle::Warning));
    }
    if (task.disable) {
        put_cell(grid, row, 23, 'D', monitor_style(MonitorStyle::Warning));
    }
    put_field(grid, row, 24, state_text(task), 5, row_style);
    Text<8> priority;
    priority.add_signed(task.priority, 3, ' ');
    put_field(grid, row, 31, priority.text, 3, row_style);
    Text<8> cpu;
    add_cpu(cpu, task.cpu_permille);
    put_field(grid, row, 36, cpu.text, 5, row_style);
}

inline void MonitorUi::render_browser(const MonitorSnapshot& snapshot, MonitorGrid& grid,
                                      i32 first, i32 count) {
    if (count <= 0) {
        return;
    }
    const u32 visible = static_cast<u32>(count);
    const u32 total = entry_count(snapshot);
    const u32 scroll = scrolled(browser_scroll_, browser_index_, total, visible);
    browser_scroll_ = scroll;
    if (total == 0) {
        put_text(grid, first, 0,
                 snapshot.listing.loading ? "loading..." : "(empty)",
                 monitor_style(MonitorStyle::Dim));
        return;
    }
    for (u32 i = 0; i < visible && scroll + i < total; ++i) {
        const MonitorEntry& entry = snapshot.listing.entries[scroll + i];
        const bool selected = scroll + i == browser_index_;
        const u8 row_style = monitor_style(
            selected ? MonitorStyle::Selected
                     : (entry.is_dir ? MonitorStyle::Normal : MonitorStyle::Dim));
        const i32 row = first + static_cast<i32>(i);
        fill_row(grid, row, 0, kTextColumns - 1, row_style);
        if (selected) {
            put_cell(grid, row, 0, '>', row_style);
        }
        put_field(grid, row, 2, entry.name, kTextColumns - 2, row_style);
    }
}

inline void MonitorUi::render_inspect(const MonitorSnapshot& snapshot, MonitorGrid& grid,
                                      i32 first, i32 count) const {
    if (count <= 0) {
        return;
    }
    const MonitorInspect& inspect = snapshot.inspect;
    if (!inspect.valid || inspect.task_addr != inspect_addr_) {
        put_text(grid, first, 0, "waiting for registers", monitor_style(MonitorStyle::Dim));
        return;
    }
    const i32 limit = first + count;
    i32 row = first;
    for (u32 group = 0; group < 4 && row < limit; ++group, ++row) {
        const bool address = group >= 2;
        const u32 base = (group & 1) * 4;
        Text<64> line;
        for (u32 i = 0; i < 4; ++i) {
            const u32 index = base + i;
            line.add(address ? 'A' : 'D');
            line.add(static_cast<char>('0' + index));
            line.add(' ');
            line.add_hex(address ? inspect.a[index] : inspect.d[index], 8);
            if (i != 3) {
                line.add("  ");
            }
        }
        put_text(grid, row, 0, line.text, monitor_style(MonitorStyle::Normal));
    }
    if (row < limit) {
        Text<32> line;
        line.add("PC ");
        if (!inspect.pc_exact) {
            line.add('~');
        }
        line.add_hex(inspect.pc, 8);
        line.add("  SR ");
        line.add_hex(inspect.sr, 4);
        put_text(grid, row, 0, line.text, monitor_style(MonitorStyle::Normal));
        ++row;
    }
    if (row < limit) {
        Text<48> line;
        line.add("Native: ");
        line.add(inspect.native[0] != '\0' ? inspect.native : "-");
        put_text(grid, row, 0, line.text, monitor_style(MonitorStyle::Normal));
        ++row;
    }
    if (inspect.has_x86 && row < limit) {
        Text<64> line;
        line.add("x86 RIP ");
        line.add_hex(inspect.rip, 16);
        line.add("  RSP ");
        line.add_hex(inspect.rsp, 16);
        put_text(grid, row, 0, line.text, monitor_style(MonitorStyle::Normal));
        ++row;
    }
    if (inspect.has_x86 && row < limit) {
        Text<32> line;
        line.add("    RFLAGS ");
        line.add_hex(inspect.rflags, 16);
        put_text(grid, row, 0, line.text, monitor_style(MonitorStyle::Normal));
    }
}

inline void MonitorUi::render_drives(MonitorGrid& grid, const MonitorSnapshot& snapshot,
                                     i32 first_row) const {
    for (u32 i = 0; i < MonitorSnapshot::kDriveCount; ++i) {
        const i32 row = first_row + static_cast<i32>(i / 2);
        const u32 column = (i % 2) * 19;
        const u8 row_style =
            monitor_style(i == drive_index_ ? MonitorStyle::Selected : MonitorStyle::Normal);
        Text<40> line;
        line.add('D');
        line.add('F');
        line.add(static_cast<char>('0' + i));
        line.add(' ');
        line.add(snapshot.drives[i].inserted ? snapshot.drives[i].volume : "(empty)");
        put_field(grid, row, column, line.text, 19, row_style);
    }
}

inline void MonitorUi::render_status(MonitorGrid& grid, const MonitorSnapshot& snapshot,
                                     i32 row) const {
    Text<96> line;
    u8 row_style = monitor_style(MonitorStyle::Dim);
    switch (confirm_) {
    case Confirm::Suspend:
        line.add("Confirm suspend of ");
        add_task_name(snapshot, line, confirm_addr_);
        break;
    case Confirm::End:
        line.add("Confirm end of ");
        add_task_name(snapshot, line, confirm_addr_);
        break;
    case Confirm::Reboot:
        line.add("Confirm reboot");
        break;
    case Confirm::Eject:
        line.add("Confirm eject of DF");
        line.add_padded(confirm_drive_, 1, ' ');
        break;
    case Confirm::None:
        break;
    }
    if (confirm_ != Confirm::None) {
        line.add(" - Y yes, any other key no");
        row_style = monitor_style(MonitorStyle::Warning);
    } else if (mode_ == Mode::Browser && snapshot.listing.error[0] != '\0') {
        line.add(snapshot.listing.error);
        row_style = monitor_style(MonitorStyle::Warning);
    } else if (mode_ == Mode::Browser && snapshot.listing.loading) {
        line.add("loading...");
    } else {
        line.add(snapshot.status);
    }
    fill_row(grid, row, 0, kTextColumns - 1, row_style);
    put_text(grid, row, 0, line.text, row_style);
}

inline void MonitorUi::render_help(MonitorGrid& grid, const MonitorSnapshot& snapshot,
                                   i32 row) const {
    Text<96> line;
    u8 row_style = monitor_style(MonitorStyle::Dim);
    if (confirm_ != Confirm::None) {
        line.add("Y confirm  any other key cancel");
        row_style = monitor_style(MonitorStyle::Warning);
    } else if (mode_ == Mode::Main) {
        if (focus_ == Focus::Tasks) {
            const MonitorTask* task = find_task(snapshot, selected_addr_);
            line.add("Tab drives  Up/Down select  Enter inspect  S ");
            line.add(task != nullptr && task->suspended ? "resume" : "suspend");
            line.add("  K end  R reboot");
        } else {
            line.add("Tab tasks  Left/Right/Up/Down select  Enter browse  E eject");
        }
    } else if (mode_ == Mode::Browser) {
        line.add("Up/Down select  Enter open  Backspace parent  Esc back");
    } else {
        line.add("Esc back");
    }
    fill_row(grid, row, 0, kTextColumns - 1, row_style);
    put_text(grid, row, 0, line.text, row_style);
}

template <u32 Capacity>
inline void MonitorUi::copy_into(char (&destination)[Capacity], const char* source) {
    u32 i = 0;
    for (; source[i] != '\0' && i + 1 < Capacity; ++i) {
        destination[i] = source[i];
    }
    destination[i] = '\0';
}

template <u32 Capacity>
inline void MonitorUi::add_confirm_question(const MonitorSnapshot& snapshot,
                                            Text<Capacity>& text) {
    switch (confirm_) {
    case Confirm::Suspend:
        text.add("Confirm suspend of ");
        add_task_name(snapshot, text, confirm_addr_);
        break;
    case Confirm::End:
        text.add("Confirm end of ");
        add_task_name(snapshot, text, confirm_addr_);
        break;
    case Confirm::Reboot:
        text.add("Confirm reboot");
        break;
    case Confirm::Eject:
        text.add("Confirm eject of DF");
        text.add_padded(confirm_drive_, 1, ' ');
        break;
    case Confirm::None:
        break;
    }
}

inline void MonitorUi::add_task_fields(const MonitorTask& task, Text<80>& text) {
    text.add("  ");
    text.add_field(task.name, 20);
    text.add(task.forbid ? 'F' : ' ');
    text.add(task.disable ? 'D' : ' ');
    text.add_field(state_text(task), 5);
    text.add(' ');
    text.add(' ');
    Text<8> priority;
    priority.add_signed(task.priority, 3, ' ');
    text.add_field(priority.text, 3);
    text.add(' ');
    text.add(' ');
    Text<8> cpu;
    add_cpu(cpu, task.cpu_permille);
    text.add_field(cpu.text, 5);
}

inline MonitorPart* MonitorUi::add_part(MonitorView& view, MonitorPartKind kind,
                                        const Rect& box) {
    if (view.count >= MonitorView::kMaxParts) {
        return nullptr;
    }
    MonitorPart* part = &view.parts[view.count++];
    *part = MonitorPart{};
    part->kind = kind;
    part->box = box;
    return part;
}

inline void MonitorUi::add_group(MonitorView& view, const Rect& box, const char* title) {
    if (MonitorPart* part = add_part(view, MonitorPartKind::Group, box)) {
        copy_into(part->text, title);
    }
}

inline void MonitorUi::add_text_part(MonitorView& view, const Rect& box, const char* text,
                                     bool warning) {
    if (MonitorPart* part = add_part(view, MonitorPartKind::Text, box)) {
        copy_into(part->text, text);
        part->warning = warning;
    }
}

inline void MonitorUi::add_button(MonitorView& view, const Rect& box, const char* label,
                                  MonitorAction action, bool disabled) {
    if (MonitorPart* part = add_part(view, MonitorPartKind::Button, box)) {
        part->action = action;
        part->disabled = disabled;
        copy_into(part->text, label);
    }
}

inline void MonitorUi::add_scroller(MonitorView& view, const Rect& box, u32 total, u32 scroll) {
    const u32 visible = list_rows();
    const i32 track = box.y1 - box.y0 + 1;
    i32 knob_height = track;
    if (total > visible) {
        knob_height = static_cast<i32>((static_cast<u64>(track) * visible) / total);
        if (knob_height < 8) {
            knob_height = 8;
        }
    }
    const u32 max = total > visible ? total - visible : 0;
    i32 knob_top = box.y0;
    if (max != 0) {
        const i32 travel = track - knob_height;
        knob_top += static_cast<i32>((static_cast<u64>(scroll) * static_cast<u32>(travel)) / max);
    }
    if (MonitorPart* part = add_part(view, MonitorPartKind::Scroller, box)) {
        part->action = MonitorAction::Scroll;
        part->knob = Rect{box.x0, knob_top, box.x1, knob_top + knob_height - 1};
    }
}

inline void MonitorUi::add_drive(MonitorView& view, u32 drive, const MonitorSnapshot& snapshot) {
    const i32 x = content_left() + static_cast<i32>(drive) * (kDriveWidth + kDriveGap);
    const Rect box{x, drives_top(), x + kDriveWidth - 1, drives_top() + kDriveHeight - 1};
    Text<40> line;
    line.add('D');
    line.add('F');
    line.add(static_cast<char>('0' + drive));
    line.add(' ');
    line.add(snapshot.drives[drive].inserted ? snapshot.drives[drive].volume : "(empty)");
    if (MonitorPart* part = add_part(view, MonitorPartKind::Drive, box)) {
        part->action = MonitorAction::SelectDrive;
        part->index = static_cast<u16>(drive);
        part->selected = drive == drive_index_;
        copy_into(part->text, line.text);
    }
}

inline void MonitorUi::clamp_scroll(u32& scroll, u32 total, u32 visible) {
    const u32 max = total > visible ? total - visible : 0;
    if (scroll > max) {
        scroll = max;
    }
}

inline void MonitorUi::scroll_by(u32& scroll, i64 delta, u32 total) {
    const u32 visible = list_rows();
    const u32 max = total > visible ? total - visible : 0;
    i64 value = static_cast<i64>(scroll) + delta;
    if (value < 0) {
        value = 0;
    }
    if (value > static_cast<i64>(max)) {
        value = static_cast<i64>(max);
    }
    scroll = static_cast<u32>(value);
}

inline void MonitorUi::layout(const MonitorSnapshot& snapshot, MonitorView& view) {
    sync(snapshot);
    const u32 visible = list_rows();
    const u32 tasks = task_count(snapshot);
    const u32 entries = entry_count(snapshot);
    clamp_scroll(task_scroll_, tasks, visible);
    clamp_scroll(browser_scroll_, entries, visible);
    if (follow_selection_) {
        if (mode_ == Mode::Main) {
            task_scroll_ = scrolled(task_scroll_, task_index_, tasks, visible);
        } else if (mode_ == Mode::Browser) {
            browser_scroll_ = scrolled(browser_scroll_, browser_index_, entries, visible);
        }
        follow_selection_ = false;
    }

    view.count = 0;
    const Rect window = window_box();
    if (MonitorPart* part = add_part(view, MonitorPartKind::Window, window)) {
        copy_into(part->text, "System Monitor");
    }
    const Rect close = chrome::close_button_box();
    if (MonitorPart* part =
            add_part(view, MonitorPartKind::Button,
                     Rect{window.x0 + close.x0, window.y0 + close.y0, window.x0 + close.x1,
                          window.y0 + close.y1})) {
        part->action = MonitorAction::Close;
    }

    const u64 seconds = snapshot.uptime_us / 1000000;
    Text<32> uptime;
    uptime.add("up ");
    uptime.add_padded(seconds / 3600, 2, '0');
    uptime.add(':');
    uptime.add_padded((seconds / 60) % 60, 2, '0');
    uptime.add(':');
    uptime.add_padded(seconds % 60, 2, '0');
    const i32 uptime_x1 = window.x1 - kWindowMargin;
    const i32 uptime_x0 =
        uptime_x1 - static_cast<i32>(uptime.length) * chrome::Metrics{}.font_width + 1;
    add_text_part(view, Rect{uptime_x0, window.y0, uptime_x1,
                             window.y0 + chrome::Metrics{}.bar_height() - 1},
                  uptime.text, false);

    const Rect cpu_box{content_left(), content_top(), content_right(),
                       content_top() + kCpuGroupHeight - 1};
    add_group(view, cpu_box, "CPU");
    const u32 cores = snapshot.cpu_count < MonitorSnapshot::kMaxCpus ? snapshot.cpu_count
                                                                     : MonitorSnapshot::kMaxCpus;
    const u32 cells = static_cast<u32>(kCpuColumns) * static_cast<u32>(kCpuRows);
    const u32 shown = cores > cells ? cells - 1 : cores;
    for (u32 i = 0; i < shown; ++i) {
        const i32 x = content_left() + static_cast<i32>(i % kCpuColumns) * kCpuCellWidth;
        const i32 y = cpu_box.y0 + 14 + static_cast<i32>(i / kCpuColumns) * kCpuCellHeight;
        const MonitorCpu& cpu = snapshot.cpus[i];
        if (!cpu.started) {
            add_text_part(view, Rect{x, y, x + kCpuCellWidth - 1, y + kCpuGaugeHeight - 1}, "off",
                          false);
            continue;
        }
        const u32 permille = cpu.busy_permille > 1000 ? 1000 : cpu.busy_permille;
        if (MonitorPart* gauge =
                add_part(view, MonitorPartKind::Gauge,
                         Rect{x, y, x + kCpuGaugeWidth - 1, y + kCpuGaugeHeight - 1})) {
            gauge->permille = permille;
        }
        Text<8> percent;
        percent.add_padded(permille / 10, 2, ' ');
        percent.add('%');
        add_text_part(view,
                      Rect{x + kCpuLabelOffset, y, x + kCpuCellWidth - 1,
                           y + kCpuGaugeHeight - 1},
                      percent.text, false);
    }
    if (cores > cells) {
        const i32 x = content_left() + static_cast<i32>(shown % kCpuColumns) * kCpuCellWidth;
        const i32 y = cpu_box.y0 + 14 + static_cast<i32>(shown / kCpuColumns) * kCpuCellHeight;
        Text<16> more;
        more.add('+');
        more.add_padded(cores - shown, 1, ' ');
        add_text_part(view, Rect{x, y, x + kCpuCellWidth - 1, y + kCpuGaugeHeight - 1}, more.text,
                      false);
    }

    const i32 memory_top = cpu_box.y1 + 1 + kGroupGap;
    const Rect memory_box{content_left(), memory_top, content_right(),
                          memory_top + kMemoryGroupHeight - 1};
    add_group(view, memory_box, "MEM");
    Text<80> chip;
    add_memory_chip(snapshot, chip);
    Text<96> kernel;
    add_memory_kernel(snapshot, kernel);
    add_text_part(view,
                  Rect{memory_box.x0 + 8, memory_box.y0 + 12, content_right() - 4,
                       memory_box.y0 + 21},
                  chip.text, false);
    add_text_part(view,
                  Rect{memory_box.x0 + 8, memory_box.y0 + 23, content_right() - 4,
                       memory_box.y0 + 32},
                  kernel.text, false);

    Text<272> header;
    switch (mode_) {
    case Mode::Main:
        header.add("TASKS");
        while (header.length < 24) {
            header.add(' ');
        }
        header.add("state  pri  cpu%");
        break;
    case Mode::Browser:
        header.add("BROWSE  ");
        header.add(browser_path_[0] != '\0' ? browser_path_ : "partitions");
        break;
    case Mode::Inspect: {
        header.add("INSPECT  ");
        const MonitorTask* task = find_task(snapshot, inspect_addr_);
        header.add(task != nullptr ? task->name : "(gone)");
        break;
    }
    }
    add_text_part(view,
                  Rect{content_left(), header_top(), content_right(),
                       header_top() + kTextHeight - 1},
                  header.text, false);

    u32 total = 0;
    u32 scroll = 0;
    switch (mode_) {
    case Mode::Main:
        total = tasks;
        scroll = task_scroll_;
        for (u32 i = 0; i < visible && scroll + i < tasks; ++i) {
            const MonitorTask& task = snapshot.tasks[scroll + i];
            const bool selected = scroll + i == task_index_;
            if (MonitorPart* part = add_part(view, MonitorPartKind::Row, list_row_box(i))) {
                part->action = MonitorAction::SelectTask;
                part->index = static_cast<u16>(scroll + i);
                part->selected = selected;
                part->warning = task.forbid || task.disable;
                Text<80> text;
                add_task_fields(task, text);
                copy_into(part->text, text.text);
            }
        }
        break;
    case Mode::Browser:
        total = entries;
        scroll = browser_scroll_;
        if (entries == 0) {
            add_text_part(view, list_row_box(0),
                          snapshot.listing.loading ? "loading..." : "(empty)", false);
        }
        for (u32 i = 0; i < visible && scroll + i < entries; ++i) {
            const MonitorEntry& entry = snapshot.listing.entries[scroll + i];
            const bool selected = scroll + i == browser_index_;
            if (MonitorPart* part = add_part(view, MonitorPartKind::Row, list_row_box(i))) {
                part->action = MonitorAction::SelectEntry;
                part->index = static_cast<u16>(scroll + i);
                part->selected = selected;
                Text<80> text;
                text.add("  ");
                text.add(entry.name);
                copy_into(part->text, text.text);
            }
        }
        break;
    case Mode::Inspect: {
        total = visible;
        const MonitorInspect& inspect = snapshot.inspect;
        u32 line = 0;
        if (!inspect.valid || inspect.task_addr != inspect_addr_) {
            add_text_part(view, list_row_box(line), "waiting for registers", false);
        } else {
            for (u32 group = 0; group < 4 && line < visible; ++group, ++line) {
                const bool address = group >= 2;
                const u32 base = (group & 1) * 4;
                Text<64> registers;
                for (u32 i = 0; i < 4; ++i) {
                    const u32 index = base + i;
                    registers.add(address ? 'A' : 'D');
                    registers.add(static_cast<char>('0' + index));
                    registers.add(' ');
                    registers.add_hex(address ? inspect.a[index] : inspect.d[index], 8);
                    if (i != 3) {
                        registers.add("  ");
                    }
                }
                add_text_part(view, list_row_box(line), registers.text, false);
            }
            if (line < visible) {
                Text<32> registers;
                registers.add("PC ");
                if (!inspect.pc_exact) {
                    registers.add('~');
                }
                registers.add_hex(inspect.pc, 8);
                registers.add("  SR ");
                registers.add_hex(inspect.sr, 4);
                add_text_part(view, list_row_box(line), registers.text, false);
                ++line;
            }
            if (line < visible) {
                Text<48> registers;
                registers.add("Native: ");
                registers.add(inspect.native[0] != '\0' ? inspect.native : "-");
                add_text_part(view, list_row_box(line), registers.text, false);
                ++line;
            }
            if (inspect.has_x86 && line < visible) {
                Text<64> registers;
                registers.add("x86 RIP ");
                registers.add_hex(inspect.rip, 16);
                registers.add("  RSP ");
                registers.add_hex(inspect.rsp, 16);
                add_text_part(view, list_row_box(line), registers.text, false);
                ++line;
            }
            if (inspect.has_x86 && line < visible) {
                Text<32> registers;
                registers.add("    RFLAGS ");
                registers.add_hex(inspect.rflags, 16);
                add_text_part(view, list_row_box(line), registers.text, false);
            }
        }
        break;
    }
    }
    add_scroller(view,
                 Rect{content_right() - kScrollerWidth + 1, rows_top(), content_right(),
                      rows_bottom()},
                 total, scroll);

    const i32 buttons_y = buttons_top();
    const i32 buttons_y1 = buttons_y + kButtonHeight - 1;
    switch (mode_) {
    case Mode::Main: {
        const MonitorTask* selected = tasks != 0 ? &snapshot.tasks[task_index_] : nullptr;
        add_button(view,
                   Rect{button_left(0), buttons_y, button_left(0) + kButtonWidth - 1, buttons_y1},
                   "Inspect", MonitorAction::Inspect, tasks == 0);
        add_button(view,
                   Rect{button_left(1), buttons_y, button_left(1) + kButtonWidth - 1, buttons_y1},
                   selected != nullptr && selected->suspended ? "Resume" : "Suspend",
                   MonitorAction::Suspend, tasks == 0);
        add_button(view,
                   Rect{button_left(2), buttons_y, button_left(2) + kButtonWidth - 1, buttons_y1},
                   "End", MonitorAction::End, tasks == 0);
        add_button(view,
                   Rect{button_left(3), buttons_y, button_left(3) + kButtonWidth - 1, buttons_y1},
                   "Reboot", MonitorAction::Reboot, false);
        add_button(view,
                   Rect{button_left(4), buttons_y, button_left(4) + kButtonWidth - 1, buttons_y1},
                   "Insert", MonitorAction::Insert, false);
        add_button(view,
                   Rect{button_left(5), buttons_y, button_left(5) + kButtonWidth - 1, buttons_y1},
                   "Eject", MonitorAction::Eject,
                   !snapshot.drives[drive_index_].inserted);
        break;
    }
    case Mode::Browser:
        add_button(view,
                   Rect{button_left(0), buttons_y, button_left(0) + kButtonWidth - 1, buttons_y1},
                   "Open", MonitorAction::Open, entries == 0);
        add_button(view,
                   Rect{button_left(1), buttons_y, button_left(1) + kButtonWidth - 1, buttons_y1},
                   "Up", MonitorAction::Up, browser_path_[0] == '\0');
        add_button(view,
                   Rect{button_left(2), buttons_y, button_left(2) + kButtonWidth - 1, buttons_y1},
                   "Cancel", MonitorAction::Cancel, false);
        break;
    case Mode::Inspect:
        add_button(view,
                   Rect{button_left(0), buttons_y, button_left(0) + kButtonWidth - 1, buttons_y1},
                   "Back", MonitorAction::Back, false);
        break;
    }

    for (u32 i = 0; i < MonitorSnapshot::kDriveCount; ++i) {
        add_drive(view, i, snapshot);
    }

    Text<96> status;
    bool warning = false;
    if (mode_ == Mode::Browser && snapshot.listing.error[0] != '\0') {
        status.add(snapshot.listing.error);
        warning = true;
    } else if (mode_ == Mode::Browser && snapshot.listing.loading) {
        status.add("loading...");
    } else {
        status.add(snapshot.status);
    }
    add_text_part(view,
                  Rect{content_left(), status_top(), content_right(),
                       status_top() + kTextHeight - 1},
                  status.text, warning);

    if (confirm_ != Confirm::None) {
        const i32 left = static_cast<i32>(kMonitorWidth) / 2 - kDialogWidth / 2;
        const i32 top = static_cast<i32>(kMonitorHeight) / 2 - kDialogHeight / 2;
        const Rect dialog{left, top, left + kDialogWidth - 1, top + kDialogHeight - 1};
        Text<96> question;
        add_confirm_question(snapshot, question);
        if (MonitorPart* part = add_part(view, MonitorPartKind::Dialog, dialog)) {
            copy_into(part->text, question.text);
        }
        const i32 button_y1 = dialog.y1 - 10;
        const i32 button_y0 = button_y1 - kDialogButtonHeight + 1;
        add_button(view,
                   Rect{dialog.x0 + 16, button_y0, dialog.x0 + 16 + kDialogButtonWidth - 1,
                        button_y1},
                   "OK", MonitorAction::Confirm, false);
        add_button(view,
                   Rect{dialog.x1 - 16 - kDialogButtonWidth + 1, button_y0, dialog.x1 - 16,
                        button_y1},
                   "Cancel", MonitorAction::Dismiss, false);
    }

    if (mouse_pressed_ && (mouse_buttons_ & 1) != 0) {
        for (u32 i = 0; i < view.count; ++i) {
            MonitorPart& part = view.parts[i];
            if (part.action == press_action_ && part.index == press_index_ &&
                mouse_x_ >= part.box.x0 && mouse_x_ <= part.box.x1 && mouse_y_ >= part.box.y0 &&
                mouse_y_ <= part.box.y1) {
                part.pressed = true;
            }
        }
    }
}

inline void MonitorUi::start_mouse(i32 x, i32 y, u8 buttons) {
    mouse_x_ = x;
    mouse_y_ = y;
    mouse_buttons_ = buttons;
    mouse_pressed_ = false;
    press_action_ = MonitorAction::None;
    press_index_ = 0;
    scroll_drag_ = false;
    drag_offset_ = 0;
    click_valid_ = false;
    click_action_ = MonitorAction::None;
    click_key_ = 0;
    click_us_ = 0;
}

inline MonitorRequest MonitorUi::mouse(const MonitorMouse& mouse_state,
                                       const MonitorSnapshot& snapshot,
                                       const MonitorView& view) {
    sync(snapshot);
    const bool was_left = (mouse_buttons_ & 1) != 0;
    const bool left = (mouse_state.buttons & 1) != 0;
    mouse_x_ = mouse_state.x;
    mouse_y_ = mouse_state.y;
    MonitorRequest request;
    if (was_left && left && scroll_drag_) {
        drag_scroller(mouse_state.y, snapshot, view);
    }
    if (mouse_state.wheel != 0 && !scroll_drag_) {
        scroll_by_wheel(mouse_state.wheel, snapshot);
    }
    if (left && !was_left) {
        request = mouse_press(mouse_state, snapshot, view);
    } else if (!left && was_left) {
        request = mouse_release(mouse_state, snapshot, view);
    }
    mouse_buttons_ = mouse_state.buttons;
    return request;
}

inline MonitorRequest MonitorUi::mouse_press(const MonitorMouse& mouse_state,
                                             const MonitorSnapshot& snapshot,
                                             const MonitorView& view) {
    mouse_pressed_ = false;
    scroll_drag_ = false;
    const MonitorPart* part = view.at(mouse_state.x, mouse_state.y);
    if (part == nullptr || part->disabled) {
        click_valid_ = false;
        return MonitorRequest{};
    }
    switch (part->action) {
    case MonitorAction::SelectTask:
    case MonitorAction::SelectEntry:
    case MonitorAction::SelectDrive:
        return press_indexed(mouse_state, part->action, part->index, snapshot);
    case MonitorAction::Scroll:
        click_valid_ = false;
        press_scroller(mouse_state, snapshot, *part);
        return MonitorRequest{};
    default:
        click_valid_ = false;
        mouse_pressed_ = true;
        press_action_ = part->action;
        press_index_ = part->index;
        return MonitorRequest{};
    }
}

inline MonitorRequest MonitorUi::press_indexed(const MonitorMouse& mouse_state,
                                               MonitorAction action, u32 index,
                                               const MonitorSnapshot& snapshot) {
    // What a second click must hit again: a task keeps its address when the
    // list moves under the pointer, the other rows have only their place.
    u32 clicked = index;
    switch (action) {
    case MonitorAction::SelectTask:
        if (index >= task_count(snapshot)) {
            return MonitorRequest{};
        }
        clicked = snapshot.tasks[index].task_addr;
        task_index_ = index;
        selected_addr_ = snapshot.tasks[index].task_addr;
        has_selection_ = true;
        focus_ = Focus::Tasks;
        break;
    case MonitorAction::SelectEntry:
        if (index >= entry_count(snapshot)) {
            return MonitorRequest{};
        }
        browser_index_ = index;
        focus_ = Focus::Drives;
        break;
    case MonitorAction::SelectDrive:
        if (index >= MonitorSnapshot::kDriveCount) {
            return MonitorRequest{};
        }
        drive_index_ = index;
        focus_ = Focus::Drives;
        break;
    default:
        return MonitorRequest{};
    }
    const bool double_click =
        click_valid_ && click_action_ == action && click_key_ == clicked &&
        mouse_state.now_us - click_us_ <= kDoubleClickUs;
    if (!double_click) {
        click_valid_ = true;
        click_action_ = action;
        click_key_ = clicked;
        click_us_ = mouse_state.now_us;
        return MonitorRequest{};
    }
    click_valid_ = false;
    switch (action) {
    case MonitorAction::SelectTask:
        return inspect_selected(snapshot);
    case MonitorAction::SelectDrive:
        return open_browser();
    default:
        return browser_enter(snapshot);
    }
}

inline void MonitorUi::press_scroller(const MonitorMouse& mouse_state,
                                      const MonitorSnapshot& snapshot,
                                      const MonitorPart& part) {
    u32* scroll = nullptr;
    u32 total = 0;
    if (mode_ == Mode::Main) {
        scroll = &task_scroll_;
        total = task_count(snapshot);
    } else if (mode_ == Mode::Browser) {
        scroll = &browser_scroll_;
        total = entry_count(snapshot);
    }
    if (scroll == nullptr) {
        return;
    }
    if (mouse_state.y >= part.knob.y0 && mouse_state.y <= part.knob.y1) {
        scroll_drag_ = true;
        drag_offset_ = mouse_state.y - part.knob.y0;
        return;
    }
    const i64 page = static_cast<i64>(list_rows());
    scroll_by(*scroll, mouse_state.y < part.knob.y0 ? -page : page, total);
}

inline void MonitorUi::drag_scroller(i32 y, const MonitorSnapshot& snapshot,
                                     const MonitorView& view) {
    const MonitorPart* part = view.find(MonitorAction::Scroll);
    u32* scroll = nullptr;
    u32 total = 0;
    if (mode_ == Mode::Main) {
        scroll = &task_scroll_;
        total = task_count(snapshot);
    } else if (mode_ == Mode::Browser) {
        scroll = &browser_scroll_;
        total = entry_count(snapshot);
    }
    if (part == nullptr || scroll == nullptr) {
        scroll_drag_ = false;
        return;
    }
    const u32 max = total > list_rows() ? total - list_rows() : 0;
    const i32 track = part->box.y1 - part->box.y0 + 1;
    const i32 knob = part->knob.y1 - part->knob.y0 + 1;
    const i32 travel = track - knob;
    if (max == 0 || travel <= 0) {
        *scroll = 0;
        return;
    }
    const i32 lowest = part->box.y1 - knob + 1;
    i32 top = y - drag_offset_;
    if (top < part->box.y0) {
        top = part->box.y0;
    }
    if (top > lowest) {
        top = lowest;
    }
    const u32 offset = static_cast<u32>(top - part->box.y0);
    *scroll = static_cast<u32>((static_cast<u64>(offset) * max) / static_cast<u32>(travel));
}

inline void MonitorUi::scroll_by_wheel(i32 wheel, const MonitorSnapshot& snapshot) {
    u32* scroll = nullptr;
    u32 total = 0;
    if (mode_ == Mode::Main) {
        scroll = &task_scroll_;
        total = task_count(snapshot);
    } else if (mode_ == Mode::Browser) {
        scroll = &browser_scroll_;
        total = entry_count(snapshot);
    }
    if (scroll != nullptr) {
        scroll_by(*scroll, -static_cast<i64>(wheel) * 3, total);
    }
}

inline MonitorRequest MonitorUi::mouse_release(const MonitorMouse& mouse_state,
                                               const MonitorSnapshot& snapshot,
                                               const MonitorView& view) {
    scroll_drag_ = false;
    if (!mouse_pressed_) {
        return MonitorRequest{};
    }
    mouse_pressed_ = false;
    const MonitorAction action = press_action_;
    const u32 index = press_index_;
    press_action_ = MonitorAction::None;
    press_index_ = 0;
    const MonitorPart* part = view.at(mouse_state.x, mouse_state.y);
    if (part == nullptr || part->disabled || part->action != action || part->index != index) {
        return MonitorRequest{};
    }
    switch (action) {
    case MonitorAction::Close:
        return task_request(MonitorOp::Close, 0, 0);
    case MonitorAction::Inspect:
        return inspect_selected(snapshot);
    case MonitorAction::Suspend:
        return suspend_or_resume(snapshot);
    case MonitorAction::End:
        begin_task_confirm(Confirm::End, snapshot);
        return MonitorRequest{};
    case MonitorAction::Reboot:
        confirm_ = Confirm::Reboot;
        return MonitorRequest{};
    case MonitorAction::Insert:
        return open_browser();
    case MonitorAction::Eject:
        return eject_selected(snapshot);
    case MonitorAction::Open:
        return browser_enter(snapshot);
    case MonitorAction::Up:
        return browser_parent();
    case MonitorAction::Cancel:
        browser_cancel();
        return MonitorRequest{};
    case MonitorAction::Back:
        mode_ = Mode::Main;
        focus_ = Focus::Tasks;
        follow_selection_ = true;
        return MonitorRequest{};
    case MonitorAction::Confirm:
        return finish_confirm(Key{KeyCode::Character, 'y'});
    case MonitorAction::Dismiss:
        return finish_confirm(Key{KeyCode::Character, 'n'});
    default:
        return MonitorRequest{};
    }
}

}

