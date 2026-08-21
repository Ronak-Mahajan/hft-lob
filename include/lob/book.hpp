#pragma once
// ---------------------------------------------------------------------------
// lob/book.hpp — Phase 2: the core engine.
//
//   PriceLevel      24-byte aggregate + FIFO queue head/tail (pool indices).
//   BookSide<IsBid> flat tick-indexed ladder + occupancy bitmap + cached best.
//   LimitOrderBook  Add / Cancel / Execute / Delete / Replace, all O(1)*.
//
// Why a flat ladder instead of std::map<price, Level>:
// * price → level is `levels_[price - base_]`: one subtract, one load. A
//   red-black tree is O(log n) with a dependent-load pointer chase per node —
//   each hop is a likely cache miss and the hops cannot overlap in the
//   pipeline. On a 5-deep tree that's ~5 serialized misses vs our 1.
// * Adjacent prices are adjacent in memory. The inside of the book — where
//   90% of traffic lands — occupies a handful of cache lines that simply
//   stay resident in L1.
// * (*) The only non-O(1) moment is re-discovering the best price after the
//   inside level empties. The occupancy bitmap makes that a tzcnt/lzcnt over
//   64 prices per instruction, and because activity clusters at the inside,
//   the next occupied tick is almost always in the same 64-bit word: one
//   masked load + one bit-scan in practice.
//
// Threading model: single writer, as in any serious feed handler. The message
// stream is inherently sequential (book state N depends on N-1), so the right
// concurrency answer is core-pinned single-threaded + SPSC queues around it,
// not locks inside the book. Hence: zero atomics, zero locks in here.
// ---------------------------------------------------------------------------
#include <vector>

#include "common.hpp"
#include "order_pool.hpp"

namespace lob {

// --------------------------------------------------------------------------
// PriceLevel — aggregate view (this IS the L2 data) + intrusive FIFO queue.
// 24 bytes; the fields an L2 consumer reads (total_qty, count) lead the
// struct so a depth snapshot touches the fewest lines.
// --------------------------------------------------------------------------
struct PriceLevel {
    uint64_t total_qty;   // sum of resting shares at this price
    uint32_t count;       // number of resting orders
    uint32_t head;        // pool index of first (oldest) order, NIL if empty
    uint32_t tail;        // pool index of last  (newest) order, NIL if empty
    uint32_t _pad;
};
static_assert(sizeof(PriceLevel) == 24);

struct BBO {
    int32_t  bid_price = 0;   // valid only if bid_qty > 0
    int32_t  ask_price = 0;   // valid only if ask_qty > 0
    uint64_t bid_qty   = 0;
    uint64_t ask_qty   = 0;
};

// --------------------------------------------------------------------------
// BookSide — one half of the ladder. IsBid is a template parameter so the
// "better price" comparison and bitmap scan direction are resolved at
// compile time; the generated code for each side is branch-free on side.
// --------------------------------------------------------------------------
template <bool IsBid>
class BookSide {
public:
    BookSide(uint32_t band, OrderPool& pool)
        : levels_(band),
          occupancy_((band + 63) / 64, 0),
          pool_(pool)
    {}

    // Insert order (already filled in) at band index li. FIFO: append at tail.
    LOB_FORCE_INLINE void insert(uint32_t oi, Order& o, uint32_t li) {
        PriceLevel& L = levels_[li];
        o.next = NIL;
        if (L.count == 0) {                       // level springs into existence
            o.prev  = NIL;
            L.head  = L.tail = oi;
            set_bit(li);
            if (best_ == NIL || better(li, best_)) best_ = li;
        } else {
            o.prev = L.tail;
            pool_[L.tail].next = oi;
            L.tail = oi;
        }
        L.count      += 1;
        L.total_qty  += o.qty;
    }

    // Reduce resting qty (partial cancel / partial execution). Caller
    // guarantees d < o.qty for the partial case; d == o.qty falls through
    // to a full unlink.
    LOB_FORCE_INLINE void reduce(Order& o, uint32_t d) {
        levels_[o.level_idx].total_qty -= d;
        o.qty -= d;
    }

    // Remove order entirely: O(1) intrusive unlink.
    LOB_FORCE_INLINE void remove(uint32_t /*oi*/, Order& o) {
        uint32_t li = o.level_idx;
        PriceLevel& L = levels_[li];
        if (o.prev != NIL) pool_[o.prev].next = o.next; else L.head = o.next;
        if (o.next != NIL) pool_[o.next].prev = o.prev; else L.tail = o.prev;
        L.count     -= 1;
        L.total_qty -= o.qty;
        if (L.count == 0) {                       // level died
            clear_bit(li);
            if (li == best_) best_ = scan_from(li);   // usually same word
        }
    }

    LOB_FORCE_INLINE uint32_t          best_idx()   const { return best_; }
    LOB_FORCE_INLINE const PriceLevel& level(uint32_t li) const { return levels_[li]; }

private:
    LOB_FORCE_INLINE void set_bit(uint32_t li)   { occupancy_[li >> 6] |=  (uint64_t{1} << (li & 63)); }
    LOB_FORCE_INLINE void clear_bit(uint32_t li) { occupancy_[li >> 6] &= ~(uint64_t{1} << (li & 63)); }

    static LOB_FORCE_INLINE bool better(uint32_t a, uint32_t b) {
        if constexpr (IsBid) return a > b;   // bids: higher price wins
        else                 return a < b;   // asks: lower price wins
    }

