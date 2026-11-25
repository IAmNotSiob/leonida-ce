// agA9 hack (uncommitted): a guard on the title's texture-pool slot buddy allocator and pool-page free list.
// LEONIDA_CE_SLOT_GUARD: 0 off; 1 (default) cheap checks of the list heads before every allocator operation, a bad head
// is dropped; 2 also walks every buddy free list before every operation (cut at the first bad node) and the pool free
// list every 32 operations (rebuilt from the slot table when it is bad), and logs the last operations when the state
// first turns bad, which tells an allocator operation from an outside writer.
// Title globals (game image at its preferred base, BOOT_NOTES agA8/agA9):
//   slot table qword* 0x1471bfb08, slot count dword 0x1471bfb18, order list heads dword 0x1471bfb24 + 4*order (0..21),
//   entry: bits 0-14 refcount, 15-19 order (0xc0000 marker = sub-allocated), 20-40 pool page/prev, 41-61 next.
//   pool size qword 0x1471bfac8 (pages = >>16), pool links (21-bit, 3 per qword) qword* 0x1471bfcd0,
//   pool head/tail/free dwords 0x1471bfcd8/cdc/ce0.
// Hooked (direct callers): alloc 0x145238780(slots), free 0x14221b7d0(slot), decref 0x1400e8890(slot),
// suballoc 0x145238590(pages), subfree 0x144f8f0a0(page4k). The title calls all of them under its lock 0x1471bfad8.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "code_hooks.h"

