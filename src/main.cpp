// ---------------------------------------------------------------------------
// main.cpp - Phase 4: verification & benchmarking.
//
// 1. Deterministic unit checks      - FIFO priority, BBO transitions, replace.
// 2. Differential fuzz             - N million random ITCH messages through
//    BOTH the optimized book and a naive std::map reference book; BBO
//    compared after every message, full depth compared periodically.
//    The reference is slow but obviously correct; divergence = bug.
// 3. Latency microbench            - per-op rdtscp-serialized cycles for
//    add / cancel, timer overhead subtracted, percentiles reported.
//    (Serialization inflates absolute numbers; the batched wall-clock
//    average below is the fair throughput figure. Both are printed.)
// 4. End-to-end throughput         - binary ITCH stream through FeedHandler.
// 5. ExactOrderBook unit checks    - wire-unit grid, overflow below / above /
//    between grid points, moves across the ladder boundary, sub-penny and
//    past-int32 prices, the reciprocal division behind ladder_index.
// 6. ExactOrderBook fuzz           - generated multi-symbol wire-unit stream
//    (src/wire_gen.hpp) through ExactOrderBooks and the ref::Market model
//    (src/ref_market.hpp); full depth and queue order audited periodically.
// 7. Recorded-day replay path       - a crafted 65-message ITCH file through
//    the lob_replay run (src/replay_fixture.hpp): sub-penny prices, prices
//    far outside every ladder, replaces across the ladder boundary, chunks
//    from 1 byte up, the reference compared after every message.
// 8. Moving ladder unit checks      - ExactOrderBooks that place and move their
//    own ladders (lob::Recenter): moves up and down with orders resting on
//    both band edges, execute / cancel / delete / replace of orders that
//    moved, grid changes across $1.00, and what must not move a ladder;
//    every step audited (ExactOrderBook::audit()).
// 9. Moving ladder fuzz            - drifting-price streams through moving
//    ladders in six configurations vs ref::Market after every message.
//
// Usage:  lob_bench [--quick] [--cpu N] [--only LIST]
//   --quick  CI mode: smaller message counts (fuzz 250k, latency 100k,
//            throughput 1M, exact fuzz 200k) so the whole run finishes in
//            seconds. Every correctness check still runs; only the sizes
//            shrink, and the numbers it prints are not the README's
//            measurements.
//   --cpu N  pin the benchmark thread to logical CPU N (Windows builds;
//            default 2).
//   --only LIST  run only the listed sections, comma-separated: 1 to 7,
//            8a to 8d (the four moving-ladder scenarios), 9.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lob/book.hpp"
#include "lob/itch.hpp"

#include "book_diff.hpp"
#include "ref_market.hpp"
#include "replay_fixture.hpp"
#include "wire_gen.hpp"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

using namespace lob;

// --- book configuration -----------------------------------------------------
static constexpr int32_t  kBaseTick   = 1;         // $0.01
static constexpr uint32_t kBand       = 1u << 17;  // ticks: up to $1310.72
static constexpr int32_t  kMid        = 10'000;    // $100.00
static constexpr uint32_t kMaxOrders  = 1u << 21;  // 2M live (64 MB slab)
static constexpr unsigned kIdMapLog2  = 22;        // 4M slots, load ≤ 0.5

// --- utilities ---------------------------------------------------------------
struct SplitMix64 {           // deterministic, fast, good enough for fuzzing
    uint64_t s;
    explicit SplitMix64(uint64_t seed) : s(seed) {}
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
};

// --only: the sections to run ("1".."7", "8a".."8d", "9"); empty runs all.
static std::vector<std::string> g_only;
static bool want(const char* id) {
    if (g_only.empty()) return true;
    for (const std::string& s : g_only) if (s == id) return true;
    return false;
}

static int g_failures = 0;
#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("  FAIL: %s (line %d)\n", what, __LINE__);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static void pin_and_boost(unsigned cpu) {
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    if (cpu < 64) SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu);
#else
    (void)cpu;
#endif
}

// Calibrate TSC frequency against the wall clock (~200 ms busy spin).
static double cycles_per_ns() {
    using clock = std::chrono::steady_clock;
    auto     w0 = clock::now();
    uint64_t c0 = rdtsc_begin();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               clock::now() - w0).count() < 200) { /* spin */ }
    uint64_t c1 = rdtsc_end();
    auto     w1 = clock::now();
    double ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(w1 - w0).count());
    return static_cast<double>(c1 - c0) / ns;
}

// Cost of an empty rdtsc_begin/rdtsc_end pair; subtracted from every sample.
static uint64_t timer_overhead() {
    uint64_t best = ~0ull;
    for (int i = 0; i < 4096; ++i) {
        uint64_t t0 = rdtsc_begin();
        uint64_t t1 = rdtsc_end();
        best = std::min(best, t1 - t0);
    }
    return best;
}

static void print_percentiles(const char* label, std::vector<uint32_t>& s,
                              double cpn) {
    std::sort(s.begin(), s.end());
    auto pct = [&](double p) {
        size_t i = std::min(s.size() - 1,
                            static_cast<size_t>(p / 100.0 * static_cast<double>(s.size())));
        return static_cast<double>(s[i]) / cpn;
    };
    std::printf("  %-18s min %6.0f | p50 %6.0f | p90 %6.0f | p99 %6.0f | "
                "p99.9 %7.0f | max %8.0f  (ns)\n",
                label, static_cast<double>(s.front()) / cpn,
                pct(50), pct(90), pct(99), pct(99.9),
                static_cast<double>(s.back()) / cpn);
}

// ============================================================================
// Reference book: std::map + std::unordered_map. Deliberately naive - its
// only job is to be OBVIOUSLY correct so divergence indicts the fast book.
// ============================================================================
class RefBook {
public:
    void add(uint64_t id, Side side, int32_t price, uint32_t qty) {
        if (static_cast<uint32_t>(price - kBaseTick) >= kBand) return;  // mirror band drop
        orders_[id] = {price, qty, side};
        bump(side, price, static_cast<int64_t>(qty), +1);
    }
    void reduce_or_remove(uint64_t id, uint32_t qty) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return;
        if (qty < it->second.qty) {
            it->second.qty -= qty;
            bump(it->second.side, it->second.price, -static_cast<int64_t>(qty), 0);
        } else {
            erase(it);
        }
    }
    void remove(uint64_t id) {
        auto it = orders_.find(id);
        if (it != orders_.end()) erase(it);
    }
    void replace(uint64_t old_id, uint64_t new_id, int32_t price, uint32_t qty) {
        auto it = orders_.find(old_id);
        if (it == orders_.end()) return;
        Side side = it->second.side;
        erase(it);
        add(new_id, side, price, qty);
    }
    BBO bbo() const {
        BBO r;
        if (!bids_.empty()) { r.bid_price = bids_.begin()->first;
                              r.bid_qty   = bids_.begin()->second.first; }
        if (!asks_.empty()) { r.ask_price = asks_.begin()->first;
                              r.ask_qty   = asks_.begin()->second.first; }
        return r;
    }
    std::pair<uint64_t, uint32_t> level_at(Side side, int32_t price) const {
        if (side == Side::Bid) {
            auto it = bids_.find(price);
            return it == bids_.end() ? std::pair<uint64_t, uint32_t>{0, 0}
                                     : std::pair<uint64_t, uint32_t>{it->second.first, it->second.second};
        }
        auto it = asks_.find(price);
        return it == asks_.end() ? std::pair<uint64_t, uint32_t>{0, 0}
                                 : std::pair<uint64_t, uint32_t>{it->second.first, it->second.second};
    }

