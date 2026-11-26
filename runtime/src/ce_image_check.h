// HACK (agA28, Leonida CE): LEONIDA_CE_IMG_CHECK=1|2 - bounds checks for every Vulkan command that writes (or reads) an
// image by region. rt3 (build 95, VA_REPORT) put the Aftermath Copy Engine write fault EXACTLY at the end of a live 15 MiB
// VkImage (0x12bd920000-0x12be820000, fault 0x12be820000, the next image after a 64 KiB gap): a copy runs past the end of its
// destination image. Build 90's copy log never checked image regions (CopyBufferToImage: source offset only).
// - vkCreateImage / vkDestroyImage are wrapped: image -> {type, format, extent, mips, layers, samples, flags, usage, tiling};
//   with LEONIDA_CE_VA_REPORT the VA file gets "IMG 0x<handle> ..." so an obj=10 handle there names its image.
// - vkCmdCopyBufferToImage / CopyImageToBuffer / CopyImage / BlitImage / ResolveImage / ClearColorImage /
//   ClearDepthStencilImage: each region's mip, array layers and offset+extent (rounded up to the format's block size) are
//   checked against the image. A miss prints "[imgcheck] OUTSIDE <cmd> <dst|src> img 0x.. <desc> | region .. | site .. caller
//   ..": the first 64, then powers of two.
// - =2 also drops the regions that miss (the copy engine never writes past the image).
// Default off: no wrapper is installed.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>
#include "ce_copy_log.h"
#include "ce_va_report.h"

namespace leonida::ce::imgcheck {
inline int level() {
    static const int v = [] {
        const char* e = std::getenv("LEONIDA_CE_IMG_CHECK");
        return e && (e[0] == '1' || e[0] == '2') ? e[0] - '0' : 0;
    }();
    return v;
}
struct Img {
    VkImageType type;
    VkFormat format;
    VkExtent3D extent;
    std::uint32_t mips, layers;
    VkSampleCountFlagBits samples;
    VkImageCreateFlags flags;
    VkImageUsageFlags usage;
    VkImageTiling tiling;
};
struct State {
    std::mutex m;
    std::unordered_map<std::uint64_t, Img> imgs;
    std::atomic<std::uint64_t> outside{0}, dropped{0}, checked{0}, ds_transfer{0}, bind_short{0}, binds{0};
    std::unordered_map<std::uint64_t, VkDeviceSize> req;       // image -> vkGetImageMemoryRequirements size
    std::unordered_map<std::uint64_t, VkDeviceSize> mem;       // VkDeviceMemory -> allocation size
    std::unordered_map<std::uint64_t, std::uint32_t> pools;    // VkCommandPool -> queue family
    std::unordered_map<std::uint64_t, std::uint32_t> cbs;      // VkCommandBuffer -> queue family
    std::atomic<std::uint32_t> no_graphics_family{UINT32_MAX};  // the transfer-only family (set by the device)
};
inline State& st() { static State* s = new State; return *s; }
inline std::uint64_t h(VkImage i) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(i)); }

