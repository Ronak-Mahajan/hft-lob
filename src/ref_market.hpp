#pragma once
// ---------------------------------------------------------------------------
// ref_market.hpp - naive whole-market reference model for ITCH 5.0.
//
// The model the fast books are compared against, both in the real-day replay
// (src/replay_main.cpp) and in lob_bench's ExactOrderBook fuzz. It is
// deliberately simple and shares no code with include/lob:
//   * it decodes every message itself, from the ITCH 5.0 byte offsets and its
//     own length table, instead of through itch.hpp's packed structs;
//   * prices stay exact wire prices ($0.0001, u32) as std::map keys: no
//     ladder, no base, no tick, no rounding;
//   * orders live in one std::unordered_map for the whole market, keyed by
//     order reference number (unique across a day). Each order remembers the
//     stock_locate it was added under, and E/C/X/D/U act on that order's own
//     book whatever locate they carry (a disagreement is counted);
//   * every level keeps its order references in a std::list in arrival order,
//     so a comparison can check queue order, not only the aggregates.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

namespace ref {

inline uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t get32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
}
inline uint64_t get64(const uint8_t* p) { return (uint64_t{get32(p)} << 32) | get32(p + 4); }

struct Level {
    uint64_t shares = 0;            // sum of remaining shares
    uint32_t orders = 0;            // resting orders
    std::list<uint64_t> fifo;       // order references, oldest first
};

struct Book {
    std::map<uint32_t, Level, std::greater<uint32_t>> bids;   // best (highest) first
    std::map<uint32_t, Level, std::less<uint32_t>>    asks;   // best (lowest) first
    uint64_t live = 0;                                        // resting orders
};

struct Order {
    uint16_t locate;
    bool     bid;
    uint32_t price;                 // exact wire price
    uint32_t shares;                // remaining
    std::list<uint64_t>::iterator pos;
};

struct Counters {
    uint64_t seen[256] = {};        // order messages by type byte (A F E C X D U)
    uint64_t bad_length = 0;        // an order message whose length is not the spec's
    uint64_t unknown_ref = 0;       // E/C/X/D/U for an order that is not resting
    uint64_t duplicate_ref = 0;     // A/F/U adding a reference that is already resting
    uint64_t over_execution = 0;    // E/C/X for more shares than remain
    uint64_t locate_mismatch = 0;   // E/C/X/D/U whose locate is not the order's
    uint64_t bad_side = 0;          // side byte other than 'B' / 'S'
};

class Market {
public:
    Market() : books_(65536) { orders_.reserve(1u << 22); }

    // One message: m points at the type byte, len is its length prefix.
    // Types other than A F E C X D U carry no book state and are ignored.
    void on_message(const uint8_t* m, uint16_t len) {
        if (len == 0) return;
        const uint8_t t = m[0];
        uint16_t want = 0;
        switch (t) {
        case 'A': want = 36; break;
        case 'F': want = 40; break;
        case 'E': want = 31; break;
        case 'C': want = 36; break;
        case 'X': want = 23; break;
        case 'D': want = 19; break;
        case 'U': want = 35; break;
        default:  return;
        }
        if (len != want) { ++c_.bad_length; return; }
        ++c_.seen[t];
        const uint16_t locate = get16(m + 1);
        switch (t) {
        case 'A':
        case 'F':                   // ref @11, side @19, shares @20, stock @24, price @32
            add(get64(m + 11), locate, m[19], get32(m + 32), get32(m + 20));
            break;
        case 'E':                   // ref @11, executed shares @19
        case 'C':
        case 'X':                   // ref @11, canceled shares @19
            take(get64(m + 11), locate, get32(m + 19));
            break;
        case 'D':                   // ref @11
            del(get64(m + 11), locate);
            break;
        case 'U':                   // old ref @11, new ref @19, shares @27, price @31
            replace(get64(m + 11), get64(m + 19), locate, get32(m + 31), get32(m + 27));
            break;
        }
    }

    const Book&     book(uint16_t locate) const { return books_[locate]; }
    const Order*    order(uint64_t ref) const {
        auto it = orders_.find(ref);
        return it == orders_.end() ? nullptr : &it->second;
    }
    const Counters& counters() const { return c_; }
    uint64_t        live_orders() const { return orders_.size(); }

private:
    using OrderMap = std::unordered_map<uint64_t, Order>;

    void add(uint64_t ref, uint16_t locate, uint8_t side, uint32_t price, uint32_t shares) {
        if (side != 'B' && side != 'S') ++c_.bad_side;
        if (orders_.count(ref) != 0) { ++c_.duplicate_ref; return; }
        const bool bid = (side == 'B');
        Book& b = books_[locate];
        Level& L = bid ? b.bids[price] : b.asks[price];
        L.fifo.push_back(ref);
        L.shares += shares;
        L.orders += 1;
        b.live   += 1;
        orders_.emplace(ref, Order{locate, bid, price, shares, std::prev(L.fifo.end())});
    }

    void take(uint64_t ref, uint16_t locate, uint32_t shares) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) { ++c_.unknown_ref; return; }
        Order& o = it->second;
        if (o.locate != locate) ++c_.locate_mismatch;
        if (shares > o.shares) ++c_.over_execution;
        if (shares >= o.shares) { erase(it); return; }
        o.shares -= shares;
        Book& b = books_[o.locate];
        if (o.bid) b.bids.find(o.price)->second.shares -= shares;
        else       b.asks.find(o.price)->second.shares -= shares;
    }

    void del(uint64_t ref, uint16_t locate) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) { ++c_.unknown_ref; return; }
        if (it->second.locate != locate) ++c_.locate_mismatch;
        erase(it);
    }

    void replace(uint64_t old_ref, uint64_t new_ref, uint16_t locate,
                 uint32_t price, uint32_t shares) {
        auto it = orders_.find(old_ref);
        if (it == orders_.end()) { ++c_.unknown_ref; return; }
        if (it->second.locate != locate) ++c_.locate_mismatch;
        const uint16_t own_locate = it->second.locate;
        const bool bid = it->second.bid;
        erase(it);
        add(new_ref, own_locate, bid ? 'B' : 'S', price, shares);
    }

    template <typename Levels>
    static void unlink(Levels& levels, const Order& o) {
        auto lv = levels.find(o.price);
        Level& L = lv->second;
        L.fifo.erase(o.pos);
        L.shares -= o.shares;
        if (--L.orders == 0) levels.erase(lv);
    }

    void erase(OrderMap::iterator it) {
        const Order& o = it->second;
        Book& b = books_[o.locate];
        if (o.bid) unlink(b.bids, o);
        else       unlink(b.asks, o);
        b.live -= 1;
        orders_.erase(it);
    }

    std::vector<Book> books_;       // indexed by stock_locate
    OrderMap          orders_;
    Counters          c_;
};

} // namespace ref
