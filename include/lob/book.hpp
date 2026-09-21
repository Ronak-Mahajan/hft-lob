#pragma once
// ---------------------------------------------------------------------------
// lob/book.hpp - Phase 2: the core engine.
//
//   PriceLevel          24-byte aggregate + FIFO queue head/tail (pool indices).
//   BookSide<IsBid, Ov> flat tick-indexed ladder + occupancy bitmap + cached
//                       best, plus (Ov) an exact ordered overflow.
//   BasicOrderBook<Ov>  Add / Cancel / Execute / Delete / Replace, all O(1)*.
//     LimitOrderBook    = BasicOrderBook<false>: ladder only, tick 1.
//     ExactOrderBook    = BasicOrderBook<true>: any grid, plus the overflow.
//
// Why a flat ladder instead of std::map<price, Level>:
// * price -> level is `levels_[price - base_]`: one subtract, one load (plus
//   one multiply-high on an ExactOrderBook whose tick is not 1). A red-black
//   tree is O(log n) with a dependent-load pointer chase per node; each hop is
//   a likely cache miss and the hops cannot overlap in the pipeline. On a
//   5-deep tree that's ~5 serialized misses vs our 1.
// * Adjacent prices are adjacent in memory. The inside of the book, where
//   90% of traffic lands, occupies a handful of cache lines that simply
//   stay resident in L1.
// * (*) The only non-O(1) moment is re-discovering the best price after the
//   inside level empties. The occupancy bitmap makes that a tzcnt/lzcnt over
//   64 prices per instruction, and because activity clusters at the inside,
//   the next occupied tick is almost always in the same 64-bit word: one
//   masked load + one bit-scan in practice.
//
// Prices, grid and overflow
// -------------------------
// A book is unit-agnostic: every price it takes or returns is a `Price` (u32)
// in the caller's units, and base / tick / band are in the same units. The
// synthetic benchmarks use whole cents. The real-day replay uses the ITCH wire
// unit ($0.0001) directly, so no price is ever rounded: a sub-penny price is
// its own level, and the highest price the feed can carry fits.
//
// LimitOrderBook is the ladder alone: prices [base, base + band) at tick 1.
// An add outside that range is dropped and counted (dropped_out_of_band()).
// The synthetic benchmark streams never leave the band.
//
// ExactOrderBook drops nothing. Its ladder covers [base, base + band * tick)
// on a grid of `tick`; a price in that range AND on the grid maps to ladder
// index (price - base) / tick. Every other price, whether below the ladder,
// above it, or between two of its grid points, lives in the side's overflow:
// a std::map from the exact price to a PriceLevel that holds the same
// intrusive FIFO of pool orders as a ladder level. Routing is a pure function
// of the price, so a price is in exactly one of the two structures, and a
// side's best price is the better of the ladder's cached best and the
// overflow's first key. Every operation (add, execute, cancel, delete,
// replace) works on an order wherever it rests, and a replace can move an
// order between the two. The overflow is the slow path (O(log n), one map
// node per new level); the ladder is meant to cover where the trading is.
//
// The two are one template so that they share every line of the ladder, FIFO,
// pool and id-map code. The overflow is a compile-time property rather than a
// runtime flag because a runtime flag measurably changed the code generated
// for the ladder-only benchmark loops; with the template, LimitOrderBook
// compiles to exactly the ladder-only code.
//
// Threading model: single writer, as in any serious feed handler. The message
// stream is inherently sequential (book state N depends on N-1), so the right
// concurrency answer is core-pinned single-threaded + SPSC queues around it,
// not locks inside the book. Hence: zero atomics, zero locks in here.
// ---------------------------------------------------------------------------
#include <functional>
#include <map>
#include <type_traits>
#include <utility>
#include <vector>

#include "common.hpp"
#include "order_pool.hpp"