    // Find the best occupied index at-or-worse-than `from` (whose bit is
    // already cleared). Bids scan downward, asks upward, 64 ticks/step.
    uint32_t scan_from(uint32_t from) const {
        uint32_t w = from >> 6;
        if constexpr (IsBid) {
            uint64_t word = occupancy_[w] & (~uint64_t{0} >> (63 - (from & 63)));
            while (word == 0) {
                if (w == 0) return NIL;
                word = occupancy_[--w];
            }
            return (w << 6) + 63 - static_cast<uint32_t>(std::countl_zero(word));
        } else {
            uint64_t word = occupancy_[w] & (~uint64_t{0} << (from & 63));
            while (word == 0) {
                if (++w == occupancy_.size()) return NIL;
                word = occupancy_[w];
            }
            return (w << 6) + static_cast<uint32_t>(std::countr_zero(word));
        }
    }

    std::vector<PriceLevel> levels_;
    std::vector<uint64_t>   occupancy_;
    uint32_t                best_ = NIL;
    OrderPool&              pool_;
};

// --------------------------------------------------------------------------
// LimitOrderBook — the venue-facing API. One instrument per instance
// (standard practice: symbols shard across book instances, never threads
// within one book).
// --------------------------------------------------------------------------
class LimitOrderBook {
public:
    // base_tick: lowest representable price (ticks); band: ladder width.
    // Defaults cover $0.01 .. $1310.72 at 1¢ ticks in 2×3 MB of ladder.
    LimitOrderBook(int32_t base_tick, uint32_t band,
                   uint32_t max_live_orders, unsigned idmap_log2)
        : base_(base_tick),
          band_(band),
          pool_(max_live_orders),
          idmap_(idmap_log2),
          bids_(band, pool_),
          asks_(band, pool_)
    {}

    // ---- Add ------------------------------------------------------------
    LOB_FORCE_INLINE void add(uint64_t id, Side side, int32_t price, uint32_t qty) {
        uint32_t li = static_cast<uint32_t>(price - base_);
        if (LOB_UNLIKELY(li >= band_)) return;    // outside band: drop (prod: log)
        uint32_t oi = pool_.alloc();
        Order& o = pool_[oi];
        o.id = id; o.qty = qty; o.price = price;
        o.level_idx = li; o.side = side;
        if (side == Side::Bid) bids_.insert(oi, o, li);
        else                   asks_.insert(oi, o, li);
        idmap_.insert(id, oi);
    }

    // ---- Execute (ITCH 'E'/'C'): shares trade against a resting order ----
    LOB_FORCE_INLINE void execute(uint64_t id, uint32_t qty) { reduce_or_remove(id, qty); }

    // ---- Cancel (ITCH 'X'): partial cancel of resting shares -------------
    LOB_FORCE_INLINE void cancel(uint64_t id, uint32_t qty)  { reduce_or_remove(id, qty); }

    // ---- Delete (ITCH 'D'): order gone entirely ---------------------------
    LOB_FORCE_INLINE void remove(uint64_t id) {
        uint32_t oi = idmap_.find(id);
        if (LOB_UNLIKELY(oi == NIL)) return;      // unknown id (outside band)
        unlink_and_free(oi);
    }

    // ---- Replace (ITCH 'U'): cancel old, add new — new id, LOSES priority -
    LOB_FORCE_INLINE void replace(uint64_t old_id, uint64_t new_id,
                                  int32_t price, uint32_t qty) {
        uint32_t oi = idmap_.find(old_id);
        if (LOB_UNLIKELY(oi == NIL)) return;
        Side side = pool_[oi].side;               // side survives a replace
        unlink_and_free(oi);
        add(new_id, side, price, qty);
    }

    // ---- Views ------------------------------------------------------------
    BBO bbo() const {
        BBO r;
        uint32_t b = bids_.best_idx();
        if (b != NIL) {
            const PriceLevel& L = bids_.level(b);
            r.bid_price = base_ + static_cast<int32_t>(b);
            r.bid_qty   = L.total_qty;
        }
        uint32_t a = asks_.best_idx();
        if (a != NIL) {
            const PriceLevel& L = asks_.level(a);
            r.ask_price = base_ + static_cast<int32_t>(a);
            r.ask_qty   = L.total_qty;
        }
        return r;
    }

    // L2 aggregate at an exact price; {0,0} if level empty/out of band.
    std::pair<uint64_t, uint32_t> level_at(Side side, int32_t price) const {
        uint32_t li = static_cast<uint32_t>(price - base_);
        if (li >= band_) return {0, 0};
        const PriceLevel& L = (side == Side::Bid) ? bids_.level(li) : asks_.level(li);
        return {L.total_qty, L.count};
    }

    const Order* find_order(uint64_t id) const {
        uint32_t oi = idmap_.find(id);
        return oi == NIL ? nullptr : &pool_[oi];
    }

private:
    // Shared body of Execute / partial Cancel. The partial-reduce branch is
    // the common case at the inside and touches only the order (already hot
    // from the id lookup) and its level's total_qty.
    LOB_FORCE_INLINE void reduce_or_remove(uint64_t id, uint32_t qty) {
        uint32_t oi = idmap_.find(id);
        if (LOB_UNLIKELY(oi == NIL)) return;
        Order& o = pool_[oi];
        if (LOB_LIKELY(qty < o.qty)) {
            if (o.side == Side::Bid) bids_.reduce(o, qty);
            else                     asks_.reduce(o, qty);
        } else {
            unlink_and_free(oi);                  // fully consumed
        }
    }

    LOB_FORCE_INLINE void unlink_and_free(uint32_t oi) {
        Order& o = pool_[oi];
        if (o.side == Side::Bid) bids_.remove(oi, o);
        else                     asks_.remove(oi, o);
        idmap_.erase(o.id);
        pool_.free(oi);
    }

    int32_t         base_;
    uint32_t        band_;
    OrderPool       pool_;
    OrderIdMap      idmap_;
    BookSide<true>  bids_;
    BookSide<false> asks_;
};

} // namespace lob
