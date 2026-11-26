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
    std::atomic<std::uint64_t> outside{0}, dropped{0}, checked{0};
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
inline bool region_ok(const Img& i, std::uint32_t mip, std::uint32_t base_layer, std::uint32_t layer_count, VkOffset3D o,
                      VkExtent3D e, std::string* why) {
    char b[160];
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
                const auto& sr = r[k].imageSubresource;
                if (region_ok(img, sr.mipLevel, sr.baseArrayLayer, sr.layerCount, r[k].imageOffset, r[k].imageExtent, &why)) {
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
                const auto& sr = r[k].imageSubresource;
                if (region_ok(img, sr.mipLevel, sr.baseArrayLayer, sr.layerCount, r[k].imageOffset, r[k].imageExtent, &why)) {
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
                bool ok = true;
                if (dk && !region_ok(di, r[k].dstSubresource.mipLevel, r[k].dstSubresource.baseArrayLayer, r[k].dstSubresource.layerCount,
                                     r[k].dstOffset, r[k].extent, &why)) {
                    report("vkCmdCopyImage", "dst", d, di, k, n, why, level() == 2);
                    ok = false;
                }
                if (sk && !region_ok(si, r[k].srcSubresource.mipLevel, r[k].srcSubresource.baseArrayLayer, r[k].srcSubresource.layerCount,
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
                if (region_ok(di, r[k].dstSubresource.mipLevel, r[k].dstSubresource.baseArrayLayer, r[k].dstSubresource.layerCount,
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
