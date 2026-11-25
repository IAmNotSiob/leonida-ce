// agA10 hack (uncommitted): diagnostics (and an optional capacity bump) for the title's DirectStorage batch contexts.
// The streaming device (vtable 0x1462fc550, BOOT_NOTES agA10) makes 0x80-byte batch contexts ("sdsm") with
// vtbl+0x210 (0x1428c9460: dev, capacity, want_release_vector, kind, prio):
//   +0x18 IDStorageQueue (ours), +0x20/+0x28 release vector (handles, capacity*4 bytes, may be null),
//   +0x30 IDStorageStatusArray (ours, IsComplete = vtbl+0x18), +0x38/+0x40/+0x48 page-mapping list,
//   +0x60 capacity, +0x64 enqueued request count, +0x68 retain capacity, +0x6c retain count, +0x70 retain array,
//   +0x78 state (1 = submitted, awaiting drain).
// Pushes into +0x70 (0x144ffd520) and +0x28 (0x1428c9680 / 0x1428c9730) have no capacity check. If a batch ever holds
// more than `capacity` handles, the pushes write past the array (heap corruption) and the drain reads neighbour memory
// as handles: the garbage range handles of re19 (0xaaaaaaac, 0xf59f59f5, ...). This wraps the vtable entries and logs
// any context over its capacity, every new maximum (>= 256), use of an unknown or destroyed context, and maps of a
// context that was not drained.
// LEONIDA_CE_DSCTX_DIAG (default 1): the checks. LEONIDA_CE_DSCTX_CAP=N: contexts are created with at least N entries.
// re20b (build 37): garbage retains (0x55555556, 0xaaaaaaac, 0x6496496c) with NO context over capacity, so the arrays
// are overwritten from outside. LEONIDA_CE_DSCTX_ISOLATE=1 moves each context's retain array (+0x70) and release vector
// (+0x20) into private VirtualAlloc pages right after creation (the title's own blocks stay allocated, unused, filled
// with a canary); =2 also makes the whole pages inside the old blocks read-only and logs every write to them (foreign
// writers: nothing of the title should touch them any more). Before each map/drain the live arrays are scanned for
// handles past the slot table ("BAD HANDLE").
#pragma once

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "slot_guard.h"

namespace leonida::dsctx_guard {

using Fn = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                         uintptr_t, uintptr_t);
constexpr uintptr_t kVtbl = 0x1462fc550;
inline Fn g_orig[0x60] = {};  // by vtable index (offset / 8)
inline uint32_t g_cap_min = 0;
inline ULONGLONG g_t0 = 0;

struct Ctx {
    uintptr_t p = 0;
    bool live = false;
    uint32_t max_ret = 0, max_vec = 0, max_req = 0;
    uint32_t* old_ret = nullptr; uint32_t* old_vec = nullptr; uint32_t cap = 0;
    uint32_t* new_ret = nullptr; uint32_t* new_vec = nullptr;
    bool canary_told = false;
};
inline int g_isolate = 0;
constexpr uint32_t kCanary = 0xdc5c0000;
inline Ctx g_ctx[64];
inline std::mutex g_mu;
inline std::atomic<int> g_told{0};

inline double now_s() { return (GetTickCount64() - g_t0) / 1000.0; }
inline unsigned long long rva(void* ra) { return reinterpret_cast<uintptr_t>(ra) - 0x140000000ull; }

inline Ctx* find(uintptr_t p) {
    for (Ctx& c : g_ctx) if (c.p == p) return &c;
    return nullptr;
}

inline void say(const char* fmt, ...) {
    if (g_told.fetch_add(1) >= 200) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fflush(stderr);
}

inline bool readable(uintptr_t p) { return p && !IsBadReadPtr(reinterpret_cast<void*>(p), 0x80); }

