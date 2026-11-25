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
// is checked: when p - heap_base >= limit (unsigned: p below heap_base also trips), the pointer becomes heap_base (the
// arena start) instead, so a garbage V# / CB-loaded V# / raw pointer reads junk and writes the arena start instead of
// faulting or hanging the GPU far away. heap_base = the most common operand of u64 adds feeding OpConvertUToPtr.
// Returns the number of pointers guarded (0: unchanged).
inline int spirv_guard_ptr_all(std::vector<std::uint32_t>& words, std::uint64_t limit) {
    constexpr std::uint16_t op_type_int = 21, op_type_bool = 20, op_constant = 43, op_function = 54, op_iadd = 128,
                            op_isub = 130, op_convert_u_to_ptr = 120, op_ult = 176, op_select = 169;
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
            case 61: case 62: case 81: case 113: case 114: case 117: case 124: case 128: case 130: case 132: case 169:
            case 194: case 195: case 196: case 197: case 198: case 199: case 245:
                return true;
            default:
                return false;
        }
    };
    std::uint32_t t_u64 = 0, t_bool = 0;
    std::size_t first_function = SIZE_MAX;
    std::unordered_map<std::uint32_t, std::size_t> def;  // result id -> instruction index (typed ops in functions)
    for (std::size_t i = 0; i < insts.size(); ++i) {
        const std::uint32_t* w = &words[insts[i].at];
        if (insts[i].op == op_type_int && insts[i].count == 4 && w[2] == 64 && w[3] == 0 && !t_u64) t_u64 = w[1];
        if (insts[i].op == op_type_bool && !t_bool) t_bool = w[1];
        if (insts[i].op == op_function && first_function == SIZE_MAX) first_function = i;
        if (first_function != SIZE_MAX && insts[i].count >= 3 && typed(insts[i].op)) def[w[2]] = i;
    }
    if (!t_u64 || !t_bool || first_function == SIZE_MAX) return 0;
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
    const std::uint32_t c_lim = bound++;
    std::vector<std::uint32_t> decls = {(5u << 16) | op_constant, t_u64, c_lim, std::uint32_t(limit), std::uint32_t(limit >> 32)};
    std::unordered_map<std::size_t, std::vector<std::uint32_t>> before;
    std::unordered_map<std::size_t, std::uint32_t> new_operand;
    for (std::size_t i : sites) {
        const std::uint32_t p = words[insts[i].at + 3];
        if (p == hb) continue;
        const std::uint32_t rel = bound++, ok = bound++, safe = bound++;
        before[i] = {(5u << 16) | op_isub, t_u64, rel, p, hb,
                     (5u << 16) | op_ult, t_bool, ok, rel, c_lim,
                     (6u << 16) | op_select, t_u64, safe, ok, p, hb};
        new_operand[i] = safe;
    }
    if (new_operand.empty()) return 0;
    std::vector<std::uint32_t> out;
    out.reserve(words.size() + decls.size() + new_operand.size() * 16);
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
    return static_cast<int>(new_operand.size());
}

}  // namespace leonida
