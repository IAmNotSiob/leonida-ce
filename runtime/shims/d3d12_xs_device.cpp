#include <array>
#include <string>
#include <set>
#include <map>
#include <vector>
#include <deque>
#include <mutex>
#include <intrin.h>
// Xbox-ABI proxy around a PC ID3D12Device. See d3d12_xs_device.h for the measured slots and the limits.
//
// How forwarding works: the proxy object's first word is a vtable built once from one thunk per slot. A thunk
// receives the proxy in RCX, replaces it with the PC device pointer from offset 8, and tail-calls the PC method at
// the same index through a 15-argument integer prototype. Every PC D3D12 device method takes integer/pointer
// arguments only, so the x64 registers and stack words line up exactly; methods that return a struct keep their
// hidden return pointer in RDX, which is passed through as the first argument unchanged. The measured exceptions
// are IUnknown (0-2), slot 14 (which wraps the heap), 62 and 76, plus the heap proxy's 9/10 conversion from the
// PC hidden-pointer form to the Xbox RAX form. Slots 8/75 (command queues), 36 (fences) and 32 (a shared fence) wrap
// what they create in queue and fence proxies built the same way, and slots 31/34/35/45/46/50 hand the real objects
// behind heap, queue and fence proxies to the real device (see the command queue and fence section).
//
// A thunk declares 14 argument words, but the title passes only as many as the method takes, so the caller's frame
// may end right after the four register homes. The compiler owns a function's incoming stack-argument slots and may
// write them, a tail call included. Every function a thunk hands its words to therefore takes them in the same
// positions (slot 8: desc, riid, out as a1-a3; slot 50: a1-a5; and so on). A tail call then writes back only the
// values it read from those slots. A helper that moved a word to another stack position would overwrite the title's
// frame. xs_proxy_mock_test's mock hit this in an optimized build, and its thunks now pass their words by pointer.
//
// The public HLE Freminet/ScarlettX reports five more Xbox device slots (a third-party observation, not a Microsoft
// ABI); the generic thunk routes them before it forwards anything: 48 CreateCommandQueueX, 73 CreateCommandListX,
// 78 SetFrameIntervalX, 79 ScheduleFrameEventX and 80 WaitFrameEventX. The slots can be moved with LEONIDA_XS_*_SLOT
// for a trace that disagrees. The frame-event calls answer from a per-device token counter: there is no vblank source
// behind the device before a presenter exists, so WaitFrameEventX returns at once with a fresh non-zero token.
#include "d3d12_xs_device.h"
#include "xmem.h"
#include "shims.h"
#include "d3d12_trace.h"
#include "page_alias.h"
#include "../src/shader_library.h"
#include "../src/resource_detile.h"  // leonida::fspx_payload_from_parts for slot 67
#include "../src/xg_texture.h"
#include "rtdiag_probe.h"  // agD hack
#include "startup_trace.h"  // agA3 hang watch
#include "../src/ce_checkpoints.h"  // agA21 local_size_fn

#include <windows.h>
#include <dxgi1_4.h>
#include <tlhelp32.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <new>
#include <memory>
#include <utility>

// HACK (agA4): pipelines whose VS is a Scaleform batch shader (_VBatch*), for the replay's SF_STRIDE fix.
std::atomic<std::uint64_t> g_leonida_sf_batch_psos[16384];
std::atomic<int> g_leonida_sf_batch_n{0};
bool leonida_is_sf_batch_pso(std::uint64_t pso) {
    const int n = g_leonida_sf_batch_n.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < 16384; ++i)
        if (g_leonida_sf_batch_psos[i].load(std::memory_order_relaxed) == pso) return true;
    return false;
}

// Tests that compile this file without the XMem allocator (shims/xmem.cpp) still link: the commits before reads of
// title memory (rt::xmem::commit_physical_range) fall back to a no-op there (MSVC / lld-link weak alias).
extern "C" bool leonida_ce_xmem_commit_fallback(const void*, std::size_t) { return false; }
#pragma comment(linker, "/alternatename:?commit_physical_range@xmem@rt@@YA_NPEBX_K@Z=leonida_ce_xmem_commit_fallback")

namespace rt::d3d12_xs {
namespace {

// Whether [p, p + len) is committed, readable memory, from VirtualQuery: unlike IsBadReadPtr it raises no access
// violation (each one is a costly first-chance exception under Wine, and the trace logs every one).
bool xs_readable(const void* p, std::size_t len) {
    if (!p) return false;
    auto at = reinterpret_cast<std::uintptr_t>(p);
    const std::uintptr_t end = at + len;
    if (end < at) return false;
    // HACK (agA3, LEONIDA_CE_VQ_CACHE, default on): rd3 caught the title thread in kernel VirtualQuery from here
    // (xs_gpu_address per command-list call). Readable regions are remembered per thread for 200 ms (only positive
    // answers: a commit-on-touch view that was not committed yet is asked again).
    static const bool cache_on = [] { const char* v = std::getenv("LEONIDA_CE_VQ_CACHE"); return !v || *v != '0'; }();
    struct Entry { std::uintptr_t base, end; std::uint64_t tick; };
    thread_local Entry cache[32] = {};
    thread_local unsigned next = 0;
    const std::uint64_t now = cache_on ? GetTickCount64() : 0;
    while (at < end) {
        bool hit = false;
        if (cache_on)
            for (const Entry& e : cache)
                if (e.end && at >= e.base && at < e.end && now - e.tick < 200) { at = e.end; hit = true; break; }
        if (hit) continue;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        const DWORD prot = mbi.Protect & 0xFF;
        if ((mbi.Protect & PAGE_GUARD) || prot == PAGE_NOACCESS || prot == PAGE_EXECUTE) return false;
        at = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (cache_on) cache[next++ % 32] = {reinterpret_cast<std::uintptr_t>(mbi.BaseAddress), at, now};
    }
    return true;
}

// Leonida CE: whether the retail title is mapped (the loader puts it at its image base, 0x140000000). The title's own
// method layouts (queue slots 16-19 two ahead of the PC chain, CopyPageMappingsBatchX/PresentX/ExecuteCommandListsX at
// queue slots 31/32/35, the fence value address at fence slot 12, placed views at device slots 70/71) apply only then;
// without a title (the unit tests, other titles' callers) the proxies keep the documented PC-chain behaviour.
// LEONIDA_CE_TITLE_LAYOUT=1 or 0 forces the answer. A positive answer is kept: the title never unmaps.
bool xs_title_loaded() {
    static std::atomic<bool> seen{false};
    if (seen.load(std::memory_order_relaxed)) return true;
    static const int forced = [] {
        char v[8] = {};
        return GetEnvironmentVariableA("LEONIDA_CE_TITLE_LAYOUT", v, sizeof(v)) ? (v[0] == '1' ? 1 : 0) : -1;
    }();
    if (forced >= 0) return forced == 1;
    const void* const base = reinterpret_cast<const void*>(0x140000000ull);
    if (base == static_cast<const void*>(GetModuleHandleW(nullptr)) || !xs_readable(base, 1)) return false;
    seen.store(true, std::memory_order_relaxed);
    return true;
}

// HACK (Leonida CE boot): console formats with no PC format, after Marvisak/VIctory's resource.rs (GPLv3): 118-120
// are the console's 16-bit depth with 8-bit stencil (and its typeless views); the 24/8 formats 45-47 stand in.
DXGI_FORMAT xs_pc_format(DXGI_FORMAT f) {
    switch (static_cast<unsigned>(f)) {
    case 118: return static_cast<DXGI_FORMAT>(45);
    case 119: return static_cast<DXGI_FORMAT>(46);
    case 120: return static_cast<DXGI_FORMAT>(47);
    case 65: return DXGI_FORMAT_R8_UNORM;  // A8_UNORM: core Vulkan has none (French-General, issue #2); alpha reads 1
    default: return f;
    }
}

// HACK (Leonida CE boot): a console resource description made acceptable to PC D3D12. The console allows unordered
// access on a depth-stencil texture (the title makes depth buffers it also writes from compute, flags 0x80300006);
// PC D3D12 refuses that pair (E_INVALIDARG), and the title asserted on the null resource and then read through it.
// The UAV flag is dropped (the texture is made; only UAVs of it are missing), and console-only formats mapped.
void xs_fix_pc_flags(D3D12_RESOURCE_DESC& desc) {
    desc.Format = xs_pc_format(desc.Format);
    const auto f = static_cast<std::uint32_t>(desc.Flags);
    if ((f & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) && (f & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
        static std::atomic<int> told{0};
        if (told++ < 4)
            std::fprintf(stderr, "[hack] depth-stencil texture %llux%u with unordered access: UAV flag dropped\n",
                         static_cast<unsigned long long>(desc.Width), desc.Height);
        desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(f & ~std::uint32_t(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
    }
}



// The retail title calls device slots well past ID3D12Device14 (slot 133 at least); every slot past the PC device's
// vtable goes through the generic thunk and is refused with E_NOTIMPL, so the table must outlast the highest one.
constexpr unsigned kMaxSlots = 192;
static_assert(kMaxSlots > 133, "the proxy table must cover the slots the retail title calls (up to 133)");
static_assert(kMaxSlots > 83, "the proxy table must cover ID3D12Device14 (83 slots)");
static_assert(sizeof(void*) == 8, "the Xbox-ABI proxy is x64-only");
static_assert(sizeof(D3D12_CPU_DESCRIPTOR_HANDLE) == 8,
              "the Xbox handle ABI is an 8-byte value in RAX; the heap thunk converts the PC hidden-pointer form");
static_assert(sizeof(D3D12_RESOURCE_DESC) == 56, "the measured Xbox description prefix is 56 bytes");

struct DeviceProxy {
    void** vtable;          // offset 0: the caller's ID3D12Device* points here
    ID3D12Device* real;     // offset 8: the PC device every thunk forwards to
    void** real_vtable;
    unsigned slot_count;    // number of valid entries in real_vtable
    std::atomic<unsigned long> refs;
    // The device's IDXGIDevice1 (see "IDXGIDevice1 on the device" below): a title's IDXGIDevice1* points here.
    void** dxgi_vtable;
    DxgiAdapterProvider adapter_provider;
    std::atomic<int> gpu_thread_priority;
    std::atomic<unsigned> max_frame_latency;
    // The first-frame subset of SetFrameIntervalX/ScheduleFrameEventX/WaitFrameEventX. Tokens were never measured;
    // they start at 1 and only have to be fresh and non-zero for the boot sequence. Waiting returns at once: there is
    // no vblank source until a presenter exists, and a bounded zero wait cannot stall the first frame.
    std::atomic<std::uint32_t> frame_interval_us{0};
    std::atomic<std::uint32_t> frame_latency{1};
    std::atomic<bool> frame_event_scheduled{false};
    std::atomic<std::uint64_t> frame_next_token{1};
};

constexpr unsigned kHeapSlots = 11;  // IUnknown + ID3D12Object + DeviceChild + Pageable + GetDesc + 2 handles

struct HeapProxy {
    void** vtable;          // offset 0: the caller's ID3D12DescriptorHeap* points here
    ID3D12DescriptorHeap* real;
    DeviceProxy* device;    // keeps the Xbox-ABI device alive; GetDevice returns it
    void** real_vtable;
    std::atomic<unsigned long> refs;
};


// HACK: the console D3D runtime takes resource memory from the title's exported __XMemAllocationHooks__
// (alloc at [0]) with allocator id 128 (bits 16-23) and memory type 0x8000; the title checks its pools afterwards.
using XsTitleAlloc = void*(WINAPI*)(std::size_t, std::uint64_t);
XsTitleAlloc xs_title_alloc() {
    static XsTitleAlloc fn = [] () -> XsTitleAlloc {
        // The 1.0.2.1 executable's __XMemAllocationHooks__ export, loaded at its preferred base (make_game.py
        // checks the version).
        void* hooks = reinterpret_cast<void*>(0x146844380ull);
        if (!xs_readable(hooks, sizeof(void*))) return nullptr;
        auto f = *reinterpret_cast<XsTitleAlloc*>(hooks);
        std::fprintf(stderr, "[hack] title XMem hooks at %p, alloc %p\n", hooks, reinterpret_cast<void*>(f));
        std::fflush(stderr);
        return f;
    }();
    return fn;
}
void xs_title_d3d_alloc(std::uint64_t size) {
    if (!size || !xs_title_alloc()) return;
    void* p = xs_title_alloc()(static_cast<std::size_t>(size), (0x80ull << 16) | 0x8000ull);
    static std::atomic<int> c{0};
    if (c++ < 8) { std::fprintf(stderr, "[hack] title D3D alloc %llu -> %p\n", (unsigned long long)size, p); std::fflush(stderr); }
}

// HACK: console graphics pipelines carry native code whose first 24 bytes identify a corpus shader.
// Z:\root\leonida-psomap.txt maps "<48 hex> <stage> <name>"; misses go to Z:\root\leonida-psowant.txt.
struct XsPsoShader {
    std::string stage, name;
    std::vector<std::uint8_t> dxbc;
    std::vector<std::string> semantics;              // owns the names the elements point at
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;  // from the corpus input signature (HACK formats)
};
DXGI_FORMAT xs_format(const std::string& type, unsigned mask) {
    const unsigned n = mask >= 8 ? 4 : mask >= 4 ? 3 : mask >= 2 ? 2 : 1;
    if (type == "uint") return n == 4 ? DXGI_FORMAT_R32G32B32A32_UINT : n == 3 ? DXGI_FORMAT_R32G32B32_UINT : n == 2 ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R32_UINT;
    if (type == "int")  return n == 4 ? DXGI_FORMAT_R32G32B32A32_SINT : n == 3 ? DXGI_FORMAT_R32G32B32_SINT : n == 2 ? DXGI_FORMAT_R32G32_SINT : DXGI_FORMAT_R32_SINT;
    return n == 4 ? DXGI_FORMAT_R32G32B32A32_FLOAT : n == 3 ? DXGI_FORMAT_R32G32B32_FLOAT : n == 2 ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_R32_FLOAT;
}
std::mutex g_pso_mu;
std::mutex g_pso_names_mu;
std::map<const void*, std::string> g_pso_names;
std::map<std::string, XsPsoShader> g_pso_map;
std::set<std::string> g_pso_wanted;
bool g_pso_loaded = false;

std::string xs_hex24(const void* p) {
    static const char* d = "0123456789abcdef";
    std::string h;
    for (int i = 0; i < 24; ++i) { const auto b = static_cast<const std::uint8_t*>(p)[i]; h += d[b >> 4]; h += d[b & 15]; }
    return h;
}
std::vector<std::uint8_t> xs_read_file(const std::wstring& path) {
    std::vector<std::uint8_t> v;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return v;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    v.resize(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    if (!v.empty()) ReadFile(h, v.data(), static_cast<DWORD>(v.size()), &got, nullptr);
    CloseHandle(h);
    return v;
}
void xs_pso_load() {
    if (g_pso_loaded) return;
    g_pso_loaded = true;
    const auto txt = xs_read_file(ce_data_path(L"leonida-psomap.txt").c_str());
    std::string all(txt.begin(), txt.end());
    size_t pos = 0;
    while (pos < all.size()) {
        size_t eol = all.find('\n', pos);
        if (eol == std::string::npos) eol = all.size();
        const std::string line = all.substr(pos, eol - pos);
        pos = eol + 1;
        const size_t a = line.find(' '), b = a == std::string::npos ? a : line.find(' ', a + 1);
        if (b == std::string::npos) continue;
        XsPsoShader sh;
        sh.stage = line.substr(a + 1, b - a - 1);
        std::string rest = line.substr(b + 1);
        const size_t tab = rest.find('\t');
        sh.name = rest.substr(0, tab);
        if (tab != std::string::npos) {
            std::string sig = rest.substr(tab + 1);
            std::vector<std::array<std::string, 4>> parts;
            size_t q = 0;
            while (q < sig.size()) {
                size_t e = sig.find(';', q);
                if (e == std::string::npos) e = sig.size();
                std::string item = sig.substr(q, e - q);
                q = e + 1;
                std::array<std::string, 4> f;
                size_t k = 0;
                for (int j = 0; j < 4; ++j) {
                    size_t c = item.find(':', k);
                    f[j] = item.substr(k, c == std::string::npos ? std::string::npos : c - k);
                    k = c == std::string::npos ? item.size() : c + 1;
                }
                if (!f[0].empty()) parts.push_back(f);
            }
            sh.semantics.reserve(parts.size());
            for (auto& f : parts) sh.semantics.push_back(f[0]);
            for (size_t j = 0; j < parts.size(); ++j) {
                D3D12_INPUT_ELEMENT_DESC el{};
                el.SemanticName = sh.semantics[j].c_str();
                el.SemanticIndex = static_cast<UINT>(std::strtoul(parts[j][1].c_str(), nullptr, 10));
                el.Format = xs_format(parts[j][3], static_cast<unsigned>(std::strtoul(parts[j][2].c_str(), nullptr, 10)));
                el.InputSlot = 0;
                el.AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
                el.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                sh.elements.push_back(el);
            }
        }
        const std::wstring path = ce_data_path((L"leonida-work\\shaders\\dxbc\\" + std::wstring(sh.name.begin(), sh.name.end()) + L".dxbc").c_str());
        sh.dxbc = xs_read_file(path);
        if (!sh.dxbc.empty()) {
            XsPsoShader& slot = g_pso_map[line.substr(0, a)];
            slot = std::move(sh);
            for (size_t j = 0; j < slot.elements.size(); ++j) slot.elements[j].SemanticName = slot.semantics[j].c_str();
        }
    }
    std::fprintf(stderr, "[hack] pso map: %zu shaders\n", g_pso_map.size());
    std::fflush(stderr);
}
void xs_pso_want(const std::string& hex) {
    if (!g_pso_wanted.insert(hex).second) return;
    HANDLE h = CreateFileW(ce_data_path(L"leonida-psowant.txt").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const std::string line = hex + "\n";
    DWORD wr = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &wr, nullptr);
    CloseHandle(h);
}

// HACK: when the PC layer rejects a console resource description (SizeInBytes = UINT64_MAX), estimate it.
// HACK (Leonida CE boot): the bytes and alignment a resource takes in console memory, after Marvisak/VIctory's
// resource.rs footprint() (GPLv3). The title sizes its GPU allocations from these (its 2.2 GB GPU pool filled
// with PC sizes: 64 KiB per small texture, and a 16-bytes-a-texel guess for formats the PC layer cannot size), and
// it keeps them in its texture tables. Textures: their console layout from the texture computer, at the layout's own
// alignment unless more is asked (layout word 0 is the library's pick: Z for depth, R for render targets, else
// 4 KiB S). Buffers: their width, 64 KiB aligned. False when the layout is unknown.
bool xs_console_footprint(const D3D12_RESOURCE_DESC& d, D3D12_RESOURCE_ALLOCATION_INFO& out) {
    constexpr std::uint64_t k64 = 65536;
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (!d.Width) return false;
        const std::uint64_t align = std::max<std::uint64_t>(d.Alignment, k64);
        if (align & (align - 1)) return false;
        out.Alignment = align;
        out.SizeInBytes = (d.Width + align - 1) & ~(align - 1);
        return true;
    }
    leonida::xg::TextureDesc td;
    switch (d.Dimension) {
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D: td.dimension = leonida::xg::Dimension::Tex1d; break;
    case D3D12_RESOURCE_DIMENSION_TEXTURE2D: td.dimension = leonida::xg::Dimension::Tex2d; break;
    case D3D12_RESOURCE_DIMENSION_TEXTURE3D: td.dimension = leonida::xg::Dimension::Tex3d; break;
    default: return false;
    }
    const auto layout = static_cast<std::uint32_t>(d.Layout);
    const auto flags = static_cast<std::uint32_t>(d.Flags);
    std::uint32_t swizzle = 0;
    if (layout == 0) {
        swizzle = td.dimension == leonida::xg::Dimension::Tex1d ? leonida::xg::Swizzle::LINEAR
                : (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ? leonida::xg::Swizzle::SW_64KB_Z_X
                : (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? leonida::xg::Swizzle::SW_64KB_R_X
                                                                    : leonida::xg::Swizzle::SW_4KB_S;
    } else if (layout == 1) {
        swizzle = leonida::xg::Swizzle::LINEAR;
    } else if ((layout & ~0xFFu) == 0x100) {
        swizzle = layout & 0xFF;
    } else {
        return false;
    }
    if (d.Width > 0xFFFFFFFFull) return false;
    td.width = static_cast<std::uint32_t>(d.Width);
    td.height = d.Height;
    td.depth_or_array = d.DepthOrArraySize;
    td.mips = d.MipLevels;
    td.format = static_cast<std::uint32_t>(d.Format);
    td.samples = std::max<std::uint32_t>(d.SampleDesc.Count, 1);
    td.swizzle.value = swizzle;
    leonida::xg::Texture t;
    if (leonida::xg::Texture::make(td, t) != leonida::xg::Error::Ok || !t.size) return false;
    const std::uint64_t align = std::max<std::uint64_t>(d.Alignment, t.alignment);
    if (!align || (align & (align - 1))) return false;
    out.Alignment = align;
    // DIAGNOSTIC (Leonida CE boot): LEONIDA_CE_PAD=<bytes> adds that much to every texture footprint, to test whether
    // the overrun past a texture's end behind the 120-140 s stop comes from these sizes.
    static const std::uint64_t pad = [] {
        char v[24] = {};
        return GetEnvironmentVariableA("LEONIDA_CE_PAD", v, sizeof(v)) ? std::strtoull(v, nullptr, 0) : 0ull;
    }();
    out.SizeInBytes = (t.size + pad + align - 1) & ~(align - 1);
    return true;
}

D3D12_RESOURCE_ALLOCATION_INFO xs_alloc_info(ID3D12Device* real, UINT mask, const D3D12_RESOURCE_DESC& console) {
    {
        D3D12_RESOURCE_ALLOCATION_INFO footprint{};
        if (xs_console_footprint(console, footprint)) return footprint;
    }
    D3D12_RESOURCE_DESC d = console;
    xs_fix_pc_flags(d);
    D3D12_RESOURCE_ALLOCATION_INFO info = real->GetResourceAllocationInfo(mask, 1, &d);
    if (info.SizeInBytes != UINT64_MAX && info.SizeInBytes < (1ull << 32)) return info;
    UINT64 bytes = d.Width;
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        const UINT64 layers = d.DepthOrArraySize ? d.DepthOrArraySize : 1;
        bytes = d.Width * (d.Height ? d.Height : 1) * layers * 16;  // up to 16 bytes per texel
        if (d.MipLevels != 1) bytes = bytes * 4 / 3;
    }
    info.Alignment = 65536;
    info.SizeInBytes = (bytes + 65535) & ~65535ull;
    static std::atomic<int> n{0};
    if (n++ < 6) {
        std::fprintf(stderr, "[hack] alloc info rejected by PC (dim=%d %llux%u x%u mips=%u fmt=%d flags=0x%x layout=%d) -> estimate %llu\n",
                     (int)d.Dimension, (unsigned long long)d.Width, d.Height, d.DepthOrArraySize, d.MipLevels, (int)d.Format,
                     (unsigned)d.Flags, (int)d.Layout, (unsigned long long)info.SizeInBytes);
        std::fflush(stderr);
    }
    return info;
}

// HACK: a placeholder object for console calls whose result type is unknown (device slot 74): QueryInterface
// answers itself, AddRef/Release count, every other slot logs once and returns 0.
struct XsProbe { void** vtable; std::atomic<long> refs{1}; };
constexpr unsigned kXsProbeSlots = 64;
template <unsigned Slot>
std::uint64_t WINAPI xs_probe_slot(XsProbe* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                   std::uint64_t a4) {
    if constexpr (Slot == 0) {
        if (a2) *reinterpret_cast<void**>(a2) = self;
        self->refs++;
        return 0;
    } else if constexpr (Slot == 1) {
        return static_cast<std::uint64_t>(++self->refs);
    } else if constexpr (Slot == 2) {
        return static_cast<std::uint64_t>(--self->refs);
    } else {
        static std::atomic<bool> seen{false};
        if (!seen.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr, "[hack] slot-74 object slot %u: a1=%llx a2=%llx a3=%llx a4=%llx\n", Slot,
                         (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3, (unsigned long long)a4);
            std::fflush(stderr);
        }
        return 0;
    }
}
template <std::size_t... I>
void xs_probe_fill(void** v, std::index_sequence<I...>) {
    ((v[I] = reinterpret_cast<void*>(&xs_probe_slot<static_cast<unsigned>(I)>)), ...);
}
void* xs_make_probe() {
    static void** vtable = [] {
        static void* storage[kXsProbeSlots];
        xs_probe_fill(storage, std::make_index_sequence<kXsProbeSlots>{});
        return storage;
    }();
    auto* p = new XsProbe;
    p->vtable = vtable;
    return p;
}

// HACK: console fence slot 12 returns a pointer to the fence's completed value, which the title then polls in
// memory (the console GPU writes it). Each fence asked for gets a UINT64 here, kept current from GetCompletedValue
// by one thread (about every millisecond). Entries are never freed.
struct XsFenceMirror { ID3D12Fence* fence; volatile std::uint64_t value; std::uint64_t written = 0; };
std::mutex g_fence_mirror_mu;
std::map<const void*, XsFenceMirror*> g_fence_mirrors;

// HACK (console list slots from Marvisak/VIctory, GPLv3): this title's list layout is two slots later than the
// 2020-era one from slot 59 on. List 71/72 write a 32/64-bit value (flag 1: the GPU timestamp, 2: the system
// timestamp) when the list has run; list 148 hands out memory the list owns until its next reset.
// magic code, don't wanna deal with hours of plumbing with what these conjured devils of the souls of gpt 6 astra did.. jeez
struct XsDeferredWrite { std::uint64_t address, value; unsigned bytes, what; };  // what: 0 value, 1 GPU ts, 2 system ts
std::atomic<IUnknown*> g_placeholder_pso{nullptr};  // slot 57 stand-in (see the HACK there)
struct XsListExtras { std::vector<XsDeferredWrite> writes; std::vector<std::unique_ptr<std::uint8_t[]>> uploads; };
std::mutex g_list_extras_mu;
std::mutex g_list_pso_mu;                          // agA21: last SetPipelineState per console list proxy
std::map<const void*, std::uint64_t> g_list_pso;
std::map<const void*, XsListExtras> g_list_extras;  // keyed by the console list proxy
struct XsPendingWrites { ID3D12Fence* fence; std::uint64_t value; std::vector<XsDeferredWrite> writes; };
std::vector<XsPendingWrites> g_pending_writes;      // under g_fence_mirror_mu

void xs_do_write(const XsDeferredWrite& w) {
    std::uint64_t v = w.value;
    if (w.what == 0 && v == ~std::uint64_t{0}) {  // HACK (agA11) diagnostic
        static std::atomic<int> told{0};
        if (told++ < 8) std::fprintf(stderr, "[hack] deferred list write of UINT64_MAX to 0x%llx\n", (unsigned long long)w.address);
    }
    if (w.what != 0) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        v = static_cast<std::uint64_t>(now.QuadPart);
    }
    if (IsBadWritePtr(reinterpret_cast<void*>(w.address), w.bytes)) return;
    if (w.bytes == 4) *reinterpret_cast<volatile std::uint32_t*>(w.address) = static_cast<std::uint32_t>(v);
    else *reinterpret_cast<volatile std::uint64_t*>(w.address) = v;
}

DWORD WINAPI xs_fence_mirror_thread(void*) {
    for (;;) {
        {
            std::lock_guard lock(g_fence_mirror_mu);
            // HACK (agA12): LEONIDA_CE_FENCE_NO_MAX=1 never publishes UINT64_MAX in a mirror (keeps the last value);
            // the title's 0x144fb0ad0 would Quitf on it. Foreign writes to a mirror word are logged.
            static const bool no_max = [] { const char* v = std::getenv("LEONIDA_CE_FENCE_NO_MAX"); return v && *v == '1'; }();
            for (auto& [key, m] : g_fence_mirrors) {
                std::uint64_t before = m->value, now = m->fence->GetCompletedValue();
                if (before != m->written) {
                    static std::atomic<int> told{0};
                    if (told++ < 16) {
                        std::fprintf(stderr, "[hack] fence mirror %p (fence %p) FOREIGN write: 0x%llx (we wrote 0x%llx)\n",
                                     (void*)&m->value, (void*)m->fence, (unsigned long long)before,
                                     (unsigned long long)m->written);
                        std::fflush(stderr);
                    }
                    if (no_max && before == ~std::uint64_t{0}) before = m->written;
                }
                if (no_max && now == ~std::uint64_t{0}) {
                    static std::atomic<int> told2{0};
                    if (told2++ < 4) {
                        std::fprintf(stderr, "[hack] FENCE_NO_MAX: fence %p completed UINT64_MAX suppressed (kept 0x%llx)\n",
                                     (void*)m->fence, (unsigned long long)m->written);
                        std::fflush(stderr);
                    }
                    now = m->written;
                }
                // HACK (agA11): the title's 0x144fb0ad0 treats UINT64_MAX as device removed (Quitf 0x142474500).
                if (now == ~std::uint64_t{0} && before != now) {
                    static std::atomic<int> told{0};
                    if (told++ < 8) {
                        std::fprintf(stderr, "[hack] fence mirror %p (fence %p) went 0x%llx -> UINT64_MAX\n",
                                     (void*)&m->value, (void*)m->fence, (unsigned long long)before);
                        std::fflush(stderr);
                    }
                }
                m->value = now;
                m->written = now;
            }
            for (auto it = g_pending_writes.begin(); it != g_pending_writes.end();) {
                if (it->fence->GetCompletedValue() >= it->value) {
                    for (const auto& w : it->writes) xs_do_write(w);
                    it = g_pending_writes.erase(it);
                } else {
                    ++it;
                }
            }
        }
        Sleep(1);
    }
}
void xs_start_mirror_thread() {
    static const bool started = [] {
        CloseHandle(CreateThread(nullptr, 0, &xs_fence_mirror_thread, nullptr, 0, nullptr));
        return true;
    }();
    (void)started;
}

// After the queue has taken `lists` (console list proxies): their bottom-of-pipe writes happen once the work is done.
void xs_schedule_list_writes(ID3D12CommandQueue* queue, UINT count, const std::uint64_t* lists) {
    std::vector<XsDeferredWrite> writes;
    {
        std::lock_guard lock(g_list_extras_mu);
        for (UINT i = 0; i < count; ++i) {
            auto it = g_list_extras.find(reinterpret_cast<const void*>(lists[i]));
            if (it == g_list_extras.end()) continue;
            writes.insert(writes.end(), it->second.writes.begin(), it->second.writes.end());
        }
    }
    if (writes.empty()) return;
    static std::mutex fences_mu;
    static std::map<ID3D12CommandQueue*, std::pair<ID3D12Fence*, std::uint64_t>> fences;
    ID3D12Fence* fence = nullptr;
    std::uint64_t value = 0;
    {
        std::lock_guard lock(fences_mu);
        auto& slot = fences[queue];
        if (!slot.first) {
            ID3D12Device* device = nullptr;
            if (FAILED(queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device) return;
            // The queue answers with the device proxy, whose fences are proxies the real queue cannot signal.
            ID3D12Device* real = device_proxy_real_device(device);
            (real ? real : device)->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&slot.first));
            device->Release();
            if (!slot.first) return;
        }
        fence = slot.first;
        value = ++slot.second;
    }
    queue->Signal(fence, value);
    std::lock_guard lock(g_fence_mirror_mu);
    g_pending_writes.push_back({fence, value, std::move(writes)});
    xs_start_mirror_thread();
}
volatile std::uint64_t* xs_fence_value_address(const void* proxy, ID3D12Fence* real) {
    std::lock_guard lock(g_fence_mirror_mu);
    auto it = g_fence_mirrors.find(proxy);
    if (it != g_fence_mirrors.end()) return &it->second->value;
    auto* m = new XsFenceMirror{real, real->GetCompletedValue()};
    m->written = m->value;
    real->AddRef();  // HACK (agA12): the mirror thread polls this fence forever; keep it alive past the title's Release
    g_fence_mirrors.emplace(proxy, m);
    xs_start_mirror_thread();
    std::fprintf(stderr, "[hack] fence %p: completed-value address %p (value %llu)\n", proxy, (void*)&m->value,
                 (unsigned long long)m->value);
    std::fflush(stderr);
    return &m->value;
}

// HACK (model from Marvisak/VIctory, GPLv3: a heap's CPU and GPU start are one address, as on the console):
// the title writes descriptors through GPU handles, so the heap proxy hands out the CPU start for both, and list
// calls that take GPU descriptor handles get the real heap's GPU handle back. Without this, an ordinary heap's GPU
// start (an arena offset) became a descriptor destination of a few hundred bytes.
// magic code, don't wanna deal with hours of plumbing with what these conjured devils of the souls of gpt 6 astra did.. jeez
struct XsHeapRange { std::uint64_t cpu, size, gpu; };
std::mutex g_heap_ranges_mu;
std::vector<XsHeapRange> g_heap_ranges;
std::atomic<int> g_heap_range_count{0};
void xs_register_heap_range(std::uint64_t cpu, std::uint64_t size, std::uint64_t gpu) {
    std::lock_guard lock(g_heap_ranges_mu);
    g_heap_ranges.push_back({cpu, size, gpu});
    g_heap_range_count.store(static_cast<int>(g_heap_ranges.size()), std::memory_order_release);
}
static std::atomic<std::uint64_t> g_mirror_lo{0}, g_mirror_hi{0};  // agA20
static std::atomic<ArenaWantHook> g_arena_want_hook{nullptr};  // agA20
std::uint64_t xs_gpu_handle_from_console(std::uint64_t v) {
    if (g_heap_range_count.load(std::memory_order_acquire) == 0) return v;
    std::lock_guard lock(g_heap_ranges_mu);
    for (const auto& r : g_heap_ranges)
        if (v >= r.cpu && v < r.cpu + r.size) return v - r.cpu + r.gpu;
    return v;
}

// HACK: write presented frames to Z:\root\frames\frame_<n>.ppm (no window yet). Plane 0's texture is copied to a
// readback buffer on the presenting queue and waited for; 8-bit RGBA/BGRA formats only.
// HACK (Leonida CE): the title renders on the passed-through GPU while the desktop runs on another adapter (a VM),
// so its swapchain never reaches the screen and the window stays white. LEONIDA_CE_PREVIEW=N reads the presented
// image back every N presents and paints it into the title's window with GDI (StretchDIBits), so it can be seen.
HWND xs_preview_window() {
    static HWND cached = nullptr;
    if (cached && IsWindow(cached)) return cached;
    struct Find { DWORD pid; HWND best; LONG area; } find{GetCurrentProcessId(), nullptr, 0};
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        auto* f = reinterpret_cast<Find*>(l);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != f->pid || !IsWindowVisible(h)) return TRUE;
        RECT r{};
        GetClientRect(h, &r);
        const LONG area = (r.right - r.left) * (r.bottom - r.top);
        if (area > f->area) { f->area = area; f->best = h; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&find));
    cached = find.best;
    return cached;
}

void xs_paint_preview(const std::uint8_t* pixels, std::uint64_t width, UINT rows, UINT64 row_pitch, bool bgra) {
    HWND wnd = xs_preview_window();
    if (!wnd) return;
    // GDI wants BGRA rows of width*4 bytes; scale down by 2 first (a 4K frame is 33 MB) and swap channels if needed.
    const std::uint64_t w = width / 2, h = rows / 2;
    static std::vector<std::uint8_t> out;
    out.resize(static_cast<std::size_t>(w * h * 4));
    for (std::uint64_t y = 0; y < h; ++y) {
        const std::uint8_t* src = pixels + (y * 2) * row_pitch;
        std::uint8_t* dst = out.data() + y * w * 4;
        for (std::uint64_t x = 0; x < w; ++x) {
            const std::uint8_t* px = src + x * 8;
            dst[x * 4 + 0] = px[bgra ? 0 : 2];
            dst[x * 4 + 1] = px[1];
            dst[x * 4 + 2] = px[bgra ? 2 : 0];
            dst[x * 4 + 3] = 255;
        }
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = static_cast<LONG>(w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(h);  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    RECT r{};
    GetClientRect(wnd, &r);
    if (HDC dc = GetDC(wnd)) {
        SetStretchBltMode(dc, HALFTONE);
        StretchDIBits(dc, 0, 0, r.right - r.left, r.bottom - r.top, 0, 0, static_cast<int>(w), static_cast<int>(h),
                      out.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(wnd, dc);
    }
}

// HACK (agA3, LEONIDA_CE_HANG_WATCH, default on; =0 off): rc5b/rc7b hung right after state 3 with no log. A watchdog
// thread: when no present has happened for 15 s, it prints every thread's stack ([hang] lines), again at 45 s and 120 s.
std::atomic<std::uint64_t> g_hang_last_tick{0}, g_hang_last_frame{0};
HANDLE g_hang_file = INVALID_HANDLE_VALUE;
void hang_write(const char* text, std::size_t n) {
    DWORD wr = 0;
    if (g_hang_file != INVALID_HANDLE_VALUE) WriteFile(g_hang_file, text, static_cast<DWORD>(n), &wr, nullptr);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);  // straight to the handle: no CRT lock
    if (err && err != INVALID_HANDLE_VALUE) WriteFile(err, text, static_cast<DWORD>(n), &wr, nullptr);
}
void hang_dump_threads(std::uint64_t idle_ms) {
    if (g_hang_file == INVALID_HANDLE_VALUE) {
        char path[260] = "C:\\zroot\\logs\\hang.txt";
        if (GetEnvironmentVariableA("LEONIDA_CE_HANG_FILE", path, sizeof path) == 0 &&
            GetFileAttributesA("C:\\zroot\\logs") == INVALID_FILE_ATTRIBUTES)
            std::snprintf(path, sizeof path, "hang.txt");
        g_hang_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    }
    char line[4096];
    int n = std::snprintf(line, sizeof line, "[hang] no present for %llu ms (last frame %llu); thread stacks:\n",
                          (unsigned long long)idle_ms, (unsigned long long)g_hang_last_frame.load());
    hang_write(line, n);
    if (auto wd = ::leonida::ce::wait_dump_fn().load()) wd();  // agA21: queue waits / fence signals
    if (auto qd = ::leonida::ce::queue_dump_fn().load()) qd();  // agA22: Vulkan timeline semaphores / submissions
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!t) continue;
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        bool got = false;
        if (SuspendThread(t) != DWORD(-1)) {
            got = GetThreadContext(t, &c) != 0;
            ResumeThread(t);
        }
        CloseHandle(t);
        if (!got) continue;
        // Raw frames first (no allocation): rip + a scan of the top of the stack for return-looking values.
        n = std::snprintf(line, sizeof line, "[hang]   tid %lu rip %s\n", te.th32ThreadID,
                          ::rt::startup_trace::where(c.Rip).c_str());
        hang_write(line, n);
        const std::string st = ::rt::startup_trace::detail::stack_from(c);
        n = std::snprintf(line, sizeof line, "[hang]   tid %lu: %s\n", te.th32ThreadID, st.c_str());
        hang_write(line, n);
    }
    CloseHandle(snap);
}
void hang_heartbeat(std::uint64_t frame) {
    static const bool on = [] { const char* v = std::getenv("LEONIDA_CE_HANG_WATCH"); return !v || *v != '0'; }();
    if (!on) return;
    g_hang_last_tick.store(GetTickCount64());
    g_hang_last_frame.store(frame);
    if (frame % 60 == 0 && frame >= 1080) {  // agA3: pace lines (WriteFile, no CRT lock)
        static std::uint64_t prev = 0;
        const std::uint64_t now = GetTickCount64();
        char line[96];
        const int n = std::snprintf(line, sizeof line, "[pace] frame %llu, %llu ms for the last 60\n",
                                    (unsigned long long)frame, (unsigned long long)(prev ? now - prev : 0));
        prev = now;
        DWORD wr = 0;
        HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        if (err && err != INVALID_HANDLE_VALUE) WriteFile(err, line, static_cast<DWORD>(n), &wr, nullptr);
    }
    static std::once_flag once;
    std::call_once(once, [] {
        HANDLE h = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            std::uint64_t reported_for = 0;
            int stage = 0;
            for (;;) {
                Sleep(1000);
                const std::uint64_t last = g_hang_last_tick.load();
                const std::uint64_t idle = GetTickCount64() - last;
                if (last != reported_for) { reported_for = last; stage = 0; }
                static const std::uint64_t marks[] = {15000, 30000, 60000, 90000, 150000};
                if (stage < 5 && idle >= marks[stage]) { ++stage; hang_dump_threads(idle); }
            }
        }, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    });
}

void xs_capture_frame(ID3D12CommandQueue* queue, ID3D12Resource* texture, std::uint64_t frame, bool preview_only = false) {
    if (!queue || !texture) return;
    const D3D12_RESOURCE_DESC rd = texture->GetDesc();
    const bool bgra = rd.Format == DXGI_FORMAT_B8G8R8A8_UNORM || rd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                      rd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    const bool rgba = rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM || rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                      rd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || (!bgra && !rgba)) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) std::fprintf(stderr, "[hack] capture: format %d not captured\n", (int)rd.Format);
        return;
    }
    ID3D12Device* dev = nullptr;
    if (FAILED(queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&dev))) || !dev) return;
    if (ID3D12Device* real = device_proxy_real_device(dev)) {  // the queue answers with the proxy: use the PC device
        real->AddRef();
        dev->Release();
        dev = real;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 row_bytes = 0, total = 0;
    dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_bytes, &total);
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
        if (preview_only && fence->GetCompletedValue() >= 1 && SUCCEEDED(rb->Map(0, nullptr, &mapped)) && mapped) {
            xs_paint_preview(static_cast<const std::uint8_t*>(mapped) + fp.Offset, rd.Width, rows, fp.Footprint.RowPitch,
                             bgra);
            rb->Unmap(0, nullptr);
            mapped = nullptr;
        } else if (!preview_only && fence->GetCompletedValue() >= 1 && SUCCEEDED(rb->Map(0, nullptr, &mapped)) &&
                   mapped) {
            CreateDirectoryW(ce_data_path(L"frames").c_str(), nullptr);
            wchar_t name[96];
            swprintf_s(name, L"%s\\frame_%06llu.ppm", ce_data_path(L"frames").c_str(), (unsigned long long)frame);
            HANDLE h = CreateFileW(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                char header[64];
                const int hl = std::snprintf(header, sizeof header, "P6\n%u %u\n255\n", (unsigned)rd.Width, rows);
                DWORD wr = 0;
                WriteFile(h, header, hl, &wr, nullptr);
                std::vector<std::uint8_t> line(static_cast<std::size_t>(rd.Width) * 3);
                for (UINT y = 0; y < rows; ++y) {
                    const auto* px = static_cast<const std::uint8_t*>(mapped) + fp.Offset + std::size_t(y) * fp.Footprint.RowPitch;
                    for (UINT64 x = 0; x < rd.Width; ++x) {
                        line[x * 3 + 0] = px[x * 4 + (bgra ? 2 : 0)];
                        line[x * 3 + 1] = px[x * 4 + 1];
                        line[x * 3 + 2] = px[x * 4 + (bgra ? 0 : 2)];
                    }
                    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &wr, nullptr);
                }
                CloseHandle(h);
                std::fprintf(stderr, "[hack] captured frame %llu (%llux%u fmt %d)\n", (unsigned long long)frame,
                             (unsigned long long)rd.Width, rows, (int)rd.Format);
                std::fflush(stderr);
            }
            rb->Unmap(0, nullptr);
        }
    }
    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (rb) rb->Release();
    dev->Release();
}

// HACK (Leonida CE boot): see set_console_arena.
std::uint8_t* g_arena_mapped = nullptr;
std::uint64_t g_arena_size = 0, g_ring_cpu = 0, g_ring_va = 0, g_ring_size = 0;
std::atomic<std::uint64_t> g_ring_cursor{0};
std::uint64_t xs_va_from_cpu(std::uint64_t v) {
    if (g_ring_cpu && v >= g_ring_cpu && v < g_ring_cpu + g_ring_size) return v - g_ring_cpu + g_ring_va;
    const auto base = reinterpret_cast<std::uint64_t>(g_arena_mapped);
    if (g_arena_mapped && v >= base && v < base + g_arena_size) return v - base;
    return v;
}
// Console upload memory: CPU pointer into the ring (wraps; the title reuses it within a few frames).
std::uint64_t xs_upload_alloc(std::uint64_t size, std::uint64_t align) {
    if (!g_ring_cpu || !g_ring_size || size > g_ring_size / 4) return 0;
    for (;;) {
        std::uint64_t cur = g_ring_cursor.load(std::memory_order_relaxed);
        std::uint64_t at = (cur + align - 1) & ~(align - 1);
        std::uint64_t next = at + size;
        if (next > g_ring_size) { at = 0; next = size; }
        if (g_ring_cursor.compare_exchange_weak(cur, next, std::memory_order_relaxed))
            return g_ring_cpu + at;
    }
}

// HACK (Leonida CE boot): a GPU address in the title's own memory becomes a snapshot in the arena (the title wrote
// the data before binding it); addresses already in the arena become offsets. VIctory copies changed title memory.
// HACK (agA2, LEONIDA_CE_SNAP_RESYNC): which title address each ring snapshot was copied from, so the replay can
// copy the title bytes again when it records the draw (the title may write the data after binding it).
std::mutex g_snap_mu;
std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> g_snaps;  // ring VA -> (title address, bytes)
void xs_note_snapshot(std::uint64_t va, std::uint64_t title, std::uint64_t size) {
    std::lock_guard l(g_snap_mu);
    auto it = g_snaps.lower_bound(va);
    if (it != g_snaps.begin()) { auto pv = std::prev(it); if (pv->first + pv->second.second > va) it = pv; }
    while (it != g_snaps.end() && it->first < va + size) it = g_snaps.erase(it);
    g_snaps[va] = {title, size};
}
std::uint64_t xs_gpu_address(std::uint64_t address, std::uint64_t size) {
    const std::uint64_t translated = xs_va_from_cpu(address);
    if (translated != address || !address || !size || size > (8u << 20)) return translated;
    if (!xs_readable(reinterpret_cast<const void*>(address), static_cast<UINT_PTR>(size))) return translated;
    const std::uint64_t copy = xs_upload_alloc(size, 256);
    if (!copy) return translated;
    std::memcpy(reinterpret_cast<void*>(copy), reinterpret_cast<const void*>(address), static_cast<std::size_t>(size));
    xs_note_snapshot(xs_va_from_cpu(copy), address, size);  // agA2
    return xs_va_from_cpu(copy);
}

// HACK (fspx CB analysis, LEONIDA_CE_CBV_SHADOW=1 on / =2 on + change log; default OFF = old ring snapshot, agA23): constant-buffer views on title
// memory (device slot 17) used to be a ONE-TIME copy into the 64 MiB console upload ring (xs_upload_alloc). That
// copy is (a) never refreshed (the title fills or updates its constants after CreateConstantBufferView, as
// SNAP_RESYNC measured for vertex data: "snapshot was stale"), and (b) overwritten when the ring wraps
// (xs_upload_alloc has no lifetime tracking; ring snapshots of vertex/index data, ROOT_SPILL arrays and RECT_VS
// vertices wrap it every few hundred frames, and SNAP_RESYNC writes stale vertex snapshots over reused ring
// ranges). A descriptor that lives longer than one ring wrap then hands the shader unrelated bytes: the fspx blur
// radius (cs_d5613de2 / cs_d94af257: s_buffer_load CB+0x43c, V# at table s4+0xca0) becomes garbage and the
// -r..+r loop never ends; cs_edc5d4c7 reads a heap index from CB+0x40 and loads a garbage V# with it.
// Instead: one stable arena shadow per (title address, size), allocated from a DEFAULT buffer in the arena
// (host-coherent, so the CPU writes it through g_arena_mapped), copied at creation and re-copied (only what
// changed, compared against a host copy) before every ExecuteCommandLists. Shadows are never freed or moved, so a
// descriptor stays valid for the life of the process; when the pool is full the old ring snapshot is used (logged).
int xs_cbv_shadow_mode() {
    static const int mode = [] { const char* v = std::getenv("LEONIDA_CE_CBV_SHADOW"); return v ? std::atoi(v) : 0; }();
    return mode;
}
struct XsCbvShadow {
    std::uint64_t title = 0;
    std::uint32_t size = 0;
    std::uint64_t va = 0;                // arena VA the V# names
    std::vector<std::uint8_t> last;      // host copy of what the shadow holds (compare without reading the arena)
    bool told_change = false;
};
std::mutex g_cbv_mu;
std::map<std::pair<std::uint64_t, std::uint32_t>, std::size_t> g_cbv_index;  // (title, size) -> g_cbv slot
std::deque<XsCbvShadow> g_cbv;
ID3D12Resource* g_cbv_pool = nullptr;
std::uint64_t g_cbv_pool_va = 0, g_cbv_pool_size = 0, g_cbv_pool_used = 0;
bool g_cbv_pool_failed = false;
std::atomic<std::uint64_t> g_cbv_syncs{0}, g_cbv_changed_bytes{0}, g_cbv_changed_views{0};

// Under g_cbv_mu. A DEFAULT buffer lands in the arena (VA = arena offset), so g_arena_mapped + VA is its CPU view.
bool xs_cbv_pool_ready(ID3D12Device* dev) {
    if (g_cbv_pool) return true;
    if (g_cbv_pool_failed || !dev || !g_arena_mapped) return false;
    std::uint64_t mib = 64;
    if (const char* v = std::getenv("LEONIDA_CE_CBV_SHADOW_MB")) mib = std::strtoull(v, nullptr, 0);
    if (mib < 1) mib = 1;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = mib << 20;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    const HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    __uuidof(ID3D12Resource), reinterpret_cast<void**>(&r));
    const std::uint64_t va = SUCCEEDED(hr) && r ? r->GetGPUVirtualAddress() : 0;
    if (!va || va + d.Width > g_arena_size) {  // must be arena memory the CPU can reach
        if (r) r->Release();
        g_cbv_pool_failed = true;
        std::fprintf(stderr, "[hack] CBV_SHADOW: no arena pool (hr 0x%08lx va 0x%llx); CBVs keep the ring snapshot\n",
                     (unsigned long)hr, (unsigned long long)va);
        std::fflush(stderr);
        return false;
    }
    g_cbv_pool = r;  // held for the life of the process
    g_cbv_pool_va = va;
    g_cbv_pool_size = d.Width;
    std::fprintf(stderr, "[hack] CBV_SHADOW: pool %llu MiB at arena VA 0x%llx\n", (unsigned long long)mib,
                 (unsigned long long)va);
    std::fflush(stderr);
    return true;
}

// The arena VA a CBV on title memory [title, title + size) should name, or 0 (caller falls back to the ring).
std::uint64_t xs_cbv_shadow(ID3D12Device* dev, std::uint64_t title, std::uint32_t size) {
    if (xs_cbv_shadow_mode() <= 0 || !title || !size) return 0;
    std::lock_guard lock(g_cbv_mu);
    if (!xs_cbv_pool_ready(dev)) return 0;
    XsCbvShadow* e = nullptr;
    auto it = g_cbv_index.find({title, size});
    if (it != g_cbv_index.end()) {
        e = &g_cbv[it->second];
    } else {
        const std::uint64_t at = (g_cbv_pool_used + 255) & ~255ull;
        if (at + size > g_cbv_pool_size) {
            static std::atomic<int> told{0};
            if (told++ < 4) {
                std::fprintf(stderr, "[hack] CBV_SHADOW: pool full (%zu views); CBV at 0x%llx (+0x%x) falls back to the ring\n",
                             g_cbv.size(), (unsigned long long)title, size);
                std::fflush(stderr);
            }
            return 0;
        }
        g_cbv_pool_used = at + size;
        g_cbv_index[{title, size}] = g_cbv.size();
        g_cbv.push_back({title, size, g_cbv_pool_va + at, std::vector<std::uint8_t>(size), false});
        e = &g_cbv.back();
    }
    // Copy now as well: the first use may come before the next ExecuteCommandLists refresh.
    std::memcpy(e->last.data(), reinterpret_cast<const void*>(title), size);
    std::memcpy(g_arena_mapped + e->va, e->last.data(), size);
    return e->va;
}

// agA23 LEONIDA_CE_CBV_SHADOW_ROOT=<bytes> (needs CBV_SHADOW >= 1 and a pool made by a slot-17 CBV first): a root CBV
// (pc 37/38) at a title address the arena does not know was passed raw (the GPU read whatever sits at that VA). Shadow
// <bytes> of it (halved down to 256 while not readable; a root CBV has no size). Root SRV/UAV stay raw (UAV writes
// would not reach the title).
std::uint64_t xs_cbv_shadow_root(std::uint64_t title) {
    static const std::uint32_t want = [] {
        const char* v = std::getenv("LEONIDA_CE_CBV_SHADOW_ROOT");
        return v ? static_cast<std::uint32_t>(std::strtoul(v, nullptr, 0)) : 0u;
    }();
    if (!want || xs_cbv_shadow_mode() <= 0) return 0;
    std::uint32_t n = want;
    while (n >= 256 && !xs_readable(reinterpret_cast<const void*>(title), n)) n >>= 1;
    if (n < 256) return 0;
    return xs_cbv_shadow(nullptr, title, n);
}

// Before every ExecuteCommandLists: bring each shadow up to date with the title bytes (only changed ones written).
void xs_cbv_shadow_refresh() {
    if (xs_cbv_shadow_mode() <= 0) return;
    std::lock_guard lock(g_cbv_mu);
    if (!g_cbv_pool) return;
    std::uint64_t bytes = 0, views = 0;
    for (XsCbvShadow& e : g_cbv) {
        const auto* src = reinterpret_cast<const std::uint8_t*>(e.title);
        if (!xs_readable(src, e.size)) continue;  // released title memory: keep the last contents
        if (std::memcmp(e.last.data(), src, e.size) == 0) continue;
        std::memcpy(e.last.data(), src, e.size);
        std::memcpy(g_arena_mapped + e.va, e.last.data(), e.size);
        bytes += e.size;
        ++views;
        if (!e.told_change && xs_cbv_shadow_mode() >= 2) {  // =2: proof that the title updates CBs after the view
            e.told_change = true;
            static std::atomic<int> told{0};
            if (told++ < 24) {
                std::fprintf(stderr, "[hack] CBV_SHADOW: title 0x%llx (+0x%x) changed after its view was made (shadow VA 0x%llx)\n",
                             (unsigned long long)e.title, e.size, (unsigned long long)e.va);
                std::fflush(stderr);
            }
        }
    }
    g_cbv_changed_bytes += bytes;
    g_cbv_changed_views += views;
    const std::uint64_t k = ++g_cbv_syncs;
    if ((k & (k - 1)) == 0 || (k & 4095) == 0) {
        std::fprintf(stderr, "[hack] CBV_SHADOW: %llu syncs, %zu views, %llu KiB pool used, %llu views / %llu KiB refreshed in all\n",
                     (unsigned long long)k, g_cbv.size(), (unsigned long long)(g_cbv_pool_used >> 10),
                     (unsigned long long)g_cbv_changed_views.load(), (unsigned long long)(g_cbv_changed_bytes.load() >> 10));
        std::fflush(stderr);
    }
}

// HACK (Leonida CE boot; slots from Marvisak/VIctory, GPLv3): resources at title addresses (device slot 133) and
// views of them (slots 70/71: (resource desc, view desc, handle), the PC view description followed by the title
// address at +40). The resources are stand-ins not backed by the title's memory yet.
// magic code, don't wanna deal with hours of plumbing with what these conjured devils of the souls of gpt 6 astra did.. jeez
struct XsPlaced { std::uint64_t va, size; ID3D12Resource* resource; D3D12_RESOURCE_DESC desc; int refs = 0; };
std::mutex g_placed_mu;
std::deque<XsPlaced> g_placed;  // agA5: deque, so xs_find_placed pointers survive push_back
// HACK (agA13): LEONIDA_CE_PLACED_EVICT=1. The arena never released a buffer (re28/re29: 57344 allocations, 0
// releases): every stand-in stays referenced by g_placed forever (agA5's AddRef, plus the creation reference of the
// slot-70/71 stand-ins nobody else holds). With eviction, a new record drops the older entries whose console range it
// overlaps (the title reused that memory, so their contents are gone on the console too) and releases our references.
// g_placed_live indexes the live, disjoint ranges: start -> index in g_placed.
std::map<std::uint64_t, std::size_t> g_placed_live;
std::uint64_t g_placed_records = 0, g_placed_evicted = 0;
bool xs_placed_evict() {
    static const bool on = [] { const char* e = std::getenv("LEONIDA_CE_PLACED_EVICT"); return e && (e[0] == '1' || e[0] == '2'); }();
    return on;
}
void xs_record_placed(std::uint64_t va, std::uint64_t size, ID3D12Resource* resource,
                      const D3D12_RESOURCE_DESC* console_desc = nullptr, bool owned = false) {
    if (!va || !resource) return;
    // HACK (agA5): hold a reference, so a stand-in the title releases is never freed under a later view of its VA
    // (re1: CreateShaderResourceView -> Object::from -> Release -> free of a dead resource, heap corruption).
    static const bool keep = [] { const char* e = std::getenv("LEONIDA_CE_PLACED_REF"); return !(e && e[0] == 0); }();
    if (keep) resource->AddRef();
    const bool evict = xs_placed_evict();
    std::vector<std::pair<ID3D12Resource*, int>> drop;
    {
        std::lock_guard lock(g_placed_mu);
        if (evict) {
            const std::uint64_t end = va + (size ? size : 1);
            auto it = g_placed_live.upper_bound(va);
            if (it != g_placed_live.begin()) {
                auto pv = std::prev(it);
                const auto& e = g_placed[pv->second];
                if (e.va + e.size > va) it = pv;
            }
            while (it != g_placed_live.end() && it->first < end) {
                auto& e = g_placed[it->second];
                if (e.resource && e.refs) drop.emplace_back(e.resource, e.refs);
                e.resource = nullptr;
                e.size = 0;
                e.refs = 0;
                e.desc.Dimension = D3D12_RESOURCE_DIMENSION_UNKNOWN;
                it = g_placed_live.erase(it);
                ++g_placed_evicted;
            }
        }
        g_placed.push_back({va, size ? size : 1, resource, console_desc ? *console_desc : resource->GetDesc(),
                            (keep ? 1 : 0) + ((owned && evict) ? 1 : 0)});
        if (evict) g_placed_live[va] = g_placed.size() - 1;
        // agA13: LEONIDA_CE_PLACED_CAP=N keeps at most N of the runtime's own slot-70/71 stand-ins (re31 census: they
        // are the arena growth, +0x83b94 = proxy_slot<70>, ~8k per minute at fresh VAs); the oldest live one goes first.
        static const std::size_t cap = [] { const char* e = std::getenv("LEONIDA_CE_PLACED_CAP"); return e ? std::size_t(std::strtoull(e, nullptr, 0)) : std::size_t(0); }();
        static std::deque<std::size_t> owned_order;
        if (evict && owned && cap) {
            owned_order.push_back(g_placed.size() - 1);
            while (owned_order.size() > cap) {
                const std::size_t idx = owned_order.front();
                owned_order.pop_front();
                auto& e = g_placed[idx];
                if (!e.resource) continue;  // evicted by overlap already
                const auto live = g_placed_live.find(e.va);
                if (live != g_placed_live.end() && live->second == idx) g_placed_live.erase(live);
                if (e.refs) drop.emplace_back(e.resource, e.refs);
                e.resource = nullptr;
                e.size = 0;
                e.refs = 0;
                e.desc.Dimension = D3D12_RESOURCE_DIMENSION_UNKNOWN;
                ++g_placed_evicted;
            }
        }
        ++g_placed_records;
        if ((g_placed_records & 2047) == 0) {
            // agA13: orphans = live entries only we hold (the title released its object). LEONIDA_CE_PLACED_EVICT=2
            // also drops the orphans the title made itself (slot 133 / console texture memory, refs == 1); the
            // runtime's own slot-70/71 stand-ins (refs == 2) are only ever evicted by overlap.
            static const bool sweep = [] { const char* e = std::getenv("LEONIDA_CE_PLACED_EVICT"); return e && e[0] == '2'; }();
            std::size_t orphans = 0, swept = 0;
            for (auto it = g_placed_live.begin(); it != g_placed_live.end();) {
                auto& e = g_placed[it->second];
                if (e.resource && e.refs) {
                    e.resource->AddRef();
                    const ULONG n = e.resource->Release();
                    if (n == ULONG(e.refs)) {
                        ++orphans;
                        if (sweep && e.refs == 1 && it->second + 1 != g_placed.size()) {
                            drop.emplace_back(e.resource, e.refs);
                            e.resource = nullptr;
                            e.size = 0;
                            e.refs = 0;
                            e.desc.Dimension = D3D12_RESOURCE_DIMENSION_UNKNOWN;
                            it = g_placed_live.erase(it);
                            ++swept;
                            continue;
                        }
                    }
                }
                ++it;
            }
            std::fprintf(stderr, "[hack] placed: %llu records, %llu evicted, %zu live, %zu orphans, %zu swept (evict %d)\n",
                         (unsigned long long)g_placed_records, (unsigned long long)g_placed_evicted,
                         g_placed_live.size(), orphans, swept, int(evict) + int(sweep));
            std::fflush(stderr);
        }
        {   // DIAGNOSTIC (agA5): small-format 2D textures (glyph-cache hunt)
            const auto& d = g_placed.back().desc;
            static std::atomic<int> told{0};
            const int f = static_cast<int>(d.Format);
            if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && (f == 60 || f == 61 || f == 62 || f == 65 || f == 49 || f == 28) &&
                d.Width >= 256 && d.Height >= 256 && told++ < 40)
                std::fprintf(stderr, "[glyph?] placed 2D va %llx %llux%u fmt %d mips %u layout 0x%x size %llx\n", (unsigned long long)va,
                             (unsigned long long)d.Width, d.Height, f, d.MipLevels, static_cast<unsigned>(d.Layout), (unsigned long long)size);
        }
    }
    // Our references go after the lock: a Release may run a destructor that calls back into the device.
    for (auto& [r, n] : drop)
        while (n-- > 0) r->Release();
}
bool xs_placed_by_resource(const void* resource, XsPlaced& out) {
    std::lock_guard lock(g_placed_mu);
    for (auto it = g_placed.rbegin(); it != g_placed.rend(); ++it)
        if (it->resource == resource) { out = *it; return true; }
    return false;
}
const XsPlaced* xs_find_placed(std::uint64_t va) {
    std::lock_guard lock(g_placed_mu);
    if (xs_placed_evict()) {  // agA13: live ranges are disjoint
        auto it = g_placed_live.upper_bound(va);
        if (it == g_placed_live.begin()) return nullptr;
        const XsPlaced& e = g_placed[std::prev(it)->second];
        return (e.resource && va >= e.va && va < e.va + e.size) ? &e : nullptr;
    }
    for (auto it = g_placed.rbegin(); it != g_placed.rend(); ++it)
        if (va >= it->va && va < it->va + it->size) return &*it;
    return nullptr;
}
// HACK (agA13, LEONIDA_CE_CBV_FIX=1): a GPU address in title memory that a placed / stand-in BUFFER covers becomes that
// buffer's GPU VA (the GPU-side copy the runtime refreshes), instead of reaching a shader as a raw title pointer
// (re30: cs_25d19264 READ_INVALID 0x1e1f7f013000). 0 when no buffer covers [va, va + bytes).
bool xs_cbv_fix_on() {
    static const bool on = [] { const char* v = std::getenv("LEONIDA_CE_CBV_FIX"); return v && v[0] == '1'; }();
    return on;
}
std::uint64_t xs_placed_buffer_va(std::uint64_t va, std::uint64_t bytes) {
    const XsPlaced* p = xs_find_placed(va);
    if (!p || !p->resource || p->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return 0;
    if (bytes && va - p->va + bytes > p->size) return 0;
    const std::uint64_t gpu = p->resource->GetGPUVirtualAddress();
    return gpu ? gpu + (va - p->va) : 0;
}
void xs_cbv_fix_note(const char* what, std::uint64_t from, std::uint64_t to) {
    static std::atomic<int> told{0};
    if (told++ < 24) {
        std::fprintf(stderr, "[hack] CBV_FIX: %s 0x%llx -> 0x%llx\n", what, (unsigned long long)from, (unsigned long long)to);
        std::fflush(stderr);
    }
}
std::uint64_t xs_format_bytes(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT: return 16;
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UINT: return 8;
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_TYPELESS: return 2;
    case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_TYPELESS: return 1;
    default: return 4;
    }
}

// HACK (Leonida CE boot): textures the title placed in its own memory get mip 0 copied in when a view of them is
// made (detiled from the console's 64 KiB layout when the layout word says so). Once per resource for now.
std::atomic<ID3D12CommandQueue*> g_upload_queue{nullptr};  // the title's direct queue, seen at ExecuteCommandLists
std::mutex g_uploaded_mu;
std::set<const void*> g_uploaded;

struct XsTexFormat { std::uint32_t element_bytes, block_dim; };
XsTexFormat xs_tex_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM: return {8, 4};
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: return {16, 4};
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_A8_UNORM: return {1, 1};
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_B5G6R5_UNORM: return {2, 1};
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R32G32_FLOAT: return {8, 1};
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return {16, 1};
    default: return {4, 1};
    }
}

bool xs_safe_get_desc(ID3D12Resource* t, D3D12_RESOURCE_DESC* out) {  // HACK (agA2)
    __try {
        *out = t->GetDesc();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void xs_upload_placed_texture(ID3D12Device* dev, const D3D12_RESOURCE_DESC& console_desc, ID3D12Resource* tex,
                              std::uint64_t va, bool force = false) {
    ID3D12CommandQueue* queue = g_upload_queue.load();
    if (!queue || !tex || console_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;
    {
        std::lock_guard lock(g_uploaded_mu);
        if (!g_uploaded.insert(tex).second && !force) return;
    }
    // The console layout word is 0x100 | swizzle mode (xg_texture.h); anything else is read as linear.
    const std::uint32_t layout_word = static_cast<std::uint32_t>(console_desc.Layout);
    leonida::xg::TextureDesc td;
    td.dimension = leonida::xg::Dimension::Tex2d;
    td.width = static_cast<std::uint32_t>(console_desc.Width);
    td.height = console_desc.Height;
    td.depth_or_array = std::max<std::uint32_t>(console_desc.DepthOrArraySize, 1);
    td.mips = console_desc.MipLevels;
    td.format = static_cast<std::uint32_t>(console_desc.Format);
    td.samples = 1;
    td.swizzle.value = (layout_word & 0x100) ? (layout_word & 0xFF) : leonida::xg::Swizzle::LINEAR;
    leonida::xg::Texture xt;
    if (leonida::xg::Texture::make(td, xt) != leonida::xg::Error::Ok || xt.planes.empty()) return;
    const leonida::xg::Layout& lay = xt.planes[0].layout;
    if (!lay.addressable()) {
        static std::atomic<int> warned{0};
        if (warned++ < 8)
            std::fprintf(stderr, "[hack] placed texture %ux%u layout 0x%x: swizzle not addressable, skipped\n", td.width,
                         td.height, layout_word);
        return;
    }
    const std::size_t src_bytes = static_cast<std::size_t>(lay.size);
    rt::xmem::commit_physical_range(reinterpret_cast<const void*>(va), src_bytes);  // XMem views commit on touch
    if (!src_bytes || !xs_readable(reinterpret_cast<const void*>(va), src_bytes)) return;
    const auto* src = reinterpret_cast<const std::uint8_t*>(va);
    // HACK (agA2): rc5/rc6 crashed here in GetDesc of a released stand-in (vtable 0, then a dangling pointer).
    if (!xs_readable(tex, sizeof(void*)) || !*reinterpret_cast<void* const*>(tex)) return;
    D3D12_RESOURCE_DESC rd{};
    if (!xs_safe_get_desc(tex, &rd)) {
        static std::atomic<int> told_gd{0};
        if (told_gd++ < 4) std::fprintf(stderr, "[hack] placed texture refresh: GetDesc faulted on %p, skipped\n", (void*)tex);
        return;
    }
    const UINT mips = std::min<UINT>(rd.MipLevels ? rd.MipLevels : 1, static_cast<UINT>(lay.mips.size()));
    const UINT slices = std::min<UINT>(rd.DepthOrArraySize ? rd.DepthOrArraySize : 1, lay.slices ? lay.slices : 1);
    const UINT pc_mips = rd.MipLevels ? rd.MipLevels : 1;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fps(std::size_t(pc_mips) * slices);
    std::vector<UINT> rows(fps.size());
    std::vector<UINT64> row_bytes(fps.size());
    UINT64 total = 0;
    dev->GetCopyableFootprints(&rd, 0, static_cast<UINT>(fps.size()), 0, fps.data(), rows.data(), row_bytes.data(), &total);
    if (!total) return;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* up = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    const D3D12_COMMAND_LIST_TYPE type = queue->GetDesc().Type;
    bool ok = SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                     nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&up))) &&
              SUCCEEDED(dev->CreateCommandAllocator(type, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&alloc))) &&
              SUCCEEDED(dev->CreateCommandList(0, type, alloc, nullptr, __uuidof(ID3D12GraphicsCommandList),
                                               reinterpret_cast<void**>(&list))) &&
              SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence)));
    void* mapped = nullptr;
    if (ok && SUCCEEDED(up->Map(0, nullptr, &mapped)) && mapped) {
        auto* dst_base = static_cast<std::uint8_t*>(mapped);
        std::size_t nz = 0, sampled = 0;
        UINT copied = 0, failed = 0;
        for (UINT s = 0; s < slices; ++s)
            for (UINT m = 0; m < mips; ++m) {
                const UINT sub = m + s * pc_mips;
                const auto& fp = fps[sub];
                const std::size_t row_pitch = fp.Footprint.RowPitch;
                const std::size_t depth_pitch = row_pitch * rows[sub];
                const std::size_t room = static_cast<std::size_t>(total - fp.Offset);
                if (lay.detile(src, src_bytes, m, s, dst_base + fp.Offset, room, row_pitch, depth_pitch) !=
                    leonida::xg::Error::Ok) {
                    ++failed;
                    continue;
                }
                ++copied;
                if (m == 0 && s == 0)
                    for (std::size_t i = 0; i < depth_pitch; i += 4099, ++sampled) nz += dst_base[fp.Offset + i] != 0;  // agA3: was 97 (WC reads)
            }
        up->Unmap(0, nullptr);
        for (UINT s = 0; s < slices; ++s)
            for (UINT m = 0; m < mips; ++m) {
                const UINT sub = m + s * pc_mips;
                D3D12_TEXTURE_COPY_LOCATION dst{}, srcl{};
                dst.pResource = tex;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = sub;
                srcl.pResource = up;
                srcl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                srcl.PlacedFootprint = fps[sub];
                list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
            }
        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, 1);
        // HACK (agA3, LEONIDA_CE_UPLOAD_WAIT=1 restores the old wait): no blocking wait; the objects are released
        // later, once their fence completes (rc8: a game thread sat in here while state 3 began).
        static const bool wait_old = [] { const char* v = std::getenv("LEONIDA_CE_UPLOAD_WAIT"); return v && v[0] == '1'; }();
        if (wait_old) {
            for (int i = 0; i < 2000 && fence->GetCompletedValue() < 1; ++i) Sleep(1);
        } else {
            struct Pending { ID3D12Fence* f; ID3D12GraphicsCommandList* l; ID3D12CommandAllocator* a; ID3D12Resource* u; };
            static std::mutex pend_mu;
            static std::vector<Pending> pend;
            std::lock_guard pl(pend_mu);
            pend.push_back({fence, list, alloc, up});
            fence = nullptr; list = nullptr; alloc = nullptr; up = nullptr;
            for (auto it = pend.begin(); it != pend.end();) {
                if (it->f->GetCompletedValue() >= 1) {
                    it->f->Release(); it->l->Release(); it->a->Release(); it->u->Release();
                    it = pend.erase(it);
                } else {
                    ++it;
                }
            }
        }
        static std::atomic<int> n{0};
        const bool has_data = nz != 0;
        static std::atomic<int> with_data{0};
        if (has_data) ++with_data;
        if (n++ < 12 || (has_data && with_data.load() <= 40)) {
            std::fprintf(stderr,
                         "[hack] uploaded placed texture %ux%u fmt %d (layout 0x%x, %u mips x %u slices, %u failed, "
                         "%.1f%% non-zero) from va %llx\n",
                         td.width, td.height, (int)console_desc.Format, layout_word, mips, slices, failed,
                         sampled ? 100.0 * nz / sampled : 0.0, (unsigned long long)va);
            std::fflush(stderr);
        }
        (void)copied;
    }
    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (up) up->Release();
}

// DIAGNOSTIC (Leonida CE boot, LEONIDA_CE_STACKS=1: at presents 300 and 20000): suspend every other thread and unwind
// its stack with the image's unwind data, printing game_scarlett_master.exe-relative frames (G+rva; others are host
// addresses, llvm-symbolizer --obj=gta6rt.exe names the runtime's). Shows what each thread waits on.
thread_local const void* t_last_tls_block = nullptr;  // the block xs_game_thread_index last read

long xs_game_thread_index(HANDLE th) {
    t_last_tls_block = nullptr;
    using QueryFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    static const auto query =
        reinterpret_cast<QueryFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    if (!query) return -2;
    struct { LONG status; PVOID teb; void* pid; void* tid; ULONG_PTR mask; LONG prio, base; } info{};
    if (query(th, 0 /* ThreadBasicInformation */, &info, sizeof(info), nullptr) != 0 || !info.teb) return -2;
    const auto slot = *reinterpret_cast<const volatile std::uint32_t*>(0x146e08b2cull);  // title TLS index
    if (!xs_readable(static_cast<std::uint8_t*>(info.teb) + 0x58, 8)) return -2;  // agA23: the thread may have exited
    const auto* tls = *reinterpret_cast<std::uint8_t* const* const*>(static_cast<std::uint8_t*>(info.teb) + 0x58);
    if (!tls || !xs_readable(tls + slot, 8)) return -2;
    const std::uint8_t* block = tls[slot];
    t_last_tls_block = block;
    if (!block || !xs_readable(block + 0x1454, 4)) return -2;
    return *reinterpret_cast<const std::int32_t*>(block + 0x1454);
}

// DIAGNOSTIC (Leonida CE boot): LEONIDA_CE_STACKS=1 also sweeps every thread's title TLS block every 300 presents
// and logs a thread whose block changed since the last sweep, or two threads sharing one (a stale or swapped
// thread-local block would explain per-thread allocator state going wrong only under Wine).
void xs_tls_sweep(std::uint64_t frame) {
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    static std::map<DWORD, const void*> last;
    std::map<DWORD, const void*> now;
    std::map<const void*, DWORD> owners;
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    int changed = 0, shared = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        xs_game_thread_index(th);
        const void* block = t_last_tls_block;
        CloseHandle(th);
        if (!block) continue;
        now[te.th32ThreadID] = block;
        if (auto o = owners.find(block); o != owners.end()) {
            if (shared++ < 8)
                std::fprintf(stderr, "[tls] frame %llu SHARED block %p: tid %lx and tid %lx\n",
                             static_cast<unsigned long long>(frame), block, o->second, te.th32ThreadID);
        } else {
            owners[block] = te.th32ThreadID;
        }
        if (auto l = last.find(te.th32ThreadID); l != last.end() && l->second != block && changed++ < 8)
            std::fprintf(stderr, "[tls] frame %llu tid %lx block CHANGED %p -> %p\n", static_cast<unsigned long long>(frame),
                         te.th32ThreadID, l->second, block);
    }
    CloseHandle(snap);
    {
        char hits[1024];
        int n = std::snprintf(hits, sizeof(hits), "[list] frame %llu calls:", static_cast<unsigned long long>(frame));
        for (unsigned slot = 0; slot < command_list_proxy_table_size() && n < static_cast<int>(sizeof(hits)) - 24; ++slot)
            if (const std::uint64_t h = list_slot_hits(slot))
                n += std::snprintf(hits + n, sizeof(hits) - n, " %u:%llu", slot, static_cast<unsigned long long>(h));
        std::fprintf(stderr, "%s\n", hits);
    }
    {   // SMOKE: the UI texture's VA from its T# (0x100001900): is there image data in title memory?
        const std::uint64_t base = 0x100001900ull, span = 3840ull * 2160 * 4;
        rt::xmem::commit_physical_range(reinterpret_cast<const void*>(base), span);
        if (xs_readable(reinterpret_cast<const void*>(base), static_cast<UINT_PTR>(span))) {
            std::uint64_t nonzero = 0, samples = 0;
            for (std::uint64_t o = 0; o + 4 <= span; o += 4096, ++samples)
                nonzero += *reinterpret_cast<const std::uint32_t*>(base + o) != 0;
            const auto* w = reinterpret_cast<const std::uint32_t*>(base + span / 2);
            std::fprintf(stderr, "[smoke] texture 0x100001900: %llu of %llu samples non-zero; middle %08x %08x %08x %08x\n",
                         static_cast<unsigned long long>(nonzero), static_cast<unsigned long long>(samples), w[0], w[1],
                         w[2], w[3]);
        } else {
            std::fprintf(stderr, "[smoke] texture 0x100001900: not readable\n");
        }
    }
    {   // SMOKE: what is at CPU address 0xb8df10 (the UI vertex view's VA) in this process?
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(0xb8df10ull), &mbi, sizeof(mbi))) {
            std::fprintf(stderr, "[smoke] cpu 0xb8df10: base %p alloc base %p region %llx state %lx protect %lx type %lx",
                         mbi.BaseAddress, mbi.AllocationBase, static_cast<unsigned long long>(mbi.RegionSize), mbi.State,
                         mbi.Protect, mbi.Type);
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                const auto* v = reinterpret_cast<const float*>(0xb8df10ull);
                std::fprintf(stderr, " | %g %g %g %g %g %g", v[0], v[1], v[2], v[3], v[4], v[5]);
            }
            std::fprintf(stderr, "\n");
        }
    }
    if (g_arena_mapped && g_arena_size > 0xb8df10 + 72) {  // SMOKE: is the UI vertex data in the arena at its VA?
        const auto* v = reinterpret_cast<const float*>(g_arena_mapped + 0xb8df10);
        const auto* c = reinterpret_cast<const std::uint32_t*>(g_arena_mapped + 0xb8df10);
        std::fprintf(stderr, "[smoke] arena+0xb8df10: %g %g %g %g %g %g | %08x %08x %08x\n", v[0], v[1], v[2], v[3], v[4],
                     v[5], c[0], c[3], c[6]);
    }
    {   // the boot state machine (static analysis): state 0x1498F828C, profile task 0x1478B2818, its inner state
        // 0x1478B4AC0, active pad 0x146C31B50 (+ cached 0x1478CCE28), profile ready byte 0x1498489C0, pad 0 entry bits
        auto rd32 = [](std::uint64_t a) -> long long {
            return xs_readable(reinterpret_cast<const void*>(a), 4) ? *reinterpret_cast<const volatile std::int32_t*>(a)
                                                                     : -999;
        };
        auto rd8 = [](std::uint64_t a) -> int {
            return xs_readable(reinterpret_cast<const void*>(a), 1) ? *reinterpret_cast<const volatile std::uint8_t*>(a)
                                                                     : -1;
        };
        std::fprintf(stderr, "[boot] frame %llu: state %lld task %lld inner %lld pad %lld cached %lld ready %d pad0 0x%02x\n",
                     static_cast<unsigned long long>(frame), rd32(0x1498F828Cull), rd32(0x1478B2818ull),
                     rd32(0x1478B4AC0ull), rd32(0x146C31B50ull), rd32(0x1478CCE28ull), rd8(0x1498489C0ull),
                     rd8(0x1498482B8ull + 0x144));
    }
    std::fprintf(stderr, "[tls] frame %llu sweep: %zu threads with a block, %d changed, %d shared\n",
                 static_cast<unsigned long long>(frame), now.size(), changed, shared);
    std::fflush(stderr);
    last.swap(now);
}

// agA23: the unwind of a RESUMED thread reads its live stack. rn9 crashed here (AV in ntdll RtlVirtualUnwind via
// KERNEL32, read of a stack address) when the walked thread exited mid-walk (background compute threads finish
// during the dump) or was itself another concurrent dumper. The walk now runs under SEH and dumps are serialized.
// No C++ objects in this function: it is a __try frame.
int xs_unwind_into(CONTEXT* ctx, char* line, int len, int cap, std::uint64_t lo, std::uint64_t hi, bool* any_game) {
    __try {
        for (int depth = 0; depth < 40 && ctx->Rip; ++depth) {
            const std::uint64_t rip = ctx->Rip;
            const bool game = rip >= lo && rip < hi;
            *any_game |= game;
            if (len < cap - 40)
                len += std::snprintf(line + len, cap - len, game ? " G+%llx" : " %llx",
                                     static_cast<unsigned long long>(game ? rip - lo : rip));
            DWORD64 image_base = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(rip, &image_base, nullptr);
            if (!fn) {  // leaf: return address at [rsp]
                if (!xs_readable(reinterpret_cast<void*>(ctx->Rsp), 8)) break;
                ctx->Rip = *reinterpret_cast<DWORD64*>(ctx->Rsp);
                ctx->Rsp += 8;
            } else {
                void* handler_data = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, rip, fn, ctx, &handler_data, &establisher, nullptr);
            }
            if (ctx->Rip == rip) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (len < cap - 16) len += std::snprintf(line + len, cap - len, " <fault>");
    }
    return len;
}

void xs_dump_thread_stacks() {
    static std::atomic<bool> dumping{false};  // agA23: one dump at a time (two dumpers walked each other in rn9)
    if (dumping.exchange(true)) {
        std::fprintf(stderr, "[stacks] dump already running, skipped\n");
        return;
    }
    struct Clear { ~Clear() { dumping.store(false); } } clear_on_exit;
    static int dump_calls = 0;
    const int dumped_call = ++dump_calls;
    const DWORD self_tid = GetCurrentThreadId(), pid = GetCurrentProcessId();
    const std::uint64_t lo = 0x140000000ull, hi = 0x147000000ull;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    int dumped = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self_tid) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                               te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) == static_cast<DWORD>(-1)) { CloseHandle(th); continue; }
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_FULL;
        char line[2048];
        int len = 0;
        // agA3: resume BEFORE unwinding/printing. rd1 deadlocked: the suspended thread held the CRT stderr lock
        // (inside fprintf) and this fprintf then waited on it forever, with the thread never resumed.
        const BOOL got_ctx = GetThreadContext(th, &ctx);
        ResumeThread(th);
        if (got_ctx) {
            bool any_game = false;
            len = xs_unwind_into(&ctx, line, len, static_cast<int>(sizeof(line)), lo, hi, &any_game);
            if (any_game) {
                const long game_index = xs_game_thread_index(th);
                const void* tls_block = t_last_tls_block;
                static std::vector<std::pair<const void*, DWORD>> seen_blocks;
                static int seen_dump = -1;
                if (seen_dump != dumped_call) { seen_blocks.clear(); seen_dump = dumped_call; }
                for (const auto& [b, owner] : seen_blocks)
                    if (b && b == tls_block)
                        std::fprintf(stderr, "[tls] SHARED block %p: tid %lx and tid %lx\n", b, owner, te.th32ThreadID);
                seen_blocks.emplace_back(tls_block, te.th32ThreadID);
                std::fprintf(stderr, "[stacks] tid %lx idx %ld tls %p:%s\n", te.th32ThreadID, game_index, tls_block,
                             line);
                ++dumped;
            }
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    std::fprintf(stderr, "[stacks] %d threads with game frames\n", dumped);
    std::fflush(stderr);
}

// DIAGNOSTIC (Leonida CE boot): LEONIDA_CE_TRAP_PAGE=<hex address> write-protects that 4 KiB page from about 90 s
// and logs every write to it (writing instruction and game callers), stepping the write with the trap flag and
// protecting the page again after it. Every 60 presents the page's 1.0f floats are counted, so floats that appear
// without a trapped write point at a writer the protection cannot see (the GPU through host-imported pages).
std::uintptr_t g_trap_page = 0;
std::atomic<bool> g_trap_armed{false};
std::atomic<int> g_trap_logged{0};
std::uintptr_t g_slot_lo = 0, g_slot_hi = 0;  // LEONIDA_CE_SLOT_TRAP: log only writes in here
thread_local bool t_trap_stepping = false;
thread_local int t_trap_before = 0;
thread_local char t_trap_line[512];
std::atomic<int> g_trap_float_logged{0};
// The arena that allocates in the trapped page (found from its allocator's writes there): its page is trapped too,
// and a write that moves its current-block pointer (+0x10) backwards, a reset, is logged with its writer.
std::atomic<std::uintptr_t> g_trap_arena{0};
std::uintptr_t g_trap_arena_page = 0;
thread_local bool t_trap_arena_step = false;
thread_local std::uint64_t t_trap_arena_before = 0;
thread_local char t_trap_arena_line[512];
std::atomic<int> g_trap_reset_logged{0};
std::atomic<std::uintptr_t> g_watch_arena{0};
std::atomic<DWORD> g_watch_owner{0};

long xs_self_game_index() {
    const auto slot = *reinterpret_cast<const volatile std::uint32_t*>(0x146e08b2cull);
    const auto* tls = *reinterpret_cast<std::uint8_t* const* const*>(reinterpret_cast<std::uint8_t*>(NtCurrentTeb()) + 0x58);
    if (!tls) return -2;
    const std::uint8_t* block = tls[slot];
    if (!block) return -2;
    return *reinterpret_cast<const std::int32_t*>(block + 0x1454);
}

int xs_trap_floats() {
    if (!g_trap_page || !xs_readable(reinterpret_cast<const void*>(g_trap_page), 4096)) return -1;
    int n = 0;
    const auto* w = reinterpret_cast<const volatile std::uint32_t*>(g_trap_page);
    for (int i = 0; i < 1024; ++i) n += w[i] == 0x3f800000u;
    return n;
}

LONG CALLBACK xs_trap_handler(EXCEPTION_POINTERS* info) {
    if (!g_trap_page || !info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    const auto* r = info->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && t_trap_arena_step) {
        t_trap_arena_step = false;
        const std::uintptr_t arena = g_trap_arena.load();
        const std::uint64_t after = *reinterpret_cast<const volatile std::uint64_t*>(arena + 0x10);
        if (after < t_trap_arena_before && g_trap_reset_logged++ < 40) {
            std::fprintf(stderr, "%s RESET %llx -> %llx\n", t_trap_arena_line,
                         static_cast<unsigned long long>(t_trap_arena_before), static_cast<unsigned long long>(after));
            std::fflush(stderr);
        }
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(g_trap_arena_page), 4096, PAGE_READONLY, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && t_trap_stepping) {
        t_trap_stepping = false;
        const int after = xs_trap_floats();
        if (after > t_trap_before && g_trap_float_logged++ < 100) {
            std::fprintf(stderr, "%s FLOATS %d->%d\n", t_trap_line, t_trap_before, after);
            std::fflush(stderr);
        }
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(g_trap_page), 4096, PAGE_READONLY, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto addr = static_cast<std::uintptr_t>(r->ExceptionInformation[1]);
    if (g_trap_arena_page && addr >= g_trap_arena_page && addr < g_trap_arena_page + 4096) {
        const std::uintptr_t arena = g_trap_arena.load();
        t_trap_arena_before = *reinterpret_cast<const volatile std::uint64_t*>(arena + 0x10);
        const std::uint64_t ip = info->ContextRecord->Rip;
        int len = std::snprintf(t_trap_arena_line, sizeof(t_trap_arena_line), "[trap] t=%lu arena write %llx by %s+%llx callers",
                                GetTickCount(), static_cast<unsigned long long>(addr),
                                (ip >= 0x140000000ull && ip < 0x147000000ull) ? "G" : "host",
                                static_cast<unsigned long long>(ip >= 0x140000000ull && ip < 0x147000000ull
                                                                    ? ip - 0x140000000ull : ip));
        const auto* sp = reinterpret_cast<const std::uint64_t*>(info->ContextRecord->Rsp);
        int found = 0;
        for (int i = 0; i < 256 && found < 8 && len < static_cast<int>(sizeof(t_trap_arena_line)) - 24; ++i) {
            if (!xs_readable(sp + i, 8)) break;
            const std::uint64_t v = sp[i];
            if (v >= 0x140001000ull && v < 0x146000000ull) {
                len += std::snprintf(t_trap_arena_line + len, sizeof(t_trap_arena_line) - len, " G+%llx",
                                     static_cast<unsigned long long>(v - 0x140000000ull));
                ++found;
            }
        }
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(g_trap_arena_page), 4096, PAGE_READWRITE, &old);
        t_trap_arena_step = true;
        info->ContextRecord->EFlags |= 0x100;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (addr < g_trap_page || addr >= g_trap_page + 4096) return EXCEPTION_CONTINUE_SEARCH;
    const std::uint64_t rip = info->ContextRecord->Rip;
    t_trap_before = xs_trap_floats();
    {
        char* line = t_trap_line;
        constexpr int kLine = sizeof(t_trap_line);
        int len = std::snprintf(line, kLine, "[trap] t=%lu tid %lx idx %ld write %llx by %s+%llx floats %d callers",
                                GetTickCount(), GetCurrentThreadId(), xs_self_game_index(),
                                static_cast<unsigned long long>(addr),
                                (rip >= 0x140000000ull && rip < 0x147000000ull) ? "G" : "host",
                                static_cast<unsigned long long>(rip >= 0x140000000ull && rip < 0x147000000ull
                                                                    ? rip - 0x140000000ull : rip),
                                xs_trap_floats());
        const auto* sp = reinterpret_cast<const std::uint64_t*>(info->ContextRecord->Rsp);
        int found = 0;
        for (int i = 0; i < 256 && found < 6 && len < kLine - 24; ++i) {
            if (!xs_readable(sp + i, 8)) break;
            const std::uint64_t v = sp[i];
            if (v >= 0x140001000ull && v < 0x146000000ull) {
                len += std::snprintf(line + len, kLine - len, " G+%llx",
                                     static_cast<unsigned long long>(v - 0x140000000ull));
                ++found;
            }
        }
        if (rip == 0x140aa0c2bull && !g_watch_arena.load()) {
            g_watch_owner = GetCurrentThreadId();
            g_watch_arena = static_cast<std::uintptr_t>(info->ContextRecord->Rcx);
            CONTEXT ctx = *info->ContextRecord;
            char line[2048];
            int len = std::snprintf(line, sizeof(line), "[owner] arena %llx; frames (rip@rsp):",
                                    static_cast<unsigned long long>(ctx.Rcx));
            const std::uint64_t arena = ctx.Rcx;
            for (int depth = 0; depth < 40 && ctx.Rip && len < static_cast<int>(sizeof(line)) - 64; ++depth) {
                const std::uint64_t frip = ctx.Rip;
                const std::uint64_t frsp = ctx.Rsp;
                const bool game = frip >= 0x140000000ull && frip < 0x147000000ull;
                len += std::snprintf(line + len, sizeof(line) - len, game ? " G+%llx@%llx" : " %llx@%llx",
                                     static_cast<unsigned long long>(game ? frip - 0x140000000ull : frip),
                                     static_cast<unsigned long long>(frsp));
                DWORD64 image_base = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(frip, &image_base, nullptr);
                if (!fn) {
                    if (!xs_readable(reinterpret_cast<void*>(ctx.Rsp), 8)) break;
                    ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                    ctx.Rsp += 8;
                } else {
                    void* handler_data = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, frip, fn, &ctx, &handler_data, &establisher,
                                     nullptr);
                }
                // the arena lies in the frame [frsp, ctx.Rsp) of the function at frip
                if (arena >= frsp && arena < ctx.Rsp)
                    len += std::snprintf(line + len, sizeof(line) - len, " <ARENA IN THIS FRAME>");
                if (ctx.Rip == frip) break;
            }
            std::fprintf(stderr, "%s\n", line);
            std::fflush(stderr);
        }
        static std::atomic<int> arena_logged{0};
        static const bool trap_arena = GetEnvironmentVariableA("LEONIDA_CE_TRAP_ARENA", nullptr, 0) != 0;
        if (trap_arena && rip == 0x140aa0c2bull && !g_trap_arena.load()) {
            const std::uintptr_t arena = static_cast<std::uintptr_t>(info->ContextRecord->Rcx);
            const std::uintptr_t page = arena & ~std::uintptr_t(4095);
            if (page != g_trap_page && ((arena + 0x18 - 1) & ~std::uintptr_t(4095)) == page) {
                g_trap_arena = arena;
                g_trap_arena_page = page;
                DWORD old = 0;
                VirtualProtect(reinterpret_cast<void*>(page), 4096, PAGE_READONLY, &old);
                std::fprintf(stderr, "[trap] arena %llx: its page %llx is trapped too\n",
                             static_cast<unsigned long long>(arena), static_cast<unsigned long long>(page));
            }
        }
        if (rip == 0x140aa0c2bull && arena_logged++ < 12) {  // the arena allocator writing its end marker here
            const auto* arena = reinterpret_cast<const std::uint64_t*>(info->ContextRecord->Rcx);
            std::fprintf(stderr, "%s ARENA rcx=%llx [+8]=%llx [+0x10]=%llx\n", line,
                         static_cast<unsigned long long>(info->ContextRecord->Rcx),
                         xs_readable(arena, 0x18) ? static_cast<unsigned long long>(arena[1]) : 0ull,
                         xs_readable(arena, 0x18) ? static_cast<unsigned long long>(arena[2]) : 0ull);
            std::fflush(stderr);
        }
        const bool slot_write = !g_slot_hi || (addr >= g_slot_lo && addr < g_slot_hi);
        if (slot_write && g_trap_logged++ < (g_slot_hi ? 60 : 20)) {
            std::fprintf(stderr, "%s\n", line);
            std::fflush(stderr);
        }
    }
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(g_trap_page), 4096, PAGE_READWRITE, &old);
    t_trap_stepping = true;
    info->ContextRecord->EFlags |= 0x100;  // trap flag: re-protect after this instruction
    return EXCEPTION_CONTINUE_EXECUTION;
}

void xs_dump_one_thread(DWORD tid, const char* why) {
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!th) return;
    if (SuspendThread(th) == static_cast<DWORD>(-1)) { CloseHandle(th); return; }
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    char line[1024];
    int len = std::snprintf(line, sizeof(line), "[watch] %s: tid %lx stack", why, tid);
    if (GetThreadContext(th, &ctx)) {
        len += std::snprintf(line + len, sizeof(line) - len, " (rsp %llx)", static_cast<unsigned long long>(ctx.Rsp));
        for (int depth = 0; depth < 24 && ctx.Rip && len < static_cast<int>(sizeof(line)) - 40; ++depth) {
            const std::uint64_t rip = ctx.Rip;
            const bool game = rip >= 0x140000000ull && rip < 0x147000000ull;
            len += std::snprintf(line + len, sizeof(line) - len, game ? " G+%llx" : " %llx",
                                 static_cast<unsigned long long>(game ? rip - 0x140000000ull : rip));
            DWORD64 image_base = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(rip, &image_base, nullptr);
            if (!fn) {
                if (!xs_readable(reinterpret_cast<void*>(ctx.Rsp), 8)) break;
                ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
            } else {
                void* handler_data = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, rip, fn, &ctx, &handler_data, &establisher, nullptr);
            }
            if (ctx.Rip == rip) break;
        }
    }
    ResumeThread(th);
    CloseHandle(th);
    std::fprintf(stderr, "%s\n", line);
    std::fflush(stderr);
}

void xs_trap_tick(std::uint64_t /*frame*/) {
    static const std::uintptr_t page = [] {
        char v[32] = {};
        if (!GetEnvironmentVariableA("LEONIDA_CE_TRAP_PAGE", v, sizeof(v))) return std::uintptr_t(0);
        return static_cast<std::uintptr_t>(std::strtoull(v, nullptr, 16)) & ~std::uintptr_t(4095);
    }();
    if (!page) return;
    static const bool started = [] {
        // Presents can stall while the title loads, so the trap runs on its own thread.
        HANDLE t = CreateThread(nullptr, 0, [](void*) -> DWORD {
            char d[16] = {};
            Sleep(GetEnvironmentVariableA("LEONIDA_CE_TRAP_DELAY", d, sizeof(d)) ? std::strtoul(d, nullptr, 10) * 1000 : 85000);
            g_trap_page = page;
            AddVectoredExceptionHandler(1, &xs_trap_handler);
            DWORD old = 0;
            const BOOL ok = VirtualProtect(reinterpret_cast<void*>(page), 4096, PAGE_READONLY, &old);
            std::fprintf(stderr, "[trap] t=%lu armed page %llx (protect %d, old 0x%lx), floats %d\n", GetTickCount(),
                         static_cast<unsigned long long>(page), ok, old, xs_trap_floats());
            std::fflush(stderr);
            g_trap_armed = true;
            int last = -2;
            std::uint64_t prev = 0;
            int resets = 0;
            for (unsigned tick = 0;; ++tick) {
                if (const std::uintptr_t arena = g_watch_arena.load()) {
                    // The arena object's first word: when it changes the object was freed or reused; dump every
                    // thread once, while the thread that freed it is likely still near the free.
                    static std::uint64_t first = 0;
                    static bool dumped = false;
                    const std::uint64_t head = *reinterpret_cast<const volatile std::uint64_t*>(arena);
                    if (!first) first = head;
                    else if (head != first && !dumped) {
                        dumped = true;
                        std::fprintf(stderr, "[watch] t=%lu arena %llx head %llx -> %llx: all threads\n", GetTickCount(),
                                     static_cast<unsigned long long>(arena), static_cast<unsigned long long>(first),
                                     static_cast<unsigned long long>(head));
                        xs_dump_thread_stacks();
                    }
                    const std::uint64_t cur = *reinterpret_cast<const volatile std::uint64_t*>(arena + 0x10);
                    if (prev && cur < prev && resets++ < 12) {
                        char why[160];
                        std::snprintf(why, sizeof(why), "t=%lu arena %llx current %llx -> %llx", GetTickCount(),
                                      static_cast<unsigned long long>(arena), static_cast<unsigned long long>(prev),
                                      static_cast<unsigned long long>(cur));
                        xs_dump_one_thread(g_watch_owner.load(), why);
                    }
                    prev = cur;
                }
                if (tick % 2000 == 0) {
                    const int n = xs_trap_floats();
                    if (n != last) {
                        std::fprintf(stderr, "[trap] t=%lu floats in page: %d\n", GetTickCount(), n);
                        std::fflush(stderr);
                        last = n;
                    }
                }
                Sleep(1);
            }
        }, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
        return true;
    }();
    (void)started;
}

// HACK: placed textures whose title memory changed since the last look are copied in again (sampled checksum,
// one word per 4 KiB). The title fills them after their views are made (streaming, CPU writes).
std::mutex g_placed_hash_mu;
std::map<const void*, std::uint64_t> g_placed_hash;
// HACK (agA13): a reference on each copied record for the refresh passes, which use the resources outside
// g_placed_mu: PLACED_EVICT / PLACED_CAP may release a record meanwhile (re34: AV in xs_safe_get_desc <-
// xs_upload_placed_texture <- xs_refresh_placed_textures at t=203.7, a freed stand-in).
struct XsHold {
    std::vector<XsPlaced>& v;
    ~XsHold() { for (auto& p : v) if (p.resource) p.resource->Release(); }
};
void xs_refresh_placed_textures(ID3D12Device* dev) {
    static const bool no_refresh = [] { const char* v = std::getenv("LEONIDA_CE_NO_PLACED_REFRESH"); return v && v[0] == '1'; }();  // agA2
    if (no_refresh) return;
    std::vector<XsPlaced> textures;
    XsHold hold{textures};
    {
        std::lock_guard lock(g_placed_mu);
        for (const auto& p : g_placed)
            if (p.resource && p.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
                textures.push_back(p);
                p.resource->AddRef();
            }
    }
    for (const auto& t : textures) {
        const std::uint64_t len = t.size;
        if (len) rt::xmem::commit_physical_range(reinterpret_cast<const void*>(t.va), len);  // XMem views commit on touch
        if (!len || !xs_readable(reinterpret_cast<const void*>(t.va), static_cast<UINT_PTR>(len))) continue;
        std::uint64_t h = 1469598103934665603ull, nonzero = 0;
        // HACK (agA5): sampling 8 bytes per 4 KiB missed glyphs Scaleform adds to its glyph-cache textures (re3: most
        // body-text glyphs missing). Textures up to LEONIDA_CE_REFRESH_FULL_KB (default 4096) are hashed in full.
        static const std::uint64_t full_bytes = [] {
            const char* v = std::getenv("LEONIDA_CE_REFRESH_FULL_KB");
            return (v ? std::strtoull(v, nullptr, 10) : 4096ull) * 1024ull;
        }();
        // Only single-channel textures (A8/R8: Scaleform glyph caches): hashing every texture in full ran re4 at ~3 fps.
        const DXGI_FORMAT ff = t.desc.Format;
        const bool glyphish = ff == DXGI_FORMAT_A8_UNORM || ff == DXGI_FORMAT_R8_UNORM || ff == DXGI_FORMAT_R8_TYPELESS ||
                              static_cast<int>(ff) == 65 || static_cast<int>(ff) == 61;
        const std::uint64_t stride = glyphish && len <= full_bytes ? 8 : 4096;
        for (std::uint64_t o = 0; o + 8 <= len; o += stride) {
            std::uint64_t w = 0;
            std::memcpy(&w, reinterpret_cast<const void*>(t.va + o), 8);
            h = (h ^ w) * 1099511628211ull;
            nonzero |= w;
        }
        bool changed = false;
        {
            std::lock_guard lock(g_placed_hash_mu);
            auto& old = g_placed_hash[t.resource];
            changed = old != h;
            old = h;
        }
        if (changed && nonzero) {
            static std::atomic<int> told_rf{0};  // DIAGNOSTIC (bw): which placed textures the refresh re-uploads
            static std::atomic<int> told_a8{0};  // agA5: every A8 (glyph cache) re-upload, up to 200
            if (static_cast<int>(t.desc.Format) == 65 && told_a8++ < 200) {
                std::fprintf(stderr, "[refresh] A8 re-upload va %llx %llux%u layout 0x%x\n", (unsigned long long)t.va,
                             (unsigned long long)t.desc.Width, t.desc.Height, static_cast<unsigned>(t.desc.Layout));
                std::fflush(stderr);
            }
            if (told_rf++ < 60 || told_rf % 200 == 0) {
                std::fprintf(stderr, "[refresh] re-upload va %llx %llux%u fmt %d\n", (unsigned long long)t.va,
                             (unsigned long long)t.desc.Width, t.desc.Height, (int)t.desc.Format);
                std::fflush(stderr);
            }
            xs_upload_placed_texture(dev, t.desc, t.resource, t.va, true);
        }
    }
}


// HACK (Leonida CE): placed BUFFERS in title memory were never refreshed (only placed textures are, every 30
// presents), so data the title writes there on the CPU each frame -- its UI vertices -- never reached the GPU copy:
// the UI draw read zeros, collapsed to a point, and every frame stayed black. Before each submission, copy every
// placed buffer up to 8 MiB whose title bytes changed (full hash) into its GPU resource. LEONIDA_CE_NO_BUFFER_SYNC=1
// turns it off.
void xs_refresh_placed_buffers(ID3D12CommandQueue* queue) {
    // Opt-in (LEONIDA_CE_BUFFER_SYNC=1): once the boot runs the title has hundreds of placed buffers, and hashing all of
    // them before every submission stalled the present thread; the per-draw ring sync covers what draws read.
    static const bool off = [] {
        char v[8] = {};
        return !(GetEnvironmentVariableA("LEONIDA_CE_BUFFER_SYNC", v, sizeof(v)) && v[0] == '1');
    }();
    if (off || !queue) return;
    std::vector<XsPlaced> buffers;
    XsHold hold{buffers};
    {
        std::lock_guard lock(g_placed_mu);
        for (const auto& p : g_placed)
            if (p.resource && p.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && p.size && p.size <= (8ull << 20)) {
                buffers.push_back(p);
                p.resource->AddRef();
            }
    }
    if (buffers.empty()) return;
    std::vector<const XsPlaced*> changed;
    std::uint64_t total = 0;
    for (const auto& b : buffers) {
        rt::xmem::commit_physical_range(reinterpret_cast<const void*>(b.va), b.size);
        if (!xs_readable(reinterpret_cast<const void*>(b.va), static_cast<UINT_PTR>(b.size))) continue;
        std::uint64_t h = 1469598103934665603ull;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(b.va);
        std::uint64_t o = 0;
        for (; o + 8 <= b.size; o += 8) {
            std::uint64_t w;
            std::memcpy(&w, bytes + o, 8);
            h = (h ^ w) * 1099511628211ull;
        }
        for (; o < b.size; ++o) h = (h ^ bytes[o]) * 1099511628211ull;
        std::lock_guard lock(g_placed_hash_mu);
        auto& old = g_placed_hash[b.resource];
        if (old != h) {
            old = h;
            changed.push_back(&b);
            total += b.size;
        }
    }
    if (changed.empty()) return;
    ID3D12Device* dev = nullptr;
    if (FAILED(queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&dev))) || !dev) return;
    if (ID3D12Device* real = device_proxy_real_device(dev)) {
        real->AddRef();
        dev->Release();
        dev = real;
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* staging = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    const D3D12_COMMAND_LIST_TYPE type = queue->GetDesc().Type;
    bool ok = SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                     nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&staging))) &&
              SUCCEEDED(dev->CreateCommandAllocator(type, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&alloc))) &&
              SUCCEEDED(dev->CreateCommandList(0, type, alloc, nullptr, __uuidof(ID3D12GraphicsCommandList),
                                               reinterpret_cast<void**>(&list))) &&
              SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence)));
    void* mapped = nullptr;
    if (ok && SUCCEEDED(staging->Map(0, nullptr, &mapped)) && mapped) {
        std::uint64_t at = 0;
        for (const XsPlaced* b : changed) {
            std::memcpy(static_cast<std::uint8_t*>(mapped) + at, reinterpret_cast<const void*>(b->va), b->size);
            list->CopyBufferRegion(b->resource, 0, staging, at, b->size);
            at += b->size;
        }
        staging->Unmap(0, nullptr);
        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, 1);
        for (int i = 0; i < 2000 && fence->GetCompletedValue() < 1; ++i) Sleep(0);
        static std::atomic<int> told{0};
        if (told++ < 5) {
            std::fprintf(stderr, "[hack] synced %zu placed buffer(s), %llu bytes, from title memory\n", changed.size(),
                         static_cast<unsigned long long>(total));
            std::fflush(stderr);
        }
    }
    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (staging) staging->Release();
    dev->Release();
}

std::atomic<std::uint64_t> g_hits[kMaxSlots];
std::atomic<bool> g_first[kMaxSlots];
std::atomic<bool> g_beyond_warned[kMaxSlots];
std::atomic<std::uint64_t> g_heap_hits[kHeapSlots];
std::atomic<bool> g_heap_first[kHeapSlots];
// The hit counters are for tests. They count from the first call of a *_slot_hits reader or of
// reset_device_proxy_stats, so the calls of a title, which reads no counter, write no shared counter.
std::atomic<bool> g_count_hits{false};

// For the readers and the reset. The flag is written one time only.
void start_hit_counts() {
    if (!g_count_hits.load(std::memory_order_relaxed)) g_count_hits.store(true, std::memory_order_relaxed);
}

void count_hit(std::atomic<std::uint64_t>& hits) {
    if (g_count_hits.load(std::memory_order_relaxed)) hits.fetch_add(1, std::memory_order_relaxed);
}

// True for the first call of a slot only. The flag is read first: a compare-exchange takes the cache line for
// writing also when it fails, and the flags of all slots share one or two lines.
bool first_call(std::atomic<bool>& seen) {
    if (seen.load(std::memory_order_relaxed)) return false;
    bool expected = false;
    return seen.compare_exchange_strong(expected, true, std::memory_order_relaxed);
}

bool trace_enabled() {
    static const bool enabled = [] {
        char value[8] = {};
        const DWORD length = GetEnvironmentVariableA("XS_TRACE", value, sizeof(value));
        return length > 0 && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

const char* slot_name(unsigned slot) {
    switch (slot) {
        case 0: return "QueryInterface";
        case 1: return "AddRef";
        case 2: return "Release";
        case 8: return "CreateCommandQueue -> queue proxy";
        case 9: return "CreateCommandAllocator (PC slot 9)";
        case 10: return "CreateGraphicsPipelineState (PC slot 10)";
        case 11: return "CreateComputePipelineState (PC slot 11)";
        case 14: return "CreateDescriptorHeap";
        case 16: return "CreateRootSignature";
        case 25: return "GetResourceAllocationInfo";
        case 27: return "CreateCommittedResource (PC slot 27)";
        case 31: return "CreateSharedHandle (proxies unwrapped)";
        case 32: return "OpenSharedHandle (a fence comes back wrapped)";
        case 34: return "MakeResident (proxies unwrapped)";
        case 35: return "Evict (proxies unwrapped)";
        case 36: return "CreateFence -> fence proxy";
        case 43: return "GetAdapterLuid";
        case 45: return "SetEventOnMultipleFenceCompletion (fence proxies unwrapped)";
        case 46: return "SetResidencyPriority (proxies unwrapped)";
        case 48: return "Xbox CreateCommandQueueX -> queue proxy";
        case 50: return "EnqueueMakeResident (proxies unwrapped)";
        case 62: return "Xbox CreateComputePipelineState";
        case 67: return "Xbox DeserializeComputePipelineStateX";
        case 73: return "Xbox CreateCommandListX -> PC CreateCommandList";
        case 75: return "CreateCommandQueue1 -> queue proxy";
        case 76: return "Xbox resource creation";
        case 78: return "Xbox SetFrameIntervalX";
        case 79: return "Xbox ScheduleFrameEventX";
        case 80: return "Xbox WaitFrameEventX";
        case 133: return "Xbox CreatePlacedResourceX (a buffer on the title's pages)";
        default: return "forwarded";
    }
}

void note_slot(unsigned slot) {
    if (slot >= kMaxSlots) return;
    count_hit(g_hits[slot]);
    const bool first = first_call(g_first[slot]);
    if (first) {
        std::fprintf(stderr, "[rt] d3d12_xs: device slot %u (%s) first call\n", slot, slot_name(slot));
    } else if (slot >= 3 && trace_enabled()) {
        std::fprintf(stderr, "[rt] d3d12_xs: device slot %u (%s) call\n", slot, slot_name(slot));
    }
}

// Xbox device slots (public HLE Freminet/ScarlettX, an observation, not Microsoft documentation). The defaults are
// the HLE's; an environment variable that names another slot in 3..127 replaces one (a whole-table move for a trace
// that disagrees). A value outside that range is logged once and ignored. The table is read once, on the first
// generic slot call, so a test that wants other slots sets the variables before it creates a device.
struct XboxDeviceSlotRoutes {
    unsigned frame_interval = 78;  // SetFrameIntervalX(output, interval_us, latency, flags)
    unsigned schedule_event = 79;  // ScheduleFrameEventX(event, offset_us, reserved, flags)
    unsigned wait_event = 80;      // WaitFrameEventX(event, timeout_ms, reserved, flags, &token)
    unsigned queue_x = 48;         // CreateCommandQueueX(desc, riid, out)
    unsigned list_x = 73;          // CreateCommandListX(desc, allocator, initial_state, riid, out)
};

unsigned env_device_slot(const char* name, unsigned fallback) {
    char value[32] = {};
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    if (length == 0) return fallback;
    if (length >= sizeof(value)) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s is too long for a device slot; the default is kept\n", name);
        return fallback;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long slot = std::strtoul(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0' || slot < 3 || slot > 127) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s=%s is not a device slot in 3..127; the default is kept\n", name,
                     value);
        return fallback;
    }
    return static_cast<unsigned>(slot);
}

const XboxDeviceSlotRoutes& xbox_device_slot_routes() {
    static const XboxDeviceSlotRoutes routes = [] {
        XboxDeviceSlotRoutes r;
        const unsigned frame_interval = env_device_slot("LEONIDA_XS_FRAME_INTERVAL_SLOT", r.frame_interval);
        const unsigned schedule_event = env_device_slot("LEONIDA_XS_SCHEDULE_EVENT_SLOT", r.schedule_event);
        const unsigned wait_event = env_device_slot("LEONIDA_XS_WAIT_EVENT_SLOT", r.wait_event);
        const unsigned queue_x = env_device_slot("LEONIDA_XS_QUEUE_X_SLOT", r.queue_x);
        const unsigned list_x = env_device_slot("LEONIDA_XS_LIST_X_SLOT", r.list_x);
        const bool moved = frame_interval != r.frame_interval || schedule_event != r.schedule_event ||
                           wait_event != r.wait_event || queue_x != r.queue_x || list_x != r.list_x;
        r.frame_interval = frame_interval;
        r.schedule_event = schedule_event;
        r.wait_event = wait_event;
        r.queue_x = queue_x;
        r.list_x = list_x;
        if (moved)
            std::fprintf(stderr,
                         "[rt] d3d12_xs: Xbox device slots: SetFrameIntervalX=%u ScheduleFrameEventX=%u "
                         "WaitFrameEventX=%u CreateCommandQueueX=%u CreateCommandListX=%u\n",
                         r.frame_interval, r.schedule_event, r.wait_event, r.queue_x, r.list_x);
        return r;
    }();
    return routes;
}

// The public HLE call shapes. SetFrameIntervalX's output and flags are unused while there is no output or pacing;
// ScheduleFrameEventX's reserved event pointer is a3 in the console call and a4 is its flags. The logged words are the
// caller's, so a trace names the slot even when an override moved it.
void log_xbox_route_once(unsigned slot, const char* name, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                         std::uint64_t a4, std::uint64_t a5) {
    static std::atomic<bool> logged[kMaxSlots];
    if (slot >= kMaxSlots) return;
    bool expected = false;
    if (!logged[slot].compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
    std::fprintf(stderr,
                 "[rt] d3d12_xs: device slot %u routed to %s (args 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx)\n", slot,
                 name, static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                 static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4),
                 static_cast<unsigned long long>(a5));
}

const char* heap_slot_name(unsigned slot) {
    switch (slot) {
        case 0: return "QueryInterface";
        case 1: return "AddRef";
        case 2: return "Release";
        case 7: return "GetDevice";
        case 8: return "GetDesc";
        case 9: return "GetCPUDescriptorHandleForHeapStart";
        case 10: return "GetGPUDescriptorHandleForHeapStart";
        default: return "forwarded";
    }
}

void note_heap_slot(unsigned slot) {
    if (slot >= kHeapSlots) return;
    count_hit(g_heap_hits[slot]);
    const bool first = first_call(g_heap_first[slot]);
    if (first) {
        std::fprintf(stderr, "[rt] d3d12_xs: heap slot %u (%s) first call\n", slot, heap_slot_name(slot));
    } else if (slot >= 3 && trace_enabled()) {
        std::fprintf(stderr, "[rt] d3d12_xs: heap slot %u (%s) call\n", slot, heap_slot_name(slot));
    }
}

__declspec(noinline) std::uint64_t hr_to_u64(HRESULT hr) {
    if (FAILED(hr)) {  // HACK diagnostic
        static std::atomic<int> n{0};
        if (n++ < 40) { std::fprintf(stderr, "[hack] routed call failed 0x%08lx from %p\n", (unsigned long)hr, _ReturnAddress()); std::fflush(stderr); }
    }
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(hr));
}

unsigned long proxy_add_ref(DeviceProxy* self) {
    note_slot(1);
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

unsigned long proxy_release(DeviceProxy* self) {
    note_slot(2);
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) {
        ID3D12Device* real = self->real;
        delete self;
        if (real) real->Release();
    }
    return refs;
}

bool is_device_iid(const IID& iid) {
    if (IsEqualIID(iid, __uuidof(ID3D12Device))) return true;
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device14))) return true;
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device13))) return true;
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device12))) return true;
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device11))) return true;
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device10))) return true;
#endif
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device9))) return true;
#endif
    if (IsEqualIID(iid, __uuidof(ID3D12Device8))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device7))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device6))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device5))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device4))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device3))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device2))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device1))) return true;
    return false;
}

HRESULT proxy_query_interface(DeviceProxy* self, const IID* riid, void** out) {
    note_slot(0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (IsEqualIID(*riid, __uuidof(IUnknown)) || IsEqualIID(*riid, __uuidof(ID3D12Object))) {
        *out = self;
        proxy_add_ref(self);
        return S_OK;
    }
    if (IsEqualIID(*riid, __uuidof(IDXGIObject)) || IsEqualIID(*riid, __uuidof(IDXGIDevice)) ||
        IsEqualIID(*riid, __uuidof(IDXGIDevice1))) {
        *out = &self->dxgi_vtable;
        proxy_add_ref(self);
        return S_OK;
    }
    if (!is_device_iid(*riid)) return E_NOINTERFACE;

    // Only claim an interface the PC device really implements; the proxy vtable covers its slots.
    void* supported = nullptr;
    HRESULT hr = self->real->QueryInterface(*riid, &supported);
    if (FAILED(hr)) return hr;
    if (!supported) return E_NOINTERFACE;
    static_cast<IUnknown*>(supported)->Release();
    *out = self;
    proxy_add_ref(self);
    return S_OK;
}

// ---- IDXGIDevice1 on the device ---------------------------------------------------------------------------------
//
// A GDKX title finds its output through the device: QueryInterface for IDXGIDevice1, GetAdapter, EnumOutputs(0),
// and hands that output to SetFrameIntervalX (docs/d3d12_vulkan.md section 4.5, from the public samples). A PC
// ID3D12Device has no IDXGIDevice, so the proxy answers it itself: a second vtable pointer inside the proxy object,
// with the proxy's reference count and QueryInterface, so the two pointers are one COM object. Only IDXGIDevice and
// IDXGIDevice1 are answered (what the samples ask for). The 14 slots are the public dxgi.h order; that the console
// keeps it is the same assumption as for the queue and the fence.
//
// GetAdapter and GetParent hand out the adapter of the device behind the proxy. The provider says how: the default
// asks the host's DXGI for the adapter with the device's LUID (the host backend); set_device_adapter_provider puts
// the Vulkan layer's own adapter there, so that backend never loads the host's dxgi.dll for it.
constexpr unsigned kDxgiDeviceSlots = 14;  // IUnknown 3, IDXGIObject 4, IDXGIDevice 5, IDXGIDevice1 2

DeviceProxy* dxgi_owner(void* self) {
    return reinterpret_cast<DeviceProxy*>(static_cast<char*>(self) - offsetof(DeviceProxy, dxgi_vtable));
}

HRESULT host_adapter(ID3D12Device* real, REFIID riid, void** out) {
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    static const CreateFactoryFn create_factory = [] {
        const HMODULE dxgi = LoadLibraryExA("dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return dxgi ? reinterpret_cast<CreateFactoryFn>(
                          reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")))
                    : nullptr;
    }();
    if (!create_factory) return DXGI_ERROR_NOT_FOUND;
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = create_factory(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) return FAILED(hr) ? hr : DXGI_ERROR_NOT_FOUND;
    hr = factory->EnumAdapterByLuid(real->GetAdapterLuid(), riid, out);
    factory->Release();
    return hr;
}

HRESULT dxgi_adapter(DeviceProxy* self, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    const DxgiAdapterProvider provider = self->adapter_provider ? self->adapter_provider : &host_adapter;
    const HRESULT hr = provider(self->real, riid, out);
    if (FAILED(hr)) *out = nullptr;
    return hr;
}

void note_dxgi(const char* method) {
    d3d12_trace::record(method);
}

HRESULT WINAPI dxgi_query_interface(void* self, const IID* riid, void** out) {
    return proxy_query_interface(dxgi_owner(self), riid, out);
}
ULONG WINAPI dxgi_add_ref(void* self) { return proxy_add_ref(dxgi_owner(self)); }
ULONG WINAPI dxgi_release(void* self) { return proxy_release(dxgi_owner(self)); }

// IDXGIObject's private data is the device's: ID3D12Object has the same three methods with the same arguments.
HRESULT WINAPI dxgi_set_private_data(void* self, const GUID* name, UINT size, const void* data) {
    if (!name) return E_INVALIDARG;
    return dxgi_owner(self)->real->SetPrivateData(*name, size, data);
}
HRESULT WINAPI dxgi_set_private_data_interface(void* self, const GUID* name, const IUnknown* object) {
    if (!name) return E_INVALIDARG;
    return dxgi_owner(self)->real->SetPrivateDataInterface(*name, object);
}
HRESULT WINAPI dxgi_get_private_data(void* self, const GUID* name, UINT* size, void* data) {
    if (!name) return E_INVALIDARG;
    return dxgi_owner(self)->real->GetPrivateData(*name, size, data);
}
HRESULT WINAPI dxgi_get_parent(void* self, const IID* riid, void** out) {
    note_dxgi("XboxIDXGIDevice::GetParent");
    if (!riid) return E_POINTER;
    return dxgi_adapter(dxgi_owner(self), *riid, out);
}
HRESULT WINAPI dxgi_get_adapter(void* self, IDXGIAdapter** out) {
    note_dxgi("XboxIDXGIDevice::GetAdapter");
    const HRESULT hr = dxgi_adapter(dxgi_owner(self), __uuidof(IDXGIAdapter), reinterpret_cast<void**>(out));
    std::fprintf(stderr, "[hack] XboxIDXGIDevice::GetAdapter -> 0x%08lx adapter=%p provider=%s caller %p\n", (unsigned long)hr,
                 out ? (void*)*out : nullptr, dxgi_owner(self)->adapter_provider ? "vulkan" : "HOST", _ReturnAddress());
    std::fflush(stderr);
    return hr;
}
// CreateSurface and QueryResourceResidency are D3D10/11-era methods no D3D12 title has a use for; what the console
// answers is not measured, so they are refused and say so once.
HRESULT WINAPI dxgi_create_surface(void*, const DXGI_SURFACE_DESC*, UINT, DXGI_USAGE, const DXGI_SHARED_RESOURCE*,
                                   IDXGISurface** out) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] d3d12_xs: IDXGIDevice::CreateSurface is not implemented; returning E_NOTIMPL\n");
    if (out) *out = nullptr;
    return E_NOTIMPL;
}
HRESULT WINAPI dxgi_query_resource_residency(void*, IUnknown* const*, DXGI_RESIDENCY*, UINT) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: IDXGIDevice::QueryResourceResidency is not implemented; returning E_NOTIMPL\n");
    return E_NOTIMPL;
}
// The priority and the frame latency are kept and read back; nothing acts on them (the Xbox presenter's latency is
// SetFrameIntervalX's). The ranges are the documented ones: -7..7, and 1..16 with 0 for the default of 3.
HRESULT WINAPI dxgi_set_gpu_thread_priority(void* self, INT priority) {
    if (priority < -7 || priority > 7) return E_INVALIDARG;
    dxgi_owner(self)->gpu_thread_priority.store(priority, std::memory_order_relaxed);
    return S_OK;
}
HRESULT WINAPI dxgi_get_gpu_thread_priority(void* self, INT* priority) {
    if (!priority) return E_POINTER;
    *priority = dxgi_owner(self)->gpu_thread_priority.load(std::memory_order_relaxed);
    return S_OK;
}
HRESULT WINAPI dxgi_set_maximum_frame_latency(void* self, UINT latency) {
    if (latency > 16) return DXGI_ERROR_INVALID_CALL;
    dxgi_owner(self)->max_frame_latency.store(latency ? latency : 3, std::memory_order_relaxed);
    return S_OK;
}
HRESULT WINAPI dxgi_get_maximum_frame_latency(void* self, UINT* latency) {
    if (!latency) return E_POINTER;
    *latency = dxgi_owner(self)->max_frame_latency.load(std::memory_order_relaxed);
    return S_OK;
}

void** get_dxgi_vtable() {
    static void* table[kDxgiDeviceSlots] = {
        reinterpret_cast<void*>(&dxgi_query_interface),          // 0
        reinterpret_cast<void*>(&dxgi_add_ref),                  // 1
        reinterpret_cast<void*>(&dxgi_release),                  // 2
        reinterpret_cast<void*>(&dxgi_set_private_data),         // 3  IDXGIObject
        reinterpret_cast<void*>(&dxgi_set_private_data_interface),
        reinterpret_cast<void*>(&dxgi_get_private_data),
        reinterpret_cast<void*>(&dxgi_get_parent),
        reinterpret_cast<void*>(&dxgi_get_adapter),              // 7  IDXGIDevice
        reinterpret_cast<void*>(&dxgi_create_surface),
        reinterpret_cast<void*>(&dxgi_query_resource_residency),
        reinterpret_cast<void*>(&dxgi_set_gpu_thread_priority),
        reinterpret_cast<void*>(&dxgi_get_gpu_thread_priority),
        reinterpret_cast<void*>(&dxgi_set_maximum_frame_latency),  // 12 IDXGIDevice1
        reinterpret_cast<void*>(&dxgi_get_maximum_frame_latency),
    };
    return table;
}

unsigned long heap_add_ref(HeapProxy* self) {
    note_heap_slot(1);
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

unsigned long heap_release(HeapProxy* self) {
    note_heap_slot(2);
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) {
        ID3D12DescriptorHeap* real = self->real;
        DeviceProxy* device = self->device;
        delete self;
        if (real) real->Release();
        if (device) proxy_release(device);
    }
    return refs;
}

bool is_heap_iid(const IID& iid) {
    return IsEqualIID(iid, __uuidof(ID3D12DescriptorHeap)) || IsEqualIID(iid, __uuidof(ID3D12Pageable)) ||
           IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12Object)) ||
           IsEqualIID(iid, __uuidof(IUnknown));
}

HRESULT heap_query_interface(HeapProxy* self, const IID* riid, void** out) {
    note_heap_slot(0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (!is_heap_iid(*riid)) return E_NOINTERFACE;
    *out = self;
    heap_add_ref(self);
    return S_OK;
}

// ID3D12DeviceChild::GetDevice on the heap must hand back the Xbox-ABI device, not the raw PC device.
HRESULT heap_get_device(HeapProxy* self, const IID* riid, void** out) {
    note_heap_slot(7);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (!self->device) return E_NOINTERFACE;
    return proxy_query_interface(self->device, riid, out);
}

// The PC methods write through the hidden return pointer (RDX) on the measured runtime; the Xbox ABI wants the
// 8-byte value in RAX. Call with both conventions and keep whichever produced a value, so a runtime that really
// returns in RAX also works. The extra register is harmless when the callee ignores it.
D3D12_CPU_DESCRIPTOR_HANDLE heap_get_cpu_handle(HeapProxy* self) {
    note_heap_slot(9);
    constexpr SIZE_T kUnset = ~static_cast<SIZE_T>(0);
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = kUnset;
    using Fn = D3D12_CPU_DESCRIPTOR_HANDLE(WINAPI*)(ID3D12DescriptorHeap*, D3D12_CPU_DESCRIPTOR_HANDLE*);
    const D3D12_CPU_DESCRIPTOR_HANDLE returned =
        reinterpret_cast<Fn>(self->real_vtable[9])(self->real, &handle);
    const D3D12_CPU_DESCRIPTOR_HANDLE result = handle.ptr != kUnset ? handle : returned;
    static std::atomic<int> n{0};  // HACK diagnostic
    if (n++ < 30) { std::fprintf(stderr, "[hack] heap %p CPU start -> %llx (caller %p)\n", (void*)self, (unsigned long long)result.ptr, _ReturnAddress()); std::fflush(stderr); }
    return result;
}

D3D12_GPU_DESCRIPTOR_HANDLE heap_get_gpu_handle(HeapProxy* self) {
    note_heap_slot(10);
    // HACK: unified, see above, for every heap (shader-visible or not): VIctory's start() is one address for both
    return D3D12_GPU_DESCRIPTOR_HANDLE{static_cast<UINT64>(self->real->GetCPUDescriptorHandleForHeapStart().ptr)};
    constexpr UINT64 kUnset = ~static_cast<UINT64>(0);
    D3D12_GPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = kUnset;
    using Fn = D3D12_GPU_DESCRIPTOR_HANDLE(WINAPI*)(ID3D12DescriptorHeap*, D3D12_GPU_DESCRIPTOR_HANDLE*);
    const D3D12_GPU_DESCRIPTOR_HANDLE returned =
        reinterpret_cast<Fn>(self->real_vtable[10])(self->real, &handle);
    return handle.ptr != kUnset ? handle : returned;
}

template <unsigned Slot>
std::uint64_t WINAPI heap_slot(HeapProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                               std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                               std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                               std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if constexpr (Slot == 0) {
        return hr_to_u64(heap_query_interface(self, reinterpret_cast<const IID*>(a1),
                                              reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return heap_add_ref(self);
    } else if constexpr (Slot == 2) {
        return heap_release(self);
    } else if constexpr (Slot == 7) {
        return hr_to_u64(heap_get_device(self, reinterpret_cast<const IID*>(a1),
                                         reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 9) {
        return static_cast<std::uint64_t>(heap_get_cpu_handle(self).ptr);
    } else if constexpr (Slot == 10) {
        return static_cast<std::uint64_t>(heap_get_gpu_handle(self).ptr);
    } else {
        note_heap_slot(Slot);
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t);
        auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot]);
        const std::uint64_t r_ = fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
        if (static_cast<std::int32_t>(r_) < 0 && (static_cast<std::uint32_t>(r_) >> 16) >= 0x8000u) {  // HACK diagnostic
            static std::atomic<int> n_{0};
            if (n_++ < 20) { std::fprintf(stderr, "[hack] %s slot %u failed 0x%08x caller %p\n", __FUNCTION__, Slot,
                                          static_cast<unsigned>(r_), _ReturnAddress()); std::fflush(stderr); }
        }
        return r_;
    }
}

template <std::size_t... I>
void fill_heap_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&heap_slot<static_cast<unsigned>(I)>) ), ...);
}

void** get_heap_vtable() {
    static void** vtable = [] {
        static void* storage[kHeapSlots];
        fill_heap_vtable(storage, std::make_index_sequence<kHeapSlots>{});
        return storage;
    }();
    return vtable;
}

HRESULT create_heap_proxy(ID3D12DescriptorHeap* real, DeviceProxy* device, REFIID riid, void** out) {
    if (!real || !device) return E_POINTER;
    if (!out) return E_POINTER;
    *out = nullptr;

    HeapProxy* proxy = new (std::nothrow) HeapProxy;
    if (!proxy) return E_OUTOFMEMORY;
    proxy->vtable = get_heap_vtable();
    proxy->real = real;
    proxy->device = device;
    proxy->real_vtable = *reinterpret_cast<void***>(real);
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy_add_ref(device);  // the heap keeps its Xbox-ABI device alive for GetDevice
    {   // HACK: unified handles (see xs_register_heap_range)
        const D3D12_DESCRIPTOR_HEAP_DESC hd = real->GetDesc();
        if (hd.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
            const std::uint64_t cpu = real->GetCPUDescriptorHandleForHeapStart().ptr;
            const std::uint64_t gpu = real->GetGPUDescriptorHandleForHeapStart().ptr;
            const std::uint64_t size =
                std::uint64_t(hd.NumDescriptors) * device->real->GetDescriptorHandleIncrementSize(hd.Type);
            if (cpu && cpu != gpu) xs_register_heap_range(cpu, size, gpu);
        }
    }

    const HRESULT hr = heap_query_interface(proxy, &riid, out);
    if (FAILED(hr)) {
        // No reference escaped, and the caller still owns its references to `real` and `device`.
        delete proxy;
        proxy_release(device);
        return hr;
    }
    if (!d3d12_trace::enabled())
        std::fprintf(stderr, "[rt] d3d12_xs: heap proxy %p wraps PC heap %p (device proxy %p)\n",
                     static_cast<void*>(proxy), static_cast<void*>(real), static_cast<void*>(device));
    heap_release(proxy);
    return S_OK;
}

// ---- Command queue and fence proxies --------------------------------------------------------------------------
//
// Device slots 8 (CreateCommandQueue), 75 (CreateCommandQueue1), 36 (CreateFence) and 32 (OpenSharedHandle, for a
// fence) wrap what they create, so a title never holds a PC or Vulkan-layer queue or fence. The proxies forward
// the public chain in PC order, exactly like the device proxy, and a slot past the PC vtable is logged and refused
// with E_NOTIMPL. PresentX, SuspendX and ResumeX are queue methods past the public chain
// (docs/d3d12_command_objects.md §5.3). Their slots are not known and nothing is guessed: the first run of the
// executable names them in the log.
//
// Slot numbers are those of the Windows SDK 10.0.26100 d3d12.h: ID3D12CommandQueue has 19, ID3D12Fence 11 and
// ID3D12Fence1 12. That the console keeps these public slots, and keeps device slots 8/32/36/75, is an assumption
// the shipped binaries cannot test (§3.5: the package has no queue or fence client). Every queue and fence method
// takes integer and pointer arguments only; GetDesc returns its struct through the hidden pointer in RDX, which is
// forwarded unchanged (no Xbox variant is measured, §6).
//
// The refused range is the proxy's own table: 128 queue slots and 64 fence slots, every one a thunk. The table sits
// at the end of the first page of a 64 KiB reservation whose other pages are never committed, so a slot index past
// the table makes the vtable read itself fault, at an address that names the slot (a vectored handler logs it and
// lets the fault continue), instead of jumping through whatever memory follows. A title whose vtable read lands
// further away than that reservation is not covered.
//
// Objects cross back into the real runtime wherever a device or queue method takes them, and the real object is
// passed there: queue Signal/Wait (14/15) take a fence; device CreateSharedHandle (31) a device child,
// MakeResident/Evict (34/35), SetResidencyPriority (46) and EnqueueMakeResident (50) pageables, and
// SetEventOnMultipleFenceCompletion (45) and EnqueueMakeResident fences. The heap proxies of slot 14 are pageables
// and device children too and are unwrapped in the same places. Command lists are not wrapped yet, so
// ExecuteCommandLists passes them through, and a heap proxy still reaches the real runtime through the list's
// SetDescriptorHeaps. A command-list proxy (docs/d3d12_command_objects.md §3.5a) adds its vtable to
// proxy_real_object, which every unwrap goes through, and a queue slot 10 branch in child_slot that unwraps the list
// array (UnwrappedArray) before forwarding, and unwraps heap proxies in its own SetDescriptorHeaps.
//
// QueryInterface answers only the interfaces the proxy models (the base chain and its own interface, Fence1 when
// the real fence has it), like the device and heap proxies, so the COM identity holds and no real object leaks to
// the title. Leonida's own code reaches the real object through queue_proxy_real_queue / fence_proxy_real_fence /
// allocator_proxy_real_allocator.
//
// Command allocators (device slot 9) are the third kind: their public chain is 9 slots (Reset at 8). Slot 9 wraps
// only an allocator that answers kLeonidaCommandAllocatorTag, because a list takes the title's allocator in
// CreateCommandList and Reset and only the Vulkan layer's lists resolve a proxy there (CommandAllocator::from);
// a PC runtime allocator is handed out unchanged until a command-list proxy unwraps it at Reset.

enum class ChildKind { queue, fence, allocator };

constexpr unsigned kQueueSlots = 128;        // the table: 19 public slots plus a refused tail
constexpr unsigned kFenceSlots = 64;         // the table: 12 public slots plus a refused tail
constexpr unsigned kQueuePublicSlots = 19;   // ID3D12CommandQueue
constexpr unsigned kFencePublicSlots = 11;   // ID3D12Fence
constexpr unsigned kFence1PublicSlots = 12;  // ID3D12Fence1
constexpr unsigned kAllocatorSlots = 64;     // the table: 9 public slots plus a refused tail
constexpr unsigned kAllocatorPublicSlots = 9;  // ID3D12CommandAllocator: the base chain and Reset (slot 8)
constexpr SIZE_T kTableRegion = 64 * 1024;   // one allocation granule: the table page plus 15 never-committed pages
static_assert(kQueueSlots > kQueuePublicSlots && kFenceSlots > kFence1PublicSlots &&
                  kAllocatorSlots > kAllocatorPublicSlots,
              "the tables need a tail");
static_assert(kQueueSlots * sizeof(void*) <= 4096 && kFenceSlots * sizeof(void*) <= 4096 &&
                  kAllocatorSlots * sizeof(void*) <= 4096,
              "a table must fit in the first page of its reservation");

constexpr unsigned kQueueSignal = 14, kQueueWait = 15;  // (ID3D12Fence*, UINT64)

struct ChildProxy {
    void** vtable;          // offset 0: the caller's ID3D12CommandQueue* / ID3D12Fence* points here
    IUnknown* real;         // offset 8: the PC or Vulkan-layer object every thunk forwards to
    DeviceProxy* device;    // keeps the Xbox-ABI device alive; GetDevice returns it
    void** real_vtable;
    unsigned slot_count;    // public slots the real object implements; the rest of the table is refused
    std::atomic<unsigned long> refs;
};

template <ChildKind K>
struct ChildStats {
    static constexpr unsigned kSlots = K == ChildKind::queue   ? kQueueSlots
                                       : K == ChildKind::fence ? kFenceSlots
                                                               : kAllocatorSlots;
    static inline std::atomic<std::uint64_t> hits[kSlots];
    static inline std::atomic<bool> first[kSlots];
    static inline std::atomic<bool> beyond_warned[kSlots];
    static inline std::atomic<std::uint64_t> overruns{0};
    // Where the table and its reservation are: written once, before the table is handed out.
    static inline std::atomic<std::uintptr_t> table_begin{0}, table_end{0}, region_end{0};
};

template <ChildKind K>
constexpr const char* child_kind_name() {
    return K == ChildKind::queue ? "queue" : K == ChildKind::fence ? "fence" : "allocator";
}

template <ChildKind K>
const char* child_public_name(unsigned slot) {
    static constexpr const char* object[] = {"QueryInterface", "AddRef", "Release", "GetPrivateData",
                                             "SetPrivateData", "SetPrivateDataInterface", "SetName", "GetDevice"};
    static constexpr const char* queue[] = {"UpdateTileMappings", "CopyTileMappings", "ExecuteCommandLists",
                                            "SetMarker", "BeginEvent", "EndEvent", "Signal", "Wait",
                                            "GetTimestampFrequency", "GetClockCalibration", "GetDesc"};
    static constexpr const char* fence[] = {"GetCompletedValue", "SetEventOnCompletion", "Signal",
                                            "GetCreationFlags"};
    if (slot < 8) return object[slot];
    if constexpr (K == ChildKind::queue) {
        if (slot < kQueuePublicSlots) return queue[slot - 8];
    } else if constexpr (K == ChildKind::fence) {
        if (slot < kFence1PublicSlots) return fence[slot - 8];
    } else {
        if (slot == 8) return "Reset";
    }
    return nullptr;
}

// Fixed names for the trace channel (d3d12_trace::record takes fixed strings): one per table slot, built once, in
// the trace grammar of runtime/README.md and check_d3d12_trace.py (an identifier with `::`, no spaces):
// XboxID3D12CommandQueue::Signal, and XboxID3D12CommandQueue::Slot40 for a slot past the public chain.
template <ChildKind K>
const char* child_trace_name(unsigned slot) {
    static const auto names = [] {
        static char storage[ChildStats<K>::kSlots][64];
        const char* iface = K == ChildKind::queue   ? "XboxID3D12CommandQueue"
                            : K == ChildKind::fence ? "XboxID3D12Fence"
                                                    : "XboxID3D12CommandAllocator";
        for (unsigned s = 0; s < ChildStats<K>::kSlots; ++s) {
            const char* method = child_public_name<K>(s);
            if (method)
                std::snprintf(storage[s], sizeof(storage[s]), "%s::%s", iface, method);
            else
                std::snprintf(storage[s], sizeof(storage[s]), "%s::Slot%u", iface, s);
        }
        return &storage;
    }();
    return (*names)[slot < ChildStats<K>::kSlots ? slot : 0];
}

template <ChildKind K>
void note_child_slot(unsigned slot) {
    using S = ChildStats<K>;
    if (slot >= S::kSlots) return;
    count_hit(S::hits[slot]);
    const bool first = first_call(S::first[slot]);
    const char* method = child_public_name<K>(slot);
    if (first) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s slot %u (%s) first call\n", child_kind_name<K>(), slot,
                     method ? method : "past the public chain");
    } else if (slot >= 3 && trace_enabled()) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s slot %u (%s) call\n", child_kind_name<K>(), slot,
                     method ? method : "past the public chain");
    }
}

// The counters reset_device_proxy_stats clears for one kind: per-slot hits, first-call and beyond-table flags, and
// the faulted reads past the table.
template <ChildKind K>
void reset_child_stats() {
    using S = ChildStats<K>;
    for (unsigned slot = 0; slot < S::kSlots; ++slot) {
        S::hits[slot].store(0, std::memory_order_relaxed);
        S::first[slot].store(false, std::memory_order_relaxed);
        S::beyond_warned[slot].store(false, std::memory_order_relaxed);
    }
    S::overruns.store(0, std::memory_order_relaxed);
}

// Frees a proxy whose last reference is gone, with the real object and the device reference it held.
void destroy_child(ChildProxy* self) {
    IUnknown* real = self->real;
    DeviceProxy* device = self->device;
    delete self;
    if (real) real->Release();
    if (device) proxy_release(device);
}

template <ChildKind K>
unsigned long child_add_ref(ChildProxy* self) {
    note_child_slot<K>(1);
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

template <ChildKind K>
unsigned long child_release(ChildProxy* self) {
    note_child_slot<K>(2);
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) destroy_child(self);
    return refs;
}

// The interfaces the proxy answers: the base chain and the object's own interface.
template <ChildKind K>
bool is_child_iid(const IID& iid) {
    if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(ID3D12Object)) ||
        IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12Pageable)))
        return true;
    if constexpr (K == ChildKind::queue) {
        return IsEqualIID(iid, __uuidof(ID3D12CommandQueue));
    } else if constexpr (K == ChildKind::fence) {
        return IsEqualIID(iid, __uuidof(ID3D12Fence)) || IsEqualIID(iid, __uuidof(ID3D12Fence1));
    } else {
        return IsEqualIID(iid, __uuidof(ID3D12CommandAllocator));
    }
}

bool supports_iid(IUnknown* real, REFIID iid) {
    void* found = nullptr;
    const HRESULT hr = real->QueryInterface(iid, &found);
    if (FAILED(hr) || !found) return false;
    static_cast<IUnknown*>(found)->Release();
    return true;
}

// An interface the real object implements but the proxy does not model is refused where the PC runtime would answer:
// logged once per interface (the first 32 of them), so a title that stops after such a refusal shows what it asked
// for. Also reached from wrap_child, for the interface a title creates a queue or fence as.
void log_refused_iid(const char* kind, const IID& iid) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static IID seen[32];
    static unsigned count = 0;
    AcquireSRWLockExclusive(&lock);
    bool known = false;
    for (unsigned i = 0; i < count && !known; ++i) known = IsEqualIID(seen[i], iid) != 0;
    const bool log = !known && count < std::size(seen);
    if (log) seen[count++] = iid;
    ReleaseSRWLockExclusive(&lock);
    if (!log) return;
    std::fprintf(stderr,
                 "[rt] d3d12_xs: %s proxy refuses {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}, which the real "
                 "object implements; E_NOINTERFACE\n",
                 kind, iid.Data1, iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3],
                 iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
}

template <ChildKind K>
HRESULT child_query_interface(ChildProxy* self, const IID* riid, void** out) {
    note_child_slot<K>(0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    // Only what the proxy models, and of that only what the real object implements (ID3D12Fence1 is optional).
    if (!is_child_iid<K>(*riid)) {
        if (supports_iid(self->real, *riid)) log_refused_iid(child_kind_name<K>(), *riid);
        return E_NOINTERFACE;
    }
    if (!supports_iid(self->real, *riid)) return E_NOINTERFACE;
    *out = self;
    child_add_ref<K>(self);
    return S_OK;
}

// ID3D12DeviceChild::GetDevice hands back the Xbox-ABI device, not the real one. Without an output it is the real
// object's own answer (what the PC runtime returns for a null pointer).
template <ChildKind K>
HRESULT child_get_device(ChildProxy* self, const IID* riid, void** out) {
    note_child_slot<K>(7);
    if (out) *out = nullptr;  // _COM_Outptr_: NULL on every failure (bc59ef46)
    if (!riid) return E_POINTER;
    if (!out) {
        using Fn = HRESULT(WINAPI*)(IUnknown*, const IID*, void**);
        return reinterpret_cast<Fn>(self->real_vtable[7])(self->real, riid, nullptr);
    }
    if (!self->device) return E_NOINTERFACE;
    return proxy_query_interface(self->device, riid, out);
}

void** get_heap_vtable();
void** get_list_vtable();

template <ChildKind K>
void** get_child_vtable();

template <ChildKind K>
bool is_child_proxy(const void* object) {
    return object && *reinterpret_cast<void* const*>(object) == reinterpret_cast<void*>(get_child_vtable<K>());
}

constexpr unsigned kListSlots = 160;
constexpr unsigned kConsoleSetDescriptorHeaps = 32;
constexpr unsigned kConsoleExecuteBundle = 31;
constexpr unsigned kConsoleBarrier = 146;

struct ListProxy {
    void** vtable;                      // +0x00
    ID3D12GraphicsCommandList* real;    // +0x08
    DeviceProxy* device;                // +0x10: keeps the Xbox-ABI device alive; GetDevice returns it
    // +0x18..: the console object body a title's inline writers read. +0x18 holds the write cursor, +0x28 and
    // +0x30 the record limits; the rest is room for the measured flags, header table and pipeline record.
    alignas(8) std::uint8_t console_fields[0x600];
    std::atomic<unsigned long> refs;    // past the console body
    unsigned pc_slot_count;             // slots the real list implements; mapped slots past it are refused

    void pin_cursor_at_limit() {
        // The put pointer (+0x18) and the plain, draw, API and unbiased-API limits (+0x20..+0x38) all zero, so every
        // inline "room left?" check in the title fails and each command reaches the method table as a call (after
        // VIctory, list.rs: GPLv3, credit to its authors). The old pinning (+0x18/+0x28/+0x30 = one address, +0x20 and
        // +0x38 left 0) let a "limit - put" check wrap around: the title wrote its draws inline into this body and
        // never called a draw method (no draws, black frames). LEONIDA_CE_OLD_CURSOR=1 restores the old pinning.
        static const bool old_cursor = [] {
            char v[8] = {};
            return GetEnvironmentVariableA("LEONIDA_CE_OLD_CURSOR", v, sizeof(v)) && v[0] == '1';
        }();
        if (old_cursor) {
            const std::uint64_t at_limit = static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(this) + 0x18 + 0x500);
            for (const size_t offset : {0x18, 0x28, 0x30})
                std::memcpy(reinterpret_cast<std::uint8_t*>(this) + offset, &at_limit, sizeof(at_limit));
            return;
        }
        std::memset(reinterpret_cast<std::uint8_t*>(this) + 0x18, 0, 5 * sizeof(std::uint64_t));
    }
};
static_assert(offsetof(ListProxy, console_fields) == 0x18, "the console fields start at +0x18");

// The real object behind a heap, queue, fence or list proxy, or null when `object` is none of them.
IUnknown* proxy_real_object(const void* object) {
    if (!object) return nullptr;
    void* const vtable = *reinterpret_cast<void* const*>(object);
    if (vtable == reinterpret_cast<void*>(get_heap_vtable())) return static_cast<const HeapProxy*>(object)->real;
    if (vtable == reinterpret_cast<void*>(get_child_vtable<ChildKind::queue>()) ||
        vtable == reinterpret_cast<void*>(get_child_vtable<ChildKind::fence>()) ||
        vtable == reinterpret_cast<void*>(get_child_vtable<ChildKind::allocator>()))
        return static_cast<const ChildProxy*>(object)->real;
    if (vtable == reinterpret_cast<void*>(get_list_vtable()))
        return static_cast<const ListProxy*>(object)->real;
    return nullptr;
}

// `object` with a proxy replaced by its real object; anything else (a real object, null) unchanged.
inline std::uint64_t unwrap_object(std::uint64_t object) {
    IUnknown* real = proxy_real_object(reinterpret_cast<const void*>(object));
    return real ? reinterpret_cast<std::uint64_t>(real) : object;
}

// A copy of an array of interface pointers with every proxy replaced by its real object. Nothing here throws, since
// an exception must not leave a thunk for the title's code: up to 16 entries are kept inline (no allocation, which
// also suits a per-frame ExecuteCommandLists), a longer array is allocated with new (std::nothrow).
class UnwrappedArray {
public:
    // Copies `count` entries (a UINT in every PC prototype) of `objects`. False on allocation failure.
    bool fill(std::uint64_t objects, std::uint64_t count) {
        const auto n = static_cast<std::uint32_t>(count);
        if (!objects || !n) return true;
        std::uint64_t* copy = inline_;
        if (n > std::size(inline_)) {
            heap_.reset(new (std::nothrow) std::uint64_t[n]);
            if (!heap_) return false;
            copy = heap_.get();
        }
        const auto* in = reinterpret_cast<const std::uint64_t*>(objects);
        for (std::uint32_t i = 0; i < n; ++i) copy[i] = unwrap_object(in[i]);
        data_ = copy;
        return true;
    }
    // The copy to pass on, or 0 when there was no array (the caller's argument then goes on unchanged).
    std::uint64_t data() const { return reinterpret_cast<std::uint64_t>(data_); }

private:
    std::uint64_t inline_[16];
    std::unique_ptr<std::uint64_t[]> heap_;
    std::uint64_t* data_ = nullptr;
};

struct ListStats {
    static inline std::atomic<std::uint64_t> hits[kListSlots];
    static inline std::atomic<bool> first[kListSlots];
};

inline void note_list_slot(unsigned slot, std::uint64_t a1 = 0, std::uint64_t a2 = 0, std::uint64_t a3 = 0, std::uint64_t a4 = 0) {
    if (slot >= kListSlots) return;
    ListStats::hits[slot].fetch_add(1, std::memory_order_relaxed);
    bool expected = false;
    if (ListStats::first[slot].compare_exchange_strong(expected, true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] d3d12_xs: list slot %u first call (args 0x%llx 0x%llx 0x%llx 0x%llx)\n", slot,
                     static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                     static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4));
}

inline bool is_list_iid(const IID& iid) {
    return IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(ID3D12Object)) ||
           IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12CommandList)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList1)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList2)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList3)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList4)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList5)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList6)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList7)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList8));
}

inline unsigned long list_add_ref(ListProxy* self) { return self->refs.fetch_add(1, std::memory_order_relaxed) + 1; }

inline void destroy_list(ListProxy* self) {
    IUnknown* real = self->real;
    DeviceProxy* device = self->device;
    delete self;
    if (real) real->Release();
    if (device) proxy_release(device);
}

inline unsigned long list_release(ListProxy* self) {
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) destroy_list(self);
    return refs;
}

inline HRESULT list_query_interface(ListProxy* self, const IID* riid, void** out) {
    note_list_slot(0, reinterpret_cast<std::uint64_t>(riid), reinterpret_cast<std::uint64_t>(out), 0, 0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (!is_list_iid(*riid)) {
        if (supports_iid(self->real, *riid)) log_refused_iid("list", *riid);
        return E_NOINTERFACE;
    }
    if (!supports_iid(self->real, *riid)) return E_NOINTERFACE;
    *out = self;
    list_add_ref(self);
    return S_OK;
}

inline unsigned detect_list_slot_count(IUnknown* real) {
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList8))) return 82;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList7))) return 81;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList6))) return 80;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList5))) return 79;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList4))) return 77;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList3))) return 68;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList2))) return 67;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList1))) return 66;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList))) return 60;
    return 9;
}

template <unsigned Slot>
std::uint64_t WINAPI list_slot(ListProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                               std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                               std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                               std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if constexpr (Slot == 0) {
        return hr_to_u64(list_query_interface(self, reinterpret_cast<const IID*>(a1),
                                              reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return list_add_ref(self);
    } else if constexpr (Slot == 2) {
        return list_release(self);
    } else if constexpr (Slot == 7) {  // GetDevice
        note_list_slot(7, a1, a2, 0, 0);
        if (a2) *reinterpret_cast<void**>(a2) = nullptr;
        if (!a1) return hr_to_u64(E_POINTER);
        if (!a2) {
            using Fn = HRESULT(WINAPI*)(IUnknown*, const IID*, void**);
            return hr_to_u64(reinterpret_cast<Fn>((*reinterpret_cast<void***>(self->real))[7])(
                self->real, reinterpret_cast<const IID*>(a1), nullptr));
        }
        return hr_to_u64(proxy_query_interface(self->device, reinterpret_cast<const IID*>(a1),
                                               reinterpret_cast<void**>(a2)));
    } else {
        note_list_slot(Slot, a1, a2, a3, a4);
        if constexpr (Slot == 14 || Slot == 80 || Slot == 61) {
            // agA21 LEONIDA_CE_SKIP_BVH=1 (a bisect, not a fix): drop the dispatches XboxBvhBuilder.dll records
            // (its image at 0x180000000 + 0xde000): its shaders read geometry through raw title VAs it got as data.
            static const bool skip_bvh = [] { const char* e = std::getenv("LEONIDA_CE_SKIP_BVH"); return e && e[0] == '1'; }();
            const auto ra = reinterpret_cast<std::uint64_t>(_ReturnAddress());
            if (skip_bvh && ra >= 0x180000000ull && ra < 0x1800de000ull) {
                static std::atomic<std::uint64_t> skipped{0};
                const std::uint64_t k = ++skipped;
                if ((k & (k - 1)) == 0) { std::fprintf(stderr, "[hack] SKIP_BVH: %llu builder dispatches dropped (slot %u from %p)\n", (unsigned long long)k, Slot, (void*)ra); std::fflush(stderr); }
                return 0;
            }
        }
        if constexpr (Slot == 25) {  // HACK diagnostic: which pipelines the title sets (first 12 distinct)
            { std::lock_guard pl(g_list_pso_mu); g_list_pso[self] = a1; }  // agA21 (slot 80 thread dims)
            static std::mutex m;
            static std::set<std::uint64_t> seen;
            std::lock_guard g(m);
            if (seen.size() < 12 && seen.insert(a1).second) {
                std::string name = "(not from slot 57)";
                {
                    std::lock_guard names_lock(g_pso_names_mu);
                    auto it = g_pso_names.find(reinterpret_cast<const void*>(a1));
                    if (it != g_pso_names.end()) name = it->second;
                }
                std::fprintf(stderr, "[hack] SetPipelineState %llx: %s\n", (unsigned long long)a1, name.c_str());
                std::fflush(stderr);
            }
        }
        // HACK: this title's console list slots (newer layout, see XsListExtras)
        if constexpr (Slot == 139) {  // ResetX(allocator, pipeline, flags)
            {
                std::lock_guard lock(g_list_extras_mu);
                g_list_extras.erase(self);
            }
            if (auto* real_alloc = allocator_proxy_real_allocator(reinterpret_cast<void*>(a1)))
                a1 = reinterpret_cast<std::uint64_t>(real_alloc);
            self->pin_cursor_at_limit();
            return hr_to_u64(self->real->Reset(reinterpret_cast<ID3D12CommandAllocator*>(a1),
                                               reinterpret_cast<ID3D12PipelineState*>(a2)));
        } else if constexpr (Slot == 71 || Slot == 72) {  // write32/write64(address, value, flush, flags)
            if (a1) {
                const unsigned what = (Slot == 72 && (a4 == 1 || a4 == 2)) ? static_cast<unsigned>(a4) : 0u;
                std::lock_guard lock(g_list_extras_mu);
                g_list_extras[self].writes.push_back({a1, a2, Slot == 71 ? 4u : 8u, what});
            }
            return 0;
        } else if constexpr (Slot == 148) {  // AllocateUploadMemory(size, alignment) -> pointer, until the next reset
            const std::uint64_t align = a2 < 16 ? 16 : a2;
            if ((align & (align - 1)) != 0 || a1 > (1ull << 31)) return 0;
            if (const std::uint64_t ring = xs_upload_alloc(a1, align)) return ring;  // GPU-visible (arena)
            auto block = std::make_unique<std::uint8_t[]>(static_cast<std::size_t>(a1 + align));
            std::memset(block.get(), 0, static_cast<std::size_t>(a1 + align));
            const auto start = reinterpret_cast<std::uint64_t>(block.get());
            const std::uint64_t at = (start + align - 1) & ~(align - 1);
            std::lock_guard lock(g_list_extras_mu);
            g_list_extras[self].uploads.push_back(std::move(block));
            return at;
        } else if constexpr (Slot == 86) {  // CopyResourceX(dst, src, flags)
            self->real->CopyResource(reinterpret_cast<ID3D12Resource*>(unwrap_object(a1)),
                                     reinterpret_cast<ID3D12Resource*>(unwrap_object(a2)));
            return 0;
        } else if constexpr (Slot == 80) {
            // agA21 LEONIDA_CE_SLOT80_DISPATCH (default on, =0 drops it as before): console slot 80 is the vtable
            // fallback of the five-dword dispatch packet 0x4003cc00,x,1,1,W (docs/d3d12_command_objects.md 3.3a/3.5a).
            // XboxBvhBuilder.dll calls it as (x, 1, 1, W) after Confirm; dropping it left the BVH build half done.
            static const bool on = [] { const char* e = std::getenv("LEONIDA_CE_SLOT80_DISPATCH"); return !e || e[0] != 0; }();
            if (on) {
                static std::atomic<std::uint64_t> calls{0};
                const std::uint64_t k = calls.fetch_add(1, std::memory_order_relaxed) + 1;
                // W is the console DISPATCH_INITIATOR: bit 5 USE_THREAD_DIMENSIONS = x, y, z count THREADS (the
                // hardware masks the partial last group), bit 15 CS_W32_EN. LEONIDA_CE_SLOT80_THREADS (default on)
                // turns thread counts into group counts with the bound PSO workgroup size.
                static const bool threads = [] { const char* e = std::getenv("LEONIDA_CE_SLOT80_THREADS"); return !e || e[0] != '0'; }();
                std::uint64_t g[3] = {a1, a2, a3};
                std::uint32_t ls[3] = {0, 0, 0};
                bool sized = false;
                if (threads && (a4 & 0x20)) {
                    std::uint64_t pso = 0;
                    { std::lock_guard pl(g_list_pso_mu); auto it = g_list_pso.find(self); if (it != g_list_pso.end()) pso = it->second; }
                    if (auto f = ::leonida::ce::local_size_fn().load(std::memory_order_relaxed); f && pso &&
                        f(reinterpret_cast<const void*>(pso), ls)) {
                        for (int i = 0; i < 3; ++i) g[i] = (g[i] + ls[i] - 1) / ls[i];
                        sized = true;
                    }
                }
                if (k <= 4 || (k & (k - 1)) == 0) {
                    std::fprintf(stderr, "[hack] SLOT80_DISPATCH #%llu from %p: (%llu, %llu, %llu) W=0x%llx -> groups (%llu, %llu, %llu)%s ls %u,%u,%u\n",
                                 (unsigned long long)k, _ReturnAddress(), (unsigned long long)a1, (unsigned long long)a2,
                                 (unsigned long long)a3, (unsigned long long)(a4 & 0xffffffffull), (unsigned long long)g[0],
                                 (unsigned long long)g[1], (unsigned long long)g[2], sized ? " thread-dims" : "", ls[0], ls[1], ls[2]);
                    std::fflush(stderr);
                }
                if (g[0] && g[1] && g[2] && g[0] <= 65535 && g[1] <= 65535 && g[2] <= 65535)
                    self->real->Dispatch(static_cast<UINT>(g[0]), static_cast<UINT>(g[1]), static_cast<UINT>(g[2]));
                return 0;
            }
        }
        constexpr unsigned pc = console_to_pc_list_slot(Slot);
        if constexpr (pc == kListNotForwarded) {
            {   // HACK diagnostic: every console-only list slot, once, with its arguments and caller
                static std::atomic<bool> seen{false};
                if (!seen.exchange(true, std::memory_order_relaxed)) {
                    std::fprintf(stderr, "[hack] console list slot %u (not forwarded) from %p: a1=%llx a2=%llx a3=%llx a4=%llx\n",
                                 Slot, _ReturnAddress(), (unsigned long long)a1, (unsigned long long)a2,
                                 (unsigned long long)a3, (unsigned long long)a4);
                    std::fflush(stderr);
                }
            }
            if constexpr (Slot == 148) {
                static std::atomic<bool> logged{false};
                if (!logged.exchange(true, std::memory_order_relaxed))
                    std::fprintf(stderr,
                                 "[rt] d3d12_xs: console list slot 148 AllocateUploadMemory stub called (size %llu, align %llu)\n",
                                 static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2));
            }
            return 0;
        } else {
            if (pc >= self->pc_slot_count) {
                static std::atomic<bool> beyond_warned[kListSlots];
                bool expected = false;
                if (beyond_warned[Slot].compare_exchange_strong(expected, true, std::memory_order_relaxed))
                    std::fprintf(stderr,
                                 "[rt] d3d12_xs: list slot %u maps to PC slot %u, beyond the real list's %u "
                                 "slots; returning E_NOTIMPL\n",
                                 Slot, pc, self->pc_slot_count);
                return hr_to_u64(E_NOTIMPL);
            }
            using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t);
            auto** const real_vtable = *reinterpret_cast<void***>(self->real);
            UnwrappedArray heaps;
            if constexpr (pc == 31 || pc == 32) {
                const std::uint64_t raw = a2;
                a2 = xs_gpu_handle_from_console(a2);  // HACK: unified handles
                if constexpr (pc == 32) {
                    // agA20 LEONIDA_CE_MIRROR_GFX=0: graphics tables keep the raw (title heap) handle, as before
                    // HEAP_MIRROR, when it was translated into the mirror (the presented surface stayed zero with the
                    // mirror; ri45-50 without it composited). Compute tables still read the mirror.
                    static const bool gfx_mirror = [] { const char* e = std::getenv("LEONIDA_CE_MIRROR_GFX"); return !e || e[0] != '0'; }();
                    const std::uint64_t lo = g_mirror_lo.load(std::memory_order_relaxed);
                    if (!gfx_mirror && lo && a2 >= lo && a2 < g_mirror_hi.load(std::memory_order_relaxed) && a2 != raw) {
                        static std::atomic<int> told_g{0};
                        if (told_g++ < 4) std::fprintf(stderr, "[hack] MIRROR_GFX=0: graphics table 0x%llx kept raw (mirror 0x%llx)\n",
                                                       (unsigned long long)raw, (unsigned long long)a2);
                        a2 = raw;
                    }
                }
                static const int tlog = [] { const char* v = std::getenv("LEONIDA_CE_TABLE_LOG"); return v ? std::atoi(v) : 0; }();
                if (tlog > 0 && pc == 31) {  // agA17 diagnostic: raw compute table handles
                    static std::atomic<int> tl{0};
                    if (tl++ < tlog) {
                        std::fprintf(stderr, "[hack] TABLE_LOG compute root %llu handle raw 0x%llx -> 0x%llx\n", (unsigned long long)a1,
                                     (unsigned long long)raw, (unsigned long long)a2);
                        std::fflush(stderr);
                    }
                }
                if (a2 >> 32) {  // agA17: a table the one-dword compute layout truncates
                    static std::atomic<int> told{0};
                    if (told++ < 24) {
                        std::fprintf(stderr, "[hack] table handle 0x%llx (raw 0x%llx, %s) above 4 GiB after translation\n",
                                     (unsigned long long)a2, (unsigned long long)raw, pc == 31 ? "compute" : "graphics");
                        std::fflush(stderr);
                    }
                }
            }
            if constexpr (pc >= 37 && pc <= 42) {
                const std::uint64_t raw = a2;
                a2 = xs_va_from_cpu(a2);  // HACK: root CBV/SRV/UAV addresses
                if (xs_cbv_fix_on() && a2 == raw && raw >= g_arena_size) {  // agA13: past the arena: a title pointer
                    if (const std::uint64_t to = xs_placed_buffer_va(raw, 0)) {
                        a2 = to;
                        xs_cbv_fix_note("root view in a placed buffer", raw, to);
                    } else if (const std::uint64_t sh = (pc == 37 || pc == 38) ? xs_cbv_shadow_root(raw) : 0) {
                        a2 = sh;  // agA23 LEONIDA_CE_CBV_SHADOW_ROOT: a refreshed arena shadow of the title constants
                        xs_cbv_fix_note("root CBV at a title address -> CBV shadow", raw, sh);
                    } else {
                        xs_cbv_fix_note("root view at an unknown title address (left)", raw, raw);
                    }
                }
            }
            if constexpr (pc == 25) {  // HACK diagnostic: which pipelines the title draws with
                static std::mutex seen_mu;
                static std::set<std::uint64_t> seen;
                std::lock_guard seen_lock(seen_mu);
                if (seen.size() < 40 && seen.insert(a1).second) {
                    std::string name = "?";
                    {
                        std::lock_guard names_lock(g_pso_names_mu);
                        auto it = g_pso_names.find(reinterpret_cast<const void*>(a1));
                        if (it != g_pso_names.end()) name = it->second;
                    }
                    std::fprintf(stderr, "[hack] list uses pipeline %llx: %s\n", (unsigned long long)a1, name.c_str());
                    std::fflush(stderr);
                }
            }
            float magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
            if constexpr (pc == 48) {  // HACK diagnostic: ClearRenderTargetView(rtv, color, rects, count)
                static std::atomic<int> n{0};
                const float* c = reinterpret_cast<const float*>(a2);
                if (n++ < 6 && c)
                    std::fprintf(stderr, "[hack] ClearRTV %llx color %.3f %.3f %.3f %.3f\n", (unsigned long long)a1, c[0],
                                 c[1], c[2], c[3]);
                if (std::getenv("LEONIDA_CE_MAGENTA")) a2 = reinterpret_cast<std::uint64_t>(magenta);
            }
            D3D12_INDEX_BUFFER_VIEW ibv{};
            D3D12_VERTEX_BUFFER_VIEW vbv[32];
            if constexpr (pc == 43) {
                if (a1) {
                    ibv = *reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(a1);
                    ibv.BufferLocation = xs_gpu_address(ibv.BufferLocation, ibv.SizeInBytes);
                    a1 = reinterpret_cast<std::uint64_t>(&ibv);
                }
            }
            if constexpr (pc == 44) {
                const UINT n = static_cast<UINT>(a2) > 32 ? 32 : static_cast<UINT>(a2);
                if (a3 && n) {
                    const auto* in = reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(a3);
                    for (UINT i = 0; i < n; ++i) {
                        vbv[i] = in[i];
                        vbv[i].BufferLocation = xs_gpu_address(vbv[i].BufferLocation, vbv[i].SizeInBytes);
                    }
                    a3 = reinterpret_cast<std::uint64_t>(vbv);
                }
            }
            if constexpr (pc == 49 || pc == 50) a1 = xs_gpu_handle_from_console(a1);
            if constexpr (Slot == 10) {
                a1 = unwrap_object(a1);
                self->pin_cursor_at_limit();
            } else if constexpr (Slot == kConsoleExecuteBundle) {
                a1 = unwrap_object(a1);
            } else if constexpr (Slot == kConsoleSetDescriptorHeaps) {
                if (!heaps.fill(a2, a1)) return 0;
                if (heaps.data()) a2 = heaps.data();
            }
            return reinterpret_cast<ForwardFn>(real_vtable[pc])(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9,
                                                              a10, a11, a12, a13, a14);
        }
    }
}

template <std::size_t... I>
void fill_list_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&list_slot<static_cast<unsigned>(I)>)), ...);
}

void** get_list_vtable() {
    static void** vtable = [] {
        static void* storage[kListSlots];
        fill_list_vtable(storage, std::make_index_sequence<kListSlots>{});
        return storage;
    }();
    return vtable;
}

HRESULT wrap_list(IUnknown* created, DeviceProxy* device, const IID* riid, void** out) {
    if (out) *out = nullptr;
    if (!created) return E_FAIL;
    auto* proxy = new (std::nothrow) ListProxy();
    if (!proxy) {
        created->Release();
        return E_OUTOFMEMORY;
    }
    proxy->vtable = get_list_vtable();
    proxy->real = static_cast<ID3D12GraphicsCommandList*>(created);
    proxy->device = device;
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy->pc_slot_count = detect_list_slot_count(created);
    proxy->pin_cursor_at_limit();
    proxy_add_ref(device);

    const HRESULT hr = list_query_interface(proxy, riid, out);
    if (FAILED(hr)) {
        destroy_list(proxy);
        return hr;
    }
    static std::atomic<bool> logged{false};
    if (!d3d12_trace::enabled() && (!logged.exchange(true, std::memory_order_relaxed) || trace_enabled()))
        std::fprintf(stderr, "[rt] d3d12_xs: list proxy %p wraps %p (%u PC vtable slots)\n",
                     static_cast<void*>(proxy), static_cast<void*>(created), proxy->pc_slot_count);
    proxy->refs.fetch_sub(1, std::memory_order_relaxed);
    return S_OK;
}

// The vtable read for a slot past a table faults in the reservation behind it; name the slot. Only reads the
// exception record and writes with WriteFile, so it is safe whatever the faulting thread holds.
template <ChildKind K>
void report_table_overrun(std::uintptr_t address) {
    using S = ChildStats<K>;
    const std::uintptr_t begin = S::table_begin.load(std::memory_order_acquire);
    const std::uintptr_t end = S::table_end.load(std::memory_order_acquire);
    const std::uintptr_t limit = S::region_end.load(std::memory_order_acquire);
    if (!end || address < end || address >= limit) return;
    S::overruns.fetch_add(1, std::memory_order_relaxed);
    char line[160];
    unsigned long long slot = (address - begin) / sizeof(void*);
    char digits[24];
    int d = 0;
    do {
        digits[d++] = static_cast<char>('0' + slot % 10);
        slot /= 10;
    } while (slot && d < 23);
    int n = 0;
    const auto put = [&](const char* text) {
        while (*text && n < static_cast<int>(sizeof(line)) - 1) line[n++] = *text++;
    };
    put("[rt] d3d12_xs: ");
    put(child_kind_name<K>());
    put(" slot ");
    while (d > 0 && n < static_cast<int>(sizeof(line)) - 1) line[n++] = digits[--d];
    put(" is past the proxy table; the vtable read faults\n");
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(n), &written, nullptr);
}

LONG CALLBACK table_overrun_handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
    if (record && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const auto address = static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
        report_table_overrun<ChildKind::queue>(address);
        report_table_overrun<ChildKind::fence>(address);
        report_table_overrun<ChildKind::allocator>(address);
    }
    return EXCEPTION_CONTINUE_SEARCH;  // diagnostics only: the fault goes on to the title's handlers or the crash
}

// Queue slot 31 on this title (VIctory's slot 29 plus the two console slots before Signal):
// CopyPageMappingsBatchX(count, batches, pool, poolPages, flags). Ported from Marvisak/VIctory queue.rs / xmem.rs
// (GPLv3). Maps pages of a pool (a window the title mapped physical pages into) at the batches' addresses: Grand
// Theft Auto VI streams textures into pool pages and maps them into its tiled heap this way (caller game+0xf2cc86,
// pool = its 99776-page window). Done at once on the CPU, where the D3D12 layer reads guest memory, as if every
// earlier submission had run. Each range is {kind, start, count}: 0 maps pool pages start.. at the next count pages,
// 1 maps pool page start at each of them, 2 unmaps them, 3 leaves them. Always S_OK, as VIctory does.
struct XsPageRange {
    std::uint32_t kind;
    std::uint32_t start;
    std::uint32_t count;
};
struct XsPageBatch {
    std::uint64_t destination;
    std::uint32_t count;
    const XsPageRange* ranges;
};
static_assert(sizeof(XsPageRange) == 12 && sizeof(XsPageBatch) == 24 && offsetof(XsPageBatch, ranges) == 16,
              "CopyPageMappingsBatchX structures follow VIctory's repr(C) layout");

// HACK (agA8) diagnostics: the title keeps its pool free list in globals (game+0x71bfcd8 head, +0x71bfcdc tail,
// +0x71bfce0 free count; 21-bit pool page indices, 0x1fffff = end). Read them safely for the log.
static void a8_pool_freelist(std::uint32_t (&g)[3]) {
    const void* gp = reinterpret_cast<const void*>(std::uintptr_t{0x1471bfcd8});
    g[0] = g[1] = g[2] = 0xdeadbeef;
    if (!IsBadReadPtr(gp, sizeof g)) std::memcpy(g, gp, sizeof g);
}

HRESULT copy_page_mappings_x(std::uint32_t count, const XsPageBatch* batches, std::uint64_t pool,
                             std::uint32_t pool_pages) {
    constexpr std::uint64_t kPage = 0x10000;
    static std::atomic<bool> first_call{false};
    if (!first_call.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr, "[rt] d3d12_xs: CopyPageMappingsBatchX first call: %u batch(es), pool %p, %u pool pages\n",
                     count, reinterpret_cast<void*>(pool), pool_pages);
    }
    // agA8: LEONIDA_CE_PAGECOPY_LENIENT (default 1): a range the console would turn into invalid PTEs (pool page index
    // past the pool, unknown kind, unmapped source) unmaps its destination pages instead of failing, so no stale alias
    // stays visible.
    static const bool lenient = [] { const char* v = std::getenv("LEONIDA_CE_PAGECOPY_LENIENT"); return !v || *v != '0'; }();
    static std::atomic<std::uint64_t> calls{0}, n_ranges[5]{}, n_pages[5]{}, n_fail{0};
    static std::atomic<int> fail_told{0};
    const std::uint64_t call_no = calls.fetch_add(1, std::memory_order_relaxed);
    {
        // agA8 (build 33): watch the title's slot buddy allocator (0x145238780) for a free-list head past its slot count
        // (re15: slot 0xA0000 of 0x60000 was handed out). Heads: dwords at 0x1471bfb24 + 4*order (0..21); count dword
        // 0x1471bfb18; table qword* 0x1471bfb08. Logs the first 8 bad observations with the order, head and entry.
        static std::atomic<int> bad_told{0};
        std::uint32_t heads[22] = {}, slot_count = 0;
        std::uint64_t table = 0;
        const void* hp = reinterpret_cast<const void*>(std::uintptr_t{0x1471bfb24});
        if (bad_told.load(std::memory_order_relaxed) < 8 && !IsBadReadPtr(hp, sizeof heads)) {
            std::memcpy(heads, hp, sizeof heads);
            std::memcpy(&slot_count, reinterpret_cast<const void*>(std::uintptr_t{0x1471bfb18}), 4);
            std::memcpy(&table, reinterpret_cast<const void*>(std::uintptr_t{0x1471bfb08}), 8);
            for (int o = 0; o < 22 && slot_count; ++o) {
                const std::uint32_t h = heads[o];
                if (h == 0x1fffff || h < slot_count) continue;
                if (bad_told.fetch_add(1, std::memory_order_relaxed) >= 8) break;
                std::uint64_t entry = 0;
                const void* ep = reinterpret_cast<const void*>(table + std::uint64_t{h} * 8);
                if (table && !IsBadReadPtr(ep, 8)) std::memcpy(&entry, ep, 8);
                std::fprintf(stderr, "[hack] slot buddy: order %d head 0x%x >= count 0x%x at CopyPageMappings call %llu; "
                             "table %llx entry %016llx (table+count*8 = %llx)\n", o, h, slot_count,
                             (unsigned long long)call_no, (unsigned long long)table, (unsigned long long)entry,
                             (unsigned long long)(table + std::uint64_t{slot_count} * 8));
                std::fflush(stderr);
            }
        }
    }
    const auto map = rt::xmem::map_alias_hook().load(std::memory_order_acquire);
    bool ok = map != nullptr;
    const auto tell = [&](std::uint32_t b, const XsPageBatch& batch, std::uint32_t r, const XsPageRange& range,
                          std::uint64_t at, const char* why) {
        n_fail.fetch_add(1, std::memory_order_relaxed);
        if (fail_told.fetch_add(1, std::memory_order_relaxed) >= 64) return;
        std::uint32_t g[3];
        a8_pool_freelist(g);
        std::fprintf(stderr,
                     "[hack] CopyPageMappingsBatchX call %llu batch %u/%u dest %llx range %u/%u at %llx: kind %u start "
                     "%u (0x%x) count %u; pool %llx pages %u: %s (error %lu); freelist head 0x%x tail 0x%x free %d%s\n",
                     (unsigned long long)call_no, b, count, (unsigned long long)batch.destination, r, batch.count,
                     (unsigned long long)at, range.kind, range.start, range.start, range.count,
                     (unsigned long long)pool, pool_pages, why, static_cast<unsigned long>(GetLastError()), g[0], g[1],
                     static_cast<int>(g[2]), lenient ? " -> unmapped (lenient)" : "");
        std::fflush(stderr);
    };
    if (map && count && batches) {
        for (std::uint32_t b = 0; b < count; ++b) {
            const XsPageBatch& batch = batches[b];
            if (!batch.ranges) continue;
            std::uint64_t at = batch.destination;
            for (std::uint32_t r = 0; r < batch.count; ++r) {
                const XsPageRange& range = batch.ranges[r];
                const std::uint64_t len = std::uint64_t{range.count} * kPage;
                const auto inside = [&](std::uint64_t pages) { return std::uint64_t{range.start} + pages <= pool_pages; };
                const std::uint64_t source = pool + std::uint64_t{range.start} * kPage;
                const unsigned k = range.kind < 4 ? range.kind : 4;
                n_ranges[k].fetch_add(1, std::memory_order_relaxed);
                n_pages[k].fetch_add(range.count, std::memory_order_relaxed);
                bool good = true;
                const char* why = "map_alias failed";
                SetLastError(0);
                switch (range.kind) {
                case 0:
                    if (!inside(range.count)) { good = false; why = "kind 0 range past the pool"; break; }
                    good = map(at, source, len);
                    break;
                case 1:
                    if (!inside(1)) { good = false; why = "kind 1 start past the pool"; break; }
                    for (std::uint32_t q = 0; q < range.count; ++q) good &= map(at + q * kPage, source, kPage);
                    break;
                case 2:
                    good = map(at, 0, len);
                    if (!good) why = "unmap failed";
                    break;
                case 3:
                    break;
                default:
                    good = false;
                    why = "unknown kind";
                    break;
                }
                if (!good) {
                    tell(b, batch, r, range, at, why);
                    if (lenient && map(at, 0, len)) good = true;
                }
                ok &= good;
                at += len;
            }
        }
    }
    if ((call_no & 1023) == 1023 || (call_no < 4096 && (call_no & (call_no + 1)) == 0)) {
        std::uint32_t g[3];
        a8_pool_freelist(g);
        std::fprintf(stderr,
                     "[hack] CopyPageMappingsBatchX stats after %llu calls: ranges/pages kind0 %llu/%llu kind1 %llu/%llu "
                     "kind2 %llu/%llu kind3 %llu/%llu other %llu/%llu; failed ranges %llu; freelist head 0x%x tail 0x%x "
                     "free %d\n",
                     (unsigned long long)(call_no + 1), (unsigned long long)n_ranges[0].load(),
                     (unsigned long long)n_pages[0].load(), (unsigned long long)n_ranges[1].load(),
                     (unsigned long long)n_pages[1].load(), (unsigned long long)n_ranges[2].load(),
                     (unsigned long long)n_pages[2].load(), (unsigned long long)n_ranges[3].load(),
                     (unsigned long long)n_pages[3].load(), (unsigned long long)n_ranges[4].load(),
                     (unsigned long long)n_pages[4].load(), (unsigned long long)n_fail.load(), g[0], g[1],
                     static_cast<int>(g[2]));
        std::fflush(stderr);
    }
    if (!ok) {
        static std::atomic<bool> failure_logged{false};
        if (!failure_logged.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr, "[rt] d3d12_xs: CopyPageMappingsBatchX: a page mapping failed (pool %p, error %lu%s)\n",
                         reinterpret_cast<void*>(pool), static_cast<unsigned long>(GetLastError()),
                         map ? "" : "; xmem.cpp's map_alias hook is not linked in");
        }
    }
    return S_OK;
}

template <ChildKind K, unsigned Slot>
std::uint64_t WINAPI child_slot(ChildProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                                std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                                std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if constexpr (Slot == 0) {
        d3d12_trace::record(child_trace_name<K>(Slot));
        return hr_to_u64(child_query_interface<K>(self, reinterpret_cast<const IID*>(a1),
                                                  reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return child_add_ref<K>(self);
    } else if constexpr (Slot == 2) {
        return child_release<K>(self);
    } else if constexpr (Slot == 7) {
        d3d12_trace::record(child_trace_name<K>(Slot));
        return hr_to_u64(child_get_device<K>(self, reinterpret_cast<const IID*>(a1),
                                             reinterpret_cast<void**>(a2)));
    } else {
        note_child_slot<K>(Slot);
        // HACK: the console queue runs two slots ahead of the PC one past Wait: the title calls slot 18 with one
        // UINT64* (GetTimestampFrequency; PC 16). Forwarding it to PC slot 18 (GetDesc) wrote a 16-byte struct
        // over the caller's frame and corrupted its saved rbx.
        if constexpr (K == ChildKind::queue && Slot == 32) if (xs_title_loaded()) {
            // HACK: PresentX(planeCount, planes, params) on this title (newer runtime layout, slot 32). No window yet:
            // accept the frame and count it. Plane: Token @0, ResourceCount @8, ppResources @0x10 (xbox_present.h).
            static std::atomic<std::uint64_t> frames{0};
            const std::uint64_t f = frames++;
            if (f < 5 || (f & (f - 1)) == 0) {
                const auto* plane = reinterpret_cast<const std::uint8_t*>(a2);
                const std::uint64_t token = plane ? *reinterpret_cast<const std::uint64_t*>(plane) : 0;
                const unsigned count = plane ? *reinterpret_cast<const unsigned*>(plane + 8) : 0;
                void* const* res = plane ? *reinterpret_cast<void* const* const*>(plane + 0x10) : nullptr;
                std::fprintf(stderr, "[hack] PresentX frame %llu: planes=%llu token=%llu resources=%u res0=%p\n",
                             (unsigned long long)f, (unsigned long long)a1, (unsigned long long)token, count,
                             res && count ? res[0] : nullptr);
                std::fflush(stderr);
            }
            if (f % 30 == 0 && self->device && self->device->real) xs_refresh_placed_textures(self->device->real);
            xs_trap_tick(f);
            {   // HACK (Leonida CE boot): frame pacing. Presents were uncapped (about 240 per second; the frame events
                // answer at once), while the title runs at 30 on the console and frees things a set number of frames
                // later. LEONIDA_CE_FPS (30 by default, 0 for none) spaces presents at least 1/FPS apart.
                static const double fps = [] {
                    char v[16] = {};
                    return GetEnvironmentVariableA("LEONIDA_CE_FPS", v, sizeof(v)) ? std::strtod(v, nullptr) : 30.0;
                }();
                if (fps > 0) {
                    static LARGE_INTEGER freq{}, next{};
                    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
                    const LONGLONG period = static_cast<LONGLONG>(freq.QuadPart / fps);
                    LARGE_INTEGER now{};
                    QueryPerformanceCounter(&now);
                    if (!next.QuadPart || now.QuadPart - next.QuadPart > period * 4) next = now;
                    if (now.QuadPart < next.QuadPart) {
                        const DWORD ms = static_cast<DWORD>((next.QuadPart - now.QuadPart) * 1000 / freq.QuadPart);
                        if (ms) Sleep(ms);
                        do { QueryPerformanceCounter(&now); } while (now.QuadPart < next.QuadPart);
                    }
                    next.QuadPart += period;
                }
            }
            {   // HACK (Leonida CE): the boot's profile load waits for an ACTIVE PAD (dword 0x146C31B50, -1 until a
                // controller is associated with the signed-in user; set only from the profile manager's event handler
                // 0x142206150 -> 0x142207680 -> 0x143DCA480). With no controller (VM, Wine) it never is, and the frontend
                // never shows its first screen. Once sign-in is done (frame 60 on), make pad 0 active: the pad index,
                // the profile manager's ready byte 0x1498489C0, and bit 0x02 of pad 0's entry (0x1498482B8 + 0x144).
                // Found by static analysis of the boot state machine (0x141D83960). LEONIDA_CE_NO_PAD_HACK=1: off.
                // OFF by default (LEONIDA_CE_PAD_HACK=1 turns it on): the byte 0x1498489C0 it sets is the online
                // system's "already initialized" flag, not a profile-ready flag. Setting it before 0x143DA1560 runs makes
                // that init fail and shut the system down, leaving a dangling singleton (0x1498C0F20) that the boot task
                // 0xB198EC07 then crashes on (swallowed): the boot state machine never started (subagent, Definite).
                static const bool no_pad_hack = [] {
                    char v[8] = {};
                    return !(GetEnvironmentVariableA("LEONIDA_CE_PAD_HACK", v, sizeof(v)) && v[0] == '1');
                }();
                auto* const pad = reinterpret_cast<volatile std::int32_t*>(0x146C31B50ull);
                if (!no_pad_hack && f >= 60 && xs_readable(const_cast<std::int32_t*>(pad), 4) && *pad == -1) {
                    auto* const ready = reinterpret_cast<volatile std::uint8_t*>(0x1498489C0ull);
                    auto* const entry = reinterpret_cast<volatile std::uint8_t*>(0x1498482B8ull + 0x144);
                    if (xs_readable(const_cast<std::uint8_t*>(ready), 1) && xs_readable(const_cast<std::uint8_t*>(entry), 1)) {
                        *pad = 0;
                        *ready = 1;
                        *entry = static_cast<std::uint8_t>(*entry | 0x02);
                        std::fprintf(stderr, "[hack] no controller: made pad 0 the active pad at present %llu\n",
                                     static_cast<unsigned long long>(f));
                        std::fflush(stderr);
                    }
                }
            }
            static const bool dump_stacks = [] {
                char v[8] = {};
                return GetEnvironmentVariableA("LEONIDA_CE_STACKS", v, sizeof(v)) > 0 && v[0] == '1';
            }();
            if (dump_stacks && f % 300 == 0) {
                struct Arg { std::uint64_t f; };
                HANDLE t = CreateThread(nullptr, 0, [](void* a) -> DWORD {
                    const std::uint64_t frame = static_cast<Arg*>(a)->f;
                    delete static_cast<Arg*>(a);
                    xs_tls_sweep(frame);
                    return 0;
                }, new Arg{f}, 0, nullptr);
                if (t) CloseHandle(t);
            }
            static const std::vector<std::uint64_t> stacks_at = [] {  // agA5: LEONIDA_CE_STACKS_AT=2400,3000
                std::vector<std::uint64_t> v;
                char b[256] = {};
                if (GetEnvironmentVariableA("LEONIDA_CE_STACKS_AT", b, sizeof(b)) > 0)
                    for (char* p = b; *p;) { v.push_back(std::strtoull(p, &p, 10)); while (*p == ',' || *p == ' ') ++p; }
                return v;
            }();
            if (std::find(stacks_at.begin(), stacks_at.end(), f) != stacks_at.end()) {
                HANDLE t = CreateThread(nullptr, 0, [](void*) -> DWORD { xs_dump_thread_stacks(); return 0; }, nullptr, 0, nullptr);
                if (t) CloseHandle(t);
            }
            if (dump_stacks && (f == 300 || f == 4000 || f == 20000)) {
                HANDLE t = CreateThread(nullptr, 0, [](void*) -> DWORD { xs_dump_thread_stacks(); return 0; }, nullptr, 0, nullptr);
                if (t) CloseHandle(t);
            }
            {   // HACK: capture frames 0, 1, 2, 4, ... and every 2000th
                const bool pow2 = (f & (f - 1)) == 0;
                const auto* plane = reinterpret_cast<const std::uint8_t*>(a2);
                const unsigned count = plane ? *reinterpret_cast<const unsigned*>(plane + 8) : 0;
                void* const* res = plane ? *reinterpret_cast<void* const* const*>(plane + 0x10) : nullptr;
                hang_heartbeat(f);  // agA3
                if (leonida::rtdiag::on())  // agD: LEONIDA_CE_RTDIAG (at_present picks the frames; agA5)
                    leonida::rtdiag::at_present(static_cast<ID3D12CommandQueue*>(static_cast<IUnknown*>(self->real)),
                                                count && res ? static_cast<ID3D12Resource*>(res[0]) : nullptr, f);
                if ((pow2 || f % 2000 == 0) && count && res && res[0])
                    xs_capture_frame(static_cast<ID3D12CommandQueue*>(static_cast<IUnknown*>(self->real)),
                                     static_cast<ID3D12Resource*>(res[0]), f);
                static const unsigned preview_every = [] {
                    char v[16] = {};
                    return GetEnvironmentVariableA("LEONIDA_CE_PREVIEW", v, sizeof(v))
                               ? static_cast<unsigned>(std::strtoul(v, nullptr, 10)) : 0u;
                }();
                if (preview_every && f % preview_every == 0 && count && res && res[0])
                    xs_capture_frame(static_cast<ID3D12CommandQueue*>(static_cast<IUnknown*>(self->real)),
                                     static_cast<ID3D12Resource*>(res[0]), f, true);
            }
            return hr_to_u64(S_OK);
        }
        if constexpr (K == ChildKind::queue && Slot == 35) if (xs_title_loaded()) {
            // HACK: console ExecuteCommandLists(count, lists, extra, flags) (PC slot 10 takes count, lists).
            auto* q = static_cast<ID3D12CommandQueue*>(static_cast<IUnknown*>(self->real));
            const UINT n = static_cast<UINT>(a1);
            auto* const* lists = reinterpret_cast<ID3D12CommandList* const*>(a2);
            static std::atomic<std::uint64_t> batches{0};
            const std::uint64_t b = batches++;
            if (b < 4 || (b & (b - 1)) == 0) {
                std::fprintf(stderr, "[hack] ExecuteCommandListsX #%llu: %u list(s), extra=%llx flags=%llx\n",
                             (unsigned long long)b, n, (unsigned long long)a3, (unsigned long long)a4);
                std::fflush(stderr);
            }
            // The lists are console-list proxies: unwrap them as slot 10 does.
            UnwrappedArray console_lists;
            if (!console_lists.fill(a2, a1)) return hr_to_u64(E_OUTOFMEMORY);
            auto* const* real_lists = console_lists.data()
                ? reinterpret_cast<ID3D12CommandList* const*>(console_lists.data()) : lists;
            if (!g_upload_queue.load() && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) g_upload_queue = q;
            if (n && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) xs_refresh_placed_buffers(q);
            if (n) xs_cbv_shadow_refresh();  // fspx CB analysis: every queue (the fspx passes also run on compute)
            if (n && real_lists) q->ExecuteCommandLists(n, real_lists);
            if (n && lists) xs_schedule_list_writes(q, n, reinterpret_cast<const std::uint64_t*>(lists));
            return 0;
        }
        // This title's queue runs two slots ahead of the PC one from Signal on: 16 = Signal(fence, value) (measured:
        // a fence proxy and 1, 2, ...), 17 = Wait, 18 = GetTimestampFrequency, 19 = GetClockCalibration. Forwarding 16
        // to PC 16 (GetTimestampFrequency) wrote the frequency over the fence proxy and never signalled the fence.
        if constexpr (K == ChildKind::fence && Slot == 8) {  // HACK diagnostic: who polls which fence, and what it sees
            auto* fence = static_cast<ID3D12Fence*>(static_cast<IUnknown*>(self->real));
            const std::uint64_t value = fence->GetCompletedValue();
            static std::atomic<std::uint64_t> calls{0};
            const std::uint64_t c = ++calls;
            if (c == 1000 || c == 100000 || c == 1000000 || c == 5000000) {
                std::fprintf(stderr, "[hack] GetCompletedValue call %llu: fence %p value %llu caller %p\n",
                             (unsigned long long)c, (void*)self, (unsigned long long)value, _ReturnAddress());
                std::fflush(stderr);
            }
            return value;
        }
        if constexpr (K == ChildKind::fence && Slot == 12) if (xs_title_loaded()) {
            auto* fence = static_cast<ID3D12Fence*>(static_cast<IUnknown*>(self->real));
            return reinterpret_cast<std::uint64_t>(xs_fence_value_address(self, fence));
        }
        if constexpr (K == ChildKind::queue && Slot == 31) if (xs_title_loaded()) {
            d3d12_trace::record(child_trace_name<K>(Slot));
            return hr_to_u64(copy_page_mappings_x(static_cast<std::uint32_t>(a1),
                                                  reinterpret_cast<const XsPageBatch*>(a2), a3,
                                                  static_cast<std::uint32_t>(a4)));
        }
        if constexpr (K == ChildKind::queue && (Slot == 16 || Slot == 17)) if (xs_title_loaded()) {
            auto* q = static_cast<ID3D12CommandQueue*>(static_cast<IUnknown*>(self->real));
            auto* fence = reinterpret_cast<ID3D12Fence*>(unwrap_object(a1));
            return hr_to_u64(Slot == 16 ? q->Signal(fence, a2) : q->Wait(fence, a2));
        }
        if constexpr (K == ChildKind::queue && Slot == 18) if (xs_title_loaded()) {
            auto* q = static_cast<ID3D12CommandQueue*>(static_cast<IUnknown*>(self->real));
            return hr_to_u64(q->GetTimestampFrequency(reinterpret_cast<UINT64*>(a1)));
        }
        if constexpr (K == ChildKind::queue && Slot == 19) if (xs_title_loaded()) {
            auto* q = static_cast<ID3D12CommandQueue*>(static_cast<IUnknown*>(self->real));
            return hr_to_u64(q->GetClockCalibration(reinterpret_cast<UINT64*>(a1), reinterpret_cast<UINT64*>(a2)));
        }
        // HACK: the title calls command-allocator slot 9 in a tight loop over its blocks (a3 advances by 0x30).
        // The PC allocator has no such method, so E_NOTIMPL made it retry forever (millions of calls). Answer S_OK
        // and it moves on.
        if constexpr (K == ChildKind::allocator && Slot == 9) {
            static std::atomic<bool> told{false};
            if (!told.exchange(true)) {
                std::fprintf(stderr, "[hack] allocator slot 9 answered S_OK (stub)\n");
                std::fflush(stderr);
            }
            return hr_to_u64(S_OK);
        }
        if (Slot >= self->slot_count) {
            bool expected = false;
            if (ChildStats<K>::beyond_warned[Slot].compare_exchange_strong(expected, true,
                                                                            std::memory_order_relaxed)) {
                std::fprintf(stderr,
                             "[rt] d3d12_xs: %s slot %u is beyond the PC %s vtable (%u slots); returning E_NOTIMPL "
                             "(args 0x%llx 0x%llx 0x%llx 0x%llx)\n",
                             child_kind_name<K>(), Slot, child_kind_name<K>(), self->slot_count,
                             static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                             static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4));
            }
            d3d12_trace::record(child_trace_name<K>(Slot), "unsupported");
            // Logged a handful of times only: an unbounded title loop calls this millions of times.
            static std::atomic<int> beyond_calls{0};
            if (beyond_calls.fetch_add(1) < 20) {
                std::fprintf(stderr, "[hack] %s slot %u beyond PC: caller %p a1=%llx a2=%llx a3=%llx a4=%llx\n",
                             child_kind_name<K>(), Slot, _ReturnAddress(), (unsigned long long)a1,
                             (unsigned long long)a2, (unsigned long long)a3, (unsigned long long)a4);
                std::fflush(stderr);
            }
            return hr_to_u64(E_NOTIMPL);
        }
        d3d12_trace::record(child_trace_name<K>(Slot));  // "called": the real object's result is not inspected
        if constexpr (K == ChildKind::queue && (Slot == kQueueSignal || Slot == kQueueWait)) a1 = unwrap_object(a1);
        UnwrappedArray unwrapped_lists;
        if constexpr (K == ChildKind::queue && Slot == 10) {
            if (a1) xs_cbv_shadow_refresh();  // fspx CB analysis: PC-slot ExecuteCommandLists as well
            if (!unwrapped_lists.fill(a2, a1)) return hr_to_u64(E_OUTOFMEMORY);
            if (unwrapped_lists.data()) a2 = unwrapped_lists.data();
        }

        // The PC vtable entry with the real object as `this`; integer/pointer arguments, the hidden struct-return
        // pointer and stack arguments keep their positions, as in the device proxy.
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t);
        auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot]);
        const std::uint64_t r_ = fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
        if (static_cast<std::int32_t>(r_) < 0 && (static_cast<std::uint32_t>(r_) >> 16) >= 0x8000u) {  // HACK diagnostic
            static std::atomic<int> n_{0};
            if (n_++ < 20) { std::fprintf(stderr, "[hack] %s slot %u failed 0x%08x caller %p\n", __FUNCTION__, Slot,
                                          static_cast<unsigned>(r_), _ReturnAddress()); std::fflush(stderr); }
        }
        return r_;
    }
}

template <ChildKind K, std::size_t... I>
void fill_child_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&child_slot<K, static_cast<unsigned>(I)>)), ...);
}

// True when the fault logger is installed (once per process; a failure is logged once).
bool install_table_overrun_handler() {
    static const bool installed = [] {
        if (AddVectoredExceptionHandler(0, &table_overrun_handler)) return true;
        std::fprintf(stderr, "[rt] d3d12_xs: AddVectoredExceptionHandler failed (error %lu); a slot past a queue or "
                     "fence proxy table faults without being named\n", GetLastError());
        return false;
    }();
    return installed;
}

template <ChildKind K>
void** get_child_vtable() {
    static void** vtable = [] {
        using S = ChildStats<K>;
        constexpr std::size_t bytes = S::kSlots * sizeof(void*);
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const SIZE_T page = system.dwPageSize;
        if (void* base = page >= bytes ? VirtualAlloc(nullptr, kTableRegion, MEM_RESERVE, PAGE_NOACCESS) : nullptr) {
            if (VirtualAlloc(base, page, MEM_COMMIT, PAGE_READWRITE)) {
                auto* table = reinterpret_cast<void**>(static_cast<char*>(base) + page - bytes);
                fill_child_vtable<K>(table, std::make_index_sequence<S::kSlots>{});
                DWORD old = 0;
                if (!VirtualProtect(base, page, PAGE_READONLY, &old))  // the table works, but stays writable
                    std::fprintf(stderr, "[rt] d3d12_xs: %s proxy table stays writable (VirtualProtect error %lu)\n",
                                 child_kind_name<K>(), GetLastError());
                S::table_begin.store(reinterpret_cast<std::uintptr_t>(table), std::memory_order_release);
                S::region_end.store(reinterpret_cast<std::uintptr_t>(base) + kTableRegion, std::memory_order_release);
                // table_end is what "guarded" means (command_object_tables_guarded, the fault logger): set only when
                // a fault past the table will be named. Without the logger the region still faults, unnamed.
                if (install_table_overrun_handler())
                    S::table_end.store(reinterpret_cast<std::uintptr_t>(base) + page, std::memory_order_release);
                return table;
            }
            VirtualFree(base, 0, MEM_RELEASE);
        }
        // No reservation: the table still works, but a slot past it is not caught.
        std::fprintf(stderr, "[rt] d3d12_xs: %s proxy table without a guard region\n", child_kind_name<K>());
        static void* storage[S::kSlots];
        fill_child_vtable<K>(storage, std::make_index_sequence<S::kSlots>{});
        return storage;
    }();
    return vtable;
}

// Wraps `created` (one reference, owned by the proxy on success, released on failure) and returns `riid` from the
// proxy through `out`.
template <ChildKind K>
HRESULT wrap_child(IUnknown* created, DeviceProxy* device, const IID* riid, void** out) {
    if (!created) return E_FAIL;
    ChildProxy* proxy = new (std::nothrow) ChildProxy;
    if (!proxy) {
        created->Release();
        return E_OUTOFMEMORY;
    }
    proxy->vtable = get_child_vtable<K>();
    proxy->real = created;
    proxy->device = device;
    proxy->real_vtable = *reinterpret_cast<void***>(created);
    if constexpr (K == ChildKind::queue) {
        proxy->slot_count = kQueuePublicSlots;
    } else if constexpr (K == ChildKind::fence) {
        proxy->slot_count = supports_iid(created, __uuidof(ID3D12Fence1)) ? kFence1PublicSlots : kFencePublicSlots;
    } else {
        proxy->slot_count = kAllocatorPublicSlots;
    }
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy_add_ref(device);  // the object keeps its Xbox-ABI device alive for GetDevice

    const HRESULT hr = child_query_interface<K>(proxy, riid, out);
    if (FAILED(hr)) {
        destroy_child(proxy);  // no reference escaped
        return hr;
    }
    // Titles create fences at run time: the first proxy of each kind is logged, every one only with XS_TRACE.
    static std::atomic<bool> logged{false};
    if (!d3d12_trace::enabled() && (!logged.exchange(true, std::memory_order_relaxed) || trace_enabled()))
        std::fprintf(stderr, "[rt] d3d12_xs: %s proxy %p wraps %p (device proxy %p, %u public slots)\n",
                     child_kind_name<K>(), static_cast<void*>(proxy), static_cast<void*>(created),
                     static_cast<void*>(device), proxy->slot_count);
    proxy->refs.fetch_sub(1, std::memory_order_relaxed);  // drop the creation reference; `out` holds the caller's
    return S_OK;
}

// Calls the real device's slot `Slot` with these five words in the positions the thunk received them (see "A thunk
// declares 14 argument words" at the top: no word may move to another stack position on the way).
template <unsigned Slot>
std::uint64_t forward_device(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                             std::uint64_t a4, std::uint64_t a5) {
    using Fn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t);
    return reinterpret_cast<Fn>(self->real_vtable[Slot])(self->real, a1, a2, a3, a4, a5);
}

// A device slot past the PC device's vtable (the real device predates the interface): E_NOTIMPL, logged once per
// slot. The generic thunk and the slots the proxy handles itself (31/34/35/45/46/50, 75) log the same line.
HRESULT refuse_beyond_device(unsigned slot, unsigned slot_count) {
    bool expected = false;
    if (slot < kMaxSlots && g_beyond_warned[slot].compare_exchange_strong(expected, true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: device slot %u (%s) is beyond the PC device vtable (%u slots); returning "
                     "E_NOTIMPL\n",
                     slot, slot_name(slot), slot_count);
    return E_NOTIMPL;
}

// Device slot 8: CreateCommandQueue(desc, riid, out). Without an output it is the device's own capability answer
// (S_FALSE for a valid description on the PC), as #558 does for slot 14.
HRESULT proxy_create_command_queue(DeviceProxy* self, const D3D12_COMMAND_QUEUE_DESC* desc, const IID* riid,
                                   void** out) {
    note_slot(8);
    if (out) *out = nullptr;  // _COM_Outptr_: NULL on every failure, as slots 14/62/76
    if (!riid) return E_POINTER;
    // The caller's null output goes to the real device on purpose: its answer (S_FALSE, E_UNEXPECTED, ...) is the
    // one to give, although the SDK annotates the parameter as required.
#pragma warning(suppress : 6387)
    if (!out) return self->real->CreateCommandQueue(desc, *riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = self->real->CreateCommandQueue(desc, __uuidof(ID3D12CommandQueue), &created);
    if (FAILED(hr)) return hr;
    return wrap_child<ChildKind::queue>(static_cast<IUnknown*>(created), self, riid, out);
}

// Xbox device slot 78 (public HLE): SetFrameIntervalX(output, interval_us, latency, flags). The output and flags are
// not used before a presenter exists. The period is a vblank period in microseconds in the public samples (16667 for
// 60 Hz), and latency is the allowed frames in flight. S_OK even for 0: a boot must not abort here, and nothing paces.
HRESULT proxy_set_frame_interval_x(DeviceProxy* self, std::uint64_t output, std::uint64_t interval_us,
                                   std::uint64_t latency, std::uint64_t flags) {
    (void)output;
    (void)flags;
    self->frame_interval_us.store(static_cast<std::uint32_t>(interval_us), std::memory_order_relaxed);
    self->frame_latency.store(static_cast<std::uint32_t>(latency ? latency : 1), std::memory_order_relaxed);
    return S_OK;
}

// Xbox device slot 79 (public HLE): ScheduleFrameEventX(event, offset_us, pEvent, flags); our pacer has only the origin.
HRESULT proxy_schedule_frame_event_x(DeviceProxy* self, std::uint64_t event, std::uint64_t offset_us,
                                     std::uint64_t flags) {
    (void)event;
    (void)offset_us;
    (void)flags;
    self->frame_event_scheduled.store(true, std::memory_order_relaxed);
    return S_OK;
}

// Xbox device slot 80 (public HLE): WaitFrameEventX(event, timeout_ms, pEvent, flags, &token). It returns at once with
// a fresh non-zero token (the first-frame choice): there is no vblank source behind the device before a presenter, and
// a zero wait cannot stall the title. A null token pointer is E_POINTER, as a _Out_ parameter.
HRESULT proxy_wait_frame_event_x(DeviceProxy* self, std::uint64_t event, std::uint64_t timeout_ms,
                                 std::uint64_t flags, std::uint64_t token_word) {
    (void)event;
    (void)timeout_ms;
    (void)flags;
    auto* token = reinterpret_cast<std::uint64_t*>(token_word);
    if (!token) return E_POINTER;
    std::uint64_t value = self->frame_next_token.fetch_add(1, std::memory_order_relaxed);
    if (value == 0) value = self->frame_next_token.fetch_add(1, std::memory_order_relaxed);  // skip the null token
    *token = value;
    return S_OK;
}


// Xbox device slot 73 (public HLE): CreateCommandListX(desc, allocator, initial_state, riid, out). The console call is
// not the PC six-argument form: the type is the first word of D3D12XBOX_COMMAND_LIST_DESC and there is no node mask
// (the HLE passes 0). The second desc word is logged, not interpreted: its public layout is not established.
HRESULT proxy_create_command_list_x(DeviceProxy* self, std::uint64_t desc_word, std::uint64_t allocator_word,
                                    std::uint64_t initial_state_word, const IID* riid, void** out) {
    if (out) *out = nullptr;
    if (!desc_word || !allocator_word) return E_INVALIDARG;
    if (!riid) return E_POINTER;
    if (!out) return E_POINTER;

    std::uint32_t words[2] = {};
    std::memcpy(words, reinterpret_cast<const void*>(desc_word), sizeof(words));
    auto type = static_cast<D3D12_COMMAND_LIST_TYPE>(words[0]);
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_BUNDLE &&
        type != D3D12_COMMAND_LIST_TYPE_COMPUTE && type != D3D12_COMMAND_LIST_TYPE_COPY)
        type = D3D12_COMMAND_LIST_TYPE_DIRECT;  // as the public HLE does

    static std::atomic<bool> logged{false};
    if (first_call(logged))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 73 CreateCommandListX desc words 0x%08x 0x%08x -> node mask 0, type %d\n",
                     words[0], words[1], static_cast<int>(type));

    // Leonida wraps its Vulkan allocators; a PC allocator comes back unwrapped and is passed as it is. Pipeline states
    // are not wrapped by this proxy, so initial_state is already the real object (a proxy there is passed through too).
    auto* allocator = allocator_proxy_real_allocator(reinterpret_cast<void*>(allocator_word));
    if (!allocator) allocator = reinterpret_cast<ID3D12CommandAllocator*>(allocator_word);
    auto* initial_state = reinterpret_cast<ID3D12PipelineState*>(initial_state_word);
    // HACK (boot-hacks): the 1.0.2.1 title asks for c3827890-4024-474c-98cf-645b51dc4874 (a console graphics-list IID,
    // same Data1 as PC ID3D12GraphicsCommandList6); the list proxy only answers PC list IIDs, so create and wrap the
    // list as ID3D12GraphicsCommandList.
    static const GUID kXboxGraphicsList = {0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};
    const IID* create_iid = riid;
    if (IsEqualGUID(*riid, kXboxGraphicsList)) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true))
            std::fprintf(stderr, "[hack] CreateCommandListX: console list IID -> ID3D12GraphicsCommandList\n");
        create_iid = &__uuidof(ID3D12GraphicsCommandList);
    }
    void* created = nullptr;
    const HRESULT hr = self->real->CreateCommandList(0, type, allocator, initial_state,
                                                     *create_iid, &created);
    if (FAILED(hr)) return hr;
    return wrap_list(static_cast<IUnknown*>(created), self, create_iid, out);
}

// Device slot 75: ID3D12Device9::CreateCommandQueue1(desc, creator_id, riid, out), called by slot index.
HRESULT proxy_create_command_queue1(DeviceProxy* self, std::uint64_t desc, std::uint64_t creator_id,
                                    const IID* riid, void** out) {
    note_slot(75);
    if (out) *out = nullptr;  // as slot 8
    if (self->slot_count <= 75) return refuse_beyond_device(75, self->slot_count);  // predates ID3D12Device9
    if (!riid) return E_POINTER;
    using Fn = HRESULT(WINAPI*)(ID3D12Device*, std::uint64_t, std::uint64_t, const IID*, void**);
    const auto create = reinterpret_cast<Fn>(self->real_vtable[75]);
    if (!out) return create(self->real, desc, creator_id, riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = create(self->real, desc, creator_id, &__uuidof(ID3D12CommandQueue), &created);
    if (FAILED(hr)) return hr;
    return wrap_child<ChildKind::queue>(static_cast<IUnknown*>(created), self, riid, out);
}

// Device slot 36: CreateFence(initial_value, flags, riid, out).
HRESULT proxy_create_fence(DeviceProxy* self, std::uint64_t initial_value, std::uint64_t flags, const IID* riid,
                           void** out) {
    note_slot(36);
    if (out) *out = nullptr;  // as slot 8
    if (!riid) return E_POINTER;
    const auto fence_flags = static_cast<D3D12_FENCE_FLAGS>(static_cast<std::uint32_t>(flags));
#pragma warning(suppress : 6387)  // the caller's null output, on purpose (see slot 8)
    if (!out) return self->real->CreateFence(initial_value, fence_flags, *riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = self->real->CreateFence(initial_value, fence_flags, __uuidof(ID3D12Fence), &created);
    if (FAILED(hr)) return hr;
    const HRESULT wrapped = wrap_child<ChildKind::fence>(static_cast<IUnknown*>(created), self, riid, out);
    static std::atomic<int> n{0};  // HACK diagnostic
    if (n++ < 12) { std::fprintf(stderr, "[hack] fence proxy %p (flags %llu)\n", *out, (unsigned long long)flags); std::fflush(stderr); }
    return wrapped;
}

// True when `allocator` is one whose consumers resolve allocator proxies: Leonida's own Vulkan-layer allocator,
// whose lists (CreateCommandList, Reset) find the real allocator through allocator_proxy_real_allocator. A PC
// runtime allocator does not answer the tag, and its lists would take a proxy for their own allocator.
bool resolves_allocator_proxies(IUnknown* allocator) {
    void* found = nullptr;
    if (FAILED(allocator->QueryInterface(kLeonidaCommandAllocatorTag, &found)) || !found) return false;
    static_cast<IUnknown*>(found)->Release();  // the implementation pointer starts with its IUnknown chain
    return true;
}

// Device slot 9: CreateCommandAllocator(type, riid, out). An allocator proxy where the list side resolves it (see
// resolves_allocator_proxies); any other allocator goes back unchanged, logged once, so the host runtime's lists
// never see a proxy. Null output and null IID as slot 8.
HRESULT proxy_create_command_allocator(DeviceProxy* self, std::uint64_t type, const IID* riid, void** out) {
    note_slot(9);
    if (out) *out = nullptr;  // as slot 8
    if (!riid) return E_POINTER;
    const auto list_type = static_cast<D3D12_COMMAND_LIST_TYPE>(static_cast<std::uint32_t>(type));
#pragma warning(suppress : 6387)  // the caller's null output, on purpose (see slot 8)
    if (!out) return self->real->CreateCommandAllocator(list_type, *riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = self->real->CreateCommandAllocator(list_type, __uuidof(ID3D12CommandAllocator), &created);
    if (FAILED(hr)) return hr;
    if (!created) return E_FAIL;
    auto* allocator = static_cast<IUnknown*>(created);
    if (!resolves_allocator_proxies(allocator)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: allocators of this device are not wrapped: its command lists "
                                 "cannot resolve an allocator proxy\n");
        const HRESULT asked = allocator->QueryInterface(*riid, out);
        allocator->Release();
        return asked;
    }
    return wrap_child<ChildKind::allocator>(allocator, self, riid, out);
}

// Device slot 32: OpenSharedHandle(handle, riid, out). The object is opened as asked; when it is a fence (whatever
// interface was asked for, IUnknown included) the title gets a fence proxy, anything else (heaps, resources) is
// returned unchanged.
HRESULT proxy_open_shared_handle(DeviceProxy* self, std::uint64_t handle, const IID* riid, void** out) {
    note_slot(32);
    if (out) *out = nullptr;  // as slot 8
    if (!riid) return E_POINTER;
    const auto shared = reinterpret_cast<HANDLE>(handle);
    if (!out) return self->real->OpenSharedHandle(shared, *riid, nullptr);
    void* opened = nullptr;
    HRESULT hr = self->real->OpenSharedHandle(shared, *riid, &opened);
    if (FAILED(hr) || !opened) return FAILED(hr) ? hr : E_FAIL;
    void* fence = nullptr;
    if (FAILED(static_cast<IUnknown*>(opened)->QueryInterface(__uuidof(ID3D12Fence), &fence)) || !fence) {
        *out = opened;  // not a fence
        return hr;
    }
    static_cast<IUnknown*>(opened)->Release();
    return wrap_child<ChildKind::fence>(static_cast<IUnknown*>(fence), self, riid, out);
}

// Device slots that take heap, queue or fence objects: the real runtime gets the real objects.
//   31 CreateSharedHandle(object, attributes, access, name, handle)
//   34 MakeResident(count, objects)      35 Evict(count, objects)
//   45 SetEventOnMultipleFenceCompletion(fences, values, count, flags, event)            (ID3D12Device1)
//   46 SetResidencyPriority(count, objects, priorities)                                  (ID3D12Device1)
//   50 EnqueueMakeResident(flags, count, objects, fence, value)                          (ID3D12Device3)
template <unsigned Slot>
HRESULT proxy_unwrapping_call(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                              std::uint64_t a4, std::uint64_t a5) {
    note_slot(Slot);
    if (Slot >= self->slot_count) return refuse_beyond_device(Slot, self->slot_count);  // predates the interface
    UnwrappedArray objects;
    std::uint64_t r1 = a1, r2 = a2, r3 = a3, r4 = a4;
    if constexpr (Slot == 31) {
        r1 = unwrap_object(a1);
    } else if constexpr (Slot == 34 || Slot == 35 || Slot == 46) {
        if (!objects.fill(a2, a1)) return E_OUTOFMEMORY;
        if (objects.data()) r2 = objects.data();
    } else if constexpr (Slot == 45) {
        if (!objects.fill(a1, a3)) return E_OUTOFMEMORY;
        if (objects.data()) r1 = objects.data();
    } else if constexpr (Slot == 50) {
        if (!objects.fill(a3, a2)) return E_OUTOFMEMORY;
        if (objects.data()) r3 = objects.data();
        r4 = unwrap_object(a4);
    }
    // r1-r4 differ from a1-a4 only in a position the method takes, so the title's frame has that word.
    return static_cast<HRESULT>(static_cast<std::uint32_t>(forward_device<Slot>(self, r1, r2, r3, r4, a5)));
}

static bool readable_range(const void* ptr, std::size_t bytes) {
    if (!ptr || !bytes) return false;
    const auto* p = static_cast<const std::uint8_t*>(ptr);
    std::size_t remaining = bytes;
    while (remaining) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
        const DWORD protect = mbi.Protect & 0xffu;
        if (protect == PAGE_NOACCESS || protect == PAGE_GUARD || protect == PAGE_EXECUTE) return false;
        const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto addr = reinterpret_cast<std::uintptr_t>(p);
        const std::size_t avail = static_cast<std::size_t>(base + mbi.RegionSize - addr);
        if (avail >= remaining) return true;
        remaining -= avail;
        p += avail;
    }
    return true;
}

ConsoleDescriptorHeapFactory g_console_heap_factory = nullptr;  // set by the Vulkan layer

HRESULT proxy_create_descriptor_heap(DeviceProxy* self, const D3D12_DESCRIPTOR_HEAP_DESC* desc, const IID* riid,
                                     void** out) {
    note_slot(14);
    if (out) *out = nullptr;  // _COM_Outptr_: NULL on every failure, as the PC device does
    if (!riid) return E_POINTER;
    // No output: a capability check, answered by the device (S_FALSE, as on the PC).
    if (!out) return self->real->CreateDescriptorHeap(desc, *riid, nullptr);

    void* created = nullptr;
    HRESULT hr = E_INVALIDARG;
    bool console_extended = false;
    std::uint64_t seen_address = 0, seen_size = 0, seen_expected = 0;
    if (desc && desc->Type <= D3D12_DESCRIPTOR_HEAP_TYPE_DSV &&
        (desc->Flags & ~D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == 0) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(desc);
        const void* tail = bytes + sizeof(D3D12_DESCRIPTOR_HEAP_DESC);
        if (readable_range(tail, sizeof(std::uint64_t) * 2)) {
            std::uint64_t address = 0;
            std::uint64_t size = 0;
            std::memcpy(&address, tail, sizeof(address));
            std::memcpy(&size, bytes + sizeof(D3D12_DESCRIPTOR_HEAP_DESC) + sizeof(address), sizeof(size));
            // Console-extended slot 14 (issue #313): address/size follow the PC desc. Only take this path when a
            // non-zero console VA is present; normal PC 16-byte descs never enter it.
            // Only a real placement: a non-zero address whose size is the heap's (count x increment). Without the
            // size check, the stack words after an ordinary 16-byte description became heap addresses (0xb, stack
            // pointers). The rule is VIctory's (descriptor.rs: placed = address != 0 && size == size).
            const std::uint64_t expected =
                std::uint64_t(desc->NumDescriptors) * self->real->GetDescriptorHandleIncrementSize(desc->Type);
            seen_address = address;
            seen_size = size;
            seen_expected = expected;
            if (address != 0 && size == expected && g_console_heap_factory) {
                console_extended = true;
                hr = g_console_heap_factory(self->real, desc, static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(address), size,
                                            __uuidof(ID3D12DescriptorHeap), &created);
            }
        }
    }
    if (!console_extended)
        hr = self->real->CreateDescriptorHeap(desc, __uuidof(ID3D12DescriptorHeap), &created);
    if (FAILED(hr) || !created) {
        // Which of the console-path conditions failed (issue #2: E_INVALIDARG here on Windows-native).
        std::fprintf(stderr,
                     "[rt] d3d12_xs: CreateDescriptorHeap failed 0x%08lx: type %d count %u flags 0x%x mask 0x%x; console "
                     "address 0x%llx size 0x%llx expected 0x%llx (increment %u) factory %s -> %s path\n",
                     static_cast<unsigned long>(hr), desc ? static_cast<int>(desc->Type) : -1,
                     desc ? desc->NumDescriptors : 0u, desc ? static_cast<unsigned>(desc->Flags) : 0u,
                     desc ? desc->NodeMask : 0u, static_cast<unsigned long long>(seen_address),
                     static_cast<unsigned long long>(seen_size), static_cast<unsigned long long>(seen_expected),
                     desc && desc->Type <= D3D12_DESCRIPTOR_HEAP_TYPE_DSV
                         ? self->real->GetDescriptorHandleIncrementSize(desc->Type)
                         : 0u,
                     g_console_heap_factory ? "set" : "unset", console_extended ? "console" : "PC");
        std::fflush(stderr);
        return FAILED(hr) ? hr : E_FAIL;
    }

    ID3D12DescriptorHeap* heap = static_cast<ID3D12DescriptorHeap*>(created);
    hr = create_heap_proxy(heap, self, *riid, out);
    if (FAILED(hr)) heap->Release();
    return hr;
}

// The Xbox slot 62 takes two more words between the description and the IID: (desc, w2, w3, riid, out). Measured
// on the package's XboxBvhBuilder.dll (its constructor passes w2 = w3 = 0 and IID_ID3D12PipelineState in the fourth
// argument), as the prior art had recorded. Their meaning is unknown; non-zero values are logged once.
HRESULT proxy_create_compute_pipeline_state(DeviceProxy* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                            std::uint64_t w2, std::uint64_t w3, const IID* riid, void** out) {
    note_slot(62);
    if (w2 || w3) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: slot 62 extra words 0x%llx 0x%llx (ignored)\n",
                         static_cast<unsigned long long>(w2), static_cast<unsigned long long>(w3));
    }
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    return self->real->CreateComputePipelineState(desc, *riid, out);
}

// The Xbox slot 67 creates a compute pipeline from native GPU code: (desc, riid, out), named
// DeserializeComputePipelineStateX in amd_fidelityfx_upscaler_xbox.dll's messages (call at RVA 0x56d9). The desc
// passes the root signature at +0x00, followed by pointer/size pairs for the shader's FSPX entries (docs/fspx.md):
// kind 0 (dispatch state) at +0x08/+0x10, kind 1 at +0x18/+0x20, kind 6 (the RDNA2 program) at +0x28/+0x30, and
// kind 7 (records) at +0x38/+0x40. Translates the console description to a standard D3D12_COMPUTE_PIPELINE_STATE_DESC
// with CS bytecode pointing at kind 6 and routes it to CreateComputePipelineState on the PC device (slot 11), which
// on the Vulkan device resolves through the translator. The PC slot 67 is CreateProtectedResourceSession1.
HRESULT proxy_create_compute_pipeline_from_native_code(DeviceProxy* self, const void* desc,
                                                       const IID* riid, void** out) {
    note_slot(67);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!desc) return E_INVALIDARG;
    if (!riid) return E_POINTER;

    // FSPX description layout (docs/fspx.md §How the DLL reads it):
    struct XboxNativeComputePipelineStateDesc {
        ID3D12RootSignature* root_signature;  // +0x00
        const void* kind0;                    // +0x08: dispatch state
        std::uint64_t kind0_size;             // +0x10: 28 bytes
        const void* kind1;                    // +0x18: unknown
        std::uint64_t kind1_size;             // +0x20: 20 bytes
        const void* kind6;                    // +0x28: RDNA2 program code
        std::uint64_t kind6_size;             // +0x30: program size in bytes
        const void* kind7;                    // +0x38: resource/hash records
        std::uint64_t kind7_size;             // +0x40: records size in bytes
    };
    const auto* xbox_desc = static_cast<const XboxNativeComputePipelineStateDesc*>(desc);

    D3D12_COMPUTE_PIPELINE_STATE_DESC pc_desc{};
    pc_desc.pRootSignature = xbox_desc->root_signature;
    pc_desc.CS.pShaderBytecode = xbox_desc->kind6;
    pc_desc.CS.BytecodeLength = static_cast<SIZE_T>(xbox_desc->kind6_size);
    // With kind 0 (the DLL always passes it: its reader refuses a payload without one), the four pairs go on as one
    // FSPX payload, so the device matches the program by its kind-6 and kind-0 keys (docs/fspx.md, "Build and run-time
    // lookup"): two programs with the same code and different dispatch state stay apart. Every pair is checked before
    // it is read (leonida::fspx_payload_from_parts); an inconsistent one refuses the call. Without kind 0 the kind-6
    // bytes go on alone, as before.
    std::vector<std::uint8_t> payload;
    if (xbox_desc->kind0 || xbox_desc->kind0_size) {
        const leonida::FspxPart parts[] = {{0, xbox_desc->kind0, xbox_desc->kind0_size},
                                           {1, xbox_desc->kind1, xbox_desc->kind1_size},
                                           {6, xbox_desc->kind6, xbox_desc->kind6_size},
                                           {7, xbox_desc->kind7, xbox_desc->kind7_size}};
        payload = leonida::fspx_payload_from_parts(parts);
        if (payload.empty()) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true, std::memory_order_relaxed))
                std::fprintf(stderr,
                             "[rt] d3d12_xs: slot 67 refused: a pointer/size pair is inconsistent or too large "
                             "(kind0 %llu B, kind1 %llu B, kind6 %llu B, kind7 %llu B)\n",
                             static_cast<unsigned long long>(xbox_desc->kind0_size),
                             static_cast<unsigned long long>(xbox_desc->kind1_size),
                             static_cast<unsigned long long>(xbox_desc->kind6_size),
                             static_cast<unsigned long long>(xbox_desc->kind7_size));
            return E_INVALIDARG;
        }
        pc_desc.CS.pShaderBytecode = payload.data();
        pc_desc.CS.BytecodeLength = payload.size();
    }

    pc_desc.NodeMask = 0;
    pc_desc.CachedPSO = {nullptr, 0};
    pc_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    // Every failed call is logged with its number: the DLL creates its pipelines one after the other, and only the
    // first call of a slot is announced, so a miss on a later program would otherwise read as the first call's.
    static std::atomic<std::uint64_t> calls{0};
    const std::uint64_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    HRESULT hr = self->real->CreateComputePipelineState(&pc_desc, *riid, out);
    // HACK (Leonida CE): a compute pipeline we cannot build (no translated shader) leaves the title a null definition
    // record that it later faults on (+0xb5a5f8 / +0x5392b80 on Windows). As for slot 57, hand back the first compute
    // pipeline that worked instead; its dispatches do the wrong thing, the title goes on.
    // LEONIDA_CE_NO_PLACEHOLDER=1 keeps the failure.
    static std::atomic<IUnknown*> stand_in{nullptr};
    if (SUCCEEDED(hr) && out && *out && !stand_in.load()) {
        static_cast<IUnknown*>(*out)->AddRef();
        stand_in = static_cast<IUnknown*>(*out);
    }
    if (FAILED(hr) && out) {
        static const bool no_placeholder = [] {
            char v[8] = {};
            return GetEnvironmentVariableA("LEONIDA_CE_NO_PLACEHOLDER", v, sizeof(v)) && v[0] == '1';
        }();
        if (IUnknown* cs = stand_in.load(); cs && !no_placeholder) {
            if (SUCCEEDED(cs->QueryInterface(*riid, out))) {
                static std::atomic<int> used{0};
                if (used++ < 3)
                    std::fprintf(stderr, "[hack] slot 67 call %llu: no compute shader; stand-in pipeline\n",
                                 static_cast<unsigned long long>(call));
                return S_OK;
            }
        }
    }
    if (FAILED(hr))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 67 call %llu failed with 0x%08lx (kind 0 %llu B, kind 1 %llu B, kind 6 %llu B, "
                     "kind 7 %llu B)\n",
                     static_cast<unsigned long long>(call), static_cast<unsigned long>(hr),
                     static_cast<unsigned long long>(xbox_desc->kind0_size),
                     static_cast<unsigned long long>(xbox_desc->kind1_size),
                     static_cast<unsigned long long>(xbox_desc->kind6_size),
                     static_cast<unsigned long long>(xbox_desc->kind7_size));
    return hr;
}

HRESULT proxy_create_resource(DeviceProxy* self, const D3D12_HEAP_PROPERTIES* heap_properties,
                              D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC* xbox_desc,
                              D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE* clear_value,
                              const IID* riid, void** out) {
    note_slot(76);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!xbox_desc) return E_POINTER;
    if (!riid) return E_POINTER;

    // The measured description is 56 bytes plus Xbox extension words; D3D12_RESOURCE_DESC is exactly 56 bytes.
    // The Xbox layout word is 0x101 (tiled) or 0x1 (row-major buffer), and is not a PC layout value. Committed
    // resources take ROW_MAJOR for buffers and UNKNOWN for everything else, so the word is ignored and replaced.
    D3D12_RESOURCE_DESC pc_desc = *xbox_desc;
    xs_fix_pc_flags(pc_desc);
    const auto xbox_layout = static_cast<std::uint32_t>(pc_desc.Layout);
    pc_desc.Layout = pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                         : D3D12_TEXTURE_LAYOUT_UNKNOWN;

    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 76 resource: dimension=%d xbox layout=0x%x -> PC layout=0x%x\n",
                     static_cast<int>(pc_desc.Dimension), xbox_layout, static_cast<unsigned>(pc_desc.Layout));
    }
    HRESULT hr = self->real->CreateCommittedResource(heap_properties, heap_flags, &pc_desc, initial_state, clear_value,
                                                     *riid, out);
    if (FAILED(hr)) {
        // HACK: retry with PC flag bits only, COMMON state and no clear value; then log.
        D3D12_RESOURCE_DESC d2 = pc_desc;
        d2.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<unsigned>(d2.Flags) & 0xffu);
        const bool clearable = (d2.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
        HRESULT hr2 = self->real->CreateCommittedResource(heap_properties, D3D12_HEAP_FLAG_NONE, &d2, D3D12_RESOURCE_STATE_COMMON,
                                                          clearable ? clear_value : nullptr, *riid, out);
        if (FAILED(hr2) && (d2.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
            // e.g. A8_UNORM render targets: drop the render-target bit as a last resort
            d2.Flags = static_cast<D3D12_RESOURCE_FLAGS>(d2.Flags & ~D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            hr2 = self->real->CreateCommittedResource(heap_properties, D3D12_HEAP_FLAG_NONE, &d2, D3D12_RESOURCE_STATE_COMMON,
                                                      nullptr, *riid, out);
        }
        static std::atomic<int> n{0};
        if (n++ < 12) {
            std::fprintf(stderr, "[hack] slot 76 CreateCommittedResource failed 0x%08lx (heap=%d hflags=0x%x dim=%d %llux%u fmt=%d flags=0x%x state=0x%x) retry -> 0x%08lx\n",
                         (unsigned long)hr, heap_properties ? (int)heap_properties->Type : -1, (unsigned)heap_flags,
                         (int)pc_desc.Dimension, (unsigned long long)pc_desc.Width, pc_desc.Height, (int)pc_desc.Format,
                         (unsigned)pc_desc.Flags, (unsigned)initial_state, (unsigned long)hr2);
            std::fflush(stderr);
        }
        hr = hr2;
    }
    return hr;
}

// HACK (Leonida CE boot): on the console a texture's GPU address is its memory, which the CPU reads and writes too
// (the title clears an R32_FLOAT texture with memset through the address ID3D12Resource::GetGPUVirtualAddress gives).
// PC D3D12 gives textures no address (0), so the title wrote through null. Each texture the title makes gets one: the
// title's own pages for slot 133, otherwise host memory the size of its console layout. The memory is registered as
// placed, so the periodic refresh copies what the title writes there into the texture.
std::atomic<TextureAddressHook> g_texture_address_hook{nullptr};
void xs_console_texture_address(ID3D12Device* dev, void* resource, const D3D12_RESOURCE_DESC& console_desc,
                                std::uint64_t va) {
    const TextureAddressHook hook = g_texture_address_hook.load(std::memory_order_acquire);
    if (!hook || !resource || console_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return;
    if (!va) {
        std::uint64_t bytes = 0;
        leonida::xg::TextureDesc td;
        td.dimension = console_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? leonida::xg::Dimension::Tex3d
                     : console_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? leonida::xg::Dimension::Tex1d
                                                                                     : leonida::xg::Dimension::Tex2d;
        td.width = static_cast<std::uint32_t>(console_desc.Width);
        td.height = console_desc.Height;
        td.depth_or_array = std::max<std::uint32_t>(console_desc.DepthOrArraySize, 1);
        td.mips = console_desc.MipLevels;
        td.format = static_cast<std::uint32_t>(console_desc.Format);
        td.samples = std::max<std::uint32_t>(console_desc.SampleDesc.Count, 1);
        const auto layout_word = static_cast<std::uint32_t>(console_desc.Layout);
        td.swizzle.value = (layout_word & 0x100) ? (layout_word & 0xFF) : leonida::xg::Swizzle::LINEAR;
        leonida::xg::Texture xt;
        if (leonida::xg::Texture::make(td, xt) == leonida::xg::Error::Ok) bytes = xt.size;
        D3D12_RESOURCE_DESC pc = console_desc;
        pc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        pc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<std::uint32_t>(pc.Flags) & 0xffu);
        bytes = std::max<std::uint64_t>(bytes, dev ? dev->GetResourceAllocationInfo(0, 1, &pc).SizeInBytes : 0);
        if (!bytes || bytes > (std::uint64_t(2) << 30)) return;
        bytes = (bytes + 0xFFFF) & ~std::uint64_t(0xFFFF);
        void* mem = VirtualAlloc(nullptr, static_cast<SIZE_T>(bytes), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!mem) return;
        va = reinterpret_cast<std::uint64_t>(mem);
        xs_record_placed(va, bytes, static_cast<ID3D12Resource*>(resource), &console_desc);
    }
    hook(resource, va);
}

// Device slot 133 is the console's CreatePlacedResourceX: (va, desc, state, clear, riid, out), measured on the retail
// executable (docs/retail_boot_chain.md, section 3.4). `va` is the start of a physical-page range the title has just
// mapped, `desc` a 56-byte description with Xbox bits above the PC resource flags and an Xbox layout word, and
// `state` is 0xFFFFFFFF, which is no PC state. The resource is a buffer on those pages, made by the handler the
// device behind the proxy registered (set_placed_at_va_handler), with the PC flags kept and the layout made
// ROW_MAJOR, as slot 76 does.
std::atomic<PlacedAtVaHandler> g_placed_at_va{nullptr};
std::atomic<PlacedAtVaPagesHook> g_placed_at_va_pages{nullptr};

HRESULT proxy_create_placed_resource_x(DeviceProxy* self, void* va, const D3D12_RESOURCE_DESC* xbox_desc,
                                       std::uint32_t state, const IID* riid, void** out) {
    note_slot(133);
    if (out) *out = nullptr;
    // Without a handler (a PC device behind the proxy, or none registered) the slot refuses like any slot past the
    // PC vtable, whatever its arguments.
    const PlacedAtVaHandler handler = g_placed_at_va.load(std::memory_order_acquire);
    if (!handler) return E_NOTIMPL;
    if (!out || !xbox_desc || !riid) return E_POINTER;
    // An integer mask: a D3D12_RESOURCE_FLAGS constant past the SDK's last enumerator is not a constant expression
    // for clang-cl with SDK 10.0.26100 (found in review). The cast is applied to the masked value at run time.
    // The PC flags up to RAYTRACING_ACCELERATION_STRUCTURE (0x100) and USE_TIGHT_ALIGNMENT (0x400); 0x200 is
    // dropped, as committed_resource_refusal says the host refuses it (found in review).
    constexpr std::uint32_t kPcResourceFlags = 0x5ff;
    D3D12_RESOURCE_DESC pc_desc = *xbox_desc;
    const auto xbox_flags = static_cast<std::uint32_t>(pc_desc.Flags);
    pc_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(xbox_flags & kPcResourceFlags);
    xs_fix_pc_flags(pc_desc);
    // The Xbox layout word (0x101 tiled, 0x1 row-major buffer) is no PC layout: ROW_MAJOR for a buffer and UNKNOWN
    // for everything else, as slot 76 does.
    pc_desc.Layout = pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                         : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 133 CreatePlacedResourceX: va=%p dimension=%d width=%llu xbox flags=0x%x "
                     "state=0x%x -> a buffer on the title's pages\n",
                     va, static_cast<int>(pc_desc.Dimension), static_cast<unsigned long long>(pc_desc.Width),
                     xbox_flags, state);
    }
    // The buffer's pages are committed before the handler walks them (set_placed_at_va_pages_hook).
    const PlacedAtVaPagesHook pages = g_placed_at_va_pages.load(std::memory_order_acquire);
    if (pages && pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && pc_desc.Width &&
        !pages(va, static_cast<std::size_t>(pc_desc.Width))) {
        static std::atomic<bool> refused{false};
        if (!refused.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr, "[rt] d3d12_xs: slot 133: committing the pages of va=%p width=%llu failed (error %lu)\n",
                         va, static_cast<unsigned long long>(pc_desc.Width), GetLastError());
        }
    }
    return handler(self->real, va, pc_desc, state, *riid, out);
}

template <unsigned Slot>
std::uint64_t WINAPI proxy_slot(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                                std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                                std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if constexpr (Slot == 0) {
        return hr_to_u64(proxy_query_interface(self, reinterpret_cast<const IID*>(a1),
                                               reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return proxy_add_ref(self);
    } else if constexpr (Slot == 2) {
        return proxy_release(self);
    } else if constexpr (Slot == 8) {
        return hr_to_u64(proxy_create_command_queue(self, reinterpret_cast<const D3D12_COMMAND_QUEUE_DESC*>(a1),
                                                    reinterpret_cast<const IID*>(a2), reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 9) {
        return hr_to_u64(proxy_create_command_allocator(self, a1, reinterpret_cast<const IID*>(a2),
                                                        reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 12) {
        note_slot(12);
        a3 = unwrap_object(a3);
        if (Slot >= self->slot_count) return hr_to_u64(refuse_beyond_device(12, self->slot_count));
        using CreateListFn = HRESULT(WINAPI*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
                                              ID3D12PipelineState*, const IID*, void**);
        auto fn = reinterpret_cast<CreateListFn>(self->real_vtable[12]);
        return hr_to_u64(fn(self->real, static_cast<UINT>(a1), static_cast<D3D12_COMMAND_LIST_TYPE>(a2),
                            reinterpret_cast<ID3D12CommandAllocator*>(a3),
                            reinterpret_cast<ID3D12PipelineState*>(a4),
                            reinterpret_cast<const IID*>(a5),
                            reinterpret_cast<void**>(a6)));
    } else if constexpr (Slot == 36) {
        return hr_to_u64(proxy_create_fence(self, a1, a2, reinterpret_cast<const IID*>(a3),
                                            reinterpret_cast<void**>(a4)));
    } else if constexpr (Slot == 32) {
        return hr_to_u64(proxy_open_shared_handle(self, a1, reinterpret_cast<const IID*>(a2),
                                                  reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 31 || Slot == 34 || Slot == 35 || Slot == 45 || Slot == 46 || Slot == 50) {
        return hr_to_u64(proxy_unwrapping_call<Slot>(self, a1, a2, a3, a4, a5));
    } else if constexpr (Slot == 75) {
        return hr_to_u64(proxy_create_command_queue1(self, a1, a2, reinterpret_cast<const IID*>(a3),
                                                     reinterpret_cast<void**>(a4)));
    } else if constexpr (Slot == 14) {
        return hr_to_u64(proxy_create_descriptor_heap(
            self, reinterpret_cast<const D3D12_DESCRIPTOR_HEAP_DESC*>(a1),
            reinterpret_cast<const IID*>(a2), reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 26) {
        // HACK: the Xbox title calls GetCustomHeapProperties with the struct-return buffer in rcx and the device in
        // rdx (measured at game+0x2478700: rcx=&local, rdx=device, r8=nodeMask, r9=heapType).
        auto* ret = reinterpret_cast<D3D12_HEAP_PROPERTIES*>(self);
        auto* dev = reinterpret_cast<DeviceProxy*>(a1);
        const D3D12_HEAP_PROPERTIES hp =
            dev->real->GetCustomHeapProperties(static_cast<UINT>(a2), static_cast<D3D12_HEAP_TYPE>(a3));
        *ret = hp;
        static std::atomic<int> n{0};
        if (n++ < 4)
            std::fprintf(stderr, "[hack] GetCustomHeapProperties(node=%llu, type=%llu) -> page=%d pool=%d\n",
                         (unsigned long long)a2, (unsigned long long)a3, (int)hp.CPUPageProperty,
                         (int)hp.MemoryPoolPreference);
        return reinterpret_cast<std::uint64_t>(ret);
    } else if constexpr (Slot == 25) {
        // HACK: GetResourceAllocationInfo(visibleMask, numDescs, descs) with the struct-return buffer in rcx and the
        // device in rdx, like slot 26. Console descs carry Xbox flag bits and a console layout word; both are cleaned.
        auto* ret = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(self);
        auto* dev = reinterpret_cast<DeviceProxy*>(a1);
        const UINT n = static_cast<UINT>(a3);
        const auto* in = reinterpret_cast<const D3D12_RESOURCE_DESC*>(a4);
        std::vector<D3D12_RESOURCE_DESC> descs(in, in + (n ? n : 0));
        for (auto& d : descs) {
            d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<unsigned>(d.Flags) & 0xffu);
            d.Layout = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                      : D3D12_TEXTURE_LAYOUT_UNKNOWN;
        }
        D3D12_RESOURCE_ALLOCATION_INFO info = n == 1
            ? xs_alloc_info(dev->real, static_cast<UINT>(a2), descs[0])
            : dev->real->GetResourceAllocationInfo(static_cast<UINT>(a2), n, descs.data());
        *ret = info;
        static std::atomic<int> c{0};
        if (c++ < 4)
            std::fprintf(stderr, "[hack] GetResourceAllocationInfo(n=%u, dim=%d, %llux%u) -> size=%llu align=%llu\n", n,
                         n ? (int)descs[0].Dimension : -1, n ? (unsigned long long)descs[0].Width : 0ull,
                         n ? descs[0].Height : 0u, (unsigned long long)info.SizeInBytes,
                         (unsigned long long)info.Alignment);
        return reinterpret_cast<std::uint64_t>(ret);
    } else if constexpr (Slot == 127) {
        // HACK: Xbox slot 127, shaped like CreateCommittedResource3:
        // (heapProps, heapFlags, desc, state/layout, clear, protectedSession, numCastable, castable, riid, out).
        auto* hp = reinterpret_cast<const D3D12_HEAP_PROPERTIES*>(a1);
        auto* desc_in = reinterpret_cast<const D3D12_RESOURCE_DESC*>(a3);
        auto* clear = reinterpret_cast<const D3D12_CLEAR_VALUE*>(a5);
        auto* riid = reinterpret_cast<const IID*>(a9);
        auto** out = reinterpret_cast<void**>(a10);
        if (!hp || !desc_in || !riid || !out) return hr_to_u64(E_POINTER);
        D3D12_RESOURCE_DESC d = *desc_in;
        d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<unsigned>(d.Flags) & 0xffu);
        xs_fix_pc_flags(d);
        d.Layout = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                  : D3D12_TEXTURE_LAYOUT_UNKNOWN;
        const bool clearable = (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                           D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
        auto flags = static_cast<D3D12_HEAP_FLAGS>(static_cast<unsigned>(a2));
        xs_title_d3d_alloc(self->real->GetResourceAllocationInfo(0, 1, &d).SizeInBytes);
        HRESULT hr = self->real->CreateCommittedResource(hp, flags, &d, D3D12_RESOURCE_STATE_COMMON,
                                                         clearable ? clear : nullptr, *riid, out);
        if (FAILED(hr))
            hr = self->real->CreateCommittedResource(hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
                                                     nullptr, *riid, out);
        static std::atomic<int> c{0};
        if (c++ < 6 || FAILED(hr)) {
            std::fprintf(stderr, "[hack] slot 127 CreateCommittedResource3(heap=%d, hflags=0x%llx, dim=%d, %llux%u fmt=%d rflags=0x%x state=0x%llx) -> 0x%08lx\n",
                         (int)hp->Type, (unsigned long long)a2, (int)desc_in->Dimension, (unsigned long long)desc_in->Width,
                         desc_in->Height, (int)desc_in->Format, (unsigned)desc_in->Flags, (unsigned long long)a4,
                         (unsigned long)hr);
            std::fflush(stderr);
        }
        if (SUCCEEDED(hr) && *out) xs_console_texture_address(self->real, *out, *desc_in, 0);
        return hr_to_u64(hr);
    } else if constexpr (Slot == 41) {
        // HACK diagnostic + fallback: CreateCommandSignature(desc, rootSig, riid, out)
        auto* desc = reinterpret_cast<const D3D12_COMMAND_SIGNATURE_DESC*>(a1);
        auto* root = reinterpret_cast<ID3D12RootSignature*>(a2);
        auto* riid = reinterpret_cast<const IID*>(a3);
        auto** out = reinterpret_cast<void**>(a4);
        if (!desc || !riid) return hr_to_u64(E_POINTER);
        HRESULT hr = self->real->CreateCommandSignature(desc, root, *riid, out);
        if ((hr == E_INVALIDARG || hr == E_NOTIMPL) && desc->NumArgumentDescs <= 16) {
            // Console-only argument types (seen: 105, 12-byte stride): retry as DISPATCH_MESH, then DISPATCH.
            D3D12_INDIRECT_ARGUMENT_DESC args[16];
            std::memcpy(args, desc->pArgumentDescs, sizeof(args[0]) * desc->NumArgumentDescs);
            D3D12_COMMAND_SIGNATURE_DESC d2 = *desc;
            d2.pArgumentDescs = args;
            for (auto replacement : {D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH, D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH}) {
                for (UINT i = 0; i < d2.NumArgumentDescs; ++i)
                    if (static_cast<int>(desc->pArgumentDescs[i].Type) >= 100 || desc->pArgumentDescs[i].Type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH) {
                        std::memset(&args[i], 0, sizeof(args[i]));
                        args[i].Type = replacement;
                    }
                hr = self->real->CreateCommandSignature(&d2, root, *riid, out);
                std::fprintf(stderr, "[hack]   retry with type %d -> 0x%08lx\n", (int)replacement, (unsigned long)hr);
                if (SUCCEEDED(hr)) break;
            }
        }
        std::fprintf(stderr, "[hack] CreateCommandSignature(stride=%u, n=%u, root=%p) -> 0x%08lx; args:", desc->ByteStride,
                     desc->NumArgumentDescs, (void*)root, (unsigned long)hr);
        for (UINT i = 0; i < desc->NumArgumentDescs && i < 8; ++i) {
            const auto& ad = desc->pArgumentDescs[i];
            const auto* w = reinterpret_cast<const std::uint32_t*>(&ad);
            std::fprintf(stderr, " [type %d: %x %x %x]", (int)ad.Type, w[1], w[2], w[3]);
        }
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
        return hr_to_u64(hr);
    } else if constexpr (Slot >= 17 && Slot <= 24) {
        // HACK: descriptor writes (CreateConstantBuffer/ShaderResource/UnorderedAccess/RenderTarget/DepthStencil
        // View, CreateSampler, CopyDescriptors, CopyDescriptorsSimple) whose destination is below 64 KiB: the title
        // builds some destinations from a base that is 0 here (0xb + i*32; its source is not found yet). Skip them so
        // the title runs on; those descriptors stay empty.
        // The destination is the last handle argument: CBV/sampler a2, SRV/RTV/DSV a3, UAV a4,
        // CopyDescriptorsSimple a2 (destination start).
        std::uint64_t dest = Slot == 17 || Slot == 22 || Slot == 24 ? a2 : Slot == 19 ? a4 : a3;
        if constexpr (Slot == 23) dest = 1u << 20;  // CopyDescriptors: arrays, not checked here
        if (dest < 0x10000) {
            static std::atomic<int> n{0};
            if (n++ < 5) {
                std::fprintf(stderr, "[hack] device slot %u: descriptor destination 0x%llx skipped\n", Slot,
                             (unsigned long long)dest);
                std::fflush(stderr);
            }
            return 0;
        }
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
        if constexpr (Slot == 17) {  // CBV: the console passes a CPU pointer as BufferLocation
            if (a1) {
                cbv = *reinterpret_cast<const D3D12_CONSTANT_BUFFER_VIEW_DESC*>(a1);
                {   static std::atomic<int> n{0};  // HACK diagnostic
                    if (n++ < 4) { std::fprintf(stderr, "[hack] CBV location %llx size %u (arena %p+%llx)\n", (unsigned long long)cbv.BufferLocation, cbv.SizeInBytes, (void*)g_arena_mapped, (unsigned long long)g_arena_size); std::fflush(stderr); } }
                const std::uint64_t translated = xs_va_from_cpu(cbv.BufferLocation);
                if (translated == cbv.BufferLocation && cbv.BufferLocation && cbv.SizeInBytes &&
                    cbv.SizeInBytes <= (1u << 16) &&
                    xs_readable(reinterpret_cast<const void*>(cbv.BufferLocation), cbv.SizeInBytes)) {
                    // HACK (Leonida CE boot): constants in the title's own memory, which the GPU cannot read here yet:
                    // snapshot them into the arena when the view is made (VIctory copies changed title memory).
                    // fspx CB analysis: a stable, refreshed arena shadow first (see xs_cbv_shadow); the ring copy
                    // below is the fallback (stale after the title's next write, clobbered when the ring wraps).
                    if (const std::uint64_t shadow = xs_cbv_shadow(self->real, cbv.BufferLocation, cbv.SizeInBytes)) {
                        cbv.BufferLocation = shadow;
                    } else if (const std::uint64_t copy = xs_upload_alloc(cbv.SizeInBytes, 256)) {
                        std::memcpy(reinterpret_cast<void*>(copy), reinterpret_cast<const void*>(cbv.BufferLocation),
                                    cbv.SizeInBytes);
                        cbv.BufferLocation = xs_va_from_cpu(copy);
                    }
                } else if (xs_cbv_fix_on() && translated == cbv.BufferLocation && cbv.BufferLocation >= g_arena_size) {
                    // agA13: too big to snapshot here, or not CPU-readable (GPU-written title memory): a placed buffer
                    // that covers it; else the vulkan device's CBV_FIX (snapshot up to 8 MiB / null view) decides.
                    if (const std::uint64_t to = xs_placed_buffer_va(cbv.BufferLocation, cbv.SizeInBytes)) {
                        xs_cbv_fix_note("CBV in a placed buffer", cbv.BufferLocation, to);
                        cbv.BufferLocation = to;
                    }
                } else {
                    cbv.BufferLocation = translated;
                }
                a1 = reinterpret_cast<std::uint64_t>(&cbv);
            }
        }
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t);
        // Console-only view formats (xs_pc_format): the view description is copied with the PC format.
        alignas(8) unsigned char view_copy[64];
        if constexpr (Slot >= 18 && Slot <= 21) {
            constexpr std::size_t kViewBytes = Slot == 18 ? sizeof(D3D12_SHADER_RESOURCE_VIEW_DESC)
                                             : Slot == 19 ? sizeof(D3D12_UNORDERED_ACCESS_VIEW_DESC)
                                             : Slot == 20 ? sizeof(D3D12_RENDER_TARGET_VIEW_DESC)
                                                          : sizeof(D3D12_DEPTH_STENCIL_VIEW_DESC);
            static_assert(kViewBytes <= sizeof(view_copy));
            const auto* view = reinterpret_cast<const DXGI_FORMAT*>(a2);
            if (view && xs_pc_format(*view) != *view) {
                std::memcpy(view_copy, view, kViewBytes);
                *reinterpret_cast<DXGI_FORMAT*>(view_copy) = xs_pc_format(*view);
                a2 = reinterpret_cast<std::uint64_t>(view_copy);
            }
            // HACK (agA6, LEONIDA_CE_A8_SWZ, default on, =0 off): an A8_UNORM texture is stored as R8 on the PC side,
            // so its SRV read (r,0,0,1) and Scaleform's glyph shader (FText samples .w only) saw alpha 1. Give A8 SRVs
            // the A8 component mapping (0,0,0,R), composed with the title's own mapping. A null view description of an
            // A8 placed texture gets a full Texture2D description.
            if constexpr (Slot == 18) {
                // build 29: A8 render targets (the scroll-pane mask, 3840x2160 A8) keep the old R8 mapping (alpha 1):
                // their PC R8 target stores the shader's .r, not its alpha, so (0,0,0,R) masked the TERMS policy text
                // away in re10. LEONIDA_CE_A8_SWZ=2 applies the swizzle to A8 render targets too.
                static const int a8mode = [] { const char* e = std::getenv("LEONIDA_CE_A8_SWZ"); return e && e[0] == '0' ? 0 : e && e[0] == '2' ? 2 : 1; }();
                const bool a8swz = a8mode != 0;
                bool a8 = view && static_cast<unsigned>(*view) == 65u;
                if (a8 && a8mode == 1 && a1) {
                    XsPlaced pr{};
                    if (xs_placed_by_resource(reinterpret_cast<const void*>(a1), pr) &&
                        (static_cast<std::uint32_t>(pr.desc.Flags) & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
                        a8 = false;
                        static std::atomic<int> told_rt{0};
                        if (told_rt++ < 8)
                            std::fprintf(stderr, "[hack] A8 SRV: res %p is a render target (%llux%u flags %x): R8 mapping kept\n", (void*)a1,
                                         (unsigned long long)pr.desc.Width, pr.desc.Height, static_cast<unsigned>(pr.desc.Flags));
                    }
                }
                if (a8swz && !view && a1) {
                    XsPlaced pl{};
                    if (xs_placed_by_resource(reinterpret_cast<const void*>(a1), pl) &&
                        static_cast<unsigned>(pl.desc.Format) == 65u && pl.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                        (a8mode == 2 || !(static_cast<std::uint32_t>(pl.desc.Flags) & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) &&
                        pl.desc.DepthOrArraySize <= 1 && pl.desc.SampleDesc.Count <= 1) {
                        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                        sd.Format = DXGI_FORMAT_R8_UNORM;
                        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        sd.Texture2D.MipLevels = static_cast<UINT>(-1);
                        std::memcpy(view_copy, &sd, sizeof(sd));
                        a2 = reinterpret_cast<std::uint64_t>(view_copy);
                        a8 = true;
                    }
                }
                if (a8swz && a8) {
                    auto* sd = reinterpret_cast<D3D12_SHADER_RESOURCE_VIEW_DESC*>(view_copy);
                    const UINT m = sd->Shader4ComponentMapping;
                    UINT sel[4];
                    for (UINT i = 0; i < 4; ++i) {
                        const UINT from = D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(i, m);
                        sel[i] = from == 3 ? 0u : from < 3 ? UINT(D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0) : from;
                    }
                    sd->Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(sel[0], sel[1], sel[2], sel[3]);
                    static std::atomic<int> told{0};
                    if (told++ < 12)
                        std::fprintf(stderr, "[hack] A8 SRV: res %p mapping %x -> %x\n", (void*)a1, m, sd->Shader4ComponentMapping);
                }
            }
        }
        const std::uint64_t r = reinterpret_cast<ForwardFn>(self->real_vtable[Slot])(self->real, a1, a2, a3, a4, a5, a6);
        if constexpr (Slot == 18) {  // SRV of a texture the title placed: copy its data in
            XsPlaced placed{};
            if (a1 && xs_placed_by_resource(reinterpret_cast<const void*>(a1), placed) &&
                placed.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
                xs_upload_placed_texture(self->real, placed.desc, placed.resource, placed.va);
        }
        return r;
    } else if constexpr (Slot == 55) {
        // HACK: console slot 55 creates a pipeline (riid = ID3D12PipelineState in a4, out in a5), not PC
        // CreateReservedResource1, which wrote through the wrong argument. Refused until its description is known.
        if (a5) *reinterpret_cast<void**>(a5) = nullptr;
        static std::atomic<int> n{0};
        if (n++ < 3) { std::fprintf(stderr, "[hack] device slot 55 (pipeline?) refused: a1=%llx count=%llu desc=%llx\n", (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3); std::fflush(stderr); }
        return hr_to_u64(E_NOTIMPL);
    } else if constexpr (Slot == 74) {
        // HACK: Xbox slot 74 = (va, page_count, &out) right after the title maps its ~6 GB physical-page GPU pool.
        // Unknown object: hand back a logging probe.
        auto** out = reinterpret_cast<void**>(a3);
        std::fprintf(stderr, "[hack] device slot 74 (va=%llx, pages=%llu = %llu MiB, out=%llx) -> probe\n",
                     (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)(a2 * 64 / 1024),
                     (unsigned long long)a3);
        std::fflush(stderr);
        // VIctory (register_page_pool): the result is a handle, the pool's index plus one.
        static std::atomic<std::uintptr_t> pools{0};
        if (out) *reinterpret_cast<std::uintptr_t*>(out) = ++pools;
        return 0;
    } else if constexpr (Slot == 61) {
        // HACK: Xbox slot 61 = a sampler-creating call (desc, dest handle) with a console desc layout; ignored.
        static std::atomic<int> n{0};
        if (n++ < 2) { std::fprintf(stderr, "[hack] device slot 61 (sampler) ignored: desc=%llx dest=%llx\n", (unsigned long long)a1, (unsigned long long)a2); std::fflush(stderr); }
        return 0;
    } else if constexpr (Slot == 108) {
        // HACK: Xbox slot 108 ~ GetResourceAllocationInfo1(visibleMask, numDescs, descs, info1[]) with the
        // struct-return buffer in rcx and the device in rdx (as slots 25/26).
        auto* ret = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(self);
        auto* dev = reinterpret_cast<DeviceProxy*>(a1);
        const UINT n = static_cast<UINT>(a3);
        const auto* in = reinterpret_cast<const D3D12_RESOURCE_DESC*>(a4);
        auto* info1 = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO1*>(a5);
        D3D12_RESOURCE_ALLOCATION_INFO total{0, 0};
        UINT64 offset = 0;
        for (UINT i = 0; i < n; ++i) {
            D3D12_RESOURCE_DESC d = in[i];
            d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<unsigned>(d.Flags) & 0xffu);
            xs_fix_pc_flags(d);
            d.Layout = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                      : D3D12_TEXTURE_LAYOUT_UNKNOWN;
            const auto one = xs_alloc_info(dev->real, static_cast<UINT>(a2), d);
            const UINT64 al = one.Alignment ? one.Alignment : 65536;
            offset = (offset + al - 1) & ~(al - 1);
            if (info1) { info1[i].Offset = offset; info1[i].Alignment = al; info1[i].SizeInBytes = one.SizeInBytes; }
            offset += one.SizeInBytes;
            total.Alignment = (std::max)(total.Alignment, al);
        }
        total.SizeInBytes = offset;
        *ret = total;
        static std::atomic<int> c{0};
        if (c++ < 3) {
            std::fprintf(stderr, "[hack] slot 108 GetResourceAllocationInfo1(n=%u, dim=%d %llux%u) -> size=%llu align=%llu\n", n,
                         n ? (int)in[0].Dimension : -1, n ? (unsigned long long)in[0].Width : 0ull, n ? in[0].Height : 0u,
                         (unsigned long long)total.SizeInBytes, (unsigned long long)total.Alignment);
            std::fflush(stderr);
        }
        return reinterpret_cast<std::uint64_t>(ret);
    } else if constexpr (Slot == 81) {
        // Device slot 81: frame statistics (token, kinds, count*, out). No statistics: count 0, S_OK (VIctory frame.rs
        // statistics, GPLv3, credit to its authors). It used to answer E_NOTIMPL.
        if (a3) *reinterpret_cast<std::uint32_t*>(a3) = 0;
        return hr_to_u64(S_OK);
    } else if constexpr (Slot == 57 || Slot == 86) {
        // HACK: Xbox slot 57 = ID3D12Device2::CreatePipelineState(streamDesc, riid, out) (PC slot 47). Slot 86 takes
        // the same (desc, riid, out) stream form (VIctory device.rs: f[86] = pipeline::create_stream; GPLv3, credit to
        // its authors); it used to answer E_NOTIMPL, so every pipeline the title made through it was missing.
        auto* desc_in = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_DESC*>(a1);
        auto* riid = reinterpret_cast<const IID*>(a2);
        auto** out = reinterpret_cast<void**>(a3);
        if (!desc_in || !riid) return hr_to_u64(E_POINTER);
        D3D12_PIPELINE_STATE_STREAM_DESC swapped = *desc_in;
        if (swapped.SizeInBytes > (1ull << 32)) {  // console order: {pStream, SizeInBytes}
            const auto* w = reinterpret_cast<const std::uint64_t*>(desc_in);
            swapped.pPipelineStateSubobjectStream = reinterpret_cast<void*>(w[0]);
            swapped.SizeInBytes = static_cast<SIZE_T>(w[1]);
        }
        const D3D12_PIPELINE_STATE_STREAM_DESC* desc = &swapped;
        (void)desc;
        {   static std::atomic<int> dumped{0};
            if (dumped++ < 2) {
                const auto* w = reinterpret_cast<const std::uint64_t*>(desc_in);
                std::fprintf(stderr, "[hack] gfx desc %p:", (void*)desc_in);
                for (int i = 0; i < 90; ++i) std::fprintf(stderr, "%s%llx", i % 8 ? " " : "\n[hack]   ", (unsigned long long)w[i]);
                std::fprintf(stderr, "\n"); std::fflush(stderr);
            }
        }
        // It is a D3D12_GRAPHICS_PIPELINE_STATE_DESC ({root signature, VS, PS, ...}), i.e. PC slot 10.
        // Not a PC descriptor: a 0x160-byte console pipeline record (root signature + six {ptr,size} blobs + hashes).
        // Refused until the record is understood. HACK: dump the first records' blobs for study.
        {
            static std::atomic<int> rec{0};
            const int r = rec++;
            if (r < 6) {
                CreateDirectoryW(ce_data_path(L"leonida-dump").c_str(), nullptr);
                const auto* w = reinterpret_cast<const std::uint64_t*>(desc_in);
                wchar_t name[128];
                swprintf_s(name, L"%s\\pso%02d_record.bin", ce_data_path(L"leonida-dump").c_str(), r);
                HANDLE h = CreateFileW(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) { DWORD wr; WriteFile(h, desc_in, 0x160, &wr, nullptr); CloseHandle(h); }
                for (int i = 0; i < 6; ++i) {
                    const void* ptr = reinterpret_cast<const void*>(w[1 + 2 * i]);
                    const std::uint64_t len = w[2 + 2 * i];
                    if (!ptr || !len || len > (1u << 24)) continue;
                    swprintf_s(name, L"%s\\pso%02d_blob%d.bin", ce_data_path(L"leonida-dump").c_str(), r, i);
                    h = CreateFileW(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (h != INVALID_HANDLE_VALUE) { DWORD wr; WriteFile(h, ptr, static_cast<DWORD>(len), &wr, nullptr); CloseHandle(h); }
                }
            }
        }
        if (out) *out = nullptr;
        HRESULT hr = E_INVALIDARG;
        {
            std::lock_guard lock(g_pso_mu);
            xs_pso_load();
            const auto* w = reinterpret_cast<const std::uint64_t*>(desc_in);
            const XsPsoShader* vs = nullptr;
            const XsPsoShader* ps = nullptr;
            bool missing = false;
            for (int i = 0; i < 6; ++i) {
                const auto* ptr = reinterpret_cast<const std::uint8_t*>(w[1 + 2 * i]);
                const std::uint64_t len = w[2 + 2 * i];
                if (!ptr || len < 64 || (i != 2 && i != 4)) continue;  // code blobs are #2 and #4
                const std::string hex = xs_hex24(ptr);
                auto it = g_pso_map.find(hex);
                if (it == g_pso_map.end()) { xs_pso_want(hex); missing = true; continue; }
                if (it->second.stage == "vs") vs = &it->second;
                else if (it->second.stage == "ps") ps = &it->second;
            }
            if (!missing && vs) {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC g{};
                g.pRootSignature = reinterpret_cast<ID3D12RootSignature*>(w[0]);
                g.VS = {vs->dxbc.data(), vs->dxbc.size()};
                g.InputLayout = {vs->elements.data(), static_cast<UINT>(vs->elements.size())};
                if (ps) g.PS = {ps->dxbc.data(), ps->dxbc.size()};
                for (auto& rt : g.BlendState.RenderTarget) rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                // HACK (agA3, LEONIDA_CE_SF_BLEND, default on, =0 off): this PSO is built from the shaders only, the
                // title's blend/depth state is not translated (every pipeline: blend off). Scaleform (VS names with
                // "Cxform") draws text and edge-AA fringes with alpha: give those normal alpha blending.
                static const bool sf_blend = [] { const char* v = std::getenv("LEONIDA_CE_SF_BLEND"); return v && *v == '1'; }();  // OFF by default: rd3 went black (the PS alpha is not what we assumed)
                if (sf_blend && vs->name.find("Cxform") != std::string::npos) {
                    auto& b = g.BlendState.RenderTarget[0];
                    b.BlendEnable = TRUE;
                    b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
                    b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                    b.BlendOp = D3D12_BLEND_OP_ADD;
                    b.SrcBlendAlpha = D3D12_BLEND_ONE;
                    b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                    b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                }
                char psostate[64] = "";
                // HACK (agA4, LEONIDA_CE_PSO_BLEND, default on, =0 off): blob #0 of the console record (0x17c/0x184 bytes)
                // carries GCN context registers at fixed offsets. +0xd0..+0xef = CB_BLEND0..7_CONTROL (0x80000000 =
                // blend off + DISABLE_ROP3; 0xe5040504 = SRC_ALPHA/INV_SRC_ALPHA, separate alpha, enable), +0xcc =
                // CB_TARGET_MASK (4 bits per RT), +0x3c = DB_DEPTH_CONTROL (logged only). Translate RT0 blend + mask.
                {
                    static const bool pso_blend = [] { const char* v = std::getenv("LEONIDA_CE_PSO_BLEND"); return !v || *v != '0'; }();
                    static const bool pso_wmask = [] { const char* v = std::getenv("LEONIDA_CE_PSO_WMASK"); return !v || *v != '0'; }();
                    const auto* b0 = reinterpret_cast<const std::uint8_t*>(w[1]);
                    const std::uint64_t b0len = w[2];
                    if (b0 && b0len >= 0x100 && b0len <= 0x400 && xs_readable(b0, 0x100)) {
                        std::uint32_t cb[8], tmask, dbc, r38;
                        std::memcpy(cb, b0 + 0xd0, 32);
                        std::memcpy(&tmask, b0 + 0xcc, 4);
                        std::memcpy(&dbc, b0 + 0x3c, 4);
                        std::memcpy(&r38, b0 + 0x38, 4);
                        const auto fac = [](std::uint32_t f, bool& ok) -> D3D12_BLEND {
                            switch (f) {
                            case 0: return D3D12_BLEND_ZERO; case 1: return D3D12_BLEND_ONE;
                            case 2: return D3D12_BLEND_SRC_COLOR; case 3: return D3D12_BLEND_INV_SRC_COLOR;
                            case 4: return D3D12_BLEND_SRC_ALPHA; case 5: return D3D12_BLEND_INV_SRC_ALPHA;
                            case 6: return D3D12_BLEND_DEST_ALPHA; case 7: return D3D12_BLEND_INV_DEST_ALPHA;
                            case 8: return D3D12_BLEND_DEST_COLOR; case 9: return D3D12_BLEND_INV_DEST_COLOR;
                            case 10: return D3D12_BLEND_SRC_ALPHA_SAT;
                            case 13: return D3D12_BLEND_BLEND_FACTOR; case 14: return D3D12_BLEND_INV_BLEND_FACTOR;
                            case 15: return D3D12_BLEND_SRC1_COLOR; case 16: return D3D12_BLEND_INV_SRC1_COLOR;
                            case 17: return D3D12_BLEND_SRC1_ALPHA; case 18: return D3D12_BLEND_INV_SRC1_ALPHA;
                            case 19: return D3D12_BLEND_ALPHA_FACTOR; case 20: return D3D12_BLEND_INV_ALPHA_FACTOR;
                            default: ok = false; return D3D12_BLEND_ONE;
                            }
                        };
                        const auto op = [](std::uint32_t f, bool& ok) -> D3D12_BLEND_OP {
                            switch (f) {
                            case 0: return D3D12_BLEND_OP_ADD; case 1: return D3D12_BLEND_OP_SUBTRACT;
                            case 2: return D3D12_BLEND_OP_MIN; case 3: return D3D12_BLEND_OP_MAX;
                            case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
                            default: ok = false; return D3D12_BLEND_OP_ADD;
                            }
                        };
                        const std::uint32_t c = cb[0];
                        bool ok = true;
                        D3D12_RENDER_TARGET_BLEND_DESC d = g.BlendState.RenderTarget[0];
                        if (c & (1u << 30)) {
                            d.BlendEnable = TRUE;
                            d.SrcBlend = fac(c & 31, ok);
                            d.BlendOp = op((c >> 5) & 7, ok);
                            d.DestBlend = fac((c >> 8) & 31, ok);
                            if (c & (1u << 29)) {
                                d.SrcBlendAlpha = fac((c >> 16) & 31, ok);
                                d.BlendOpAlpha = op((c >> 21) & 7, ok);
                                d.DestBlendAlpha = fac((c >> 24) & 31, ok);
                            } else {
                                d.SrcBlendAlpha = d.SrcBlend; d.BlendOpAlpha = d.BlendOp; d.DestBlendAlpha = d.DestBlend;
                            }
                            // colour factors are not valid for alpha in D3D12: use their alpha forms
                            const auto amap = [](D3D12_BLEND& f) {
                                if (f == D3D12_BLEND_SRC_COLOR) f = D3D12_BLEND_SRC_ALPHA;
                                else if (f == D3D12_BLEND_INV_SRC_COLOR) f = D3D12_BLEND_INV_SRC_ALPHA;
                                else if (f == D3D12_BLEND_DEST_COLOR) f = D3D12_BLEND_DEST_ALPHA;
                                else if (f == D3D12_BLEND_INV_DEST_COLOR) f = D3D12_BLEND_INV_DEST_ALPHA;
                                else if (f == D3D12_BLEND_SRC1_COLOR) f = D3D12_BLEND_SRC1_ALPHA;
                                else if (f == D3D12_BLEND_INV_SRC1_COLOR) f = D3D12_BLEND_INV_SRC1_ALPHA;
                                else if (f == D3D12_BLEND_BLEND_FACTOR) f = D3D12_BLEND_ALPHA_FACTOR;
                                else if (f == D3D12_BLEND_INV_BLEND_FACTOR) f = D3D12_BLEND_INV_ALPHA_FACTOR;
                            };
                            amap(d.SrcBlendAlpha); amap(d.DestBlendAlpha);
                        }
                        if (pso_blend && ok) g.BlendState.RenderTarget[0] = d;
                        if (pso_wmask) g.BlendState.RenderTarget[0].RenderTargetWriteMask = static_cast<UINT8>(tmask & 0xf);
                        std::snprintf(psostate, sizeof(psostate), " [cb0 %08x tm %x db %x]", cb[0], tmask & 0xf, dbc);
                        static std::atomic<int> dec_logged{0};
                        const bool sf = vs->name.find("Cxform") != std::string::npos || vs->name.find("TexTG") != std::string::npos;
                        if (dec_logged++ < 40 || (sf && dec_logged < 4000)) {
                            std::fprintf(stderr, "[psodec] %s + %s: len 0x%llx cb0 %08x cb1 %08x tmask %08x db %08x r38 %08x -> blend %d %d/%d op %d a %d/%d op %d wmask %x%s\n",
                                         vs->name.c_str(), ps ? ps->name.c_str() : "(no PS)", (unsigned long long)b0len, cb[0], cb[1], tmask, dbc, r38,
                                         int(d.BlendEnable), int(d.SrcBlend), int(d.DestBlend), int(d.BlendOp), int(d.SrcBlendAlpha),
                                         int(d.DestBlendAlpha), int(d.BlendOpAlpha), unsigned(g.BlendState.RenderTarget[0].RenderTargetWriteMask),
                                         ok ? "" : " (unknown factor: not applied)");
                            std::fflush(stderr);
                        }
                    }
                }
                g.SampleMask = UINT_MAX;
                g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                g.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                g.RasterizerState.DepthClipEnable = TRUE;
                g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                g.NumRenderTargets = 1;
                g.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                g.SampleDesc.Count = 1;
                hr = self->real->CreateGraphicsPipelineState(&g, *riid, out);
                if (SUCCEEDED(hr) && out && *out && !g_placeholder_pso.load()) {
                    // keep the first working pipeline as the stand-in for pipelines we cannot build (below)
                    static_cast<IUnknown*>(*out)->AddRef();
                    g_placeholder_pso = static_cast<IUnknown*>(*out);
                }
                if (SUCCEEDED(hr) && out && *out) {  // HACK diagnostic: pipeline -> shader names
                    std::lock_guard names_lock(g_pso_names_mu);
                    g_pso_names[*out] = vs->name + " + " + (ps ? ps->name : std::string("(no PS)")) + psostate;
                    if (vs->name.find("VBatch") != std::string::npos) {
                        const int k = g_leonida_sf_batch_n.load();
                        if (k < 16384) { g_leonida_sf_batch_psos[k].store(reinterpret_cast<std::uint64_t>(*out)); g_leonida_sf_batch_n.store(k + 1, std::memory_order_release); }
                    }
                }
                static std::atomic<int> logged{0};
                if (logged++ < 8 || FAILED(hr)) {
                    std::fprintf(stderr, "[hack] slot 57 pipeline %s + %s -> 0x%08lx\n", vs->name.c_str(),
                                 ps ? ps->name.c_str() : "(no PS)", (unsigned long)hr);
                    std::fflush(stderr);
                }
            }
        }
        // HACK (Leonida CE): a pipeline we cannot build (no vertex stage in the PSO map: the world's cable
        // pipelines carry their pixel shader in blob #2 and a merged geometry stage elsewhere) used to fail, and the
        // title kept a null definition record that crashed it later (+0xb5a5f8, +0x5392b80). Hand back the first
        // working pipeline instead: those draws come out wrong, but the title goes on. LEONIDA_CE_NO_PLACEHOLDER=1
        // keeps the failure.
        static const bool no_placeholder = [] {
            char v[8] = {};
            return GetEnvironmentVariableA("LEONIDA_CE_NO_PLACEHOLDER", v, sizeof(v)) && v[0] == '1';
        }();
        if (FAILED(hr) && out && !no_placeholder) {
            if (IUnknown* stand_in = g_placeholder_pso.load()) {
                const HRESULT qi = stand_in->QueryInterface(*riid, out);
                static std::atomic<int> told{0};
                if (told++ < 20) {
                    std::fprintf(stderr, "[hack] slot 57: no vertex stage for this pipeline; stand-in pipeline -> 0x%08lx\n",
                                 static_cast<unsigned long>(qi));
                    std::fflush(stderr);
                }
                if (SUCCEEDED(qi)) hr = S_OK;
            }
        }
        static std::atomic<int> ok{0}, bad{0};
        if (SUCCEEDED(hr)) {
            const int k = ok++;
            if (k < 3 || k % 500 == 0) { std::fprintf(stderr, "[hack] slot 57 CreateGraphicsPipelineState #%d ok (%llu)\n", k, (unsigned long long)desc->SizeInBytes); std::fflush(stderr); }
        } else if (bad++ < 10) {
            const auto* w = reinterpret_cast<const std::uint64_t*>(desc_in);
            std::fprintf(stderr, "[hack] slot 57 desc words: %llx %llx %llx %llx %llx %llx\n", (unsigned long long)w[0],
                         (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3],
                         (unsigned long long)w[4], (unsigned long long)w[5]);
            if (w[0] > 0x10000) { const auto* q = reinterpret_cast<const std::uint32_t*>(w[0]);
                std::fprintf(stderr, "[hack]   *w0: %08x %08x %08x %08x %08x %08x %08x %08x\n", q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]); }
            for (int i = 0; i < 6; ++i) {  // the six (pointer, length) blob pairs after the root signature
                const auto* ptr = reinterpret_cast<const std::uint8_t*>(w[1 + 2 * i]);
                const std::uint64_t len = w[2 + 2 * i];
                char head[40] = "-";
                if (ptr && len >= 16 && xs_readable(ptr, 16))
                    std::snprintf(head, sizeof(head), "%02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x", ptr[0], ptr[1],
                                  ptr[2], ptr[3], ptr[4], ptr[5], ptr[6], ptr[7], ptr[8], ptr[9], ptr[10], ptr[11]);
                std::string stage = "-";
                if (ptr && len >= 64 && xs_readable(ptr, 24)) {
                    char hex[49];
                    for (int b = 0; b < 24; ++b) std::snprintf(hex + 2 * b, 3, "%02x", ptr[b]);
                    const auto it = g_pso_map.find(std::string(hex, 48));
                    stage = it == g_pso_map.end() ? "not in map" : it->second.stage + " " + it->second.name;
                }
                std::fprintf(stderr, "[hack]   blob %d: len 0x%llx head %s map %s\n", i, static_cast<unsigned long long>(len),
                             head, stage.c_str());
            }
            std::fprintf(stderr, "[hack] slot 57 CreatePipelineState failed 0x%08lx (stream %llu bytes)\n", (unsigned long)hr, (unsigned long long)desc->SizeInBytes);
            std::fflush(stderr);
        }
        return hr_to_u64(hr);
    } else if constexpr (Slot == 70 || Slot == 71) {
        if (!xs_title_loaded()) {  // no title: PC slots 70/71, forwarded as any other PC slot
            note_slot(Slot);
            if (!self->real || !self->real_vtable) return 0x80004001ull;
            if (Slot >= self->slot_count) return hr_to_u64(refuse_beyond_device(Slot, self->slot_count));
            using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t);
            return reinterpret_cast<ForwardFn>(self->real_vtable[Slot])(self->real, a1, a2, a3, a4, a5, a6, a7, a8,
                                                                         a9, a10, a11, a12, a13, a14);
        }
        // Placed SRV (70) / UAV (71): (resource desc, view desc + title address at +40, handle).
        auto* rdesc = reinterpret_cast<const D3D12_RESOURCE_DESC*>(a1);
        const auto* vbytes = reinterpret_cast<const std::uint8_t*>(a2);
        const std::uint64_t handle = a3;
        if (!rdesc || !vbytes || handle < 0x10000) return 0;
        std::uint64_t va = 0;
        std::memcpy(&va, vbytes + 40, sizeof va);
        const XsPlaced* placed = va ? xs_find_placed(va) : nullptr;
        if (!placed && va) {  // not from slot 133: make a stand-in from the description and remember it
            D3D12_RESOURCE_DESC d = *rdesc;
            d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<unsigned>(d.Flags) & 0xffu);
            d.Layout = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                      : D3D12_TEXTURE_LAYOUT_UNKNOWN;
            if (Slot == 71) d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(d.Flags | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            void* created = nullptr;
            if (SUCCEEDED(self->real->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
                                                              nullptr, __uuidof(ID3D12Resource), &created)) && created) {
                const std::uint64_t bytes = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                    ? d.Width : self->real->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
                xs_record_placed(va, bytes, static_cast<ID3D12Resource*>(created), rdesc, true);  // agA13: owned
                placed = xs_find_placed(va);
            }
        }
        static std::atomic<int> logged{0};
        if (logged++ < 8) {
            std::fprintf(stderr, "[hack] placed %s: va=%llx dim=%d %llux%u fmt=%d -> %s\n", Slot == 70 ? "SRV" : "UAV",
                         (unsigned long long)va, (int)rdesc->Dimension, (unsigned long long)rdesc->Width, rdesc->Height,
                         (int)rdesc->Format, placed ? "resource" : "none");
            std::fflush(stderr);
        }
        if (!placed) return 0;
        const std::uint64_t offset = va - placed->va;
        if constexpr (Slot == 70) {
            D3D12_SHADER_RESOURCE_VIEW_DESC v{};
            std::memcpy(&v, vbytes, sizeof v);
            if (v.ViewDimension == D3D12_SRV_DIMENSION_BUFFER && offset) {
                const std::uint64_t stride = v.Buffer.StructureByteStride ? v.Buffer.StructureByteStride
                                             : (v.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) ? 4 : xs_format_bytes(v.Format);
                v.Buffer.FirstElement += offset / stride;
            }
            self->real->CreateShaderResourceView(placed->resource, &v, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(handle)});
            if (rdesc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
                xs_upload_placed_texture(self->real, *rdesc, placed->resource, placed->va);
        } else {
            D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
            std::memcpy(&v, vbytes, sizeof v);
            if (v.ViewDimension == D3D12_UAV_DIMENSION_BUFFER && offset) {
                const std::uint64_t stride = v.Buffer.StructureByteStride ? v.Buffer.StructureByteStride
                                             : (v.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) ? 4 : xs_format_bytes(v.Format);
                v.Buffer.FirstElement += offset / stride;
            }
            self->real->CreateUnorderedAccessView(placed->resource, nullptr, &v,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(handle)});
        }
        return 0;
    } else if constexpr (Slot == 1070) {
        // HACK: Xbox slot 70 takes three title structs (not PC CreatePlacedResource1); logged and ignored.
        static std::atomic<int> n{0};
        if (n++ < 3) {
            const auto* p1 = reinterpret_cast<const std::uint64_t*>(a1);
            const auto* p2 = reinterpret_cast<const std::uint64_t*>(a2);
            std::fprintf(stderr, "[hack] device slot 70 ignored: a1=%llx [%llx %llx %llx %llx %llx %llx %llx] a2=%llx [%llx %llx %llx %llx %llx %llx] a3=%llx\n",
                         (unsigned long long)a1, (unsigned long long)p1[0], (unsigned long long)p1[1],
                         (unsigned long long)p1[2], (unsigned long long)p1[3], (unsigned long long)p1[4],
                         (unsigned long long)p1[5], (unsigned long long)p1[6], (unsigned long long)a2,
                         (unsigned long long)p2[0], (unsigned long long)p2[1], (unsigned long long)p2[2],
                         (unsigned long long)p2[3], (unsigned long long)p2[4], (unsigned long long)p2[5],
                         (unsigned long long)a3);
            std::fflush(stderr);
        }
        return 0;
    } else if constexpr (Slot == 133) {
        // HACK stand-in (docs/retail_boot_chain.md 3.4): CreatePlacedResourceX(va, desc, state, clear, riid, out).
        // A committed DEFAULT-heap resource with PC flags only; not tied to va, so CPU writes there never reach it.
        // Without a title (Leonida CE gate) slot 133 keeps its documented path: the placed-at-VA handler, or
        // E_NOTIMPL when none is installed.
        if (!xs_title_loaded())
            return hr_to_u64(proxy_create_placed_resource_x(self, reinterpret_cast<void*>(a1),
                                                            reinterpret_cast<const D3D12_RESOURCE_DESC*>(a2),
                                                            static_cast<std::uint32_t>(a3),
                                                            reinterpret_cast<const IID*>(a5),
                                                            reinterpret_cast<void**>(a6)));
        auto* desc_in = reinterpret_cast<const D3D12_RESOURCE_DESC*>(a2);
        auto* riid = reinterpret_cast<const IID*>(a5);
        auto** out = reinterpret_cast<void**>(a6);
        if (!desc_in || !riid || !out) return hr_to_u64(E_POINTER);
        D3D12_RESOURCE_DESC d = *desc_in;
        d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<unsigned>(d.Flags) & 0xffu);
        xs_fix_pc_flags(d);
        d.Layout = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                  : D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        const ArenaWantHook want = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? g_arena_want_hook.load() : nullptr;
        if (want) want(a1);  // agA20 PLACED_IDENTITY (the hook ignores it unless the env is on)
        HRESULT hr = self->real->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr, *riid, out);
        if (want) want(0);
        if (FAILED(hr) && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
            // HACK: formats the PC layer cannot render to (A8_UNORM): keep the texture, drop the render-target bit
            d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(d.Flags & ~D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            hr = self->real->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
                                                     nullptr, *riid, out);
            std::fprintf(stderr, "[hack] slot 133 retry without RENDER_TARGET (fmt %d) -> 0x%08lx\n", (int)d.Format,
                         (unsigned long)hr);
        }
        if (SUCCEEDED(hr) && *out) {
            const std::uint64_t bytes = desc_in->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                ? desc_in->Width : self->real->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
            xs_record_placed(a1, bytes, static_cast<ID3D12Resource*>(*out), desc_in);
            xs_console_texture_address(self->real, *out, *desc_in, a1);
        }
        std::fprintf(stderr, "[hack] slot 133 CreatePlacedResourceX(va=%llx, dim=%d, width=%llu, flags=0x%x) -> 0x%08lx\n",
                     (unsigned long long)a1, (int)desc_in->Dimension, (unsigned long long)desc_in->Width,
                     (unsigned)desc_in->Flags, (unsigned long)hr);
        std::fflush(stderr);
        return hr_to_u64(hr);
    } else if constexpr (Slot == 62) {
        return hr_to_u64(proxy_create_compute_pipeline_state(
            self, reinterpret_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(a1), a2, a3,
            reinterpret_cast<const IID*>(a4), reinterpret_cast<void**>(a5)));
    } else if constexpr (Slot == 67) {
        return hr_to_u64(proxy_create_compute_pipeline_from_native_code(
            self, reinterpret_cast<const void*>(a1), reinterpret_cast<const IID*>(a2),
            reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 133) {
        return hr_to_u64(proxy_create_placed_resource_x(self, reinterpret_cast<void*>(a1),
                                                        reinterpret_cast<const D3D12_RESOURCE_DESC*>(a2),
                                                        static_cast<std::uint32_t>(a3), reinterpret_cast<const IID*>(a5),
                                                        reinterpret_cast<void**>(a6)));
    } else if constexpr (Slot == 76) {
        const HRESULT hr = proxy_create_resource(
            self, reinterpret_cast<const D3D12_HEAP_PROPERTIES*>(a1),
            static_cast<D3D12_HEAP_FLAGS>(static_cast<std::uint32_t>(a2)),
            reinterpret_cast<const D3D12_RESOURCE_DESC*>(a3),
            static_cast<D3D12_RESOURCE_STATES>(static_cast<std::uint32_t>(a4)),
            reinterpret_cast<const D3D12_CLEAR_VALUE*>(a5), reinterpret_cast<const IID*>(a6),
            reinterpret_cast<void**>(a7));
        if (SUCCEEDED(hr) && a3 && a7 && *reinterpret_cast<void**>(a7))
            xs_console_texture_address(self->real, *reinterpret_cast<void**>(a7),
                                       *reinterpret_cast<const D3D12_RESOURCE_DESC*>(a3), 0);
        return hr_to_u64(hr);
    } else {
        note_slot(Slot);
        // Xbox device slots, before the PC vtable is indexed: the public HLE's 78/79/80 would otherwise reach
        // CreateReservedResource2/CreateSampler2 or fall past an 80-slot table, and 48/73 would reach unrelated PC
        // methods. The check is before the slot-count refusal because slot 80 is outside a PC ID3D12Device11 table.
        const XboxDeviceSlotRoutes& routes = xbox_device_slot_routes();
        if (Slot == routes.frame_interval) {
            log_xbox_route_once(Slot, "SetFrameIntervalX", a1, a2, a3, a4, a5);
            return hr_to_u64(proxy_set_frame_interval_x(self, a1, a2, a3, a4));
        }
        if (Slot == routes.schedule_event) {
            log_xbox_route_once(Slot, "ScheduleFrameEventX", a1, a2, a3, a4, a5);
            return hr_to_u64(proxy_schedule_frame_event_x(self, a1, a2, a4));
        }
        if (Slot == routes.wait_event) {
            log_xbox_route_once(Slot, "WaitFrameEventX", a1, a2, a3, a4, a5);
            return hr_to_u64(proxy_wait_frame_event_x(self, a1, a2, a4, a5));
        }
        if (Slot == routes.queue_x) {
            log_xbox_route_once(Slot, "CreateCommandQueueX", a1, a2, a3, a4, a5);
            // HACK: Xbox queue flags above bit 0 (seen: 2, 3) have no PC meaning; keep DISABLE_GPU_TIMEOUT only.
            D3D12_COMMAND_QUEUE_DESC qd{};
            // Only a description that carries such flags is copied; a PC-valid one is passed through as given.
            if (a1 && (static_cast<unsigned>(reinterpret_cast<const D3D12_COMMAND_QUEUE_DESC*>(a1)->Flags) & ~1u)) {
                qd = *reinterpret_cast<const D3D12_COMMAND_QUEUE_DESC*>(a1);
                std::fprintf(stderr, "[hack] CreateCommandQueueX type=%d flags=0x%x -> 0x%x\n", (int)qd.Type,
                             (unsigned)qd.Flags, (unsigned)qd.Flags & 1u);
                qd.Flags = static_cast<D3D12_COMMAND_QUEUE_FLAGS>(static_cast<unsigned>(qd.Flags) & 1u);
                a1 = reinterpret_cast<std::uint64_t>(&qd);
            }
            return hr_to_u64(proxy_create_command_queue(self, reinterpret_cast<const D3D12_COMMAND_QUEUE_DESC*>(a1),
                                                        reinterpret_cast<const IID*>(a2),
                                                        reinterpret_cast<void**>(a3)));
        }
        if (Slot == routes.list_x) {
            log_xbox_route_once(Slot, "CreateCommandListX", a1, a2, a3, a4, a5);
            return hr_to_u64(proxy_create_command_list_x(self, a1, a2, a3, reinterpret_cast<const IID*>(a4),
                                                         reinterpret_cast<void**>(a5)));
        }
        if (!self->real || !self->real_vtable) {  // HACK diagnostic: not a live DeviceProxy
            auto* q = reinterpret_cast<std::uint64_t*>(self);
            std::fprintf(stderr, "[hack] proxy slot %u on %p with no real device; caller %p; words %llx %llx %llx %llx %llx %llx; "
                                 "a1=%llx a2=%llx a3=%llx a4=%llx\n", Slot, (void*)self, _ReturnAddress(),
                         (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2],
                         (unsigned long long)q[3], (unsigned long long)q[4], (unsigned long long)q[5],
                         (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3, (unsigned long long)a4);
            std::fflush(stderr);
            return 0x80004001ull;
        }
        if (Slot >= self->slot_count) {  // HACK diagnostic: who calls past the PC vtable, with what (first 3 calls)
            static std::atomic<int> logged{0};
            if (logged++ < 3) std::fprintf(stderr, "[hack] device slot %u beyond PC: caller %p a1=%llx a2=%llx a3=%llx a4=%llx a5=%llx a6=%llx\n",
                         Slot, _ReturnAddress(), (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3,
                         (unsigned long long)a4, (unsigned long long)a5, (unsigned long long)a6);
            std::fflush(stderr);
            return hr_to_u64(refuse_beyond_device(Slot, self->slot_count));
        }

        // The PC vtable entry, called with the PC device as `this`. Integer/pointer arguments, the hidden
        // struct-return pointer and any stack arguments keep their positions; XMM registers are untouched.
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t);
        auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot]);
        const std::uint64_t r = fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
        if ((Slot < 17 || Slot > 24) && static_cast<std::int32_t>(r) < 0 && (static_cast<std::uint32_t>(r) >> 16) >= 0x8000u) {  // HACK diagnostic
            static std::atomic<int> n{0};
            if (n++ < 40) {
                std::fprintf(stderr, "[hack] device slot %u failed 0x%08x  a1=%llx a2=%llx a3=%llx a4=%llx caller %p\n", Slot,
                             static_cast<unsigned>(r), (unsigned long long)a1, (unsigned long long)a2,
                             (unsigned long long)a3, (unsigned long long)a4, _ReturnAddress());
                std::fflush(stderr);
            }
        }
        return r;
    }
}

template <std::size_t... I>
void fill_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&proxy_slot<static_cast<unsigned>(I)>) ), ...);
}

void** get_vtable() {
    static void** vtable = [] {
        static void* storage[kMaxSlots];
        fill_vtable(storage, std::make_index_sequence<kMaxSlots>{});
        return storage;
    }();
    return vtable;
}

bool supports(ID3D12Device* real, REFIID iid) {
    void* found = nullptr;
    const HRESULT hr = real->QueryInterface(iid, &found);
    if (FAILED(hr) || !found) return false;
    static_cast<IUnknown*>(found)->Release();
    return true;
}

// Highest device interface the PC runtime exposes; its cumulative slot count (from d3d12.h) bounds forwarding.
// Windows SDK 10.0.26100 defines ID3D12Device through ID3D12Device14 only; there is no Device15/Device16 header.
unsigned detect_slot_count(ID3D12Device* real) {
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device14))) return 83;
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device13))) return 82;
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device12))) return 81;
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device11))) return 80;
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device10))) return 79;
#endif
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device9))) return 76;
#endif
    if (supports(real, __uuidof(ID3D12Device8))) return 73;
    if (supports(real, __uuidof(ID3D12Device7))) return 68;
    if (supports(real, __uuidof(ID3D12Device6))) return 66;
    if (supports(real, __uuidof(ID3D12Device5))) return 65;
    if (supports(real, __uuidof(ID3D12Device4))) return 57;
    if (supports(real, __uuidof(ID3D12Device3))) return 51;
    if (supports(real, __uuidof(ID3D12Device2))) return 48;
    if (supports(real, __uuidof(ID3D12Device1))) return 47;
    return 44;
}

}  // namespace

bool snapshot_source(std::uint64_t va, std::uint64_t& title, std::uint64_t& size) {  // HACK (agA2)
    std::lock_guard l(g_snap_mu);
    auto it = g_snaps.upper_bound(va);
    if (it == g_snaps.begin()) return false;
    --it;
    if (va >= it->first + it->second.second) return false;
    title = it->second.first + (va - it->first);
    size = it->second.second - (va - it->first);
    return true;
}

std::uint64_t upload_snapshot(const void* data, std::uint64_t size) {  // HACK (agA2, LEONIDA_CE_RECT_VS)
    const std::uint64_t a = reinterpret_cast<std::uint64_t>(data);
    const std::uint64_t va = xs_gpu_address(a, size);
    return va == a ? 0 : va;
}

HRESULT create_device_proxy(ID3D12Device* real, REFIID riid, void** out) {
    if (!real) return E_POINTER;
    if (!out) return E_POINTER;
    *out = nullptr;

    DeviceProxy* proxy = new (std::nothrow) DeviceProxy;
    if (!proxy) return E_OUTOFMEMORY;
    proxy->vtable = get_vtable();
    proxy->real = real;
    proxy->real_vtable = *reinterpret_cast<void***>(real);
    proxy->slot_count = detect_slot_count(real);
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy->dxgi_vtable = get_dxgi_vtable();
    proxy->adapter_provider = nullptr;
    proxy->gpu_thread_priority.store(0, std::memory_order_relaxed);
    proxy->max_frame_latency.store(3, std::memory_order_relaxed);

    const unsigned slot_count = proxy->slot_count;
    HRESULT hr = proxy_query_interface(proxy, &riid, out);
    if (FAILED(hr)) {
        // No reference escaped, and the caller still owns its reference to `real`.
        delete proxy;
        return hr;
    }
    if (!d3d12_trace::enabled())
        std::fprintf(stderr, "[rt] d3d12_xs: device proxy %p wraps PC device %p (%u PC vtable slots)\n",
                     static_cast<void*>(proxy), static_cast<void*>(real), slot_count);
    proxy_release(proxy);  // drop the creation reference; `out` holds the caller's reference
    return S_OK;
}

bool is_device_proxy(const void* device) {
    return device && *reinterpret_cast<void* const*>(device) == reinterpret_cast<void*>(get_vtable());
}

void* device_proxy_from_dxgi_device(void* dxgi_device) {
    if (!dxgi_device || *static_cast<void***>(dxgi_device) != get_dxgi_vtable()) return nullptr;
    return dxgi_owner(dxgi_device);
}

void* device_proxy_primary(void* device) {
    if (is_device_proxy(device)) return device;
    return device_proxy_from_dxgi_device(device);
}

void set_texture_address_hook(TextureAddressHook hook) { g_texture_address_hook.store(hook, std::memory_order_release); }

// HACK (Leonida CE boot): GTA VI 1.0.2.1 only (the bytes are checked first). The audio function at game+0x5267720
// takes its object's lock (engine+0x3ced0) and, when the object's count at +0x52c is 0, returns from the branch at
// +0x5267747 without unlocking; every other path ends in the unlock (+0x4d9dab0), as does its sibling +0x5594650
// on the same zero count. Here the count is 0 (on the console it is evidently not by then), the lock stays with a
// job worker, the audio thread blocks on it forever, the per-frame command stream is never drained and overflows
// (about 100 s), and the title crashes on the error path. The branch is sent to the unlocking tail instead
// (+0x5267c7f: rcx = the object, frame popped, jmp unlock). Found with hacks/engine.gdb; see hacks/BOOT_NOTES.md.
// DIAGNOSTIC (Leonida CE boot): LEONIDA_CE_PROBE=<hex>,<hex>,... puts an int3 at each title address at device setup
// and logs the registers there (rcx rdx r8 r9 rbp r13 r14 and [r14+0x38]/[r14+0x3c], the row copy's), up to 60
// lines per probe; the instruction then runs (original byte back, trap flag, int3 again). Unlike gdb it keeps the
// title's timing, so paths that only run at full speed can be watched.
struct XsCodeProbe { std::uintptr_t at; std::uint8_t orig; std::atomic<int> hits{0}; };
XsCodeProbe g_code_probes[8];
int g_code_probe_count = 0;
thread_local XsCodeProbe* t_code_probe_step = nullptr;

void xs_probe_write(std::uintptr_t at, std::uint8_t byte) {
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(at), 1, PAGE_EXECUTE_READWRITE, &old);
    *reinterpret_cast<volatile std::uint8_t*>(at) = byte;
    VirtualProtect(reinterpret_cast<void*>(at), 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), 1);
}

LONG CALLBACK xs_probe_handler(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    auto* c = info->ContextRecord;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && t_code_probe_step) {
        xs_probe_write(t_code_probe_step->at, 0xCC);
        t_code_probe_step = nullptr;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    const auto at = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    for (int i = 0; i < g_code_probe_count; ++i) {
        XsCodeProbe& pr = g_code_probes[i];
        if (pr.at != at) continue;
        // HACK (Leonida CE, Windows): a probe on +0x1dc5ca1 (mov (%rax,%rcx,8),%rax: a definition-table read) waits up
        // to 10 s for a null entry to be registered before the read runs, and logs whether it appeared (a race with
        // registration) or not (never registered). The crash it guards is +0xb5a5f8 reading [null+0x68].
        // DIAGNOSTIC: LEONIDA_CE_SLOT_TRAP=1 with a probe on +0x247bd99 (rax = &cfg->c78[0] of a definition table just
        // deserialized) write-protects that page, so the code that later fills the record slots shows up in [trap].
        if (at == 0x14247bd99ull && !g_trap_page) {
            static const bool slot_trap = [] {
                char v[8] = {};
                return GetEnvironmentVariableA("LEONIDA_CE_SLOT_TRAP", v, sizeof(v)) && v[0] == '1';
            }();
            if (slot_trap) {
                g_slot_lo = static_cast<std::uintptr_t>(c->Rax);
                g_slot_hi = g_slot_lo + static_cast<std::uintptr_t>(c->Rcx) * 8;
                g_trap_page = static_cast<std::uintptr_t>(c->Rax) & ~std::uintptr_t(4095);
                AddVectoredExceptionHandler(1, &xs_trap_handler);
                DWORD old = 0;
                const BOOL ok = VirtualProtect(reinterpret_cast<void*>(g_trap_page), 4096, PAGE_READONLY, &old);
                std::fprintf(stderr, "[trap] slot trap on page %llx (c78[0] at %llx, %llu slots): %s\n",
                             static_cast<unsigned long long>(g_trap_page), static_cast<unsigned long long>(c->Rax),
                             static_cast<unsigned long long>(c->Rcx), ok ? "armed" : "VirtualProtect failed");
            }
        }
        if (at == 0x141dc5ca1ull || at == 0x145bb7c58ull) {
            // +0x1dc5ca1: mov (%rax,%rcx,8),%rax; +0x5bb7c58: mov (%rdx,%rax,8),%rax
            auto* slot = reinterpret_cast<volatile std::uint64_t*>(at == 0x141dc5ca1ull ? c->Rax + c->Rcx * 8
                                                                                         : c->Rdx + c->Rax * 8);
            if (xs_readable(const_cast<std::uint64_t*>(slot), 8) && *slot == 0) {
                int waited = 0;
                while (*slot == 0 && waited < 10000) { Sleep(1); ++waited; }
                static std::atomic<int> told{0};
                if (told++ < 20)
                    std::fprintf(stderr, "[wait] table %llx index %llu: %s after %d ms (tid %lx)\n",
                                 static_cast<unsigned long long>(c->Rax), static_cast<unsigned long long>(c->Rcx),
                                 *slot ? "registered" : "STILL NULL", waited, GetCurrentThreadId());
            }
        }
        static std::uint64_t range_lo = 0, range_hi = ~0ull;
        static const bool range_read = [] {
            char v[64] = {};
            if (GetEnvironmentVariableA("LEONIDA_CE_PROBE_RANGE", v, sizeof(v))) {
                char* end = nullptr;
                range_lo = std::strtoull(v, &end, 16);
                if (end && *end == '-') range_hi = std::strtoull(end + 1, nullptr, 16);
            }
            return true;
        }();
        (void)range_read;
        bool in_range = c->R8 >= range_lo && c->R8 < range_hi;
        // Failure-only probes (Leonida CE, the Windows null definition records): +0x1d65b83 logs only when the
        // record publisher +0x2476e20 returned false (al == 0); +0x247758b only when its slot 57 call failed (r14 < 0).
        if (at == 0x141d65b83ull) in_range = (c->Rax & 0xff) == 0;
        if (at == 0x14247758bull) in_range = static_cast<std::int32_t>(c->R14) < 0;
        if (at == 0x142476f9full) in_range = static_cast<std::int32_t>(c->Rbx) < 0;  // +0xfa09f0 failed
        // LEONIDA_CE_PROBE_STALE=1: at the walk's attach entry +0x183190 (rcx = child), log only a child that is
        // already dead: its first qword is the base vtable 0x146639ac0 (stored by +0x5390160 on destruction) or is
        // not an address in the title's read-only data (a free-list link or junk).
        static const bool stale_mode = [] {
            char v[8] = {};
            return GetEnvironmentVariableA("LEONIDA_CE_PROBE_STALE", v, sizeof(v)) && v[0] == '1';
        }();
        if (stale_mode && at == 0x140183190ull) {
            const auto* obj = reinterpret_cast<const std::uint64_t*>(c->Rcx);
            const std::uint64_t vt = xs_readable(obj, 8) ? obj[0] : 0;
            in_range = vt == 0x146639ac0ull || vt < 0x146000000ull || vt >= 0x147000000ull;
            if (in_range) {
                std::fprintf(stderr, "[stale] child %llx first qword %llx at attach (tid %lx)\n",
                             static_cast<unsigned long long>(c->Rcx), static_cast<unsigned long long>(vt),
                             GetCurrentThreadId());
                std::fflush(stderr);
            }
        }
        // DIAGNOSTIC (Leonida CE, agC): LEONIDA_CE_PROBE_TEX=1 with probes on +0x1335be6 (the generated-texture
        // request +0x1335a20 calls its generator's fill, vtable +0x40: rsi = the 0xa0 request node, rcx = generator,
        // rdx = the CPU image), +0x1335c05 (fill returned) and +0x133609a (the final call *0xb0 through the node that
        // crashes): logs the image's mips (w, h, pitch, size, data) against the node's address and the bytes the
        // fill writes (rows of generator width*16), and whether the node's vtable is still a title address.
        static const bool tex_mode = [] {
            char v[8] = {};
            if (GetEnvironmentVariableA("LEONIDA_CE_PROBE_TEX", v, sizeof(v)) && v[0] == '1') return true;
            char pv[256] = {};  // agW: or a trailing "tex" in the probe list
            return GetEnvironmentVariableA("LEONIDA_CE_PROBE", pv, sizeof(pv)) && std::strstr(pv, "tex") != nullptr;
        }();
        if (tex_mode && (at == 0x141335be6ull || at == 0x141335c05ull || at == 0x14133609aull)) {
            in_range = false;
            const auto* node = reinterpret_cast<const std::uint64_t*>(c->Rsi);
            const std::uint64_t nvt = xs_readable(node, 0xa0) ? node[0] : 0;
            const bool vt_ok = nvt >= 0x146000000ull && nvt < 0x147000000ull;
            static std::atomic<int> told{0};
            if (at == 0x141335be6ull) {
                const auto* img = reinterpret_cast<const std::uint8_t*>(c->Rdx);
                const auto* gen = reinterpret_cast<const std::uint8_t*>(c->Rcx);
                if (told++ < 400 && xs_readable(img, 0x40) && xs_readable(gen, 0x48)) {
                    const std::uint32_t fmt = *reinterpret_cast<const std::uint32_t*>(img);
                    const std::uint16_t cnt = *reinterpret_cast<const std::uint16_t*>(img + 10);
                    const auto* sub = *reinterpret_cast<const std::uint8_t* const*>(img + 0x10);
                    const std::uint32_t gw = *reinterpret_cast<const std::uint32_t*>(gen + 0x38);
                    const std::uint32_t gh = *reinterpret_cast<const std::uint32_t*>(gen + 0x3c);
                    std::fprintf(stderr, "[tex] fill tid %lx node %llx vt %llx gen %llx gvt %llx gen w %u h %u img fmt %x mips %u"
                                 " node+38 %llx\n", GetCurrentThreadId(), (unsigned long long)c->Rsi,
                                 (unsigned long long)nvt, (unsigned long long)c->Rcx,
                                 (unsigned long long)*reinterpret_cast<const std::uint64_t*>(gen),
                                 gw, gh, fmt, cnt, xs_readable(node, 0x40) ? (unsigned long long)node[7] : 0ull);
                    for (unsigned i = 0; i < cnt && i < 4 && xs_readable(sub + i * 0x20, 0x20); ++i) {
                        const auto* e = reinterpret_cast<const std::uint64_t*>(sub + i * 0x20);
                        const std::uint32_t w = static_cast<std::uint32_t>(e[0]), h = static_cast<std::uint32_t>(e[0] >> 32);
                        const std::uint64_t pitch = e[1], size = e[2], data = e[3];
                        const std::uint64_t end_w = gh ? data + (gh - 1) * pitch + std::uint64_t(gw) * 16 : data;
                        std::fprintf(stderr, "[tex]   mip %u w %u h %u pitch %llx size %llx data %llx..%llx writes to %llx%s%s\n",
                                     i, w, h, (unsigned long long)pitch, (unsigned long long)size,
                                     (unsigned long long)data, (unsigned long long)(data + size),
                                     (unsigned long long)end_w, end_w > data + size ? " OVERRUN" : "",
                                     (c->Rsi >= data && c->Rsi < end_w) ? " NODE-INSIDE-WRITE" : "");
                    }
                    std::fflush(stderr);
                }
            } else if (!vt_ok || told < 400) {
                if (!vt_ok || at == 0x14133609aull) {
                    ++told;
                    std::fprintf(stderr, "[tex] %s tid %lx node %llx vt %llx%s\n",
                                 at == 0x141335c05ull ? "after-fill" : "final-call", GetCurrentThreadId(),
                                 (unsigned long long)c->Rsi, (unsigned long long)nvt, vt_ok ? "" : " BAD VTABLE");
                    std::fflush(stderr);
                }
            }
        }
        // DIAGNOSTIC (Leonida CE, bw): the texture lock +0x1336320 calls lockobj->vtable[1](lockobj, tex, 0, mips) at
        // +0x1336364 and tests al at +0x1336367. Logs the lock object, its vtable, the target and the result.
        if (tex_mode && (at == 0x141336364ull || at == 0x141336367ull)) {
            in_range = false;
            static std::atomic<int> told_lk{0};
            if (told_lk++ < 200) {
                if (at == 0x141336364ull) {
                    const auto* o = reinterpret_cast<const std::uint64_t*>(c->Rcx);
                    const std::uint64_t vt = xs_readable(o, 0x40) ? o[0] : 0;
                    const auto* v = reinterpret_cast<const std::uint64_t*>(vt);
                    std::fprintf(stderr, "[lock] call tid %lx obj %llx vt %llx fn %llx tex %llx r8 %llx r9 %llx obj+8..+38 %llx %llx %llx %llx %llx %llx %llx\n",
                                 GetCurrentThreadId(), (unsigned long long)c->Rcx, (unsigned long long)vt,
                                 xs_readable(v, 16) ? (unsigned long long)v[1] : 0ull, (unsigned long long)c->Rdx,
                                 (unsigned long long)c->R8, (unsigned long long)c->R9,
                                 vt ? (unsigned long long)o[1] : 0ull, vt ? (unsigned long long)o[2] : 0ull,
                                 vt ? (unsigned long long)o[3] : 0ull, vt ? (unsigned long long)o[4] : 0ull,
                                 vt ? (unsigned long long)o[5] : 0ull, vt ? (unsigned long long)o[6] : 0ull,
                                 vt ? (unsigned long long)o[7] : 0ull);
                } else {
                    std::fprintf(stderr, "[lock] ret tid %lx al %llx rcx %llx rdx %llx\n", GetCurrentThreadId(),
                                 (unsigned long long)(c->Rax & 0xff), (unsigned long long)c->Rcx,
                                 (unsigned long long)c->Rdx);
                }
                std::fflush(stderr);
            }
        }
        if (in_range && pr.hits++ < 60) {
            {
                char chain[256];
                int n = 0;
                const auto* sp = reinterpret_cast<const std::uint64_t*>(c->Rsp);
                int found = 0;
                for (int i = 0; i < 256 && found < 8 && n < static_cast<int>(sizeof(chain)) - 24; ++i) {
                    if (!xs_readable(sp + i, 8)) break;
                    const std::uint64_t v = sp[i];
                    if (v >= 0x140001000ull && v < 0x146000000ull) {
                        n += std::snprintf(chain + n, sizeof(chain) - n, " G+%llx",
                                           static_cast<unsigned long long>(v - 0x140000000ull));
                        ++found;
                    }
                }
                chain[n] = 0;
                std::fprintf(stderr, "[probe] G+%llx callers%s\n", static_cast<unsigned long long>(at - 0x140000000ull),
                             chain);
            }
            const auto* hdr = reinterpret_cast<const std::uint64_t*>(c->R15 ? c->R15 - 0x20 : 0);
            if (xs_readable(hdr, 0x40))
                std::fprintf(stderr, "[probe] G+%llx r15=%llx rsi=%llx  [r15-20..+18]= %llx %llx %llx %llx | %llx %llx %llx %llx\n",
                             static_cast<unsigned long long>(at - 0x140000000ull), static_cast<unsigned long long>(c->R15),
                             static_cast<unsigned long long>(c->Rsi),
                             static_cast<unsigned long long>(hdr[0]), static_cast<unsigned long long>(hdr[1]),
                             static_cast<unsigned long long>(hdr[2]), static_cast<unsigned long long>(hdr[3]),
                             static_cast<unsigned long long>(hdr[4]), static_cast<unsigned long long>(hdr[5]),
                             static_cast<unsigned long long>(hdr[6]), static_cast<unsigned long long>(hdr[7]));
            {
                const auto* vt = reinterpret_cast<const std::uint64_t*>(c->R10);
                const unsigned long long target = xs_readable(vt ? vt + 8 : nullptr, 8) ? vt[8] : 0ull;
                std::fprintf(stderr, "[probe] G+%llx rax=%llx r10=%llx [r10+40]=%llx r12=%llx rbx=%llx rdi=%llx\n",
                             static_cast<unsigned long long>(at - 0x140000000ull), static_cast<unsigned long long>(c->Rax),
                             static_cast<unsigned long long>(c->R10), target, static_cast<unsigned long long>(c->R12),
                             static_cast<unsigned long long>(c->Rbx), static_cast<unsigned long long>(c->Rdi));
            }
            {
                // a virtual call site: rax is the vtable, print the slot [rax+0x70] it is about to call
                const auto* vt = reinterpret_cast<const std::uint64_t*>(c->Rax);
                const bool vok = xs_readable(vt ? vt + 14 : nullptr, 8);
                {
                    const auto* q = reinterpret_cast<const std::uint64_t*>(c->Rdx);
                    if (xs_readable(q, 32))
                        std::fprintf(stderr, "[probe] G+%llx [rdx]= %llx %llx %llx %llx rcx=%llx\n",
                                     static_cast<unsigned long long>(at - 0x140000000ull),
                                     static_cast<unsigned long long>(q[0]), static_cast<unsigned long long>(q[1]),
                                     static_cast<unsigned long long>(q[2]), static_cast<unsigned long long>(q[3]),
                                     static_cast<unsigned long long>(c->Rcx));
                }
                std::fprintf(stderr, "[probe] G+%llx rax=%llx [rax+70]=%llx\n",
                             static_cast<unsigned long long>(at - 0x140000000ull), static_cast<unsigned long long>(c->Rax),
                             vok ? static_cast<unsigned long long>(vt[14]) : 0ull);
            }
            const auto* item = reinterpret_cast<const std::uint32_t*>(c->R14);
            const bool ok = xs_readable(item ? item + 14 : nullptr, 8);
            std::fprintf(stderr,
                         "[probe] G+%llx tid %lx rcx=%llx rdx=%llx r8=%llx r9=%llx rbp=%llx r13=%llx r14=%llx [r14+38]=%d "
                         "[r14+3c]=%d\n",
                         static_cast<unsigned long long>(at - 0x140000000ull), GetCurrentThreadId(),
                         static_cast<unsigned long long>(c->Rcx), static_cast<unsigned long long>(c->Rdx),
                         static_cast<unsigned long long>(c->R8), static_cast<unsigned long long>(c->R9),
                         static_cast<unsigned long long>(c->Rbp), static_cast<unsigned long long>(c->R13),
                         static_cast<unsigned long long>(c->R14), ok ? static_cast<int>(item[14]) : -1,
                         ok ? static_cast<int>(item[15]) : -1);
            std::fflush(stderr);
        }
        xs_probe_write(at, pr.orig);  // run the real instruction once
        t_code_probe_step = &pr;
        c->Rip = at;
        c->EFlags |= 0x100;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// HACK (Leonida CE, Windows): +0xb5a5d0 interns a definition record and reads [record+0x68] before any null check;
// on native Windows a slot of a definition table can still be empty when a job asks for it (+0x5bb7cc9,
// +0x1dc5caf), and the title faults at +0xb5a5f8 (20-100 s). Make that call return 0 instead: undo the prologue
// (8 pushes, sub 0x58) and return to the caller with rax = 0. LEONIDA_CE_KEEP_NULLDEF=1 leaves the fault alone.
LONG CALLBACK xs_null_definition_handler(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = info->ContextRecord;
    if (c->Rip != 0x140b5a5f8ull || c->Rax != 0) return EXCEPTION_CONTINUE_SEARCH;
    const auto* p = reinterpret_cast<const std::uint64_t*>(c->Rsp + 0x58);
    if (!xs_readable(p, 9 * 8)) return EXCEPTION_CONTINUE_SEARCH;
    c->Rbx = p[0]; c->Rbp = p[1]; c->Rdi = p[2]; c->Rsi = p[3];
    c->R12 = p[4]; c->R13 = p[5]; c->R14 = p[6]; c->R15 = p[7];
    c->Rip = p[8];
    c->Rsp = c->Rsp + 0x58 + 9 * 8;
    c->Rax = 0;
    static std::atomic<int> told{0};
    if (told++ < 20) {
        std::fprintf(stderr, "[hack] null definition at +0xb5a5f8: returned 0 to G+%llx (tid %lx)\n",
                     static_cast<unsigned long long>(c->Rip - 0x140000000ull), GetCurrentThreadId());
        std::fflush(stderr);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// EXPERIMENT (Leonida CE boot): the startup task graph's node 0xB198EC07 (body 0x14220F4C0) calls 0x1422082D0(2),
// which never returns: it hangs inside 0x142208CA0 (called at 0x1422085D9), so every node depending on it starves,
// the boot state machine never starts and the frontend stays off (black screen). LEONIDA_CE_SKIP_B198=1 replaces that
// call with a 5-byte NOP to test whether the boot then proceeds (what the skipped init sets up is unknown).
void xs_patch_skip_b198() {
    char v[8] = {};
    if (!GetEnvironmentVariableA("LEONIDA_CE_SKIP_B198", v, sizeof(v)) || v[0] != '1') return;
    auto* at = reinterpret_cast<std::uint8_t*>(0x1422085d9ull);
    static const std::uint8_t expect[5] = {0xe8, 0xc2, 0x06, 0x00, 0x00};
    static const std::uint8_t nop5[5] = {0x0f, 0x1f, 0x44, 0x00, 0x00};
    if (!xs_readable(at, 5) || std::memcmp(at, expect, 5) != 0) {
        std::fprintf(stderr, "[hack] skip-b198: bytes differ, not applied\n");
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    std::memcpy(at, nop5, 5);
    VirtualProtect(at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    std::fprintf(stderr, "[hack] skip-b198: call at game+0x22085d9 (to +0x2208ca0) skipped\n");
}

void xs_install_probes() {
    xs_patch_skip_b198();
    {
        char k[8] = {};
        if (!(GetEnvironmentVariableA("LEONIDA_CE_KEEP_NULLDEF", k, sizeof(k)) && k[0] == '1'))
            AddVectoredExceptionHandler(1, &xs_null_definition_handler);
    }
    char v[256] = {};
    if (!GetEnvironmentVariableA("LEONIDA_CE_PROBE", v, sizeof(v))) return;
    AddVectoredExceptionHandler(1, &xs_probe_handler);
    for (char* p = v; *p && g_code_probe_count < 8;) {
        char* end = nullptr;
        const std::uintptr_t at = static_cast<std::uintptr_t>(std::strtoull(p, &end, 16));
        if (end == p) break;
        if (at >= 0x140001000ull && at < 0x146000000ull && xs_readable(reinterpret_cast<void*>(at), 1)) {
            XsCodeProbe& pr = g_code_probes[g_code_probe_count++];
            pr.at = at;
            pr.orig = *reinterpret_cast<const std::uint8_t*>(at);
            xs_probe_write(at, 0xCC);
            std::fprintf(stderr, "[probe] armed G+%llx (byte %02x)\n", static_cast<unsigned long long>(at - 0x140000000ull),
                         pr.orig);
        }
        p = (*end == ',') ? end + 1 : end;
    }
}

// HACK (Leonida CE, agW): the title's CPU-image pitch function +0x1555c10 (and the size function +0x1555b60 built on
// it) returns 0 for staging format 0x0a (R32G32B32A32: word 0x10000a). The generated-texture request +0x1335a20 falls
// back to a CPU staging image when it cannot map the texture itself (+0x1336320 -> lock fails), allocates 0 bytes for
// the 256x1 float4 colour ramp, and the fill +0x550e2d0 writes 4 KiB over the request node and its neighbours: the
// crash at +0x13360a3 (vtable 0x3f8000003f800000), the bad frees and +0x538d68e. Patch +0x1555c29 to jump to a stub
// that answers width*16 for format 0x0a. LEONIDA_CE_NO_FMT10_PITCH=1 leaves it.
void xs_patch_fmt10_pitch() {
    char v[8] = {};
    if (GetEnvironmentVariableA("LEONIDA_CE_NO_FMT10_PITCH", v, sizeof(v)) && v[0] == '1') return;
    auto* at = reinterpret_cast<std::uint8_t*>(0x141555c29ull);
    static const std::uint8_t expect[10] = {0x83, 0xc0, 0xfd, 0x83, 0xf8, 0x02, 0x72, 0x55, 0x31, 0xd2};
    if (!xs_readable(at, 10) || std::memcmp(at, expect, 10) != 0) {
        std::fprintf(stderr, "[hack] fmt 0x0a pitch patch: bytes differ, not applied\n");
        return;
    }
    std::uint8_t* stub = nullptr;
    for (std::uintptr_t a = 0x13f000000ull; a > 0xc1000000ull && !stub; a -= 0x10000)
        stub = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(a), 4096, MEM_COMMIT | MEM_RESERVE,
                                                       PAGE_EXECUTE_READWRITE));
    if (!stub) {
        std::fprintf(stderr, "[hack] fmt 0x0a pitch patch: no stub page near the image\n");
        return;
    }
    const std::uint8_t code[] = {
        0x83, 0xf8, 0x0a,              // cmp eax, 10   (eax = format & 0xfff here)
        0x75, 0x06,                    // jne +6
        0xc1, 0xe2, 0x04,              // shl edx, 4    (width * 16)
        0x89, 0xd0,                    // mov eax, edx
        0xc3,                          // ret
        0x83, 0xc0, 0xfd,              // add eax, -3   (the replaced code)
        0x83, 0xf8, 0x02,              // cmp eax, 2
        0x73, 0x0e,                    // jae +14
        0xff, 0x25, 0, 0, 0, 0, 0x86, 0x5c, 0x55, 0x41, 0x01, 0, 0, 0,  // jmp 0x141555c86
        0xff, 0x25, 0, 0, 0, 0, 0x31, 0x5c, 0x55, 0x41, 0x01, 0, 0, 0,  // jmp 0x141555c31
    };
    std::memcpy(stub, code, sizeof(code));
    FlushInstructionCache(GetCurrentProcess(), stub, sizeof(code));
    std::uint8_t patch[8] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const std::int32_t rel = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(stub) - 0x141555c2ell);
    std::memcpy(patch + 1, &rel, 4);
    DWORD old_protect = 0;
    if (!VirtualProtect(at, 8, PAGE_EXECUTE_READWRITE, &old_protect)) return;
    std::memcpy(at, patch, 8);
    VirtualProtect(at, 8, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), at, 8);
    std::fprintf(stderr, "[hack] fmt 0x0a pitch patch applied (game+0x1555c29 -> stub %p)\n", static_cast<void*>(stub));
}

void xs_patch_audio_lock_leak() {
    auto* at = reinterpret_cast<std::uint8_t*>(0x145267747ull);
    static const std::uint8_t expect[6] = {0x0f, 0x84, 0xae, 0x05, 0x00, 0x00};  // je +0x5ae (to +0x5267cfb)
    if (!xs_readable(at, 6) || std::memcmp(at, expect, 6) != 0) {
        std::fprintf(stderr, "[hack] audio lock patch: bytes differ, not applied\n");
        return;
    }
    DWORD old_protect = 0;
    if (!VirtualProtect(at, 6, PAGE_EXECUTE_READWRITE, &old_protect)) return;
    const std::int32_t rel = static_cast<std::int32_t>(0x145267c7fll - 0x14526774dll);  // je to the unlocking tail
    std::memcpy(at + 2, &rel, 4);
    VirtualProtect(at, 6, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), at, 6);
    std::fprintf(stderr, "[hack] audio lock patch applied (game+0x5267747 -> +0x5267c7f)\n");
}

// HACK (Leonida CE boot): the lookup-table fill +0x550e2d0 copies a 256-entry float4 table (4 KiB) into a mapped
// texture whose CPU pointer lands right after a 0xa0-byte arena node; the copy overruns the following nodes and the
// arena itself and the title crashes at ~120-145 s (hacks/BOOT_NOTES.md). Until the undersized block is understood,
// skip both row copies (callq *0x38(%rsp) at +0x550e57e and +0x550e60f) so the table stays empty but nothing is
// overwritten. Opt-in with LEONIDA_CE_SKIP_LUT=1: it did not stop the crash (try137), so it is off by default.
void xs_patch_lut_fill() {
    char v[8] = {};
    if (!GetEnvironmentVariableA("LEONIDA_CE_SKIP_LUT", v, sizeof(v)) || v[0] != '1') return;
    static const std::uint8_t expect[4] = {0xff, 0x54, 0x24, 0x38};  // callq *0x38(%rsp)
    static const std::uint8_t nop4[4] = {0x0f, 0x1f, 0x40, 0x00};
    for (const std::uintptr_t a : {std::uintptr_t{0x14550e57eull}, std::uintptr_t{0x14550e60full}}) {
        auto* at = reinterpret_cast<std::uint8_t*>(a);
        if (!xs_readable(at, 4) || std::memcmp(at, expect, 4) != 0) {
            std::fprintf(stderr, "[hack] lut fill patch: bytes differ at game+%llx, not applied\n",
                         static_cast<unsigned long long>(a - 0x140000000ull));
            continue;
        }
        DWORD old_protect = 0;
        if (!VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &old_protect)) continue;
        std::memcpy(at, nop4, 4);
        VirtualProtect(at, 4, old_protect, &old_protect);
        FlushInstructionCache(GetCurrentProcess(), at, 4);
        std::fprintf(stderr, "[hack] lut fill patch applied at game+%llx\n",
                     static_cast<unsigned long long>(a - 0x140000000ull));
    }
}

// HACK (Leonida CE boot): the 120-140 s stop. The recursive walk +0x183aaf/+0x18435f allocates its 0xa0/0xe0-byte
// nodes with allocator slot +0x70 (+0x538d040: "allocate in the pool that owns this hint pointer"), passing an older
// node as the hint. Those older nodes were built inside a job whose scratch arena lives in the job's stack frame
// (+0x45e1170 builds it with +0xcf86d0, destroys it with +0x2101350) and is gone by the time the walk runs, so the
// pool lookup reads freed memory (+0x538d0e6, +0x538d68e, +0x515acf5). Call slot +0x50 (plain allocate: rcx = pool,
// rdx = size) at both sites instead: the nodes land in the default pool, no hint is looked up.
// Opt-in with LEONIDA_CE_NO_HINT=1: it moves the crash but does not stop it (try141).
void xs_patch_walk_alloc_hint() {
    char v[8] = {};
    if (!GetEnvironmentVariableA("LEONIDA_CE_NO_HINT", v, sizeof(v)) || v[0] != '1') return;
    struct Site {
        std::uintptr_t size_at;  // mov $size, %r8d (6 bytes) -> mov $size, %edx; nop
        std::uintptr_t hint_at;  // mov %reg, %rdx (3 bytes) -> nop
        std::uintptr_t call_at;  // callq *0x70(%rax) -> callq *0x50(%rax)
        std::uint8_t size;
        std::uint8_t hint_modrm;
    };
    static const Site sites[] = {{0x140182b05ull, 0x140182b0bull, 0x140182b18ull, 0xa0, 0xfa},
                                 {0x140182bc6ull, 0x140182bccull, 0x140182bd9ull, 0xe0, 0xf2}};
    for (const Site& site : sites) {
        auto* sz = reinterpret_cast<std::uint8_t*>(site.size_at);
        auto* hn = reinterpret_cast<std::uint8_t*>(site.hint_at);
        auto* cl = reinterpret_cast<std::uint8_t*>(site.call_at);
        const std::uint8_t want_sz[6] = {0x41, 0xb8, site.size, 0x00, 0x00, 0x00};
        const std::uint8_t want_hn[3] = {0x48, 0x89, site.hint_modrm};
        const std::uint8_t want_cl[3] = {0xff, 0x50, 0x70};
        if (!xs_readable(sz, 6) || !xs_readable(cl, 3) || std::memcmp(sz, want_sz, 6) != 0 ||
            std::memcmp(hn, want_hn, 3) != 0 || std::memcmp(cl, want_cl, 3) != 0) {
            std::fprintf(stderr, "[hack] walk alloc patch: bytes differ at game+%llx, not applied\n",
                         static_cast<unsigned long long>(site.size_at - 0x140000000ull));
            continue;
        }
        DWORD old_protect = 0;
        if (!VirtualProtect(sz, site.call_at + 3 - site.size_at, PAGE_EXECUTE_READWRITE, &old_protect)) continue;
        const std::uint8_t new_sz[6] = {0xba, site.size, 0x00, 0x00, 0x00, 0x90};  // mov $size, %edx; nop
        const std::uint8_t nop3[3] = {0x0f, 0x1f, 0x00};
        std::memcpy(sz, new_sz, 6);
        std::memcpy(hn, nop3, 3);
        cl[2] = 0x50;
        VirtualProtect(sz, site.call_at + 3 - site.size_at, old_protect, &old_protect);
        FlushInstructionCache(GetCurrentProcess(), sz, site.call_at + 3 - site.size_at);
        std::fprintf(stderr, "[hack] walk alloc patch applied at game+%llx (size 0x%x, no hint)\n",
                     static_cast<unsigned long long>(site.size_at - 0x140000000ull), site.size);
    }
}

// HACK (Leonida CE boot): the job context +0xcf86d0 / ~+0x2101350 keeps a scratch arena at +0x120 whose chunks are
// freed by +0x15a0630 when the context is destroyed, but nodes the walk +0x183aaf keeps using were built in those
// chunks, and the walk later writes floats into memory the pool has already reused (the 120-140 s stop). Skip that
// release in the destructor (call at +0x21013dd): the chunks leak, the nodes stay valid.
// Opt-in with LEONIDA_CE_LEAK_SCRATCH=1: it does not stop the crash either (try142).
void xs_patch_scratch_release() {
    char v[8] = {};
    if (!GetEnvironmentVariableA("LEONIDA_CE_LEAK_SCRATCH", v, sizeof(v)) || v[0] != '1') return;
    auto* at = reinterpret_cast<std::uint8_t*>(0x1421013ddull);
    const std::int32_t rel = static_cast<std::int32_t>(0x1415a0630ll - 0x1421013e2ll);
    std::uint8_t want[5] = {0xe8, 0, 0, 0, 0};
    std::memcpy(want + 1, &rel, 4);
    if (!xs_readable(at, 5) || std::memcmp(at, want, 5) != 0) {
        std::fprintf(stderr, "[hack] scratch release patch: bytes differ, not applied\n");
        return;
    }
    DWORD old_protect = 0;
    if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old_protect)) return;
    static const std::uint8_t nop5[5] = {0x0f, 0x1f, 0x44, 0x00, 0x00};
    std::memcpy(at, nop5, 5);
    VirtualProtect(at, 5, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    std::fprintf(stderr, "[hack] scratch release patch applied (game+0x21013dd)\n");
}

void set_console_arena(std::uint8_t* mapped, std::uint64_t arena_size, std::uint64_t ring_cpu, std::uint64_t ring_va,
                       std::uint64_t ring_size) {
    xs_patch_audio_lock_leak();
    xs_patch_fmt10_pitch();
    xs_patch_lut_fill();
    xs_patch_walk_alloc_hint();
    xs_patch_scratch_release();
    xs_install_probes();
    g_arena_mapped = mapped;
    g_arena_size = arena_size;
    g_ring_cpu = ring_cpu;
    g_ring_va = ring_va;
    g_ring_size = ring_size;
    std::fprintf(stderr, "[hack] console arena %p + %llx, upload ring at VA %llx (%llu MiB)\n", (void*)mapped,
                 (unsigned long long)arena_size, (unsigned long long)ring_va, (unsigned long long)(ring_size >> 20));
}

void set_console_descriptor_heap_factory(ConsoleDescriptorHeapFactory factory) {
    g_console_heap_factory = factory;
}

void set_arena_want_hook(ArenaWantHook hook) { g_arena_want_hook.store(hook); }
void set_heap_mirror_range(std::uint64_t lo, std::uint64_t hi) {
    g_mirror_hi.store(hi);
    g_mirror_lo.store(lo);
}

bool placed_title_of(const void* resource, std::uint64_t& va, std::uint64_t& size) {
    std::lock_guard lock(g_placed_mu);
    for (auto it = g_placed.rbegin(); it != g_placed.rend(); ++it)
        if (it->resource == resource) { va = it->va; size = it->size; return true; }
    return false;
}

bool placed_buffer_holds(std::uint64_t va, std::uint64_t bytes) {
    std::lock_guard lock(g_placed_mu);
    for (const auto& p : g_placed)
        if (p.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && va >= p.va && va - p.va + bytes <= p.size) return true;
    return false;
}

bool set_device_adapter_provider(void* device, DxgiAdapterProvider provider) {
    auto* proxy = static_cast<DeviceProxy*>(device_proxy_primary(device));
    if (!proxy) {
        // Not installing one silently would leave the host's adapter in its place.
        std::fprintf(stderr, "[rt] d3d12_xs: set_device_adapter_provider: %p is not a device proxy; no provider set\n",
                     device);
        return false;
    }
    proxy->adapter_provider = provider;
    return true;
}

bool is_heap_proxy(const void* heap) {
    return heap && *reinterpret_cast<void* const*>(heap) == reinterpret_cast<void*>(get_heap_vtable());
}

ID3D12DescriptorHeap* heap_proxy_real_heap(const void* heap) {
    if (!is_heap_proxy(heap)) return nullptr;
    return static_cast<const HeapProxy*>(heap)->real;
}

unsigned device_proxy_slot_count(const void* device) {
    if (!is_device_proxy(device)) return 0;
    return static_cast<const DeviceProxy*>(device)->slot_count;
}

void set_placed_at_va_handler(PlacedAtVaHandler handler) {
    g_placed_at_va.store(handler, std::memory_order_release);
}

void set_placed_at_va_pages_hook(PlacedAtVaPagesHook hook) {
    g_placed_at_va_pages.store(hook, std::memory_order_release);
}

ID3D12Device* device_proxy_real_device(const void* device) {
    if (!is_device_proxy(device)) return nullptr;
    return static_cast<const DeviceProxy*>(device)->real;
}

bool device_slot_overridden(unsigned slot) {
    if (slot <= 2 || slot == 8 || slot == 9 || slot == 14 || slot == 31 || slot == 32 || slot == 34 || slot == 35 ||
        slot == 36 || slot == 45 || slot == 46 || slot == 50 || slot == 62 || slot == 67 ||
        slot == 75 || slot == 76 || slot == 133)
        return true;
    const XboxDeviceSlotRoutes& routes = xbox_device_slot_routes();
    return slot == routes.frame_interval || slot == routes.schedule_event || slot == routes.wait_event ||
           slot == routes.queue_x || slot == routes.list_x;
}

bool is_queue_proxy(const void* queue) { return is_child_proxy<ChildKind::queue>(queue); }
bool is_fence_proxy(const void* fence) { return is_child_proxy<ChildKind::fence>(fence); }
bool is_allocator_proxy(const void* allocator) { return is_child_proxy<ChildKind::allocator>(allocator); }

ID3D12CommandAllocator* allocator_proxy_real_allocator(const void* allocator) {
    if (!is_allocator_proxy(allocator)) return nullptr;
    return reinterpret_cast<ID3D12CommandAllocator*>(static_cast<const ChildProxy*>(allocator)->real);
}

ID3D12CommandQueue* queue_proxy_real_queue(const void* queue) {
    if (!is_queue_proxy(queue)) return nullptr;
    return reinterpret_cast<ID3D12CommandQueue*>(static_cast<const ChildProxy*>(queue)->real);
}

ID3D12Fence* fence_proxy_real_fence(const void* fence) {
    if (!is_fence_proxy(fence)) return nullptr;
    return reinterpret_cast<ID3D12Fence*>(static_cast<const ChildProxy*>(fence)->real);
}

unsigned queue_proxy_slot_count(const void* queue) {
    return is_queue_proxy(queue) ? static_cast<const ChildProxy*>(queue)->slot_count : 0;
}

unsigned fence_proxy_slot_count(const void* fence) {
    return is_fence_proxy(fence) ? static_cast<const ChildProxy*>(fence)->slot_count : 0;
}

unsigned queue_proxy_table_size() { return kQueueSlots; }
unsigned fence_proxy_table_size() { return kFenceSlots; }
unsigned allocator_proxy_table_size() { return kAllocatorSlots; }
unsigned allocator_proxy_slot_count(const void* allocator) {
    return is_allocator_proxy(allocator) ? static_cast<const ChildProxy*>(allocator)->slot_count : 0;
}

unsigned long command_object_proxy_ref_count(const void* object) {
    if (!is_queue_proxy(object) && !is_fence_proxy(object) && !is_allocator_proxy(object)) return 0;
    return static_cast<const ChildProxy*>(object)->refs.load(std::memory_order_relaxed);
}

std::uint64_t queue_slot_hits(unsigned slot) {
    using S = ChildStats<ChildKind::queue>;
    start_hit_counts();
    return slot < S::kSlots ? S::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool queue_slot_first_call_seen(unsigned slot) {
    using S = ChildStats<ChildKind::queue>;
    return slot < S::kSlots && S::first[slot].load(std::memory_order_relaxed);
}

std::uint64_t fence_slot_hits(unsigned slot) {
    using S = ChildStats<ChildKind::fence>;
    start_hit_counts();
    return slot < S::kSlots ? S::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool fence_slot_first_call_seen(unsigned slot) {
    using S = ChildStats<ChildKind::fence>;
    return slot < S::kSlots && S::first[slot].load(std::memory_order_relaxed);
}

std::uint64_t queue_table_overruns() {
    return ChildStats<ChildKind::queue>::overruns.load(std::memory_order_relaxed);
}

std::uint64_t fence_table_overruns() {
    return ChildStats<ChildKind::fence>::overruns.load(std::memory_order_relaxed);
}

bool command_object_tables_guarded() {
    get_child_vtable<ChildKind::queue>();
    get_child_vtable<ChildKind::fence>();
    get_child_vtable<ChildKind::allocator>();
    return ChildStats<ChildKind::queue>::table_end.load(std::memory_order_acquire) &&
           ChildStats<ChildKind::fence>::table_end.load(std::memory_order_acquire) &&
           ChildStats<ChildKind::allocator>::table_end.load(std::memory_order_acquire);
}

std::uint64_t allocator_slot_hits(unsigned slot) {
    using S = ChildStats<ChildKind::allocator>;
    start_hit_counts();
    return slot < S::kSlots ? S::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool allocator_slot_first_call_seen(unsigned slot) {
    using S = ChildStats<ChildKind::allocator>;
    return slot < S::kSlots && S::first[slot].load(std::memory_order_relaxed);
}

std::uint64_t allocator_table_overruns() {
    return ChildStats<ChildKind::allocator>::overruns.load(std::memory_order_relaxed);
}

bool heap_slot_overridden(unsigned slot) {
    return slot <= 2 || slot == 7 || slot == 9 || slot == 10;
}

std::uint64_t device_slot_hits(unsigned slot) {
    start_hit_counts();
    return slot < kMaxSlots ? g_hits[slot].load(std::memory_order_relaxed) : 0;
}

bool device_slot_first_call_seen(unsigned slot) {
    return slot < kMaxSlots && g_first[slot].load(std::memory_order_relaxed);
}

unsigned long device_proxy_ref_count(const void* device) {
    if (!is_device_proxy(device)) return 0;
    return static_cast<const DeviceProxy*>(device)->refs.load(std::memory_order_relaxed);
}

std::uint64_t heap_slot_hits(unsigned slot) {
    start_hit_counts();
    return slot < kHeapSlots ? g_heap_hits[slot].load(std::memory_order_relaxed) : 0;
}

bool heap_slot_first_call_seen(unsigned slot) {
    return slot < kHeapSlots && g_heap_first[slot].load(std::memory_order_relaxed);
}

bool is_command_list_proxy(const void* list) {
    return list && *reinterpret_cast<void* const*>(list) == reinterpret_cast<void*>(get_list_vtable());
}

ID3D12GraphicsCommandList* command_list_proxy_real_list(const void* list) {
    if (!is_command_list_proxy(list)) return nullptr;
    return static_cast<const ListProxy*>(list)->real;
}

unsigned command_list_proxy_table_size() { return kListSlots; }

std::uint64_t list_slot_hits(unsigned slot) {
    start_hit_counts();
    return slot < kListSlots ? ListStats::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool list_slot_first_call_seen(unsigned slot) {
    return slot < kListSlots && ListStats::first[slot].load(std::memory_order_relaxed);
}

void reset_device_proxy_stats() {
    start_hit_counts();
    for (unsigned slot = 0; slot < kMaxSlots; ++slot) {
        g_hits[slot].store(0, std::memory_order_relaxed);
        g_first[slot].store(false, std::memory_order_relaxed);
        g_beyond_warned[slot].store(false, std::memory_order_relaxed);
    }
    for (unsigned slot = 0; slot < kHeapSlots; ++slot) {
        g_heap_hits[slot].store(0, std::memory_order_relaxed);
        g_heap_first[slot].store(false, std::memory_order_relaxed);
    }
    for (unsigned slot = 0; slot < kListSlots; ++slot) {
        ListStats::hits[slot].store(0, std::memory_order_relaxed);
        ListStats::first[slot].store(false, std::memory_order_relaxed);
    }
    reset_child_stats<ChildKind::queue>();
    reset_child_stats<ChildKind::fence>();
    reset_child_stats<ChildKind::allocator>();
}

}  // namespace rt::d3d12_xs