// Checks a context after (or before) a method; `what` names the vtable offset.
inline void check(uintptr_t ctx, int off, void* ra, const char* when) {
    if (!readable(ctx)) { say("[hack] dsctx: +0x%x %s: ctx %p unreadable (from game+0x%llx, t=%.1f)\n", off, when, (void*)ctx, rva(ra), now_s()); return; }
    const auto* b = reinterpret_cast<const uint8_t*>(ctx);
    const uint32_t cap = *reinterpret_cast<const uint32_t*>(b + 0x60), req = *reinterpret_cast<const uint32_t*>(b + 0x64);
    const uint32_t rcap = *reinterpret_cast<const uint32_t*>(b + 0x68), ret = *reinterpret_cast<const uint32_t*>(b + 0x6c);
    const uintptr_t v0 = *reinterpret_cast<const uintptr_t*>(b + 0x20), v1 = *reinterpret_cast<const uintptr_t*>(b + 0x28);
    const uint64_t vec = v0 ? (v1 - v0) / 4 : 0;
    const std::lock_guard lock(g_mu);
    Ctx* c = find(ctx);
    if (!c || !c->live) {
        say("[hack] dsctx: +0x%x %s on %s ctx %p (from game+0x%llx, t=%.1f)\n", off, when, c ? "DESTROYED" : "unknown",
            (void*)ctx, rva(ra), now_s());
        if (!c) return;
    }
    const bool over = ret > rcap || (v0 && vec > cap) || v1 < v0;
    if (over)
        say("[hack] dsctx: OVER CAPACITY ctx %p after +0x%x (%s, from game+0x%llx, t=%.1f): retains %u/%u, release vector %llu/%u, "
            "requests %u, state %u\n", (void*)ctx, off, when, rva(ra), now_s(), ret, rcap, (unsigned long long)vec, cap, req, b[0x78]);
    auto bump = [&](uint32_t& m, uint64_t v, const char* name, uint32_t of) {
        if (v <= m) return;
        const bool tell = v >= 256 && (v >= 2 * uint64_t{m} || v * 8 >= uint64_t{of} * 7);
        m = static_cast<uint32_t>(v);
        if (tell)
            say("[hack] dsctx: ctx %p new max %s %llu (cap %u) at +0x%x from game+0x%llx, t=%.1f\n", (void*)ctx, name,
                (unsigned long long)v, of, off, rva(ra), now_s());
    };
    bump(c->max_ret, ret, "retains", rcap);
    bump(c->max_vec, vec, "release vector", cap);
    bump(c->max_req, req, "requests", cap);
}


// ---- isolation and the foreign-write watch on the abandoned blocks ----
struct WRange { uintptr_t lo, hi; };
inline WRange g_wr[128];
inline std::atomic<int> g_wr_n{0};
inline std::atomic<int> g_wtold{0};
inline thread_local uintptr_t t_wpage = 0;
inline uintptr_t g_rlo = 0, g_rhi = 0;

inline LONG CALLBACK wveh(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && t_wpage) {
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(t_wpage), 4096, PAGE_READONLY, &old);
        t_wpage = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION || ep->ExceptionRecord->NumberParameters < 2 ||
        ep->ExceptionRecord->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t a = ep->ExceptionRecord->ExceptionInformation[1];
    bool in = false;
    const int n = g_wr_n.load();
    for (int i = 0; i < n && !in; ++i) in = a >= g_wr[i].lo && a < g_wr[i].hi;
    if (!in) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t rip = ep->ContextRecord->Rip;
    const int k = g_wtold.fetch_add(1);
    if (k < 40) {
        const auto* sp = reinterpret_cast<const uintptr_t*>(ep->ContextRecord->Rsp);
        char st[256] = {};
        int at = 0;
        // return-address candidates in the title image from the first 64 stack slots
        for (int i = 0, got = 0; i < 64 && got < 8 && at < 230; ++i)
            if (sp[i] >= 0x140001000 && sp[i] < 0x14a000000) { at += std::snprintf(st + at, sizeof st - at, " +0x%llx", (unsigned long long)(sp[i] - 0x140000000)); ++got; }
        std::fprintf(stderr, "[hack] dsctx watch: FOREIGN WRITE to abandoned ctx array at %llx by rip %llx%s%llx tid %lu t=%.1f; "
                     "rax %llx rcx %llx rdx %llx r8 %llx; stack?%s\n", (unsigned long long)a, (unsigned long long)rip,
                     rip >= 0x140000000 && rip < 0x14a000000 ? " game+0x" : " host ",
                     (unsigned long long)(rip >= 0x140000000 && rip < 0x14a000000 ? rip - 0x140000000 : (rip >= g_rlo && rip < g_rhi ? rip - g_rlo : rip)),
                     GetCurrentThreadId(), now_s(), (unsigned long long)ep->ContextRecord->Rax,
                     (unsigned long long)ep->ContextRecord->Rcx, (unsigned long long)ep->ContextRecord->Rdx,
                     (unsigned long long)ep->ContextRecord->R8, st);
        std::fflush(stderr);
    }
    const uintptr_t page = a & ~uintptr_t{4095};
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(page), 4096, PAGE_READWRITE, &old);
    if (t_wpage && t_wpage != page) VirtualProtect(reinterpret_cast<void*>(t_wpage), 4096, PAGE_READONLY, &old);
    t_wpage = page;
    ep->ContextRecord->EFlags |= 0x100;
    return EXCEPTION_CONTINUE_EXECUTION;
}

