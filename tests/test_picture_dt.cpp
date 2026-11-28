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

// picture.datatype between a subclass in 68000 code, which loads a picture
// the way ilbm.datatype does, and a datatypesclass that is 68000 code to
// BOOPSI: an A-line stub whose native keeps a DTSpecialInfo, a domain and
// a FrameInfo as datatypes.library's class does. A datatypes.library with
// a DoAsyncLayout that lays out at once, and a dos.library whose Write
// keeps the bytes, stand in for the libraries the class calls.

#include "gfx_rig.hpp"
#include "libs/display_modes.hpp"
#include "libs/picture_dt.hpp"
#include "image/ilbm.hpp"

#include <cstring>
#include <vector>

using namespace hombre;
using namespace hombre::m68k;
using namespace hombre::ndk;
using gfx_test::Base;
using gfx_test::GfxRig;
using gfx_test::kData;
using native::Bus;

namespace {

constexpr u32 kTags = kData;
constexpr u32 kStore = kData + 0x400;
constexpr u32 kMsg = kData + 0x500;
constexpr u32 kName = kData + 0x600;
constexpr u32 kCode = kData + 0x3000;
constexpr u32 kSuperCaller = kCode + 0x40;
constexpr u32 kStandInStub = kCode + 0x80;
constexpr u32 kStandInName = kCode + 0xC0;
constexpr u32 kMiB = 1024 * 1024;

constexpr u32 DTA_Dummy = 0x80001000;
constexpr u32 DTA_TopVert = DTA_Dummy + 11;
constexpr u32 DTA_TopHoriz = DTA_Dummy + 15;
constexpr u32 DTA_Title = DTA_Dummy + 20;
constexpr u32 DTA_Methods = DTA_Dummy + 24;
constexpr u32 DTA_Busy = DTA_Dummy + 28;
constexpr u32 DTA_ErrorLevel = DTA_Dummy + 32;
constexpr u32 DTA_ErrorNumber = DTA_Dummy + 33;
constexpr u32 DTA_Domain = DTA_Dummy + 104;
constexpr u32 DTA_ObjName = DTA_Dummy + 109;
constexpr u32 DTA_FrameInfo = DTA_Dummy + 116;
constexpr u32 DTA_SelectDomain = DTA_Dummy + 121;
constexpr u32 DTA_NominalVert = DTA_Dummy + 124;
constexpr u32 DTA_NominalHoriz = DTA_Dummy + 125;
constexpr u32 PDTA_ModeID = DTA_Dummy + 200;
constexpr u32 PDTA_BitMapHeader = DTA_Dummy + 201;
constexpr u32 PDTA_BitMap = DTA_Dummy + 202;
constexpr u32 PDTA_ColorRegisters = DTA_Dummy + 203;
constexpr u32 PDTA_CRegs = DTA_Dummy + 204;
constexpr u32 PDTA_GRegs = DTA_Dummy + 205;
constexpr u32 PDTA_ColorTable = DTA_Dummy + 206;
constexpr u32 PDTA_ColorTable2 = DTA_Dummy + 207;
constexpr u32 PDTA_NumColors = DTA_Dummy + 209;
constexpr u32 PDTA_NumAlloc = DTA_Dummy + 210;
constexpr u32 PDTA_Remap = DTA_Dummy + 211;
constexpr u32 PDTA_Screen = DTA_Dummy + 212;
constexpr u32 PDTA_FreeSourceBitMap = DTA_Dummy + 213;
constexpr u32 PDTA_Grab = DTA_Dummy + 214;
constexpr u32 PDTA_DestBitMap = DTA_Dummy + 215;
constexpr u32 PDTA_NumSparse = DTA_Dummy + 217;
constexpr u32 PDTA_SparseTable = DTA_Dummy + 218;
constexpr u32 DTM_FRAMEBOX = 0x601;
constexpr u32 DTM_PROCLAYOUT = 0x602;
constexpr u32 DTM_ASYNCLAYOUT = 0x603;
constexpr u32 DTM_SELECT = 0x605;
constexpr u32 DTM_CLEARSELECTED = 0x606;
constexpr u32 DTM_COPY = 0x607;
constexpr u32 DTM_WRITE = 0x650;

// The stand-in datatypesclass's instance data, laid out as the real one's.
constexpr u32 dt_Domain = 22;
constexpr u32 dt_Select = 30;
constexpr u32 dt_NominalHoriz = 54;
constexpr u32 dt_NominalVert = 58;
constexpr u32 dt_ObjName = 62;
constexpr u32 dt_FrameInfo = 90;
constexpr u32 dt_Special = 130;
constexpr u32 kStandInBytes = 246;
constexpr u32 si_Flags = 46;
constexpr u32 si_TopVert = 50;
constexpr u32 si_VisVert = 54;
constexpr u32 si_TotVert = 58;
constexpr u32 si_OTopVert = 62;
constexpr u32 si_VertUnit = 66;
constexpr u32 si_TopHoriz = 70;
constexpr u32 si_VisHoriz = 74;
constexpr u32 si_TotHoriz = 78;
constexpr u32 DTSIF_HIGHLIGHT = 0x10;

Machine* g_machine = nullptr;

struct Seen {
    u32 method = 0;
    u32 run_depth = 0;
};

// What reached the stand-in datatypesclass.
struct StandIn {
    Boopsi* bo = nullptr;
    Exec* exec = nullptr;
    u32 cl = 0;
    std::vector<Seen> methods;
    // The tag pairs of every OM_NOTIFY.
    std::vector<std::vector<std::pair<u32, u32>>> notifies;