namespace lob {

// Order::level_idx of an order that rests in the overflow, not on the ladder.
inline constexpr uint32_t kOverflowLevel = NIL;

// --------------------------------------------------------------------------
// PriceLevel - aggregate view (this IS the L2 data) + intrusive FIFO queue.
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
    Price    bid_price = 0;   // valid only if bid_qty > 0
    Price    ask_price = 0;   // valid only if ask_qty > 0
    uint64_t bid_qty   = 0;
    uint64_t ask_qty   = 0;
};

// --------------------------------------------------------------------------
// BookSide - one half of the book. IsBid is a template parameter so the
// "better price" comparison and bitmap scan direction are resolved at
// compile time; the generated code for each side is branch-free on side.
// --------------------------------------------------------------------------
template <bool IsBid, bool WithOverflow>
class BookSide {
    // Overflow levels, best price first (bids descending, asks ascending).
    using Better   = std::conditional_t<IsBid, std::greater<Price>, std::less<Price>>;
    using Overflow = std::map<Price, PriceLevel, Better>;

public:
    BookSide(uint32_t band, OrderPool& pool)
        : levels_(band),
          occupancy_((band + 63) / 64, 0),
          pool_(pool)
    {}

    // Insert order (already filled in) at ladder index li. FIFO: append at tail.
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

    // Insert order (already filled in, level_idx == kOverflowLevel) at its
    // exact price in the overflow. FIFO: append at tail.
    LOB_COLD void insert_overflow(uint32_t oi, Order& o) {
        static_assert(WithOverflow);
        PriceLevel& L = overflow_[o.price];      // value-initialized: count 0
        o.next = NIL;
        if (L.count == 0) {
            o.prev = NIL;
            L.head = L.tail = oi;
        } else {
            o.prev = L.tail;
            pool_[L.tail].next = oi;
            L.tail = oi;
        }
        L.count     += 1;
        L.total_qty += o.qty;
    }

    // Reduce resting qty (partial cancel / partial execution). Caller
    // guarantees d < o.qty for the partial case; d == o.qty falls through
    // to a full unlink.
    LOB_FORCE_INLINE void reduce(Order& o, uint32_t d) {
        if constexpr (WithOverflow) {
            if (LOB_UNLIKELY(o.level_idx == kOverflowLevel)) {
                overflow_level(o.price).total_qty -= d;
                o.qty -= d;
                return;
            }
        }
        levels_[o.level_idx].total_qty -= d;
        o.qty -= d;
    }