private:
    struct O { int32_t price; uint32_t qty; Side side; };
    using Agg = std::pair<uint64_t, uint32_t>;   // total_qty, count

    void bump(Side side, int32_t price, int64_t dqty, int dcount) {
        Agg& a = (side == Side::Bid) ? bids_[price] : asks_[price];
        a.first  = static_cast<uint64_t>(static_cast<int64_t>(a.first) + dqty);
        a.second = static_cast<uint32_t>(static_cast<int32_t>(a.second) + dcount);
        if (a.second == 0) {
            if (side == Side::Bid) bids_.erase(price); else asks_.erase(price);
        }
    }
    void erase(std::unordered_map<uint64_t, O>::iterator it) {
        bump(it->second.side, it->second.price,
             -static_cast<int64_t>(it->second.qty), -1);
        orders_.erase(it);
    }

    std::map<int32_t, Agg, std::greater<int32_t>> bids_;
    std::map<int32_t, Agg>                        asks_;
    std::unordered_map<uint64_t, O>               orders_;
};

// ============================================================================
// Mock ITCH generator: emits a binary length-prefixed ITCH stream plus a
// decoded op list (so the reference book can replay identical semantics).
// ============================================================================
struct Op {
    char     type;      // A F E C X D U
    uint64_t id, id2;
    int32_t  price;
    uint32_t qty;
    char     side;      // 'B'/'S' for adds
};

class MockItch {
public:
    explicit MockItch(uint64_t seed) : rng_(seed) {}

    // Generate n messages; realistic-ish mix, prices clustered at the inside.
    // `variants`: also emit the attributed/priced twins of add and execute
    // ('F' Add Order with MPID, 'C' Order Executed with Price) so every
    // dispatch branch is exercised. Off by default so the benchmark streams
    // stay byte-identical to the ones the recorded measurements used.
    void generate(size_t n, uint32_t max_live, bool variants = false) {
        variants_ = variants;
        bytes.reserve(n * 40);
        ops.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            // slow random walk of the mid
            if ((i & 1023) == 0 && !live_.empty()) {
                mid_ += static_cast<int32_t>(rng_.below(3)) - 1;
            }
            uint32_t roll = rng_.below(100);
            bool force_add    = live_.size() < 1000;
            bool force_delete = live_.size() >= max_live;
            if (force_add || (!force_delete && roll < 40))      emit_add();
            else if (force_delete || roll < 65)                 emit_delete();
            else if (roll < 75)                                 emit_execute();
            else if (roll < 85)                                 emit_cancel();
            else                                                emit_replace();
        }
    }

    std::vector<uint8_t> bytes;
    std::vector<Op>      ops;

private:
    struct Gen { int32_t price; uint32_t qty; char side; uint32_t live_pos; };

    // --- big-endian writers -------------------------------------------------
    void p8(uint8_t v)   { bytes.push_back(v); }
    void p16(uint16_t v) { p8(static_cast<uint8_t>(v >> 8)); p8(static_cast<uint8_t>(v)); }
    void p32(uint32_t v) { p16(static_cast<uint16_t>(v >> 16)); p16(static_cast<uint16_t>(v)); }
    void p48(uint64_t v) { p16(static_cast<uint16_t>(v >> 32)); p32(static_cast<uint32_t>(v)); }
    void p64(uint64_t v) { p32(static_cast<uint32_t>(v >> 32)); p32(static_cast<uint32_t>(v)); }
    void pstr(const char* s, size_t n) { bytes.insert(bytes.end(), s, s + n); }

    void header(char type, uint16_t body_len) {
        p16(body_len);          // stream length prefix
        p8(static_cast<uint8_t>(type));
        p16(1);                 // stock locate
        p16(0);                 // tracking
        p48(++ts_);             // timestamp
    }

    int32_t pick_price(char side) {
        // geometric-ish clustering at the inside of the spread
        uint32_t r = rng_.below(100);
        int32_t depth = (r < 60) ? static_cast<int32_t>(rng_.below(3))
                      : (r < 90) ? static_cast<int32_t>(rng_.below(16))
                                 : static_cast<int32_t>(rng_.below(128));
        return (side == 'B') ? mid_ - 1 - depth : mid_ + 1 + depth;
    }

    uint64_t pick_live() {      // uniform over live orders
        return live_[rng_.below(static_cast<uint32_t>(live_.size()))];
    }

    void drop_live(uint64_t id) {   // O(1) swap-pop
        uint32_t pos = gen_[id].live_pos;
        uint64_t back = live_.back();
        live_[pos] = back;
        gen_[back].live_pos = pos;
        live_.pop_back();
        gen_.erase(id);
    }

    void emit_add() {
        uint64_t id   = ++next_id_;
        char    side  = (rng_.next() & 1) ? 'B' : 'S';
        int32_t price = pick_price(side);
        uint32_t qty  = 1 + rng_.below(500);
        // 'F' is 'A' plus a 4-byte MPID; book semantics are identical.
        bool mpid = variants_ && (rng_.next() & 1);
        header(mpid ? 'F' : 'A', mpid ? 40 : 36);
        p64(id); p8(static_cast<uint8_t>(side)); p32(qty);
        pstr("MOCK    ", 8);
        p32(static_cast<uint32_t>(price) * 100);   // ticks → 1/10000 $
        if (mpid) pstr("MPID", 4);
        gen_[id] = {price, qty, side, static_cast<uint32_t>(live_.size())};
        live_.push_back(id);
        ops.push_back({mpid ? 'F' : 'A', id, 0, price, qty, side});
    }
    void emit_delete() {
        uint64_t id = pick_live();
        header('D', 19); p64(id);
        drop_live(id);
        ops.push_back({'D', id, 0, 0, 0, 0});
    }
    void emit_execute() {
        uint64_t id = pick_live();
        Gen& g = gen_[id];
        uint32_t d = 1 + rng_.below(g.qty);        // may fully consume
        // 'C' is 'E' plus printable flag + execution price; the resting
        // order is reduced by the same shares either way.
        bool priced = variants_ && (rng_.next() & 1);
        header(priced ? 'C' : 'E', priced ? 36 : 31);
        p64(id); p32(d); p64(++match_);
        if (priced) { p8('Y'); p32(static_cast<uint32_t>(g.price) * 100); }
        ops.push_back({priced ? 'C' : 'E', id, 0, 0, d, 0});
        if (d >= g.qty) drop_live(id); else g.qty -= d;
    }
    void emit_cancel() {
        uint64_t id = pick_live();
        Gen& g = gen_[id];
        if (g.qty <= 1) { header('D', 19); p64(id); drop_live(id);
                          ops.push_back({'D', id, 0, 0, 0, 0}); return; }
        uint32_t d = 1 + rng_.below(g.qty - 1);    // strictly partial
        header('X', 23); p64(id); p32(d);
        ops.push_back({'X', id, 0, 0, d, 0});
        g.qty -= d;
    }
    void emit_replace() {
        uint64_t old_id = pick_live();
        uint64_t new_id = ++next_id_;
        char    side  = gen_[old_id].side;
        int32_t price = pick_price(side);
        uint32_t qty  = 1 + rng_.below(500);
        header('U', 35); p64(old_id); p64(new_id); p32(qty);
        p32(static_cast<uint32_t>(price) * 100);
        drop_live(old_id);
        gen_[new_id] = {price, qty, side, static_cast<uint32_t>(live_.size())};
        live_.push_back(new_id);
        ops.push_back({'U', old_id, new_id, price, qty, side});
    }

    SplitMix64 rng_;
    std::unordered_map<uint64_t, Gen> gen_;
    std::vector<uint64_t> live_;
    uint64_t next_id_ = 0, match_ = 0, ts_ = 0;
    int32_t  mid_ = kMid;
    bool     variants_ = false;
};

// The reference treats 'F' exactly as 'A' and 'C' exactly as 'E': the MPID
// and execution price carry no book-state information at L2.
static void apply_to_ref(RefBook& ref, const Op& op) {
    switch (op.type) {
    case 'A':
    case 'F': ref.add(op.id, op.side == 'B' ? Side::Bid : Side::Ask,
                      op.price, op.qty);                          break;
    case 'E':
    case 'C': ref.reduce_or_remove(op.id, op.qty);                break;
    case 'X': ref.reduce_or_remove(op.id, op.qty);                break;
    case 'D': ref.remove(op.id);                                  break;
    case 'U': ref.replace(op.id, op.id2, op.price, op.qty);       break;
    }
}

