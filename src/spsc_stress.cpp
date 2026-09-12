// ---------------------------------------------------------------------------
// spsc_stress.cpp - Phase 5: SPSC ring stress / ThreadSanitizer target.
//
// One producer thread pushes N sequence-numbered messages of varying length
// through a deliberately SMALL SpscRing (1024 slots) so the ring wraps
// thousands of times and both the "looks full" and "looks empty" slow paths
// (the only places the shared indices are loaded) fire constantly. One
// consumer drains it in batches and checks, for every message:
//   * the sequence number is exactly the one expected (no loss, no
//     duplication, no reordering),
//   * the slot length matches what the producer wrote for that sequence,
//   * every payload byte carries the sequence's fill pattern (no torn or
//     stale slot contents).
//
// Build it with -fsanitize=thread: a missing acquire/release pair on tail_ /
// head_, or a slot reused before the consumer is done with it, shows up as a
// TSan report even when the sequence check happens to pass.
//
// Usage:  spsc_stress [N]   (default N = 2,000,000)
// Exit status 0 on a clean run, 1 on any check failure.
// ---------------------------------------------------------------------------
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "lob/spsc.hpp"

using namespace lob;

namespace {

void cpu_relax() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

// Message layout: [u64 sequence][fill bytes]. Length varies 8..62 with the
// sequence so consecutive slots carry different lengths.
constexpr uint16_t kHeader = 8;
constexpr uint16_t kMaxLen = static_cast<uint16_t>(sizeof(MsgSlot::data));   // 62

uint16_t len_for(uint64_t seq) {
    return static_cast<uint16_t>(kHeader + seq % (kMaxLen - kHeader + 1));
}
uint8_t fill_for(uint64_t seq) {
    return static_cast<uint8_t>((seq * 0x9E3779B97F4A7C15ull) >> 56);
}

} // namespace

int main(int argc, char** argv) {
    uint64_t n = 2'000'000;
    if (argc > 1) {
        char* endp = nullptr;
        unsigned long long v = std::strtoull(argv[1], &endp, 10);
        if (endp == argv[1] || *endp != '\0' || v == 0) {
            std::printf("usage: %s [N]\n", argv[0]);
            return 2;
        }
        n = v;
    }

    // 1024 slots: small on purpose (see header comment). The engine uses 2^14.
    SpscRing<10> ring;

    uint64_t bad_seq = 0, bad_len = 0, bad_fill = 0;
    uint64_t consumed = 0, batches = 0, max_batch = 0, empty_polls = 0;

    std::thread consumer([&] {
        uint64_t expect = 0;
        auto check = [&](const MsgSlot& s) {
            uint64_t seq;
            std::memcpy(&seq, s.data, sizeof seq);
            if (seq != expect) ++bad_seq;
            uint16_t want_len = len_for(seq);
            if (s.len != want_len) {
                ++bad_len;
            } else {
                uint8_t f = fill_for(seq);
                for (uint16_t i = kHeader; i < s.len; ++i)
                    if (s.data[i] != f) { ++bad_fill; break; }
            }
            ++expect;
        };
        while (expect < n) {
            uint64_t got = ring.consume_batch(check);
            if (got == 0) { ++empty_polls; cpu_relax(); continue; }
            consumed += got;
            ++batches;
            if (got > max_batch) max_batch = got;
        }
    });

    auto t0 = std::chrono::steady_clock::now();
    uint64_t full_stalls = 0;
    uint8_t msg[kMaxLen];
    for (uint64_t seq = 0; seq < n; ++seq) {
        uint16_t len = len_for(seq);
        std::memcpy(msg, &seq, sizeof seq);
        std::memset(msg + kHeader, fill_for(seq), len - kHeader);
        while (!ring.try_push(msg, len)) { ++full_stalls; cpu_relax(); }
    }
    consumer.join();
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    bool ok = consumed == n && bad_seq == 0 && bad_len == 0 && bad_fill == 0 &&
              max_batch <= SpscRing<10>::kMaxBatch;
    std::printf("SPSC stress: %llu messages, 1024-slot ring, 1 producer + 1 consumer\n"
                "  consumed %llu | batches %llu (max %llu, cap %llu) | empty polls %llu | "
                "full stalls %llu\n"
                "  sequence errors %llu | length errors %llu | payload errors %llu\n"
                "  %.3f s (%.1f M msgs/s; a correctness run, not a benchmark)\n"
                "  %s\n",
                (unsigned long long)n, (unsigned long long)consumed,
                (unsigned long long)batches, (unsigned long long)max_batch,
                (unsigned long long)SpscRing<10>::kMaxBatch,
                (unsigned long long)empty_polls, (unsigned long long)full_stalls,
                (unsigned long long)bad_seq, (unsigned long long)bad_len,
                (unsigned long long)bad_fill,
                secs, secs > 0 ? static_cast<double>(n) / secs / 1e6 : 0.0,
                ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
