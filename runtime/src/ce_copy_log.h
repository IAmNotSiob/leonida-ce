// HACK (agA26, Leonida CE): copy logging / guarding for the post-Confirm COPY ENGINE page fault (Aftermath rs8/rs9:
// a copy-engine WRITE to heap_base + 0x26EC1E000 (pool window, unmapped) or heap_base + 0xFB6A00000 (no reservation)).
//   LEONIDA_CE_COPY_LOG=1   : log once per distinct (kind, site, title caller), and EVERY copy whose source or destination
//                             range lies outside its VkBuffer / D3D12 resource, or whose title VA is past
//                             LEONIDA_CE_COPY_VA_LIMIT (default 0xE52E00000, the end of the highest title reservation).
//   LEONIDA_CE_COPY_GUARD=1 : additionally skip (and log) every such copy whose DESTINATION range is outside.
// Both default off: then no wrapper is installed and nothing changes.
// Vulkan level: vkCmdCopyBuffer / CopyBufferToImage / CopyImageToBuffer / CopyImage / FillBuffer / UpdateBuffer /
// CopyQueryPoolResults are wrapped in Functions::load (vulkan_backend.h), with a VkBuffer -> size registry (wrapped
// vkCreateBuffer) and a command buffer -> queue family map. Record level: d3d12_com.cpp CopyBufferRegion /
// CopyTextureRegion / CopyResource / CopyTiles check the D3D12 resource size and VA; the title return address comes
// from the console list proxy (d3d12_xs_device.cpp list_slot) through tl_title_ra.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <type_traits>
#include <vector>
#include <vulkan/vulkan.h>

namespace leonida::ce::copylog {
inline bool env_on(const char* n) { const char* e = std::getenv(n); return e && e[0] == '1'; }
inline bool guard_on() { static const bool v = env_on("LEONIDA_CE_COPY_GUARD"); return v; }
inline bool log_on() { static const bool v = env_on("LEONIDA_CE_COPY_LOG") || guard_on(); return v; }
inline std::uint64_t va_limit() {
    static const std::uint64_t v = [] {
        const char* e = std::getenv("LEONIDA_CE_COPY_VA_LIMIT");
        return e && *e ? std::strtoull(e, nullptr, 0) : 0xE52E00000ull;
    }();
    return v;
}

struct State {
    std::mutex mu;
    std::unordered_map<std::uint64_t, std::uint64_t> buffers;     // VkBuffer -> size
    std::unordered_map<std::uint64_t, std::uint32_t> pools;       // VkCommandPool -> queue family
    std::unordered_map<std::uint64_t, std::uint32_t> cbs;         // VkCommandBuffer -> queue family
    std::unordered_map<std::uint64_t, std::uint64_t> res_caller;  // D3D12 resource id -> last title caller that copied it
    std::set<std::string> seen;
    std::set<std::uint64_t> unregistered;
    std::atomic<std::uint64_t> outside{0}, skipped{0};
};
inline State& st() { static State* s = new State; return *s; }  // never destroyed (used at exit by other threads)

// Replay context for the Vulkan-level wrappers: which recorded command emits the copy.
struct Ctx { const char* site = nullptr; std::uint64_t dst_res = 0, src_res = 0; };
inline thread_local Ctx tl_ctx;
inline thread_local std::uint64_t tl_title_ra = 0;  // set by the console list proxy at every list slot call
struct Scope {
    Ctx saved;
    Scope(const char* site, std::uint64_t dst, std::uint64_t src) : saved(tl_ctx) { tl_ctx = {site, dst, src}; }
    ~Scope() { tl_ctx = saved; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

inline std::uint64_t h(const void* p) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)); }
template <class T> inline std::uint64_t hv(T v) {
    if constexpr (sizeof(T) == 8 && !std::is_pointer_v<T>) return static_cast<std::uint64_t>(v);
    else return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(v));
}

// Range [off, off+bytes) outside [0, size)?
inline bool past(std::uint64_t off, std::uint64_t bytes, std::uint64_t size) { return off > size || bytes > size - off; }

