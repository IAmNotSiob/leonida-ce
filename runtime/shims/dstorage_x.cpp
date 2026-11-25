// HACK (local, exploratory): stand-in for the Xbox DirectStorage factory (xcrdapi.dll!DStorageGetFactory).
// The Xbox interface layout is not known, so every object handed out is a "probe" whose vtable slots log their slot
// number and first six arguments and return S_OK. Slots 0-2 behave as IUnknown. The log shows what the game calls,
// and real behaviour is filled in slot by slot (see kKnown below).
//
// Requests complete asynchronously, after Marvisak/VIctory dstorage.rs (GPLv3): EnqueueRequest (queue slot 3) and
// EnqueueStatus (queue slot 4) only record commands in queue order; Submit (queue slot 6) hands them to a small pool
// of workers that read and inflate in parallel, started in submission order. Completion stays in order: a status
// entry turns complete once every request enqueued before it on its queue has finished, carrying the first failure
// (in queue order) since the previous status entry. As on the console, a queue also submits itself once half its
// capacity is enqueued. LEONIDA_CE_DSTORAGE_SYNC=1 restores the old behaviour (read inside EnqueueRequest, every
// status entry complete) for A/B testing; LEONIDA_CE_DSTORAGE_WORKERS sets the pool size (default 4).
#include "bcpack_decode.h"
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <thread>
#include <utility>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "shims.h"
#include "page_alias.h"
#include "../src/shader_library.h"
#include <vector>

