// DIAGNOSTIC (agA39, Leonida CE): LEONIDA_CE_NANPROBE=<frame>[:<max samples>] samples one colour target (by its RTDIAG
// T# address, LEONIDA_CE_NANPROBE_TARGET, default 0x100001b00 = the 2560x1440 RGBA16F scene target in rt59) after every
// draw that renders into it, after every dispatch / ExecuteIndirect, after clears and copies into it, and at the start
// of every list replayed while that frame is built. Each sample copies 8 texels spread over the image into an arena
// region (vkCmdCopyImageToBuffer). ~12 frames later the replay prints "[nanprobe]" lines: per sample the kind, the
// pipeline and how many of the 8 texels are NaN/Inf, so the first command after which the target holds NaN is named.
// The device allocates the region (2 MiB, 128 bytes per sample) when the variable is set.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vulkan/vulkan.h>

namespace leonida::ce::nanprobe {
struct Config {
    std::uint64_t frame = 0;   // 0 = off (or auto)
    bool automatic = false;    // "auto[:<min frame>[:<draws>]]": arm 2 frames after the first frame >= min frame in
    std::uint64_t min_frame = 3000;  // which at least <draws> draws rendered into the target
    std::uint32_t threshold = 150;
    std::uint32_t max = 16384;  // samples (128 bytes each)
    std::uint64_t target = 0x100001b00ull;
};
inline const Config& config() {
    static const Config c = [] {
        Config r;
        if (const char* e = std::getenv("LEONIDA_CE_NANPROBE")) {
            char* end = nullptr;
            if (std::strncmp(e, "auto", 4) == 0) {
                r.automatic = true;
                end = const_cast<char*>(e + 4);
                if (*end == ':') r.min_frame = std::strtoull(end + 1, &end, 0);
                if (end && *end == ':') r.threshold = std::uint32_t(std::strtoul(end + 1, &end, 0));
            } else {
                r.frame = std::strtoull(e, &end, 0);
                if (end && *end == ':') r.max = std::uint32_t(std::strtoul(end + 1, nullptr, 0));
            }
            if (r.max == 0 || r.max > 16384) r.max = 16384;
        }
        if (const char* t = std::getenv("LEONIDA_CE_NANPROBE_TARGET")) r.target = std::strtoull(t, nullptr, 0);
        return r;
    }();
    return c;
}
inline bool on() { return config().frame != 0 || config().automatic; }
// The probed frame: the configured one, or 0 until auto mode arms.
inline std::atomic<std::uint64_t>& armed() { static std::atomic<std::uint64_t> a{config().frame}; return a; }
// Auto mode: count a draw into the target while frame f is built; arms when a whole frame had enough of them.
inline void count_target_draw(std::uint64_t f) {
    static std::atomic<std::uint64_t> cur_frame{0}, cur_n{0};
    if (!config().automatic || armed().load(std::memory_order_relaxed)) return;
    std::uint64_t pf = cur_frame.load(std::memory_order_relaxed);
    if (pf != f && cur_frame.compare_exchange_strong(pf, f)) {
        const std::uint64_t n = cur_n.exchange(0);
        if (pf + 1 == f && f >= config().min_frame && n >= config().threshold) {
            armed().store(f + 2);
            std::fprintf(stderr, "[nanprobe] auto: frame %llu had %llu draws into the target: probing frame %llu\n",
                         (unsigned long long)pf, (unsigned long long)n, (unsigned long long)(f + 2));
            std::fflush(stderr);
        }
    }
    cur_n.fetch_add(1, std::memory_order_relaxed);
}
constexpr std::uint32_t slot_bytes = 128;
constexpr std::uint32_t pixels = 8;
constexpr std::uint64_t region_bytes = 2ull << 20;
// Set by the device: the arena buffer and the region's offset in it (arena VA).
inline std::atomic<VkBuffer>& buffer() { static std::atomic<VkBuffer> b{VK_NULL_HANDLE}; return b; }
inline std::atomic<std::uint64_t>& region() { static std::atomic<std::uint64_t> r{0}; return r; }
}  // namespace leonida::ce::nanprobe