// Block size of the compressed formats the runtime uses (BC, ETC2/EAC, ASTC 4x4 family); 1x1 otherwise.
inline void block_dims(VkFormat f, std::uint32_t& bw, std::uint32_t& bh) {
    bw = bh = 1;
    if ((f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK) ||
        f == VK_FORMAT_ASTC_4x4_UNORM_BLOCK || f == VK_FORMAT_ASTC_4x4_SRGB_BLOCK)
        bw = bh = 4;
}
// The aspects a format has (depth / stencil / color; multi-planar color formats accept their PLANE_n bits too).
inline VkImageAspectFlags format_aspects(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT;
    }
}
inline bool aspect_ok(const Img& i, VkImageAspectFlags a, std::string* why) {
    const VkImageAspectFlags have = format_aspects(i.format);
    if (a && !(a & ~have)) return true;
    char b[96];
    std::snprintf(b, sizeof b, "aspect 0x%x not in format aspects 0x%x", unsigned(a), unsigned(have));
    *why = b;
    return false;
}
inline void count_checked(const char* cmd) {
    const auto k = ++st().checked;
    if ((k & (k - 1)) == 0 && k >= 64) {
        std::fprintf(stderr, "[imgcheck] %llu regions checked (last %s), %llu outside, %llu dropped\n", (unsigned long long)k, cmd,
                     (unsigned long long)st().outside.load(), (unsigned long long)st().dropped.load());
        std::fflush(stderr);
    }
}
// agA28: the D3D12 COPY queue family when it has no GRAPHICS bit (d3d12_vulkan_device.cpp, set_transfer_only_family).
inline void set_no_graphics_family(std::uint32_t f) { st().no_graphics_family.store(f); }
template <class H> inline std::uint64_t hk(H v) {
    if constexpr (std::is_pointer_v<H>) return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(v));
    else return static_cast<std::uint64_t>(v);
}
inline bool on_transfer_only(VkCommandBuffer cb, std::uint32_t* family) {
    auto& s = st();
    std::lock_guard lock(s.m);
    const auto it = s.cbs.find(hk(cb));
    *family = it == s.cbs.end() ? UINT32_MAX : it->second;
    return it != s.cbs.end() && it->second == s.no_graphics_family.load();
}
inline std::string desc(const Img& i) {
    char b[200];
    std::snprintf(b, sizeof b, "type %d fmt %d %ux%ux%u mips %u layers %u samples %d flags 0x%x usage 0x%x tiling %d", int(i.type),
                  int(i.format), i.extent.width, i.extent.height, i.extent.depth, i.mips, i.layers, int(i.samples), unsigned(i.flags),
                  unsigned(i.usage), int(i.tiling));
    return b;
}
inline bool lookup(VkImage image, Img& out) {
    auto& s = st();
    std::lock_guard lock(s.m);
    const auto it = s.imgs.find(h(image));
    if (it == s.imgs.end()) return false;
    out = it->second;
    return true;
}
// True when the region lies inside the image (or the image is unknown).
inline bool region_ok(const Img& i, VkImageAspectFlags aspect, std::uint32_t mip, std::uint32_t base_layer, std::uint32_t layer_count, VkOffset3D o,
                      VkExtent3D e, std::string* why) {
    char b[160];
    if (!aspect_ok(i, aspect, why)) return false;
    if (mip >= i.mips) {
        std::snprintf(b, sizeof b, "mip %u >= %u", mip, i.mips);
        *why = b;
        return false;
    }
    const std::uint32_t layers = i.type == VK_IMAGE_TYPE_3D ? 1 : i.layers;
    if (layer_count == VK_REMAINING_ARRAY_LAYERS) layer_count = base_layer < layers ? layers - base_layer : 1;
    if (base_layer >= layers || layer_count > layers - base_layer) {
        std::snprintf(b, sizeof b, "layers %u+%u > %u", base_layer, layer_count, layers);
        *why = b;
        return false;
    }
    std::uint32_t bw, bh;
    block_dims(i.format, bw, bh);
    const auto dim = [&](std::uint32_t full, std::uint32_t block) {
        const std::uint32_t m = full >> mip ? full >> mip : 1;
        return (m + block - 1) / block * block;
    };
    const std::uint32_t mw = dim(i.extent.width, bw), mh = dim(i.extent.height, bh),
                        md = i.type == VK_IMAGE_TYPE_3D ? dim(i.extent.depth, 1) : 1;
    const auto bad = [](std::int32_t off, std::uint32_t ext, std::uint32_t lim) {
        return off < 0 || std::uint64_t(std::uint32_t(off)) + ext > lim;
    };
    if (bad(o.x, e.width, mw) || bad(o.y, e.height, mh) || bad(o.z, e.depth, md)) {
        std::snprintf(b, sizeof b, "offset (%d,%d,%d) + extent (%u,%u,%u) > mip %u size (%u,%u,%u)", o.x, o.y, o.z, e.width, e.height,
                      e.depth, mip, mw, mh, md);
        *why = b;
        return false;
    }
    return true;
}
inline void report(const char* cmd, const char* side, VkImage image, const Img& i, std::uint32_t k, std::uint32_t n,
                   const std::string& why, bool drop) {
    const auto c = ++st().outside;
    if (drop) ++st().dropped;
    if (c > 64 && (c & (c - 1)) != 0) return;
    const auto& ctx = copylog::tl_ctx;
    std::fprintf(stderr, "[imgcheck] OUTSIDE%s %s %s img 0x%llx %s | region %u/%u: %s | site %s dst_res %llu caller 0x%llx (#%llu)\n",
                 drop ? " (dropped)" : "", cmd, side, (unsigned long long)h(image), desc(i).c_str(), k, n, why.c_str(),
                 ctx.site ? ctx.site : "?", (unsigned long long)ctx.dst_res, (unsigned long long)copylog::tl_title_ra,
                 (unsigned long long)c);
    std::fflush(stderr);
}