// Print a line; OUTSIDE lines are all printed up to 4000, then at powers of two.
inline void emit(const char* tag, const std::string& body, bool outside) {
    if (outside) {
        const std::uint64_t k = ++st().outside;
        if (k > 4000 && (k & (k - 1)) != 0) return;
        std::fprintf(stderr, "[copylog] %s #%llu %s\n", tag, (unsigned long long)k, body.c_str());
    } else {
        std::fprintf(stderr, "[copylog] %s %s\n", tag, body.c_str());
    }
    std::fflush(stderr);
}

// First sighting of (kind, site, caller)?
inline bool first(const char* kind, const char* site, std::uint64_t caller) {
    char key[160];
    std::snprintf(key, sizeof key, "%s|%s|%llx", kind, site ? site : "?", (unsigned long long)caller);
    std::lock_guard lock(st().mu);
    return st().seen.insert(key).second;
}

// ---- record level (D3D12 API, with the title caller) ----
// dst_size / src_size: the D3D12 resource size (0 = unknown). dst_va / src_va: its GPU VA (title VA), 0 = none.
// Returns true when the guard says skip.
inline bool record(const char* kind, std::uint64_t dst_res, std::uint64_t dst_off, std::uint64_t dst_size, std::uint64_t dst_va,
                   std::uint64_t src_res, std::uint64_t src_off, std::uint64_t src_size, std::uint64_t src_va,
                   std::uint64_t bytes) {
    const std::uint64_t ra = tl_title_ra;
    {
        std::lock_guard lock(st().mu);
        if (dst_res) st().res_caller[dst_res] = ra;
        if (src_res && !st().res_caller.count(src_res)) st().res_caller[src_res] = ra;
    }
    const bool bad_dst = (dst_size && past(dst_off, bytes, dst_size)) ||
                         (dst_va && (dst_va + dst_off >= va_limit() || dst_va + dst_off + bytes > va_limit()));
    const bool bad_src = (src_size && past(src_off, bytes, src_size)) ||
                         (src_va && (src_va + src_off >= va_limit() || src_va + src_off + bytes > va_limit()));
    const bool skip = guard_on() && bad_dst;
    const bool fresh = first(kind, "record", ra);
    if (!fresh && !bad_dst && !bad_src) return false;
    char b[512];
    std::snprintf(b, sizeof b,
                  "%s caller=%p | dst res 0x%llx (size 0x%llx, va 0x%llx) +0x%llx | src res 0x%llx (size 0x%llx, va 0x%llx) +0x%llx "
                  "| bytes 0x%llx%s%s",
                  kind, (void*)ra, (unsigned long long)dst_res, (unsigned long long)dst_size, (unsigned long long)dst_va,
                  (unsigned long long)dst_off, (unsigned long long)src_res, (unsigned long long)src_size,
                  (unsigned long long)src_va, (unsigned long long)src_off, (unsigned long long)bytes,
                  bad_dst ? " DST-OUTSIDE" : "", bad_src ? " SRC-OUTSIDE" : "");
    if (skip) ++st().skipped;
    emit(skip ? "REC-SKIP" : (bad_dst || bad_src) ? "REC-OUTSIDE" : "REC-FIRST", b, bad_dst || bad_src);
    return skip;
}

// ---- Vulkan level ----
inline std::uint64_t caller_of(std::uint64_t res) {
    if (!res) return 0;
    std::lock_guard lock(st().mu);
    auto it = st().res_caller.find(res);
    return it == st().res_caller.end() ? 0 : it->second;
}
inline std::uint64_t buffer_size(std::uint64_t b, bool& known) {
    std::lock_guard lock(st().mu);
    auto it = st().buffers.find(b);
    known = it != st().buffers.end();
    if (!known && b && st().unregistered.insert(b).second)
        std::fprintf(stderr, "[copylog] note: VkBuffer 0x%llx has no registered size (created before the wrapper or elsewhere)\n",
                     (unsigned long long)b);
    return known ? it->second : 0;
}
inline int family_of(VkCommandBuffer cb) {
    std::lock_guard lock(st().mu);
    auto it = st().cbs.find(h(cb));
    return it == st().cbs.end() ? -1 : int(it->second);
}

