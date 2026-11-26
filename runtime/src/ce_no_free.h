// HACK (main session, LEONIDA_CE_NO_FREE=1, default off): a use-after-free test. Nsight Aftermath (rs8-rs13) shows the
// post-landing device loss as a Copy Engine WRITE to an unmapped GPU VA in a stable band (0x12A6A00000 / 0x12B6A00000),
// while build 90's copy log found every recorded copy inside a live buffer. If the title releases a resource that a
// recorded copy still targets and the runtime frees its memory before the copy runs, the copy writes into nothing.
// With the switch, vkFreeMemory / vkDestroyBuffer / vkDestroyImage do nothing (memory is leaked for the run), so a
// late copy lands in still-mapped memory. Counts are printed at powers of two.
#pragma once
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vulkan/vulkan.h>

namespace leonida::ce::nofree {
// 1 = skip vkFreeMemory, vkDestroyBuffer and vkDestroyImage; 2 = skip only vkFreeMemory (objects are destroyed, their
// memory and its GPU mapping stay; rs15 showed that skipping image destroys trips the driver).
inline int level() {
    static const int v = [] {
        const char* e = std::getenv("LEONIDA_CE_NO_FREE");
        return e && (e[0] == '1' || e[0] == '2') ? e[0] - '0' : 0;
    }();
    return v;
}
inline bool on() { return level() != 0; }
inline void tell(const char* what, std::atomic<std::uint64_t>& n) {
    const auto k = ++n;
    if ((k & (k - 1)) == 0) {
        std::fprintf(stderr, "[hack] NO_FREE: %s skipped %llu times\n", what, (unsigned long long)k);
        std::fflush(stderr);
    }
}
template <class F> inline void install(F& vk) {
    if (!on()) return;
    static std::atomic<std::uint64_t> n_mem{0}, n_buf{0}, n_img{0};
    static const PFN_vkFreeMemory fm = [](VkDevice, VkDeviceMemory m, const VkAllocationCallbacks*) {
        if (m) tell("vkFreeMemory", n_mem);
    };
    static const PFN_vkDestroyBuffer db = [](VkDevice, VkBuffer b, const VkAllocationCallbacks*) {
        if (b) tell("vkDestroyBuffer", n_buf);
    };
    static const PFN_vkDestroyImage di = [](VkDevice, VkImage i, const VkAllocationCallbacks*) {
        if (i) tell("vkDestroyImage", n_img);
    };
    if (vk.vkFreeMemory) vk.vkFreeMemory = fm;
    if (level() == 1) {
        if (vk.vkDestroyBuffer) vk.vkDestroyBuffer = db;
        if (vk.vkDestroyImage) vk.vkDestroyImage = di;
    }
    static std::atomic<bool> said{false};
    if (!said.exchange(true)) {
        std::fprintf(stderr, "[hack] NO_FREE=%d: on (%s)\n", level(), level() == 1 ? "vkFreeMemory / vkDestroyBuffer / vkDestroyImage are no-ops" : "vkFreeMemory is a no-op");
        std::fflush(stderr);
    }
}
}  // namespace leonida::ce::nofree