inline void watch_block(void* p, size_t bytes) {
    const uintptr_t lo = (reinterpret_cast<uintptr_t>(p) + 4095) & ~uintptr_t{4095};
    const uintptr_t hi = (reinterpret_cast<uintptr_t>(p) + bytes) & ~uintptr_t{4095};
    if (hi <= lo) return;
    const int i = g_wr_n.load();
    if (i >= 128) return;
    g_wr[i] = WRange{lo, hi};
    g_wr_n.store(i + 1);
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(lo), hi - lo, PAGE_READONLY, &old);
}

inline uint32_t* fresh(size_t entries) {
    const size_t bytes = ((entries * 4 + 0xffff) & ~size_t{0xffff}) + 0x10000;  // + 64 KiB slack
    return static_cast<uint32_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}

inline void isolate(uintptr_t ctx, Ctx* c) {
    auto* b = reinterpret_cast<uint8_t*>(ctx);
    const uint32_t cap = *reinterpret_cast<uint32_t*>(b + 0x60), rcap = *reinterpret_cast<uint32_t*>(b + 0x68);
    c->cap = cap;
    auto& r70 = *reinterpret_cast<uint32_t**>(b + 0x70);
    auto& v20 = *reinterpret_cast<uint32_t**>(b + 0x20);
    auto& v28 = *reinterpret_cast<uint32_t**>(b + 0x28);
    if (r70 && rcap && *reinterpret_cast<uint32_t*>(b + 0x6c) == 0) {
        c->old_ret = r70;
        c->new_ret = fresh(rcap);
        if (c->new_ret) {
            for (uint32_t i = 0; i < rcap; ++i) c->old_ret[i] = kCanary | (i & 0xffff);
            r70 = c->new_ret;
            if (g_isolate >= 2) watch_block(c->old_ret, size_t{rcap} * 4);
        }
    }
    if (v20 && v28 == v20 && cap) {
        c->old_vec = v20;
        c->new_vec = fresh(cap);
        if (c->new_vec) {
            for (uint32_t i = 0; i < cap; ++i) c->old_vec[i] = kCanary | 0x8000 | (i & 0x7fff);
            v20 = v28 = c->new_vec;
            if (g_isolate >= 2) watch_block(c->old_vec, size_t{cap} * 4);
        }
    }
}

// The abandoned blocks keep their canary unless something foreign writes them.
inline void canary_check(Ctx* c, int off, void* ra) {
    if (c->canary_told || !c->cap) return;
    auto scan = [&](const uint32_t* p, uint32_t n, uint32_t tag, const char* name) {
        if (!p) return false;
        for (uint32_t i = 0; i < n; ++i)
            if (p[i] != (kCanary | tag | (i & (tag ? 0x7fff : 0xffff)))) {
                std::fprintf(stderr, "[hack] dsctx: CANARY of abandoned %s of ctx %p changed at index %u (block %p): %08x %08x %08x %08x "
                             "(seen at +0x%x from game+0x%llx, t=%.1f)\n", name, (void*)c->p, i, (const void*)p, p[i],
                             i + 1 < n ? p[i + 1] : 0, i + 2 < n ? p[i + 2] : 0, i + 3 < n ? p[i + 3] : 0, off, rva(ra), now_s());
                std::fflush(stderr);
                return true;
            }
        return false;
    };
    const uint32_t rcap = *reinterpret_cast<const uint32_t*>(c->p + 0x68);
    if (scan(c->old_ret, rcap, 0, "retain array") | scan(c->old_vec, c->cap, 0x8000, "release vector")) c->canary_told = true;
}

