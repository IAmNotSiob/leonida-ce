#pragma once

// HACK (agA14): LEONIDA_CE_LOOP_CAP. A bounded-iteration watchdog for translated shaders.
//
// re34: ~66 s after Confirm the GPU hung (TDR, no page fault) in fspx_game/cs_f33ac9e9 (a single-workgroup free-block
// coalescer). Its barrier pass loop runs s_ff1(count) times; with a count of 0 s_ff1 is -1, so the loop runs 2^32 - 1
// rounds with an s_barrier in each (and its chain walk does not advance when a block header reads 0). A console GPU
// would hang on the same inputs, so the inputs are wrong here (zero/stale), but whatever the cause, a hang loses the
// device and the title quits. This patch gives every structured loop whose continue block ends in
// OpBranchConditional between its header and its merge block a private counter: once the counter reaches the cap,
// the loop leaves through its merge block. Outer (barrier-round) loops of the translator are workgroup-uniform, so
// the exit is too; an inner (dispatcher) loop of a wave that hits the cap leaves for the barrier round and then runs
// one block per round until the outer cap ends the program.
//
// No Vulkan headers. Returns the number of loops capped (0: the module is unchanged).

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace leonida {

inline int spirv_cap_loops(std::vector<std::uint32_t>& words, std::uint32_t inner_cap, std::uint32_t outer_cap) {
    constexpr std::uint16_t op_type_bool = 20, op_type_int = 21, op_type_pointer = 32, op_constant = 43,
                            op_function = 54, op_variable = 59, op_load = 61, op_store = 62, op_iadd = 128,
                            op_ult = 176, op_logical_or = 166, op_logical_and = 167, op_label = 248,
                            op_branch_conditional = 250, op_loop_merge = 246, op_selection_merge = 247;
    constexpr std::uint32_t class_function = 7;
    if (words.size() < 5 || words[0] != 0x07230203) return 0;

    struct Inst { std::size_t at; std::uint16_t op, count; };
    std::vector<Inst> insts;
    for (std::size_t at = 5; at < words.size();) {
        const std::uint32_t count = words[at] >> 16;
        if (count == 0 || at + count > words.size()) return 0;
        insts.push_back({at, static_cast<std::uint16_t>(words[at] & 0xffff), static_cast<std::uint16_t>(count)});
        at += count;
    }
    std::uint32_t t_uint = 0, t_bool = 0, t_ptr = 0;
    std::size_t first_function = SIZE_MAX, functions = 0;
    std::unordered_map<std::uint32_t, std::size_t> label_index;  // label id -> instruction index
    for (std::size_t i = 0; i < insts.size(); ++i) {
        const std::uint32_t* w = &words[insts[i].at];
        switch (insts[i].op) {
        case op_type_int: if (!t_uint && insts[i].count == 4 && w[2] == 32 && w[3] == 0) t_uint = w[1]; break;
        case op_type_bool: if (!t_bool) t_bool = w[1]; break;
        case op_function: if (first_function == SIZE_MAX) first_function = i; ++functions; break;
        case op_label: label_index[w[1]] = i; break;
        default: break;
        }
    }
    // agA22: modules with helper functions too (cs_d5613de2 has 3 and its loop was never capped): each counter
    // variable goes into the function that holds its loop.
    if (!t_uint || first_function == SIZE_MAX || functions == 0) return 0;
    for (std::size_t i = 0; i < first_function; ++i) {
        const std::uint32_t* w = &words[insts[i].at];
        if (insts[i].op == op_type_pointer && w[2] == class_function && w[3] == t_uint) { t_ptr = w[1]; break; }
    }

    // Loops to cap: header (has OpLoopMerge merge continue), continue block ends in OpBranchConditional c, a, b with
    // {a, b} == {header, merge}.
    struct Loop { std::size_t term; std::uint32_t header, merge; bool true_is_header; std::size_t first_label; };
    // The first OpLabel of the function that holds instruction i.
    auto function_first_label = [&](std::size_t i) -> std::size_t {
        std::size_t f = SIZE_MAX;
        for (std::size_t j = i + 1; j-- > 0;)
            if (insts[j].op == op_function) { f = j; break; }
        if (f == SIZE_MAX) return SIZE_MAX;
        for (std::size_t j = f; j < insts.size(); ++j)
            if (insts[j].op == op_label) return j;
        return SIZE_MAX;
    };
    std::vector<Loop> loops;
    for (std::size_t i = 0; i < insts.size(); ++i) {
        if (insts[i].op != op_loop_merge) continue;
        const std::uint32_t merge = words[insts[i].at + 1], cont = words[insts[i].at + 2];
        std::uint32_t header = 0;
        for (std::size_t j = i; j-- > 0;)
            if (insts[j].op == op_label) { header = words[insts[j].at + 1]; break; }
        const auto c = label_index.find(cont);
        if (!header || c == label_index.end()) continue;
        std::size_t k = c->second + 1;
        while (k < insts.size() && insts[k].op != op_label && insts[k].op != op_branch_conditional &&
               insts[k].op != 249 /*OpBranch*/ && insts[k].op != 253 /*OpReturn*/ && insts[k].op != 251 /*OpSwitch*/)
            ++k;
        if (k >= insts.size() || insts[k].op != op_branch_conditional) continue;
        const std::uint32_t a = words[insts[k].at + 2], b = words[insts[k].at + 3];
        const std::size_t fl = function_first_label(i);
        if (fl == SIZE_MAX) continue;
        if (a == header && b == merge) loops.push_back({k, header, merge, true, fl});
        else if (a == merge && b == header) loops.push_back({k, header, merge, false, fl});
    }
    if (loops.empty()) return 0;

    std::uint32_t bound = words[3];
    auto fresh = [&] { return bound++; };
    std::vector<std::uint32_t> decls;  // before the first OpFunction
    if (!t_bool) { t_bool = fresh(); decls.insert(decls.end(), {(2u << 16) | op_type_bool, t_bool}); }
    if (!t_ptr) { t_ptr = fresh(); decls.insert(decls.end(), {(4u << 16) | op_type_pointer, t_ptr, class_function, t_uint}); }
    const std::uint32_t c_zero = fresh(), c_one = fresh(), c_inner = fresh(), c_outer = fresh();
    decls.insert(decls.end(), {(4u << 16) | op_constant, t_uint, c_zero, 0u});
    decls.insert(decls.end(), {(4u << 16) | op_constant, t_uint, c_one, 1u});
    decls.insert(decls.end(), {(4u << 16) | op_constant, t_uint, c_inner, inner_cap});
    decls.insert(decls.end(), {(4u << 16) | op_constant, t_uint, c_outer, outer_cap});

    // Outer loops: their continue block branches to the merge block on true (the translator's barrier rounds).
    std::unordered_map<std::size_t, std::vector<std::uint32_t>> vars;      // after each function's first OpLabel
    std::unordered_map<std::size_t, std::vector<std::uint32_t>> before;     // instruction index -> words before it
    std::unordered_map<std::size_t, std::uint32_t> new_cond;                // terminator index -> new condition id
    for (const Loop& l : loops) {
        const std::uint32_t var = fresh();
        auto& fv = vars[l.first_label];
        fv.insert(fv.end(), {(5u << 16) | op_variable, t_ptr, var, class_function, c_zero});
        const std::uint32_t old_cond = words[insts[l.term].at + 1];
        const std::uint32_t ld = fresh(), inc = fresh(), lt = fresh(), cond = fresh();
        std::vector<std::uint32_t> code = {
            (4u << 16) | op_load, t_uint, ld, var,
            (5u << 16) | op_iadd, t_uint, inc, ld, c_one,
            (3u << 16) | op_store, var, inc,
            (5u << 16) | op_ult, t_bool, lt, inc, l.true_is_header ? c_inner : c_outer,
        };
        if (l.true_is_header) {
            code.insert(code.end(), {(5u << 16) | op_logical_and, t_bool, cond, old_cond, lt});
        } else {
            const std::uint32_t ge = fresh();
            code.insert(code.end(), {(4u << 16) | 168 /*OpLogicalNot*/, t_bool, ge, lt});
            code.insert(code.end(), {(5u << 16) | op_logical_or, t_bool, cond, old_cond, ge});
        }
        // A single-block loop: the merge instruction must stay right before the terminator.
        std::size_t at = l.term;
        if (insts[at - 1].op == op_loop_merge || insts[at - 1].op == op_selection_merge) --at;
        auto& slot = before[at];
        slot.insert(slot.end(), code.begin(), code.end());
        new_cond[l.term] = cond;
    }

    std::vector<std::uint32_t> out;
    out.reserve(words.size() + decls.size() + loops.size() * 40);
    out.insert(out.end(), words.begin(), words.begin() + 5);
    for (std::size_t i = 0; i < insts.size(); ++i) {
        if (i == first_function) out.insert(out.end(), decls.begin(), decls.end());
        if (auto b = before.find(i); b != before.end()) out.insert(out.end(), b->second.begin(), b->second.end());
        const std::size_t from = out.size();
        out.insert(out.end(), words.begin() + insts[i].at, words.begin() + insts[i].at + insts[i].count);
        if (auto n = new_cond.find(i); n != new_cond.end()) out[from + 1] = n->second;
        if (auto v = vars.find(i); v != vars.end()) out.insert(out.end(), v->second.begin(), v->second.end());
    }
    out[3] = bound;
    words.swap(out);
    return static_cast<int>(loops.size());
}

}  // namespace leonida