// ============================================================================
// 1. Deterministic unit checks
// ============================================================================
static void unit_checks() {
    std::printf("[1] Deterministic unit checks\n");
    LimitOrderBook b(kBaseTick, kBand, 1u << 12, 14);

    b.add(1, Side::Bid, 10000, 100);
    b.add(2, Side::Bid, 10001, 50);
    b.add(3, Side::Ask, 10003, 80);
    b.add(4, Side::Bid, 10001, 25);            // joins level 10001 behind id 2
    BBO q = b.bbo();
    CHECK(q.bid_price == 10001 && q.bid_qty == 75, "best bid 10001 x 75");
    CHECK(q.ask_price == 10003 && q.ask_qty == 80, "best ask 10003 x 80");

    auto [lq, lc] = b.level_at(Side::Bid, 10001);
    CHECK(lq == 75 && lc == 2, "L2 aggregate at 10001");

    b.remove(2);                               // FIFO head leaves; id 4 remains
    q = b.bbo();
    CHECK(q.bid_price == 10001 && q.bid_qty == 25, "best bid after head delete");

    b.remove(4);                               // level 10001 dies → bitmap rescan
    q = b.bbo();
    CHECK(q.bid_price == 10000 && q.bid_qty == 100, "best bid falls to 10000");

    b.execute(1, 40);                          // partial execution
    q = b.bbo();
    CHECK(q.bid_qty == 60, "partial execution reduces aggregate");

    b.replace(1, 9, 9995, 60);                 // replace: new id, new price
    q = b.bbo();
    CHECK(q.bid_price == 9995 && q.bid_qty == 60, "replace moved best bid");
    CHECK(b.find_order(1) == nullptr, "old id gone after replace");
    CHECK(b.find_order(9) != nullptr && b.find_order(9)->qty == 60, "new id live");

    b.execute(9, 60);                          // full execution empties side
    q = b.bbo();
    CHECK(q.bid_qty == 0, "bid side empty");
    std::printf("  %s\n\n", g_failures == 0 ? "all passed" : "FAILURES ABOVE");
}