    // Remove order entirely: O(1) intrusive unlink.
    LOB_FORCE_INLINE void remove(uint32_t oi, Order& o) {
        if constexpr (WithOverflow) {
            if (LOB_UNLIKELY(o.level_idx == kOverflowLevel)) { remove_overflow(oi, o); return; }
        }
        (void)oi;
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

    // Best overflow level, or nullptr if the overflow is empty (always, for
    // a ladder-only side).
    const std::pair<const Price, PriceLevel>* overflow_best() const {
        if constexpr (WithOverflow) {
            if (!overflow_.empty()) return &*overflow_.begin();
        }
        return nullptr;
    }
    const PriceLevel* find_overflow(Price p) const {
        if constexpr (WithOverflow) {
            auto it = overflow_.find(p);
            if (it != overflow_.end()) return &it->second;
        }
        (void)p;
        return nullptr;
    }
    size_t overflow_levels() const { return WithOverflow ? overflow_.size() : 0; }

    static bool better_price(Price a, Price b) {
        if constexpr (IsBid) return a > b;
        else                 return a < b;
    }

    // Visit every non-empty level, best price first, ladder and overflow
    // merged by price: f(Price, const PriceLevel&). Ladder index li stands
    // for price base + li * tick.
    template <typename F>
    void for_each_level(Price base, Price tick, F& f) const {
        auto it = overflow_.begin();
        const auto end = overflow_.end();
        uint32_t li = best_;
        while (li != NIL || it != end) {
            if (li != NIL) {
                Price p = base + li * tick;
                if (it == end || better_price(p, it->first)) {
                    f(p, levels_[li]);
                    li = next_worse(li);
                    continue;
                }
            }
            f(it->first, it->second);
            ++it;
        }
    }

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

    // Next occupied ladder index strictly worse than li, or NIL.
    uint32_t next_worse(uint32_t li) const {
        if constexpr (IsBid) return li == 0 ? NIL : scan_from(li - 1);
        else return li + 1 >= levels_.size() ? NIL : scan_from(li + 1);
    }

    LOB_COLD PriceLevel& overflow_level(Price p) {
        auto it = overflow_.find(p);
        LOB_ASSERT(it != overflow_.end(), "overflow level missing");
        return it->second;
    }

    LOB_COLD void remove_overflow(uint32_t /*oi*/, Order& o) {
        auto it = overflow_.find(o.price);
        LOB_ASSERT(it != overflow_.end(), "overflow level missing");
        PriceLevel& L = it->second;
        if (o.prev != NIL) pool_[o.prev].next = o.next; else L.head = o.next;
        if (o.next != NIL) pool_[o.next].prev = o.prev; else L.tail = o.prev;
        L.count     -= 1;
        L.total_qty -= o.qty;
        if (L.count == 0) overflow_.erase(it);    // level died
    }

    std::vector<PriceLevel> levels_;
    std::vector<uint64_t>   occupancy_;
    uint32_t                best_ = NIL;
    OrderPool&              pool_;
    Overflow                overflow_;              // stays empty without WithOverflow
};

// --------------------------------------------------------------------------
// BookParams - construction parameters of an ExactOrderBook.
// --------------------------------------------------------------------------
struct BookParams {
    Price    base;              // lowest ladder price
    Price    tick;              // ladder grid step (>= 1), same units as base
    uint32_t band;              // ladder width in ticks (>= 1)
    uint32_t max_live_orders;   // order pool capacity
    unsigned idmap_log2;        // id map slots = 2^idmap_log2 >= 2 * max_live_orders
};

// --------------------------------------------------------------------------
// BasicOrderBook - the venue-facing API. One instrument per instance
// (standard practice: symbols shard across book instances, never threads
// within one book).
// --------------------------------------------------------------------------
template <bool WithOverflow>
class BasicOrderBook {
public:
    // Ladder-only constructor (LimitOrderBook): tick 1 in the caller's units.
    // base_tick: lowest representable price (ticks); band: ladder width.
    // Defaults in the synthetic benchmarks cover $0.01 .. $1310.72 at 1c
    // ticks in 2 x 3 MB of ladder.
    BasicOrderBook(int32_t base_tick, uint32_t band,
                   uint32_t max_live_orders, unsigned idmap_log2)
        : base_(static_cast<Price>(base_tick)),
          band_(band),
          pool_(max_live_orders),
          idmap_(idmap_log2),
          bids_(band, pool_),
          asks_(band, pool_)
    {
        static_assert(!WithOverflow, "an ExactOrderBook is built from BookParams");
        LOB_ASSERT(base_tick >= 0, "base_tick must be >= 0");
        LOB_ASSERT(uint64_t{base_} + band <= (uint64_t{1} << 32),
                   "ladder exceeds the u32 price range");
        check_capacity(max_live_orders, idmap_log2);
    }

    // ExactOrderBook constructor: explicit grid, overflow for the rest.
    explicit BasicOrderBook(const BookParams& p)
        : base_(p.base),
          band_(p.band),
          pool_(p.max_live_orders),
          idmap_(p.idmap_log2),
          bids_(p.band, pool_),
          asks_(p.band, pool_),
          tick_(p.tick),
          tick_recip_(p.tick > 1 ? ~uint64_t{0} / p.tick + 1 : 0)
    {
        static_assert(WithOverflow, "a LimitOrderBook takes (base_tick, band, max_live, idmap_log2)");
        LOB_ASSERT(p.tick >= 1, "tick must be >= 1");
        LOB_ASSERT(p.band >= 1 && p.band < kOverflowLevel, "band out of range");
        // Keeps base + li * tick inside u32 for every ladder index, and makes
        // an offset that wrapped below base land past the ladder's end.
        LOB_ASSERT(uint64_t{p.base} + uint64_t{p.band} * p.tick <= (uint64_t{1} << 32),
                   "ladder exceeds the u32 price range");
        check_capacity(p.max_live_orders, p.idmap_log2);
    }

