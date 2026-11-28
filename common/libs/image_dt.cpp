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

#include "image_dt.hpp"

#include "boopsi.hpp"
#include "dos.hpp"
#include "intuition.hpp"
#include "tags.hpp"

#include <new>

namespace hombre {

using m68k::Machine;
using native::Bus;
using native::LogLine;
using namespace ndk;
namespace image = hombre::image;

namespace {

constexpr char kPngLeaf[] = "png.datatype";
constexpr char kJpegLeaf[] = "jpeg.datatype";
constexpr char kDir[] = "datatypes/";
constexpr char kPngId[] = "png.datatype 60.0 (Hombre)\r\n";
constexpr char kJpegId[] = "jpeg.datatype 60.0 (Hombre)\r\n";

// Attributes (datatypes/datatypesclass.h, datatypes/pictureclass.h).
constexpr u32 DTA_Dummy = 0x80001000;
constexpr u32 DTA_Name = DTA_Dummy + 100;
constexpr u32 DTA_SourceType = DTA_Dummy + 101;
constexpr u32 DTA_Handle = DTA_Dummy + 102;
constexpr u32 DTA_ObjName = DTA_Dummy + 109;
constexpr u32 DTA_NominalVert = DTA_Dummy + 124;
constexpr u32 DTA_NominalHoriz = DTA_Dummy + 125;
constexpr u32 PDTA_ModeID = DTA_Dummy + 200;
constexpr u32 PDTA_BitMapHeader = DTA_Dummy + 201;
constexpr u32 PDTA_SourceMode = DTA_Dummy + 250;

// The source types, the interface modes and the pixel array formats.
constexpr u32 DTST_RAM = 1;
constexpr u32 DTST_FILE = 2;
constexpr u32 PMODE_V43 = 1;
constexpr u32 PBPAFMT_RGBA = 1;
constexpr u32 PDTM_WRITEPIXELARRAY = 0x660;

// The struct BitMapHeader PDTA_BitMapHeader points at.
constexpr u32 bmh_Width = 0;
constexpr u32 bmh_Height = 2;
constexpr u32 bmh_Left = 4;
constexpr u32 bmh_Top = 6;
constexpr u32 bmh_Depth = 8;
constexpr u32 bmh_Masking = 9;
constexpr u32 bmh_Compression = 10;
constexpr u32 bmh_Transparent = 12;
constexpr u32 bmh_XAspect = 14;
constexpr u32 bmh_YAspect = 15;
constexpr u32 bmh_PageWidth = 16;
constexpr u32 bmh_PageHeight = 18;
constexpr u32 mskHasAlpha = 4;

// The V42 hires and lace bits PDTA_ModeID names, as ilbm.datatype picks
// them from the picture's size.
constexpr u32 kHires = 0x8000;
constexpr u32 kLace = 0x0004;
constexpr u32 kHiresWidth = 640;
constexpr u32 kLaceHeight = 400;

// A file is read in pieces, yielding between them, and the pixels are
// scanned for translucency a mebibyte at a time.
constexpr u32 kReadBytes = 65536;
constexpr u32 kScanPixels = 262144;

constexpr u32 kMaxFileBytes = 256u * 1024u * 1024u;
constexpr u32 DTERROR_UNKNOWN_COMPRESSION = 2006;
constexpr u32 DTERROR_NOT_ENOUGH_DATA = 2007;
constexpr u32 DTERROR_INVALID_DATA = 2008;

// The guest messages a load sends, built one at a time in the block its
// row holds: OM_GET with its storage, OM_SET with up to five tags,
// PDTM_WRITEPIXELARRAY and OM_DISPOSE.
constexpr u32 kSetTags = 5;
constexpr u32 kMessageBytes = 12 + (kSetTags + 1) * 8;

// What a dos call that failed without an error leaves.
u32 dos_error(u32 error) { return error != 0 ? error : DTERROR_INVALID_DATA; }

u32 error_of(image::Status status) {
    switch (status) {
    case image::Status::NotThisFormat: return ERROR_OBJECT_WRONG_TYPE;
    case image::Status::Unsupported: return DTERROR_UNKNOWN_COMPRESSION;
    case image::Status::Truncated: return DTERROR_NOT_ENOUGH_DATA;
    case image::Status::Corrupt: return DTERROR_INVALID_DATA;
    case image::Status::TooLarge:
    case image::Status::BufferTooSmall:
    case image::Status::NoMemory: return ERROR_NO_FREE_STORE;
    case image::Status::Ok:
    case image::Status::Partial:
    case image::Status::Stopped: break;
    }
    return ERROR_OBJECT_WRONG_TYPE;
}

}

struct ImageDataType::Ops {
    // What the decoder's allocators see: the load row and the generation it
    // had when this load claimed it, so that a row another load has taken
    // over is left alone.
    struct AllocCtx {
        Load* load;
        u32 generation;
    };

