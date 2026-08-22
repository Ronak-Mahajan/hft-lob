#pragma once
// ---------------------------------------------------------------------------
// lob/spsc.hpp - Phase 5: wait-free single-producer/single-consumer ring.
//
// Why SPSC and not a Disruptor-style MPMC or a mutex queue: the demux thread
// is the only producer for a given shard and the shard worker is the only
// consumer. With exactly one writer per index, correctness needs nothing but
// acquire/release *loads and stores*: no CAS, no RMW, no lock. Both sides
// are wait-free (the producer's "full" case spins in the caller, by policy).
//
// False-sharing architecture (the whole point of this file):
//
//   [ line 0 ]  ptail_, phead_cache_      producer-PRIVATE. Never read by
//                                         the consumer; stays in M state in
//                                         the producer core's L1 forever.
//   [ line 1 ]  tail_ (atomic)            producer→consumer publication.
//   [ line 2 ]  chead_, ctail_cache_      consumer-PRIVATE.
//   [ line 3 ]  head_ (atomic)            consumer→producer publication.
//   [ ...    ]  slots                     64B each: one message per line.
//
// Each region is isolated on a 128-byte boundary, not 64: Intel's L2
// spatial prefetcher pulls cache lines in *pairs*, so two hot variables in
// adjacent 64B lines still ping-pong between cores ("destructive
// interference", the same reason std::hardware_destructive_interference_size
// is 128 on some x86 targets).
//
// Index-caching trick (the big coherence win): the producer only needs the
// consumer's head to detect "full". Instead of loading the shared atomic
// every push, which would drag head_'s line across the interconnect at
// message rate, it works from a private cached copy and refreshes it only
// when the ring *appears* full. Symmetrically for the consumer and tail_.
// Steady state: each side touches the other's line ~once per ring lap, not
// once per message.
// ---------------------------------------------------------------------------
#include <atomic>
#include <cstring>
#include <memory>

#include "common.hpp"

namespace lob {

// Isolation quantum: 2 cache lines (adjacent-line prefetcher, see above).
inline constexpr size_t kIsolate = 128;

// One ITCH message per slot, one slot per cache line. Largest message we
// carry ('F', 40B) fits with room; fixed size keeps consumer access purely
// sequential - the hardware prefetcher streams the ring like an array.
struct alignas(64) MsgSlot {
    uint16_t len;
    uint8_t  data[62];
};
static_assert(sizeof(MsgSlot) == 64);

template <unsigned LOG2>
class SpscRing {
    static constexpr uint64_t N = uint64_t{1} << LOG2;
    static constexpr uint64_t MASK = N - 1;

public:
    SpscRing() : buf_(new MsgSlot[N]) {}          // startup-only allocation

    // ---- producer side ----------------------------------------------------
    LOB_FORCE_INLINE bool try_push(const uint8_t* msg, uint16_t len) {
        uint64_t t = ptail_;
        if (LOB_UNLIKELY(t - phead_cache_ >= N)) {         // looks full:
            phead_cache_ = head_.load(std::memory_order_acquire);  // refresh
            if (t - phead_cache_ >= N) return false;       // genuinely full
        }
        MsgSlot& s = buf_[t & MASK];
        s.len = len;
        std::memcpy(s.data, msg, len);                     // single copy
        tail_.store(t + 1, std::memory_order_release);     // publish
        ptail_ = t + 1;
        return true;
    }

    // ---- consumer side ----------------------------------------------------
    // Drain visible slots, invoking f(const MsgSlot&) on each. One head_
    // publication per BATCH, not per message - coherence traffic amortizes
    // over the burst. Batches are capped: publishing head at most every
    // kMaxBatch messages keeps the producer's view of free space fresh.
    // (Uncapped draining of a full ring stalls the producer for the entire
    // batch - head-of-line blocking that serializes the whole pipeline when
    // consumers are the bottleneck.) Returns messages consumed (0 = empty).
    static constexpr uint64_t kMaxBatch = 256;

    template <typename F>
    LOB_FORCE_INLINE uint64_t consume_batch(F&& f) {
        uint64_t h = chead_;
        uint64_t t = ctail_cache_;
        if (h == t) {                                      // looks empty:
            t = ctail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == t) return 0;                          // genuinely empty
        }
        uint64_t n = t - h;
        if (n > kMaxBatch) { n = kMaxBatch; t = h + n; }   // cap; cache keeps
        do {                                               // the true tail
            f(buf_[h & MASK]);
        } while (++h != t);
        head_.store(h, std::memory_order_release);         // publish batch
        chead_ = h;
        return n;
    }

private:
    alignas(kIsolate) uint64_t ptail_ = 0;        // producer-private
    uint64_t phead_cache_ = 0;                    //   (same private line)
    alignas(kIsolate) std::atomic<uint64_t> tail_{0};   // producer publishes
    alignas(kIsolate) uint64_t chead_ = 0;        // consumer-private
    uint64_t ctail_cache_ = 0;                    //   (same private line)
    alignas(kIsolate) std::atomic<uint64_t> head_{0};   // consumer publishes
    alignas(kIsolate) std::unique_ptr<MsgSlot[]> buf_;
};

} // namespace lob