// One Vulkan copy (one region). src/dst: VkBuffer handles (0 = an image side). Bytes for each side (~0 = unknown).
// Returns true when the guard says skip this region.
inline bool vk_check(const char* kind, VkCommandBuffer cb, std::uint64_t src, std::uint64_t src_off, std::uint64_t src_bytes,
                     std::uint64_t dst, std::uint64_t dst_off, std::uint64_t dst_bytes) {
    bool sk = false, dk = false;
    const std::uint64_t ssz = src ? buffer_size(src, sk) : 0, dsz = dst ? buffer_size(dst, dk) : 0;
    const bool bad_src = src && sk && (src_bytes == ~0ull ? src_off >= ssz : past(src_off, src_bytes, ssz));
    const bool bad_dst = dst && dk && (dst_bytes == ~0ull ? dst_off >= dsz : past(dst_off, dst_bytes, dsz));
    const Ctx c = tl_ctx;
    const std::uint64_t ra = caller_of(c.dst_res ? c.dst_res : c.src_res);
    const bool fresh = first(kind, c.site, ra);
    if (!fresh && !bad_src && !bad_dst) return false;
    const bool skip = guard_on() && bad_dst;
    char b[600];
    std::snprintf(b, sizeof b,
                  "%s site=%s caller=%p queue-family=%d cb=%p | src buf 0x%llx (size 0x%llx%s) +0x%llx bytes 0x%llx | dst buf 0x%llx "
                  "(size 0x%llx%s) +0x%llx bytes 0x%llx | res dst 0x%llx src 0x%llx%s%s",
                  kind, c.site ? c.site : "?", (void*)ra, family_of(cb), (void*)cb, (unsigned long long)src,
                  (unsigned long long)ssz, (src && !sk) ? " unknown" : "", (unsigned long long)src_off,
                  (unsigned long long)src_bytes, (unsigned long long)dst, (unsigned long long)dsz,
                  (dst && !dk) ? " unknown" : "", (unsigned long long)dst_off, (unsigned long long)dst_bytes,
                  (unsigned long long)c.dst_res, (unsigned long long)c.src_res, bad_dst ? " DST-OUTSIDE" : "",
                  bad_src ? " SRC-OUTSIDE" : "");
    if (skip) ++st().skipped;
    emit(skip ? "VK-SKIP" : (bad_dst || bad_src) ? "VK-OUTSIDE" : "VK-FIRST", b, bad_dst || bad_src);
    return skip;
}

// A title-VA addressed GPU copy (XDXR BVH copy, compute): VAs past the limit count as outside.
inline bool va_check(const char* kind, std::uint64_t src_va, std::uint64_t dst_va, std::uint64_t bytes) {
    const bool bad_dst = dst_va >= va_limit() || bytes > va_limit() - dst_va;
    const bool bad_src = src_va >= va_limit() || bytes > va_limit() - src_va;
    const Ctx c = tl_ctx;
    const std::uint64_t ra = caller_of(c.dst_res);
    if (!first(kind, c.site, ra) && !bad_dst && !bad_src) return false;
    const bool skip = guard_on() && bad_dst;
    char b[300];
    std::snprintf(b, sizeof b, "%s site=%s src va 0x%llx dst va 0x%llx bytes 0x%llx%s%s", kind, c.site ? c.site : "?",
                  (unsigned long long)src_va, (unsigned long long)dst_va, (unsigned long long)bytes,
                  bad_dst ? " DST-OUTSIDE" : "", bad_src ? " SRC-OUTSIDE" : "");
    if (skip) ++st().skipped;
    emit(skip ? "VA-SKIP" : (bad_dst || bad_src) ? "VA-OUTSIDE" : "VA-FIRST", b, bad_dst || bad_src);
    return skip;
}

inline std::uint64_t texel_guess(std::uint64_t row_len, std::uint64_t height, std::uint64_t depth) {
    // Bytes a buffer<->image region spans, at most 16 bytes per texel (an upper bound; block formats are smaller).
    return row_len * height * depth * 16;
}