    Library& l;
    ImageDataType& p;
    Bus b;
    u32 cl;
    u32 object;
    Load* load = nullptr;
    u32 generation = 0;
    u32 task = 0;
    bool claimed = false;
    AllocCtx host_ctx{};
    AllocCtx work_ctx{};

    Ops(Library& library, u32 cls, u32 obj)
        : l(library), p(*library.owner), b(*library.owner->machine_), cl(cls), object(obj) {}

    Boopsi& boopsi() { return p.intuition_->boopsi(); }
    bool stopped() const { return !b.ok() || p.machine_->cpu.exit != m68k::ExitReason::None; }
    // The task that began the load no longer runs this C++: an exec task
    // end hook gave the load's allocations back and this stack is only a
    // host context's remains.
    bool abandoned() const { return claimed && task != p.exec_->this_task(); }
    bool still_running() const { return !stopped() && !abandoned(); }

    static bool keep_going(void* ctx) {
        Ops& o = *static_cast<Ops*>(ctx);
        o.p.exec_->yield_in_native();
        return o.still_running();
    }

    static bool matches(const AllocCtx& ctx) {
        return ctx.load != nullptr && ctx.load->used && ctx.load->owner != nullptr &&
               ctx.load->generation == ctx.generation;
    }

    static void* alloc_host(void* raw, usize bytes) {
        AllocCtx& ctx = *static_cast<AllocCtx*>(raw);
        if (!matches(ctx) || ctx.load->host != nullptr || bytes == 0) {
            return nullptr;
        }
        void* block = ::operator new(bytes, std::nothrow_t{});
        if (block == nullptr) {
            return nullptr;
        }
        ctx.load->host = block;
        ctx.load->host_bytes = static_cast<u32>(bytes);
        return block;
    }

    static void release_host(void* raw, void* block, usize) {
        AllocCtx& ctx = *static_cast<AllocCtx*>(raw);
        if (!matches(ctx) || ctx.load->host != block) {
            return;
        }
        ::operator delete(block);
        ctx.load->host = nullptr;
        ctx.load->host_bytes = 0;
    }

    static void* alloc_work(void* raw, usize bytes) {
        AllocCtx& ctx = *static_cast<AllocCtx*>(raw);
        if (!matches(ctx) || ctx.load->work != 0 || bytes == 0 || bytes > 0xFFFFFFFFu) {
            return nullptr;
        }
        ImageDataType& p = *ctx.load->owner;
        const u32 size = static_cast<u32>(bytes);
        const u32 addr = p.exec_->alloc(size, MEMF_PUBLIC);
        if (addr == 0) {
            return nullptr;
        }
        Bus b(*p.machine_);
        u8* at = b.bytes_at(addr, size);
        if (at == nullptr) {
            p.exec_->free(addr, size);
            return nullptr;
        }
        ctx.load->work = addr;
        ctx.load->work_bytes = size;
        ctx.load->work_host = at;
        return at;
    }

    static void release_work(void* raw, void* block, usize) {
        AllocCtx& ctx = *static_cast<AllocCtx*>(raw);
        if (!matches(ctx) || ctx.load->work == 0 || ctx.load->work_host != block) {
            return;
        }
        ImageDataType& p = *ctx.load->owner;
        p.exec_->free(ctx.load->work, ctx.load->work_bytes);
        ctx.load->work = 0;
        ctx.load->work_bytes = 0;
        ctx.load->work_host = nullptr;
    }

