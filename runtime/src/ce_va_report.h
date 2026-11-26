// HACK (agA28, Leonida CE): LEONIDA_CE_VA_REPORT=<file> - who owns a GPU virtual address. Nsight Aftermath (rs8-rs13)
// shows the post-landing device loss as a Copy Engine WRITE to an untranslatable GPU VA in a stable band (0x12A6D00000,
// 0x12B69B0000; the band moved by the POOL_GPU buffer size when that switch was on, so it is driver VA handed out after
// the arena). VK_EXT_device_address_binding_report makes the driver report every GPU VA bind / unbind (buffers, images,
// memory, driver-internal objects) through a VK_EXT_debug_utils messenger. Every event goes to <file> as one line:
//   t=<s> B|U[i] base=0x.. end=0x.. size=0x.. obj=<type>:0x<handle>        (B bind, U unbind, i driver-internal)
// LEONIDA_CE_VA_REPORT_WATCH=lo:hi (hex, default 0x1200000000:0x1400000000): events overlapping it also go to stderr.
// LEONIDA_CE_VA_REPORT_AT=a,b,.. (hex, default the rs10/rs13 fault VAs): at device loss, the bind history of each
// address (and of every VK_EXT_device_fault address) is printed: every event whose range covers it, plus the nearest
// ranges below and above that were live at the end. Default off: no extension, no messenger, nothing changes.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace leonida::ce::vareport {
struct Event {
    double t;
    std::uint64_t base, size, handle;
    std::uint32_t objtype;
    bool bind, internal;
};
struct State {
    std::mutex m;
    std::vector<Event> events;
    std::FILE* file = nullptr;
    std::uint64_t watch_lo = 0x1200000000ull, watch_hi = 0x1400000000ull;
    std::vector<std::uint64_t> at{0x12A6D00000ull, 0x12B69B0000ull};
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    std::uint64_t binds = 0, unbinds = 0;
};
inline State& state() { static State s; return s; }
inline const char* path() {
    static const char* p = [] {
        const char* e = std::getenv("LEONIDA_CE_VA_REPORT");
        return e && e[0] && !(e[0] == '0' && !e[1]) ? e : nullptr;
    }();
    return p;
}
inline bool on() { return path() != nullptr; }
inline double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - state().t0).count();
}
inline void print_event(std::FILE* f, const char* prefix, const Event& e) {
    std::fprintf(f, "%st=%.3f %c%s base=0x%llx end=0x%llx size=0x%llx obj=%u:0x%llx\n", prefix, e.t, e.bind ? 'B' : 'U',
                 e.internal ? "i" : "", (unsigned long long)e.base, (unsigned long long)(e.base + e.size),
                 (unsigned long long)e.size, e.objtype, (unsigned long long)e.handle);
}
inline VKAPI_ATTR VkBool32 VKAPI_CALL callback(VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT types,
                                               const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (!(types & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) || !data) return VK_FALSE;
    const VkDeviceAddressBindingCallbackDataEXT* b = nullptr;
    for (auto* p = static_cast<const VkBaseInStructure*>(data->pNext); p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) {
            b = reinterpret_cast<const VkDeviceAddressBindingCallbackDataEXT*>(p);
            break;
        }
    if (!b) return VK_FALSE;
    Event e{now_s(), b->baseAddress, b->size, data->objectCount ? data->pObjects[0].objectHandle : 0,
            data->objectCount ? std::uint32_t(data->pObjects[0].objectType) : 0u,
            b->bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT,
            (b->flags & VK_DEVICE_ADDRESS_BINDING_INTERNAL_OBJECT_BIT_EXT) != 0};
    auto& s = state();
    std::lock_guard lock(s.m);
    s.events.push_back(e);
    (e.bind ? s.binds : s.unbinds)++;
    if (s.file) {
        print_event(s.file, "", e);
        if ((s.events.size() & 63) == 0) std::fflush(s.file);
    }
    if (e.base < s.watch_hi && e.base + e.size > s.watch_lo) {
        print_event(stderr, "[hack] VA_REPORT watch: ", e);
        std::fflush(stderr);
    }
    return VK_FALSE;
}
// agA28 (IMG_CHECK): an extra line in the VA file (image descriptions), so obj=10 handles there can be named offline.
inline void note(const std::string& line) {
    if (!on()) return;
    auto& s = state();
    std::lock_guard lock(s.m);
    if (s.file) std::fprintf(s.file, "t=%.3f %s\n", now_s(), line.c_str());
}
// The device feature struct to chain into vkCreateDevice (nullptr when off or unsupported) and the extension name.
inline VkPhysicalDeviceAddressBindingReportFeaturesEXT& features() {
    static VkPhysicalDeviceAddressBindingReportFeaturesEXT f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT};
    return f;
}
// After vkCreateDevice: open the file, parse the knobs, create the messenger on the instance.
inline void install(VkInstance instance, PFN_vkGetInstanceProcAddr get) {
    auto& s = state();
    if (const char* w = std::getenv("LEONIDA_CE_VA_REPORT_WATCH")) {
        char* end = nullptr;
        const auto lo = std::strtoull(w, &end, 16);
        if (end && *end == ':') {
            s.watch_lo = lo;
            s.watch_hi = std::strtoull(end + 1, nullptr, 16);
        }
    }
    if (const char* a = std::getenv("LEONIDA_CE_VA_REPORT_AT")) {
        s.at.clear();
        for (const char* p = a; *p;) {
            char* end = nullptr;
            const auto v = std::strtoull(p, &end, 16);
            if (end == p) break;
            s.at.push_back(v);
            p = *end == ',' ? end + 1 : end;
        }
    }
    s.file = std::fopen(path(), "w");
    const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(get(instance, "vkCreateDebugUtilsMessengerEXT"));
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_EXTENSION_NOT_PRESENT;
    if (create) {
        VkDebugUtilsMessengerCreateInfoEXT ci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT;
        ci.pfnUserCallback = callback;
        r = create(instance, &ci, nullptr, &messenger);
    }
    std::fprintf(stderr, "[hack] VA_REPORT: messenger %s (%d), file %s %s, watch [0x%llx, 0x%llx), %zu lookup address(es)\n",
                 r == VK_SUCCESS ? "on" : "FAILED", int(r), path(), s.file ? "open" : "NOT OPEN", (unsigned long long)s.watch_lo,
                 (unsigned long long)s.watch_hi, s.at.size());
    std::fflush(stderr);
}
// The bind history of one address: every event covering it, then the nearest ranges live at the end below / above.
inline void lookup_locked(State& s, std::uint64_t va) {
    std::fprintf(stderr, "[hack] VA_REPORT lookup 0x%llx (%llu binds, %llu unbinds so far):\n", (unsigned long long)va,
                 (unsigned long long)s.binds, (unsigned long long)s.unbinds);
    unsigned covering = 0;
    for (const auto& e : s.events)
        if (va >= e.base && va < e.base + e.size) {
            print_event(stderr, "[hack]   covers: ", e);
            if (++covering >= 64) { std::fprintf(stderr, "[hack]   (more covering events cut)\n"); break; }
        }
    // Live ranges at the end: a bind not followed by an unbind of the same (base, size).
    std::map<std::pair<std::uint64_t, std::uint64_t>, const Event*> open;
    for (const auto& e : s.events) {
        if (e.bind) open[{e.base, e.size}] = &e;
        else open.erase({e.base, e.size});
    }
    std::vector<const Event*> live;
    live.reserve(open.size());
    for (const auto& kv : open) live.push_back(kv.second);
    const Event *below = nullptr, *above = nullptr;
    for (const Event* e : live) {
        if (e->base + e->size <= va && (!below || e->base + e->size > below->base + below->size)) below = e;
        if (e->base > va && (!above || e->base < above->base)) above = e;
    }
    std::fprintf(stderr, "[hack]   %u covering event(s); %zu live range(s) at the end\n", covering, live.size());
    if (below) print_event(stderr, "[hack]   live below: ", *below);
    if (above) print_event(stderr, "[hack]   live above: ", *above);
    // The last unbinds near the address (within 256 MiB), newest first.
    unsigned nearby = 0;
    for (auto it = s.events.rbegin(); it != s.events.rend() && nearby < 8; ++it)
        if (!it->bind && it->base < va + (256ull << 20) && it->base + it->size + (256ull << 20) > va) {
            print_event(stderr, "[hack]   recent unbind nearby: ", *it);
            ++nearby;
        }
}
inline void lookup(std::uint64_t va) {
    if (!on()) return;
    auto& s = state();
    std::lock_guard lock(s.m);
    lookup_locked(s, va);
    std::fflush(stderr);
}
// Device-loss dump: the configured addresses; flushes the file.
inline void dump() {
    if (!on()) return;
    auto& s = state();
    std::lock_guard lock(s.m);
    std::fprintf(stderr, "[hack] VA_REPORT at device loss t=%.3f: %zu events\n", now_s(), s.events.size());
    for (std::uint64_t va : s.at) lookup_locked(s, va);
    if (s.file) std::fflush(s.file);
    std::fflush(stderr);
}
}  // namespace leonida::ce::vareport