inline std::atomic<int> g_bad_told{0};
inline void scan_handles(uintptr_t ctx, int off, void* ra) {
    const auto* b = reinterpret_cast<const uint8_t*>(ctx);
    const uint32_t count = leonida::slot_guard::slot_count();
    if (!count) return;
    auto bad = [&](uint32_t h) { return h >= 0x200000 && uint64_t{h & 0x1fffff} + (h >> 21) > count; };
    const uint32_t* r = *reinterpret_cast<uint32_t* const*>(b + 0x70);
    const uint32_t n = *reinterpret_cast<const uint32_t*>(b + 0x6c);
    if (r) for (uint32_t i = 0; i < n && i < 0x100000; ++i)
        if (bad(r[i])) {
            if (g_bad_told.fetch_add(1) < 24)
                say("[hack] dsctx: BAD HANDLE in retain array %p of ctx %p: [%u/%u] = %08x (next %08x) before +0x%x from game+0x%llx, t=%.1f\n",
                    (const void*)r, (void*)ctx, i, n, r[i], i + 1 < n ? r[i + 1] : 0, off, rva(ra), now_s());
            break;
        }
    const uint32_t* v0 = *reinterpret_cast<uint32_t* const*>(b + 0x20);
    const uint32_t* v1 = *reinterpret_cast<uint32_t* const*>(b + 0x28);
    if (v0 && v1 > v0 && v1 - v0 < 0x100000) for (const uint32_t* q = v0; q < v1; ++q)
        if (bad(*q)) {
            if (g_bad_told.fetch_add(1) < 24)
                say("[hack] dsctx: BAD HANDLE in release vector %p of ctx %p: [%lld/%lld] = %08x before +0x%x from game+0x%llx, t=%.1f\n",
                    (const void*)v0, (void*)ctx, (long long)(q - v0), (long long)(v1 - v0), *q, off, rva(ra), now_s());
            break;
        }
}

