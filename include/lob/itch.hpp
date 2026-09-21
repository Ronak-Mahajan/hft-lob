#pragma once
// ---------------------------------------------------------------------------
// lob/itch.hpp - Phase 3: NASDAQ ITCH 5.0 feed handler (zero-copy).
//
// Wire facts:
// * Every ITCH message is fixed-size and starts with a 1-byte type code.
// * All integers are BIG-endian; prices are u32 with 4 implied decimals
//   (so 1 cent == 100 wire units).
// * In the standard file format (and inside a MoldUDP64 packet) each message
//   is prefixed with a u16 big-endian length.
//
// Zero-copy strategy: declare #pragma pack(1) structs that mirror the wire
// layout byte-for-byte (verified by static_assert on sizeof), then
// reinterpret_cast the buffer pointer. Nothing is copied out of the receive
// buffer; the only per-field cost is the unavoidable bswap (one instruction).
//
// Aliasing note: casting char* → struct* is formally UB under strict
// aliasing; in practice every HFT shop does exactly this on x86 where
// unaligned loads are free, and packed structs force byte-granular access.
// The fully-portable alternative (memcpy into a local struct) compiles to
// the *same* instructions on GCC/Clang -O2; use it if you need to satisfy
// UBSan. We keep the cast for clarity of intent.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "book.hpp"
#include "common.hpp"

namespace lob::itch {

#pragma pack(push, 1)

// 'A' - Add Order (no MPID). 36 bytes.
struct AddOrder {
    char     type;             // 'A'
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];     // ns since midnight, 48-bit BE
    uint64_t order_ref;
    char     side;             // 'B' / 'S'
    uint32_t shares;
    char     stock[8];
    uint32_t price;            // 4 implied decimals
};
static_assert(sizeof(AddOrder) == 36);

// 'F' - Add Order with MPID attribution. 40 bytes.
struct AddOrderMPID {
    AddOrder base;
    char     mpid[4];
};
static_assert(sizeof(AddOrderMPID) == 40);

// 'E' - Order Executed. 31 bytes.
struct OrderExecuted {
    char     type;             // 'E'
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    uint64_t order_ref;
    uint32_t executed_shares;
    uint64_t match_number;
};
static_assert(sizeof(OrderExecuted) == 31);

// 'C' - Order Executed With Price. 36 bytes.
struct OrderExecutedPrice {
    OrderExecuted base;
    char     printable;        // 'Y'/'N'
    uint32_t execution_price;
};
static_assert(sizeof(OrderExecutedPrice) == 36);

// 'X' - Order Cancel (partial). 23 bytes.
struct OrderCancel {
    char     type;             // 'X'
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    uint64_t order_ref;
    uint32_t canceled_shares;
};
static_assert(sizeof(OrderCancel) == 23);

// 'D' - Order Delete. 19 bytes.
struct OrderDelete {
    char     type;             // 'D'
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    uint64_t order_ref;
};
static_assert(sizeof(OrderDelete) == 19);

// 'U' - Order Replace. 35 bytes.
struct OrderReplace {
    char     type;             // 'U'
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    uint64_t orig_ref;
    uint64_t new_ref;
    uint32_t shares;
    uint32_t price;
};
static_assert(sizeof(OrderReplace) == 35);

// 'R' - Stock Directory. 39 bytes. Carries the symbol for a stock_locate.
struct StockDirectory {
    char     type;             // 'R'
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    char     stock[8];         // symbol, space padded
    char     market_category;
    char     financial_status;
    uint32_t round_lot_size;
    char     round_lots_only;
    char     issue_classification;
    char     issue_subtype[2];
    char     authenticity;
    char     short_sale_threshold;
    char     ipo_flag;
    char     luld_tier;
    char     etp_flag;
    uint32_t etp_leverage;
    char     inverse;
};
static_assert(sizeof(StockDirectory) == 39);

// 'Q' - Cross Trade. 40 bytes. One per opening / closing / halt cross.
struct CrossTrade {
    char     type;             // 'Q'
    uint16_t stock_locate;
    uint16_t tracking;
    uint8_t  timestamp[6];
    uint64_t shares;
    char     stock[8];
    uint32_t cross_price;      // 4 implied decimals
    uint64_t match_number;
    char     cross_type;       // 'O' opening, 'C' closing, 'H' halt/IPO, 'I' intraday
};
static_assert(sizeof(CrossTrade) == 40);

#pragma pack(pop)