    u32 data(u32 object) const {
        Bus b(*g_machine);
        return object + b.r16(cl + cl_InstOffset);
    }
    u32 count(u32 method) const {
        u32 n = 0;
        for (const Seen& s : methods) {
            n += s.method == method ? 1 : 0;
        }
        return n;
    }
};

StandIn* g_stand_in = nullptr;
// What the stand-in answers DTM_FRAMEBOX with: datatypes.library 40.6 leaves
// a register as it is, 0 in the traces.
u32 g_frame_box_result = 0;
// Set, the stand-in's OM_GET of DTA_ObjName ends the run with a bus error.
bool g_fail_obj_name = false;

u32 stand_in_dispatch(StandIn& s, Machine& m, u32 cl, u32 object, u32 msg) {
    Bus b(m);
    const u32 method = b.r32(msg);
    s.methods.push_back(Seen{method, m.jit->run_depth()});
    switch (method) {
    case OM_NEW: {
        const u32 made = s.bo->call_super(cl, object, msg);
        if (made != 0) {
            const u32 data = s.data(made);
            b.w32(made + gg_SpecialInfo, data + dt_Special);
            LibraryCall args;
            args.a[0] = data + dt_Special;
            u32 d0 = 0;
            CHECK(s.exec->call_library(s.exec->exec_base(), exec_lvo::InitSemaphore, args, d0));
        }
        return made;
    }
    case OM_GET: {
        const u32 data = s.data(object);
        const u32 si = data + dt_Special;
        const u32 id = b.r32(msg + 4);
        const u32 storage = b.r32(msg + 8);
        u32 value = 0;
        switch (id) {
        case DTA_Domain:
            value = data + dt_Domain;
            break;
        case DTA_SelectDomain:
            value = (b.r32(si + si_Flags) & DTSIF_HIGHLIGHT) != 0 ? data + dt_Select : 0;
            break;
        case DTA_FrameInfo:
            value = data + dt_FrameInfo;
            break;
        case DTA_ObjName:
            if (g_fail_obj_name) {
                m.cpu.exit = ExitReason::BusError;
                return 0;
            }
            value = b.r32(data + dt_ObjName);
            break;
        case DTA_NominalHoriz:
            value = b.r32(data + dt_NominalHoriz);
            break;
        default:
            return s.bo->call_super(cl, object, msg);
        }
        b.w32(storage, value);
        return 1;
    }
    case OM_SET:
    case OM_UPDATE: {
        const u32 result = s.bo->call_super(cl, object, msg);
        const u32 data = s.data(object);
        const u32 si = data + dt_Special;
        u32 changed = 0;
        for (u32 item = b.r32(msg + 4); b.r32(item) != 0; item += 8) {
            const u32 value = b.r32(item + 4);
            switch (b.r32(item)) {
            case DTA_ObjName:
                b.w32(data + dt_ObjName, value);
                break;
            case DTA_NominalHoriz:
                b.w32(data + dt_NominalHoriz, value);
                break;
            case DTA_NominalVert:
                b.w32(data + dt_NominalVert, value);
                break;
            case DTA_TopVert:
                changed |= b.r32(si + si_TopVert) != value ? 1 : 0;
                b.w32(si + si_TopVert, value);
                break;
            case DTA_TopHoriz:
                changed |= b.r32(si + si_TopHoriz) != value ? 1 : 0;
                b.w32(si + si_TopHoriz, value);
                break;
            default:
                break;
            }
        }
        return result + changed;
    }
    case GM_LAYOUT:
    case DTM_PROCLAYOUT: {
        const u32 data = s.data(object);
        for (u32 i = 0; i < 8; i += 2) {
            b.w16(data + dt_Domain + i, b.r16(object + gg_LeftEdge + i));
        }
        return 1;
    }
    case DTM_FRAMEBOX:
        return g_frame_box_result;
    case OM_NOTIFY: {
        std::vector<std::pair<u32, u32>> pairs;
        for (u32 item = b.r32(msg + 4); b.r32(item) != 0; item += 8) {
            pairs.emplace_back(b.r32(item), b.r32(item + 4));
        }
        s.notifies.push_back(pairs);
        return s.bo->call_super(cl, object, msg);
    }
    default:
        return s.bo->call_super(cl, object, msg);
    }
}

void stand_in_entry(void* ctx, Machine& m) {
    StandIn& s = *static_cast<StandIn*>(ctx);
    const u32 result = stand_in_dispatch(s, m, m.cpu.a[0], m.cpu.a[2], m.cpu.a[1]);
    native::set_result(m, result);
}

// datatypes.library's DoAsyncLayout, done at once in the caller's task:
// DTM_ASYNCLAYOUT with the gpLayout's GadgetInfo and gpl_Initial.
u32 g_async_layouts = 0;
void do_async_layout(void*, Machine& m) {
    ++g_async_layouts;
    Bus b(m);
    const u32 object = m.cpu.a[0];
    const u32 layout = m.cpu.a[1];
    const u32 msg[] = {DTM_ASYNCLAYOUT, b.r32(layout + 4), b.r32(layout + 8)};
    Boopsi& bo = *g_stand_in->bo;
    bo.send(bo.class_of(object), object, msg, 3);
    native::set_result(m, 1);
}

// dos.library's Write(file D1, buffer D2, length D3), into g_written.
std::vector<u8> g_written;
u32 g_write_file = 0;
// The lock dos_write finds held, as SS_NESTCOUNT of the semaphore here.
u32 g_watched_lock = 0;
u32 g_lock_nest_seen = 0;
void dos_write(void*, Machine& m) {
    Bus b(m);
    g_write_file = m.cpu.d[1];
    if (g_watched_lock != 0) {
        g_lock_nest_seen = b.r16(g_watched_lock + SS_NESTCOUNT);
    }
    for (u32 i = 0; i < m.cpu.d[3]; ++i) {
        g_written.push_back(static_cast<u8>(b.r8(m.cpu.d[2] + i)));
    }
    native::set_result(m, m.cpu.d[3]);
}

void put_code(GfxRig& r, u32 at, std::initializer_list<u16> words) {
    u32 p = at;
    for (const u16 w : words) {
        r.put16(p, w);
        p += 2;
    }
    r.jit.flush();
}

// The rig with the stand-ins, picture.datatype opened as datatypes.library
// opens a subclass's superclass, and a subclass in 68000 code that sends
// every method on with a JSR to its superclass's h_Entry.
struct Rig {
    GfxRig r;
    StandIn s;
    PictureDataType picture;
    u32 base = 0;
    u32 picture_class = 0;
    u32 sub = 0;

    explicit Rig(bool with_datatypes = true) {
        g_machine = &r.machine;
        g_stand_in = &s;
        g_async_layouts = 0;
        g_written.clear();
        Boopsi& bo = r.intuition.boopsi();
        s.bo = &bo;
        s.exec = &r.exec;
        if (with_datatypes) {
            const u16 id = r.jit.add_native(stand_in_entry, &s, "datatypesclass");
            put_code(r, kStandInStub, {static_cast<u16>(0xA000 | id), 0x4E75});
            r.str(kStandInName, "datatypesclass");
            s.cl = bo.make_class(kStandInName, 0, bo.gadget_class(), kStandInBytes, 0);
            r.put32(s.cl + h_Entry, kStandInStub);
            bo.add_class(s.cl);
            static NativeVector datatypes_fns[14] = {};
            datatypes_fns[13] = NativeVector{do_async_layout, "DoAsyncLayout"};
            CHECK(r.exec.make_native_library("datatypes.library", 40, datatypes_fns, 14) != 0);
        }
        static NativeVector dos_fns[8] = {};
        dos_fns[7] = NativeVector{dos_write, "Write"};
        CHECK(r.exec.make_native_library("dos.library", 40, dos_fns, 8) != 0);
        CHECK(picture.init(r.machine, r.exec, r.graphics, r.intuition));
        base = r.exec.open_library("datatypes/picture.datatype", 0);
        if (base == 0) {
            return;
        }
        picture_class = picture.class_ptr();
        put_code(r, kSuperCaller, {
                                      0x2F0B,          // move.l a3,-(sp)
                                      0x2068, 0x0018,  // movea.l cl_Super(a0),a0
                                      0x2668, 0x0008,  // movea.l h_Entry(a0),a3
                                      0x4E93,          // jsr (a3)
                                      0x265F,          // movea.l (sp)+,a3
                                      0x4E75,          // rts
                                  });
        sub = bo.make_class(0, 0, picture_class, 0, 0);
        r.put32(sub + h_Entry, kSuperCaller);
    }

    Boopsi& bo() { return r.intuition.boopsi(); }

    u32 get(u32 object, u32 attr) {
        r.put32(kStore, 0xDEADBEEF);
        CHECK(bo().get_attr(attr, object, kStore) != 0);
        return r.get32(kStore);
    }

    u32 set(u32 object, std::initializer_list<std::pair<u32, u32>> items, u32 ginfo = 0) {
        std::vector<std::pair<u32, u32>> all(items);
        all.emplace_back(0, 0);
        u32 at = kTags + 0x200;
        for (const auto& [tag, data] : all) {
            r.put32(at, tag);
            r.put32(at + 4, data);
            at += 8;
        }
        return bo().set_attrs(object, kTags + 0x200, ginfo);
    }

    u32 send(u32 object, std::initializer_list<u32> longs) {
        std::vector<u32> v(longs);
        return bo().send(bo().class_of(object), object, v.data(), static_cast<u32>(v.size()));
    }

    u32 alloc_bitmap(u32 width, u32 height, u32 depth, u32 friend_bm = 0) {
        r.cpu().d[0] = width;
        r.cpu().d[1] = height;
        r.cpu().d[2] = depth;
        r.cpu().d[3] = 1;
        r.cpu().a[0] = friend_bm;
        return r.result(Base::Graphics, graphics_lvo::AllocBitMap);
    }