    bool claim() {
        for (Load& row : p.loads_) {
            if (row.used) {
                continue;
            }
            row = Load{};
            row.owner = &p;
            row.used = true;
            row.task = p.exec_->this_task();
            row.generation = ++p.next_generation_;
            if (row.generation == 0) {
                row.generation = ++p.next_generation_;
            }
            load = &row;
            generation = row.generation;
            task = row.task;
            claimed = true;
            host_ctx = AllocCtx{load, generation};
            work_ctx = AllocCtx{load, generation};
            return true;
        }
        return false;
    }

    // The row is still this load's: no task end hook has given it back and
    // no other load has taken it over.
    bool owns() const {
        return claimed && load != nullptr && load->used && load->owner == &p &&
               load->generation == generation;
    }

    void release_load() {
        if (!claimed) {
            return;
        }
        if (owns()) {
            p.release(*load);
        }
        claimed = false;
        load = nullptr;
    }

    void free_guest(u32& addr, u32& bytes) {
        if (addr != 0) {
            p.exec_->free(addr, bytes);
            addr = 0;
            bytes = 0;
        }
    }

    void log_loaded(u32 width, u32 height) {
        LogLine().text(l.leaf).text(": ").dec(width).text("x").dec(height).text(" loaded").send();
    }

    void log_failed(u32 error) {
        LogLine().text(l.leaf).text(": could not load, error ").dec(error).send();
    }

    // Leaves error in the caller's IoErr, as SetIoErr does, before there is
    // an object.
    u32 refuse(u32 error) {
        if (!stopped()) {
            p.dos_->set_io_err(error);
            log_failed(error);
        }
        return 0;
    }

    // Frees the load's buffers, disposes of the object the superclass made
    // with the message the row holds, frees the row and leaves error in the
    // caller's IoErr. A run that ended or a task that went stops the class
    // instead: nothing more is called.
    u32 fail(u32 error) {
        if (still_running() && owns()) {
            p.release_buffers(*load);
            const u32 msg = load->message;
            b.w32(msg, OM_DISPOSE);
            if (b.ok()) {
                boopsi().coerce_method(cl, object, msg);
            }
        }
        const bool lost = !still_running();
        release_load();
        if (lost) {
            return 0;
        }
        return refuse(error);
    }

    u32 om_new(u32 msg) {
        // The row and the block its messages are built in come first, so
        // that disposing of the object after a failed load needs no memory.
        if (!claim()) {
            return refuse(ERROR_NO_FREE_STORE);
        }
        load->message = p.exec_->alloc(kMessageBytes, MEMF_PUBLIC | MEMF_CLEAR);
        if (load->message == 0) {
            release_load();
            return refuse(ERROR_NO_FREE_STORE);
        }
        const u32 made = boopsi().call_super(cl, object, msg);
        if (made == 0 || !still_running()) {
            release_load();
            return 0;
        }
        object = made;
        const u32 name = tags::data(b, DTA_Name, b.r32(msg + 4), 0);
        // What the superclasses made of the source: for a file,
        // datatypesclass answers the handle it opened, where the tags hold
        // the lock the file was identified through.
        const u32 source = get(DTA_SourceType);
        const u32 handle = get(DTA_Handle);
        if (!still_running()) {
            return fail(ERROR_NO_FREE_STORE);
        }
        if (source == DTST_RAM) {
            release_load();
            return made;
        }
        if (source != DTST_FILE || handle == 0) {
            return fail(ERROR_OBJECT_WRONG_TYPE);
        }
        const u32 error = load_file(handle, name);
        if (error != 0) {
            return fail(error);
        }
        release_load();
        return made;
    }

