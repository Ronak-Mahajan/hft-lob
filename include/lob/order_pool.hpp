#pragma once
// ---------------------------------------------------------------------------
// lob/order_pool.hpp - Phase 1: the memory architecture.
//
//   Order       32-byte POD, exactly two per 64-byte cache line.
//   OrderPool   slab allocator: one up-front allocation, O(1) alloc/free via
//               an intrusive free list threaded through Order::next.
//   OrderIdMap  flat open-addressing hash map (order id → pool index).
//               Linear probing + Fibonacci hashing + backward-shift erase.
//
// Design notes
// ------------
// * Pool indices (u32), never pointers. Half the width of a pointer, which is
//   what gets Order down to 32 bytes; also relocation- and snapshot-safe.
// * Field order inside Order is deliberate: the cancel path touches
//   {qty, prev, next, level_idx}, all co-resident with id in one line, so an
//   unlink costs a single L1 line, not a scatter of misses.
// * The free list is intrusive: a dead order's own `next` field is the link.
//   Freeing is two stores; the pool carries zero per-slot metadata.
// * LIFO free list on purpose: the most-recently-freed slot is the hottest in
//   cache, and it's the first one handed back out.
// * OrderIdMap: 16-byte slots → 4 per cache line. At load ≤ 0.5 a probe
//   sequence almost never leaves its first line. Erase uses backward-shift
//   (Knuth 6.4R), so there are no tombstones and probe lengths never decay
//   over a trading day: critical for a process that runs 6.5 hours without
//   a rehash.
// ---------------------------------------------------------------------------
#include <cstring>
#include <vector>

#include "common.hpp"

namespace lob {

enum class Side : uint8_t { Bid = 0, Ask = 1 };

// A price in the book's own units (see LimitOrderBook). Unsigned 32-bit
// because that is the ITCH 5.0 price field: in wire units ($0.0001) it spans
// $0 to $429,496.7295, which a signed 32-bit type would cut at $214,748.
using Price = uint32_t;

// --------------------------------------------------------------------------
// Order - 32 bytes, POD.
// --------------------------------------------------------------------------
struct Order {
    uint64_t id;         // exchange order reference number
    uint32_t qty;        // remaining shares
    Price    price;      // exact price, in the book's units
    uint32_t prev;       // pool index of prior order at this level (NIL=head)
    uint32_t next;       // pool index of next order / free-list link
    uint32_t level_idx;  // ladder index of owning PriceLevel, or kOverflowLevel
    Side     side;
    uint8_t  _pad[3];
};
static_assert(sizeof(Order) == 32, "two orders per cache line");
static_assert(alignof(Order) == 8);

// --------------------------------------------------------------------------
// OrderPool - pre-allocated slab, O(1) alloc/free, zero critical-path malloc.
// --------------------------------------------------------------------------
class OrderPool {
public:
    explicit OrderPool(uint32_t capacity)
        : slab_(capacity)      // the ONLY allocation, at startup
    {}

    LOB_FORCE_INLINE uint32_t alloc() {
        if (LOB_LIKELY(free_head_ != NIL)) {        // reuse hottest slot first
            uint32_t idx = free_head_;
            free_head_ = slab_[idx].next;
            return idx;
        }
        LOB_ASSERT(high_water_ < slab_.size(), "OrderPool exhausted");
        return high_water_++;                       // bump-allocate fresh slot
    }

    LOB_FORCE_INLINE void free(uint32_t idx) {
        slab_[idx].next = free_head_;               // intrusive LIFO push
        free_head_ = idx;
    }

    LOB_FORCE_INLINE Order&       operator[](uint32_t idx)       { return slab_[idx]; }
    LOB_FORCE_INLINE const Order& operator[](uint32_t idx) const { return slab_[idx]; }

    uint32_t capacity() const { return static_cast<uint32_t>(slab_.size()); }

private:
    std::vector<Order> slab_;
    uint32_t free_head_ = NIL;
    uint32_t high_water_ = 0;   // slots [0, high_water_) have been handed out
};

// --------------------------------------------------------------------------
// OrderIdMap - order id (u64, nonzero) → pool index (u32).
// Fixed capacity (power of two), no rehash, no allocation after construction.
// --------------------------------------------------------------------------
class OrderIdMap {
public:
    // `capacity_log2` should give load factor ≤ 0.5 at peak live orders.
    explicit OrderIdMap(unsigned capacity_log2)
        : shift_(64 - capacity_log2),
          mask_((uint64_t{1} << capacity_log2) - 1),
          slots_(uint64_t{1} << capacity_log2)   // zero-initialized: key 0 = empty
    {}

    // Full-table guard: with at least one empty slot the probe loop must
    // terminate; without one it would spin forever. The live count is kept
    // exact by insert/erase, so the check is one compare on the hot path.
    // The intended operating point is load <= 0.5 (see LimitOrderBook's
    // constructor check that pool capacity <= idmap capacity / 2).
    LOB_FORCE_INLINE void insert(uint64_t key, uint32_t val) {
        LOB_ASSERT(size_ <= mask_, "OrderIdMap full");
        uint64_t i = ideal(key);
        while (slots_[i].key != kEmpty) i = (i + 1) & mask_;
        slots_[i].key = key;
        slots_[i].val = val;
        ++size_;
    }

    // Returns NIL if absent.
    LOB_FORCE_INLINE uint32_t find(uint64_t key) const {
        uint64_t i = ideal(key);
        while (true) {
            if (slots_[i].key == key)    return slots_[i].val;
            if (slots_[i].key == kEmpty) return NIL;
            i = (i + 1) & mask_;
        }
    }

    // Erase by key. Backward-shift deletion (Knuth 6.4 Algorithm R):
    // walk forward from the hole; any element whose home slot does not lie
    // cyclically in (hole, element] can be pulled back into the hole. No
    // tombstones → probe distances stay tight forever.
    LOB_FORCE_INLINE void erase(uint64_t key) {
        uint64_t i = ideal(key);
        while (slots_[i].key != key) {
            if (slots_[i].key == kEmpty) return;   // absent
            i = (i + 1) & mask_;
        }
        uint64_t hole = i;
        uint64_t j = hole;
        while (true) {
            j = (j + 1) & mask_;
            if (slots_[j].key == kEmpty) break;
            uint64_t home = ideal(slots_[j].key);
            bool reachable = (hole < j) ? (home > hole && home <= j)
                                        : (home > hole || home <= j);
            if (!reachable) {                      // safe to pull back
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].key = kEmpty;
        --size_;
    }

    uint64_t size()     const { return size_; }       // live keys
    uint64_t capacity() const { return mask_ + 1; }   // slots

private:
    struct Slot {
        uint64_t key;
        uint32_t val;
        uint32_t _pad;
    };
    static_assert(sizeof(Slot) == 16, "four slots per cache line");
    static constexpr uint64_t kEmpty = 0;   // ITCH order refs are nonzero

    // Fibonacci hashing: multiply by 2^64/φ, keep the top bits. Cheap (one
    // imul) and mixes sequential exchange order refs - which are nearly
    // consecutive integers - across the whole table.
    LOB_FORCE_INLINE uint64_t ideal(uint64_t key) const {
        return (key * 0x9E3779B97F4A7C15ull) >> shift_;
    }

    unsigned shift_;
    uint64_t mask_;
    std::vector<Slot> slots_;
    uint64_t size_ = 0;
};

} // namespace lob