    // A picture of width by height pixels and depth planes, made the way
    // ilbm.datatype makes one: pixel (x, y) is colour (x / 2 + y) mod
    // colours, whose colour is colours[i].
    u32 load(u32 width, u32 height, u32 depth, const std::vector<u32>& colours,
             u32 left = 0, u32 top = 0) {
        r.str(kName, "pic");
        const u32 tags = r.tags(kTags, {{GA_Left, left}, {GA_Top, top}, {GA_Width, 200},
                                        {GA_Height, 100}, {GA_ID, 7}, {0, 0}});
        const u32 object = bo().new_object(sub, 0, tags);
        CHECK(object != 0);
        const u32 bmh = get(object, PDTA_BitMapHeader);
        CHECK(bmh != 0);
        r.put16(bmh, width);
        r.put16(bmh + 2, height);
        r.put8(bmh + 8, depth);
        r.put8(bmh + 10, 1);
        r.put8(bmh + 14, 10);
        r.put8(bmh + 15, 11);
        r.put16(bmh + 16, 320);
        r.put16(bmh + 18, 200);
        const u32 bm = alloc_bitmap(width, height, depth);
        CHECK(bm != 0);
        const u32 bpr = r.get16(bm + bm_BytesPerRow);
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                const u32 index = (x / 2 + y) % colours.size();
                for (u32 plane = 0; plane < depth; ++plane) {
                    if (((index >> plane) & 1) != 0) {
                        const u32 at = r.get32(bm + bm_Planes + plane * 4) + y * bpr + x / 8;
                        r.put8(at, r.get8(at) | (0x80 >> (x & 7)));
                    }
                }
            }
        }
        set(object, {{DTA_ObjName, kName}, {DTA_NominalHoriz, width}, {DTA_NominalVert, height},
                     {PDTA_BitMap, bm}, {PDTA_ModeID, 0x8000}});
        set(object, {{PDTA_NumColors, static_cast<u32>(colours.size())}});
        const u32 cmap = get(object, PDTA_ColorRegisters);
        const u32 cregs = get(object, PDTA_CRegs);
        CHECK(cmap != 0);
        CHECK(cregs != 0);
        for (u32 i = 0; i < colours.size(); ++i) {
            for (u32 k = 0; k < 3; ++k) {
                const u32 c = (colours[i] >> (16 - 8 * k)) & 0xFF;
                r.put8(cmap + i * 3 + k, c);
                r.put32(cregs + i * 12 + k * 4, c * 0x01010101u);
            }
        }
        return object;
    }
};

u32 open_planar_screen(GfxRig& r, u32 depth) {
    const u32 tags = r.tags(kTags + 0x100, {{SA_Width, 320}, {SA_Height, 200}, {SA_Depth, depth},
                                            {0, 0}});
    const u32 screen = r.intuition.open_screen(0, tags);
    CHECK(screen != 0);
    return screen;
}

u32 open_window(GfxRig& r, u32 screen) {
    const u32 tags = r.tags(kTags + 0x180, {{WA_Left, 0}, {WA_Top, 0}, {WA_Width, 320},
                                            {WA_Height, 200}, {WA_CustomScreen, screen},
                                            {WA_Borderless, 1}, {0, 0}});
    const u32 window = r.intuition.open_window(0, tags);
    CHECK(window != 0);
    return window;
}

void set_colour(GfxRig& r, u32 screen, u32 pen, u32 rgb) {
    r.cpu().a[0] = screen + sc_ViewPort;
    r.cpu().d[0] = pen;
    r.cpu().d[1] = ((rgb >> 16) & 0xFF) * 0x01010101u;
    r.cpu().d[2] = ((rgb >> 8) & 0xFF) * 0x01010101u;
    r.cpu().d[3] = (rgb & 0xFF) * 0x01010101u;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetRGB32), ExitReason::Halt);
}

u32 planar_pixel(GfxRig& r, u32 bitmap, u32 x, u32 y) {
    const u32 bpr = r.get16(bitmap + bm_BytesPerRow);
    u32 pen = 0;
    for (u32 plane = 0; plane < r.get8(bitmap + bm_Depth); ++plane) {
        const u32 at = r.get32(bitmap + bm_Planes + plane * 4) + y * bpr + x / 8;
        pen |= ((r.get8(at) >> (7 - (x & 7))) & 1) << plane;
    }
    return pen;
}

u32 read_pixel(GfxRig& r, u32 rp, u32 x, u32 y) {
    r.cpu().a[1] = rp;
    r.cpu().d[0] = x;
    r.cpu().d[1] = y;
    return r.result(Base::Graphics, graphics_lvo::ReadPixel);
}

bool notified(const StandIn& s, u32 tag, u32 value) {
    for (const auto& pairs : s.notifies) {
        for (const auto& [t, v] : pairs) {
            if (t == tag && v == value) {
                return true;
            }
        }
    }
    return false;
}

// GM_LAYOUT as Intuition sends it, through the window's GadgetInfo.
u32 lay_out(Rig& t, u32 object, u32 window, u32 initial) {
    const u32 info = t.r.intuition.make_gadget_info(window);
    CHECK(info != 0);
    const u32 result = t.send(object, {GM_LAYOUT, info, initial});
    t.r.intuition.free_gadget_info(info);
    return result;
}

void opens_as_the_class_datatypes_library_asks_for() {
    Rig t;
    CHECK(t.base != 0);
    CHECK_EQ(t.r.get16(t.base + LIB_VERSION), 60u);
    CHECK(t.r.read_str(t.r.get32(t.base + LN_NAME)) == "picture.datatype");
    CHECK(t.picture_class != 0);
    CHECK_EQ(t.bo().find_class_named("picture.datatype"), t.picture_class);
    CHECK_EQ(t.r.get32(t.picture_class + cl_Super), t.s.cl);
    CHECK_EQ(t.r.get32(t.picture_class + cl_UserData), t.base);
    CHECK_EQ(t.r.get16(t.picture_class + cl_InstOffset),
             ExtGadget_SIZEOF + kStandInBytes);
    // ObtainEngine, LVO -30, hands out the class.
    CHECK_EQ(t.r.call_at(t.base, 30), ExitReason::Halt);
    CHECK_EQ(t.r.cpu().d[0], t.picture_class);
    // Opened again, by its plain name too, it is the same library.
    CHECK_EQ(t.r.exec.open_library("PICTURE.DATATYPE", 0), t.base);
    CHECK_EQ(t.picture.class_ptr(), t.picture_class);
    CHECK_EQ(t.r.get16(t.base + LIB_OPENCNT), 2u);
}

void does_not_open_without_datatypesclass() {
    Rig t(false);
    CHECK_EQ(t.base, 0u);
    CHECK(gfx_test::logged("picture.datatype: no datatypesclass"));
}

