#pragma once

// The hook from the queue proxy's CopyPageMappingsBatchX (d3d12_xs_device.cpp) to xmem.cpp's page aliasing, kept in
// a header so neither file needs the other at link time (standalone test targets compile one without the other).
// xmem.cpp stores the function; a null hook means xmem.cpp is not linked in.
//
// bool map_alias(to, from, len): maps the physical pages shown at [from, from + len) at `to` as well, in place of
// what `to` shows; `from` 0 unmaps [to, to + len). Addresses and length are multiples of 64 KiB. False on failure.

#include <atomic>
#include <cstdint>

namespace rt::xmem {

using MapAliasFn = bool (*)(std::uint64_t to, std::uint64_t from, std::uint64_t len);

inline std::atomic<MapAliasFn>& map_alias_hook() {
    static std::atomic<MapAliasFn> hook{nullptr};
    return hook;
}

// agA10 hack: copies n bytes to the physical pages mapped at guest address `va` (through the section, so the view's
// protection does not matter). False, with nothing written past the failing page, if a page there is not mapped.
using WritePhysFn = bool (*)(std::uint64_t va, const void* src, std::uint64_t n);

inline std::atomic<WritePhysFn>& write_phys_hook() {
    static std::atomic<WritePhysFn> hook{nullptr};
    return hook;
}

// agA11 hack: copies n bytes into physical page `page` (an XMemAllocatePhysicalPages page number = 4K frame number of a
// 64K page) at byte offset `off` (off + n <= 64 KiB), through the section. False if the page number is unknown.
using WritePageFn = bool (*)(std::uint64_t page, std::uint64_t off, const void* src, std::uint64_t n);

inline std::atomic<WritePageFn>& write_page_hook() {
    static std::atomic<WritePageFn> hook{nullptr};
    return hook;
}

// agA18 hack (LEONIDA_CE_HEAP_MIRROR=2): d3d12's write tracker for the console descriptor heap. xmem's
// physical_page_fault calls it first, so a write to a chunk the tracker made read-only is not mistaken for an
// already-committed page (which would return CONTINUE_EXECUTION and fault forever).
using WriteTrackFn = long (__stdcall*)(struct _EXCEPTION_POINTERS*);

inline std::atomic<WriteTrackFn>& write_track_hook() {
    static std::atomic<WriteTrackFn> hook{nullptr};
    return hook;
}

// agA24 hack (LEONIDA_CE_POOL_GPU): told about every 64 KiB physical page xmem maps (host = the page's bytes in its
// section's scratch view) or unmaps (host = null) at a guest address. Called with xmem's lock held: keep it cheap.
using PageMapFn = void (*)(std::uint64_t address, void* host);
inline std::atomic<PageMapFn>& page_map_hook() {
    static std::atomic<PageMapFn> hook{nullptr};
    return hook;
}
// agA24: told when xmem_write_page / xmem_write_phys (dstorage's page paths) wrote bytes into the 64 KiB host page `host`
// (its scratch-view address), so a GPU copy of that page can be refreshed.
using PageDirtyFn = void (*)(void* host);
inline std::atomic<PageDirtyFn>& page_dirty_hook() {
    static std::atomic<PageDirtyFn> hook{nullptr};
    return hook;
}
// Calls fn for every page mapped now in [lo, hi) (to catch up a hook installed late).
using ForEachMappedFn = void (*)(std::uint64_t lo, std::uint64_t hi, PageMapFn fn);
inline std::atomic<ForEachMappedFn>& for_each_mapped_hook() {
    static std::atomic<ForEachMappedFn> hook{nullptr};
    return hook;
}

}  // namespace rt::xmem
