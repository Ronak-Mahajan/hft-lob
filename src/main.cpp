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
//
// Usage:  lob_bench [--quick]
//   --quick  CI mode: smaller message counts (fuzz 250k, latency 100k,
//            throughput 1M) so the whole run finishes in seconds. Every
//            correctness check still runs; only the benchmark sizes shrink,
//            and the numbers it prints are not the README's measurements.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

#include "lob/book.hpp"
#include "lob/itch.hpp"

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

static int g_failures = 0;
#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("  FAIL: %s (line %d)\n", what, __LINE__);         \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static void pin_and_boost() {
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << 2);  // pin to core 2
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
                std::printf("  BBO mismatch at msg %zu: fast %d/%llu %d/%llu  ref %d/%llu %d/%llu\n",
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
    std::printf("  %zu messages in %.3f s  →  %.1f M msgs/s  (%.1f ns/msg)\n",
                done, secs, static_cast<double>(done) / secs / 1e6,
                secs * 1e9 / static_cast<double>(done));

    BBO q = book.bbo();
    std::printf("  final book: bid %d x %llu | ask %d x %llu\n",
                q.bid_price, (unsigned long long)q.bid_qty,
                q.ask_price, (unsigned long long)q.ask_qty);
    CHECK(done == gen.ops.size(), "every framed message consumed");
    CHECK(fh.bad_length() == 0, "no bad-length messages in a well-formed stream");
    std::printf("  counters: out-of-band drops %llu | bad-length %llu | live orders %llu\n\n",
                (unsigned long long)book.dropped_out_of_band(),
                (unsigned long long)fh.bad_length(),
                (unsigned long long)book.live_orders());
}

int main(int argc, char** argv) {
    bool quick = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0) quick = true;
        else {
            std::printf("usage: %s [--quick]\n", argv[0]);
            return 2;
        }
    }

    pin_and_boost();
    double cpn = cycles_per_ns();
    std::printf("=== L2 Limit Order Book - verification & benchmarks%s ===\n",
                quick ? " (--quick: CI sizes, not a measurement)" : "");
    std::printf("TSC: %.2f cycles/ns  |  sizeof(Order)=%zu  sizeof(PriceLevel)=%zu\n\n",
                cpn, sizeof(Order), sizeof(PriceLevel));

    unit_checks();
    differential_fuzz(quick ? 250'000 : 2'000'000);
    latency_bench(cpn, quick ? 100'000 : 1'000'000);
    throughput_bench(quick ? 1'000'000 : 10'000'000);

    if (g_failures) { std::printf("*** %d FAILURE(S) ***\n", g_failures); return 1; }
    std::printf("All verification passed.\n");
    return 0;
}