void new_object_has_storage_for_its_subclass() {
    Rig t;
    const u32 tags = t.r.tags(kTags, {{0, 0}});
    const u32 object = t.bo().new_object(t.sub, 0, tags);
    CHECK(object != 0);
    const u32 bmh = t.get(object, PDTA_BitMapHeader);
    const u32 grab = t.get(object, PDTA_Grab);
    CHECK(bmh != 0);
    CHECK(grab != 0);
    CHECK(grab >= bmh + 20 || grab + 4 <= bmh);
    CHECK_EQ(t.get(object, PDTA_ModeID), 0xFFFFFFFFu);
    CHECK_EQ(t.get(object, PDTA_NumColors), 0u);
    CHECK_EQ(t.get(object, PDTA_BitMap), 0u);
    const u32 si = t.r.get32(object + gg_SpecialInfo);
    CHECK_EQ(t.r.get32(si + si_VertUnit), 1u);
    CHECK_EQ(t.r.get32(si + si_OTopVert), 0xFFFFFFFFu);
    // The methods it answers: no copy or print yet.
    const u32 methods = t.get(object, DTA_Methods);
    std::vector<u32> list;
    for (u32 at = methods; t.r.get32(at) != 0xFFFFFFFF; at += 4) {
        list.push_back(t.r.get32(at));
    }
    CHECK_EQ(list.size(), 14u);
    CHECK_EQ(list.front(), OM_NEW);
    CHECK_EQ(list.back(), DTM_WRITE);
    for (const u32 m : list) {
        CHECK(m != DTM_COPY);
    }
    // The colour tables come with the number of colours, apart.
    t.set(object, {{PDTA_NumColors, 32}});
    CHECK_EQ(t.get(object, PDTA_NumColors), 32u);
    const u32 cmap = t.get(object, PDTA_ColorRegisters);
    const u32 cregs = t.get(object, PDTA_CRegs);
    const u32 gregs = t.get(object, PDTA_GRegs);
    CHECK(cmap != 0);
    CHECK(cregs >= cmap + 96);
    CHECK(gregs >= cregs + 384);
    CHECK(t.get(object, PDTA_ColorTable) >= gregs + 384);
    CHECK_EQ(t.r.get32(t.s.data(object) + dt_FrameInfo + 28), cregs);
    // A bitmap sets the frame's size and depth.
    const u32 bm = t.alloc_bitmap(48, 20, 3);
    t.set(object, {{PDTA_BitMap, bm}});
    const u32 info = t.s.data(object) + dt_FrameInfo;
    CHECK_EQ(t.r.get32(info + 12), 48u);
    CHECK_EQ(t.r.get32(info + 16), 20u);
    CHECK_EQ(t.r.get32(info + 20), 3u);
    t.bo().dispose_object(object);
}

// A method the class leaves to datatypesclass reaches it in the run of the
// 68000 code that sent it, one it answers after datatypesclass's answer
// in a run of its own.
void methods_it_leaves_go_on_without_a_run_of_their_own() {
    Rig t;
    const u32 object = t.bo().new_object(t.sub, 0, t.r.tags(kTags, {{0, 0}}));
    CHECK(object != 0);
    t.s.methods.clear();
    CHECK_EQ(t.get(object, DTA_Domain), t.s.data(object) + dt_Domain);
    CHECK_EQ(t.s.methods.size(), 1u);
    const u32 passed = t.s.methods.back().run_depth;
    t.r.put32(kMsg, 0);
    t.send(object, {DTM_FRAMEBOX, 0, 0, kMsg, 36, 0});
    CHECK(t.s.methods.size() >= 2);
    CHECK_EQ(t.s.methods[1].method, DTM_FRAMEBOX);
    CHECK_EQ(t.s.methods[1].run_depth, passed + 1);
    t.bo().dispose_object(object);
    CHECK_EQ(t.s.methods.back().method, OM_DISPOSE);
    CHECK_EQ(t.s.methods.back().run_depth, passed);
}

void first_layout_remaps_for_a_planar_screen() {
    Rig t;
    const u32 screen = open_planar_screen(t.r, 3);
    const u32 window = open_window(t.r, screen);
    const u32 palette[] = {0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00,
                           0x0000FF, 0x808080, 0xFFFF00, 0x00FFFF};
    for (u32 i = 0; i < 8; ++i) {
        set_colour(t.r, screen, i, palette[i]);
    }
    // Four colours: white, blue, a dark red between red and black, black.
    const u32 object = t.load(40, 10, 2, {0xFFFFFF, 0x0000FF, 0x900000, 0x000000});
    t.s.notifies.clear();
    CHECK(lay_out(t, object, window, 1) != 0);
    CHECK_EQ(g_async_layouts, 1u);
    CHECK_EQ(t.s.count(GM_LAYOUT), 1u);
    // Busy first, then the result of the layout.
    CHECK(t.s.notifies.size() >= 2);
    CHECK(t.s.notifies[0][0] == std::make_pair(GA_ID, 7u));
    CHECK(t.s.notifies[0][1] == std::make_pair(DTA_Busy, 1u));
    CHECK(notified(t.s, DTA_NominalVert, 10));
    CHECK(notified(t.s, DTA_NominalHoriz, 40));
    CHECK(notified(t.s, DTA_ErrorLevel, 0));
    CHECK(notified(t.s, DTA_Title, kName));
    const u32 dest = t.get(object, PDTA_DestBitMap);
    CHECK(dest != 0);
    CHECK_EQ(t.get(object, PDTA_BitMap), dest);
    CHECK_EQ(t.r.get8(dest + bm_Depth), 3u);
    const u32 table = t.get(object, PDTA_ColorTable);
    const u32 table2 = t.get(object, PDTA_ColorTable2);
    CHECK_EQ(t.r.get8(table), 1u);
    CHECK_EQ(t.r.get8(table + 1), 4u);
    CHECK_EQ(t.r.get8(table + 3), 0u);
    CHECK_EQ(t.get(object, PDTA_NumAlloc), 4u);
    // The dark red is far from every pen: its second pen mixes with red.
    const u32 red = t.r.get8(table + 2);
    const u32 red2 = t.r.get8(table2 + 2);
    CHECK(red == 0 || red == 2);
    CHECK(red2 == 0 || red2 == 2);
    CHECK(red != red2);
    CHECK_EQ(t.r.get8(table2), 1u);
    // GRegs holds the colour each index became.
    const u32 gregs = t.get(object, PDTA_GRegs);
    CHECK_EQ(t.r.get32(gregs + 12 * 1 + 8), 0xFFFFFFFFu);
    CHECK_EQ(t.r.get32(gregs + 12 * 1), 0u);
    // The pixels: the first pen on even squares, the second on odd ones.
    CHECK_EQ(planar_pixel(t.r, dest, 0, 0), 1u);
    CHECK_EQ(planar_pixel(t.r, dest, 2, 0), 4u);
    CHECK_EQ(planar_pixel(t.r, dest, 4, 0), red);
    CHECK_EQ(planar_pixel(t.r, dest, 5, 0), red2);
    CHECK_EQ(planar_pixel(t.r, dest, 6, 0), 0u);
    const u32 si = t.r.get32(object + gg_SpecialInfo);
    CHECK_EQ(t.r.get32(si + si_VisVert), 100u);
    CHECK_EQ(t.r.get32(si + si_TotVert), 10u);
    CHECK_EQ(t.r.get32(si + si_VisHoriz), 200u);
    CHECK_EQ(t.r.get32(si + si_TotHoriz), 40u);
    CHECK(gfx_test::logged("picture.datatype: 40x10x2 on 3 planes, 4 pens"));

    // A later layout keeps the remapped picture and sizes the view again.
    t.r.put16(object + gg_Height, 50);
    t.s.notifies.clear();
    lay_out(t, object, window, 0);
    CHECK_EQ(t.get(object, PDTA_DestBitMap), dest);
    CHECK_EQ(t.r.get32(si + si_VisVert), 50u);
    CHECK(!notified(t.s, DTA_NominalVert, 10));
    const u32 before = t.r.exec.free_bytes();
    t.bo().dispose_object(object);
    CHECK(t.r.exec.free_bytes() > before);
}

