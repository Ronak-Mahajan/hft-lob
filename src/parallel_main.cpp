// ---------------------------------------------------------------------------
// parallel_main.cpp - Phase 5: verification & scaling benchmarks.
//
// 1. Verification: the SAME multi-instrument stream is processed (a) by the
//    parallel engine across N cores and (b) sequentially on one thread; every
//    instrument's full depth must match exactly. This proves the sharding
//    invariant (per-instrument order preserved through demux + ring).
// 2. Demux scaling: 1 producer fanning out to W workers, the realistic
//    single-feed topology; exposes the demux thread as the eventual ceiling.
// 3. Multi-channel scaling: the stream pre-split per shard (exactly how
//    NASDAQ actually distributes ITCH across parallel MoldUDP channels),
//    each worker ingesting its own channel; measures aggregate book
//    throughput without a demux bottleneck.
//
// Usage:  lob_parallel [--quick]
//   --quick  CI mode: a 2M-message stream instead of 16M and at most 4
//            workers, so the verification finishes in seconds on a 4-vCPU
//            runner. The scaling tables it prints are not measurements.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <latch>
#include <thread>
#include <unordered_map>
#include <vector>

#include "lob/engine.hpp"

using namespace lob;

// Per-instrument book config: $0.01 ticks, band 8192 ticks around $100 mids.
static constexpr BookConfig kCfg = {
    /*base_tick*/ 6000, /*band*/ 1u << 13,
    /*max_live_orders*/ 1u << 15, /*idmap_log2*/ 16,
};
static constexpr uint16_t kInstruments = 128;
static constexpr size_t   kMessages    = 16'000'000;   // full run (README)
static constexpr size_t   kQuickMessages = 2'000'000;  // --quick (CI)
static constexpr uint32_t kMaxLivePerInst = 20'000;

struct SplitMix64 {
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

// ============================================================================
// Multi-instrument mock ITCH stream (interleaved across stock_locates).
// ============================================================================
class MarketMock {
public:
    explicit MarketMock(uint64_t seed, uint16_t n_inst)
        : rng_(seed), inst_(n_inst) {
        for (auto& s : inst_) s.mid = 10'000;
    }

    void generate(size_t n) {
        bytes.reserve(n * 36);
        for (size_t i = 0; i < n; ++i) {
            uint16_t locate = static_cast<uint16_t>(
                1 + rng_.below(static_cast<uint32_t>(inst_.size())));
            Inst& in = inst_[locate - 1];
            if ((++in.n_msgs & 1023) == 0)
                in.mid += static_cast<int32_t>(rng_.below(3)) - 1;

            uint32_t roll = rng_.below(100);
            bool force_add = in.live.size() < 500;
            bool force_del = in.live.size() >= kMaxLivePerInst;
            if (force_add || (!force_del && roll < 40)) emit_add(locate, in);
            else if (force_del || roll < 65)            emit_delete(locate, in);
            else if (roll < 75)                         emit_execute(locate, in);
            else if (roll < 85)                         emit_cancel(locate, in);
            else                                        emit_replace(locate, in);
        }
    }

    std::vector<uint8_t> bytes;

private:
    struct Gen { int32_t price; uint32_t qty; char side; uint32_t live_pos; };
    struct Inst {
        int32_t mid;
        uint64_t n_msgs = 0;
        std::vector<uint64_t> live;
        std::unordered_map<uint64_t, Gen> gen;
    };

    void p8(uint8_t v)   { bytes.push_back(v); }
    void p16(uint16_t v) { p8(static_cast<uint8_t>(v >> 8)); p8(static_cast<uint8_t>(v)); }
    void p32(uint32_t v) { p16(static_cast<uint16_t>(v >> 16)); p16(static_cast<uint16_t>(v)); }
    void p48(uint64_t v) { p16(static_cast<uint16_t>(v >> 32)); p32(static_cast<uint32_t>(v)); }
    void p64(uint64_t v) { p32(static_cast<uint32_t>(v >> 32)); p32(static_cast<uint32_t>(v)); }

