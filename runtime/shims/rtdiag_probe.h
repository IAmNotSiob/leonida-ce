#pragma once
// RTDIAG probe (Leonida CE hack, Windows/Wine side): at a present, print the replay's per-target draw counts
// (rtdiag.h) and, with LEONIDA_CE_RTDIAG_DUMP=1, read back mip 0 of the presented image, the busiest colour targets and
// the texture named by LEONIDA_CE_RTDIAG_ADDR (a T# address, e.g. 0x100001900): logs the non-zero byte share and writes
// 4-byte-per-texel images to Z:\root\agD-dump\<frame>_<tag>.ppm.
#include "../src/rtdiag.h"

#include <windows.h>
#include <d3d12.h>

#include <string>
#include <vector>

namespace leonida::rtdiag {

inline void probe_resource(ID3D12Device* dev, ID3D12CommandQueue* queue, ID3D12Resource* texture, std::uint64_t frame,
                           const char* tag) {
    const D3D12_RESOURCE_DESC rd = texture->GetDesc();
    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        std::fprintf(stderr, "[rtdiag] probe %s: dimension %d skipped\n", tag, int(rd.Dimension));
        return;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 row_bytes = 0, total = 0;
    dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_bytes, &total);
    if (!total || total == ~0ull) {
        std::fprintf(stderr, "[rtdiag] probe %s: no footprint (fmt %d)\n", tag, int(rd.Format));
        return;
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    const D3D12_COMMAND_LIST_TYPE type = queue->GetDesc().Type;
    bool ok = SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&rb))) &&
              SUCCEEDED(dev->CreateCommandAllocator(type, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&alloc))) &&
              SUCCEEDED(dev->CreateCommandList(0, type, alloc, nullptr, __uuidof(ID3D12GraphicsCommandList),
                                               reinterpret_cast<void**>(&list))) &&
              SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence)));
    if (ok) {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = rb;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = texture;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, 1);
        for (int i = 0; i < 5000 && fence->GetCompletedValue() < 1; ++i) Sleep(1);
        void* mapped = nullptr;
        if (fence->GetCompletedValue() >= 1 && SUCCEEDED(rb->Map(0, nullptr, &mapped)) && mapped) {
            const auto* base = static_cast<const std::uint8_t*>(mapped) + fp.Offset;
            std::uint64_t nz = 0, n = 0;
            std::uint32_t first_nz = 0;
            for (UINT y = 0; y < rows; ++y) {
                const auto* px = base + std::size_t(y) * fp.Footprint.RowPitch;
                for (UINT64 x = 0; x < row_bytes; ++x) {
                    ++n;
                    if (px[x]) {
                        if (!nz) first_nz = y;
                        ++nz;
                    }
                }
            }
            const std::uint32_t* mid = reinterpret_cast<const std::uint32_t*>(
                base + std::size_t(rows / 2) * fp.Footprint.RowPitch + (row_bytes / 2 & ~15ull));
            std::fprintf(stderr, "[rtdiag] probe %s frame %llu: res %p %llux%u fmt %d flags %x: %llu of %llu bytes non-zero "
                                 "(first row %u) middle %08x %08x %08x %08x\n",
                         tag, (unsigned long long)frame, static_cast<void*>(texture), (unsigned long long)rd.Width,
                         rd.Height, int(rd.Format), unsigned(rd.Flags), (unsigned long long)nz, (unsigned long long)n,
                         first_nz, mid[0], mid[1], mid[2], mid[3]);
            if (nz && row_bytes == rd.Width * 4) {
                char dir[200] = "Z:\\root\\agD-dump";
                GetEnvironmentVariableA("LEONIDA_CE_RTDIAG_DIR", dir, sizeof(dir));
                CreateDirectoryA(dir, nullptr);
                char name[320];
                std::snprintf(name, sizeof name, "%s\\%06llu_%s_%dx%u_f%d.ppm", dir, (unsigned long long)frame,
                              tag, int(rd.Width), rd.Height, int(rd.Format));
                HANDLE h = CreateFileA(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    char header[64];
                    const int hl = std::snprintf(header, sizeof header, "P6\n%u %u\n255\n", unsigned(rd.Width), rows);
                    DWORD wr = 0;
                    WriteFile(h, header, hl, &wr, nullptr);
                    std::vector<std::uint8_t> line(static_cast<std::size_t>(rd.Width) * 3);
                    for (UINT y = 0; y < rows; ++y) {
                        const auto* px = base + std::size_t(y) * fp.Footprint.RowPitch;
                        for (UINT64 x = 0; x < rd.Width; ++x)
                            for (int c = 0; c < 3; ++c) line[x * 3 + c] = px[x * 4 + c];
                        WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &wr, nullptr);
                    }
                    CloseHandle(h);
                }
            }
            rb->Unmap(0, nullptr);
        } else {
            std::fprintf(stderr, "[rtdiag] probe %s: readback did not complete\n", tag);
        }
    }
    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (rb) rb->Release();
}

// At present `f` (queue proxy's real queue, presented plane 0 resource, the PC device).
inline void at_present(ID3D12CommandQueue* queue, ID3D12Resource* presented, std::uint64_t f) {
    static const std::uint64_t every = [] {  // agA5: LEONIDA_CE_RTDIAG_EVERY = probe + dump interval (frames)
        char v[16] = {};
        return GetEnvironmentVariableA("LEONIDA_CE_RTDIAG_EVERY", v, sizeof(v)) > 0 ? std::strtoull(v, nullptr, 10) : 0ull;
    }();
    if (!on() || f % (every ? every : 300) != 0) return;
    const std::vector<std::uintptr_t> top = dump(reinterpret_cast<std::uintptr_t>(presented), f);
    static const bool deep = [] {
        char v[8] = {};
        return GetEnvironmentVariableA("LEONIDA_CE_RTDIAG_DUMP", v, sizeof(v)) > 0 && v[0] == '1';
    }();
    if (!deep || f % (every ? every : 600) != 0) return;
    ID3D12Device* dev = nullptr;
    if (FAILED(queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&dev))) || !dev) return;
    if (ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(dev)) {
        real->AddRef();
        dev->Release();
        dev = real;
    }
    struct Rel { ID3D12Device* d; ~Rel() { d->Release(); } } rel{dev};
    if (presented) probe_resource(dev, queue, presented, f, "presented");
    char tag[32];
    for (std::size_t i = 0; i < top.size() && i < 8; ++i) {
        std::snprintf(tag, sizeof tag, "target%zu", i);
        probe_resource(dev, queue, reinterpret_cast<ID3D12Resource*>(top[i]), f, tag);
    }
    char addr[32] = {};
    if (GetEnvironmentVariableA("LEONIDA_CE_RTDIAG_ADDR", addr, sizeof(addr)) > 0) {
        const std::uint64_t a = std::strtoull(addr, nullptr, 0);
        if (const std::uintptr_t id = id_for_address(a))
            probe_resource(dev, queue, reinterpret_cast<ID3D12Resource*>(id), f, "addr");
        else
            std::fprintf(stderr, "[rtdiag] no resource has texture address %llx\n", (unsigned long long)a);
    }
}

} // namespace leonida::rtdiag