void sparse_table_limits_the_pens() {
    Rig t;
    const u32 screen = open_planar_screen(t.r, 3);
    const u32 window = open_window(t.r, screen);
    set_colour(t.r, screen, 5, 0xFF0000);
    set_colour(t.r, screen, 6, 0x0000FF);
    const u32 object = t.load(16, 4, 1, {0xF00000, 0x0000E0});
    t.r.put8(kStore + 0x20, 6);
    t.r.put8(kStore + 0x21, 5);
    t.set(object, {{PDTA_NumSparse, 2}, {PDTA_SparseTable, kStore + 0x20}});
    lay_out(t, object, window, 1);
    const u32 table = t.get(object, PDTA_ColorTable);
    CHECK_EQ(t.r.get8(table), 5u);
    CHECK_EQ(t.r.get8(table + 1), 6u);
    CHECK_EQ(t.get(object, PDTA_NumAlloc), 0u);
    t.bo().dispose_object(object);
}

void without_remap_the_source_is_shown() {
    Rig t;
    const u32 screen = open_planar_screen(t.r, 3);
    const u32 window = open_window(t.r, screen);
    const u32 object = t.load(16, 4, 2, {0, 0xFFFFFF, 0xFF0000, 0x00FF00});
    t.set(object, {{PDTA_Remap, 0}});
    lay_out(t, object, window, 1);
    const u32 dest = t.get(object, PDTA_DestBitMap);
    CHECK(dest != 0);
    CHECK_EQ(t.r.get8(dest + bm_Depth), 2u);
    CHECK_EQ(t.get(object, PDTA_NumAlloc), 0u);
    const u32 before = t.r.exec.free_bytes();
    t.bo().dispose_object(object);
    CHECK(t.r.exec.free_bytes() > before);
}

void render_draws_from_the_scroll_offsets_and_marks_the_selection() {
    Rig t;
    const u32 screen = open_planar_screen(t.r, 3);
    const u32 window = open_window(t.r, screen);
    const u32 rp = t.r.get32(window + wd_RPort);
    const u32 palette[] = {0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00};
    for (u32 i = 0; i < 4; ++i) {
        set_colour(t.r, screen, i, palette[i]);
    }
    const u32 object = t.load(64, 32, 2, {0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00}, 10, 20);
    // A domain smaller than the picture, which can scroll in it.
    t.r.put16(object + gg_Width, 32);
    t.r.put16(object + gg_Height, 16);
    lay_out(t, object, window, 1);
    const u32 si = t.r.get32(object + gg_SpecialInfo);
    t.r.put32(si + si_TopHoriz, 2);
    t.r.put32(si + si_TopVert, 1);
    const u32 info = t.r.intuition.make_gadget_info(window);
    t.s.methods.clear();
    t.send(object, {GM_RENDER, info, rp, GREDRAW_REDRAW});
    CHECK_EQ(t.s.count(GM_RENDER), 1u);
    // Picture pixel (2, 1) is colour (1 + 1) = 2, at the domain's corner.
    CHECK_EQ(read_pixel(t.r, rp, 10, 20), 2u);
    CHECK_EQ(read_pixel(t.r, rp, 12, 20), 3u);
    CHECK_EQ(t.r.get32(si + si_OTopVert), 1u);
    // Offsets past the picture's edge stay inside it: the 32 by 16 domain
    // shows columns 32 to 63 and rows 16 to 31.
    t.r.put32(si + si_TopHoriz, 1000);
    t.r.put32(si + si_TopVert, 1000);
    t.send(object, {GM_RENDER, info, rp, GREDRAW_UPDATE});
    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 0; x < 32; ++x) {
            CHECK_EQ(read_pixel(t.r, rp, 10 + x, 20 + y), ((32 + x) / 2 + 16 + y) % 4);
        }
    }
    CHECK_EQ(read_pixel(t.r, rp, 42, 20), 0u);
    CHECK_EQ(read_pixel(t.r, rp, 10, 36), 0u);
    // The selection: pen 2 on one pixel in four, the pen and mode put back.
    t.r.put32(si + si_TopHoriz, 0);
    t.r.put32(si + si_TopVert, 0);
    const u32 data = t.s.data(object);
    t.r.put16(data + dt_Select, 4);
    t.r.put16(data + dt_Select + 2, 0);
    t.r.put16(data + dt_Select + 4, 8);
    t.r.put16(data + dt_Select + 6, 4);
    t.send(object, {GM_RENDER, info, rp, GREDRAW_REDRAW});
    std::vector<u32> before;
    for (u32 y = 18; y < 26; ++y) {
        for (u32 x = 12; x < 24; ++x) {
            before.push_back(read_pixel(t.r, rp, x, y));
        }
    }
    t.r.put32(si + si_Flags, DTSIF_HIGHLIGHT);
    t.r.put8(rp + rp_FgPen, 1);
    t.send(object, {DTM_SELECT, info, 0, 0});
    // The box covers columns 14 to 21 and rows 20 to 23 of the window; the
    // stipple's rows alternate $4444 and $1111 from row 0 of the RastPort.
    u32 at = 0;
    for (u32 y = 18; y < 26; ++y) {
        for (u32 x = 12; x < 24; ++x, ++at) {
            const bool inside = x >= 14 && x < 22 && y >= 20 && y < 24;
            const bool dot = (y % 2 == 0 ? x % 4 == 1 : x % 4 == 3);
            CHECK_EQ(read_pixel(t.r, rp, x, y), inside && dot ? 2u : before[at]);
        }
    }
    CHECK_EQ(t.r.get8(rp + rp_FgPen), 1u);
    CHECK_EQ(t.r.get32(rp + rp_AreaPtrn), 0u);
    // Cleared, the picture is drawn again without it.
    t.send(object, {DTM_CLEARSELECTED, info});
    CHECK_EQ(t.r.get32(si + si_Flags) & DTSIF_HIGHLIGHT, 0u);
    at = 0;
    for (u32 y = 18; y < 26; ++y) {
        for (u32 x = 12; x < 24; ++x, ++at) {
            CHECK_EQ(read_pixel(t.r, rp, x, y), before[at]);
        }
    }
    // Neither goes to datatypesclass.
    CHECK_EQ(t.s.count(DTM_SELECT), 0u);
    t.r.intuition.free_gadget_info(info);
    t.bo().dispose_object(object);
}

void set_renders_when_the_view_moves_and_answers_0() {
    Rig t;
    const u32 screen = open_planar_screen(t.r, 3);
    const u32 window = open_window(t.r, screen);
    const u32 object = t.load(64, 32, 2, {0, 0xFFFFFF, 0xFF0000, 0x00FF00});
    lay_out(t, object, window, 1);
    const u32 info = t.r.intuition.make_gadget_info(window);
    t.s.methods.clear();
    CHECK_EQ(t.set(object, {{DTA_TopVert, 3}}, info), 0u);
    CHECK_EQ(t.s.count(GM_RENDER), 1u);
    CHECK_EQ(t.set(object, {{DTA_TopVert, 3}}, info), 0u);
    CHECK_EQ(t.s.count(GM_RENDER), 1u);
    t.r.intuition.free_gadget_info(info);
    t.bo().dispose_object(object);
}

void frame_box_copies_the_frame_info() {
    Rig t;
    const u32 object = t.load(64, 32, 2, {0, 0xFFFFFF, 0xFF0000, 0x00FF00});
    for (u32 i = 0; i < 40; ++i) {
        t.r.put8(kMsg + 0x40 + i, 0xAA);
    }
    // datatypesclass's answer plus one, as the disk's class gives it.
    CHECK_EQ(t.send(object, {DTM_FRAMEBOX, 0, 0, kMsg + 0x40, 36, 0}), 1u);
    g_frame_box_result = 4;
    CHECK_EQ(t.send(object, {DTM_FRAMEBOX, 0, 0, kMsg + 0x40, 36, 0}), 5u);
    g_frame_box_result = 0;
    CHECK_EQ(t.r.get32(kMsg + 0x40 + 12), 64u);
    CHECK_EQ(t.r.get32(kMsg + 0x40 + 20), 2u);
    CHECK_EQ(t.r.get8(kMsg + 0x40 + 36), 0xAAu);
    for (u32 i = 0; i < 36; ++i) {
        t.r.put8(kMsg + 0x40 + i, 0xAA);
    }
    CHECK_EQ(t.send(object, {DTM_FRAMEBOX, 0, 0, kMsg + 0x40, 36, 1}), 1u);
    CHECK_EQ(t.r.get32(kMsg + 0x40 + 12), 0xAAAAAAAAu);
    t.bo().dispose_object(object);
}