    // GetAttr on the object as its own class answers it, with the message
    // in the row's block; 0 if it does not, or the load was left behind.
    u32 get(u32 id) {
        if (!still_running() || !owns()) {
            return 0;
        }
        const u32 msg = load->message;
        b.w32(msg, OM_GET);
        b.w32(msg + 4, id);
        b.w32(msg + 8, msg + 12);
        b.w32(msg + 12, 0);
        if (!b.ok() || boopsi().do_method(object, msg) == 0) {
            return 0;
        }
        return b.r32(msg + 12);
    }

    // OM_SET of up to kSetTags attribute pairs on the object as its own
    // class, with the message in the row's block.
    u32 set(const u32* pairs, u32 count) {
        if (count > kSetTags) {
            return DTERROR_INVALID_DATA;
        }
        if (!still_running() || !owns()) {
            return ERROR_NO_FREE_STORE;
        }
        const u32 msg = load->message;
        b.w32(msg, OM_SET);
        b.w32(msg + 4, msg + 12);
        b.w32(msg + 8, 0);
        for (u32 i = 0; i < count * 2; ++i) {
            b.w32(msg + 12 + i * 4, pairs[i]);
        }
        b.w32(msg + 12 + count * 8, tags::kTagDone);
        b.w32(msg + 16 + count * 8, 0);
        if (!b.ok()) {
            return DTERROR_INVALID_DATA;
        }
        boopsi().do_method(object, msg);
        return still_running() ? 0 : ERROR_NO_FREE_STORE;
    }

    // Reads the file through its handle, decodes it and writes it into the
    // object: 0, or the error a failed load leaves.
    u32 load_file(u32 handle, u32 name) {
        u32 size = 0;
        u32 error = 0;
        if (!p.dos_->handle_size(handle, size, error)) {
            return dos_error(error);
        }
        if (size == 0) {
            return DTERROR_INVALID_DATA;
        }
        if (size > kMaxFileBytes) {
            return ERROR_NO_FREE_STORE;
        }
        u32 old_position = 0;
        if (!p.dos_->seek_handle(handle, 0, OFFSET_BEGINNING, old_position, error)) {
            return dos_error(error);
        }
        load->file = p.exec_->alloc(size, MEMF_PUBLIC);
        if (load->file == 0) {
            return ERROR_NO_FREE_STORE;
        }
        load->file_bytes = size;
        u8* data = b.bytes_at(load->file, size);
        if (data == nullptr) {
            return ERROR_NO_FREE_STORE;
        }
        for (u32 at = 0; at < size;) {
            const u32 part = size - at < kReadBytes ? size - at : kReadBytes;
            u32 got = 0;
            if (!p.dos_->read_handle(handle, data + at, part, got, error)) {
                return dos_error(error);
            }
            if (got == 0) {
                return DTERROR_INVALID_DATA;
            }
            at += got;
            if (at < size) {
                p.exec_->yield_in_native();
                if (!still_running()) {
                    return ERROR_NO_FREE_STORE;
                }
            }
        }
        image::Allocator host{&host_ctx, alloc_host, release_host};
        image::Info info;
        const image::Status probed = image::probe(data, size, host, info);
        if (probed != image::Status::Ok) {
            return error_of(probed);
        }
        if (info.format != l.format) {
            return ERROR_OBJECT_WRONG_TYPE;
        }
        const u64 pixel_bytes = u64{info.width} * u64{info.height} * 4u;
        if (pixel_bytes == 0 || pixel_bytes > 0xFFFFFFFFu) {
            return ERROR_NO_FREE_STORE;
        }
        load->pixels = p.exec_->alloc(static_cast<u32>(pixel_bytes), MEMF_PUBLIC);
        if (load->pixels == 0) {
            return ERROR_NO_FREE_STORE;
        }
        load->pixels_bytes = static_cast<u32>(pixel_bytes);
        u8* pixels = b.bytes_at(load->pixels, load->pixels_bytes);
        if (pixels == nullptr) {
            return ERROR_NO_FREE_STORE;
        }
        image::Allocator work{&work_ctx, alloc_work, release_work};
        image::Progress progress{this, keep_going};
        const image::Status decoded =
            image::decode(data, size, host, work, pixels, load->pixels_bytes, progress);
        // The decode yields: the row may have gone with the task meanwhile.
        if (!owns()) {
            return ERROR_NO_FREE_STORE;
        }
        free_guest(load->file, load->file_bytes);
        if (decoded != image::Status::Ok && decoded != image::Status::Partial) {
            return error_of(decoded);
        }
        // Only a picture that may carry alpha, or one a cut left partly
        // clear, can have a pixel that is not opaque.
        const bool scan = info.alpha || decoded != image::Status::Ok;
        const u32 masking = scan ? scan_masking(pixels, info.width * info.height) : 0;
        if (masking == 0xFFFFFFFFu) {
            return ERROR_NO_FREE_STORE;
        }
        u32 result = fill_header(info.width, info.height, masking);
        if (result == 0) {
            const u32 pairs[] = {DTA_ObjName,      name,          DTA_NominalHoriz, info.width,
                                 DTA_NominalVert,  info.height,   PDTA_SourceMode,  PMODE_V43};
            result = set(pairs, 4);
        }
        if (result == 0) {
            result = write_pixels(info.width, info.height);
        }
        // The mode comes once the pixels are there, so that picture.datatype
        // holds the frame's depth to the mode's deepest.
        if (result == 0) {
            const u32 pairs[] = {PDTA_ModeID, mode_of(info.width, info.height)};
            result = set(pairs, 1);
        }
        if (result != 0) {
            return result;
        }
        log_loaded(info.width, info.height);
        return 0;
    }

