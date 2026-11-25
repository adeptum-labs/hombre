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

#include "libs/chrome_palette.hpp"
#include "libs/chrome_surface.hpp"
#include "libs/raster_text.hpp"

// The system monitor draws its windows into an 8-bit bitmap in memory the
// caller owns, from a timer interrupt: an m68k::Machine over that memory
// with no translator and raster::Bitmap's deep chunky drawing. The palette
// keeps chunky::default_palette, whose table is made on first use, out of
// the interrupt.

namespace hombre {

constexpr u32 kMonitorWidth = 640;
constexpr u32 kMonitorHeight = 400;

// The colours of the pens the monitor draws in, 0xRRGGBB. The pens outside
// the chrome range stay black.
inline void monitor_palette(u32 (&rgb)[256]) {
    for (u32 i = 0; i < 256; ++i) {
        rgb[i] = 0;
    }
    rgb[0] = 0xAAAAAA;
    rgb[1] = 0x000000;
    rgb[2] = 0xFFFFFF;
    rgb[3] = 0x6688BB;
    for (u32 i = 0; i < chrome::kScreenBarSteps; ++i) {
        rgb[chrome::kScreenBar0 + i] = chrome::screen_bar_rgb(0x6688BB, i);
    }
    for (u32 i = 0; i < 4; ++i) {
        rgb[chrome::kMenuPaperPen + i] = chrome::kMenuPalette[i];
    }
    for (u32 i = 0; i < chrome::kChromeCount; ++i) {
        rgb[chrome::kChromeBase + i] = chrome::kChromePalette[i];
    }
}

// An 8-bit bitmap of kMonitorWidth x kMonitorHeight pens in memory the
// caller owns, drawn with the raster primitives and the built-in font.
class BitmapSurface {
public:
    BitmapSurface() = default;
    // The bitmap points at this object's palette.
    BitmapSurface(const BitmapSurface&) = delete;
    BitmapSurface& operator=(const BitmapSurface&) = delete;

    void init(u8* pens) {
        machine_.cpu = m68k::CpuState{};
        machine_.mem = m68k::GuestMemory{pens, kMonitorWidth * kMonitorHeight};
        machine_.jit = nullptr;

        bitmap_ = raster::Bitmap{};
        bitmap_.deep = true;
        bitmap_.fmt = PixFmt::Lut8;
        bitmap_.width = kMonitorWidth;
        bitmap_.rows = kMonitorHeight;
        bitmap_.bytes_per_row = kMonitorWidth;
        bitmap_.depth = 8;
        bitmap_.pixels = 0;
        bitmap_.palette = palette_;
        monitor_palette(palette_);

        set_clip(Rect{0, 0, static_cast<i32>(kMonitorWidth) - 1,
                      static_cast<i32>(kMonitorHeight) - 1});
    }

    void set_clip(const Rect& clip) {
        const i32 right = static_cast<i32>(kMonitorWidth) - 1;
        const i32 bottom = static_cast<i32>(kMonitorHeight) - 1;
        clip_ = raster::Clip{clip.x0 < 0 ? 0 : clip.x0, clip.y0 < 0 ? 0 : clip.y0,
                             clip.x1 > right ? right : clip.x1,
                             clip.y1 > bottom ? bottom : clip.y1};
    }

    chrome::Surface surface() {
        return chrome::Surface{this, 8, fill, text_fit, text_width, text};
    }

private:
    static constexpr i32 kGlyphWidth = static_cast<i32>(raster::kBuiltinSize);

    static void fill(void* ctx, const Rect& box, u8 pen) {
        if (box.x1 < box.x0 || box.y1 < box.y0) {
            return;
        }
        BitmapSurface& self = *static_cast<BitmapSurface*>(ctx);
        native::Bus bus(self.machine_);
        raster::fill_rect(bus, self.bitmap_, self.clip_, box, pen);
    }

    static u32 text_fit(void*, u32, const u8*, u32 count, i32 room) {
        if (room < kGlyphWidth) {
            return 0;
        }
        const u32 fits = static_cast<u32>(room) / static_cast<u32>(kGlyphWidth);
        return fits < count ? fits : count;
    }

    static i32 text_width(void*, u32, const u8*, u32 count) {
        return static_cast<i32>(count * static_cast<u32>(kGlyphWidth));
    }

    static void text(void* ctx, u32, i32 x, i32 baseline, u8 pen, const u8* chars, u32 count) {
        if (count == 0) {
            return;
        }
        BitmapSurface& self = *static_cast<BitmapSurface*>(ctx);
        native::Bus bus(self.machine_);
        const raster::Pens pens{pen, 0, static_cast<u8>(ndk::JAM1)};
        u32 advance = 0;
        raster::draw_text(bus, self.bitmap_, self.clip_, pens, raster::builtin_font(),
                          raster::TextStyle{}, x, baseline, chars, count, advance);
    }

    m68k::Machine machine_{};
    raster::Bitmap bitmap_{};
    raster::Clip clip_{};
    u32 palette_[256] = {};
};

}