// 48-bit big-endian timestamp (nanoseconds since midnight) at bytes 5..10 of
// every ITCH 5.0 message.
LOB_FORCE_INLINE uint64_t timestamp_ns(const uint8_t* msg) {
    uint64_t t = 0;
    for (int i = 5; i < 11; ++i) t = (t << 8) | msg[i];
    return t;
}

// Wire price (1/10000 $) -> whole-cent ticks, truncating. Exact only for
// whole-cent prices: a sub-penny price (legal below $1.00) is merged into the
// cent below it. The synthetic benchmark streams are whole-cent by
// construction; recorded data goes through WireUnits instead.
LOB_FORCE_INLINE Price price_to_ticks(uint32_t wire_be) {
    return be32(wire_be) / 100;
}

// How dispatch() turns a wire price into a book price. CentTicks feeds the
// benchmarks' cent-denominated books; WireUnits passes the exact wire price
// ($0.0001) to books built in wire units, such as the real-day replay's.
struct CentTicks {
    static LOB_FORCE_INLINE Price convert(uint32_t wire_be) { return price_to_ticks(wire_be); }
};
struct WireUnits {
    static LOB_FORCE_INLINE Price convert(uint32_t wire_be) { return be32(wire_be); }
};

// Length of every ITCH 5.0 message type in the specification, modeled or
// not; 0 for a type byte the specification does not define.
constexpr uint16_t spec_len(uint8_t type) {
    switch (static_cast<char>(type)) {
    case 'S': return 12; case 'R': return 39; case 'H': return 25; case 'Y': return 20;
    case 'L': return 26; case 'V': return 35; case 'W': return 12; case 'K': return 28;
    case 'J': return 35; case 'h': return 21; case 'A': return 36; case 'F': return 40;
    case 'E': return 31; case 'C': return 36; case 'X': return 23; case 'D': return 19;
    case 'U': return 35; case 'P': return 44; case 'Q': return 40; case 'B': return 19;
    case 'I': return 50; case 'N': return 20;
    default:  return 0;
    }
}
static_assert(spec_len('R') == sizeof(StockDirectory) && spec_len('Q') == sizeof(CrossTrade));

// Per-type expected wire length for every message type this build models.
// 0 = not modeled (system/admin messages), which the callers skip by the
// stream's length prefix. dispatch_checked() compares the prefix against
// this table BEFORE any reinterpret_cast, so a truncated or corrupt message
// can never be read past its own bytes.
constexpr uint16_t expected_len(uint8_t type) {
    switch (static_cast<char>(type)) {
    case 'A': return static_cast<uint16_t>(sizeof(AddOrder));
    case 'F': return static_cast<uint16_t>(sizeof(AddOrderMPID));
    case 'E': return static_cast<uint16_t>(sizeof(OrderExecuted));
    case 'C': return static_cast<uint16_t>(sizeof(OrderExecutedPrice));
    case 'X': return static_cast<uint16_t>(sizeof(OrderCancel));
    case 'D': return static_cast<uint16_t>(sizeof(OrderDelete));
    case 'U': return static_cast<uint16_t>(sizeof(OrderReplace));
    default:  return 0;
    }
}
static_assert(expected_len('A') == 36 && expected_len('F') == 40 &&
              expected_len('E') == 31 && expected_len('C') == 36 &&
              expected_len('X') == 23 && expected_len('D') == 19 &&
              expected_len('U') == 35 && expected_len('S') == 0);
static_assert(spec_len('A') == expected_len('A') && spec_len('F') == expected_len('F') &&
              spec_len('E') == expected_len('E') && spec_len('C') == expected_len('C') &&
              spec_len('X') == expected_len('X') && spec_len('D') == expected_len('D') &&
              spec_len('U') == expected_len('U'));