void deep_screen_gets_the_colours_themselves() {
    Rig t;
    display_modes::configure_hombre(1280, 800, 8 * kMiB);
    const u32 tags = t.r.tags(kTags + 0x100, {{SA_DisplayID, display_modes::hombre_id(0, 2)},
                                              {SA_Depth, 24}, {0, 0}});
    const u32 screen = t.r.intuition.open_screen(0, tags);
    CHECK(screen != 0);
    const u32 window = open_window(t.r, screen);
    const u32 object = t.load(40, 10, 2, {0x102030, 0xFFFFFF, 0x900000, 0x00FF00});
    lay_out(t, object, window, 1);
    const u32 dest = t.get(object, PDTA_DestBitMap);
    const DeepBitmap* deep = t.r.graphics.deep(dest);
    CHECK(deep != nullptr);
    CHECK(deep->fmt == PixFmt::Argb32);
    CHECK_EQ(t.r.get32(deep->pixels), 0xFF102030u);
    CHECK_EQ(t.r.get32(deep->pixels + 4 * 4), 0xFF900000u);
    CHECK_EQ(t.r.get32(deep->pixels + 4 * 5), 0xFF900000u);
    CHECK_EQ(t.r.get32(deep->pixels + deep->bytes_per_row), 0xFFFFFFFFu);
    CHECK_EQ(t.get(object, PDTA_NumAlloc), 0u);
    const u32 gregs = t.get(object, PDTA_GRegs);
    CHECK_EQ(t.r.get32(gregs + 24), 0x90909090u);
    CHECK(gfx_test::logged("picture.datatype: 40x10x2 on a deep bitmap of 32 bits"));
    // Drawn into the deep window.
    const u32 rp = t.r.get32(window + wd_RPort);
    const u32 info = t.r.intuition.make_gadget_info(window);
    t.send(object, {GM_RENDER, info, rp, GREDRAW_REDRAW});
    const DeepBitmap* shown = t.r.graphics.deep(t.r.get32(rp + rp_BitMap));
    CHECK(shown != nullptr);
    CHECK_EQ(t.r.get32(shown->pixels) & 0xFFFFFF, 0x102030u);
    t.r.intuition.free_gadget_info(info);
    t.bo().dispose_object(object);
    display_modes::configure_hombre(0, 0, 0);
}

u32 be32(const std::vector<u8>& v, u32 at) {
    return (u32{v[at]} << 24) | (u32{v[at + 1]} << 16) | (u32{v[at + 2]} << 8) | v[at + 3];
}

// The offset of a chunk's data in an ILBM file, 0 if it has none.
u32 chunk(const std::vector<u8>& file, const char* id, u32& size) {
    for (u32 at = 12; at + 8 <= file.size();) {
        size = be32(file, at + 4);
        if (std::memcmp(&file[at], id, 4) == 0) {
            return at + 8;
        }
        at += 8 + size + (size & 1);
    }
    return 0;
}

void write_saves_an_ilbm_of_the_source() {
    Rig t;
    const u32 object = t.load(20, 3, 2, {0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00});
    CHECK_EQ(t.send(object, {DTM_WRITE, 0, 0x1234, 0, 0}), 1u);
    CHECK_EQ(g_write_file, 0x1234u);
    CHECK(gfx_test::logged("picture.datatype: wrote 20x3x2"));
    const std::vector<u8>& f = g_written;
    CHECK(f.size() > 12);
    CHECK_EQ(be32(f, 0), 0x464F524Du);
    CHECK_EQ(be32(f, 4), f.size() - 8);
    CHECK_EQ(be32(f, 12), 0x424D4844u);  // BMHD first
    u32 size = 0;
    const u32 bmhd = chunk(f, "BMHD", size);
    CHECK_EQ(f[bmhd + 1], 20u);
    CHECK_EQ(f[bmhd + 3], 3u);
    CHECK_EQ(f[bmhd + 8], 2u);
    CHECK_EQ(f[bmhd + 10], 1u);
    CHECK_EQ(f[bmhd + 14], 10u);
    const u32 camg = chunk(f, "CAMG", size);
    CHECK(camg != 0);
    CHECK_EQ(be32(f, camg), 0x8000u);
    const u32 cmap = chunk(f, "CMAP", size);
    CHECK_EQ(size, 12u);
    CHECK_EQ(f[cmap + 3], 0xFFu);
    CHECK_EQ(f[cmap + 6], 0xFFu);
    const u32 name = chunk(f, "NAME", size);
    CHECK_EQ(size, 4u);
    CHECK(std::memcmp(&f[name], "pic", 4) == 0);
    const u32 body = chunk(f, "BODY", size);
    CHECK(body != 0);
    // Row 0, plane 0: colours 0 0 1 1 2 2 3 3 ...: bits 0011 0011 ...
    std::vector<u8> unpacked;
    for (u32 i = body; i < body + size;) {
        const i8 n = static_cast<i8>(f[i++]);
        if (n >= 0) {
            for (i32 k = 0; k <= n; ++k) {
                unpacked.push_back(f[i++]);
            }
        } else {
            for (i32 k = 0; k < 1 - n; ++k) {
                unpacked.push_back(f[i]);
            }
            ++i;
        }
    }
    CHECK_EQ(unpacked.size(), 3u * 2u * 4u);
    CHECK_EQ(unpacked[0], 0x33u);
    CHECK_EQ(unpacked[4], 0x0Fu);  // plane 1 of row 0

    // A selection is written alone.
    g_written.clear();
    const u32 si = t.r.get32(object + gg_SpecialInfo);
    const u32 data = t.s.data(object);
    t.r.put16(data + dt_Select, 2);
    t.r.put16(data + dt_Select + 2, 1);
    t.r.put16(data + dt_Select + 4, 8);
    t.r.put16(data + dt_Select + 6, 2);
    t.r.put32(si + si_Flags, DTSIF_HIGHLIGHT);
    CHECK_EQ(t.send(object, {DTM_WRITE, 0, 0x1234, 0, 0}), 1u);
    const u32 cut = chunk(g_written, "BMHD", size);
    CHECK_EQ(g_written[cut + 1], 8u);
    CHECK_EQ(g_written[cut + 3], 2u);
    t.bo().dispose_object(object);
}

void dispose_gives_back_what_the_object_took() {
    Rig t;
    const u32 screen = open_planar_screen(t.r, 3);
    const u32 window = open_window(t.r, screen);
    // One object made, laid out and freed first, so that what stays (the
    // class's own memory, the screen's DrawInfo) is out of the count.
    const u32 first = t.load(8, 2, 1, {0, 0xFFFFFF});
    lay_out(t, first, window, 1);
    t.bo().dispose_object(first);
    const u32 before = t.r.exec.free_bytes();
    const u32 object = t.load(64, 32, 2, {0, 0xFFFFFF, 0x900000, 0x00FF00});
    lay_out(t, object, window, 1);
    CHECK(t.get(object, PDTA_NumAlloc) != 0);
    t.bo().dispose_object(object);
    CHECK_EQ(t.r.exec.free_bytes(), before);
}


