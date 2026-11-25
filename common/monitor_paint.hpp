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

#include "libs/chrome.hpp"
#include "libs/chrome_paint.hpp"
#include "libs/chrome_palette.hpp"
#include "libs/chrome_surface.hpp"
#include "monitor_view.hpp"
#include "types.hpp"

namespace hombre {

namespace monitor_look {

inline i32 paint_text_length(const char* text) {
    i32 length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

// A surface that moves everything the window chrome draws at the window's
// place in the 640x400 view.
struct OffsetSurface {
    chrome::Surface inner;
    i32 dx;
    i32 dy;

    static void fill(void* ctx, const Rect& box, u8 pen) {
        OffsetSurface& self = *static_cast<OffsetSurface*>(ctx);
        self.inner.fill(self.inner.ctx,
                        Rect{box.x0 + self.dx, box.y0 + self.dy, box.x1 + self.dx,
                             box.y1 + self.dy},
                        pen);
    }

    static u32 text_fit(void* ctx, u32 font, const u8* chars, u32 count, i32 room) {
        OffsetSurface& self = *static_cast<OffsetSurface*>(ctx);
        return self.inner.text_fit(self.inner.ctx, font, chars, count, room);
    }

    static i32 text_width(void* ctx, u32 font, const u8* chars, u32 count) {
        OffsetSurface& self = *static_cast<OffsetSurface*>(ctx);
        return self.inner.text_width(self.inner.ctx, font, chars, count);
    }

    static void text(void* ctx, u32 font, i32 x, i32 baseline, u8 pen, const u8* chars,
                     u32 count) {
        OffsetSurface& self = *static_cast<OffsetSurface*>(ctx);
        self.inner.text(self.inner.ctx, font, x + self.dx, baseline + self.dy, pen, chars, count);
    }

    chrome::Surface surface() {
        return chrome::Surface{this, inner.depth, fill, text_fit, text_width, text};
    }
};

inline void paint_left_text(const chrome::Surface& surface, const Rect& box, const char* chars,
                     u8 pen, const chrome::Metrics& metrics) {
    if (chars[0] == '\0' || box.x1 < box.x0 || box.y1 < box.y0) {
        return;
    }
    const i32 height = box.y1 - box.y0 + 1;
    if (height < metrics.font_height) {
        return;
    }
    const u8* bytes = reinterpret_cast<const u8*>(chars);
    const u32 length = static_cast<u32>(paint_text_length(chars));
    const u32 shown =
        surface.text_fit(surface.ctx, metrics.font, bytes, length, box.x1 - box.x0 + 1);
    if (shown == 0) {
        return;
    }
    const i32 baseline = box.y0 + (height - metrics.font_height) / 2 + metrics.font_baseline;
    surface.text(surface.ctx, metrics.font, box.x0, baseline, pen, bytes, shown);
}

inline void paint_window(const chrome::Surface& surface, const MonitorPart& part,
                  const MonitorView& view) {
    OffsetSurface offset{surface, part.box.x0, part.box.y0};
    chrome::WindowChrome window{};
    window.width = part.box.x1 - part.box.x0 + 1;
    window.height = part.box.y1 - part.box.y0 + 1;
    window.flags = ndk::WFLG_CLOSEGADGET | ndk::WFLG_ACTIVATE;
    window.active = true;
    window.border_left = 2;
    window.border_right = 2;
    window.border_bottom = 2;
    window.border_top = static_cast<u32>(window.metrics.border_top());
    const MonitorPart* close = view.find(MonitorAction::Close);
    if (close != nullptr && close->pressed) {
        window.pressed = chrome::BarButton::Close;
    }
    const chrome::Title title{reinterpret_cast<const u8*>(part.text),
                              static_cast<u32>(paint_text_length(part.text))};
    chrome::paint_modern_window(offset.surface(), window, title);
}

inline void paint_group(const chrome::Surface& surface, const MonitorPart& part,
                 const chrome::BoxPens& pens, const chrome::Metrics& metrics) {
    chrome::paint_box(surface, part.box, pens, true);
    paint_left_text(surface,
                    Rect{part.box.x0 + 8, part.box.y0 + 3, part.box.x1 - 8, part.box.y0 + 12},
                    part.text, chrome::kFrameDark, metrics);
}

inline void paint_text_part(const chrome::Surface& surface, const MonitorPart& part,
                     const chrome::Metrics& metrics) {
    paint_left_text(surface, part.box, part.text,
                    part.warning ? chrome::kCloseRing : chrome::kFrameDark, metrics);
}

inline void paint_row(const chrome::Surface& surface, const MonitorPart& part,
               const chrome::Metrics& metrics) {
    if (part.selected) {
        surface.fill(surface.ctx, part.box, chrome::kActiveBar4);
    }
    const u8 pen = part.selected ? chrome::kFrameLight
                                 : (part.warning ? chrome::kCloseRing : chrome::kFrameDark);
    paint_left_text(surface, part.box, part.text, pen, metrics);
}

inline void paint_gauge(const chrome::Surface& surface, const MonitorPart& part,
                 const chrome::BoxPens& pens) {
    chrome::paint_box(surface, part.box, pens, true);
    const i32 x0 = part.box.x0 + 2;
    const i32 y0 = part.box.y0 + 2;
    const i32 x1 = part.box.x1 - 2;
    const i32 y1 = part.box.y1 - 2;
    if (x1 < x0 || y1 < y0) {
        return;
    }
    const u32 permille = part.permille > 1000 ? 1000 : part.permille;
    const i32 filled =
        static_cast<i32>((static_cast<u64>(x1 - x0 + 1) * permille) / 1000);
    if (filled != 0) {
        surface.fill(surface.ctx, Rect{x0, y0, x0 + filled - 1, y1}, chrome::kActiveBar4);
    }
}

inline void paint_scroller(const chrome::Surface& surface, const MonitorPart& part) {
    chrome::PropChrome prop{};
    prop.box = part.box;
    prop.knob = part.knob;
    prop.pflags = ndk::FREEVERT;
    chrome::paint_modern_prop(surface, prop);
}

inline void paint_button(const chrome::Surface& surface, const MonitorPart& part,
                  const chrome::BoxPens& pens, const chrome::Metrics& metrics) {
    if (part.action == MonitorAction::Close) {
        return;
    }
    chrome::paint_box(surface, part.box, pens, part.pressed);
    chrome::paint_label(surface, part.box, reinterpret_cast<const u8*>(part.text),
                        static_cast<u32>(paint_text_length(part.text)),
                        part.disabled ? chrome::kTitleInactive : chrome::kFrameDark, metrics);
}

inline void paint_drive(const chrome::Surface& surface, const MonitorPart& part,
                 const chrome::BoxPens& pens, const chrome::Metrics& metrics) {
    chrome::paint_box(surface, part.box, pens, part.pressed || part.selected);
    chrome::paint_label(surface, part.box, reinterpret_cast<const u8*>(part.text),
                        static_cast<u32>(paint_text_length(part.text)), chrome::kFrameDark,
                        metrics);
}

inline void paint_dialog(const chrome::Surface& surface, const MonitorPart& part,
                  const chrome::BoxPens& pens, const chrome::Metrics& metrics) {
    chrome::paint_box(surface, part.box, pens, false);
    paint_left_text(surface,
                    Rect{part.box.x0 + 16, part.box.y0 + 10, part.box.x1 - 16,
                         part.box.y0 + 26},
                    part.text, chrome::kFrameDark, metrics);
}

} // namespace monitor_look

// Backdrop pen 0, the window chrome, then every part in array order.
inline void paint_monitor(const chrome::Surface& surface, const MonitorView& view) {
    const chrome::BoxPens pens = chrome::requester_pens(8, {});
    const chrome::Metrics metrics{};
    surface.fill(surface.ctx,
                 Rect{0, 0, static_cast<i32>(kMonitorWidth) - 1,
                      static_cast<i32>(kMonitorHeight) - 1},
                 0);
    const u32 count = view.count < MonitorView::kMaxParts ? view.count : MonitorView::kMaxParts;
    for (u32 i = 0; i < count; ++i) {
        const MonitorPart& part = view.parts[i];
        switch (part.kind) {
        case MonitorPartKind::Window:
            monitor_look::paint_window(surface, part, view);
            break;
        case MonitorPartKind::Group:
            monitor_look::paint_group(surface, part, pens, metrics);
            break;
        case MonitorPartKind::Text:
            monitor_look::paint_text_part(surface, part, metrics);
            break;
        case MonitorPartKind::Gauge:
            monitor_look::paint_gauge(surface, part, pens);
            break;
        case MonitorPartKind::Row:
            monitor_look::paint_row(surface, part, metrics);
            break;
        case MonitorPartKind::Scroller:
            monitor_look::paint_scroller(surface, part);
            break;
        case MonitorPartKind::Button:
            monitor_look::paint_button(surface, part, pens, metrics);
            break;
        case MonitorPartKind::Drive:
            monitor_look::paint_drive(surface, part, pens, metrics);
            break;
        case MonitorPartKind::Dialog:
            monitor_look::paint_dialog(surface, part, pens, metrics);
            break;
        }
    }
}

}
