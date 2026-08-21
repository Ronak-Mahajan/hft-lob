#pragma once
// ---------------------------------------------------------------------------
// lob/engine.hpp — Phase 5: sharded multi-core market engine.
//
// Topology:
//
//     demux (this thread, core 0)
//        │  reads the wire stream, peeks stock_locate (2 bytes),
//        │  routes the raw message into shard ring  locate % W
//        ├── SpscRing ──► worker 0 (core 1)  owns books where locate%W == 0
//        ├── SpscRing ──► worker 1 (core 2)  owns books where locate%W == 1
//        └── ...
//
// The correctness invariant that makes the books lock-free: ALL messages for
// a given instrument land on ONE worker, and the ring is FIFO — so each book
// still sees its message stream in exchange order, and the single-writer
// LimitOrderBook from Phase 2 is reused byte-for-byte, no atomics added.
// Cross-instrument ordering is not preserved, and doesn't need to be (the
// venue itself splits ITCH across parallel MoldUDP channels).
//
// Memory placement: each worker constructs its own books inside its own
// thread (first-touch), so slabs/ladders/idmaps are filled from the core
// that will hammer them. Per-worker message counters are 128B-padded.
// ---------------------------------------------------------------------------
#include <atomic>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

#include "book.hpp"
#include "itch.hpp"
#include "spsc.hpp"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

namespace lob {

inline void pin_current_thread(unsigned cpu) {
#ifdef _WIN32
    SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#else
    (void)cpu;   // linux: pthread_setaffinity_np
#endif
}

struct BookConfig {
    int32_t  base_tick;
    uint32_t band;
    uint32_t max_live_orders;
    unsigned idmap_log2;
};

class ParallelEngine {
public:
    ParallelEngine(unsigned n_workers, uint16_t n_instruments, BookConfig cfg)
        : W_(n_workers), n_inst_(n_instruments), cfg_(cfg),
          rings_(n_workers), books_(size_t{n_instruments} + 1),
          counts_(n_workers)
    {
        for (auto& r : rings_) r = std::make_unique<Ring>();
    }

    // Demux + process a length-prefixed ITCH stream. Caller's thread becomes
    // the producer (pinned to cpu 0). Returns wall seconds measured from
    // "all workers ready" to "all rings drained and workers joined".
    double run(const uint8_t* buf, size_t len) {
        std::atomic<bool> done{false};
        std::latch ready(static_cast<ptrdiff_t>(W_) + 1);
        std::vector<std::thread> threads;
        threads.reserve(W_);
        for (unsigned w = 0; w < W_; ++w)
            threads.emplace_back([&, w] { worker_main(w, ready, done); });

        pin_current_thread(0);
        ready.arrive_and_wait();          // books built, everyone pinned
        auto t0 = std::chrono::steady_clock::now();

        const uint8_t* p   = buf;
        const uint8_t* end = buf + len;
        while (p + 2 <= end) {
            uint16_t mlen = static_cast<uint16_t>((p[0] << 8) | p[1]);
            const uint8_t* msg = p + 2;
            if (LOB_UNLIKELY(msg + mlen > end)) break;
            // stock_locate sits at bytes 1..2 of every order message — the
            // demux never decodes more than that.
            uint16_t locate = static_cast<uint16_t>((msg[1] << 8) | msg[2]);
            Ring& r = *rings_[locate % W_];
            while (LOB_UNLIKELY(!r.try_push(msg, mlen)))
                _mm_pause();              // backpressure: shard is saturated
            p += 2 + mlen;
        }
        done.store(true, std::memory_order_release);
        for (auto& t : threads) t.join();
        auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(t1 - t0).count();
    }

    LimitOrderBook*       book(uint16_t locate)       { return books_[locate].get(); }
    const LimitOrderBook* book(uint16_t locate) const { return books_[locate].get(); }
    uint64_t processed(unsigned w) const { return counts_[w].v; }

private:
    using Ring = SpscRing<14>;            // 16384 slots × 64B = 1 MB/shard

    struct alignas(kIsolate) PaddedU64 { uint64_t v = 0; };

    void worker_main(unsigned wid, std::latch& ready, std::atomic<bool>& done) {
        pin_current_thread(wid + 1);
        // First-touch: build the books this worker owns from its own core.
        // Workers write disjoint books_ entries → no synchronization needed.
        for (uint16_t loc = 1; loc <= n_inst_; ++loc)
            if (loc % W_ == wid)
                books_[loc] = std::make_unique<LimitOrderBook>(
                    cfg_.base_tick, cfg_.band, cfg_.max_live_orders,
                    cfg_.idmap_log2);
        ready.arrive_and_wait();

        Ring& ring = *rings_[wid];
        uint64_t n_done = 0;
        auto handle = [&](const MsgSlot& s) {
            uint16_t locate = static_cast<uint16_t>((s.data[1] << 8) | s.data[2]);
            if (LOB_LIKELY(locate >= 1 && locate <= n_inst_))
                itch::dispatch(*books_[locate], s.data);
        };
        unsigned idle = 0;
        for (;;) {
            uint64_t n = ring.consume_batch(handle);
            if (LOB_LIKELY(n != 0)) { n_done += n; idle = 0; continue; }
            if (done.load(std::memory_order_acquire)) {
                // done was published AFTER the final push (release/acquire),
                // so one more drain loop is guaranteed to see everything.
                while ((n = ring.consume_batch(handle)) != 0) n_done += n;
                break;
            }
            // Exponential backoff on an empty ring. A tight poll keeps the
            // ring's tail_ line in Shared state, so every producer push to
            // this shard pays a request-for-ownership — idle consumers were
            // measurably slowing the producer down. Backoff caps at ~64
            // pauses (≈ a few hundred ns of added wake-up latency; a
            // latency-critical deployment would bound this lower).
            for (unsigned s = 1u << (idle < 6 ? idle : 6); s; --s)
                _mm_pause();
            if (idle < 6) ++idle;
        }
        counts_[wid].v = n_done;
    }

    unsigned   W_;
    uint16_t   n_inst_;
    BookConfig cfg_;
    std::vector<std::unique_ptr<Ring>>           rings_;
    std::vector<std::unique_ptr<LimitOrderBook>> books_;
    std::vector<PaddedU64>                       counts_;
};

} // namespace lob
