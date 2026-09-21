// ---------------------------------------------------------------------------
// replay_main.cpp - replay a recorded NASDAQ TotalView-ITCH 5.0 day.
//
// Input is NASDAQ's historical BinaryFILE format, decompressed: every message
// is a 2-byte big-endian length followed by the message. The file is streamed
// in large chunks (256 MB by default, never loaded whole), and a message that
// straddles two chunks is carried over to the next one.
//
// Pass 1, untimed (pre-scan): counts every message by type, validates every
// length against the ITCH 5.0 table, reads the Stock Directory ('R') for
// symbol names, tracks resting orders to find each symbol's peak, records the
// opening and closing cross prices ('Q'), and keeps every add price. From
// those it sizes one ExactOrderBook per stock_locate (see size_books() in
// src/replay_day.hpp, which lob_perf shares).
//
// Pass 2: the same bytes through itch::dispatch_checked<WireUnits>, the
// dispatch the benchmarks use, into the book of each message's stock_locate.
// Only the dispatch loop over a chunk already in memory is timed; reading the
// file never is. With --differential, the books and the ref::Market model
// (src/ref_market.hpp) take the file one message at a time. After every order
// message, what it touched is compared (the book's resting-order count and
// BBO, the orders it names, and the price levels it changed; diff_touch() in
// src/book_diff.hpp), and at every checkpoint (every --checkpoint messages, at
// each --at time, and at the end) every book is compared with the model in
// full: both sides level by level, the queue order of every level order by
// order, the BBO and the resting-order count (diff_book()). Timings from a
// differential run are not measurements: the model's work between messages
// evicts the books from cache.
// After every chunk a digest of the full state of every book is printed
// (books_digest() in src/replay_day.hpp); lob_perf prints the same digests,
// so its runs can be checked against this one.
//
// The run itself is replay() in src/replay_run.hpp; this file is the command
// line and the two built-in tests.
//
// Usage:
//   lob_replay FILE [--differential] [--checkpoint N] [--at HH:MM:SS[,...]]
//              [--symbols A,B,...] [--depth N] [--chunk-mb N] [--ladder-mb N]
//              [--cpu N]
//   lob_replay --fixture      crafted 65-message file (src/replay_fixture.hpp):
//                             sub-penny prices, prices far outside every
//                             ladder, replaces across the ladder boundary,
//                             chunks from 1 byte up, differential after every
//                             message, and the expected books written out
//   lob_replay --selftest     generated 40-symbol day through 4 KB chunks,
//                             differential on
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "replay_day.hpp"
#include "replay_fixture.hpp"
#include "replay_run.hpp"
#include "wire_gen.hpp"

using namespace day;

namespace {

// A generated day (src/wire_gen.hpp) written to disk and replayed with the
// differential through deliberately small, odd-sized chunks, so that
// messages and even length prefixes straddle chunk boundaries thousands of
// times: every check of a real run, on data a CI runner can carry.
int selftest() {
    std::vector<WireSymbol> syms;
    const uint32_t bases[] = {1'000'000, 5'000, 250'000, 16'000'000, 1'999'000'000u,
                              3'000'000'000u, 9'000, 150'000, 42'000'000, 700};
    for (uint16_t i = 0; i < 40; ++i) {
        const uint32_t base = bases[i % 10] + 100 * i;
        const uint32_t tick = base < 10'000 ? 1 : 100;
        syms.push_back({static_cast<uint16_t>(1 + i * 1601), "SYM" + std::to_string(i), base, tick,
                        static_cast<uint32_t>(64u << (i % 5))});
    }
    WireGen gen(0x5E1F7E57ull, syms, 30'000);
    gen.generate(1'500'000);
    const char* path = "lob_replay_selftest.itch";
    FILE* f = std::fopen(path, "wb");
    if (!f || std::fwrite(gen.bytes.data(), 1, gen.bytes.size(), f) != gen.bytes.size()) {
        std::printf("selftest: cannot write %s\n", path);
        return 2;
    }
    std::fclose(f);
    ReplayOptions o;
    o.path = path;
    o.differential = true;
    o.checkpoint = 100'000;
    o.at = {"09:45:00"};
    o.symbols = {"SYM0", "SYM1", "SYM5", "NOSUCH"};
    o.chunk = 4096 + 7;
    o.ladder_mb = 1;                     // small budget: many adds must take the overflow
    o.print_chunks = false;              // thousands of 4 KB chunks: the chain is printed
    o.command = "lob_replay --selftest";
    int rc = replay(o);
    std::remove(path);
    return rc;
}

int fixture_test() {
    std::printf("=== lob_replay --fixture: the replay path on a crafted ITCH 5.0 file ===\n");
    const int rc = fixture::run(/*verbose=*/true, "lob_replay_fixture.itch", "lob_replay --fixture");
    std::printf("\n%s\n", rc == 0 ? "RESULT: PASS" : "RESULT: FAIL");
    return rc;
}

} // namespace

int main(int argc, char** argv) {
    ReplayOptions o;
    bool selftest_mode = false, fixture_mode = false;
    for (int i = 0; i < argc; ++i) {
        std::string a = i == 0 ? std::string("lob_replay") : std::string(argv[i]);
        o.command += (i ? " " : "") + a;
    }
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) { std::printf("%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--selftest") selftest_mode = true;
        else if (a == "--fixture") fixture_mode = true;
        else if (a == "--differential") o.differential = true;
        else if (a == "--checkpoint") o.checkpoint = std::max<uint64_t>(1, std::strtoull(val().c_str(), nullptr, 10));
        else if (a == "--at") o.at = split(val(), ',');
        else if (a == "--symbols") o.symbols = split(val(), ',');
        else if (a == "--depth") o.depth = std::max(1, std::atoi(val().c_str()));
        else if (a == "--chunk-mb") o.chunk = size_t{std::max<uint64_t>(1, std::strtoull(val().c_str(), nullptr, 10))} << 20;
        else if (a == "--ladder-mb") o.ladder_mb = std::strtoull(val().c_str(), nullptr, 10);
        else if (a == "--cpu") o.cpu = std::atoi(val().c_str());
        else if (!a.empty() && a[0] != '-' && o.path.empty()) o.path = a;
        else {
            std::printf("usage: %s FILE [--differential] [--checkpoint N] [--at HH:MM:SS,...] "
                        "[--symbols A,B,...] [--depth N] [--chunk-mb N] [--ladder-mb N] [--cpu N]\n"
                        "       %s --fixture\n"
                        "       %s --selftest\n", argv[0], argv[0], argv[0]);
            return 2;
        }
    }
    if (fixture_mode) return fixture_test();
    if (selftest_mode) return selftest();
    if (o.path.empty()) { std::printf("no input file (see --help)\n"); return 2; }
    return replay(o);
}