    void header(char type, uint16_t body_len, uint16_t locate) {
        p16(body_len);
        p8(static_cast<uint8_t>(type));
        p16(locate);
        p16(0);
        p48(++ts_);
    }
    int32_t pick_price(const Inst& in, char side) {
        uint32_t r = rng_.below(100);
        int32_t d = (r < 60) ? static_cast<int32_t>(rng_.below(3))
                  : (r < 90) ? static_cast<int32_t>(rng_.below(16))
                             : static_cast<int32_t>(rng_.below(128));
        return (side == 'B') ? in.mid - 1 - d : in.mid + 1 + d;
    }
    uint64_t pick_live(Inst& in) {
        return in.live[rng_.below(static_cast<uint32_t>(in.live.size()))];
    }
    void drop_live(Inst& in, uint64_t id) {
        uint32_t pos = in.gen[id].live_pos;
        uint64_t back = in.live.back();
        in.live[pos] = back;
        in.gen[back].live_pos = pos;
        in.live.pop_back();
        in.gen.erase(id);
    }

    void emit_add(uint16_t locate, Inst& in) {
        uint64_t id = ++next_id_;
        char side = (rng_.next() & 1) ? 'B' : 'S';
        int32_t price = pick_price(in, side);
        uint32_t qty = 1 + rng_.below(500);
        header('A', 36, locate);
        p64(id); p8(static_cast<uint8_t>(side)); p32(qty);
        bytes.insert(bytes.end(), {'M','O','C','K',' ',' ',' ',' '});
        p32(static_cast<uint32_t>(price) * 100);
        in.gen[id] = {price, qty, side, static_cast<uint32_t>(in.live.size())};
        in.live.push_back(id);
    }
    void emit_delete(uint16_t locate, Inst& in) {
        uint64_t id = pick_live(in);
        header('D', 19, locate); p64(id);
        drop_live(in, id);
    }
    void emit_execute(uint16_t locate, Inst& in) {
        uint64_t id = pick_live(in);
        Gen& g = in.gen[id];
        uint32_t d = 1 + rng_.below(g.qty);
        header('E', 31, locate); p64(id); p32(d); p64(++match_);
        if (d >= g.qty) drop_live(in, id); else g.qty -= d;
    }
    void emit_cancel(uint16_t locate, Inst& in) {
        uint64_t id = pick_live(in);
        Gen& g = in.gen[id];
        if (g.qty <= 1) { header('D', 19, locate); p64(id); drop_live(in, id); return; }
        uint32_t d = 1 + rng_.below(g.qty - 1);
        header('X', 23, locate); p64(id); p32(d);
        g.qty -= d;
    }
    void emit_replace(uint16_t locate, Inst& in) {
        uint64_t old_id = pick_live(in);
        uint64_t new_id = ++next_id_;
        char side = in.gen[old_id].side;
        int32_t price = pick_price(in, side);
        uint32_t qty = 1 + rng_.below(500);
        header('U', 35, locate);
        p64(old_id); p64(new_id); p32(qty);
        p32(static_cast<uint32_t>(price) * 100);
        drop_live(in, old_id);
        in.gen[new_id] = {price, qty, side, static_cast<uint32_t>(in.live.size())};
        in.live.push_back(new_id);
    }

    SplitMix64 rng_;
    std::vector<Inst> inst_;
    uint64_t next_id_ = 0, match_ = 0, ts_ = 0;
};

// ============================================================================
// Sequential baseline: one thread, locate → book table. Also the reference
// for differential verification of the parallel run.
// ============================================================================
struct SequentialMarket {
    std::vector<std::unique_ptr<LimitOrderBook>> books;
    uint64_t bad_length = 0;

