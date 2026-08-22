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

#pragma pack(pop)

// Wire price (1/10000 $) → book ticks (1 cent). Mock/real NASDAQ prices for
// stocks ≥ $1 are always whole cents, so this divide is exact.
LOB_FORCE_INLINE int32_t price_to_ticks(uint32_t wire_be) {
    return static_cast<int32_t>(be32(wire_be) / 100);
}

// Handle one message against a book. `p` points at the type byte. Returns
// the message length consumed, 0 if the type is unknown (caller resyncs).
// Free function so both the single-book FeedHandler and the sharded parallel
// engine share one dispatch implementation.
LOB_FORCE_INLINE size_t dispatch(LimitOrderBook& book, const uint8_t* p) {
    switch (static_cast<char>(*p)) {
    case 'A': {
        auto* m = reinterpret_cast<const AddOrder*>(p);
        book.add(be64(m->order_ref),
                 m->side == 'B' ? Side::Bid : Side::Ask,
                 price_to_ticks(m->price), be32(m->shares));
        return sizeof(AddOrder);
    }
    case 'F': {
        auto* m = reinterpret_cast<const AddOrderMPID*>(p);
        book.add(be64(m->base.order_ref),
                 m->base.side == 'B' ? Side::Bid : Side::Ask,
                 price_to_ticks(m->base.price), be32(m->base.shares));
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
                     price_to_ticks(m->price), be32(m->shares));
        return sizeof(OrderReplace);
    }
    default:
        return 0;   // system/admin messages not modeled in this build
    }
}

// --------------------------------------------------------------------------
// FeedHandler - dispatches one instrument's messages into a LimitOrderBook.
// --------------------------------------------------------------------------
class FeedHandler {
public:
    explicit FeedHandler(LimitOrderBook& book) : book_(book) {}

    LOB_FORCE_INLINE size_t on_message(const uint8_t* p) {
        return dispatch(book_, p);
    }

    // Consume a length-prefixed stream (ITCH file format / MoldUDP payload):
    // [u16 BE length][message] repeated. Returns messages processed.
    size_t on_stream(const uint8_t* buf, size_t len) {
        size_t n = 0;
        const uint8_t* p   = buf;
        const uint8_t* end = buf + len;
        while (p + 2 <= end) {
            uint16_t msg_len;
            __builtin_memcpy(&msg_len, p, 2);
            msg_len = be16(msg_len);
            p += 2;
            if (LOB_UNLIKELY(p + msg_len > end)) break;   // truncated tail
            on_message(p);
            p += msg_len;
            ++n;
        }
        return n;
    }

private:
    LimitOrderBook& book_;
};

} // namespace lob::itch
