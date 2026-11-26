// HACK (agA31, Leonida CE): indexes over the append-only placed-record list (d3d12_xs_device.cpp g_placed, used when
// LEONIDA_CE_PLACED_EVICT is off: records are never changed or removed there). The reverse linear scans they replace
// cost O(records) per call and the list reaches ~120k records in the intro (rt29 HANG_SAMPLE: ~2/3 of the slow-frame
// samples were in xs_find_placed / placed_title_of / placed_buffer_holds).
//
// Every query returns exactly what the scan it replaces returns:
//  - PaintIndex::find(va): the NEWEST record (highest index) with start <= va < start + size. The index keeps disjoint
//    segments, each tagged with the newest record covering it; a new record (always the newest) overwrites its range.
//    A record whose start + size wraps 2^64 never matched the scan (va < wrapped end < start), so it is not painted.
//  - ContainIndex::holds(va, bytes): is there ANY record with start <= va and va - start + bytes <= size. It keeps the
//    records not contained in another one (an antichain: ascending starts have ascending ends), so the closest start
//    <= va has the largest end among all records starting at or before va. (Exact while va - start + bytes does not
//    wrap 2^64, i.e. for any real byte count.)
//  - by-resource: a plain map resource -> newest index (in the .cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>

namespace ce_placed {

class PaintIndex {
public:
    static constexpr std::size_t npos = ~std::size_t(0);

    void add(std::uint64_t start, std::uint64_t size, std::size_t idx) {
        if (!size) return;
        const std::uint64_t a = start, b = start + size;
        if (b < a) return;  // wraps: the scan's va < start + size never held for it
        auto it = segs_.upper_bound(a);
        if (it != segs_.begin()) {
            auto p = std::prev(it);
            if (p->first < a && p->second.end > a) {  // a segment straddles a: keep its head (and its tail past b)
                const Seg old = p->second;
                p->second.end = a;
                if (old.end > b) segs_.emplace(b, old);
            }
        }
        it = segs_.lower_bound(a);
        while (it != segs_.end() && it->first < b) {
            if (it->second.end > b) {  // straddles b: keep its tail
                const Seg old = it->second;
                segs_.erase(it);
                segs_.emplace(b, old);
                break;
            }
            it = segs_.erase(it);
        }
        segs_.emplace(a, Seg{b, idx});
    }

    std::size_t find(std::uint64_t va) const {
        auto it = segs_.upper_bound(va);
        if (it == segs_.begin()) return npos;
        --it;
        return va < it->second.end ? it->second.idx : npos;
    }

    std::size_t segments() const { return segs_.size(); }
    void clear() { segs_.clear(); }

private:
    struct Seg { std::uint64_t end; std::size_t idx; };
    std::map<std::uint64_t, Seg> segs_;  // disjoint [start, end) -> newest record index
};

class ContainIndex {
public:
    void add(std::uint64_t start, std::uint64_t size) {
        const std::uint64_t end = start + size;
        if (end < start) { wide_.emplace(start, size); return; }  // wraps: kept aside, checked with the scan's formula
        auto it = items_.upper_bound(start);
        if (it != items_.begin() && std::prev(it)->second >= end) return;  // inside an existing range
        it = items_.lower_bound(start);
        while (it != items_.end() && it->second <= end) it = items_.erase(it);
        items_[start] = end;
    }

    // The scan: any record with va >= start && va - start + bytes <= size (all uint64 arithmetic).
    bool holds(std::uint64_t va, std::uint64_t bytes) const {
        auto it = items_.upper_bound(va);
        if (it != items_.begin()) {
            --it;
            if (va - it->first + bytes <= it->second - it->first) return true;
        }
        for (const auto& [s, n] : wide_)
            if (va >= s && va - s + bytes <= n) return true;
        return false;
    }

    std::size_t size() const { return items_.size() + wide_.size(); }
    void clear() { items_.clear(); wide_.clear(); }

private:
    std::map<std::uint64_t, std::uint64_t> items_;       // antichain: start -> end
    std::multimap<std::uint64_t, std::uint64_t> wide_;   // start -> size, ranges whose end wraps
};

}  // namespace ce_placed