// ============================================================================
// 2. Differential fuzz vs reference book
// ============================================================================
static void differential_fuzz(size_t n_msgs) {
    std::printf("[2] Differential fuzz: %zu random ITCH messages (A/F/E/C/X/D/U) "
                "vs std::map reference\n", n_msgs);
    MockItch gen(0xC0FFEEull);
    gen.generate(n_msgs, 100'000, /*variants=*/true);
    size_t n_by_type[256] = {};
    for (const Op& op : gen.ops) ++n_by_type[static_cast<uint8_t>(op.type)];
    std::printf("  mix: A %zu  F %zu  E %zu  C %zu  X %zu  D %zu  U %zu\n",
                n_by_type['A'], n_by_type['F'], n_by_type['E'], n_by_type['C'],
                n_by_type['X'], n_by_type['D'], n_by_type['U']);
    CHECK(n_by_type['F'] > 0 && n_by_type['C'] > 0, "fuzz stream exercises F and C");

    LimitOrderBook book(kBaseTick, kBand, kMaxOrders, kIdMapLog2);
    itch::FeedHandler fh(book);
    RefBook ref;

    const uint8_t* p = gen.bytes.data();
    size_t mismatches = 0;
    for (size_t i = 0; i < gen.ops.size(); ++i) {
        uint16_t len = static_cast<uint16_t>((p[0] << 8) | p[1]);
        fh.on_message(p + 2, len);                 // length-validated path
        p += 2 + len;
        apply_to_ref(ref, gen.ops[i]);

        BBO a = book.bbo(), b = ref.bbo();
        if (a.bid_price != b.bid_price || a.bid_qty != b.bid_qty ||
            a.ask_price != b.ask_price || a.ask_qty != b.ask_qty) {
            if (++mismatches < 5)
                std::printf("  BBO mismatch at msg %zu: fast %u/%llu %u/%llu  ref %u/%llu %u/%llu\n",
                            i, a.bid_price, (unsigned long long)a.bid_qty,
                            a.ask_price, (unsigned long long)a.ask_qty,
                            b.bid_price, (unsigned long long)b.bid_qty,
                            b.ask_price, (unsigned long long)b.ask_qty);
        }
        if ((i % 50'000) == 49'999) {          // periodic full-band depth audit
            // Every price in the configured band, not a window around the
            // mid: a level that drifted outside a window would otherwise
            // never be compared.
            for (int32_t px = kBaseTick;
                 px < kBaseTick + static_cast<int32_t>(kBand); ++px) {
                if (book.level_at(Side::Bid, px) != ref.level_at(Side::Bid, px) ||
                    book.level_at(Side::Ask, px) != ref.level_at(Side::Ask, px)) {
                    ++mismatches;
                    std::printf("  depth mismatch at msg %zu price %d\n", i, px);
                    break;
                }
            }
        }
    }
    CHECK(mismatches == 0, "differential fuzz");
    CHECK(fh.bad_length() == 0, "no bad-length messages in a well-formed stream");
    std::printf("  %zu messages, %zu mismatches - %s\n", gen.ops.size(),
                mismatches, mismatches == 0 ? "PASS" : "FAIL");
    std::printf("  counters: out-of-band drops %llu | bad-length %llu | live orders %llu\n",
                (unsigned long long)book.dropped_out_of_band(),
                (unsigned long long)fh.bad_length(),
                (unsigned long long)book.live_orders());

    // Corrupt-frame check: a modeled type with the wrong length prefix must
    // be refused and counted, never parsed. Take the first message of the
    // stream (a 36- or 40-byte add) and claim it is one byte shorter.
    {
        LimitOrderBook b2(kBaseTick, kBand, 1u << 12, 14);
        itch::FeedHandler fh2(b2);
        const uint8_t* m = gen.bytes.data() + 2;
        uint16_t len = static_cast<uint16_t>((gen.bytes[0] << 8) | gen.bytes[1]);
        size_t consumed = fh2.on_message(m, static_cast<uint16_t>(len - 1));
        CHECK(consumed == 0 && fh2.bad_length() == 1 && b2.live_orders() == 0,
              "bad-length frame refused and counted");
        consumed = fh2.on_message(m, len);
        CHECK(consumed == len && b2.live_orders() == 1, "correct length parses");
    }

    // Reference-implementation comparison (timing only; the stream is the one
    // just verified). The std::map + std::unordered_map reference replays the
    // decoded op list; the flat book replays the wire bytes and therefore
    // also pays for ITCH parsing. Single run, this machine, this synthetic
    // stream: a baseline for the "rejected alternative" column of the README
    // table, not a headline number.
    {
        using clock = std::chrono::steady_clock;
        const double n = static_cast<double>(gen.ops.size());
        RefBook ref2;
        auto r0 = clock::now();
        for (const Op& op : gen.ops) apply_to_ref(ref2, op);
        auto r1 = clock::now();
        LimitOrderBook book2(kBaseTick, kBand, kMaxOrders, kIdMapLog2);
        itch::FeedHandler fh2(book2);
        auto f0 = clock::now();
        fh2.on_stream(gen.bytes.data(), gen.bytes.size());
        auto f1 = clock::now();
        double ref_ns  = std::chrono::duration<double, std::nano>(r1 - r0).count() / n;
        double fast_ns = std::chrono::duration<double, std::nano>(f1 - f0).count() / n;
        BBO qa = ref2.bbo(), qb = book2.bbo();
        CHECK(qa.bid_price == qb.bid_price && qa.ask_price == qb.ask_price,
              "timed replays end in the same BBO");
        std::printf("  reference std::map book: %.1f ns/msg | flat book (incl. parsing): "
                    "%.1f ns/msg | ratio %.1fx\n"
                    "  (reference-implementation comparison on this stream, "
                    "single run; not a headline number)\n\n",
                    ref_ns, fast_ns, fast_ns > 0 ? ref_ns / fast_ns : 0.0);
    }
}

// ============================================================================
// 3. Latency microbench (per-op, serialized) + batched averages
// ============================================================================
static void latency_bench(double cpn, size_t N) {
    std::printf("[3] Latency microbench (rdtscp-serialized per op, overhead-subtracted, "
                "%zu ops)\n", N);
    uint64_t ovh = timer_overhead();
    std::printf("  timer pair overhead: %.0f ns (subtracted)\n",
                static_cast<double>(ovh) / cpn);

    // Pre-generate all arguments so generation never pollutes the timed region.
    SplitMix64 rng(0xBEEFull);
    std::vector<uint64_t> ids(N);
    std::vector<int32_t>  px(N);
    std::vector<uint32_t> qt(N);
    std::vector<uint8_t>  sd(N);
    for (size_t i = 0; i < N; ++i) {
        ids[i] = i + 1;
        sd[i]  = static_cast<uint8_t>(rng.next() & 1);
        int32_t depth = static_cast<int32_t>(rng.below(64));
        px[i]  = sd[i] ? kMid + 1 + depth : kMid - 1 - depth;
        qt[i]  = 1 + rng.below(500);
    }
    // Random cancellation order - the realistic (cache-hostile) case.
    std::vector<uint64_t> kill = ids;
    for (size_t i = N - 1; i > 0; --i)
        std::swap(kill[i], kill[rng.below(static_cast<uint32_t>(i + 1))]);

    LimitOrderBook book(kBaseTick, kBand, kMaxOrders, kIdMapLog2);
    // Warm-up: touch the paths once so the bench measures steady state,
    // then reset by cancelling.
    for (size_t i = 0; i < 10'000; ++i)
        book.add(ids[i], sd[i] ? Side::Ask : Side::Bid, px[i], qt[i]);
    for (size_t i = 0; i < 10'000; ++i) book.remove(ids[i]);

    std::vector<uint32_t> s_add(N), s_del(N);
    for (size_t i = 0; i < N; ++i) {
        uint64_t t0 = rdtsc_begin();
        book.add(ids[i], sd[i] ? Side::Ask : Side::Bid, px[i], qt[i]);
        uint64_t t1 = rdtsc_end();
        uint64_t d = t1 - t0;
        s_add[i] = static_cast<uint32_t>(d > ovh ? d - ovh : 0);
    }
    for (size_t i = 0; i < N; ++i) {
        uint64_t t0 = rdtsc_begin();
        book.remove(kill[i]);
        uint64_t t1 = rdtsc_end();
        uint64_t d = t1 - t0;
        s_del[i] = static_cast<uint32_t>(d > ovh ? d - ovh : 0);
    }
    print_percentiles(N >= 1'000'000 ? "add   (1M live)" : "add",  s_add, cpn);
    print_percentiles("cancel(random)",  s_del, cpn);

    // Batched (unserialized) wall-clock averages - the honest throughput number.
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    for (size_t i = 0; i < N; ++i)
        book.add(ids[i], sd[i] ? Side::Ask : Side::Bid, px[i], qt[i]);
    auto t1 = clock::now();
    for (size_t i = 0; i < N; ++i) book.remove(kill[i]);
    auto t2 = clock::now();
    double add_ns = std::chrono::duration<double, std::nano>(t1 - t0).count()
                    / static_cast<double>(N);
    double del_ns = std::chrono::duration<double, std::nano>(t2 - t1).count()
                    / static_cast<double>(N);
    std::printf("  batched averages: add %.1f ns/op, cancel %.1f ns/op "
                "(pipelined, no fences)\n\n", add_ns, del_ns);
}

// ============================================================================
// 4. End-to-end feed throughput
// ============================================================================
static void throughput_bench(size_t n_msgs) {
    std::printf("[4] End-to-end ITCH stream throughput (%zu messages)\n", n_msgs);
    MockItch gen(0xDEAD5EEDull);
    gen.generate(n_msgs, 200'000);
    std::printf("  stream size: %.1f MB\n",
                static_cast<double>(gen.bytes.size()) / 1e6);

    LimitOrderBook book(kBaseTick, kBand, kMaxOrders, kIdMapLog2);
    itch::FeedHandler fh(book);

    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    size_t done = fh.on_stream(gen.bytes.data(), gen.bytes.size());
    auto t1 = clock::now();

    double secs = std::chrono::duration<double>(t1 - t0).count();
    std::printf("  %zu messages in %.3f s  ->  %.1f M msgs/s  (%.1f ns/msg)\n",
                done, secs, static_cast<double>(done) / secs / 1e6,
                secs * 1e9 / static_cast<double>(done));

    BBO q = book.bbo();
    std::printf("  final book: bid %u x %llu | ask %u x %llu\n",
                q.bid_price, (unsigned long long)q.bid_qty,
                q.ask_price, (unsigned long long)q.ask_qty);
    CHECK(done == gen.ops.size(), "every framed message consumed");
    CHECK(fh.bad_length() == 0, "no bad-length messages in a well-formed stream");
    std::printf("  counters: out-of-band drops %llu | bad-length %llu | live orders %llu\n\n",
                (unsigned long long)book.dropped_out_of_band(),
                (unsigned long long)fh.bad_length(),
                (unsigned long long)book.live_orders());
}

// ============================================================================
// 5. ExactOrderBook unit checks
// ============================================================================
static void exact_book_checks() {
    std::printf("[5] ExactOrderBook unit checks (wire units: ladder, overflow, "
                "sub-penny and past-int32 prices)\n");
    const int before = g_failures;
    using Lvl = std::pair<uint64_t, uint32_t>;

    // ladder_index divides by the tick with a precomputed reciprocal.
    {
        SplitMix64 rng(0x5EEDull);
        const uint32_t divisors[] = {2u, 3u, 7u, 10u, 100u, 1000u, 10000u, 65536u,
                                     99991u, 2147483659u, 4294967295u};
        bool ok = true;
        for (uint32_t d : divisors) {
            const uint64_t M = ~uint64_t{0} / d + 1;
            auto check = [&](uint32_t n) { if (mulhi64(M, n) != n / d) ok = false; };
            const uint32_t top = (0xFFFFFFFFu / d) * d;
            for (uint32_t n : {0u, 1u, d - 1, d, d + 1, top - 1, top, 0xFFFFFFFFu - 1, 0xFFFFFFFFu})
                check(n);
            for (int i = 0; i < 100'000; ++i) check(static_cast<uint32_t>(rng.next()));
        }
        CHECK(ok, "reciprocal division equals integer division");
    }

    // $100.00 .. $100.63 on a one-cent grid (100 wire units); overflow elsewhere.
    ExactOrderBook b(BookParams{1'000'000, 100, 64, 1u << 10, 12});
    CHECK(b.ladder_index(1'000'000) == 0 && b.ladder_index(1'006'300) == 63,
          "first and last ladder ticks");
    CHECK(b.ladder_index(1'006'400) == NIL && b.ladder_index(999'900) == NIL &&
          b.ladder_index(1'000'050) == NIL && b.ladder_index(5) == NIL,
          "one past the top, below the base, between grid points, far below");
    b.add(1, Side::Bid, 1'000'100, 100);   // $100.01   ladder
    b.add(2, Side::Bid,   999'900,  50);   // $99.99    below the ladder
    b.add(3, Side::Bid, 1'007'000,  10);   // $100.70   above the ladder: best bid
    b.add(4, Side::Bid, 1'000'150,  20);   // $100.015  between two grid points
    b.add(5, Side::Ask, 1'008'000,  30);   // $100.80   above the ladder
    b.add(6, Side::Ask, 1'006'300,  40);   // $100.63   last ladder tick: best ask
    BBO q = b.bbo();
    CHECK(q.bid_price == 1'007'000 && q.bid_qty == 10, "best bid comes from the overflow");
    CHECK(q.ask_price == 1'006'300 && q.ask_qty == 40, "best ask comes from the ladder");
    CHECK(b.overflow_adds() == 4 && b.live_orders() == 6, "four overflow adds, six live orders");
    CHECK(b.level_at(Side::Bid, 1'000'150) == Lvl(20, 1), "a sub-penny price is its own level");
    std::vector<Price> seen;
    b.for_each_level(Side::Bid, [&](Price p, const PriceLevel&) { seen.push_back(p); });
    CHECK((seen == std::vector<Price>{1'007'000, 1'000'150, 1'000'100, 999'900}),
          "bid walk merges ladder and overflow by price");
    seen.clear();
    b.for_each_level(Side::Ask, [&](Price p, const PriceLevel&) { seen.push_back(p); });
    CHECK((seen == std::vector<Price>{1'006'300, 1'008'000}),
          "ask walk merges ladder and overflow by price");

    b.execute(3, 4);                          // partial execution in the overflow
    CHECK(b.bbo().bid_qty == 6, "partial execution in the overflow");
    b.cancel(4, 5);                           // partial cancel between grid points
    CHECK(b.level_at(Side::Bid, 1'000'150) == Lvl(15, 1), "partial cancel between grid points");
    b.execute(3, 6);                          // full execution empties the best level
    q = b.bbo();
    CHECK(q.bid_price == 1'000'150 && q.bid_qty == 15, "best bid falls to the sub-penny level");
    b.remove(4);                              // delete in the overflow
    q = b.bbo();
    CHECK(q.bid_price == 1'000'100 && q.bid_qty == 100, "best bid falls back to the ladder");
    b.replace(1, 7, 1'009'900, 25);           // ladder -> overflow
    q = b.bbo();
    CHECK(q.bid_price == 1'009'900 && q.bid_qty == 25 && b.find_order(1) == nullptr &&
          b.level_at(Side::Bid, 1'000'100) == Lvl(0, 0),
          "replace moves an order from the ladder to the overflow");
    b.replace(7, 8, 1'000'500, 25);           // overflow -> ladder
    q = b.bbo();
    CHECK(q.bid_price == 1'000'500 && b.find_order(8) != nullptr &&
          b.find_order(8)->level_idx == 5 && b.level_at(Side::Bid, 1'009'900) == Lvl(0, 0),
          "replace moves an order from the overflow to the ladder");
    b.replace(5, 9, 1'000'700, 30);           // ask: overflow -> ladder, new best ask
    q = b.bbo();
    CHECK(q.ask_price == 1'000'700 && q.ask_qty == 30, "ask replace from the overflow to the ladder");
    b.add(10, Side::Ask, 1'008'000, 5);       // three orders at one overflow price
    b.add(11, Side::Ask, 1'008'000, 6);
    b.add(12, Side::Ask, 1'008'000, 7);
    CHECK(b.level_at(Side::Ask, 1'008'000) == Lvl(18, 3), "three orders at one overflow price");
    b.remove(11);                             // middle of the queue
    std::vector<uint64_t> ids;
    b.for_each_level(Side::Ask, [&](Price p, const PriceLevel& L) {
        if (p != 1'008'000) return;
        for (uint32_t i = L.head; i != NIL; i = b.order_at(i).next) ids.push_back(b.order_at(i).id);
    });
    CHECK((ids == std::vector<uint64_t>{10, 12}), "overflow FIFO after a delete mid-queue");
    b.execute(10, 5);
    b.execute(12, 7);
    CHECK(b.level_at(Side::Ask, 1'008'000) == Lvl(0, 0) && b.overflow_levels() == 1,
          "an emptied overflow level is erased");
    b.remove(424242);
    CHECK(b.unknown_id() == 1 && b.dropped_out_of_band() == 0, "unknown id counted, nothing dropped");

    // Tick of one wire unit: $0.5000 .. $0.5511, every sub-penny price a level.
    ExactOrderBook s(BookParams{5'000, 1, 512, 1u << 8, 10});
    s.add(1, Side::Bid, 5'001, 100);          // $0.5001
    s.add(2, Side::Bid, 5'002, 100);          // $0.5002
    s.add(3, Side::Ask, 5'009, 100);          // $0.5009
    size_t n_bid_levels = 0;
    s.for_each_level(Side::Bid, [&](Price, const PriceLevel&) { ++n_bid_levels; });
    q = s.bbo();
    CHECK(n_bid_levels == 2 && q.bid_price == 5'002 && q.ask_price == 5'009 &&
          s.overflow_adds() == 0, "sub-penny prices are distinct ladder levels");
    CHECK(itch::price_to_ticks(be32(5'001)) == itch::price_to_ticks(be32(5'002)),
          "whole-cent ticks would merge the same two prices");

    // Prices past the int32 range: $300,000.00 on the ladder, u32 max above it.
    ExactOrderBook h(BookParams{2'999'990'000u, 100, 256, 1u << 6, 8});
    h.add(1, Side::Ask, 3'000'000'000u, 1);   // ladder index 100
    h.add(2, Side::Ask, 4'294'967'295u, 1);   // u32 maximum: overflow above
    h.add(3, Side::Bid, 2'147'483'648u, 1);   // one past int32: overflow below
    q = h.bbo();
    CHECK(q.ask_price == 3'000'000'000u && q.bid_price == 2'147'483'648u &&
          h.overflow_adds() == 2 && h.find_order(1)->level_idx == 100,
          "prices past the int32 range stay exact");

    // The ladder-only LimitOrderBook still drops out-of-band adds, and counts them.
    LimitOrderBook lb(1, 100, 1u << 6, 8);
    lb.add(1, Side::Bid, 500, 10);            // outside [1, 101)
    lb.remove(1);                             // its later delete is an unknown id
    CHECK(lb.dropped_out_of_band() == 1 && lb.unknown_id() == 1 && lb.live_orders() == 0,
          "ladder-only book drops out-of-band adds and counts them");

    std::printf("  %s\n\n", g_failures == before ? "all passed" : "FAILURES ABOVE");
}

// ============================================================================
// 6. ExactOrderBook differential fuzz vs ref::Market
// ============================================================================
static void exact_book_fuzz(size_t n_msgs) {
    // Ladders chosen so the generator's prices straddle every boundary:
    // one-cent grid, sub-penny grid, $1 grid past int32, near the NASDAQ price
    // cap, and a ladder starting at $0 on the highest stock_locate.
    const std::vector<WireSymbol> syms = {
        {1,     "EXA",  1'000'000,      100,    64},
        {2,     "SUBP", 5'000,          1,      512},
        {3,     "BIGT", 3'000'000'000u, 10'000, 100},
        {777,   "HIGH", 1'999'990'000u, 100,    128},
        {65535, "EDGE", 0,              100,    64},
    };
    std::printf("[6] ExactOrderBook differential fuzz: %zu generated wire-unit messages, "
                "%zu symbols, vs ref::Market\n", n_msgs, syms.size());
    WireGen gen(0xFACADEull, syms, 20'000);
    gen.generate(n_msgs);

    std::vector<std::unique_ptr<ExactOrderBook>> books(65536);
    for (const WireSymbol& s : syms)
        books[s.locate] = std::make_unique<ExactOrderBook>(
            BookParams{s.base, s.tick, s.band, 1u << 16, 17});
    ref::Market refm;

    uint64_t msgs = 0, bad_length = 0, mismatches = 0, audits = 0;
    DiffTotals tot;
    const uint64_t every = std::max<uint64_t>(1, n_msgs / 40);
    auto audit = [&] {
        ++audits;
        for (const WireSymbol& s : syms) {
            std::string d = diff_book(*books[s.locate], refm, s.locate, tot);
            if (!d.empty() && ++mismatches <= 5)
                std::printf("  mismatch after message %llu, %s: %s\n",
                            (unsigned long long)msgs, s.name.c_str(), d.c_str());
        }
    };
    const uint8_t* p   = gen.bytes.data();
    const uint8_t* end = p + gen.bytes.size();
    while (p + 2 <= end) {
        uint16_t len = static_cast<uint16_t>((p[0] << 8) | p[1]);
        const uint8_t* m = p + 2;
        if (static_cast<size_t>(end - m) < len) break;
        if (len >= 3) {
            uint16_t loc = static_cast<uint16_t>((m[1] << 8) | m[2]);
            if (books[loc]) itch::dispatch_checked<itch::WireUnits>(*books[loc], m, len, bad_length);
        }
        refm.on_message(m, len);
        p += 2 + len;
        if (++msgs % every == 0) audit();
    }
    audit();

    uint64_t ovf = 0, dropped = 0, unknown = 0, live = 0;
    for (const WireSymbol& s : syms) {
        ovf     += books[s.locate]->overflow_adds();
        dropped += books[s.locate]->dropped_out_of_band();
        unknown += books[s.locate]->unknown_id();
        live    += books[s.locate]->live_orders();
    }
    const ref::Counters& rc = refm.counters();
    const uint64_t adds = rc.seen['A'] + rc.seen['F'] + rc.seen['U'];
    CHECK(mismatches == 0, "ExactOrderBook matches ref::Market");
    CHECK(bad_length == 0 && rc.bad_length == 0, "no bad-length messages in a well-formed stream");
    CHECK(dropped == 0 && unknown == 0 && rc.unknown_ref == 0, "nothing dropped, no unknown ids");
    CHECK(ovf > adds / 10 && ovf < adds, "the stream exercises both the ladder and the overflow");
    std::printf("  %llu messages, %llu audits (%llu levels, %llu orders compared in queue order), "
                "%llu mismatches - %s\n",
                (unsigned long long)msgs, (unsigned long long)audits,
                (unsigned long long)tot.levels, (unsigned long long)tot.orders,
                (unsigned long long)mismatches, mismatches == 0 ? "PASS" : "FAIL");
    std::printf("  counters: overflow adds %llu of %llu | dropped %llu | unknown id %llu | "
                "bad-length %llu | live orders %llu\n\n",
                (unsigned long long)ovf, (unsigned long long)adds, (unsigned long long)dropped,
                (unsigned long long)unknown, (unsigned long long)bad_length,
                (unsigned long long)live);
}

// ============================================================================
// 7. The recorded-day replay path on a crafted file
// ============================================================================
// The same test `lob_replay --fixture` runs (src/replay_fixture.hpp): the
// lob_replay run over a 65-message file built to contain sub-penny prices,
// prices far outside every ladder and replaces across the ladder boundary,
// read in chunks from 1 byte up, with the reference compared after every
// message and the final books checked against levels written out by hand.
// Its size does not depend on --quick.
static void replay_fixture() {
    std::printf("[7] Recorded-day replay path on a crafted ITCH file (lob_replay --fixture)\n");
    const int rc = fixture::run(/*verbose=*/false, "lob_bench_fixture.itch", "lob_bench [7]");
    CHECK(rc == 0, "crafted-file replay");
    std::printf("\n");
}

// ============================================================================
// 8. Moving ladder (causal placement) unit checks
// ============================================================================
// Books built with BookParams::recenter (see include/lob/book.hpp): the first
// add places the ladder, near-touch misses move it. Wire units, 64-tick
// ladders, $0.01 grid at or above $1.00 and $0.0001 below. After every step
// the whole book is audited (ExactOrderBook::audit(): bitmap, cached best,
// every order's level_idx and price, both queues, the id map).
static void moving_ladder_checks() {
    std::printf("[8] Moving ladder unit checks (placement, moves with orders at the band edges, "
                "orders that moved, grid changes, what does not move a ladder)\n");
    const int before = g_failures;
    using Lvl = std::pair<uint64_t, uint32_t>;
    auto fifo = [](const ExactOrderBook& b, Side side, Price px) {
        std::vector<uint64_t> ids;
        b.for_each_level(side, [&](Price p, const PriceLevel& L) {
            if (p != px) return;
            for (uint32_t i = L.head; i != NIL; i = b.order_at(i).next) ids.push_back(b.order_at(i).id);
        });
        return ids;
    };
    auto idx = [](const ExactOrderBook& b, uint64_t id) { return b.find_order(id)->level_idx; };
#define AUDIT(b, what) CHECK((b).audit().empty(), what)

    // (a) Shift up, K = 2: orders resting on both edges of the ladder, an
    // overflow level that the move brings onto the ladder, and a deep order
    // that stays in the overflow; then every operation on orders that moved.
    if (want("8a")) {
        ExactOrderBook b(BookParams{0, 100, 64, 1u << 10, 12, Recenter{2, 10'000, 1}});
        CHECK(b.band() == 0, "a moving ladder starts unplaced");
        b.add(1, Side::Bid, 1'000'000, 100);                 // $100.00: places the ladder
        CHECK(b.placements() == 1 && b.recenters() == 0 && b.base() == 996'800 && b.tick() == 100 &&
              b.band() == 64 && idx(b, 1) == 32 && b.overflow_adds() == 0,
              "the first add places a 64-tick ladder centered on it");
        b.add(2, Side::Bid, 996'800, 10);                    // bottom edge (index 0)
        b.add(3, Side::Bid, 996'800, 20);
        b.add(4, Side::Ask, 1'003'100, 30);                  // top edge (index 63)
        b.add(5, Side::Ask, 1'004'000, 40);                  // near-touch miss 1 of 2: overflow
        b.add(6, Side::Bid, 990'000, 50);                    // 32+ ticks behind the best bid: not counted
        CHECK(b.recenters() == 0 && b.overflow_adds() == 2 && idx(b, 5) == kOverflowLevel &&
              idx(b, 2) == 0 && idx(b, 4) == 63, "one near-touch miss and one deep add do not move it");
        AUDIT(b, "audit before the move");
        b.add(7, Side::Ask, 1'003'300, 60);                  // near-touch miss 2 of 2: moves, then lands
        CHECK(b.recenters() == 1 && b.base() == 998'300 && b.overflow_adds() == 2,
              "the second near-touch miss re-centers on the midpoint $100.155 and lands on the ladder");
        CHECK(idx(b, 2) == kOverflowLevel && idx(b, 3) == kOverflowLevel,
              "the bottom-edge level left the ladder for the overflow");
        CHECK(idx(b, 1) == 17 && idx(b, 4) == 48 && idx(b, 5) == 57 && idx(b, 7) == 50 &&
              idx(b, 6) == kOverflowLevel,
              "shifted, imported and newly added orders carry their new ladder index");
        CHECK(b.level_at(Side::Bid, 996'800) == Lvl(30, 2) &&
              (fifo(b, Side::Bid, 996'800) == std::vector<uint64_t>{2, 3}),
              "the level that left keeps its shares, count and queue order");
        CHECK(b.level_at(Side::Ask, 1'004'000) == Lvl(40, 1), "the imported level keeps its shares");
        BBO q = b.bbo();
        CHECK(q.bid_price == 1'000'000 && q.ask_price == 1'003'100, "BBO across the move");
        AUDIT(b, "audit after the move");
        b.cancel(2, 4);                                      // partial cancel, moved to the overflow
        CHECK(b.level_at(Side::Bid, 996'800) == Lvl(26, 2), "partial cancel of an order that left the ladder");
        b.execute(3, 20);                                    // full execution, moved to the overflow
        CHECK(b.level_at(Side::Bid, 996'800) == Lvl(6, 1) && b.find_order(3) == nullptr,
              "full execution of an order that left the ladder");
        b.replace(2, 8, 1'000'100, 6);                       // overflow -> ladder
        CHECK(b.level_at(Side::Bid, 996'800) == Lvl(0, 0) && idx(b, 8) == 18 &&
              b.bbo().bid_price == 1'000'100, "replace of an order that left the ladder, back onto it");
        b.execute(5, 10);                                    // partial execution, imported
        CHECK(b.level_at(Side::Ask, 1'004'000) == Lvl(30, 1), "partial execution of an imported order");
        b.remove(4);                                         // delete, shifted
        CHECK(b.bbo().ask_price == 1'003'300 && b.bbo().ask_qty == 60, "delete of a shifted best ask");
        b.replace(5, 9, 1'003'100, 40);                      // replace of an imported order
        CHECK(idx(b, 9) == 48 && b.bbo().ask_price == 1'003'100 && b.level_at(Side::Ask, 1'004'000) == Lvl(0, 0),
              "replace of an imported order");
        b.replace(7, 10, 998'300, 60);                       // shifted order to the new bottom edge
        CHECK(idx(b, 10) == 0, "replace to the bottom edge of the moved ladder");
        AUDIT(b, "audit after operations on moved orders");
        CHECK(b.live_orders() == 5 && b.unknown_id() == 0, "five orders rest, no unknown ids");
        b.add(11, Side::Bid, 1'004'700, 5);                  // near-touch miss 1 of 2 since the move
        CHECK(b.recenters() == 1 && idx(b, 11) == kOverflowLevel,
              "a move resets the count: one more near-touch miss does not move it");
        AUDIT(b, "audit after a miss above the moved ladder");
    }

    // (b) Shift down, K = 1: levels that stay move up the ladder (visited top
    // down), the top-edge level leaves, a two-order level keeps its FIFO.
    if (want("8b")) {
        ExactOrderBook b(BookParams{0, 100, 64, 1u << 10, 12, Recenter{1, 10'000, 1}});
        b.add(1, Side::Bid, 500'000, 1);                     // places: base 496,800
        b.add(2, Side::Ask, 500'100, 1);                     // index 33
        b.add(3, Side::Bid, 496'800, 1);                     // bottom edge
        b.add(4, Side::Ask, 503'100, 1);                     // top edge
        b.add(5, Side::Bid, 499'000, 2);                     // index 22, two orders
        b.add(6, Side::Bid, 499'000, 3);
        b.remove(1);
        CHECK(b.base() == 496'800 && b.recenters() == 0 && idx(b, 4) == 63 && idx(b, 3) == 0,
              "placed at $49.68, edges on indices 0 and 63");
        b.add(7, Side::Ask, 496'500, 1);                     // better than the best ask: near; moves down 5 ticks
        CHECK(b.recenters() == 1 && b.base() == 496'300, "a near-touch miss below the ladder moves it down");
        CHECK(idx(b, 4) == kOverflowLevel && idx(b, 3) == 5 && idx(b, 2) == 38 && idx(b, 5) == 27 &&
              idx(b, 6) == 27 && idx(b, 7) == 2, "downward shift: the top edge leaves, the rest move up 5");
        CHECK((fifo(b, Side::Bid, 499'000) == std::vector<uint64_t>{5, 6}) &&
              b.level_at(Side::Bid, 499'000) == Lvl(5, 2), "a shifted two-order level keeps its queue");
        AUDIT(b, "audit after a downward shift");
        b.execute(4, 1);                                     // full execution in the overflow
        b.cancel(6, 1);
        b.replace(5, 8, 496'400, 2);
        CHECK(b.level_at(Side::Bid, 499'000) == Lvl(2, 1) && idx(b, 8) == 1 && b.overflow_levels() == 0,
              "operations on orders that shifted down");
        AUDIT(b, "audit after operations on shifted orders");
    }

    // (c) Grid changes, K = 1: a book above $1.00 on the $0.01 grid falls
    // below it and moves to the $0.0001 grid (a sub-penny overflow level comes
    // onto the ladder), then rises again (a sub-penny ladder level leaves).
    if (want("8c")) {
        ExactOrderBook b(BookParams{0, 100, 64, 1u << 10, 12, Recenter{1, 10'000, 1}});
        b.add(1, Side::Bid, 10'100, 100);                    // $1.01: places on the $0.01 grid
        b.add(2, Side::Ask, 10'200, 100);
        b.add(3, Side::Bid, 9'950, 50);                      // $0.9950: off that grid, not counted
        CHECK(b.tick() == 100 && b.base() == 6'900 && b.recenters() == 0 && idx(b, 3) == kOverflowLevel,
              "an add off the grid of the would-be center does not count");
        b.remove(1);
        b.remove(2);
        b.add(4, Side::Ask, 9'960, 10);                      // one-sided book, center $0.9960: $0.0001 grid
        CHECK(b.recenters() == 1 && b.move_stats().grid_changes == 1 && b.tick() == 1 && b.base() == 9'928 &&
              idx(b, 3) == 22 && idx(b, 4) == 32, "moving below $1.00 switches to the $0.0001 grid");
        b.add(5, Side::Bid, 9'930, 5);
        b.add(6, Side::Ask, 9'991, 7);                       // top edge
        b.add(7, Side::Ask, 10'100, 5);                      // 140 ticks behind the best ask: not counted
        CHECK(b.recenters() == 1 && idx(b, 6) == 63 && idx(b, 7) == kOverflowLevel,
              "a deep add past the top edge stays in the overflow");
        AUDIT(b, "audit on the $0.0001 grid");
        b.remove(3);
        b.remove(4);
        b.remove(5);
        b.add(8, Side::Bid, 10'000, 5);                      // center $1.00: back to the $0.01 grid
        CHECK(b.recenters() == 2 && b.move_stats().grid_changes == 2 && b.tick() == 100 && b.base() == 6'800 &&
              idx(b, 6) == kOverflowLevel && idx(b, 7) == 33 && idx(b, 8) == 32,
              "moving to $1.00 switches back: $0.9991 leaves the ladder, $1.01 comes onto it");
        AUDIT(b, "audit after two grid changes");
        b.remove(6);                                          // an order that left the ladder
        b.replace(7, 9, 10'200, 5);                          // an order that came onto it
        CHECK(b.level_at(Side::Ask, 10'200) == Lvl(5, 1) && idx(b, 9) == 34 && b.live_orders() == 2 &&
              b.overflow_levels() == 0 && b.recenters() == 2, "operations after the grid changes");
        AUDIT(b, "audit after operations on the $0.01 grid");
    }

    // (d) What does not move a ladder, and what does, K = 3: stub quotes,
    // deep orders and sub-penny prices never count; near-touch misses count
    // until the third; with a spread wider than half the ladder the move
    // centers on the add, and the bid that falls out of the window leaves.
    if (want("8d")) {
        ExactOrderBook b(BookParams{0, 100, 64, 1u << 10, 12, Recenter{3, 10'000, 1}});
        b.add(1, Side::Bid, 1'000'000, 1);
        b.add(2, Side::Ask, 1'000'100, 1);
        b.add(3, Side::Ask, 1'999'999'900, 1);               // stub ask
        b.add(4, Side::Bid, 100, 1);                         // stub bid
        b.add(5, Side::Bid, 900'000, 1);                     // deep bid
        b.add(6, Side::Bid, 1'000'050, 1);                   // sub-penny above $1.00
        CHECK(b.recenters() == 0 && b.overflow_adds() == 4, "stubs, deep orders and sub-penny prices do not count");
        b.remove(2);                                          // best ask is now the stub: spread too wide
        b.add(7, Side::Ask, 1'005'000, 1);                   // near-touch miss 1
        b.add(8, Side::Ask, 1'005'100, 1);                   // 2
        CHECK(b.recenters() == 0 && idx(b, 7) == kOverflowLevel, "two near-touch misses of three do not move it");
        b.add(9, Side::Ask, 1'005'200, 1);                   // 3: centers on this add
        CHECK(b.recenters() == 1 && b.base() == 1'002'000 && idx(b, 1) == kOverflowLevel && idx(b, 7) == 30 &&
              idx(b, 8) == 31 && idx(b, 9) == 32, "the third moves it, centered on the add");
        AUDIT(b, "audit after a move centered on the add");
    }
#undef AUDIT
    std::printf("  %s\n\n", g_failures == before ? "all passed" : "FAILURES ABOVE");
}

// ============================================================================
// 9. Moving ladder fuzz vs ref::Market
// ============================================================================
// Generated streams whose prices drift (WireGen::set_drift), through books
// whose ladders move (every combination of 64 or 256 ticks and K = 1, 2 or 8
// near-touch misses), including a symbol that crosses $1.00 and one whose
// window is clamped at the top of the price range. After every order message
// the book is compared with ref::Market on what the message touched
// (diff_touch), every 1,000 messages it is audited, and every n / 40 messages
// every book is compared in full.
static void moving_ladder_fuzz(size_t n_msgs) {
    const std::vector<WireSymbol> syms = {
        {10,    "MOV",  1'000'000,       100, 256},
        {11,    "PENY", 9'900,           1,   256},     // crosses $1.00 both ways
        {12,    "DIME", 10'050,          100, 64},      // starts above $1.00, drifts across
        {13,    "TOPP", 4'294'000'000u,  100, 64},      // the ladder clamps at the top of u32
        {14,    "LOWW", 700,             1,   128},     // the ladder clamps at $0
        {60000, "WIDE", 250'000,         100, 1024},
    };
    const uint32_t widths[] = {64, 256};
    const uint32_t ks[] = {1, 2, 8};
    std::printf("[9] Moving ladder fuzz: %zu drifting wire-unit messages x 6 ladder configurations, "
                "%zu symbols, vs ref::Market\n", n_msgs, syms.size());
    uint64_t total_moves = 0, total_grid = 0, total_orders = 0, total_msgs = 0, mismatches = 0, audits_bad = 0;
    TouchTotals touched;
    for (uint32_t w : widths) {
        for (uint32_t k : ks) {
            WireGen gen(0xC0FFEEull + w * 31 + k, syms, 20'000);
            gen.set_drift(3);
            gen.generate(n_msgs);
            std::vector<std::unique_ptr<ExactOrderBook>> books(65536);
            for (const WireSymbol& s : syms)
                books[s.locate] = std::make_unique<ExactOrderBook>(
                    BookParams{0, 100, w, 1u << 16, 17, Recenter{k, 10'000, 1}});
            ref::Market refm;
            uint64_t msgs = 0, bad_length = 0;
            DiffTotals tot;
            const uint64_t every = std::max<uint64_t>(1, n_msgs / 40);
            auto full = [&] {
                for (const WireSymbol& s : syms) {
                    std::string d = diff_book(*books[s.locate], refm, s.locate, tot);
                    if (!d.empty() && ++mismatches <= 5)
                        std::printf("  width %u K %u, mismatch after message %llu, %s: %s\n", w, k,
                                    (unsigned long long)msgs, s.name.c_str(), d.c_str());
                }
            };
            auto audit = [&] {
                for (const WireSymbol& s : syms) {
                    std::string d = books[s.locate]->audit();
                    if (!d.empty() && ++audits_bad <= 5)
                        std::printf("  width %u K %u, audit after message %llu, %s: %s\n", w, k,
                                    (unsigned long long)msgs, s.name.c_str(), d.c_str());
                }
            };
            const uint8_t* p   = gen.bytes.data();
            const uint8_t* end = p + gen.bytes.size();
            while (p + 2 <= end) {
                uint16_t len = static_cast<uint16_t>((p[0] << 8) | p[1]);
                const uint8_t* m = p + 2;
                if (static_cast<size_t>(end - m) < len) break;
                const Touch t = touch_before(refm, m, len);
                if (len >= 3) {
                    uint16_t loc = static_cast<uint16_t>((m[1] << 8) | m[2]);
                    if (books[loc]) itch::dispatch_checked<itch::WireUnits>(*books[loc], m, len, bad_length);
                }
                refm.on_message(m, len);
                if (t.check) {
                    std::string d = diff_touch(*books[t.locate], refm, t, touched);
                    if (!d.empty() && ++mismatches <= 5)
                        std::printf("  width %u K %u, after message %llu (%c): %s\n", w, k,
                                    (unsigned long long)msgs, static_cast<char>(t.type), d.c_str());
                }
                p += 2 + len;
                ++msgs;
                if (msgs % 1000 == 0) audit();
                if (msgs % every == 0) full();
            }
            full();
            audit();
            uint64_t moves = 0, grid = 0, relinked = 0, unknown = 0;
            for (const WireSymbol& s : syms) {
                moves += books[s.locate]->recenters();
                grid += books[s.locate]->move_stats().grid_changes;
                relinked += books[s.locate]->move_stats().orders;
                unknown += books[s.locate]->unknown_id();
            }
            CHECK(bad_length == 0 && unknown == 0 && refm.counters().unknown_ref == 0,
                  "moving ladder fuzz: no bad lengths, no unknown ids");
            total_moves += moves;
            total_grid += grid;
            total_orders += relinked;
            total_msgs += msgs;
        }
    }
    CHECK(mismatches == 0, "moving ladders match ref::Market");
    CHECK(audits_bad == 0, "moving ladders pass every audit");
    CHECK(total_moves >= 1000 && total_grid >= 10 && total_orders >= 10'000,
          "the fuzz moves ladders often, across grids, with orders resting");
    std::printf("  %llu messages, %llu checked after the message, %llu ladder moves (%llu onto a new grid), "
                "%llu orders relinked, %llu mismatches, %llu failed audits - %s\n\n",
                (unsigned long long)total_msgs, (unsigned long long)touched.messages,
                (unsigned long long)total_moves, (unsigned long long)total_grid,
                (unsigned long long)total_orders, (unsigned long long)mismatches,
                (unsigned long long)audits_bad, mismatches == 0 && audits_bad == 0 ? "PASS" : "FAIL");
}

int main(int argc, char** argv) {
    bool quick = false;
    unsigned cpu = 2;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0) quick = true;
        else if (std::strcmp(argv[i], "--cpu") == 0 && i + 1 < argc)
            cpu = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
            const std::string list = argv[++i];
            for (size_t a = 0; a <= list.size();) {
                size_t b = list.find(',', a);
                if (b == std::string::npos) b = list.size();
                if (b > a) g_only.push_back(list.substr(a, b - a));
                a = b + 1;
            }
        }
        else {
            std::printf("usage: %s [--quick] [--cpu N] [--only 1,2,...,7,8a,8b,8c,8d,9]\n", argv[0]);
            return 2;
        }
    }

    pin_and_boost(cpu);
    double cpn = cycles_per_ns();
    std::printf("=== L2 Limit Order Book - verification & benchmarks%s ===\n",
                quick ? " (--quick: CI sizes, not a measurement)" : "");
    std::printf("TSC: %.2f cycles/ns  |  sizeof(Order)=%zu  sizeof(PriceLevel)=%zu\n\n",
                cpn, sizeof(Order), sizeof(PriceLevel));

    if (want("1")) unit_checks();
    if (want("2")) differential_fuzz(quick ? 250'000 : 2'000'000);
    if (want("3")) latency_bench(cpn, quick ? 100'000 : 1'000'000);
    if (want("4")) throughput_bench(quick ? 1'000'000 : 10'000'000);
    if (want("5")) exact_book_checks();
    if (want("6")) exact_book_fuzz(quick ? 200'000 : 2'000'000);
    if (want("7")) replay_fixture();
    if (want("8a") || want("8b") || want("8c") || want("8d")) moving_ladder_checks();
    if (want("9")) moving_ladder_fuzz(quick ? 100'000 : 1'000'000);

    if (g_failures) { std::printf("*** %d FAILURE(S) ***\n", g_failures); return 1; }
    std::printf("All verification passed.\n");
    return 0;
}
