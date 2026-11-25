// HACK (agA12, Leonida CE): GPU breadcrumbs for the post-Confirm device loss. LEONIDA_CE_CHECKPOINTS=1 enables
// VK_NV_device_diagnostic_checkpoints; the replay sets a checkpoint before every draw/dispatch (marker = the VkPipeline)
// and the device-lost hook prints the last checkpoints each queue reached, with the pipeline's shader names.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

namespace leonida::ce {
inline std::atomic<PFN_vkCmdSetCheckpointNV>& set_checkpoint_fn() {
    static std::atomic<PFN_vkCmdSetCheckpointNV> f{nullptr};
    return f;
}
inline std::mutex& names_mutex() { static std::mutex m; return m; }
inline std::unordered_map<std::uint64_t, std::string>& names() {
    static std::unordered_map<std::uint64_t, std::string> n;
    return n;
}
inline void name_pipeline(VkPipeline p, const std::string& name) {
    if (!p || !set_checkpoint_fn().load(std::memory_order_relaxed)) return;
    std::lock_guard lock(names_mutex());
    names()[reinterpret_cast<std::uint64_t>(p)] = name;
}
inline std::string pipeline_name(std::uint64_t p) {
    std::lock_guard lock(names_mutex());
    const auto it = names().find(p);
    return it == names().end() ? std::string("?") : it->second;
}
// HACK (agA16, LEONIDA_CE_PTR_CHECK): the device installs a scan of the root views / descriptor tables of a dispatch.
using PtrCheckFn = void (*)(const std::uint32_t* user_data, std::uint32_t view_mask, std::uint32_t table_mask,
                            std::uint64_t pipeline);
inline std::atomic<PtrCheckFn>& ptr_check_fn() {
    static std::atomic<PtrCheckFn> f{nullptr};
    return f;
}
// HACK (agA17, LEONIDA_CE_HEAP_MIRROR): copies mirrored console descriptor heaps into the arena (ExecuteCommandLists).
using HeapSyncFn = void (*)();
inline std::atomic<HeapSyncFn>& heap_sync_fn() {
    static std::atomic<HeapSyncFn> f{nullptr};
    return f;
}
// agA20 (LEONIDA_CE_INDIRECT_SRC diagnostics): the arena host mapping, for raw reads of GPU-only (DEFAULT) buffers.
inline std::atomic<std::uint8_t*>& arena_host() {
    static std::atomic<std::uint8_t*> p{nullptr};
    return p;
}
inline std::atomic<std::uint64_t>& arena_bytes() {
    static std::atomic<std::uint64_t> n{0};
    return n;
}
// agA20 (LEONIDA_CE_MIRROR_REMAP, default on with HEAP_MIRROR): the console heap mirror [lo, hi) in arena VAs and the
// title heap address it copies. Replay hacks read descriptor tables from TITLE memory at the table VA (RECTLIST
// vertices at table+0x20, CB fix, VSHARP sync); with the mirror a table VA names the mirror, whose title-memory bytes
// are unrelated (ri53+: presented surface all zero). They read the title heap instead.
inline std::atomic<std::uint64_t>& mirror_lo() { static std::atomic<std::uint64_t> v{0}; return v; }
inline std::atomic<std::uint64_t>& mirror_hi() { static std::atomic<std::uint64_t> v{0}; return v; }
inline std::atomic<std::uint64_t>& mirror_title() { static std::atomic<std::uint64_t> v{0}; return v; }
inline std::uint64_t mirror_to_title(std::uint64_t va) {
    const std::uint64_t lo = mirror_lo().load(std::memory_order_relaxed);
    if (!lo || va < lo || va >= mirror_hi().load(std::memory_order_relaxed)) return va;
    return mirror_title().load(std::memory_order_relaxed) + (va - lo);
}
// agA20 (LEONIDA_CE_MIRROR_POKE, default on): a replay hack that patches a table in the title heap also patches the
// arena mirror, which was synced at ExecuteCommandLists BEFORE the replay ran (else the GPU sees the patch one submit late).
inline void mirror_poke32(std::uint64_t va, std::uint32_t v) {
    static const bool on = [] { const char* e = std::getenv("LEONIDA_CE_MIRROR_POKE"); return !e || e[0] != '0'; }();
    const std::uint64_t lo = mirror_lo().load(std::memory_order_relaxed);
    std::uint8_t* const a = arena_host().load(std::memory_order_relaxed);
    if (!on || !lo || !a || va < lo || va + 4 > mirror_hi().load(std::memory_order_relaxed)) return;
    *reinterpret_cast<volatile std::uint32_t*>(a + va) = v;
}
inline void checkpoint(VkCommandBuffer cb, std::uint64_t marker) {
    if (auto f = set_checkpoint_fn().load(std::memory_order_relaxed)) f(cb, reinterpret_cast<const void*>(marker));
}
struct QueueSet {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetQueueCheckpointDataNV get = nullptr;
    std::vector<std::pair<const char*, VkQueue>> queues;
};
inline QueueSet& queue_set() { static QueueSet q; return q; }
inline void print_checkpoints() {
    auto& qs = queue_set();
    if (!qs.get) return;
    for (const auto& [kind, q] : qs.queues) {
        std::uint32_t n = 0;
        qs.get(q, &n, nullptr);
        std::vector<VkCheckpointDataNV> data(n, VkCheckpointDataNV{VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
        if (n) qs.get(q, &n, data.data());
        std::fprintf(stderr, "[hack] CHECKPOINTS %s queue %p: %u\n", kind, (void*)q, n);
        for (const auto& d : data) {
            const auto m = reinterpret_cast<std::uint64_t>(d.pCheckpointMarker);
            std::fprintf(stderr, "[hack]   stage 0x%x marker 0x%llx %s\n", unsigned(d.stage), (unsigned long long)m,
                         (m >> 48) == 0xCE00 ? "(ce marker: list begin = 0xCE00, indirect = 0xCE01 + kind)" : pipeline_name(m).c_str());
        }
        std::fflush(stderr);
    }
}
}  // namespace leonida::ce