    // mskHasAlpha when a decoded pixel is not opaque. 0xFFFFFFFF when a
    // yield left the run or the task behind.
    u32 scan_masking(u8* pixels, u32 count) {
        for (u32 at = 0; at < count; at += kScanPixels) {
            if (at != 0) {
                p.exec_->yield_in_native();
                if (!still_running()) {
                    return 0xFFFFFFFFu;
                }
            }
            const u32 run = count - at < kScanPixels ? count - at : kScanPixels;
            for (u32 i = 0; i < run; ++i) {
                if (pixels[(at + i) * 4 + 3] != 255) {
                    return mskHasAlpha;
                }
            }
        }
        return 0;
    }

    u32 fill_header(u32 width, u32 height, u32 masking) {
        const u32 header = get(PDTA_BitMapHeader);
        if (!still_running()) {
            return ERROR_NO_FREE_STORE;
        }
        if (header == 0) {
            return DTERROR_INVALID_DATA;
        }
        Bus h(*p.machine_);
        h.w16(header + bmh_Width, width);
        h.w16(header + bmh_Height, height);
        h.w16(header + bmh_Left, 0);
        h.w16(header + bmh_Top, 0);
        h.w8(header + bmh_Depth, 24);
        h.w8(header + bmh_Masking, masking);
        h.w8(header + bmh_Compression, 0);
        h.w16(header + bmh_Transparent, 0);
        h.w8(header + bmh_XAspect, 1);
        h.w8(header + bmh_YAspect, 1);
        h.w16(header + bmh_PageWidth, width < 32767 ? width : 32767);
        h.w16(header + bmh_PageHeight, height < 32767 ? height : 32767);
        return h.ok() ? 0 : DTERROR_INVALID_DATA;
    }

    // The V42 mode ilbm.datatype would pick for the picture's size.
    static u32 mode_of(u32 width, u32 height) {
        return (width >= kHiresWidth ? kHires : 0) | (height >= kLaceHeight ? kLace : 0);
    }