// Handle one message against a book. `p` points at the type byte. Returns
// the message length consumed, 0 if the type is unknown (caller resyncs).
// Free function so the single-book FeedHandler, the sharded parallel engine
// and the real-day replay share one dispatch implementation. `Units` picks
// the price conversion (CentTicks by default, WireUnits for wire-unit books);
// `Book` is a LimitOrderBook or an ExactOrderBook.
//
// UNCHECKED: trusts the type byte and reads sizeof(struct) bytes. Use only
// when the caller has already validated the length (see dispatch_checked).
template <typename Units = CentTicks, typename Book>
LOB_FORCE_INLINE size_t dispatch(Book& book, const uint8_t* p) {
    switch (static_cast<char>(*p)) {
    case 'A': {
        auto* m = reinterpret_cast<const AddOrder*>(p);
        book.add(be64(m->order_ref),
                 m->side == 'B' ? Side::Bid : Side::Ask,
                 Units::convert(m->price), be32(m->shares));
        return sizeof(AddOrder);
    }
    case 'F': {
        auto* m = reinterpret_cast<const AddOrderMPID*>(p);
        book.add(be64(m->base.order_ref),
                 m->base.side == 'B' ? Side::Bid : Side::Ask,
                 Units::convert(m->base.price), be32(m->base.shares));
        return sizeof(AddOrderMPID);
    }
    case 'E': {
        auto* m = reinterpret_cast<const OrderExecuted*>(p);
        book.execute(be64(m->order_ref), be32(m->executed_shares));
        return sizeof(OrderExecuted);
    }
    case 'C': {
        auto* m = reinterpret_cast<const OrderExecutedPrice*>(p);
        book.execute(be64(m->base.order_ref), be32(m->base.executed_shares));
        return sizeof(OrderExecutedPrice);
    }
    case 'X': {
        auto* m = reinterpret_cast<const OrderCancel*>(p);
        book.cancel(be64(m->order_ref), be32(m->canceled_shares));
        return sizeof(OrderCancel);
    }
    case 'D': {
        auto* m = reinterpret_cast<const OrderDelete*>(p);
        book.remove(be64(m->order_ref));
        return sizeof(OrderDelete);
    }
    case 'U': {
        auto* m = reinterpret_cast<const OrderReplace*>(p);
        book.replace(be64(m->orig_ref), be64(m->new_ref),
                     Units::convert(m->price), be32(m->shares));
        return sizeof(OrderReplace);
    }
    default:
        return 0;   // system/admin messages not modeled in this build
    }
}

// Validated dispatch: `msg_len` is the wire length prefix of the message at
// `p`. A modeled type whose prefix disagrees with expected_len() is NOT
// parsed; it is counted in `bad_length` and 0 is returned so the caller
// skips it by prefix. Unknown types return 0 without counting (they are
// legal ITCH, just not modeled). Cost on the hot path: one table lookup and
// one predictable compare.
template <typename Units = CentTicks, typename Book>
LOB_FORCE_INLINE size_t dispatch_checked(Book& book, const uint8_t* p,
                                         uint16_t msg_len, uint64_t& bad_length) {
    if (LOB_UNLIKELY(msg_len == 0)) { ++bad_length; return 0; }
    uint16_t want = expected_len(*p);
    if (want == 0) return 0;                       // not modeled: skip
    if (LOB_UNLIKELY(msg_len != want)) { ++bad_length; return 0; }
    return dispatch<Units>(book, p);
}

// --------------------------------------------------------------------------
// FeedHandler - dispatches one instrument's messages into a LimitOrderBook.
// --------------------------------------------------------------------------
class FeedHandler {
public:
    explicit FeedHandler(LimitOrderBook& book) : book_(book) {}

    // Unchecked single message (caller has validated the length).
    LOB_FORCE_INLINE size_t on_message(const uint8_t* p) {
        return dispatch(book_, p);
    }

    // Length-validated single message; `msg_len` is the stream prefix.
    LOB_FORCE_INLINE size_t on_message(const uint8_t* p, uint16_t msg_len) {
        return dispatch_checked(book_, p, msg_len, bad_length_);
    }

    // Consume a length-prefixed stream (ITCH file format / MoldUDP payload):
    // [u16 BE length][message] repeated. Returns messages processed (every
    // framed message counts, including skipped and bad-length ones).
    size_t on_stream(const uint8_t* buf, size_t len) {
        size_t n = 0;
        const uint8_t* p   = buf;
        const uint8_t* end = buf + len;
        while (p + 2 <= end) {
            uint16_t msg_len;
            std::memcpy(&msg_len, p, 2);
            msg_len = be16(msg_len);
            p += 2;
            if (LOB_UNLIKELY(static_cast<size_t>(end - p) < msg_len)) break;  // truncated tail
            on_message(p, msg_len);
            p += msg_len;
            ++n;
        }
        return n;
    }

    // Messages of a modeled type whose length prefix did not match the
    // per-type table (never parsed, skipped by prefix). 0 on a clean feed.
    uint64_t bad_length() const { return bad_length_; }

private:
    LimitOrderBook& book_;
    uint64_t        bad_length_ = 0;
};

} // namespace lob::itch