namespace {

constexpr int kSlots = 96;

struct Probe;
struct QueueState;
struct StatusState;
using Slot = uintptr_t(__stdcall*)(Probe*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

struct Probe {
    const Slot* vtbl;
    const char* name;
    std::atomic<long> refs{1};
    HANDLE file = INVALID_HANDLE_VALUE;  // "file" probes: the opened package file
    wchar_t path[260] = {};
    QueueState* queue = nullptr;    // "queue" probes (async mode)
    StatusState* status = nullptr;  // "status" probes (async mode)
};

// ---- asynchronous completion (after Marvisak/VIctory dstorage.rs, GPLv3) ----

constexpr int32_t kPending = static_cast<int32_t>(0x8000000Au);  // E_PENDING

bool env_flag(const char* name) {
    char buf[16] = {};
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
    return n > 0 && n < sizeof buf && buf[0] != '0';
}

bool sync_mode() {
    static const bool on = env_flag("LEONIDA_CE_DSTORAGE_SYNC");
    return on;
}

// A request as copied out of the title's 0x48-byte struct at EnqueueRequest.
struct Req {
    uint32_t comp;
    uint32_t opts = 0;    // the whole option dword (agA10)
    uint16_t low16 = 0;    // agA11: low 16 bits of the destination (offset into the first descriptor's 64K page)
    uint64_t desc_va = 0;  // agA10: option bit 21 -> +8 is a pool slot descriptor; the guest address it stands for
    uint8_t* dst;
    uint32_t dst_size;
    Probe* f;
    uint64_t off;
    uint32_t src_size;
};

Req parse_request(const void* p) {
    // Console request layout (measured): +0 options (low byte = compression: 0 none, 1 zlib), +8 destination,
    // +0x18 destination size, +0x20 IDStorageFile*, +0x28 file offset, +0x30 source size.
    const auto* rq = static_cast<const uint8_t*>(p);
    Req r{};
    r.comp = rq[0];
    r.opts = *reinterpret_cast<const uint32_t*>(rq);
    r.dst = *reinterpret_cast<uint8_t* const*>(rq + 0x08);
    r.dst_size = *reinterpret_cast<const uint32_t*>(rq + 0x18);
    r.f = *reinterpret_cast<Probe* const*>(rq + 0x20);
    r.off = *reinterpret_cast<const uint64_t*>(rq + 0x28);
    r.src_size = *reinterpret_cast<const uint32_t*>(rq + 0x30);
    // HACK (agA10): option bit 21 (0x200000, set by the title at 0x144ffd520 when 0x144ffd8c0 finds the destination in
    // its GPU texture pool) means +8 is NOT memory but &table2[slot] (table2 = qword* 0x1471bfb10) and +0x10 holds the
    // low 16 bits of the destination. Copying to +8 as if it were memory overwrote table2, the pool links and the heap
    // behind them (re15-re22 corruption; re22 watch: VCRUNTIME memcpy from our workers, rax = 0xd849804a0 in table2).
    // The destination is gpu base (qword 0x149e1dad0) + slot * 64 KiB + low16. LEONIDA_CE_DS_DESC=0: old behaviour.
    static const bool desc_on = [] { const char* v = std::getenv("LEONIDA_CE_DS_DESC"); return !v || *v != '0'; }();
    if (desc_on && (r.opts & 0x200000)) {
        const uint64_t table2 = *reinterpret_cast<const volatile uint64_t*>(uintptr_t{0x1471bfb10});
        const uint64_t gpu = *reinterpret_cast<const volatile uint64_t*>(uintptr_t{0x149e1dad0});
        const uint64_t p = reinterpret_cast<uint64_t>(r.dst);
        const uint16_t low = *reinterpret_cast<const uint16_t*>(rq + 0x10);
        r.low16 = low;
        if (table2 && gpu && p >= table2 && ((p - table2) & 7) == 0)
            r.desc_va = gpu + (((p - table2) >> 3) << 16) + low;
        else
            r.desc_va = ~uint64_t{0};  // unknown shape: never write it
    }
    return r;
}

struct StatusState {
    uint32_t count = 0;
    std::unique_ptr<std::atomic<int32_t>[]> entries;  // S_OK until a marker makes one pending
};

struct Cmd {
    bool is_status = false;
    uint64_t seq = 0;
    Req req{};
    StatusState* array = nullptr;
    uint32_t index = 0;
};

struct QueueState {
    size_t capacity = 0;
    uint64_t next_seq = 0;
    std::vector<Cmd> unsubmitted;      // enqueued, not yet submitted
    std::set<uint64_t> outstanding;    // submitted reads not finished
    std::deque<Cmd> statuses;          // submitted status markers not yet written
    std::map<uint64_t, int32_t> failures;  // failed reads not yet reported, by seq
};

struct Job {
    QueueState* q;
    uint64_t seq;
    Req req;
};

std::mutex g_mu;
std::condition_variable g_work;
std::deque<Job> g_jobs;  // every queue's submitted reads, in submission order

// Writes the status entries whose earlier reads have all finished (g_mu held).
void flush_locked(QueueState* q) {
    while (!q->statuses.empty()) {
        const Cmd& s = q->statuses.front();
        if (!q->outstanding.empty() && *q->outstanding.begin() < s.seq) return;
        int32_t hr = S_OK;
        auto it = q->failures.begin();
        if (it != q->failures.end() && it->first < s.seq) hr = it->second;
        q->failures.erase(q->failures.begin(), q->failures.lower_bound(s.seq));
        s.array->entries[s.index].store(hr, std::memory_order_release);
        q->statuses.pop_front();
    }
}

void worker_main();

void start_workers() {
    static std::once_flag once;
    std::call_once(once, [] {
        int n = 4;
        char buf[16] = {};
        const DWORD len = GetEnvironmentVariableA("LEONIDA_CE_DSTORAGE_WORKERS", buf, sizeof buf);
        if (len > 0 && len < sizeof buf) n = std::clamp(std::atoi(buf), 1, 64);
        for (int i = 0; i < n; ++i) std::thread(worker_main).detach();
        std::fprintf(stderr, "[dstorage] async completion: %d workers\n", n);
        std::fflush(stderr);
    });
}

// Moves a queue's enqueued commands to the workers (g_mu held).
void submit_locked(QueueState* q) {
    bool any = false;
    for (Cmd& c : q->unsubmitted) {
        if (c.is_status) {
            q->statuses.push_back(c);
        } else {
            q->outstanding.insert(c.seq);
            g_jobs.push_back(Job{q, c.seq, c.req});
            any = true;
        }
    }
    q->unsubmitted.clear();
    flush_locked(q);
    if (any) g_work.notify_all();
}

void enqueue_locked(QueueState* q, Cmd c) {
    c.seq = q->next_seq++;
    q->unsubmitted.push_back(c);
    if (q->capacity && q->unsubmitted.size() >= (std::max)(q->capacity / 2, size_t(1))) submit_locked(q);
}

void print_guid(const void* p, char* out, size_t n) {
    GUID g{};
    if (!p || IsBadReadPtr(p, sizeof g)) { std::snprintf(out, n, "(unreadable)"); return; }
    std::memcpy(&g, p, sizeof g);
    std::snprintf(out, n, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3, g.Data4[0],
                  g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

Probe* make_probe(const char* name);

void hexdump(const char* tag, const void* p, size_t n) {
    if (!p || IsBadReadPtr(p, n)) { std::fprintf(stderr, "[dstorage]   %s: (unreadable %p)\n", tag, p); return; }
    const auto* w = static_cast<const uint32_t*>(p);
    std::fprintf(stderr, "[dstorage]   %s:", tag);
    for (size_t i = 0; i < n / 4; ++i) std::fprintf(stderr, " %08x", w[i]);
    std::fprintf(stderr, "\n");
}

// agA10: writes a request's output: plain memory, or through the physical pages behind a pool slot descriptor.
bool write_out(const Req& r, const void* data, size_t n, bool zero = false) {
    if (!r.desc_va) {
        if (zero) std::memset(r.dst, 0, n); else std::memcpy(r.dst, data, n);
        return true;
    }
    static std::atomic<int> told{0};
    const auto hook = rt::xmem::write_phys_hook().load(std::memory_order_acquire);
    bool ok = false;
    const char* how = "none";
    if (r.desc_va != ~uint64_t{0}) {
        std::vector<uint8_t> z;
        if (zero) { z.assign(n, 0); data = z.data(); }
        // 1) the physical pages mapped at the GPU-pool address; 2) the same offset in the CPU pool view (agA10 re23:
        // some slots have no GPU-view mapping yet, e.g. 0xc31ae0000); 3) plain committed, writable memory there.
        // agA11 (THE PAGE PATH): table2[slot] (written by 0x1455aa6a0) = the slot's physical page number, i.e. the
        // XMemAllocatePhysicalPages page number (4K frame number of a 64K page). The console's DMA writes to that
        // physical page whether or not any VA maps it yet (re24: 16 targets only MEM_RESERVE at write time). A request
        // that runs past its page continues with the next descriptor (table2[slot + 1], ...). LEONIDA_CE_DS_DESC=2:
        // skip this and use only the build-40 VA paths.
        static const bool page_path = [] { const char* v = std::getenv("LEONIDA_CE_DS_DESC"); return !v || *v != '2'; }();
        const auto wpage = rt::xmem::write_page_hook().load(std::memory_order_acquire);
        if (page_path && wpage) {
            const auto* desc = reinterpret_cast<const volatile uint64_t*>(r.dst);
            const auto* from = static_cast<const uint8_t*>(data);
            uint64_t off = r.low16, left = n;
            bool good = true, dropped = false;
            // HACK (agA12): table2[slot] == 0 = the slot was released before the request ran (re26: one terrain
            // request FAILED this way). Drop that part and answer S_OK; LEONIDA_CE_DS_DROP0=0 = old failure.
            static const bool drop0 = [] { const char* v = std::getenv("LEONIDA_CE_DS_DROP0"); return !v || *v != '0'; }();
            for (size_t k = 0; left && good; ++k) {
                const uint64_t page = desc[k];
                const uint64_t chunk = (std::min)(left, uint64_t{0x10000} - off);
                if (!page && drop0) {
                    static std::atomic<int> dt{0};
                    if (dt++ < 16) std::fprintf(stderr, "[dstorage] page path: desc[%zu] = 0 (released slot), %llu bytes dropped (desc %p low 0x%x) [hack DROP0]\n",
                                                k, (unsigned long long)chunk, (void*)r.dst, r.low16);
                    dropped = true; from += chunk; left -= chunk; off = 0; continue; }
                good = page && wpage(page, off, from, chunk);
                if (!good) {
                    static std::atomic<int> pt{0};
                    if (pt++ < 16) std::fprintf(stderr, "[dstorage] page path: desc[%zu] = 0x%llx not a known page (desc %p low 0x%x n %zu)\n",
                                                k, (unsigned long long)page, (void*)r.dst, r.low16, n);
                }
                from += chunk;
                left -= chunk;
                off = 0;
            }
            if (good) { ok = true; how = dropped ? "phys page (released slot part dropped)" : "phys page"; }
        }
        const uint64_t gpu = *reinterpret_cast<const volatile uint64_t*>(uintptr_t{0x149e1dad0});
        const uint64_t cpu = *reinterpret_cast<const volatile uint64_t*>(uintptr_t{0x149e1dac0});
        const uint64_t cva = cpu + (r.desc_va - gpu);
        if (ok) {}
        else if (hook && hook(r.desc_va, data, n)) { ok = true; how = "gpu pages"; }
        else if (hook && cpu && hook(cva, data, n)) { ok = true; how = "cpu pages"; }
        else {
            for (uint64_t va : {r.desc_va, cva}) {
                MEMORY_BASIC_INFORMATION mi{};
                if (!VirtualQuery(reinterpret_cast<void*>(va), &mi, sizeof mi) || mi.State != MEM_COMMIT) continue;
                const DWORD pr = mi.Protect & 0xff;
                if (pr != PAGE_READWRITE && pr != PAGE_EXECUTE_READWRITE && pr != PAGE_WRITECOPY) continue;
                if (reinterpret_cast<uint64_t>(mi.BaseAddress) + mi.RegionSize < va + n) continue;
                std::memcpy(reinterpret_cast<void*>(va), data, n);
                ok = true;
                how = va == r.desc_va ? "gpu committed" : "cpu committed";
                break;
            }
        }
    }
    if (told++ < 16 || !ok) {
        static std::atomic<int> bad{0};
        if (ok || bad++ < 16) {
            MEMORY_BASIC_INFORMATION mi{};
            if (!ok) VirtualQuery(reinterpret_cast<void*>(r.desc_va), &mi, sizeof mi);
            std::fprintf(stderr, "[dstorage] slot-descriptor request (opts 0x%08x, desc %p, comp %u, %zu bytes) -> va 0x%llx: %s%s [hack]",
                         r.opts, (void*)r.dst, r.comp, n, (unsigned long long)r.desc_va, ok ? "written via " : "NOT WRITTEN (page not mapped)", ok ? how : "");
            if (!ok) std::fprintf(stderr, " host state 0x%lx type 0x%lx protect 0x%lx base %p", mi.State, mi.Type, mi.Protect, mi.AllocationBase);
            std::fprintf(stderr, "\n");
            std::fflush(stderr);
        }
    }
    return ok;
}

// Reads and inflates one request into its destination; returns S_OK or the failure. Thread-safe (positional reads).
int32_t run_request(const Req& r) {
    const uint32_t comp = r.comp;
    uint8_t* dst = r.dst;
    const uint32_t dst_size = r.dst_size;
    Probe* f = r.f;
    const uint64_t off = r.off;
    const uint32_t src_size = r.src_size;
    static std::atomic<uint64_t> total{0}, fails{0};
    bool ok = false;
    int32_t hr = E_INVALIDARG;
    std::vector<uint8_t> src;
    try {
        src.resize(src_size);
    } catch (...) {
        src.clear();
        hr = E_OUTOFMEMORY;
    }
    if (src.size() == src_size && f && f->file != INVALID_HANDLE_VALUE && dst) {
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(off);
        ov.OffsetHigh = static_cast<DWORD>(off >> 32);
        DWORD got = 0;
        const BOOL read_ok = ReadFile(f->file, src.data(), src_size, &got, &ov);
        if (read_ok && got == src_size) {
            if (comp == 0) {
                ok = write_out(r, src.data(), (std::min)(src_size, dst_size));
                if (!ok) hr = E_FAIL;
            } else if (comp == 1) {
                std::vector<uint8_t> out;
                if (leonida::zlib_inflate(std::span<const uint8_t>(src.data(), src.size()), out, dst_size)) {
                    ok = write_out(r, out.data(), (std::min)(out.size(), static_cast<size_t>(dst_size)));
                    if (!ok) hr = E_FAIL;
                } else {
                    hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                }
            } else {
                // HACK (agA5): comp 14 = BCPack (ttdb_graphics.ttpf), no runtime decoder yet. LEONIDA_CE_DS_ZEROFILL=1
                // answers such requests with a zero-filled destination and success instead of E_NOTIMPL.
                static const bool zero = [] { const char* e = std::getenv("LEONIDA_CE_DS_ZEROFILL"); return !e || e[0] != '0'; }();  // agA11: default on
                // HACK (agA12): LEONIDA_CE_BCPACK (default on, =0 off) decodes comp 14 with shims/bcpack_decode (strict mode, auto mode
                // detection); a chunk it refuses falls back to the zero fill below.
                static const bool use_bcpack = [] { const char* e = std::getenv("LEONIDA_CE_BCPACK"); return !e || e[0] != '0'; }();  // default on
                static std::atomic<int> bc_ok{0}, bc_bad{0};
                std::vector<uint8_t> bc_out;
                std::string bc_why;
                int bc_mode = 0;
                if (comp == 14 && use_bcpack) {
                    bc_out.resize(dst_size);
                    if (bcpack::decode(src.data(), src.size(), bcpack::kAuto, bc_out.data(), bc_out.size(), &bc_why, &bc_mode)) {
                        ok = write_out(r, bc_out.data(), bc_out.size());
                        if (!ok) hr = E_FAIL;
                        const int k = bc_ok++;
                        if (k < 4 || (k & (k - 1)) == 0)
                            std::fprintf(stderr, "[dstorage] comp=14 BCPack decoded #%d (mode %d, %u -> %u bytes) [hack]\n", k + 1, bc_mode, src_size, dst_size);
                    } else {
                        const int k = bc_bad++;
                        if (k < 8 || (k & (k - 1)) == 0)
                            std::fprintf(stderr, "[dstorage] comp=14 BCPack refused #%d (%s; %u -> %u bytes), zero fill [hack]\n", k + 1, bc_why.c_str(), src_size, dst_size);
                    }
                }
                if (ok || hr == E_FAIL) {
                } else if (zero) {
                    write_out(r, nullptr, dst_size, true);
                    static std::atomic<int> told{0};
                    if (told++ < 4) std::fprintf(stderr, "[dstorage] comp=%u zero-filled (%u bytes) [hack]\n", comp, dst_size);
                    ok = true;
                } else {
                    hr = E_NOTIMPL;
                }
            }
        } else {
            const DWORD err = read_ok ? ERROR_SUCCESS : GetLastError();
            hr = HRESULT_FROM_WIN32(err != ERROR_SUCCESS ? err : ERROR_HANDLE_EOF);
        }
    }
    total += src_size;
    if (!ok) {
        if (fails++ < 12) {
            const bool have = src.size() == src_size;
            std::fprintf(stderr, "[dstorage] request failed: comp=%u file=%ls off=0x%llx size=%u dst=%p/%u first=%02x %02x %02x %02x\n",
                         comp, f ? f->path : L"?", (unsigned long long)off, src_size, (void*)dst, dst_size,
                         have && src_size > 0 ? src[0] : 0, have && src_size > 1 ? src[1] : 0,
                         have && src_size > 2 ? src[2] : 0, have && src_size > 3 ? src[3] : 0);
            std::fflush(stderr);
        }
        return hr;
    }
    static std::atomic<int> n{0};
    const int k = n++;
    if (k < 3 || (k % 2000) == 0) {
        std::fprintf(stderr, "[dstorage] request #%d ok: comp=%u %ls @0x%llx %u -> %u (total %llu MiB read)\n",
                     k, comp, f->path, (unsigned long long)off, src_size, dst_size,
                     (unsigned long long)(total.load() >> 20));
        std::fflush(stderr);
    }
    return S_OK;
}

void worker_main() {
    std::unique_lock<std::mutex> lock(g_mu);
    for (;;) {
        g_work.wait(lock, [] { return !g_jobs.empty(); });
        Job job = g_jobs.front();
        g_jobs.pop_front();
        lock.unlock();
        const int32_t hr = run_request(job.req);
        lock.lock();
        job.q->outstanding.erase(job.seq);
        if (hr != S_OK) job.q->failures.emplace(job.seq, hr);
        flush_locked(job.q);
    }
}

// EnqueueStatus / Submit used to log every call through the generic probe line; keep a sparse trace.
void log_marker(int slot, uintptr_t a, uintptr_t b) {
    static std::atomic<int> n[8]{};
    const int k = n[slot & 7]++;
    if (k < 4 || (k % 64) == 0) {
        std::fprintf(stderr, "[dstorage] queue slot %d (%s) #%d: a=0x%llx b=0x%llx\n", slot,
                     slot == 4 ? "EnqueueStatus" : "Submit", k, (unsigned long long)a, (unsigned long long)b);
        std::fflush(stderr);
    }
}

// HACK: known shapes (PC IDStorageFactory order where it matches the title's calls).
bool special(Probe* self, int slot, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t, uintptr_t,
             uintptr_t* result) {
    const bool factory = std::strcmp(self->name, "factory") == 0;
    if (factory && slot == 4) {  // OpenFile(path, riid, ppv)
        static std::atomic<int> n{0};
        const wchar_t* path = reinterpret_cast<const wchar_t*>(a);
        if (n++ < 40) {
            std::fprintf(stderr, "[dstorage] OpenFile(%ls)\n", path && !IsBadReadPtr(path, 2) ? path : L"?");
            std::fflush(stderr);
        }
        Probe* f = make_probe("file");
        if (path && !IsBadReadPtr(path, 2)) {
            // G:\x -> x, relative to the working directory (the package folder: gta6rt.sh / leonida-try.sh cd there)
            const wchar_t* rel = path;
            if (rel[0] && rel[1] == L':') rel += 2;
            while (*rel == L'\\' || *rel == L'/') ++rel;
            wcsncpy_s(f->path, rel, _TRUNCATE);
            f->file = CreateFileW(f->path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f->file == INVALID_HANDLE_VALUE) {
                std::fprintf(stderr, "[dstorage] OpenFile(%ls): open failed, error %lu\n", path, GetLastError());
                std::fflush(stderr);
            }
        }
        if (c) *reinterpret_cast<void**>(c) = f;
        *result = (f->file != INVALID_HANDLE_VALUE) ? S_OK : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        return true;
    }
    if (factory && slot == 5) {  // CreateStatusArray(capacity, name, riid, ppv)
        std::fprintf(stderr, "[dstorage] CreateStatusArray(capacity=%llu, name=%s)\n", (unsigned long long)a,
                     b && !IsBadReadPtr(reinterpret_cast<void*>(b), 1) ? reinterpret_cast<const char*>(b) : "?");
        std::fflush(stderr);
        Probe* p = make_probe("status");
        if (!sync_mode()) {
            auto* s = new StatusState;
            s->count = static_cast<uint32_t>((std::min)(a, uintptr_t(1) << 20));
            s->entries.reset(new std::atomic<int32_t>[s->count]);
            for (uint32_t i = 0; i < s->count; ++i) s->entries[i].store(S_OK, std::memory_order_relaxed);
            p->status = s;
        }
        if (d) *reinterpret_cast<void**>(d) = p;
        *result = S_OK;
        return true;
    }
    if (factory && slot == 10) {  // console CreateQueue-like (desc, riid, ppv)
        std::fprintf(stderr, "[dstorage] factory slot 10 (queue?) desc=%p\n", reinterpret_cast<void*>(a));
        hexdump("queue desc", reinterpret_cast<void*>(a), 48);
        std::fflush(stderr);
        Probe* q = make_probe("queue");
        if (!sync_mode()) {
            auto* s = new QueueState;
            // desc: +0 source type (u64), +8 capacity (u16), +0xA priority (i8), +0x10 name
            if (a && !IsBadReadPtr(reinterpret_cast<void*>(a), 10))
                s->capacity = *reinterpret_cast<const uint16_t*>(a + 8);
            q->queue = s;
            start_workers();
        }
        if (c) *reinterpret_cast<void**>(c) = q;
        *result = S_OK;
        return true;
    }
    if (std::strcmp(self->name, "status") == 0 && slot == 3) {  // IsComplete(index)
        // Entries that never had a marker (and sync mode) answer TRUE, as the old HACK did.
        const uint32_t index = static_cast<uint32_t>(a);
        StatusState* s = self->status;
        *result = (s && index < s->count) ? (s->entries[index].load(std::memory_order_acquire) != kPending) : 1;
        return true;
    }
    if (std::strcmp(self->name, "status") == 0 && slot == 4) {  // GetHResult(index)
        const uint32_t index = static_cast<uint32_t>(a);
        StatusState* s = self->status;
        *result = static_cast<uintptr_t>(static_cast<uint32_t>(
            (s && index < s->count) ? s->entries[index].load(std::memory_order_acquire) : S_OK));
        return true;
    }
    if (std::strcmp(self->name, "queue") == 0 && slot == 3) {  // EnqueueRequest(request*)
        if (!a || IsBadReadPtr(reinterpret_cast<void*>(a), 0x38)) { *result = S_OK; return true; }
        const Req r = parse_request(reinterpret_cast<void*>(a));
        {   // DIAGNOSTIC (agA7): a destination outside every title range (re13: memcpy faults at 0x1cd7d40000)
            const auto d = reinterpret_cast<uintptr_t>(r.dst);
            MEMORY_BASIC_INFORMATION mi{};
            const bool q = VirtualQuery(r.dst, &mi, sizeof mi) != 0;
            if (!q || mi.State == MEM_FREE || (mi.State == MEM_RESERVE && mi.Type != MEM_MAPPED) || (d >> 36) == 1) {
                static std::atomic<int> told{0};
                if (told++ < 12 && !IsBadReadPtr(reinterpret_cast<void*>(a), 0x48)) {
                    const auto* w = reinterpret_cast<const uint32_t*>(a);
                    std::fprintf(stderr, "[dstorage] odd dst %p (state 0x%lx type 0x%lx base %p) comp=%u size %u file %ls off 0x%llx raw:",
                                 r.dst, q ? mi.State : 0ul, q ? mi.Type : 0ul, q ? mi.AllocationBase : nullptr, r.comp,
                                 r.dst_size, r.f ? r.f->path : L"?", (unsigned long long)r.off);
                    for (int i = 0; i < 18; ++i) std::fprintf(stderr, " %08x", w[i]);
                    std::fprintf(stderr, "\n");
                    std::fflush(stderr);
                }
            }
        }
        if (r.comp > 1) {  // DIAGNOSTIC (agA6): raw request bytes of compressed formats we do not decode (BCPack = 14)
            static std::atomic<int> told{0};
            if (told++ < 8 && !IsBadReadPtr(reinterpret_cast<void*>(a), 0x48)) {
                const auto* q = reinterpret_cast<const uint32_t*>(a);
                std::fprintf(stderr, "[dstorage] comp=%u raw req:", r.comp);
                for (int i = 0; i < 18; ++i) std::fprintf(stderr, " %08x", q[i]);
                std::fprintf(stderr, "\n");
            }
        }
        if (sync_mode() || !self->queue) {
            run_request(r);  // synchronous read (old behaviour)
        } else {
            std::lock_guard<std::mutex> lock(g_mu);
            Cmd c;
            c.req = r;
            enqueue_locked(self->queue, c);
        }
        *result = S_OK;
        return true;
    }
    if (!sync_mode() && self->queue && slot == 4) {  // EnqueueStatus(array, index)
        auto* arr = reinterpret_cast<Probe*>(a);
        const uint32_t index = static_cast<uint32_t>(b);
        log_marker(slot, a, b);
        const bool is_array = arr && !IsBadReadPtr(arr, sizeof(Probe)) && arr->name &&
                              std::strcmp(arr->name, "status") == 0 && arr->status;
        if (is_array && index < arr->status->count) {
            std::lock_guard<std::mutex> lock(g_mu);
            arr->status->entries[index].store(kPending, std::memory_order_release);
            Cmd c;
            c.is_status = true;
            c.array = arr->status;
            c.index = index;
            enqueue_locked(self->queue, c);
        } else {
            std::fprintf(stderr, "[dstorage] EnqueueStatus: not a known status entry (array=%p index=%u)\n",
                         reinterpret_cast<void*>(a), index);
            std::fflush(stderr);
        }
        *result = S_OK;
        return true;
    }
    if (!sync_mode() && self->queue && slot == 6) {  // Submit()
        log_marker(slot, a, b);
        std::lock_guard<std::mutex> lock(g_mu);
        submit_locked(self->queue);
        *result = S_OK;
        return true;
    }
    return false;
}

template <int N>
uintptr_t __stdcall slot_fn(Probe* self, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) {
    void* ret = _ReturnAddress();
    if constexpr (N == 0) {  // QueryInterface(riid, ppv)
        char g[64];
        print_guid(reinterpret_cast<void*>(a), g, sizeof g);
        std::fprintf(stderr, "[dstorage] %s::QueryInterface(%s) -> self\n", self->name, g);
        if (b) *reinterpret_cast<void**>(b) = self;
        self->refs++;
        return S_OK;
    } else if constexpr (N == 1) {
        return static_cast<uintptr_t>(++self->refs);
    } else if constexpr (N == 2) {
        return static_cast<uintptr_t>(--self->refs);
    } else {
        uintptr_t result = S_OK;
        if (special(self, N, a, b, c, d, e, f, &result)) return result;
        std::fprintf(stderr,
                     "[dstorage] %s slot %d called from %p: a=0x%llx b=0x%llx c=0x%llx d=0x%llx e=0x%llx f=0x%llx\n",
                     self->name, N, ret, (unsigned long long)a, (unsigned long long)b, (unsigned long long)c,
                     (unsigned long long)d, (unsigned long long)e, (unsigned long long)f);
        std::fflush(stderr);
        return S_OK;
    }
}

template <int... I>
constexpr auto make_vtbl(std::integer_sequence<int, I...>) {
    return std::array<Slot, sizeof...(I)>{&slot_fn<I>...};
}
const auto kVtbl = make_vtbl(std::make_integer_sequence<int, kSlots>{});

Probe* make_probe(const char* name) {
    auto* p = new Probe;
    p->vtbl = kVtbl.data();
    p->name = name;
    return p;
}

// HRESULT DStorageGetFactory(REFIID riid, void** ppv)
HRESULT WINAPI DStorageGetFactory(const GUID* riid, void** ppv) {
    char g[64];
    print_guid(riid, g, sizeof g);
    std::fprintf(stderr, "[dstorage] DStorageGetFactory(%s, %p) -> probe factory\n", g, (void*)ppv);
    std::fflush(stderr);
    // HACK: switch off the title hang watchdog (1.0.2.1: bit 0 of the byte at 0x1471b6340 makes its thread exit).
    // Pipeline creation here takes far longer than on the console, and the watchdog kills the process after 30 s.
    {
        auto* flag = reinterpret_cast<unsigned char*>(0x1471b6340ull);
        DWORD old_protect = 0;
        if (VirtualProtect(flag, 1, PAGE_READWRITE, &old_protect)) {
            *reinterpret_cast<volatile unsigned char*>(flag) |= 1;
            std::fprintf(stderr, "[hack] hang watchdog disabled\n");
        }
    }
    if (!ppv) return E_POINTER;
    static Probe* factory = make_probe("factory");
    factory->refs++;
    *ppv = factory;
    return S_OK;
}

}  // namespace

SHIM_TABLE(dstorage_x,
    {"xcrdapi.dll", "DStorageGetFactory", reinterpret_cast<void*>(&DStorageGetFactory)},
);