namespace leonida::slot_guard {

constexpr uint32_t kNil = 0x1fffff;
inline int g_mode = 0;

inline uint64_t*& table() { return *reinterpret_cast<uint64_t**>(uintptr_t{0x1471bfb08}); }
inline uint32_t slot_count() { return *reinterpret_cast<volatile uint32_t*>(uintptr_t{0x1471bfb18}); }
inline volatile uint32_t* heads() { return reinterpret_cast<volatile uint32_t*>(uintptr_t{0x1471bfb24}); }
inline uint32_t pool_pages() { return static_cast<uint32_t>(*reinterpret_cast<volatile uint64_t*>(uintptr_t{0x1471bfac8}) >> 16); }
inline uint64_t*& pool_links() { return *reinterpret_cast<uint64_t**>(uintptr_t{0x1471bfcd0}); }
inline volatile uint32_t* pool_hdr() { return reinterpret_cast<volatile uint32_t*>(uintptr_t{0x1471bfcd8}); }

inline uint32_t pool_link(uint32_t i) {
    return static_cast<uint32_t>(pool_links()[i / 3] >> ((i % 3) * 21)) & kNil;
}
inline void set_pool_link(uint32_t i, uint32_t v) {
    uint64_t& q = pool_links()[i / 3];
    const unsigned sh = (i % 3) * 21;
    q = (q & ~(uint64_t{kNil} << sh)) | (uint64_t{v & kNil} << sh);
}

struct Op { uint64_t tick; uint32_t tid; char kind; uint32_t arg; };
inline Op g_ring[32];
inline uint32_t g_ring_n = 0;
inline uint64_t g_ops = 0;
inline bool g_was_bad = false;
inline int g_told = 0;

inline void dump_ring() {
    const uint32_t n = g_ring_n < 32 ? g_ring_n : 32;
    for (uint32_t k = n; k > 0; --k) {
        const Op& o = g_ring[(g_ring_n - k) % 32];
        std::fprintf(stderr, "[hack]   slot guard op -%u: %c(0x%x) tid %u tick %llu\n", k, o.kind, o.arg, o.tid,
                     (unsigned long long)o.tick);
    }
}

inline void tell(const char* what, int order, uint32_t node, uint32_t prev, uint64_t entry) {
    if (g_told >= 64) return;
    ++g_told;
    std::fprintf(stderr, "[hack] slot guard (op %llu): %s: order %d node 0x%x (after 0x%x) entry %016llx count 0x%x "
                 "table %p links %p%s\n", (unsigned long long)g_ops, what, order, node, prev, (unsigned long long)entry,
                 slot_count(), static_cast<void*>(table()), static_cast<void*>(pool_links()),
                 g_mode ? " -> cut" : "");
    if (!g_was_bad) {
        g_was_bad = true;
        std::fprintf(stderr, "[hack] slot guard: FIRST BAD STATE after these operations (oldest first):\n");
        dump_ring();
    }
    std::fflush(stderr);
}

// A block that can sit in the order-o list: in range, aligned, inside the slot count, order field o, not sub-allocated.
inline bool node_ok(uint32_t n, int o, uint32_t count, uint64_t e) {
    if (n >= count || (n & ((1u << o) - 1)) || uint64_t{n} + (uint64_t{1} << o) > count) return false;
    const uint32_t lo = static_cast<uint32_t>(e);
    const uint32_t field = (lo >> 15) & 0x1f;
    // A freed sub-allocated slot goes back to the order-0 list with its 0xc0000 marker kept (free path, 0x14221b7d0).
    return field == static_cast<uint32_t>(o) || (o == 0 && field >= 24);
}

// Rebuilds the pool free list from the slot table: a pool page is in use when an allocated slot (refcount non-zero)
// names it in bits 20-40.
inline void rebuild_pool() {
    const uint32_t pages = pool_pages(), count = slot_count();
    std::vector<uint8_t> used(pages, 0);
    uint64_t* t = table();
    uint32_t n_used = 0;
    for (uint32_t s = 0; s < count; ++s) {
        const uint64_t e = t[s];
        if ((e & 0x7fff) == 0) continue;
        const uint32_t p = static_cast<uint32_t>(e >> 20) & kNil;
        if (p < pages && !used[p]) { used[p] = 1; ++n_used; }
    }
    uint32_t head = kNil, tail = kNil, n_free = 0;
    for (uint32_t p = 0; p < pages; ++p) {
        if (used[p]) continue;
        if (tail == kNil) head = p; else set_pool_link(tail, p);
        tail = p;
        ++n_free;
    }
    if (tail != kNil) set_pool_link(tail, kNil);
    std::fprintf(stderr, "[hack] slot guard: pool free list rebuilt: was head 0x%x tail 0x%x free %u; now head 0x%x tail "
                 "0x%x free %u (%u pages named by slots)\n", pool_hdr()[0], pool_hdr()[1], pool_hdr()[2], head, tail,
                 n_free, n_used);
    std::fflush(stderr);
    pool_hdr()[0] = head;
    pool_hdr()[1] = tail;
    pool_hdr()[2] = n_free;
}

// Walks the pool list from the head to the tail (the title never writes the tail's link, so it is not a terminator;
// cap: pages steps). True when every node is a pool page and the walk reaches the tail.
inline bool pool_ok(bool walk) {
    const uint32_t pages = pool_pages();
    const uint32_t h = pool_hdr()[0], tl = pool_hdr()[1];
    if (h == kNil) return true;
    if (h >= pages || tl >= pages) return false;
    if (!walk) return true;
    uint32_t n = h, steps = 0;
    while (n != tl) {
        if (++steps > pages) return false;
        n = pool_link(n);
        if (n >= pages) return false;
    }
    return true;
}


// LEONIDA_CE_SLOT_WATCH=1: the slot table and the pool links are made read-only; a write from code outside the
// title's allocator ranges below (and outside gta6rt) is logged with its RIP and target (first 48 distinct RIPs), then
// let through by unprotecting the page for one single-stepped instruction.
struct Range { uintptr_t lo, hi; };
inline const Range kAllocCode[] = {
    {0x1400e8890, 0x1400e88e2}, {0x14014c640, 0x14014c950}, {0x1403ab6e0, 0x1403ab780},
    {0x140f2c400, 0x140f2cd20}, {0x14221b7d0, 0x14221bad0}, {0x145238400, 0x145238b00},
    {0x144f8ef60, 0x144f8f410}, {0x144f658c0, 0x144f65a00}, {0x14555b0f0, 0x14555b300}};
inline std::atomic<uintptr_t> g_watch_lo[2] = {}, g_watch_hi[2] = {};
inline uintptr_t g_rt_lo = 0, g_rt_hi = 0;
inline thread_local uintptr_t t_unprot_page = 0;
inline std::atomic<int> g_watch_told{0};
inline uintptr_t g_watch_rips[48] = {};

inline bool in_watch(uintptr_t a) {
    for (int i = 0; i < 2; ++i) {
        const uintptr_t lo = g_watch_lo[i].load(std::memory_order_relaxed);
        if (lo && a >= lo && a < g_watch_hi[i].load(std::memory_order_relaxed)) return true;
    }
    return false;
}

inline LONG CALLBACK watch_veh(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && t_unprot_page) {
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(t_unprot_page), 4096, PAGE_READONLY, &old);
        t_unprot_page = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION || ep->ExceptionRecord->NumberParameters < 2 ||
        ep->ExceptionRecord->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t a = ep->ExceptionRecord->ExceptionInformation[1];
    if (!in_watch(a)) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t rip = ep->ContextRecord->Rip;
    bool known = rip >= g_rt_lo && rip < g_rt_hi;
    for (const Range& r : kAllocCode) known = known || (rip >= r.lo && rip < r.hi);
    if (!known) {
        bool seen = false;
        for (int i = 0; i < 48 && g_watch_rips[i]; ++i) seen = seen || g_watch_rips[i] == rip;
        if (!seen) {
            const int k = g_watch_told.fetch_add(1);
            if (k < 48) {
                g_watch_rips[k] = rip;
                const uintptr_t t = reinterpret_cast<uintptr_t>(table());
                std::fprintf(stderr, "[hack] slot watch: FOREIGN WRITE rip %llx (game+0x%llx) to %llx (table+0x%llx) tid %lu "
                             "ret? %llx op %llu\n", (unsigned long long)rip, (unsigned long long)(rip - 0x140000000),
                             (unsigned long long)a, (unsigned long long)(a - t), GetCurrentThreadId(),
                             (unsigned long long)*reinterpret_cast<uintptr_t*>(ep->ContextRecord->Rsp),
                             (unsigned long long)g_ops);
                std::fflush(stderr);
            }
        }
    }
    const uintptr_t page = a & ~uintptr_t{4095};
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(page), 4096, PAGE_READWRITE, &old);
    if (t_unprot_page && t_unprot_page != page) {
        VirtualProtect(reinterpret_cast<void*>(t_unprot_page), 4096, PAGE_READONLY, &old);
    }
    t_unprot_page = page;
    ep->ContextRecord->EFlags |= 0x100;  // trap flag: re-protect after this instruction
    return EXCEPTION_CONTINUE_EXECUTION;
}

