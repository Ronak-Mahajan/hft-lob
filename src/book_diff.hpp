#pragma once
// ---------------------------------------------------------------------------
// book_diff.hpp - full comparison of one fast book against the reference.
//
// For one stock_locate, compares an ExactOrderBook (or LimitOrderBook) with
// the ref::Market book of the same locate:
//   * the resting-order count;
//   * both sides, level by level from the best price outwards: price,
//     aggregate shares, order count, and the number of levels;
//   * inside every level, the FIFO queue order by order: reference number,
//     remaining shares, price and side, walking the fast book's intrusive
//     list from head to tail and checking its prev links on the way;
//   * the BBO the fast book reports, against the reference's first levels.
// Returns an empty string when the two are identical, otherwise a one-line
// description of the first difference found.
// ---------------------------------------------------------------------------
#include <cinttypes>
#include <cstdio>
#include <string>
#include <vector>

#include "lob/book.hpp"
#include "ref_market.hpp"

struct DiffTotals {
    uint64_t books  = 0;   // books compared
    uint64_t levels = 0;   // price levels compared (both sides)
    uint64_t orders = 0;   // resting orders compared in queue order
};

namespace diff_detail {

struct FastLevel {
    lob::Price price;
    uint64_t   shares;
    uint32_t   orders;
    uint32_t   head;
    uint32_t   tail;
};

template <typename Book>
std::vector<FastLevel> levels_of(const Book& b, lob::Side side) {
    std::vector<FastLevel> v;
    b.for_each_level(side, [&](lob::Price p, const lob::PriceLevel& L) {
        v.push_back({p, L.total_qty, L.count, L.head, L.tail});
    });
    return v;
}

template <typename Book, typename RefLevels>
std::string diff_side(const Book& fast, const ref::Market& refm, const RefLevels& rl,
                      lob::Side side, DiffTotals& tot) {
    const char* sname = side == lob::Side::Bid ? "bid" : "ask";
    char buf[256];
    std::vector<FastLevel> fl = levels_of(fast, side);
    if (fl.size() != rl.size()) {
        std::snprintf(buf, sizeof buf, "%s levels: fast %zu, reference %zu", sname,
                      fl.size(), rl.size());
        return buf;
    }
    size_t i = 0;
    for (const auto& [price, L] : rl) {
        const FastLevel& f = fl[i++];
        ++tot.levels;
        if (f.price != price || f.shares != L.shares || f.orders != L.orders) {
            std::snprintf(buf, sizeof buf,
                          "%s level %zu: fast %" PRIu32 " x %" PRIu64 " (%" PRIu32 " orders), "
                          "reference %" PRIu32 " x %" PRIu64 " (%" PRIu32 " orders)",
                          sname, i - 1, f.price, f.shares, f.orders, price, L.shares, L.orders);
            return buf;
        }
        // Queue order, order by order.
        uint32_t oi = f.head, prev = lob::NIL;
        uint64_t sum = 0;
        size_t k = 0;
        for (uint64_t ref_id : L.fifo) {
            if (oi == lob::NIL) {
                std::snprintf(buf, sizeof buf, "%s level %" PRIu32 ": fast queue ends at %zu of %" PRIu32,
                              sname, price, k, L.orders);
                return buf;
            }
            const lob::Order& o = fast.order_at(oi);
            const ref::Order* ro = refm.order(ref_id);
            if (o.id != ref_id || ro == nullptr || o.qty != ro->shares || o.price != price ||
                o.side != side || o.prev != prev) {
                std::snprintf(buf, sizeof buf,
                              "%s level %" PRIu32 " queue position %zu: fast ref %" PRIu64 " x %" PRIu32
                              ", reference ref %" PRIu64 " x %" PRIu32,
                              sname, price, k, o.id, o.qty, ref_id, ro ? ro->shares : 0u);
                return buf;
            }
            sum += o.qty;
            prev = oi;
            oi = o.next;
            ++k;
            ++tot.orders;
        }
        if (oi != lob::NIL || prev != f.tail || sum != f.shares) {
            std::snprintf(buf, sizeof buf, "%s level %" PRIu32 ": fast queue longer than %zu, "
                          "or tail / share sum inconsistent", sname, price, k);
            return buf;
        }
    }
    return {};
}

} // namespace diff_detail

template <typename Book>
std::string diff_book(const Book& fast, const ref::Market& refm, uint16_t locate,
                      DiffTotals& tot) {
    const ref::Book& rb = refm.book(locate);
    char buf[256];
    ++tot.books;
    if (fast.live_orders() != rb.live) {
        std::snprintf(buf, sizeof buf, "resting orders: fast %" PRIu64 ", reference %" PRIu64,
                      fast.live_orders(), rb.live);
        return buf;
    }
    std::string d = diff_detail::diff_side(fast, refm, rb.bids, lob::Side::Bid, tot);
    if (!d.empty()) return d;
    d = diff_detail::diff_side(fast, refm, rb.asks, lob::Side::Ask, tot);
    if (!d.empty()) return d;

    lob::BBO q = fast.bbo();
    lob::Price rbp = rb.bids.empty() ? 0 : rb.bids.begin()->first;
    uint64_t   rbq = rb.bids.empty() ? 0 : rb.bids.begin()->second.shares;
    lob::Price rap = rb.asks.empty() ? 0 : rb.asks.begin()->first;
    uint64_t   raq = rb.asks.empty() ? 0 : rb.asks.begin()->second.shares;
    bool bid_ok = rb.bids.empty() ? q.bid_qty == 0 : (q.bid_price == rbp && q.bid_qty == rbq);
    bool ask_ok = rb.asks.empty() ? q.ask_qty == 0 : (q.ask_price == rap && q.ask_qty == raq);
    if (!bid_ok || !ask_ok) {
        std::snprintf(buf, sizeof buf,
                      "BBO: fast %" PRIu32 " x %" PRIu64 " / %" PRIu32 " x %" PRIu64
                      ", reference %" PRIu32 " x %" PRIu64 " / %" PRIu32 " x %" PRIu64,
                      q.bid_price, q.bid_qty, q.ask_price, q.ask_qty, rbp, rbq, rap, raq);
        return buf;
    }
    return {};
}
