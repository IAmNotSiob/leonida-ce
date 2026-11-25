#pragma once

// HACK (agA14): LEONIDA_CE_VA_GUARD. Translated memory accesses compute heap_base + console VA (V# base) and convert
// that to a PhysicalStorageBuffer pointer. re29-re36: fspx compute shaders read through V#s whose VA is terabytes away
// from the arena (garbage descriptors downstream of unsupported root-argument spills, zero-filled inputs) -> NVIDIA
// page fault -> device lost. This patch rewrites every OpConvertUToPtr whose operand is OpIAdd(heap_base, va): when
// va is not within +-window of the arena (own-memory buffers have VA = device address - arena address, small either
// way), the pointer becomes heap_base itself (the arena's reserved first bytes), so the read returns junk instead of
// faulting. heap_base is taken as the most common first operand of those 64-bit adds.
//
// Returns the number of pointers guarded (0: the module is unchanged).

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace leonida {

inline int spirv_guard_va(std::vector<std::uint32_t>& words, std::uint64_t window) {
    constexpr std::uint16_t op_type_int = 21, op_type_bool = 20, op_constant = 43, op_function = 54, op_iadd = 128,
                            op_convert_u_to_ptr = 120, op_ult = 176, op_select = 169;
    if (words.size() < 5 || words[0] != 0x07230203 || !window) return 0;
    struct Inst { std::size_t at; std::uint16_t op, count; };
    std::vector<Inst> insts;
    for (std::size_t at = 5; at < words.size();) {
        const std::uint32_t count = words[at] >> 16;
        if (count == 0 || at + count > words.size()) return 0;
        insts.push_back({at, static_cast<std::uint16_t>(words[at] & 0xffff), static_cast<std::uint16_t>(count)});
        at += count;
    }
    std::uint32_t t_u64 = 0, t_bool = 0;
    std::size_t first_function = SIZE_MAX;
    std::unordered_map<std::uint32_t, std::size_t> def;  // result id -> instruction index (iadd only)
    for (std::size_t i = 0; i < insts.size(); ++i) {
        const std::uint32_t* w = &words[insts[i].at];
        if (insts[i].op == op_type_int && insts[i].count == 4 && w[2] == 64 && w[3] == 0 && !t_u64) t_u64 = w[1];
        if (insts[i].op == op_type_bool && !t_bool) t_bool = w[1];
        if (insts[i].op == op_function && first_function == SIZE_MAX) first_function = i;
        if (insts[i].op == op_iadd && insts[i].count == 5) def[w[2]] = i;
    }
    if (!t_u64 || !t_bool || first_function == SIZE_MAX) return 0;
    // heap_base = the most common first operand of a u64 add feeding OpConvertUToPtr.
    std::unordered_map<std::uint32_t, int> votes;
    std::vector<std::size_t> sites;
    for (std::size_t i = 0; i < insts.size(); ++i) {
        if (insts[i].op != op_convert_u_to_ptr || insts[i].count != 4) continue;
        const auto d = def.find(words[insts[i].at + 3]);
        if (d == def.end() || words[insts[d->second].at + 1] != t_u64) continue;
        ++votes[words[insts[d->second].at + 3]];
        sites.push_back(i);
    }
    std::uint32_t hb = 0;
    int best = 0;
    for (const auto& [id, n] : votes)
        if (n > best) { best = n; hb = id; }
    if (!hb) return 0;
    std::uint32_t bound = words[3];
    const std::uint32_t c_win = bound++, c_two = bound++;
    std::vector<std::uint32_t> decls = {
        (5u << 16) | op_constant, t_u64, c_win, std::uint32_t(window), std::uint32_t(window >> 32),
        (5u << 16) | op_constant, t_u64, c_two, std::uint32_t(window * 2), std::uint32_t((window * 2) >> 32),
    };
    std::unordered_map<std::size_t, std::vector<std::uint32_t>> before;
    std::unordered_map<std::size_t, std::uint32_t> new_operand;
    int guarded = 0;
    for (std::size_t i : sites) {
        const std::uint32_t sum = words[insts[i].at + 3];
        const std::size_t d = def[sum];
        const std::uint32_t a = words[insts[d].at + 3], b = words[insts[d].at + 4];
        if (a != hb && b != hb) continue;
        const std::uint32_t va = a == hb ? b : a;
        const std::uint32_t shifted = bound++, ok = bound++, safe = bound++;
        before[i] = {(5u << 16) | op_iadd, t_u64, shifted, va, c_win,
                     (5u << 16) | op_ult, t_bool, ok, shifted, c_two,
                     (6u << 16) | op_select, t_u64, safe, ok, sum, hb};
        new_operand[i] = safe;
        ++guarded;
    }
    if (!guarded) return 0;
    std::vector<std::uint32_t> out;
    out.reserve(words.size() + decls.size() + guarded * 16);
    out.insert(out.end(), words.begin(), words.begin() + 5);
    for (std::size_t i = 0; i < insts.size(); ++i) {
        if (i == first_function) out.insert(out.end(), decls.begin(), decls.end());
        if (auto b = before.find(i); b != before.end()) out.insert(out.end(), b->second.begin(), b->second.end());
        const std::size_t from = out.size();
        out.insert(out.end(), words.begin() + insts[i].at, words.begin() + insts[i].at + insts[i].count);
        if (auto n = new_operand.find(i); n != new_operand.end()) out[from + 3] = n->second;
    }
    out[3] = bound;
    words.swap(out);
    return guarded;
}

// HACK (agA25): LEONIDA_CE_VSHARP_GUARD_ALL=1. A blanket version of spirv_guard_va for EVERY module (compute and graphics).
// spirv_guard_va only rewrote ConvertUToPtr(heap_base + va) operands, and only when the add fed the pointer directly:
// per-component accesses are formed as (heap_base + va) + k, so cs_edc5d4c7 had 50 of 108 pointers guarded and the rest
// (stores and loads through the chained add) still reached any address. Here every OpConvertUToPtr operand p of type u64
// is checked: when p >= heap_base + limit (p below heap_base passes, down to 1 TiB below), the pointer becomes heap_base + sink
// (a reserved arena area) instead, so a garbage V# / CB-loaded V# / raw pointer reads junk and writes the sink instead of
// faulting or hanging the GPU far away. heap_base = the most common OpBitwiseOr operand of u64 adds feeding
// OpConvertUToPtr (it is (u64(pc[2]) << 32) | pc[1]); no such value -> the module is left alone.
// hits != 0 (compute only: graphics stages may lack the store feature): on a trip, a small selection stores into the
// 16-byte record at heap_base + hits: atomic count (+0), low (+4) and high (+8) dword of the bad pointer. Sites in a
// loop header block are guarded without recording (splitting the block would move OpLoopMerge out of the header).
// Returns the number of pointers guarded (0: unchanged).
inline int spirv_guard_ptr_all(std::vector<std::uint32_t>& words, std::uint64_t limit, std::uint64_t sink = 0,
                               std::uint64_t hits = 0) {
    constexpr std::uint16_t op_type_int = 21, op_type_bool = 20, op_constant = 43, op_function = 54, op_iadd = 128,
                            op_isub = 130, op_convert_u_to_ptr = 120, op_ult = 176, op_select = 169, op_label = 248,
                            op_loop_merge = 246, op_phi = 245, op_type_pointer = 32, op_lnot = 168, op_sel_merge = 247,
                            op_branch_cond = 250, op_branch = 249, op_store = 62, op_atomic_inc = 232, op_uconvert = 113,
                            op_shr = 194;
    if (words.size() < 5 || words[0] != 0x07230203 || !limit) return 0;
    struct Inst { std::size_t at; std::uint16_t op, count; };
    std::vector<Inst> insts;
    for (std::size_t at = 5; at < words.size();) {
        const std::uint32_t count = words[at] >> 16;
        if (count == 0 || at + count > words.size()) return 0;
        insts.push_back({at, static_cast<std::uint16_t>(words[at] & 0xffff), static_cast<std::uint16_t>(count)});
        at += count;
    }
    // Opcodes laid out [op, result type, result id, ...] whose result may feed a pointer conversion.
    auto typed = [](std::uint16_t op) {
        switch (op) {
            case 61: case 81: case 113: case 114: case 117: case 124: case 128: case 130: case 132: case 169:
            case 194: case 195: case 196: case 197: case 198: case 199: case 245:
                return true;
            default:
                return false;
        }
    };
    std::uint32_t t_u64 = 0, t_u32 = 0, t_bool = 0;
    std::size_t first_function = SIZE_MAX;
    std::unordered_map<std::uint32_t, std::size_t> def;  // result id -> instruction index (typed ops in functions)
    std::vector<std::uint32_t> block_of(insts.size(), 0);  // label of the block holding each instruction
    std::unordered_map<std::uint32_t, bool> loop_header;
    std::uint32_t label = 0;
    for (std::size_t i = 0; i < insts.size(); ++i) {
        const std::uint32_t* w = &words[insts[i].at];
        if (insts[i].op == op_type_int && insts[i].count == 4 && w[2] == 64 && w[3] == 0 && !t_u64) t_u64 = w[1];
        if (insts[i].op == op_type_int && insts[i].count == 4 && w[2] == 32 && w[3] == 0 && !t_u32) t_u32 = w[1];
        if (insts[i].op == op_type_bool && !t_bool) t_bool = w[1];
        if (insts[i].op == op_function && first_function == SIZE_MAX) first_function = i;
        if (first_function != SIZE_MAX && insts[i].count >= 3 && typed(insts[i].op)) def[w[2]] = i;
        if (insts[i].op == op_label) label = w[1];
        if (insts[i].op == op_loop_merge) loop_header[label] = true;
        block_of[i] = label;
    }
    if (!t_u64 || !t_bool || first_function == SIZE_MAX) return 0;
    if (!t_u32) hits = 0;
    std::unordered_map<std::uint32_t, int> votes;
    std::vector<std::size_t> sites;
    for (std::size_t i = 0; i < insts.size(); ++i) {
        if (insts[i].op != op_convert_u_to_ptr || insts[i].count != 4) continue;
        const auto d = def.find(words[insts[i].at + 3]);
        if (d == def.end() || words[insts[d->second].at + 1] != t_u64) continue;  // not a known u64 value
        sites.push_back(i);
        if (insts[d->second].op == op_iadd && insts[d->second].count == 5)
            for (int k = 3; k <= 4; ++k) {  // heap_base = (u64(pc[2]) << 32) | pc[1]: an OpBitwiseOr; never a constant
                const std::uint32_t id = words[insts[d->second].at + k];
                const auto od = def.find(id);
                if (od == def.end()) continue;
                votes[id] += insts[od->second].op == 197 ? 1000 : 1;
            }
    }
    std::uint32_t hb = 0;
    int best = 0;
    for (const auto& [id, n] : votes)
        if (n > best) { best = n; hb = id; }
    if (!hb || sites.empty() || best < 1000) return 0;  // no heap_base found: leave the module alone
    std::uint32_t bound = words[3];
    // Pointers BELOW heap_base (other Vulkan buffers' real device addresses) pass: only [heap_base + limit, 2^64 - 2^40) trips.
    constexpr std::uint64_t low = 1ull << 40;
    const std::uint32_t c_lim = bound++, c_sink = bound++, c_low = bound++;
    std::vector<std::uint32_t> decls = {(5u << 16) | op_constant, t_u64, c_lim, std::uint32_t(limit + low), std::uint32_t((limit + low) >> 32),
                                        (5u << 16) | op_constant, t_u64, c_low, std::uint32_t(low), std::uint32_t(low >> 32),
                                        (5u << 16) | op_constant, t_u64, c_sink, std::uint32_t(sink), std::uint32_t(sink >> 32)};
    std::uint32_t t_ptr = 0, c_hits = 0, c_hits4 = 0, c_hits8 = 0, c_32 = 0, c_scope = 0, c_sem = 0;
    if (hits) {
        t_ptr = bound++; c_hits = bound++; c_hits4 = bound++; c_hits8 = bound++; c_32 = bound++; c_scope = bound++; c_sem = bound++;
        const std::uint32_t more[] = {
            (4u << 16) | op_type_pointer, t_ptr, 5349u /* PhysicalStorageBuffer */, t_u32,
            (5u << 16) | op_constant, t_u64, c_hits, std::uint32_t(hits), std::uint32_t(hits >> 32),
            (5u << 16) | op_constant, t_u64, c_hits4, std::uint32_t(hits + 4), std::uint32_t((hits + 4) >> 32),
            (5u << 16) | op_constant, t_u64, c_hits8, std::uint32_t(hits + 8), std::uint32_t((hits + 8) >> 32),
            (5u << 16) | op_constant, t_u64, c_32, 32u, 0u,
            (4u << 16) | op_constant, t_u32, c_scope, 1u /* Device */,
            (4u << 16) | op_constant, t_u32, c_sem, 0u};
        decls.insert(decls.end(), std::begin(more), std::end(more));
    }
    std::unordered_map<std::size_t, std::vector<std::uint32_t>> before;
    std::unordered_map<std::size_t, std::uint32_t> new_operand;
    std::unordered_map<std::uint32_t, std::uint32_t> last_label;  // original block label -> label of its last split part
    for (std::size_t i : sites) {
        const std::uint32_t p = words[insts[i].at + 3];
        if (p == hb) continue;
        const std::uint32_t rel0 = bound++, rel = bound++, ok = bound++, sk = bound++, safe = bound++;
        std::vector<std::uint32_t> v = {(5u << 16) | op_isub, t_u64, rel0, p, hb,
                                        (5u << 16) | op_iadd, t_u64, rel, rel0, c_low,
                                        (5u << 16) | op_ult, t_bool, ok, rel, c_lim,
                                        (5u << 16) | op_iadd, t_u64, sk, hb, c_sink,
                                        (6u << 16) | op_select, t_u64, safe, ok, p, sk};
        const std::uint32_t blk = block_of[i];
        if (hits && blk && !loop_header.count(blk)) {
            const std::uint32_t bad = bound++, h = bound++, m = bound++, a0 = bound++, p0 = bound++, cnt = bound++,
                                a1 = bound++, p1 = bound++, lo = bound++, a2 = bound++, p2 = bound++, sh = bound++, hi = bound++;
            const std::uint32_t rec[] = {
                (4u << 16) | op_lnot, t_bool, bad, ok,
                (3u << 16) | op_sel_merge, m, 0u,
                (4u << 16) | op_branch_cond, bad, h, m,
                (2u << 16) | op_label, h,
                (5u << 16) | op_iadd, t_u64, a0, hb, c_hits,
                (4u << 16) | op_convert_u_to_ptr, t_ptr, p0, a0,
                (6u << 16) | op_atomic_inc, t_u32, cnt, p0, c_scope, c_sem,
                (5u << 16) | op_iadd, t_u64, a1, hb, c_hits4,
                (4u << 16) | op_convert_u_to_ptr, t_ptr, p1, a1,
                (4u << 16) | op_uconvert, t_u32, lo, p,
                (5u << 16) | op_store, p1, lo, 2u /* Aligned */, 4u,
                (5u << 16) | op_iadd, t_u64, a2, hb, c_hits8,
                (4u << 16) | op_convert_u_to_ptr, t_ptr, p2, a2,
                (5u << 16) | op_shr, t_u64, sh, p, c_32,
                (4u << 16) | op_uconvert, t_u32, hi, sh,
                (5u << 16) | op_store, p2, hi, 2u, 4u,
                (2u << 16) | op_branch, m,
                (2u << 16) | op_label, m};
            v.insert(v.end(), std::begin(rec), std::end(rec));
            last_label[blk] = m;
        }
        before[i] = std::move(v);
        new_operand[i] = safe;
    }
    if (new_operand.empty()) return 0;
    std::vector<std::uint32_t> out;
    out.reserve(words.size() + decls.size() + new_operand.size() * 80);
    out.insert(out.end(), words.begin(), words.begin() + 5);
    for (std::size_t i = 0; i < insts.size(); ++i) {
        if (i == first_function) out.insert(out.end(), decls.begin(), decls.end());
        if (auto b = before.find(i); b != before.end()) out.insert(out.end(), b->second.begin(), b->second.end());
        const std::size_t from = out.size();
        out.insert(out.end(), words.begin() + insts[i].at, words.begin() + insts[i].at + insts[i].count);
        if (auto n = new_operand.find(i); n != new_operand.end()) out[from + 3] = n->second;
        if (insts[i].op == op_phi && !last_label.empty())  // a split block's successors now come from its last part
            for (std::size_t k = from + 4; k < out.size(); k += 2)
                if (auto l = last_label.find(out[k]); l != last_label.end()) out[k] = l->second;
    }
    out[3] = bound;
    words.swap(out);
    return static_cast<int>(new_operand.size());
}

}  // namespace leonida