// Vulkan forbids DEPTH / STENCIL aspects in buffer<->image copies on a queue family without GRAPHICS
// (VUID-vkCmdCopyBufferToImage-commandBuffer-07739 / CopyImageToBuffer-07746). rt3/rt4: the CE fault is at the end of a
// D32 depth image's own VA mapping.
inline void ds_check(const char* cmd, VkCommandBuffer cb, VkImage image, const Img& i, VkImageAspectFlags aspect) {
    if (!(aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) return;
    std::uint32_t family = UINT32_MAX;
    if (!on_transfer_only(cb, &family)) return;
    const auto c = ++st().ds_transfer;
    if (c > 64 && (c & (c - 1)) != 0) return;
    const auto& ctx = copylog::tl_ctx;
    std::fprintf(stderr, "[imgcheck] DS-ON-TRANSFER %s img 0x%llx %s aspect 0x%x family %u | site %s dst_res %llu caller 0x%llx (#%llu)\n", cmd,
                 (unsigned long long)h(image), desc(i).c_str(), unsigned(aspect), family, ctx.site ? ctx.site : "?",
                 (unsigned long long)ctx.dst_res, (unsigned long long)copylog::tl_title_ra, (unsigned long long)c);
    std::fflush(stderr);
}
template <class F> inline void install(F& vk) {
    if (!level()) return;
    static PFN_vkCreateImage o_create = nullptr;
    static PFN_vkDestroyImage o_destroy = nullptr;
    static PFN_vkCmdCopyBufferToImage o_b2i = nullptr;
    static PFN_vkCmdCopyImageToBuffer o_i2b = nullptr;
    static PFN_vkCmdCopyImage o_i2i = nullptr;
    static PFN_vkCmdResolveImage o_resolve = nullptr;
    static PFN_vkCmdClearColorImage o_clear = nullptr;
    static PFN_vkCmdClearDepthStencilImage o_clear_ds = nullptr;
    const bool drop = level() == 2;
    {
        static const PFN_vkCreateImage w = [](VkDevice d, const VkImageCreateInfo* ci, const VkAllocationCallbacks* a, VkImage* out) {
            const VkResult r = o_create(d, ci, a, out);
            if (r == VK_SUCCESS && out && *out && ci) {
                Img i{ci->imageType, ci->format, ci->extent, ci->mipLevels, ci->arrayLayers, ci->samples, ci->flags, ci->usage, ci->tiling};
                {
                    auto& s = st();
                    std::lock_guard lock(s.m);
                    s.imgs[h(*out)] = i;
                }
                char b[64];
                std::snprintf(b, sizeof b, "IMG 0x%llx ", (unsigned long long)h(*out));
                vareport::note(b + desc(i));
            }
            return r;
        };
        if (vk.vkCreateImage && vk.vkCreateImage != w) { o_create = vk.vkCreateImage; vk.vkCreateImage = w; }
    }
    {
        static const PFN_vkDestroyImage w = [](VkDevice d, VkImage i, const VkAllocationCallbacks* a) {
            if (i) {
                auto& s = st();
                std::lock_guard lock(s.m);
                s.imgs.erase(h(i));
            }
            o_destroy(d, i, a);
        };
        if (vk.vkDestroyImage && vk.vkDestroyImage != w) { o_destroy = vk.vkDestroyImage; vk.vkDestroyImage = w; }
    }
    static PFN_vkCreateCommandPool o_pool = nullptr;
    static PFN_vkAllocateCommandBuffers o_alloc_cb = nullptr;
    static PFN_vkAllocateMemory o_alloc_mem = nullptr;
    static PFN_vkGetImageMemoryRequirements o_req = nullptr;
    static PFN_vkBindImageMemory o_bind = nullptr;
    {
        static const PFN_vkCreateCommandPool w = [](VkDevice d, const VkCommandPoolCreateInfo* i, const VkAllocationCallbacks* a, VkCommandPool* out) {
            const VkResult r = o_pool(d, i, a, out);
            if (r == VK_SUCCESS && out && i) { std::lock_guard lock(st().m); st().pools[hk(*out)] = i->queueFamilyIndex; }
            return r;
        };
        if (vk.vkCreateCommandPool && vk.vkCreateCommandPool != w) { o_pool = vk.vkCreateCommandPool; vk.vkCreateCommandPool = w; }
    }
    {
        static const PFN_vkAllocateCommandBuffers w = [](VkDevice d, const VkCommandBufferAllocateInfo* i, VkCommandBuffer* out) {
            const VkResult r = o_alloc_cb(d, i, out);
            if (r == VK_SUCCESS && out && i) {
                std::lock_guard lock(st().m);
                const auto p = st().pools.find(hk(i->commandPool));
                if (p != st().pools.end())
                    for (std::uint32_t k = 0; k < i->commandBufferCount; ++k) st().cbs[hk(out[k])] = p->second;
            }
            return r;
        };
        if (vk.vkAllocateCommandBuffers && vk.vkAllocateCommandBuffers != w) { o_alloc_cb = vk.vkAllocateCommandBuffers; vk.vkAllocateCommandBuffers = w; }
    }
    {
        static const PFN_vkAllocateMemory w = [](VkDevice d, const VkMemoryAllocateInfo* i, const VkAllocationCallbacks* a, VkDeviceMemory* out) {
            const VkResult r = o_alloc_mem(d, i, a, out);
            if (r == VK_SUCCESS && out && *out && i) { std::lock_guard lock(st().m); st().mem[hk(*out)] = i->allocationSize; }
            return r;
        };
        if (vk.vkAllocateMemory && vk.vkAllocateMemory != w) { o_alloc_mem = vk.vkAllocateMemory; vk.vkAllocateMemory = w; }
    }
    {
        static const PFN_vkGetImageMemoryRequirements w = [](VkDevice d, VkImage i, VkMemoryRequirements* r) {
            o_req(d, i, r);
            if (r) { std::lock_guard lock(st().m); st().req[h(i)] = r->size; }
        };
        if (vk.vkGetImageMemoryRequirements && vk.vkGetImageMemoryRequirements != w) { o_req = vk.vkGetImageMemoryRequirements; vk.vkGetImageMemoryRequirements = w; }
    }
    {
        // agA28 (main-session request): flag an image bound with less room than vkGetImageMemoryRequirements().size.
        static const PFN_vkBindImageMemory w = [](VkDevice d, VkImage i, VkDeviceMemory m, VkDeviceSize off) {
            VkDeviceSize need = 0, have = 0;
            bool known_req = false, known_mem = false;
            Img img{};
            bool known_img = false;
            {
                std::lock_guard lock(st().m);
                auto r = st().req.find(h(i));
                if (r != st().req.end()) { need = r->second; known_req = true; }
                auto a = st().mem.find(hk(m));
                if (a != st().mem.end()) { have = a->second; known_mem = true; }
                auto g = st().imgs.find(h(i));
                if (g != st().imgs.end()) { img = g->second; known_img = true; }
            }
            if (!known_req) {
                VkMemoryRequirements q{};
                o_req(d, i, &q);
                need = q.size;
                known_req = true;
            }
            const auto nb = ++st().binds;
            const bool short_bind = known_mem && (off > have || need > have - off);
            if (short_bind) {
                const auto c = ++st().bind_short;
                if (c <= 64 || (c & (c - 1)) == 0) {
                    std::fprintf(stderr, "[imgcheck] BIND-SHORT img 0x%llx %s: needs 0x%llx at offset 0x%llx of a 0x%llx allocation (0x%llx short) (#%llu)\n",
                                 (unsigned long long)h(i), known_img ? desc(img).c_str() : "?", (unsigned long long)need,
                                 (unsigned long long)off, (unsigned long long)have,
                                 (unsigned long long)(off + need - have), (unsigned long long)c);
                    std::fflush(stderr);
                }
            }
            if ((nb & (nb - 1)) == 0 && nb >= 256) {
                std::fprintf(stderr, "[imgcheck] %llu image binds, %llu short\n", (unsigned long long)nb, (unsigned long long)st().bind_short.load());
                std::fflush(stderr);
            }
            char b[160];
            std::snprintf(b, sizeof b, "IMGBIND 0x%llx mem 0x%llx off 0x%llx need 0x%llx alloc 0x%llx%s", (unsigned long long)h(i),
                          (unsigned long long)hk(m), (unsigned long long)off, (unsigned long long)need, (unsigned long long)have,
                          short_bind ? " SHORT" : "");
            vareport::note(b);
            return o_bind(d, i, m, off);
        };
        if (vk.vkBindImageMemory && vk.vkBindImageMemory != w) { o_bind = vk.vkBindImageMemory; vk.vkBindImageMemory = w; }
    }
    // Filters `n` regions through `ok(k)`; calls `run(count, ptr)` with all of them, the kept ones, or none.
    {
        static const PFN_vkCmdCopyBufferToImage w = [](VkCommandBuffer cb, VkBuffer s, VkImage i, VkImageLayout l, std::uint32_t n,
                                                       const VkBufferImageCopy* r) {
            Img img;
            if (!lookup(i, img)) return o_b2i(cb, s, i, l, n, r);
            std::vector<VkBufferImageCopy> keep;
            bool any_bad = false;
            for (std::uint32_t k = 0; k < n; ++k) {
                std::string why;
                count_checked("copy");
                const auto& sr = r[k].imageSubresource;
                ds_check("buffer<->image", cb, i, img, sr.aspectMask);
                if (region_ok(img, sr.aspectMask, sr.mipLevel, sr.baseArrayLayer, sr.layerCount, r[k].imageOffset, r[k].imageExtent, &why)) {
                    keep.push_back(r[k]);
                } else {
                    any_bad = true;
                    report("vkCmdCopyBufferToImage", "dst", i, img, k, n, why, level() == 2);
                }
            }
            if (!any_bad || level() != 2) return o_b2i(cb, s, i, l, n, r);
            if (!keep.empty()) o_b2i(cb, s, i, l, std::uint32_t(keep.size()), keep.data());
        };
        if (vk.vkCmdCopyBufferToImage && vk.vkCmdCopyBufferToImage != w) { o_b2i = vk.vkCmdCopyBufferToImage; vk.vkCmdCopyBufferToImage = w; }
    }
    {
        static const PFN_vkCmdCopyImageToBuffer w = [](VkCommandBuffer cb, VkImage i, VkImageLayout l, VkBuffer d, std::uint32_t n,
                                                       const VkBufferImageCopy* r) {
            Img img;
            if (!lookup(i, img)) return o_i2b(cb, i, l, d, n, r);
            std::vector<VkBufferImageCopy> keep;
            bool any_bad = false;
            for (std::uint32_t k = 0; k < n; ++k) {
                std::string why;
                count_checked("copy");
                const auto& sr = r[k].imageSubresource;
                ds_check("buffer<->image", cb, i, img, sr.aspectMask);
                if (region_ok(img, sr.aspectMask, sr.mipLevel, sr.baseArrayLayer, sr.layerCount, r[k].imageOffset, r[k].imageExtent, &why)) {
                    keep.push_back(r[k]);
                } else {
                    any_bad = true;
                    report("vkCmdCopyImageToBuffer", "src", i, img, k, n, why, level() == 2);
                }
            }
            if (!any_bad || level() != 2) return o_i2b(cb, i, l, d, n, r);
            if (!keep.empty()) o_i2b(cb, i, l, d, std::uint32_t(keep.size()), keep.data());
        };
        if (vk.vkCmdCopyImageToBuffer && vk.vkCmdCopyImageToBuffer != w) { o_i2b = vk.vkCmdCopyImageToBuffer; vk.vkCmdCopyImageToBuffer = w; }
    }
    {
        static const PFN_vkCmdCopyImage w = [](VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, std::uint32_t n,
                                               const VkImageCopy* r) {
            Img si, di;
            const bool sk = lookup(s, si), dk = lookup(d, di);
            std::vector<VkImageCopy> keep;
            bool any_bad = false;
            for (std::uint32_t k = 0; k < n; ++k) {
                std::string why;
                count_checked("vkCmdCopyImage");
                if (dk) ds_check("vkCmdCopyImage dst", cb, d, di, r[k].dstSubresource.aspectMask);
                if (sk) ds_check("vkCmdCopyImage src", cb, s, si, r[k].srcSubresource.aspectMask);
                bool ok = true;
                if (dk && !region_ok(di, r[k].dstSubresource.aspectMask, r[k].dstSubresource.mipLevel, r[k].dstSubresource.baseArrayLayer, r[k].dstSubresource.layerCount,
                                     r[k].dstOffset, r[k].extent, &why)) {
                    report("vkCmdCopyImage", "dst", d, di, k, n, why, level() == 2);
                    ok = false;
                }
                if (sk && !region_ok(si, r[k].srcSubresource.aspectMask, r[k].srcSubresource.mipLevel, r[k].srcSubresource.baseArrayLayer, r[k].srcSubresource.layerCount,
                                     r[k].srcOffset, r[k].extent, &why)) {
                    report("vkCmdCopyImage", "src", s, si, k, n, why, level() == 2);
                    ok = false;
                }
                if (ok) keep.push_back(r[k]);
                else any_bad = true;
            }
            if (!any_bad || level() != 2) return o_i2i(cb, s, sl, d, dl, n, r);
            if (!keep.empty()) o_i2i(cb, s, sl, d, dl, std::uint32_t(keep.size()), keep.data());
        };
        if (vk.vkCmdCopyImage && vk.vkCmdCopyImage != w) { o_i2i = vk.vkCmdCopyImage; vk.vkCmdCopyImage = w; }
    }
    {
        static const PFN_vkCmdResolveImage w = [](VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl,
                                                  std::uint32_t n, const VkImageResolve* r) {
            Img di;
            if (!lookup(d, di)) return o_resolve(cb, s, sl, d, dl, n, r);
            std::vector<VkImageResolve> keep;
            bool any_bad = false;
            for (std::uint32_t k = 0; k < n; ++k) {
                std::string why;
                if (region_ok(di, r[k].dstSubresource.aspectMask, r[k].dstSubresource.mipLevel, r[k].dstSubresource.baseArrayLayer, r[k].dstSubresource.layerCount,
                              r[k].dstOffset, r[k].extent, &why)) {
                    keep.push_back(r[k]);
                } else {
                    any_bad = true;
                    report("vkCmdResolveImage", "dst", d, di, k, n, why, level() == 2);
                }
            }
            if (!any_bad || level() != 2) return o_resolve(cb, s, sl, d, dl, n, r);
            if (!keep.empty()) o_resolve(cb, s, sl, d, dl, std::uint32_t(keep.size()), keep.data());
        };
        if (vk.vkCmdResolveImage && vk.vkCmdResolveImage != w) { o_resolve = vk.vkCmdResolveImage; vk.vkCmdResolveImage = w; }
    }
    // Clears: subresource ranges only (mips / layers).
    {
        static const auto range_ok = [](const Img& i, const VkImageSubresourceRange& r, std::string* why) {
            const std::uint32_t mips = r.levelCount == VK_REMAINING_MIP_LEVELS ? (r.baseMipLevel < i.mips ? i.mips - r.baseMipLevel : 1) : r.levelCount;
            const std::uint32_t layers = i.type == VK_IMAGE_TYPE_3D ? 1 : i.layers;
            const std::uint32_t lc = r.layerCount == VK_REMAINING_ARRAY_LAYERS ? (r.baseArrayLayer < layers ? layers - r.baseArrayLayer : 1) : r.layerCount;
            if (!aspect_ok(i, r.aspectMask, why)) return false;
            if (r.baseMipLevel >= i.mips || mips > i.mips - r.baseMipLevel || r.baseArrayLayer >= layers || lc > layers - r.baseArrayLayer) {
                char b[128];
                std::snprintf(b, sizeof b, "range mips %u+%u of %u, layers %u+%u of %u", r.baseMipLevel, mips, i.mips, r.baseArrayLayer, lc, layers);
                *why = b;
                return false;
            }
            return true;
        };
        static const PFN_vkCmdClearColorImage w = [](VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue* c,
                                                     std::uint32_t n, const VkImageSubresourceRange* r) {
            Img img;
            if (!lookup(i, img)) return o_clear(cb, i, l, c, n, r);
            std::vector<VkImageSubresourceRange> keep;
            bool any_bad = false;
            for (std::uint32_t k = 0; k < n; ++k) {
                std::string why;
                if (range_ok(img, r[k], &why)) keep.push_back(r[k]);
                else { any_bad = true; report("vkCmdClearColorImage", "dst", i, img, k, n, why, level() == 2); }
            }
            if (!any_bad || level() != 2) return o_clear(cb, i, l, c, n, r);
            if (!keep.empty()) o_clear(cb, i, l, c, std::uint32_t(keep.size()), keep.data());
        };
        if (vk.vkCmdClearColorImage && vk.vkCmdClearColorImage != w) { o_clear = vk.vkCmdClearColorImage; vk.vkCmdClearColorImage = w; }
        static const PFN_vkCmdClearDepthStencilImage wd = [](VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue* c,
                                                             std::uint32_t n, const VkImageSubresourceRange* r) {
            Img img;
            if (!lookup(i, img)) return o_clear_ds(cb, i, l, c, n, r);
            std::vector<VkImageSubresourceRange> keep;
            bool any_bad = false;
            for (std::uint32_t k = 0; k < n; ++k) {
                std::string why;
                if (range_ok(img, r[k], &why)) keep.push_back(r[k]);
                else { any_bad = true; report("vkCmdClearDepthStencilImage", "dst", i, img, k, n, why, level() == 2); }
            }
            if (!any_bad || level() != 2) return o_clear_ds(cb, i, l, c, n, r);
            if (!keep.empty()) o_clear_ds(cb, i, l, c, std::uint32_t(keep.size()), keep.data());
        };
        if (vk.vkCmdClearDepthStencilImage && vk.vkCmdClearDepthStencilImage != wd) { o_clear_ds = vk.vkCmdClearDepthStencilImage; vk.vkCmdClearDepthStencilImage = wd; }
    }
    static std::atomic<int> said{0};
    if (said++ == 0) {
        std::fprintf(stderr, "[imgcheck] installed (LEONIDA_CE_IMG_CHECK=%d: %s)\n", level(), drop ? "log + drop regions outside" : "log only");
        std::fflush(stderr);
    }
}
}  // namespace leonida::ce::imgcheck