inline bool g_watch_on = false;
inline void watch_arm(int i, void* p, size_t bytes) {
    if (!g_watch_on || !p || !bytes || g_watch_lo[i].load()) return;
    const uintptr_t lo = (reinterpret_cast<uintptr_t>(p) + 4095) & ~uintptr_t{4095};
    const uintptr_t hi = (reinterpret_cast<uintptr_t>(p) + bytes) & ~uintptr_t{4095};
    if (hi <= lo) return;
    DWORD old = 0;
    const BOOL ok = VirtualProtect(reinterpret_cast<void*>(lo), hi - lo, PAGE_READONLY, &old);
    if (ok) { g_watch_hi[i] = hi; g_watch_lo[i] = lo; }
    std::fprintf(stderr, "[hack] slot watch: %s %llx..%llx read-only: %s (old protect 0x%lx)\n", i ? "pool links" : "slot table",
                 (unsigned long long)lo, (unsigned long long)hi, ok ? "ok" : "FAILED", old);
    std::fflush(stderr);
}

inline void check(char kind, uint32_t arg) {
    if (!table() || !slot_count()) return;
    if (g_watch_on) {
        watch_arm(0, table(), size_t{slot_count()} * 8);
        if (pool_links() && pool_pages()) watch_arm(1, pool_links(), (size_t{pool_pages()} / 3 + 1) * 8);
    }
    ++g_ops;
    if ((g_ops & 0xffff) == 1)
        std::fprintf(stderr, "[hack] slot guard: op %llu (%c 0x%x), pool head 0x%x tail 0x%x free %u, problems so far %d\n",
                     (unsigned long long)g_ops, kind, arg, pool_hdr()[0], pool_hdr()[1], pool_hdr()[2], g_told);
    const uint32_t count = slot_count();
    uint64_t* t = table();
    for (int o = 0; o < 22; ++o) {
        uint32_t h = heads()[o];
        if (h == kNil) continue;
        if (h >= count || !node_ok(h, o, count, t[h])) {
            tell("bad list head", o, h, kNil, h < count ? t[h] : 0);
            heads()[o] = kNil;
            continue;
        }
        if (g_mode < 2) continue;
        uint32_t prev = h, steps = 0;
        for (uint32_t n = static_cast<uint32_t>(t[h] >> 41) & kNil; n != kNil;
             prev = n, n = static_cast<uint32_t>(t[n] >> 41) & kNil) {
            if (n >= count || !node_ok(n, o, count, t[n]) || ++steps > count) {
                tell(steps > count ? "list cycle" : "bad list node", o, n, prev, n < count ? t[n] : 0);
                t[prev] = (t[prev] & 0xc00001ffffffffffull) | (uint64_t{kNil} << 41);
                break;
            }
        }
    }
    const bool walk = g_mode >= 2 && (g_ops % 32) == 0;
    if (!pool_ok(walk)) {
        tell(walk ? "pool free list bad (walk)" : "pool free list head/tail out of range", -1, pool_hdr()[0],
             pool_hdr()[1], pool_hdr()[2]);
        // Not from the decref/free hooks: they run inside the title's unmap loop, after the page left its slot and
        // before the title pushes it, so a rebuild there would list that page twice.
        if (kind == 'A' || kind == 'S' || kind == 's') rebuild_pool();
    }
    Op& r = g_ring[g_ring_n++ % 32];
    r.tick = GetTickCount64();
    r.tid = GetCurrentThreadId();
    r.kind = kind;
    r.arg = arg;
}