template <int Off>
uintptr_t wrap(uintptr_t dev, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g,
               uintptr_t h, uintptr_t i) {
    void* ra = _ReturnAddress();
    if constexpr (Off == 0x210) {  // create(dev, capacity, want_vector, kind, prio) -> ctx
        if (g_cap_min && a < g_cap_min) a = g_cap_min;
        const uintptr_t ctx = g_orig[Off / 8](dev, a, b, c, d, e, f, g, h, i);
        {
            const std::lock_guard lock(g_mu);
            Ctx* s = find(ctx);
            if (!s) s = find(0);
            if (s) {
                *s = Ctx{};
                s->p = ctx;
                s->live = true;
                if (g_isolate && readable(ctx)) isolate(ctx, s);
            }
        }
        say("[hack] dsctx: create ctx %p capacity %llu vector %d kind %llu (from game+0x%llx, t=%.1f)\n", (void*)ctx,
            (unsigned long long)a, int(b & 0xff), (unsigned long long)(c & 0xffffffff), rva(ra), now_s());
        return ctx;
    } else if constexpr (Off == 0x218) {  // destroy(dev, ctx)
        check(a, Off, ra, "before");
        {
            const std::lock_guard lock(g_mu);
            if (Ctx* s = find(a)) s->live = false;
        }
        say("[hack] dsctx: destroy ctx %p (from game+0x%llx, t=%.1f)\n", (void*)a, rva(ra), now_s());
        {   // hand the title its own blocks back before it frees them
            const std::lock_guard lock(g_mu);
            if (Ctx* s = find(a); s && readable(a)) {
                auto* b = reinterpret_cast<uint8_t*>(a);
                if (s->old_ret) *reinterpret_cast<uint32_t**>(b + 0x70) = s->old_ret;
                if (s->old_vec) *reinterpret_cast<uint32_t**>(b + 0x20) = *reinterpret_cast<uint32_t**>(b + 0x28) = s->old_vec;
                s->old_ret = s->old_vec = nullptr;
                s->cap = 0;
            }
        }
        return g_orig[Off / 8](dev, a, b, c, d, e, f, g, h, i);
    } else {
        if constexpr (Off == 0x240) {
            if (readable(a) && reinterpret_cast<const uint8_t*>(a)[0x78] == 1) {
                static std::atomic<int> n{0};
                if (n++ < 8) say("[hack] dsctx: map/submit of ctx %p that is still awaiting its drain (from game+0x%llx, t=%.1f)\n",
                                 (void*)a, rva(ra), now_s());
            }
        }
        check(a, Off, ra, "before");
        if constexpr (Off == 0x240 || Off == 0x248 || Off == 0x260) {
            if (readable(a)) {
                scan_handles(a, Off, ra);
                if (g_isolate) {
                    const std::lock_guard lock(g_mu);
                    if (Ctx* s = find(a)) canary_check(s, Off, ra);
                }
            }
        }
        const uintptr_t r = g_orig[Off / 8](dev, a, b, c, d, e, f, g, h, i);
        check(a, Off, ra, "after");
        return r;
    }
}

inline void install() {
    const char* d = std::getenv("LEONIDA_CE_DSCTX_DIAG");
    const char* cap = std::getenv("LEONIDA_CE_DSCTX_CAP");
    g_cap_min = cap ? static_cast<uint32_t>(std::strtoul(cap, nullptr, 0)) : 0;
    const char* iso = std::getenv("LEONIDA_CE_DSCTX_ISOLATE");
    g_isolate = iso ? std::atoi(iso) : 0;
    if (d && *d == '0' && !g_cap_min && !g_isolate) return;
    if (g_isolate >= 2) {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&wveh), &self);
        g_rlo = reinterpret_cast<uintptr_t>(self);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
        g_rhi = g_rlo + reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(self) + dos->e_lfanew)->OptionalHeader.SizeOfImage;
        AddVectoredExceptionHandler(1, &wveh);
    }
    g_t0 = GetTickCount64();
    auto* vt = reinterpret_cast<uintptr_t*>(kVtbl);
    // Sanity: the entries this was measured with.
    if (vt[0x210 / 8] != 0x1428c9460 || vt[0x240 / 8] != 0x1400e88f0 || vt[0x260 / 8] != 0x144f65820) {
        std::fprintf(stderr, "[hack] dsctx: vtable mismatch, not installed\n");
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(vt + 0x40, 0x60 * 8, PAGE_READWRITE, &old)) {
        std::fprintf(stderr, "[hack] dsctx: VirtualProtect failed %lu\n", GetLastError());
        return;
    }
    auto put = [&](int off, Fn fn) { g_orig[off / 8] = reinterpret_cast<Fn>(vt[off / 8]); vt[off / 8] = reinterpret_cast<uintptr_t>(fn); };
    static_assert(0x260 / 8 < sizeof(g_orig) / sizeof(g_orig[0]));
    put(0x210, &wrap<0x210>);
    put(0x218, &wrap<0x218>);
    put(0x220, &wrap<0x220>);
    put(0x228, &wrap<0x228>);
    put(0x230, &wrap<0x230>);
    put(0x240, &wrap<0x240>);
    put(0x248, &wrap<0x248>);
    put(0x260, &wrap<0x260>);
    VirtualProtect(vt + 0x40, 0x60 * 8, old, &old);
    std::fprintf(stderr, "[hack] dsctx: vtable 0x%llx wrapped (diag, capacity min %u, isolate %d)\n", (unsigned long long)kVtbl, g_cap_min, g_isolate);
}

}  // namespace leonida::dsctx_guard