    u32 write_pixels(u32 width, u32 height) {
        if (!still_running() || !owns()) {
            return ERROR_NO_FREE_STORE;
        }
        const u32 longs[] = {PDTM_WRITEPIXELARRAY, load->pixels, PBPAFMT_RGBA, width * 4,
                             0,                    0,            width,        height};
        const u32 msg = load->message;
        for (u32 i = 0; i < sizeof longs / sizeof longs[0]; ++i) {
            b.w32(msg + i * 4, longs[i]);
        }
        if (!b.ok() || boopsi().coerce_method(cl, object, msg) == 0) {
            return ERROR_NO_FREE_STORE;
        }
        return still_running() ? 0 : ERROR_NO_FREE_STORE;
    }
};

bool ImageDataType::init(Machine& m, Exec& exec, Intuition& intuition, Dos& dos) {
    machine_ = &m;
    exec_ = &exec;
    intuition_ = &intuition;
    dos_ = &dos;
    png_.owner = this;
    png_.format = image::Format::Png;
    png_.leaf = kPngLeaf;
    png_.id = kPngId;
    jpeg_.owner = this;
    jpeg_.format = image::Format::Jpeg;
    jpeg_.leaf = kJpegLeaf;
    jpeg_.id = kJpegId;
    // A new system: the picture.datatype bases and the loads of the last
    // one are gone with it.
    png_.picture = 0;
    jpeg_.picture = 0;
    for (Load& load : loads_) {
        load = Load{};
    }
    next_generation_ = 0;
    if (!exec.add_library_provider(
            LibraryProvider{png_.leaf, kHombreLibVersion, make_library, &png_, kDir})) {
        return false;
    }
    if (!exec.add_library_provider(
            LibraryProvider{jpeg_.leaf, kHombreLibVersion, make_library, &jpeg_, kDir})) {
        return false;
    }
    exec.set_task_end_hook(&ImageDataType::task_ended, this);
    return true;
}

u32 ImageDataType::make_library(void* ctx) {
    Library& l = *static_cast<Library*>(ctx);
    ImageDataType& self = *l.owner;
    return l.library.make(*self.machine_, *self.exec_, l.leaf, l.id, make_class, ctx);
}

// The class over picture.datatype, which is opened when the class is made
// and kept open while it lives.
u32 ImageDataType::make_class(void* ctx, u32) {
    Library& l = *static_cast<Library*>(ctx);
    ImageDataType& self = *l.owner;
    if (l.picture == 0) {
        l.picture = self.exec_->open_library("datatypes/picture.datatype", 0);
    }
    const u32 super = self.intuition_->boopsi().find_class_named("picture.datatype");
    if (l.picture == 0 || super == 0) {
        LogLine().text(l.leaf).text(": no picture.datatype").send();
        if (l.picture != 0) {
            self.exec_->close_library(l.picture);
            l.picture = 0;
        }
        return 0;
    }
    const u32 cl = self.intuition_->boopsi().make_native_class(l.leaf, super, 0, dispatch, ctx);
    if (cl == 0) {
        self.exec_->close_library(l.picture);
        l.picture = 0;
    }
    return cl;
}

u32 ImageDataType::dispatch(void* ctx, u32 cl, u32 object, u32 msg) {
    Library& l = *static_cast<Library*>(ctx);
    Ops ops(l, cl, object);
    const u32 method = ops.b.r32(msg);
    if (!ops.b.ok()) {
        return 0;
    }
    if (method == OM_NEW) {
        return ops.om_new(msg);
    }
    return l.owner->intuition_->boopsi().pass_on(cl, object, msg);
}

void ImageDataType::task_ended(void* ctx, u32 task, bool) {
    static_cast<ImageDataType*>(ctx)->release_task(task);
}

void ImageDataType::release_task(u32 task) {
    for (Load& load : loads_) {
        if (load.used && load.task == task) {
            release(load);
        }
    }
}

void ImageDataType::release_buffers(Load& load) {
    if (load.host != nullptr) {
        ::operator delete(load.host);
        load.host = nullptr;
        load.host_bytes = 0;
    }
    if (load.work != 0) {
        exec_->free(load.work, load.work_bytes);
        load.work = 0;
        load.work_bytes = 0;
        load.work_host = nullptr;
    }
    if (load.file != 0) {
        exec_->free(load.file, load.file_bytes);
        load.file = 0;
        load.file_bytes = 0;
    }
    if (load.pixels != 0) {
        exec_->free(load.pixels, load.pixels_bytes);
        load.pixels = 0;
        load.pixels_bytes = 0;
    }
}

void ImageDataType::release(Load& load) {
    release_buffers(load);
    if (load.message != 0) {
        exec_->free(load.message, kMessageBytes);
    }
    load = Load{};
}

}
