#pragma once
// RTDIAG (Leonida CE hack): LEONIDA_CE_RTDIAG=1 counts draws per colour target, skipped draws and failed lists; the
// presenter prints and resets the counts every 300 presents.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ce_checkpoints.h"  // agA32 pipeline names per target

namespace leonida::rtdiag {
inline bool on() {
    static const bool v = [] {
        const char* e = std::getenv("LEONIDA_CE_RTDIAG");
        return e && e[0] == '1';
    }();
    return v;
}
struct ImageInfo {
    std::uint64_t address = 0;
    std::uint32_t w = 0, h = 0, fmt = 0, flags = 0;
};
struct State {
    std::mutex m;
    std::unordered_map<std::uintptr_t, std::uint64_t> target_draws, depth_draws;
    std::unordered_map<std::uintptr_t, std::unordered_map<std::uint64_t, std::uint64_t>> target_pipes;  // agA32
    std::unordered_map<std::uintptr_t, ImageInfo> images;
    std::unordered_map<std::uint64_t, std::uintptr_t> by_address;
    std::uint64_t draws = 0, pending = 0, variant = 0, no_target = 0, lists_ok = 0, lists_failed = 0, dispatches = 0;
    std::uint64_t fail[16] = {};
    std::uint64_t draw_fail[16] = {};
    std::vector<std::pair<std::uint64_t, std::string>> want;  // agA34 DRAW_SRV: T# addresses to probe at the dump
    // agA39: drawn draws that bind a DSV, of those the ones whose pipeline has no depth format (the DSV is dropped),
    // draws that bind more render targets than their pipeline has (the extra ones are dropped), and per pipeline.
    std::uint64_t dsv_bound = 0, depth_dropped = 0, mrt_dropped = 0;
    std::uint64_t depth_attached = 0, depth_off = 0;  // agA41: DSV bound + attached / pipeline without depth state
    std::unordered_map<std::uint64_t, std::uint64_t> notarget_pipes, depthdrop_pipes, mrtdrop_pipes;
};
inline State& state() {
    static State s;
    return s;
}
inline void note_image(std::uintptr_t id, std::uint64_t address, std::uint32_t w, std::uint32_t h, std::uint32_t fmt,
                       std::uint32_t flags) {
    if (!on()) return;
    std::lock_guard l(state().m);
    state().images[id] = {address, w, h, fmt, flags};
    state().by_address[address] = id;
}
// LEONIDA_CE_RTDIAG_PAINT=<T# address>: the replay paints that colour target opaque after each draw into it.
inline std::uint64_t paint_address() {
    static const std::uint64_t a = [] {
        const char* e = std::getenv("LEONIDA_CE_RTDIAG_PAINT");
        return e ? std::strtoull(e, nullptr, 0) : 0ull;
    }();
    return a;
}
inline bool paint(std::uintptr_t id) {
    if (!id || !paint_address()) return false;
    std::lock_guard l(state().m);
    auto it = state().images.find(id);
    static const std::uint64_t hi = [] {  // agSA: PAINT=<lo>-<hi> paints every target in the range
        const char* e = std::getenv("LEONIDA_CE_RTDIAG_PAINT");
        const char* dash = e ? std::strchr(e, 0x2d) : nullptr;
        return dash ? std::strtoull(dash + 1, nullptr, 0) : paint_address();
    }();
    return it != state().images.end() && it->second.address >= paint_address() && it->second.address <= hi;
}
// LEONIDA_CE_TDIAG=<addr> or <lo>-<hi> (agSA): T# address range the tdiag / bind_image diagnostics match.
inline bool tdiag_match(std::uint64_t address) {
    static const std::pair<std::uint64_t, std::uint64_t> r = [] {
        const char* e = std::getenv("LEONIDA_CE_TDIAG");
        if (!e) return std::pair<std::uint64_t, std::uint64_t>{1, 0};
        char* end = nullptr;
        const std::uint64_t lo = std::strtoull(e, &end, 0);
        const std::uint64_t hi = (end && *end == 0x2d) ? std::strtoull(end + 1, nullptr, 0) : lo;
        return std::pair<std::uint64_t, std::uint64_t>{lo, hi};
    }();
    return address && address >= r.first && address <= r.second;
}
// agA34 LEONIDA_CE_DRAW_SRV: remember a texture a named draw reads (probed and dumped at the next RTDIAG dump). True if new.
inline bool want_address(std::uint64_t address, const std::string& tag) {
    std::lock_guard l(state().m);
    if (!state().by_address.count(address)) return false;
    for (const auto& w : state().want)
        if (w.first == address) return false;
    if (state().want.size() >= 32) return false;
    state().want.push_back({address, tag});
    return true;
}
inline std::vector<std::pair<std::uint64_t, std::string>> wanted() {
    std::lock_guard l(state().m);
    return state().want;
}
inline ImageInfo info_for_address(std::uint64_t address) {
    std::lock_guard l(state().m);
    auto it = state().by_address.find(address);
    if (it == state().by_address.end()) return {};
    auto im = state().images.find(it->second);
    return im == state().images.end() ? ImageInfo{} : im->second;
}
inline std::uint64_t address_for_id(std::uintptr_t id) {
    std::lock_guard l(state().m);
    auto it = state().images.find(id);
    return it == state().images.end() ? 0 : it->second.address;
}
inline std::uintptr_t id_for_address(std::uint64_t address) {
    std::lock_guard l(state().m);
    auto it = state().by_address.find(address);
    return it == state().by_address.end() ? 0 : it->second;
}
inline std::vector<std::uintptr_t> dump(std::uintptr_t presented, std::uint64_t present_no) {
    std::vector<std::uintptr_t> top;
    if (!on()) return top;
    auto& s = state();
    std::lock_guard l(s.m);
    auto img = [&](std::uintptr_t id) {
        auto it = s.images.find(id);
        return it == s.images.end() ? ImageInfo{} : it->second;
    };
    const ImageInfo pi = img(presented);
    std::fprintf(stderr,
                 "[rtdiag] present %llu: presented res %llx (tex addr %llx %ux%u fmt %u flags %x) draws %llu "
                 "pending-pso %llu variant-skip %llu no-target %llu lists ok %llu failed %llu\n",
                 (unsigned long long)present_no, (unsigned long long)presented, (unsigned long long)pi.address, pi.w,
                 pi.h, pi.fmt, pi.flags, (unsigned long long)s.draws, (unsigned long long)s.pending,
                 (unsigned long long)s.variant, (unsigned long long)s.no_target, (unsigned long long)s.lists_ok,
                 (unsigned long long)s.lists_failed);
    std::fprintf(stderr, "[rtdiag]   list fails by result:");
    for (int i = 0; i < 16; ++i)
        if (s.fail[i]) std::fprintf(stderr, " %d:%llu", i, (unsigned long long)s.fail[i]);
    std::fprintf(stderr, " | from a draw:");
    for (int i = 0; i < 16; ++i)
        if (s.draw_fail[i]) std::fprintf(stderr, " %d:%llu", i, (unsigned long long)s.draw_fail[i]);
    std::fprintf(stderr, "\n");
    // agA39: dropped bindings and the pipelines of draws without a colour target.
    std::fprintf(stderr, "[rtdiag]   bindings: dsv-bound %llu, depth-attached %llu, depth-off %llu (pipeline without depth state), "
                 "depth-dropped %llu (pipeline has depth state, no attachment), mrt-dropped %llu (more targets bound than the pipeline has)\n",
                 (unsigned long long)s.dsv_bound, (unsigned long long)s.depth_attached, (unsigned long long)s.depth_off,
                 (unsigned long long)s.depth_dropped, (unsigned long long)s.mrt_dropped);
    {
        const std::pair<const char*, std::unordered_map<std::uint64_t, std::uint64_t>*> maps[] = {
            {"no-target", &s.notarget_pipes}, {"depth-dropped", &s.depthdrop_pipes}, {"mrt-dropped", &s.mrtdrop_pipes}};
        for (const auto& [label, m] : maps) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> pv;
            for (const auto& [pipe, n] : *m) pv.push_back({n, pipe});
            std::sort(pv.rbegin(), pv.rend());
            for (std::size_t k = 0; k < pv.size() && k < 8; ++k)
                std::fprintf(stderr, "[rtdiag]   %s pipe x%llu %s\n", label, (unsigned long long)pv[k].first,
                             ::leonida::ce::pipeline_name(pv[k].second).c_str());
            m->clear();
        }
        s.dsv_bound = s.depth_dropped = s.mrt_dropped = s.depth_attached = s.depth_off = 0;
    }
    for (int pass = 0; pass < 2; ++pass) {
        auto& m = pass ? s.depth_draws : s.target_draws;
        std::vector<std::pair<std::uint64_t, std::uintptr_t>> v;
        for (auto& [id, n] : m) v.push_back({n, id});
        std::sort(v.rbegin(), v.rend());
        for (std::size_t i = 0; i < v.size() && i < 24; ++i) {
            const ImageInfo ii = img(v[i].second);
            std::fprintf(stderr, "[rtdiag]   %s %llx: %llu draws (tex addr %llx %ux%u fmt %u flags %x)%s\n",
                         pass ? "depth " : "target", (unsigned long long)v[i].second, (unsigned long long)v[i].first,
                         (unsigned long long)ii.address, ii.w, ii.h, ii.fmt, ii.flags,
                         v[i].second == presented ? " PRESENTED" : "");
            if (!pass) {
                top.push_back(v[i].second);
                // agA32: the pipelines that drew into this target (most draws first).
                if (const auto pt = s.target_pipes.find(v[i].second); pt != s.target_pipes.end()) {
                    std::vector<std::pair<std::uint64_t, std::uint64_t>> pv;
                    for (const auto& [pipe, n] : pt->second) pv.push_back({n, pipe});
                    std::sort(pv.rbegin(), pv.rend());
                    for (std::size_t k = 0; k < pv.size() && k < 6; ++k)
                        std::fprintf(stderr, "[rtdiag]     pipe x%llu %s\n", (unsigned long long)pv[k].first,
                                     ::leonida::ce::pipeline_name(pv[k].second).c_str());
                }
            }
        }
    }
    s.target_draws.clear();
    s.target_pipes.clear();
    s.depth_draws.clear();
    s.draws = s.pending = s.variant = s.no_target = s.lists_ok = s.lists_failed = 0;
    std::fill(std::begin(s.fail), std::end(s.fail), 0);
    std::fill(std::begin(s.draw_fail), std::end(s.draw_fail), 0);
    return top;
}
// how: 0 drawn, 1 pipeline pending, 2 variant pending
template<typename Colors>
inline void count_draw(const Colors& colors, std::uintptr_t depth, int how, std::uint64_t pipe = 0) {
    if (!on()) return;
    auto& s = state();
    std::lock_guard l(s.m);
    if (how == 1) { ++s.pending; return; }
    if (how == 2) { ++s.variant; return; }
    ++s.draws;
    bool any = false;
    for (const auto& a : colors)
        if (a.id) { ++s.target_draws[a.id]; any = true; if (pipe) ++s.target_pipes[a.id][pipe]; }
    if (depth) ++s.depth_draws[depth];
    if (!any) { ++s.no_target; if (pipe) ++s.notarget_pipes[pipe]; }
}
// agA39: what a drawn draw binds that its pipeline cannot use (counted with the draw).
inline void note_binding(std::uint64_t pipe, bool dsv, bool depth_dropped, int extra_rts, bool attached = false, bool off = false) {
    if (!on()) return;
    auto& s = state();
    std::lock_guard l(s.m);
    if (dsv) ++s.dsv_bound;
    if (attached) ++s.depth_attached;
    if (off) ++s.depth_off;
    if (depth_dropped) { ++s.depth_dropped; if (pipe) ++s.depthdrop_pipes[pipe]; }
    if (extra_rts > 0) { ++s.mrt_dropped; if (pipe) ++s.mrtdrop_pipes[pipe]; }
}
inline void count_list(int result, bool draw_fail) {
    if (!on()) return;
    auto& s = state();
    std::lock_guard l(s.m);
    if (!result) { ++s.lists_ok; return; }
    ++s.lists_failed;
    ++s.fail[result & 15];
    if (draw_fail) ++s.draw_fail[result & 15];
}
} // namespace leonida::rtdiag