inline uint32_t on_alloc(uint32_t a) { check('A', a); return a; }
inline uint32_t on_free(uint32_t a) {
    if (a >= slot_count()) tell("free of a slot past the count", -1, a, kNil, 0);
    check('F', a);
    return a;
}
inline uint32_t on_decref(uint32_t a) {
    if (a >= slot_count()) tell("decref of a slot past the count", -1, a, kNil, 0);
    check('D', a);
    return a;
}
// Range handles (slot | count << 21) released by 0x144f658c0 and retained by 0x144f65910 from a deferred-release
// object's vector (0x1400e8960 / 0x144f65820). re18: handles naming slots past the count (0xcf400.., 0x8cc88,
// 0x1aadb3) were released and walked past the table. A handle below 0x200000 is a no-op for both, so a handle whose
// range leaves the slot table is turned into 0.
inline std::atomic<int> g_range_told{0};
inline uint32_t range_guard(uint32_t h, const char* what) {
    if (h < 0x200000) return h;
    const uint32_t slot = h & kNil, n = h >> 21, count = slot_count();
    if (count && uint64_t{slot} + n <= count) return h;
    if (g_range_told.fetch_add(1) < 48) {
        std::fprintf(stderr, "[hack] slot guard: %s of range handle 0x%x (slot 0x%x count %u) past the slot count 0x%x "
                     "-> skipped (op %llu)\n", what, h, slot, n, count, (unsigned long long)g_ops);
        std::fflush(stderr);
    }
    return 0;
}
inline uint32_t on_range_release(uint32_t h) { return range_guard(h, "release"); }
inline uint32_t on_range_retain(uint32_t h) { return range_guard(h, "retain"); }
inline uint32_t on_suballoc(uint32_t a) { check('S', a); return a; }
inline uint32_t on_subfree(uint32_t a) { check('s', a); return a; }

inline void install(uint8_t* base) {
    const char* v = std::getenv("LEONIDA_CE_SLOT_GUARD");
    g_mode = v ? std::atoi(v) : 1;
    if (g_mode <= 0 || reinterpret_cast<uintptr_t>(base) != 0x140000000) return;
    if (const char* w = std::getenv("LEONIDA_CE_SLOT_WATCH"); w && *w == '1') {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&watch_veh), &self);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(self) + dos->e_lfanew);
        g_rt_lo = reinterpret_cast<uintptr_t>(self);
        g_rt_hi = g_rt_lo + nt->OptionalHeader.SizeOfImage;
        g_watch_on = AddVectoredExceptionHandler(1, &watch_veh) != nullptr;
        std::fprintf(stderr, "[hack] slot watch: handler %s\n", g_watch_on ? "installed" : "FAILED");
    }
    struct H { uint32_t rva; hooks::RemapFn fn; const char* name; };
    const H hs[] = {{0x5238780, &on_alloc, "alloc"},     {0x221b7d0, &on_free, "free"},
                    {0x00e8890, &on_decref, "decref"},   {0x5238590, &on_suballoc, "suballoc"},
                    {0x4f8f0a0, &on_subfree, "subfree"},   {0x4f658c0, &on_range_release, "range release"},
                    {0x4f65910, &on_range_retain, "range retain"}};
    for (const H& h : hs) {
        const auto r = hooks::hook_image_calls(base, h.rva, h.fn, 0);
        std::fprintf(stderr, "[hack] slot guard (mode %d): %s at RVA 0x%X: %s, %zu direct call(s)\n", g_mode, h.name,
                     h.rva, r.ok ? "hooked" : r.error, r.redirected);
    }
}

}  // namespace leonida::slot_guard