    explicit SequentialMarket(uint16_t n_inst) : books(size_t{n_inst} + 1) {
        for (uint16_t i = 1; i <= n_inst; ++i)
            books[i] = std::make_unique<LimitOrderBook>(
                kCfg.base_tick, kCfg.band, kCfg.max_live_orders, kCfg.idmap_log2);
    }
    double run(const uint8_t* buf, size_t len) {
        auto t0 = std::chrono::steady_clock::now();
        const uint8_t* p = buf;
        const uint8_t* end = buf + len;
        while (p + 2 <= end) {
            uint16_t mlen = static_cast<uint16_t>((p[0] << 8) | p[1]);
            const uint8_t* msg = p + 2;
            if (static_cast<size_t>(end - msg) < mlen) break;
            if (mlen < 3) { ++bad_length; p += 2 + mlen; continue; }
            uint16_t locate = static_cast<uint16_t>((msg[1] << 8) | msg[2]);
            if (locate >= 1 && locate < books.size())
                itch::dispatch_checked(*books[locate], msg, mlen, bad_length);
            p += 2 + mlen;
        }
        auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(t1 - t0).count();
    }
    uint64_t dropped_out_of_band_total() const {
        uint64_t s = 0;
        for (const auto& b : books) if (b) s += b->dropped_out_of_band();
        return s;
    }
};

// Full-depth equality of two books across the entire band.
static bool books_equal(const LimitOrderBook& a, const LimitOrderBook& b) {
    for (int32_t px = kCfg.base_tick;
         px < kCfg.base_tick + static_cast<int32_t>(kCfg.band); ++px) {
        if (a.level_at(Side::Bid, px) != b.level_at(Side::Bid, px)) return false;
        if (a.level_at(Side::Ask, px) != b.level_at(Side::Ask, px)) return false;
    }
    return true;
}

// ============================================================================
// Multi-channel mode: pre-split the stream per shard (as the venue does with
// parallel MoldUDP channels); each worker ingests its own channel directly.
// ============================================================================
static double multichannel_run(const std::vector<std::vector<uint8_t>>& chans,
                               uint16_t n_inst, uint64_t& bad_length_out) {
    unsigned W = static_cast<unsigned>(chans.size());
    std::latch ready(static_cast<ptrdiff_t>(W) + 1);
    std::vector<std::thread> threads;
    std::vector<uint64_t> bad_len(W, 0);     // written by one worker each
    threads.reserve(W);
    for (unsigned w = 0; w < W; ++w) {
        threads.emplace_back([&, w] {
            pin_current_thread(w + 1);
            // first-touch books for this channel's instruments
            std::vector<std::unique_ptr<LimitOrderBook>> books(size_t{n_inst} + 1);
            for (uint16_t loc = 1; loc <= n_inst; ++loc)
                if (loc % W == w)
                    books[loc] = std::make_unique<LimitOrderBook>(
                        kCfg.base_tick, kCfg.band, kCfg.max_live_orders,
                        kCfg.idmap_log2);
            ready.arrive_and_wait();
            uint64_t bad = 0;
            const auto& ch = chans[w];
            const uint8_t* p = ch.data();
            const uint8_t* end = p + ch.size();
            while (p + 2 <= end) {
                uint16_t mlen = static_cast<uint16_t>((p[0] << 8) | p[1]);
                const uint8_t* msg = p + 2;
                if (static_cast<size_t>(end - msg) < mlen) break;
                if (mlen < 3) { ++bad; p += 2 + mlen; continue; }
                uint16_t locate = static_cast<uint16_t>((msg[1] << 8) | msg[2]);
                if (locate >= 1 && locate <= n_inst && books[locate])
                    itch::dispatch_checked(*books[locate], msg, mlen, bad);
                p += 2 + mlen;
            }
            bad_len[w] = bad;
        });
    }
    pin_current_thread(0);
    ready.arrive_and_wait();
    auto t0 = std::chrono::steady_clock::now();
    for (auto& t : threads) t.join();
    auto t1 = std::chrono::steady_clock::now();
    for (uint64_t b : bad_len) bad_length_out += b;
    return std::chrono::duration<double>(t1 - t0).count();
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
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif
    const size_t n_messages = quick ? kQuickMessages : kMessages;
    unsigned hw = std::thread::hardware_concurrency();
    unsigned max_workers = hw > 1 ? hw - 1 : 1;   // core 0 reserved for demux
    if (quick) max_workers = std::min(max_workers, 4u);
    std::printf("=== Phase 5: sharded multi-core market engine%s ===\n",
                quick ? " (--quick: CI sizes, not a measurement)" : "");
    std::printf("hardware threads: %u | instruments: %u | messages: %zu\n\n",
                hw, kInstruments, n_messages);

    std::printf("generating %zu-message market stream...\n", n_messages);
    MarketMock mock(0x5EEDF00Dull, kInstruments);
    mock.generate(n_messages);
    std::printf("stream: %.1f MB\n\n", static_cast<double>(mock.bytes.size()) / 1e6);

    // ---- 1. sequential baseline + parallel verification --------------------
    std::printf("[1] Sequential baseline (1 thread, %u books)\n", kInstruments);
    SequentialMarket seq(kInstruments);
    double t_seq = seq.run(mock.bytes.data(), mock.bytes.size());
    double seq_rate = static_cast<double>(n_messages) / t_seq / 1e6;
    std::printf("  %.3f s  →  %.1f M msgs/s\n", t_seq, seq_rate);
    std::printf("  counters: out-of-band drops %llu | bad-length %llu\n\n",
                (unsigned long long)seq.dropped_out_of_band_total(),
                (unsigned long long)seq.bad_length);

    // W=8 is the recorded configuration; --quick caps it at the worker budget
    // (hardware threads minus the demux core, at most 4), because spinning
    // workers oversubscribing a 4-vCPU runner would only slow the check down
    // without testing anything extra.
    const unsigned verify_w = quick ? std::min(8u, std::max(2u, max_workers)) : 8u;
    std::printf("[2] Parallel-vs-sequential differential verification (W=%u)\n",
                verify_w);
    ParallelEngine verify_eng(verify_w, kInstruments, kCfg);
    verify_eng.run(mock.bytes.data(), mock.bytes.size());
    size_t bad = 0;
    for (uint16_t loc = 1; loc <= kInstruments; ++loc)
        if (!books_equal(*verify_eng.book(loc), *seq.books[loc])) {
            ++bad;
            std::printf("  MISMATCH: instrument %u\n", loc);
        }
    uint64_t par_processed = 0;
    for (unsigned w = 0; w < verify_w; ++w) par_processed += verify_eng.processed(w);
    if (par_processed != n_messages) {
        ++bad;
        std::printf("  MISMATCH: workers processed %llu of %zu messages\n",
                    (unsigned long long)par_processed, n_messages);
    }
    if (verify_eng.bad_length_total() != seq.bad_length ||
        verify_eng.dropped_out_of_band_total() != seq.dropped_out_of_band_total()) {
        ++bad;
        std::printf("  MISMATCH: counters differ (bad-length %llu vs %llu, "
                    "drops %llu vs %llu)\n",
                    (unsigned long long)verify_eng.bad_length_total(),
                    (unsigned long long)seq.bad_length,
                    (unsigned long long)verify_eng.dropped_out_of_band_total(),
                    (unsigned long long)seq.dropped_out_of_band_total());
    }
    std::printf("  %u instruments, full-band depth compare: %s\n\n",
                kInstruments, bad ? "FAIL" : "all identical - PASS");
    if (bad) return 1;

    // ---- 2. demux fan-out scaling ------------------------------------------
    std::printf("[3] Demux fan-out scaling (1 producer + W workers)\n");
    std::printf("  %3s | %9s | %8s\n", "W", "M msgs/s", "speedup");
    std::vector<unsigned> ws;
    for (unsigned w : {1u, 2u, 4u, 8u, 12u, 16u, max_workers})
        if (w <= max_workers && (ws.empty() || w > ws.back())) ws.push_back(w);
    for (unsigned w : ws) {
        ParallelEngine eng(w, kInstruments, kCfg);
        double secs = eng.run(mock.bytes.data(), mock.bytes.size());
        double rate = static_cast<double>(n_messages) / secs / 1e6;
        std::printf("  %3u | %9.1f | %7.2fx\n", w, rate, rate / seq_rate);
    }

    // ---- 3. multi-channel scaling ------------------------------------------
    std::printf("\n[4] Multi-channel scaling (per-shard feeds, no demux - venue-style)\n");
    std::printf("  %3s | %9s | %8s\n", "W", "M msgs/s", "speedup");
    for (unsigned w : ws) {
        // offline split: channel w gets instruments where locate % W == w
        std::vector<std::vector<uint8_t>> chans(w);
        for (auto& c : chans) c.reserve(mock.bytes.size() / w + 64);
        const uint8_t* p = mock.bytes.data();
        const uint8_t* end = p + mock.bytes.size();
        while (p + 2 <= end) {
            uint16_t mlen = static_cast<uint16_t>((p[0] << 8) | p[1]);
            const uint8_t* msg = p + 2;
            if (static_cast<size_t>(end - msg) < mlen) break;
            if (mlen < 3) { p += 2 + mlen; continue; }   // unroutable frame
            uint16_t locate = static_cast<uint16_t>((msg[1] << 8) | msg[2]);
            auto& c = chans[locate % w];
            c.insert(c.end(), p, msg + mlen);
            p += 2 + mlen;
        }
        uint64_t bad_length = 0;
        double secs = multichannel_run(chans, kInstruments, bad_length);
        double rate = static_cast<double>(n_messages) / secs / 1e6;
        std::printf("  %3u | %9.1f | %7.2fx%s\n", w, rate, rate / seq_rate,
                    bad_length ? "  (bad-length frames seen!)" : "");
    }

    std::printf("\ndone.\n");
    return 0;
}