// Install the wrappers into a Functions-like table (vulkan_backend.h Functions::load). Idempotent per table.
template <class F> inline void install(F& vk) {
    if (!log_on()) return;
    static PFN_vkCreateBuffer o_create = nullptr;
    static PFN_vkDestroyBuffer o_destroy = nullptr;
    static PFN_vkCreateCommandPool o_pool = nullptr;
    static PFN_vkAllocateCommandBuffers o_alloc = nullptr;
    static PFN_vkCmdCopyBuffer o_copy = nullptr;
    static PFN_vkCmdCopyBufferToImage o_b2i = nullptr;
    static PFN_vkCmdCopyImageToBuffer o_i2b = nullptr;
    static PFN_vkCmdCopyImage o_i2i = nullptr;
    static PFN_vkCmdFillBuffer o_fill = nullptr;
    static PFN_vkCmdUpdateBuffer o_update = nullptr;
    static PFN_vkCmdCopyQueryPoolResults o_query = nullptr;
    static std::atomic<int> installs{0};
    {
        static const PFN_vkCreateBuffer w = [](VkDevice d, const VkBufferCreateInfo* i, const VkAllocationCallbacks* a, VkBuffer* out) {
            const VkResult r = o_create(d, i, a, out);
            if (r == VK_SUCCESS && out && i) { std::lock_guard lock(st().mu); st().buffers[hv(*out)] = i->size; }
            return r;
        };
        if (vk.vkCreateBuffer && vk.vkCreateBuffer != w) { o_create = vk.vkCreateBuffer; vk.vkCreateBuffer = w; }
    }
    {
        static const PFN_vkDestroyBuffer w = [](VkDevice d, VkBuffer b, const VkAllocationCallbacks* a) {
            { std::lock_guard lock(st().mu); st().buffers.erase(hv(b)); }
            o_destroy(d, b, a);
        };
        if (vk.vkDestroyBuffer && vk.vkDestroyBuffer != w) { o_destroy = vk.vkDestroyBuffer; vk.vkDestroyBuffer = w; }
    }
    {
        static const PFN_vkCreateCommandPool w = [](VkDevice d, const VkCommandPoolCreateInfo* i, const VkAllocationCallbacks* a, VkCommandPool* out) {
            const VkResult r = o_pool(d, i, a, out);
            if (r == VK_SUCCESS && out && i) { std::lock_guard lock(st().mu); st().pools[hv(*out)] = i->queueFamilyIndex; }
            return r;
        };
        if (vk.vkCreateCommandPool && vk.vkCreateCommandPool != w) { o_pool = vk.vkCreateCommandPool; vk.vkCreateCommandPool = w; }
    }
    {
        static const PFN_vkAllocateCommandBuffers w = [](VkDevice d, const VkCommandBufferAllocateInfo* i, VkCommandBuffer* out) {
            const VkResult r = o_alloc(d, i, out);
            if (r == VK_SUCCESS && out && i) {
                std::lock_guard lock(st().mu);
                auto it = st().pools.find(hv(i->commandPool));
                const std::uint32_t fam = it == st().pools.end() ? 0xffffffffu : it->second;
                for (std::uint32_t k = 0; k < i->commandBufferCount; ++k) st().cbs[h(out[k])] = fam;
            }
            return r;
        };
        if (vk.vkAllocateCommandBuffers && vk.vkAllocateCommandBuffers != w) { o_alloc = vk.vkAllocateCommandBuffers; vk.vkAllocateCommandBuffers = w; }
    }
    {
        static const PFN_vkCmdCopyBuffer w = [](VkCommandBuffer cb, VkBuffer s, VkBuffer d, std::uint32_t n, const VkBufferCopy* r) {
            std::vector<VkBufferCopy> keep;
            bool dropped = false;
            for (std::uint32_t k = 0; k < n; ++k) {
                if (vk_check("vkCmdCopyBuffer", cb, hv(s), r[k].srcOffset, r[k].size, hv(d), r[k].dstOffset, r[k].size)) dropped = true;
                else keep.push_back(r[k]);
            }
            if (!dropped) return o_copy(cb, s, d, n, r);
            if (!keep.empty()) o_copy(cb, s, d, std::uint32_t(keep.size()), keep.data());
        };
        if (vk.vkCmdCopyBuffer && vk.vkCmdCopyBuffer != w) { o_copy = vk.vkCmdCopyBuffer; vk.vkCmdCopyBuffer = w; }
    }
    {
        static const PFN_vkCmdCopyBufferToImage w = [](VkCommandBuffer cb, VkBuffer s, VkImage i, VkImageLayout l, std::uint32_t n,
                                       const VkBufferImageCopy* r) {
            for (std::uint32_t k = 0; k < n; ++k)
                vk_check("vkCmdCopyBufferToImage", cb, hv(s), r[k].bufferOffset, ~0ull, 0, 0, 0);
            o_b2i(cb, s, i, l, n, r);
        };
        if (vk.vkCmdCopyBufferToImage && vk.vkCmdCopyBufferToImage != w) { o_b2i = vk.vkCmdCopyBufferToImage; vk.vkCmdCopyBufferToImage = w; }
    }
    {
        static const PFN_vkCmdCopyImageToBuffer w = [](VkCommandBuffer cb, VkImage i, VkImageLayout l, VkBuffer d, std::uint32_t n,
                                       const VkBufferImageCopy* r) {
            std::vector<VkBufferImageCopy> keep;
            bool dropped = false;
            for (std::uint32_t k = 0; k < n; ++k) {
                // The destination offset itself must be inside; the span is only estimated, so it is not guarded.
                if (vk_check("vkCmdCopyImageToBuffer", cb, 0, 0, 0, hv(d), r[k].bufferOffset, ~0ull)) dropped = true;
                else keep.push_back(r[k]);
            }
            if (!dropped) return o_i2b(cb, i, l, d, n, r);
            if (!keep.empty()) o_i2b(cb, i, l, d, std::uint32_t(keep.size()), keep.data());
        };
        if (vk.vkCmdCopyImageToBuffer && vk.vkCmdCopyImageToBuffer != w) { o_i2b = vk.vkCmdCopyImageToBuffer; vk.vkCmdCopyImageToBuffer = w; }
    }
    {
        static const PFN_vkCmdCopyImage w = [](VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, std::uint32_t n,
                               const VkImageCopy* r) {
            vk_check("vkCmdCopyImage", cb, 0, 0, 0, 0, 0, 0);
            o_i2i(cb, s, sl, d, dl, n, r);
        };
        if (vk.vkCmdCopyImage && vk.vkCmdCopyImage != w) { o_i2i = vk.vkCmdCopyImage; vk.vkCmdCopyImage = w; }
    }
    {
        static const PFN_vkCmdFillBuffer w = [](VkCommandBuffer cb, VkBuffer d, VkDeviceSize off, VkDeviceSize n, std::uint32_t v) {
            if (vk_check("vkCmdFillBuffer", cb, 0, 0, 0, hv(d), off, n == VK_WHOLE_SIZE ? ~0ull : n)) return;
            o_fill(cb, d, off, n, v);
        };
        if (vk.vkCmdFillBuffer && vk.vkCmdFillBuffer != w) { o_fill = vk.vkCmdFillBuffer; vk.vkCmdFillBuffer = w; }
    }
    {
        static const PFN_vkCmdUpdateBuffer w = [](VkCommandBuffer cb, VkBuffer d, VkDeviceSize off, VkDeviceSize n, const void* p) {
            if (vk_check("vkCmdUpdateBuffer", cb, 0, 0, 0, hv(d), off, n)) return;
            o_update(cb, d, off, n, p);
        };
        if (vk.vkCmdUpdateBuffer && vk.vkCmdUpdateBuffer != w) { o_update = vk.vkCmdUpdateBuffer; vk.vkCmdUpdateBuffer = w; }
    }
    {
        static const PFN_vkCmdCopyQueryPoolResults w = [](VkCommandBuffer cb, VkQueryPool q, std::uint32_t first_q, std::uint32_t count,
                                          VkBuffer d, VkDeviceSize off, VkDeviceSize stride, VkQueryResultFlags f) {
            const std::uint64_t bytes = count ? stride * (count - 1) + ((f & VK_QUERY_RESULT_64_BIT) ? 8 : 4) : 0;
            if (vk_check("vkCmdCopyQueryPoolResults", cb, 0, 0, 0, hv(d), off, bytes)) return;
            o_query(cb, q, first_q, count, d, off, stride, f);
        };
        if (vk.vkCmdCopyQueryPoolResults && vk.vkCmdCopyQueryPoolResults != w) { o_query = vk.vkCmdCopyQueryPoolResults; vk.vkCmdCopyQueryPoolResults = w; }
    }
    if (installs++ == 0)
        std::fprintf(stderr, "[copylog] installed (COPY_LOG=%d COPY_GUARD=%d VA_LIMIT=0x%llx)\n", int(log_on()), int(guard_on()),
                     (unsigned long long)va_limit());
}
}  // namespace leonida::ce::copylog
