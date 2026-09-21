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
//
// diff_touch() is the per-message check the replay runs between those full
// comparisons: right after the fast book and the reference have both applied
// one order message, it compares what that message touched (see below).
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

// The resting-order count and the BBO of one book against the reference's.
template <typename Book>
std::string diff_count_bbo(const Book& fast, const ref::Book& rb) {
    char buf[256];
    if (fast.live_orders() != rb.live) {
        std::snprintf(buf, sizeof buf, "resting orders: fast %" PRIu64 ", reference %" PRIu64,
                      fast.live_orders(), rb.live);
        return buf;
    }
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

template <typename Book>
std::string diff_book(const Book& fast, const ref::Market& refm, uint16_t locate,
                      DiffTotals& tot) {
    const ref::Book& rb = refm.book(locate);
    ++tot.books;
    if (fast.live_orders() != rb.live) return diff_count_bbo(fast, rb);   // reports the counts
    std::string d = diff_detail::diff_side(fast, refm, rb.bids, lob::Side::Bid, tot);
    if (!d.empty()) return d;
    d = diff_detail::diff_side(fast, refm, rb.asks, lob::Side::Ask, tot);
    if (!d.empty()) return d;
    return diff_count_bbo(fast, rb);
}

// ---------------------------------------------------------------------------
// Per-message check. After the fast book and the reference have both applied
// one order message, diff_touch() compares, in the book of the message's
// stock_locate:
//   * the resting-order count and the BBO;
//   * the order the message names (A/F: the order it adds; E/C/X/D: the order
//     it acts on; U: both the original and the replacement), which must be
//     resting in both or in neither, with the same shares, price and side;
//   * an order just added (A/F, and the replacement of a U) must be the last
//     order in its level's queue in both;
//   * the aggregate shares and order count at every price the message touched
//     (the order's price before the message, and the price it adds at).
// Each of those is a hash or tree lookup, so the check runs after every
// message of a day, between the full comparisons of diff_book().
//
// touch_before() reads what the message will touch, from the message bytes
// and the reference, BEFORE the reference applies it (a delete or a full
// execution removes the order it names).
// ---------------------------------------------------------------------------
struct TouchTotals {
    uint64_t messages = 0;   // order messages checked
    uint64_t orders   = 0;   // order references compared
    uint64_t levels   = 0;   // price levels compared
};

struct Touch {
    bool     check = false;      // an order message of its specified length
    uint8_t  type = 0;
    uint16_t locate = 0;         // the message's stock_locate
    uint64_t ref = 0;            // the order the message names (A/F: the new order)
    uint64_t new_ref = 0;        // U: the replacement
    bool     had = false;        // E/C/X/D/U: the named order was resting before
    bool     had_bid = false;
    uint32_t had_price = 0;
    bool     add_bid = false;    // A/F/U: side and price of the order added
    uint32_t add_price = 0;
};

inline Touch touch_before(const ref::Market& refm, const uint8_t* m, uint16_t len) {
    Touch t;
    if (len == 0) return t;
    uint16_t want = 0;
    switch (m[0]) {                      // ITCH 5.0 lengths, as in ref::Market
    case 'A': want = 36; break;
    case 'F': want = 40; break;
    case 'E': want = 31; break;
    case 'C': want = 36; break;
    case 'X': want = 23; break;
    case 'D': want = 19; break;
    case 'U': want = 35; break;
    default:  return t;
    }
    if (len != want) return t;
    t.check  = true;
    t.type   = m[0];
    t.locate = ref::get16(m + 1);
    t.ref    = ref::get64(m + 11);
    if (t.type == 'A' || t.type == 'F') {
        t.add_bid   = m[19] == 'B';
        t.add_price = ref::get32(m + 32);
        return t;
    }
    if (const ref::Order* o = refm.order(t.ref)) {
        t.had       = true;
        t.had_bid   = o->bid;
        t.had_price = o->price;
    }
    if (t.type == 'U') {
        t.new_ref   = ref::get64(m + 19);
        t.add_bid   = t.had_bid;         // a replacement keeps the side
        t.add_price = ref::get32(m + 31);
    }
    return t;
}

template <typename Book>
std::string diff_touch(const Book& fast, const ref::Market& refm, const Touch& t,
                       TouchTotals& tot) {
    char buf[256];
    ++tot.messages;
    const ref::Book& rb = refm.book(t.locate);
    std::string d = diff_count_bbo(fast, rb);
    if (!d.empty()) return d;

    auto order_eq = [&](uint64_t id, bool last_in_queue) -> std::string {
        ++tot.orders;
        const lob::Order* f = fast.find_order(id);
        const ref::Order* r = refm.order(id);
        if (!f && !r) return {};
        if (!f || !r) {
            std::snprintf(buf, sizeof buf, "order %" PRIu64 ": resting in the fast book %s, in the reference %s",
                          id, f ? "yes" : "no", r ? "yes" : "no");
            return buf;
        }
        if (f->qty != r->shares || f->price != r->price || (f->side == lob::Side::Bid) != r->bid) {
            std::snprintf(buf, sizeof buf,
                          "order %" PRIu64 ": fast %s %" PRIu32 " x %" PRIu32 ", reference %s %" PRIu32 " x %" PRIu32,
                          id, f->side == lob::Side::Bid ? "bid" : "ask", f->price, f->qty,
                          r->bid ? "bid" : "ask", r->price, r->shares);
            return buf;
        }
        if (last_in_queue) {
            bool ref_last = false;
            if (r->bid) { auto it = rb.bids.find(r->price); ref_last = it != rb.bids.end() && it->second.fifo.back() == id; }
            else        { auto it = rb.asks.find(r->price); ref_last = it != rb.asks.end() && it->second.fifo.back() == id; }
            if (f->next != lob::NIL || !ref_last) {
                std::snprintf(buf, sizeof buf, "order %" PRIu64 " just added is not last in its queue (fast %s, reference %s)",
                              id, f->next == lob::NIL ? "last" : "not last", ref_last ? "last" : "not last");
                return buf;
            }
        }
        return {};
    };
    auto level_eq = [&](bool bid, uint32_t price) -> std::string {
        ++tot.levels;
        const auto [shares, count] = fast.level_at(bid ? lob::Side::Bid : lob::Side::Ask, price);
        uint64_t rs = 0;
        uint32_t rc = 0;
        if (bid) { auto it = rb.bids.find(price); if (it != rb.bids.end()) { rs = it->second.shares; rc = it->second.orders; } }
        else     { auto it = rb.asks.find(price); if (it != rb.asks.end()) { rs = it->second.shares; rc = it->second.orders; } }
        if (shares != rs || count != rc) {
            std::snprintf(buf, sizeof buf, "%s level %" PRIu32 ": fast %" PRIu64 " (%" PRIu32 " orders), reference %" PRIu64 " (%" PRIu32 " orders)",
                          bid ? "bid" : "ask", price, shares, count, rs, rc);
            return buf;
        }
        return {};
    };

    switch (t.type) {
    case 'A':
    case 'F':
        if (d = order_eq(t.ref, true); !d.empty()) return d;
        return level_eq(t.add_bid, t.add_price);
    case 'E':
    case 'C':
    case 'X':
    case 'D':
        if (d = order_eq(t.ref, false); !d.empty()) return d;
        return t.had ? level_eq(t.had_bid, t.had_price) : std::string{};
    case 'U':
        if (d = order_eq(t.ref, false); !d.empty()) return d;
        if (d = order_eq(t.new_ref, true); !d.empty()) return d;
        if (!t.had) return {};
        if (d = level_eq(t.had_bid, t.had_price); !d.empty()) return d;
        return level_eq(t.add_bid, t.add_price);
    default:
        return {};
    }
}
