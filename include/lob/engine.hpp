#pragma once
// ---------------------------------------------------------------------------
// lob/engine.hpp - Phase 5: sharded multi-core market engine.
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
// The correctness invariant that lets the books stay single-writer, and so
// need no locks and no atomics of their own: ALL messages for a given
// instrument land on ONE worker, and the ring is FIFO, so each book
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
#include <chrono>
#include <functional>
#include <latch>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
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
#elif defined(__linux__)
  #include <pthread.h>
  #include <sched.h>
#endif

namespace lob {

// Pin the calling thread to one logical CPU. Best effort: a cpu index the
// host does not have (e.g. a 23-worker configuration on a 4-vCPU CI runner)
// is ignored and the thread stays unpinned. Returns true if the pin took.
inline bool pin_current_thread(unsigned cpu) {
#ifdef _WIN32
    if (cpu >= 64) return false;
    DWORD_PTR ok = SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    return ok != 0;
#elif defined(__linux__)
    if (cpu >= CPU_SETSIZE) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void)cpu;   // other platforms: no pinning
    return false;
#endif
}

struct BookConfig {
    int32_t  base_tick;
    uint32_t band;
    uint32_t max_live_orders;
    unsigned idmap_log2;
};

// BasicParallelEngine<Book, Units>: the engine over any book type and price
// conversion. ParallelEngine (below) is the original configuration, one
// LimitOrderBook per instrument in cent ticks; the recorded-day tools use
// BasicParallelEngine<ExactOrderBook, itch::WireUnits> with one book per
// stock_locate built by a factory.
//
// Two ways to drive it:
//   run(buf, len)       the whole stream in memory: start, demux, finish.
//                       Returns wall seconds from "all workers ready" to
//                       "all rings drained and workers joined".
//   start(); { feed(); wait_drained(); }... finish();
//                       a stream too large for memory, fed one chunk at a
//                       time. feed() demuxes every whole message of the
//                       chunk and returns the bytes it consumed, so the
//                       caller can carry a straddling message into its next
//                       read; wait_drained() returns once every worker has
//                       applied everything pushed so far.
template <class Book = LimitOrderBook, class Units = itch::CentTicks>
class BasicParallelEngine {
public:
    // Builds the book for one locate, or returns nullptr for a locate that
    // carries no book (its messages are then skipped). Called on the owning
    // worker's thread, so the book is first-touched from its core.
    using Factory = std::function<std::unique_ptr<Book>(uint16_t locate)>;

    // Original constructor: locates 1..n_instruments each get a
    // LimitOrderBook built from cfg; demux on cpu 0, worker w on cpu w + 1.
    BasicParallelEngine(unsigned n_workers, uint16_t n_instruments, BookConfig cfg)
        requires std::is_same_v<Book, LimitOrderBook>
        : BasicParallelEngine(n_workers, n_instruments, [cfg](uint16_t) {
              return std::make_unique<LimitOrderBook>(cfg.base_tick, cfg.band,
                                                      cfg.max_live_orders, cfg.idmap_log2);
          }) {}

    // `cpus`, when it has at least n_workers + 1 entries, gives the demux
    // cpu (cpus[0]) and worker w's cpu (cpus[w + 1]); otherwise the demux
    // takes cpu 0 and worker w cpu w + 1.
    BasicParallelEngine(unsigned n_workers, uint16_t n_instruments, Factory make,
                        std::vector<unsigned> cpus = {})
        : W_(n_workers), n_inst_(n_instruments), make_(std::move(make)),
          rings_(n_workers), books_(size_t{n_instruments} + 1),
          counts_(n_workers)
    {
        if (cpus.size() < size_t{n_workers} + 1) {
            cpus.resize(size_t{n_workers} + 1);
            for (unsigned i = 0; i <= n_workers; ++i) cpus[i] = i;
        }
        cpus_ = std::move(cpus);
        for (auto& r : rings_) r = std::make_unique<Ring>();
    }

    ~BasicParallelEngine() { if (started_ && !finished_) finish(); }
    BasicParallelEngine(const BasicParallelEngine&) = delete;
    BasicParallelEngine& operator=(const BasicParallelEngine&) = delete;

    double run(const uint8_t* buf, size_t len) {
        start();
        auto t0 = std::chrono::steady_clock::now();
        feed(buf, len);
        finish();
        auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(t1 - t0).count();
    }

    // Spawns and pins the workers, pins the caller as the demux, and returns
    // once every worker has built its books.
    void start() {
        started_ = true;
        ready_ = std::make_unique<std::latch>(static_cast<ptrdiff_t>(W_) + 1);
        threads_.reserve(W_);
        for (unsigned w = 0; w < W_; ++w)
            threads_.emplace_back([this, w] { worker_main(w); });
        pin_current_thread(cpus_[0]);
        ready_->arrive_and_wait();        // books built, everyone pinned
    }