// A picture neither laid out yet nor drawn while the layout holds it: the
// DTSIF_LAYOUT flag or the lock held by another task leaves the window as
// it is, and GM_RENDER still reaches datatypesclass.
void drawing_waits_for_no_layout() {
    Rig t;
    const u32 screen = open_planar_screen(t.r, 3);
    const u32 window = open_window(t.r, screen);
    const u32 rp = t.r.get32(window + wd_RPort);
    const u32 object = t.load(64, 32, 2, {0, 0xFFFFFF, 0xFF0000, 0x00FF00}, 10, 20);
    lay_out(t, object, window, 1);
    const u32 si = t.r.get32(object + gg_SpecialInfo);
    const u32 info = t.r.intuition.make_gadget_info(window);
    const u32 pen = planar_pixel(t.r, t.get(object, PDTA_DestBitMap), 2, 0);
    CHECK(pen != 0);
    t.s.methods.clear();
    t.r.put32(si + si_Flags, 1);
    t.send(object, {GM_RENDER, info, rp, GREDRAW_REDRAW});
    CHECK_EQ(read_pixel(t.r, rp, 12, 20), 0u);
    t.r.put32(si + si_Flags, 0);
    t.r.put32(si + SS_OWNER, 0x7000);
    t.r.put16(si + SS_NESTCOUNT, 1);
    t.send(object, {GM_RENDER, info, rp, GREDRAW_REDRAW});
    CHECK_EQ(read_pixel(t.r, rp, 12, 20), 0u);
    CHECK_EQ(t.s.count(GM_RENDER), 2u);
    t.r.put32(si + SS_OWNER, 0);
    t.r.put16(si + SS_NESTCOUNT, 0);
    t.send(object, {GM_RENDER, info, rp, GREDRAW_REDRAW});
    CHECK_EQ(read_pixel(t.r, rp, 12, 20), pen);
    CHECK_EQ(t.r.get16(si + SS_NESTCOUNT), 0u);
    // DTM_WRITE holds the lock while it writes, so that no layout frees
    // the source under it.
    g_watched_lock = si;
    CHECK_EQ(t.send(object, {DTM_WRITE, 0, 0x1234, 0, 0}), 1u);
    g_watched_lock = 0;
    CHECK_EQ(g_lock_nest_seen, 1u);
    CHECK_EQ(t.r.get16(si + SS_NESTCOUNT), 0u);
    t.r.intuition.free_gadget_info(info);
    t.bo().dispose_object(object);
}

// A destination that cannot be had leaves the picture unremapped and says
// so in the layout's notification.
void a_failed_remap_is_reported() {
    Rig t;
    const u32 screen = open_planar_screen(t.r, 3);
    const u32 window = open_window(t.r, screen);
    const u32 object = t.load(40, 10, 2, {0, 0xFFFFFF, 0x900000, 0x00FF00});
    const u32 bmh = t.get(object, PDTA_BitMapHeader);
    t.r.put16(bmh, 0x7FFF);
    t.r.put16(bmh + 2, 0x7FFF);
    t.s.notifies.clear();
    lay_out(t, object, window, 1);
    CHECK_EQ(t.get(object, PDTA_DestBitMap), 0u);
    CHECK(notified(t.s, DTA_ErrorLevel, 20));
    CHECK(notified(t.s, DTA_ErrorNumber, 103));
    CHECK(gfx_test::logged("picture.datatype: 32767x32767x2 not remapped, error 103"));
    // A layout without the GadgetInfo's screen and with PDTA_Remap set
    // makes nothing and reports nothing wrong.
    const u32 info = t.r.intuition.make_gadget_info(window);
    t.r.put16(bmh, 40);
    t.r.put16(bmh + 2, 10);
    t.s.notifies.clear();
    CHECK_EQ(t.send(object, {DTM_ASYNCLAYOUT, 0, 1}), 1u);
    CHECK_EQ(t.get(object, PDTA_DestBitMap), 0u);
    CHECK(notified(t.s, DTA_ErrorLevel, 0));
    t.r.intuition.free_gadget_info(info);
    t.bo().dispose_object(object);
}

void free_source_bitmap_frees_the_source_after_the_remap() {
    Rig t;
    const u32 screen = open_planar_screen(t.r, 3);
    const u32 window = open_window(t.r, screen);
    const u32 first = t.load(8, 2, 1, {0, 0xFFFFFF});
    lay_out(t, first, window, 1);
    t.bo().dispose_object(first);
    const u32 before = t.r.exec.free_bytes();
    const u32 object = t.load(64, 32, 2, {0, 0xFFFFFF, 0x900000, 0x00FF00});
    t.set(object, {{PDTA_FreeSourceBitMap, 1}});
    const u32 holding = t.r.exec.free_bytes();
    lay_out(t, object, window, 1);
    const u32 dest = t.get(object, PDTA_DestBitMap);
    CHECK(dest != 0);
    // The source's two planes and its BitMap went; the destination's three
    // planes came.
    CHECK(t.r.exec.free_bytes() < holding);
    // Nothing is left to save.
    CHECK_EQ(t.send(object, {DTM_WRITE, 0, 0x1234, 0, 0}), 0u);
    CHECK(gfx_test::logged("picture.datatype: could not write, no planar source bitmap"));
    // A second first layout has no source to remap and keeps what it has.
    lay_out(t, object, window, 1);
    CHECK_EQ(t.get(object, PDTA_DestBitMap), dest);
    t.bo().dispose_object(object);
    CHECK_EQ(t.r.exec.free_bytes(), before);
}

// Without remapping, a PDTA_Screen deeper than the picture gets a copy of
// the screen's depth; the picture's own pens stay.
void without_remap_a_deeper_screen_gets_a_copy() {
    Rig t;
    const u32 screen = open_planar_screen(t.r, 3);
    const u32 window = open_window(t.r, screen);
    const u32 object = t.load(16, 4, 2, {0, 0xFFFFFF, 0xFF0000, 0x00FF00});
    t.set(object, {{PDTA_Remap, 0}, {PDTA_Screen, screen}});
    lay_out(t, object, window, 1);
    const u32 dest = t.get(object, PDTA_DestBitMap);
    CHECK(dest != 0);
    CHECK_EQ(t.r.get8(dest + bm_Depth), 3u);
    for (u32 x = 0; x < 16; ++x) {
        CHECK_EQ(planar_pixel(t.r, dest, x, 1), (x / 2 + 1) % 4);
    }
    CHECK_EQ(t.get(object, PDTA_NumAlloc), 0u);
    t.bo().dispose_object(object);
}

u32 open_hombre_screen(Rig& t, u32 depth_code) {
    display_modes::configure_hombre(1280, 800, 8 * kMiB);
    const u32 tags = t.r.tags(kTags + 0x100,
                              {{SA_DisplayID, display_modes::hombre_id(0, depth_code)},
                               {SA_Depth, 8}, {0, 0}});
    const u32 screen = t.r.intuition.open_screen(0, tags);
    CHECK(screen != 0);
    return screen;
}