    // ---- Add ------------------------------------------------------------
    // Off-ladder prices rest in the overflow (ExactOrderBook) or are dropped
    // and counted (LimitOrderBook, see dropped_out_of_band()).
    LOB_FORCE_INLINE void add(uint64_t id, Side side, Price price, uint32_t qty) {
        uint32_t li = ladder_index(price);
        if (LOB_UNLIKELY(li == NIL)) {
            if constexpr (WithOverflow) add_overflow(id, side, price, qty);
            else                        ++dropped_out_of_band_;
            return;
        }
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
        if (LOB_UNLIKELY(oi == NIL)) { ++unknown_id_; return; }
        unlink_and_free(oi);
    }

    // ---- Replace (ITCH 'U'): cancel old, add new - new id, LOSES priority -
    LOB_FORCE_INLINE void replace(uint64_t old_id, uint64_t new_id,
                                  Price price, uint32_t qty) {
        uint32_t oi = idmap_.find(old_id);
        if (LOB_UNLIKELY(oi == NIL)) { ++unknown_id_; return; }
        Side side = pool_[oi].side;               // side survives a replace
        unlink_and_free(oi);
        add(new_id, side, price, qty);
    }

    // ---- Views ------------------------------------------------------------
    BBO bbo() const {
        BBO r;
        best_of(bids_, r.bid_price, r.bid_qty);
        best_of(asks_, r.ask_price, r.ask_qty);
        return r;
    }

    // L2 aggregate at an exact price; {0,0} if no order rests there.
    std::pair<uint64_t, uint32_t> level_at(Side side, Price price) const {
        uint32_t li = ladder_index(price);
        if (li != NIL) {
            const PriceLevel& L = (side == Side::Bid) ? bids_.level(li) : asks_.level(li);
            return {L.total_qty, L.count};
        }
        const PriceLevel* L = (side == Side::Bid) ? bids_.find_overflow(price)
                                                  : asks_.find_overflow(price);
        if (L == nullptr) return {0, 0};
        return {L->total_qty, L->count};
    }

    // Visit one side's non-empty levels, best price first, ladder and
    // overflow merged: f(Price price, const PriceLevel& level). A level's
    // orders are reachable in FIFO order from level.head via order_at() and
    // Order::next, ending at NIL.
    template <typename F>
    void for_each_level(Side side, F&& f) const {
        if (side == Side::Bid) bids_.for_each_level(base_, tick(), f);
        else                   asks_.for_each_level(base_, tick(), f);
    }

    const Order& order_at(uint32_t pool_index) const { return pool_[pool_index]; }

    const Order* find_order(uint64_t id) const {
        uint32_t oi = idmap_.find(id);
        return oi == NIL ? nullptr : &pool_[oi];
    }

    // Ladder index of `price`, or NIL when the price is outside
    // [base, base + band * tick) or between two grid points.
    LOB_FORCE_INLINE uint32_t ladder_index(Price price) const {
        uint32_t li = price - base_;              // wraps past the ladder if price < base
        if constexpr (WithOverflow) {
            if (tick_ != 1) {
                uint32_t off = li;
                li = static_cast<uint32_t>(mulhi64(tick_recip_, off));  // off / tick_, exact
                if (LOB_UNLIKELY(li * tick_ != off)) return NIL;        // between grid points
            }
        }
        return LOB_LIKELY(li < band_) ? li : NIL;
    }

    Price    base() const { return base_; }
    Price    tick() const { if constexpr (WithOverflow) return tick_; else return 1; }
    uint32_t band() const { return band_; }

