#include "boot_info.hpp"
#include "check.hpp"
#include "memmap.hpp"

#include <cstddef>
#include <cstring>
#include <vector>

using namespace hombre;

// The kernel reads these structures as written by a loader built with a
// different compiler, so their layout is part of the interface.
static_assert(sizeof(MemoryRegion) == 24);
static_assert(offsetof(MemoryRegion, phys) == 0);
static_assert(offsetof(MemoryRegion, pages) == 8);
static_assert(offsetof(MemoryRegion, type) == 16);
static_assert(sizeof(FramebufferInfo) == 24);
static_assert(offsetof(FramebufferInfo, width) == 8);
static_assert(offsetof(FramebufferInfo, format) == 20);
static_assert(sizeof(BootInfo) == 80);
static_assert(offsetof(BootInfo, magic) == 0);
static_assert(offsetof(BootInfo, version) == 8);
static_assert(offsetof(BootInfo, region_count) == 12);
static_assert(offsetof(BootInfo, regions) == 16);
static_assert(offsetof(BootInfo, fb) == 24);
static_assert(offsetof(BootInfo, rsdp_phys) == 48);
static_assert(offsetof(BootInfo, direct_map_base) == 56);
static_assert(offsetof(BootInfo, kernel_phys_start) == 64);
static_assert(offsetof(BootInfo, kernel_phys_end) == 72);
static_assert(boot_info_magic == 0x484F4D4252450001ull);
static_assert(boot_info_version == 1);
static_assert(kDirectMapBase == 0xFFFF800000000000ull);
static_assert(kKernelBase == 0xFFFFFFFF80000000ull);
static_assert(sizeof(EfiMemoryDescriptor) == 40);

namespace {

// UEFI memory type numbers, spelled out here so a wrong value in the
// converter cannot be mirrored by the test.
constexpr u32 kReservedType = 0;
constexpr u32 kLoaderCode = 1;
constexpr u32 kLoaderData = 2;
constexpr u32 kBootServicesCode = 3;
constexpr u32 kBootServicesData = 4;
constexpr u32 kRuntimeServicesCode = 5;
constexpr u32 kRuntimeServicesData = 6;
constexpr u32 kConventional = 7;
constexpr u32 kUnusable = 8;
constexpr u32 kAcpiReclaim = 9;
constexpr u32 kAcpiNvs = 10;
constexpr u32 kMmio = 11;
constexpr u32 kMmioPortSpace = 12;
constexpr u32 kPalCode = 13;
constexpr u32 kPersistent = 14;
constexpr u32 kUnaccepted = 15;

constexpr u64 kPage = 0x1000;

// Lays descriptors out the way GetMemoryMap does, with a caller-chosen
// stride. The bytes past each descriptor are filled with junk so that a
// converter reading them would produce wrong results.
class MapBuilder {
public:
    explicit MapBuilder(u64 stride = sizeof(EfiMemoryDescriptor)) : stride_(stride) {}

    MapBuilder& add(u32 type, u64 phys, u64 pages) {
        const usize offset = bytes_.size();
        bytes_.resize(offset + stride_, 0xA5);
        EfiMemoryDescriptor d{};
        d.type = type;
        d.phys = phys;
        d.virt = 0xDEAD0000;
        d.pages = pages;
        d.attribute = 0xF;
        std::memcpy(bytes_.data() + offset, &d, sizeof(d));
        return *this;
    }