// A chunky screen of pens gets pens; one of 16 bits each pixel's colour.
void lut8_and_rgb16_screens() {
    {
        Rig t;
        const u32 screen = open_hombre_screen(t, 0);
        const u32 window = open_window(t.r, screen);
        const u32 object = t.load(40, 10, 2, {0x102030, 0xFFFFFF, 0x900000, 0x00FF00});
        lay_out(t, object, window, 1);
        const DeepBitmap* deep = t.r.graphics.deep(t.get(object, PDTA_DestBitMap));
        CHECK(deep != nullptr);
        CHECK(deep->fmt == PixFmt::Lut8);
        CHECK(t.get(object, PDTA_NumAlloc) != 0);
        const u32 table = t.get(object, PDTA_ColorTable);
        CHECK_EQ(t.r.get8(deep->pixels), t.r.get8(table));
        CHECK_EQ(t.r.get8(deep->pixels + 2), t.r.get8(table + 1));
        t.bo().dispose_object(object);
        display_modes::configure_hombre(0, 0, 0);
    }
    {
        Rig t;
        const u32 screen = open_hombre_screen(t, 1);
        const u32 window = open_window(t.r, screen);
        const u32 object = t.load(40, 10, 2, {0x102030, 0xFFFFFF, 0x900000, 0x00FF00});
        lay_out(t, object, window, 1);
        const DeepBitmap* deep = t.r.graphics.deep(t.get(object, PDTA_DestBitMap));
        CHECK(deep != nullptr);
        CHECK(deep->fmt == PixFmt::Rgb16);
        CHECK_EQ(t.r.get16(deep->pixels), encode_rgb(PixFmt::Rgb16, 0x10, 0x20, 0x30));
        CHECK_EQ(t.r.get16(deep->pixels + 2 * 4), encode_rgb(PixFmt::Rgb16, 0x90, 0, 0));
        CHECK_EQ(t.get(object, PDTA_NumAlloc), 0u);
        CHECK(gfx_test::logged("picture.datatype: 40x10x2 on a deep bitmap of 16 bits"));
        t.bo().dispose_object(object);
        display_modes::configure_hombre(0, 0, 0);
    }
}

// A picture with a mask plane is written without one, and a selection
// keeps its corner in the header.
void write_drops_the_mask_and_keeps_the_corner() {
    Rig t;
    const u32 object = t.load(20, 3, 2, {0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00});
    t.r.put8(t.get(object, PDTA_BitMapHeader) + 9, 1);
    const u32 si = t.r.get32(object + gg_SpecialInfo);
    const u32 data = t.s.data(object);
    t.r.put16(data + dt_Select, 3);
    t.r.put16(data + dt_Select + 2, 1);
    t.r.put16(data + dt_Select + 4, 5);
    t.r.put16(data + dt_Select + 6, 2);
    t.r.put32(si + si_Flags, DTSIF_HIGHLIGHT);
    CHECK_EQ(t.send(object, {DTM_WRITE, 0, 0x1234, 0, 0}), 1u);
    u32 size = 0;
    const u32 bmhd = chunk(g_written, "BMHD", size);
    CHECK_EQ(g_written[bmhd + 1], 5u);
    CHECK_EQ(g_written[bmhd + 3], 2u);
    CHECK_EQ(g_written[bmhd + 5], 3u);
    CHECK_EQ(g_written[bmhd + 7], 1u);
    CHECK_EQ(g_written[bmhd + 9], 0u);
    const u32 body = chunk(g_written, "BODY", size);
    std::vector<u8> rows;
    for (u32 i = body; i < body + size;) {
        const i8 n = static_cast<i8>(g_written[i++]);
        if (n >= 0) {
            for (i32 k = 0; k <= n; ++k) {
                rows.push_back(g_written[i++]);
            }
        } else {
            for (i32 k = 0; k < 1 - n; ++k) {
                rows.push_back(g_written[i]);
            }
            ++i;
        }
    }
    CHECK_EQ(rows.size(), 8u);
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 5; ++x) {
            u32 index = 0;
            for (u32 plane = 0; plane < 2; ++plane) {
                index |= ((rows[(y * 2 + plane) * 2 + x / 8] >> (7 - (x & 7))) & 1) << plane;
            }
            CHECK_EQ(index, ((x + 3) / 2 + y + 1) % 4);
        }
    }
    t.bo().dispose_object(object);
}

// A display mode the database does not have becomes the best one that
// shows the picture; the frame takes its properties.
void an_unknown_mode_becomes_the_best_one() {
    Rig t;
    const u32 object = t.load(40, 10, 2, {0, 0xFFFFFF, 0xFF0000, 0x00FF00});
    t.set(object, {{PDTA_ModeID, 0x7FF00000}});
    const u32 mode = t.get(object, PDTA_ModeID);
    CHECK(mode != 0x7FF00000u);
    CHECK(mode != 0xFFFFFFFFu);
    const u32 info = t.s.data(object) + dt_FrameInfo;
    CHECK(t.r.get32(info + 4) != 0);  // fri_Resolution
    t.bo().dispose_object(object);
}

void a_replaced_bitmap_is_freed() {
    Rig t;
    const u32 screen = open_planar_screen(t.r, 3);
    const u32 window = open_window(t.r, screen);
    const u32 first = t.load(8, 2, 1, {0, 0xFFFFFF});
    lay_out(t, first, window, 1);
    t.bo().dispose_object(first);
    const u32 before = t.r.exec.free_bytes();
    const u32 object = t.load(64, 32, 2, {0, 0xFFFFFF, 0x900000, 0x00FF00});
    // Shown as it is, the destination is the bitmap that goes.
    t.set(object, {{PDTA_Remap, 0}});
    lay_out(t, object, window, 1);
    CHECK(t.get(object, PDTA_DestBitMap) != 0);
    t.set(object, {{PDTA_BitMap, t.alloc_bitmap(64, 32, 2)}});
    CHECK_EQ(t.get(object, PDTA_DestBitMap), 0u);
    t.bo().dispose_object(object);
    CHECK_EQ(t.r.exec.free_bytes(), before);
}

// A run that ends inside a call the class makes stays ended: nothing the
// class does after it runs, and nothing is notified.
void an_ended_run_stays_ended() {
    Rig t;
    const u32 screen = open_planar_screen(t.r, 3);
    const u32 window = open_window(t.r, screen);
    const u32 object = t.load(16, 4, 1, {0, 0xFFFFFF});
    const u32 info = t.r.intuition.make_gadget_info(window);
    g_fail_obj_name = true;
    t.s.notifies.clear();
    t.s.methods.clear();
    CHECK_EQ(t.send(object, {DTM_ASYNCLAYOUT, info, 1}), 0u);
    g_fail_obj_name = false;
    CHECK_EQ(t.r.cpu().exit, ExitReason::BusError);
    CHECK_EQ(t.s.notifies.size(), 0u);
    CHECK_EQ(t.s.count(OM_GET), 1u);
    CHECK_EQ(t.get(object, PDTA_DestBitMap), 0u);
    t.r.intuition.free_gadget_info(info);
}

}

int main() {
    opens_as_the_class_datatypes_library_asks_for();
    does_not_open_without_datatypesclass();
    new_object_has_storage_for_its_subclass();
    methods_it_leaves_go_on_without_a_run_of_their_own();
    first_layout_remaps_for_a_planar_screen();
    sparse_table_limits_the_pens();
    without_remap_the_source_is_shown();
    render_draws_from_the_scroll_offsets_and_marks_the_selection();
    set_renders_when_the_view_moves_and_answers_0();
    frame_box_copies_the_frame_info();
    deep_screen_gets_the_colours_themselves();
    write_saves_an_ilbm_of_the_source();
    dispose_gives_back_what_the_object_took();
    drawing_waits_for_no_layout();
    a_failed_remap_is_reported();
    free_source_bitmap_frees_the_source_after_the_remap();
    without_remap_a_deeper_screen_gets_a_copy();
    lut8_and_rgb16_screens();
    write_drops_the_mask_and_keeps_the_corner();
    an_unknown_mode_becomes_the_best_one();
    a_replaced_bitmap_is_freed();
    an_ended_run_stays_ended();
    return check::finish("test_picture_dt");
}