    // Demuxes every whole length-prefixed message in [buf, buf + len) and
    // returns the bytes consumed (a trailing partial message is left).
    size_t feed(const uint8_t* buf, size_t len) {
        const uint8_t* p   = buf;
        const uint8_t* end = buf + len;
        while (p + 2 <= end) {
            uint16_t mlen = static_cast<uint16_t>((p[0] << 8) | p[1]);
            const uint8_t* msg = p + 2;
            if (LOB_UNLIKELY(static_cast<size_t>(end - msg) < mlen)) break;
            // Frame sanity before the ring: a message must carry at least
            // type + stock_locate (3 bytes) and fit one 62-byte slot (the
            // largest ITCH 5.0 message is 50). Anything else is a corrupt
            // prefix; it is skipped by prefix and counted, never pushed.
            if (LOB_UNLIKELY(mlen < 3 || mlen > sizeof(MsgSlot::data))) {
                ++demux_bad_length_;
                p += 2 + mlen;
                continue;
            }
            // stock_locate sits at bytes 1..2 of every order message - the
            // demux never decodes more than that.
            uint16_t locate = static_cast<uint16_t>((msg[1] << 8) | msg[2]);
            Ring& r = *rings_[locate % W_];
            while (LOB_UNLIKELY(!r.try_push(msg, mlen)))
                _mm_pause();              // backpressure: shard is saturated
            p += 2 + mlen;
        }
        return static_cast<size_t>(p - buf);
    }

    // Returns once every message pushed so far has been applied. A worker
    // publishes its ring's head only after applying the batch, so a drained
    // ring means its books are up to date.
    void wait_drained() {
        for (auto& r : rings_)
            while (!r->drained()) _mm_pause();
    }

    void finish() {
        done_.store(true, std::memory_order_release);
        for (auto& t : threads_) t.join();
        finished_ = true;
    }

    unsigned workers() const { return W_; }
    unsigned cpu(unsigned i) const { return cpus_[i]; }   // 0: demux, w + 1: worker w
    Book*       book(uint16_t locate)       { return books_[locate].get(); }
    const Book* book(uint16_t locate) const { return books_[locate].get(); }
    uint64_t processed(unsigned w) const { return counts_[w].v; }

    // Messages a worker refused because the length prefix did not match
    // the per-type table (itch::expected_len). 0 on a clean feed.
    uint64_t bad_length(unsigned w) const { return counts_[w].bad_len; }
    // Frames the demux skipped for being too short or too long for a slot.
    uint64_t demux_bad_length() const { return demux_bad_length_; }
    // Totals across every worker and the demux.
    uint64_t bad_length_total() const {
        uint64_t s = demux_bad_length_;
        for (unsigned w = 0; w < W_; ++w) s += counts_[w].bad_len;
        return s;
    }
    uint64_t dropped_out_of_band_total() const {
        uint64_t s = 0;
        for (const auto& b : books_) if (b) s += b->dropped_out_of_band();
        return s;
    }

private:
    using Ring = SpscRing<14>;            // 16384 slots x 64B = 1 MB/shard

    struct alignas(kIsolate) PaddedU64 { uint64_t v = 0; uint64_t bad_len = 0; };

    void worker_main(unsigned wid) {
        pin_current_thread(cpus_[wid + 1]);
        // First-touch: build the books this worker owns from its own core.
        // Workers write disjoint books_ entries, so no synchronization needed.
        for (uint32_t loc = 1; loc <= n_inst_; ++loc)
            if (loc % W_ == wid) books_[loc] = make_(static_cast<uint16_t>(loc));
        ready_->arrive_and_wait();

        Ring& ring = *rings_[wid];
        uint64_t n_done = 0;
        uint64_t bad_len = 0;
        auto handle = [&](const MsgSlot& s) {
            uint16_t locate = static_cast<uint16_t>((s.data[1] << 8) | s.data[2]);
            if (LOB_LIKELY(locate >= 1 && locate <= n_inst_)) {
                Book* b = books_[locate].get();
                if (LOB_LIKELY(b != nullptr))
                    itch::dispatch_checked<Units>(*b, s.data, s.len, bad_len);
            }
        };
        unsigned idle = 0;
        for (;;) {
            uint64_t n = ring.consume_batch(handle);
            if (LOB_LIKELY(n != 0)) { n_done += n; idle = 0; continue; }
            if (done_.load(std::memory_order_acquire)) {
                // done was published AFTER the final push (release/acquire),
                // so one more drain loop is guaranteed to see everything.
                while ((n = ring.consume_batch(handle)) != 0) n_done += n;
                break;
            }
            // Exponential backoff on an empty ring. A tight poll keeps the
            // ring's tail_ line in Shared state, so every producer push to
            // this shard pays a request-for-ownership - idle consumers were
            // measurably slowing the producer down. Backoff caps at ~64
            // pauses (a few hundred ns of added wake-up latency; a
            // latency-critical deployment would bound this lower).
            for (unsigned s = 1u << (idle < 6 ? idle : 6); s; --s)
                _mm_pause();
            if (idle < 6) ++idle;
        }
        counts_[wid].v = n_done;
        counts_[wid].bad_len = bad_len;
    }

    unsigned   W_;
    uint16_t   n_inst_;
    Factory    make_;
    std::vector<unsigned>                        cpus_;
    std::vector<std::unique_ptr<Ring>>           rings_;
    std::vector<std::unique_ptr<Book>>           books_;
    std::vector<PaddedU64>                       counts_;
    std::vector<std::thread>                     threads_;
    std::unique_ptr<std::latch>                  ready_;
    std::atomic<bool>                            done_{false};
    bool       started_ = false, finished_ = false;
    uint64_t   demux_bad_length_ = 0;
};

// The original engine: one LimitOrderBook per instrument, cent ticks.
using ParallelEngine = BasicParallelEngine<>;

} // namespace lob