    const u8* data() const { return bytes_.data(); }
    u64 size() const { return bytes_.size(); }
    u64 stride() const { return stride_; }

private:
    u64 stride_;
    std::vector<u8> bytes_;
};

// Converts into a heap array of exactly `capacity` entries so that ASan
// flags any write past it.
std::vector<MemoryRegion> convert(const MapBuilder& map, u32 capacity, u32& count,
                                  u64 map_bytes = ~0ull) {
    std::vector<MemoryRegion> out(capacity);
    const u64 bytes = map_bytes == ~0ull ? map.size() : map_bytes;
    count = convert_efi_map(map.data(), bytes, map.stride(), capacity ? out.data() : nullptr,
                            capacity);
    return out;
}

bool region_is(const MemoryRegion& r, u64 phys, u64 pages, MemType type) {
    return r.phys == phys && r.pages == pages && r.type == type && r.reserved == 0;
}

void merges_adjacent_usable_regions() {
    MapBuilder map;
    map.add(kConventional, 0, 16)
        .add(kBootServicesData, 16 * kPage, 4)
        .add(kBootServicesCode, 20 * kPage, 4)
        .add(kConventional, 24 * kPage, 8)
        // A gap before this one.
        .add(kConventional, 64 * kPage, 2)
        // Adjacent, but a different type.
        .add(kReservedType, 66 * kPage, 1)
        .add(kConventional, 67 * kPage, 1);

    u32 count = 0;
    const auto out = convert(map, 8, count);
    CHECK_EQ(count, 4u);
    CHECK(region_is(out[0], 0, 32, MemType::Usable));
    CHECK(region_is(out[1], 64 * kPage, 2, MemType::Usable));
    CHECK(region_is(out[2], 66 * kPage, 1, MemType::Reserved));
    CHECK(region_is(out[3], 67 * kPage, 1, MemType::Usable));
}

void honours_descriptor_size_larger_than_struct() {
    MapBuilder map(48);
    map.add(kConventional, 0x100000, 3).add(kAcpiNvs, 0x200000, 5).add(kMmio, 0xFEC00000, 1);

    u32 count = 0;
    const auto out = convert(map, 3, count);
    CHECK_EQ(count, 3u);
    CHECK(region_is(out[0], 0x100000, 3, MemType::Usable));
    CHECK(region_is(out[1], 0x200000, 5, MemType::AcpiNvs));
    CHECK(region_is(out[2], 0xFEC00000, 1, MemType::Mmio));
}

void honours_a_stride_with_an_odd_tail() {
    // 52 bytes per descriptor leaves a 12-byte tail, so every second
    // descriptor starts on a 4-byte boundary only.
    MapBuilder map(52);
    map.add(kConventional, 0, 1)
        .add(kLoaderData, kPage, 1)
        .add(kAcpiReclaim, 2 * kPage, 7)
        .add(kConventional, 9 * kPage, 1);

    u32 count = 0;
    const auto out = convert(map, 4, count);
    CHECK_EQ(count, 4u);
    CHECK(region_is(out[0], 0, 1, MemType::Usable));
    CHECK(region_is(out[1], kPage, 1, MemType::Loader));
    CHECK(region_is(out[2], 2 * kPage, 7, MemType::AcpiReclaimable));
    CHECK(region_is(out[3], 9 * kPage, 1, MemType::Usable));
}

void marks_loader_regions_as_loader() {
    MapBuilder map;
    map.add(kConventional, 0, 0x100)
        .add(kLoaderData, 0x100 * kPage, 0x20)
        .add(kLoaderCode, 0x120 * kPage, 0x10)
        .add(kLoaderData, 0x130 * kPage, 0x30)
        .add(kConventional, 0x160 * kPage, 0x40);

    u32 count = 0;
    const auto out = convert(map, 3, count);
    CHECK_EQ(count, 3u);
    CHECK(region_is(out[0], 0, 0x100, MemType::Usable));
    // Loader code and data next to each other become one Loader region.
    CHECK(region_is(out[1], 0x100 * kPage, 0x60, MemType::Loader));
    CHECK(region_is(out[2], 0x160 * kPage, 0x40, MemType::Usable));
}

void returns_zero_when_capacity_too_small() {
    MapBuilder map;
    map.add(kConventional, 0, 1).add(kReservedType, kPage, 1).add(kConventional, 2 * kPage, 1);

    u32 count = 7;
    convert(map, 2, count);
    CHECK_EQ(count, 0u);
    count = 7;
    convert(map, 0, count);
    CHECK_EQ(count, 0u);
}

void accepts_capacity_equal_to_the_needed_count() {
    MapBuilder map;
    // Five descriptors that merge into three regions.
    map.add(kConventional, 0, 1)
        .add(kConventional, kPage, 1)
        .add(kReservedType, 2 * kPage, 1)
        .add(kConventional, 3 * kPage, 1)
        .add(kBootServicesData, 4 * kPage, 1);

    u32 count = 0;
    convert(map, 2, count);
    CHECK_EQ(count, 0u);
    const auto exact = convert(map, 3, count);
    CHECK_EQ(count, 3u);
    CHECK(region_is(exact[0], 0, 2, MemType::Usable));
    CHECK(region_is(exact[1], 2 * kPage, 1, MemType::Reserved));
    CHECK(region_is(exact[2], 3 * kPage, 2, MemType::Usable));
}

void maps_unknown_types_to_reserved() {
    const u32 reserved_types[] = {kReservedType, kRuntimeServicesCode, kRuntimeServicesData,
                                  kUnusable, kPalCode, kPersistent, kUnaccepted, 16, 0x70000000,
                                  0x80000000, 0xFFFFFFFF};
    MapBuilder map;
    u64 phys = 0;
    for (u32 type : reserved_types) {
        map.add(type, phys, 1);
        // Leave a gap so nothing merges.
        phys += 2 * kPage;
    }

    u32 count = 0;
    const u32 n = sizeof(reserved_types) / sizeof(reserved_types[0]);
    const auto out = convert(map, n, count);
    CHECK_EQ(count, n);
    for (u32 i = 0; i < n; ++i) {
        CHECK_EQ(out[i].type, MemType::Reserved);
    }
}

void maps_acpi_and_mmio_types() {
    MapBuilder map;
    map.add(kAcpiReclaim, 0, 1)
        .add(kAcpiNvs, 2 * kPage, 1)
        .add(kMmio, 4 * kPage, 1)
        .add(kMmioPortSpace, 5 * kPage, 1);

    u32 count = 0;
    const auto out = convert(map, 4, count);
    CHECK_EQ(count, 3u);
    CHECK(region_is(out[0], 0, 1, MemType::AcpiReclaimable));
    CHECK(region_is(out[1], 2 * kPage, 1, MemType::AcpiNvs));
    // Both MMIO kinds are Mmio and merge when adjacent.
    CHECK(region_is(out[2], 4 * kPage, 2, MemType::Mmio));
}

void ignores_a_trailing_partial_descriptor() {
    MapBuilder map(48);
    map.add(kConventional, 0, 1).add(kReservedType, 4 * kPage, 1).add(kAcpiNvs, 8 * kPage, 1);

    u32 count = 0;
    // Two whole descriptors and 47 bytes of the third.
    auto out = convert(map, 3, count, 3 * 48 - 1);
    CHECK_EQ(count, 2u);
    CHECK(region_is(out[1], 4 * kPage, 1, MemType::Reserved));

    // Less than one descriptor.
    out = convert(map, 3, count, 47);
    CHECK_EQ(count, 0u);
}

void skips_zero_length_descriptors() {
    MapBuilder map;
    map.add(kConventional, 0, 4)
        .add(kReservedType, 4 * kPage, 0)
        .add(kConventional, 4 * kPage, 4)
        .add(kAcpiNvs, 0x100000, 0);

    u32 count = 0;
    const auto out = convert(map, 1, count);
    CHECK_EQ(count, 1u);
    CHECK(region_is(out[0], 0, 8, MemType::Usable));
}

void drops_descriptors_that_wrap_the_address_space() {
    MapBuilder map;
    map.add(kConventional, 0, 1)
        .add(kReservedType, 0xFFFFFFFFFFFFF000ull, 1)
        .add(kConventional, 0x100000, 0x0010000000000000ull)
        .add(kConventional, 0xFFFFFFFFFFFFE000ull, 1);

    u32 count = 0;
    const auto out = convert(map, 2, count);
    CHECK_EQ(count, 2u);
    CHECK(region_is(out[0], 0, 1, MemType::Usable));
    CHECK(region_is(out[1], 0xFFFFFFFFFFFFE000ull, 1, MemType::Usable));
}

void keeps_unsorted_and_overlapping_descriptors_in_input_order() {
    // Out-of-order and overlapping input is passed through as given: only a
    // descriptor that starts exactly where the previous region ends is merged.
    MapBuilder map;
    map.add(kConventional, 8 * kPage, 4)
        .add(kConventional, 0, 8)            // precedes the previous one
        .add(kConventional, 4 * kPage, 8)    // overlaps both
        .add(kConventional, 12 * kPage, 2);  // adjacent to the previous one

    u32 count = 0;
    const auto out = convert(map, 4, count);
    CHECK_EQ(count, 3u);
    CHECK(region_is(out[0], 8 * kPage, 4, MemType::Usable));
    CHECK(region_is(out[1], 0, 8, MemType::Usable));
    CHECK(region_is(out[2], 4 * kPage, 10, MemType::Usable));
}

void rejects_bad_arguments() {
    MapBuilder map;
    map.add(kConventional, 0, 1);
    MemoryRegion out[2]{};

    CHECK_EQ(convert_efi_map(nullptr, map.size(), map.stride(), out, 2), 0u);
    CHECK_EQ(convert_efi_map(map.data(), map.size(), 0, out, 2), 0u);
    CHECK_EQ(convert_efi_map(map.data(), map.size(), sizeof(EfiMemoryDescriptor) - 1, out, 2),
             0u);
    CHECK_EQ(convert_efi_map(map.data(), map.size(), map.stride(), nullptr, 2), 0u);
    CHECK_EQ(convert_efi_map(map.data(), 0, map.stride(), out, 2), 0u);
}

}

int main() {
    merges_adjacent_usable_regions();
    honours_descriptor_size_larger_than_struct();
    honours_a_stride_with_an_odd_tail();
    marks_loader_regions_as_loader();
    returns_zero_when_capacity_too_small();
    accepts_capacity_equal_to_the_needed_count();
    maps_unknown_types_to_reserved();
    maps_acpi_and_mmio_types();
    ignores_a_trailing_partial_descriptor();
    skips_zero_length_descriptors();
    drops_descriptors_that_wrap_the_address_space();
    keeps_unsorted_and_overlapping_descriptors_in_input_order();
    rejects_bad_arguments();
    return check::finish("test_memmap");
}
