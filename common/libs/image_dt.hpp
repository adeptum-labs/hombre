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

#include "../image/decode.hpp"
#include "../m68k_cpu.hpp"
#include "../types.hpp"
#include "exec.hpp"
#include "native_class_lib.hpp"

// png.datatype and jpeg.datatype, subclasses of picture.datatype that load
// an ordinary file of their kind in their OM_NEW. exec offers them as
// datatypes/png.datatype and datatypes/jpeg.datatype at Hombre's version.
//
// The classes use only the public interface of their superclass: they fill
// the BitMapHeader PDTA_BitMapHeader gives, set PDTA_SourceMode PMODE_V43
// and hand the decoded RGBA pixels over with PDTM_WRITEPIXELARRAY. A
// DTST_RAM object is returned as the superclass made it; a DTST_FILE one is
// read through its DTA_Handle, probed and decoded in guest memory, and the
// pixels are written into the object. What is wrong with the file becomes
// the caller's IoErr, as SetIoErr leaves it, and the object is disposed of
// again with a message made before the superclass was called, so that a
// load that ran out of memory can still dispose of it.
//
// A load's allocations, its messages among them, are tracked per task, so
// that an exec task end hook gives them back when the task is removed while
// a yield has left its C++ stack behind.

namespace hombre {

class Dos;
class Intuition;

class ImageDataType {
public:
    // Registers both libraries with exec's providers and the task end hook,
    // and forgets the picture.datatype bases and the loads of an earlier
    // system. False if exec has no provider left. dos is used from the
    // first load on, after dos.init.
    bool init(m68k::Machine& m, Exec& exec, Intuition& intuition, Dos& dos);

private:
    struct Ops;

    // One library of the pair: its name, the picture.datatype base its class
    // needs, and the class itself kept by NativeClassLibrary.
    struct Library {
        ImageDataType* owner = nullptr;
        image::Format format = image::Format::Unknown;
        const char* leaf = nullptr;
        const char* id = nullptr;
        NativeClassLibrary library{};
        u32 picture = 0;
    };

    // What one task's load holds: the decoder's block in kernel memory, the
    // work, file and pixel buffers in guest memory, and the guest block the
    // load's method messages are built in. An exec task end hook frees them
    // when the task goes while a yield has abandoned its C++ stack.
    struct Load {
        ImageDataType* owner = nullptr;
        bool used = false;
        u32 generation = 0;
        u32 task = 0;
        void* host = nullptr;
        u32 host_bytes = 0;
        u32 work = 0;
        u32 work_bytes = 0;
        void* work_host = nullptr;
        u32 file = 0;
        u32 file_bytes = 0;
        u32 pixels = 0;
        u32 pixels_bytes = 0;
        u32 message = 0;
    };

    static u32 make_library(void* ctx);
    static u32 make_class(void* ctx, u32 base);
    static u32 dispatch(void* ctx, u32 cl, u32 object, u32 msg);
    static void task_ended(void* ctx, u32 task, bool self);

    void release_task(u32 task);
    // The buffers alone, or those and the message block, leaving the row
    // free.
    void release_buffers(Load& load);
    void release(Load& load);

    static constexpr u32 kMaxLoads = 8;

    m68k::Machine* machine_ = nullptr;
    Exec* exec_ = nullptr;
    Intuition* intuition_ = nullptr;
    Dos* dos_ = nullptr;
    // Each library's leaf, its class and the picture.datatype it keeps open
    // as the superclass.
    Library png_{};
    Library jpeg_{};
    Load loads_[kMaxLoads]{};
    // Tells a load's own row from one another load has taken over.
    u32 next_generation_ = 0;
};

}