    // LimitOrderBook: adds (including the add half of a replace) whose price
    // fell outside [base, base + band) and were therefore dropped. Their later
    // E/X/D/U messages are ignored (unknown id). 0 on a stream within the
    // band. An ExactOrderBook has no drop path, and this reads 0.
    uint64_t dropped_out_of_band() const { return dropped_out_of_band_; }

    // ExactOrderBook: adds placed in the overflow (0 on a LimitOrderBook).
    uint64_t overflow_adds() const { return overflow_adds_; }

    // Price levels currently in the overflow, both sides.
    uint64_t overflow_levels() const { return bids_.overflow_levels() + asks_.overflow_levels(); }

    // E/C/X/D/U messages whose order id is not in the book. 0 on a complete
    // stream whose adds were all accepted.
    uint64_t unknown_id() const { return unknown_id_; }

    // Live orders currently resting in the book.
    uint64_t live_orders() const { return idmap_.size(); }

private:
    static void check_capacity(uint32_t max_live_orders, unsigned idmap_log2) {
        // The id map must stay at load <= 0.5 when every pool slot is live:
        // that is what keeps probe sequences short and, together with the
        // full-table guard in OrderIdMap::insert, what makes OrderPool's
        // "exhausted" assert the one that fires first.
        LOB_ASSERT(idmap_log2 >= 1 && idmap_log2 <= 40, "idmap_log2 out of range");
        LOB_ASSERT(uint64_t{max_live_orders} <= (uint64_t{1} << idmap_log2) / 2,
                   "pool capacity must be <= idmap capacity / 2");
    }

    // Shared body of Execute / partial Cancel. The partial-reduce branch is
    // the common case at the inside and touches only the order (already hot
    // from the id lookup) and its level's total_qty.
    LOB_FORCE_INLINE void reduce_or_remove(uint64_t id, uint32_t qty) {
        uint32_t oi = idmap_.find(id);
        if (LOB_UNLIKELY(oi == NIL)) { ++unknown_id_; return; }
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

    LOB_COLD void add_overflow(uint64_t id, Side side, Price price, uint32_t qty) {
        uint32_t oi = pool_.alloc();
        Order& o = pool_[oi];
        o.id = id; o.qty = qty; o.price = price;
        o.level_idx = kOverflowLevel; o.side = side;
        if (side == Side::Bid) bids_.insert_overflow(oi, o);
        else                   asks_.insert_overflow(oi, o);
        idmap_.insert(id, oi);
        ++overflow_adds_;
    }

    template <bool IsBid>
    void best_of(const BookSide<IsBid, WithOverflow>& s, Price& price, uint64_t& qty) const {
        bool have = false;
        uint32_t li = s.best_idx();
        if (li != NIL) {
            price = base_ + li * tick();
            qty   = s.level(li).total_qty;
            have  = true;
        }
        if (const auto* ob = s.overflow_best()) {
            if (!have || BookSide<IsBid, WithOverflow>::better_price(ob->first, price)) {
                price = ob->first;
                qty   = ob->second.total_qty;
            }
        }
    }

    // Hot members first, in the order the ladder-only book has always had
    // them; the ExactOrderBook-only members follow.
    Price                        base_;
    uint32_t                     band_;
    OrderPool                    pool_;
    OrderIdMap                   idmap_;
    BookSide<true, WithOverflow>  bids_;
    BookSide<false, WithOverflow> asks_;
    uint64_t                     dropped_out_of_band_ = 0;
    uint64_t                     unknown_id_          = 0;
    uint64_t                     overflow_adds_       = 0;
    Price                        tick_                = 1;
    uint64_t                     tick_recip_          = 0;   // floor((2^64 - 1) / tick) + 1 when tick > 1
};

using LimitOrderBook = BasicOrderBook<false>;
using ExactOrderBook = BasicOrderBook<true>;

} // namespace lob
