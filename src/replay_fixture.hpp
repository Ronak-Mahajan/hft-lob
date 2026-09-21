#pragma once
// ---------------------------------------------------------------------------
// replay_fixture.hpp - the recorded-day replay path on a small crafted file.
//
// CI cannot download NASDAQ's 4.8 GB sample day, so this test writes a
// 65-message ITCH 5.0 BinaryFILE by hand and runs the real lob_replay code
// (src/replay_run.hpp: chunked reader, pre-scan, per-symbol sizing, dispatch,
// ref::Market differential) over it. The file contains, on purpose:
//
//   * sub-penny prices: FXS trades near $0.50 on a $0.0001 grid, with
//     $0.5000, $0.5001 and $0.5002 as three distinct levels; FXC (a $100
//     stock on a one-cent grid) has a resting bid at $100.0050;
//   * prices far outside any ladder: $0.0001, $199,999.99 and $429,496.7295
//     (the largest u32 price) in the $100 stock, FXH near $1,650 (above the
//     benchmark book's $1,310.72 cap) and FXB near $300,000 (above the int32
//     range);
//   * replaces (U) that cross the ladder boundary both ways: last tick to one
//     past the top, first tick to one below the base, and back from the
//     overflow onto the ladder;
//   * executions (E, C), partial cancels (X) and deletes (D) of orders in the
//     overflow and on the ladder, a FIFO queue inside one overflow level, and
//     the non-order messages a day carries (S, R, Q, I, P).
//
// It is replayed with the differential on and a book comparison after EVERY
// message, through several chunk sizes: the whole file at once, 1 byte (every
// message and every length prefix straddles a chunk boundary), 2, 37 and 64
// bytes. Each run must pass every lob_replay check (zero mismatches, zero
// drops, zero unknown ids, overflow adds as the sizing predicted); the chunk
// boundaries that fall inside a message must be exactly the ones the file
// layout implies; every run must end with the same book digest; and the final
// books must equal the levels and queues written out by hand below. As a
// negative control, the same messages through the cent-denominated benchmark
// book (LimitOrderBook, $0.01..$1310.72) must drop adds and merge the
// sub-penny levels, which is what the crafted cases are there to catch.
// ---------------------------------------------------------------------------
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lob/book.hpp"
#include "lob/itch.hpp"

#include "replay_day.hpp"
#include "replay_run.hpp"

namespace fixture {

using namespace lob;

// ============================================================================
// The file
// ============================================================================
class Writer {
public:
    std::vector<uint8_t> bytes;
    std::vector<std::pair<size_t, size_t>> spans;   // [start, end) of each framed message
    uint64_t by_type[256] = {};

    void at(uint64_t ns) { ts_ = ns; }

    void system(char code) { head('S', 0, 12); p8(code); done(); }
    void directory(uint16_t loc, const char* stock) {
        head('R', loc, 39);
        str(stock, 8);
        str("Q N", 2);                   // market category, financial status
        p32(100);                        // round lot size
        str("NCZ PN 1N", 9);             // round lots only .. ETP flag
        p32(0);                          // ETP leverage factor
        p8('N');                         // inverse indicator
        done();
    }
    void add(uint16_t loc, const char* stock, uint64_t ref, char side, uint32_t shares, uint32_t price,
             const char* mpid = nullptr) {
        head(mpid ? 'F' : 'A', loc, mpid ? 40 : 36);
        p64(ref); p8(side); p32(shares); str(stock, 8); p32(price);
        if (mpid) str(mpid, 4);
        done();
    }
    void exec(uint16_t loc, uint64_t ref, uint32_t shares, uint64_t match) {
        head('E', loc, 31); p64(ref); p32(shares); p64(match); done();
    }
    void exec_price(uint16_t loc, uint64_t ref, uint32_t shares, uint64_t match, uint32_t price) {
        head('C', loc, 36); p64(ref); p32(shares); p64(match); p8('Y'); p32(price); done();
    }
    void cancel(uint16_t loc, uint64_t ref, uint32_t shares) {
        head('X', loc, 23); p64(ref); p32(shares); done();
    }
    void del(uint16_t loc, uint64_t ref) { head('D', loc, 19); p64(ref); done(); }
    void replace(uint16_t loc, uint64_t old_ref, uint64_t new_ref, uint32_t shares, uint32_t price) {
        head('U', loc, 35); p64(old_ref); p64(new_ref); p32(shares); p32(price); done();
    }
    void cross(uint16_t loc, const char* stock, uint64_t shares, uint32_t price, uint64_t match, char type) {
        head('Q', loc, 40); p64(shares); str(stock, 8); p32(price); p64(match); p8(type); done();
    }
    void noii(uint16_t loc, const char* stock, uint32_t price) {
        head('I', loc, 50);
        p64(1000); p64(200); p8('B'); str(stock, 8);
        p32(price); p32(price); p32(price); p8('O'); p8(' ');
        done();
    }
    void trade(uint16_t loc, const char* stock, uint32_t shares, uint32_t price, uint64_t match) {
        head('P', loc, 44); p64(0); p8('B'); p32(shares); str(stock, 8); p32(price); p64(match); done();
    }

private:
    void p8(char v) { bytes.push_back(static_cast<uint8_t>(v)); }
    void p8(uint8_t v) { bytes.push_back(v); }
    void p16(uint16_t v) { p8(static_cast<uint8_t>(v >> 8)); p8(static_cast<uint8_t>(v)); }
    void p32(uint32_t v) { p16(static_cast<uint16_t>(v >> 16)); p16(static_cast<uint16_t>(v)); }
    void p48(uint64_t v) { p16(static_cast<uint16_t>(v >> 32)); p32(static_cast<uint32_t>(v)); }
    void p64(uint64_t v) { p32(static_cast<uint32_t>(v >> 32)); p32(static_cast<uint32_t>(v)); }
    void str(const char* s, size_t n) {
        size_t i = 0;
        for (; i < n && s[i]; ++i) p8(s[i]);
        for (; i < n; ++i) p8(' ');
    }
    void head(char type, uint16_t loc, uint16_t len) {
        start_ = bytes.size();
        len_ = len;
        p16(len);
        p8(type);
        p16(loc);
        p16(0);                          // tracking number
        p48(ts_);
        ts_ += 1'000'000;                // 1 ms apart
        ++by_type[static_cast<uint8_t>(type)];
    }
    void done() {
        LOB_ASSERT(bytes.size() - start_ == size_t{2} + len_, "fixture message length");
        spans.push_back({start_, bytes.size()});
    }

    uint64_t ts_ = 0;
    size_t start_ = 0;
    uint16_t len_ = 0;
};

constexpr uint64_t hms(uint64_t h, uint64_t m, uint64_t s) {
    return ((h * 60 + m) * 60 + s) * 1'000'000'000ull;
}

// Stock locates and symbols. FXN has a directory entry and no orders.
constexpr uint16_t kFXC = 1, kFXS = 2, kFXH = 3, kFXN = 4, kFXB = 65535;

inline Writer build() {
    Writer w;
    w.at(hms(3, 5, 0));
    w.system('O');
    w.directory(kFXC, "FXC");
    w.directory(kFXS, "FXS");
    w.directory(kFXH, "FXH");
    w.directory(kFXN, "FXN");
    w.directory(kFXB, "FXB");
    w.at(hms(4, 0, 0));
    w.system('S');
    w.at(hms(9, 30, 0));
    w.system('Q');

    // Phase 1, from 09:30:00.001. Prices are ITCH wire units ($0.0001).
    // FXC, a $100 stock: most adds on $100.00 .. $100.63, so the pre-scan puts
    // its 64-tick one-cent ladder there.
    w.add(kFXC, "FXC",  1, 'B', 100, 1'000'000);          // $100.00  first ladder tick
    w.add(kFXC, "FXC",  2, 'B', 200, 1'000'000);
    w.add(kFXC, "FXC",  3, 'B',  80, 1'000'000);
    w.add(kFXC, "FXC",  4, 'B', 300, 1'001'000);          // $100.10
    w.add(kFXC, "FXC",  5, 'S', 400, 1'006'300);          // $100.63  last ladder tick
    w.add(kFXC, "FXC",  6, 'S', 500, 1'006'300);
    w.add(kFXC, "FXC",  7, 'S',  70, 1'006'300);
    w.add(kFXC, "FXC",  8, 'S', 150, 1'005'000);          // $100.50
    w.add(kFXC, "FXC",  9, 'B',  10,   999'900);          // $99.99   one tick below: overflow
    w.add(kFXC, "FXC", 10, 'S',  20, 1'006'400, "FIXT");  // $100.64  one past the top: overflow (F)
    w.add(kFXC, "FXC", 11, 'B',  30, 1'000'050);          // $100.0050 sub-penny, between grid points
    w.add(kFXC, "FXC", 12, 'S',  40, 1'999'999'900u);     // $199,999.99 far above
    w.add(kFXC, "FXC", 13, 'B',  50, 1);                  // $0.0001  far below
    w.add(kFXC, "FXC", 14, 'S',  60, 4'294'967'295u);     // $429,496.7295, the largest u32 price
    // FXS, a sub-dollar stock: $0.0001 grid, ladder $0.5000 .. $0.5063.
    w.add(kFXS, "FXS", 15, 'B', 100, 5'000);              // $0.5000
    w.add(kFXS, "FXS", 16, 'B', 100, 5'000);
    w.add(kFXS, "FXS", 17, 'B', 100, 5'000);
    w.add(kFXS, "FXS", 18, 'B', 200, 5'001);              // $0.5001  its own level
    w.add(kFXS, "FXS", 19, 'B', 300, 5'002);              // $0.5002  its own level
    w.add(kFXS, "FXS", 20, 'B', 300, 5'002);
    w.add(kFXS, "FXS", 21, 'S', 400, 5'030);
    w.add(kFXS, "FXS", 22, 'S', 500, 5'063);              // last ladder tick
    w.add(kFXS, "FXS", 23, 'S', 500, 5'063);
    w.add(kFXS, "FXS", 24, 'B',  50, 4'999);              // one below the ladder: overflow
    // FXH near $1,650 and FXB near $300,000: above the benchmark book's cent
    // band, and FXB above the int32 range.
    w.add(kFXH, "FXH", 25, 'B', 100, 16'500'000);         // $1,650.00
    w.add(kFXH, "FXH", 26, 'B', 200, 16'500'000);
    w.add(kFXH, "FXH", 27, 'B', 300, 16'500'500);         // $1,650.05
    w.add(kFXB, "FXB", 28, 'S',  10, 3'000'000'000u);     // $300,000.00
    w.add(kFXB, "FXB", 29, 'S',  30, 3'000'000'000u);
    w.add(kFXB, "FXB", 30, 'S',  20, 3'000'005'000u);     // $300,000.50
    w.add(kFXB, "FXB", 31, 'B',   5, 2'999'990'000u);     // $299,999.00 below the ladder
    w.add(kFXB, "FXB", 32, 'S',   7, 3'000'010'000u);     // $300,001.00 above the ladder
    // Messages that carry no book state.
    w.cross(kFXC, "FXC", 1'000, 1'000'500, 1, 'O');      // opening cross $100.05
    w.noii(kFXC, "FXC", 1'000'500);
    w.trade(kFXS, "FXS", 100, 5'002, 2);
    // Replaces from the ladder into the overflow.
    w.replace(kFXC,  5, 33, 400, 1'006'400);              // $100.63 -> $100.64, behind order 10
    w.replace(kFXC,  1, 34, 100,   999'900);              // $100.00 -> $99.99, behind order 9
    w.replace(kFXS, 22, 35, 500, 5'064);                  // $0.5063 -> $0.5064
    w.replace(kFXH, 27, 36, 300, 16'507'000);             // $1,650.05 -> $1,650.70, the best bid

    // Phase 2, from 09:45:00.000 (the checkpoint at 09:45:00 prints phase 1's books).
    w.at(hms(9, 45, 0));
    w.replace(kFXC, 10, 37, 20, 1'006'200);               // overflow -> ladder, $100.62
    w.replace(kFXC,  9, 38, 10, 1'000'100);               // overflow -> ladder, $100.01
    w.exec(kFXC, 12, 15, 3);                              // partial execution, far above the ladder
    w.cancel(kFXC, 11, 10);                               // partial cancel at the sub-penny price
    w.exec_price(kFXC, 33, 400, 4, 1'006'400);            // full execution empties $100.64
    w.del(kFXC, 13);                                      // delete at $0.0001
    w.exec(kFXC, 2, 50, 5);                               // partial execution on the ladder
    w.del(kFXC, 6);
    w.exec_price(kFXC, 8, 150, 6, 1'005'000);             // the best ask trades away
    w.cancel(kFXS, 19, 100);
    w.del(kFXS, 15);
    w.exec(kFXH, 36, 100, 7);                             // partial execution of the best bid
    w.del(kFXB, 30);
    w.exec(kFXB, 28, 4, 8);
    w.at(hms(16, 0, 0));
    w.cross(kFXC, "FXC", 500, 1'001'000, 9, 'C');        // closing cross $100.10
    w.system('M');
    w.at(hms(20, 0, 0));
    w.system('E');
    w.at(hms(20, 5, 0));
    w.system('C');
    return w;
}

// ============================================================================
// The books the file must leave, written out by hand
// ============================================================================
struct QOrder { uint64_t ref; uint32_t shares; };
struct XLevel { Price price; std::vector<QOrder> queue; };
struct XBook {
    uint16_t locate;
    const char* name;
    Price base, tick;
    uint32_t band;
    uint64_t ladder_adds, overflow_adds, overflow_levels;
    std::vector<XLevel> bids, asks;
};

inline std::vector<XBook> expected() {
    return {
        {kFXC, "FXC", 1'000'000, 100, 64, 10, 8, 4,
         {{1'001'000, {{4, 300}}},
          {1'000'100, {{38, 10}}},
          {1'000'050, {{11, 20}}},
          {1'000'000, {{2, 150}, {3, 80}}},
          {  999'900, {{34, 100}}}},
         {{1'006'200, {{37, 20}}},
          {1'006'300, {{7, 70}}},
          {1'999'999'900u, {{12, 25}}},
          {4'294'967'295u, {{14, 60}}}}},
        {kFXS, "FXS", 5'000, 1, 64, 9, 2, 2,
         {{5'002, {{19, 200}, {20, 300}}},
          {5'001, {{18, 200}}},
          {5'000, {{16, 100}, {17, 100}}},
          {4'999, {{24, 50}}}},
         {{5'030, {{21, 400}}},
          {5'063, {{23, 500}}},
          {5'064, {{35, 500}}}}},
        {kFXH, "FXH", 16'500'000, 100, 64, 3, 1, 1,
         {{16'507'000, {{36, 200}}},
          {16'500'000, {{25, 100}, {26, 200}}}},
         {}},
        {kFXB, "FXB", 3'000'000'000u, 100, 64, 3, 2, 2,
         {{2'999'990'000u, {{31, 5}}}},
         {{3'000'000'000u, {{28, 6}, {29, 30}}},
          {3'000'010'000u, {{32, 7}}}}},
    };
}

// First difference between one side of a book and its expected levels, or "".
inline std::string diff_side(const ExactOrderBook& b, Side side, const std::vector<XLevel>& want,
                             uint64_t& levels, uint64_t& orders) {
    std::vector<XLevel> got;
    std::string err;
    b.for_each_level(side, [&](Price p, const PriceLevel& L) {
        XLevel x{p, {}};
        uint64_t sum = 0;
        for (uint32_t i = L.head; i != NIL; i = b.order_at(i).next) {
            x.queue.push_back({b.order_at(i).id, b.order_at(i).qty});
            sum += b.order_at(i).qty;
        }
        if (err.empty() && (sum != L.total_qty || x.queue.size() != L.count))
            err = "level " + std::to_string(p) + ": aggregate disagrees with its queue";
        got.push_back(std::move(x));
    });
    if (!err.empty()) return err;
    const char* s = side == Side::Bid ? "bid" : "ask";
    if (got.size() != want.size())
        return std::string(s) + " levels: " + std::to_string(got.size()) + ", expected " +
               std::to_string(want.size());
    for (size_t i = 0; i < want.size(); ++i) {
        ++levels;
        if (got[i].price != want[i].price)
            return std::string(s) + " level " + std::to_string(i) + ": price " +
                   std::to_string(got[i].price) + ", expected " + std::to_string(want[i].price);
        if (got[i].queue.size() != want[i].queue.size())
            return std::string(s) + " " + std::to_string(want[i].price) + ": " +
                   std::to_string(got[i].queue.size()) + " orders, expected " +
                   std::to_string(want[i].queue.size());
        for (size_t k = 0; k < want[i].queue.size(); ++k) {
            ++orders;
            if (got[i].queue[k].ref != want[i].queue[k].ref ||
                got[i].queue[k].shares != want[i].queue[k].shares)
                return std::string(s) + " " + std::to_string(want[i].price) + " position " +
                       std::to_string(k) + ": order " + std::to_string(got[i].queue[k].ref) + " x " +
                       std::to_string(got[i].queue[k].shares) + ", expected " +
                       std::to_string(want[i].queue[k].ref) + " x " +
                       std::to_string(want[i].queue[k].shares);
        }
    }
    return {};
}

// Chunk boundaries (multiples of `chunk`) that fall strictly inside a
// message, and those that fall between the two bytes of a length prefix.
inline std::pair<uint64_t, uint64_t> layout_cuts(const Writer& w, size_t chunk) {
    uint64_t cuts = 0, prefix = 0;
    for (const auto& [s, e] : w.spans) {
        cuts += (e - 1) / chunk - s / chunk;
        if ((s + 1) % chunk == 0) ++prefix;
    }
    return {cuts, prefix};
}

// ============================================================================
// The test
// ============================================================================
// Returns 0 when everything holds. verbose: print the first run's full
// lob_replay output (the books at 09:45:00 and at the end of the file).
inline int run(bool verbose, const char* file_name, const std::string& command) {
    int failures = 0;
    auto fail = [&](const std::string& what) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++failures;
    };
    const Writer w = build();
    const std::string path = file_name;              // in the working directory, removed at the end
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f || std::fwrite(w.bytes.data(), 1, w.bytes.size(), f) != w.bytes.size()) {
            std::printf("  FAIL: cannot write %s\n", path.c_str());
            if (f) std::fclose(f);
            return 1;
        }
        std::fclose(f);
    }
    uint64_t adds = 0;
    for (char t : {'A', 'F', 'U'}) adds += w.by_type[static_cast<uint8_t>(t)];
    std::printf("  crafted file: %zu bytes, %zu messages (A %" PRIu64 ", F %" PRIu64 ", U %" PRIu64
                ", E %" PRIu64 ", C %" PRIu64 ", X %" PRIu64 ", D %" PRIu64 ", and S, R, Q, I, P), "
                "%" PRIu64 " adds\n",
                w.bytes.size(), w.spans.size(), w.by_type['A'], w.by_type['F'], w.by_type['U'],
                w.by_type['E'], w.by_type['C'], w.by_type['X'], w.by_type['D'], adds);

    const std::vector<XBook> want = expected();
    uint64_t want_overflow = 0, want_resting = 0;
    for (const XBook& x : want) {
        want_overflow += x.overflow_adds;
        for (const auto* side : {&x.bids, &x.asks})
            for (const XLevel& L : *side) want_resting += L.queue.size();
    }

    struct Run { size_t chunk; bool differential; };
    const std::vector<Run> runs = {{w.bytes.size(), true}, {1, true}, {2, true},
                                   {37, true}, {64, true}, {37, false}};
    uint64_t digest0 = 0;
    for (size_t r = 0; r < runs.size(); ++r) {
        day::ReplayOptions o;
        o.path = path;
        o.differential = runs[r].differential;
        o.checkpoint = 1;                            // compare every book after every message
        o.at = {"09:45:00"};
        o.symbols = {"FXC", "FXS", "FXH", "FXB", "NOSUCH"};
        o.chunk = runs[r].chunk;
        o.ladder_mb = 0;                             // every ladder at its minimum, 64 ticks
        o.cpu = -1;
        o.print_chunks = false;
        o.print_checkpoints = false;
        o.quiet = !(verbose && r == 0);
        o.command = command;
        if (!o.quiet) std::printf("\n");
        day::ReplayReport rep;
        const int rc = day::replay(o, &rep);
        if (!o.quiet) std::printf("\n");
        const auto [cuts, prefix] = layout_cuts(w, runs[r].chunk);
        const std::string tag = "chunk " + std::to_string(runs[r].chunk) +
                                (runs[r].differential ? "" : " (no differential)");
        if (rc != 0) fail(tag + ": lob_replay's checks failed");
        if (rep.messages != w.spans.size()) fail(tag + ": message count");
        for (int t = 0; t < 256; ++t)
            if (rep.by_type[t] != w.by_type[t]) { fail(tag + ": pre-scan count for type " + std::string(1, static_cast<char>(t))); break; }
        if (rep.cuts != cuts || rep.prefix_cuts != prefix) fail(tag + ": chunk boundaries inside messages differ from the file layout");
        if (runs[r].chunk < w.bytes.size() && (rep.cuts == 0 || (runs[r].chunk == 1 && rep.prefix_cuts == 0)))
            fail(tag + ": no message straddled a chunk boundary");
        if (runs[r].differential && (rep.checkpoints != w.spans.size() || rep.mismatches != 0))
            fail(tag + ": not every message was followed by a clean comparison");
        if (rep.dropped != 0 || rep.unknown != 0 || rep.bad_length != 0) fail(tag + ": drops, unknown ids or bad lengths");
        if (rep.adds != adds || rep.overflow_adds != want_overflow || rep.predicted_overflow != want_overflow)
            fail(tag + ": overflow adds");
        if (rep.resting != want_resting) fail(tag + ": resting orders at end of file");
        if (r == 0) digest0 = rep.final_digest;
        else if (rep.final_digest != digest0) fail(tag + ": final books differ from the whole-file run");

        // The final books, level by level and order by order.
        uint64_t levels = 0, orders = 0;
        for (const XBook& x : want) {
            const day::Sizing& q = rep.sizing[x.locate];
            const ExactOrderBook* b = rep.books[x.locate].get();
            if (!q.has_book || !b) { fail(tag + ": no book for " + x.name); continue; }
            if (q.base != x.base || q.tick != x.tick || q.band != x.band || q.ladder_adds != x.ladder_adds)
                fail(tag + ": " + x.name + " ladder " + std::to_string(q.base) + " / " +
                     std::to_string(q.tick) + " / " + std::to_string(q.band) +
                     ", expected " + std::to_string(x.base) + " / " + std::to_string(x.tick) +
                     " / " + std::to_string(x.band));
            if (b->overflow_adds() != x.overflow_adds || b->overflow_levels() != x.overflow_levels)
                fail(tag + ": " + x.name + " overflow adds or levels");
            for (Side side : {Side::Bid, Side::Ask}) {
                std::string d = diff_side(*b, side, side == Side::Bid ? x.bids : x.asks, levels, orders);
                if (!d.empty()) fail(tag + ": " + x.name + " " + d);
            }
        }
        if (rep.books[kFXN]) fail(tag + ": a book for FXN, which has no orders");
        const std::string cmp = runs[r].differential
            ? "compared with the reference after every message, " + std::to_string(rep.mismatches) + " mismatches"
            : "no reference";
        std::printf("  chunk %4zu bytes%s: %" PRIu64 " messages | %" PRIu64
                    " chunk boundaries inside a message (%" PRIu64 " inside a length prefix) | "
                    "%s | overflow adds %" PRIu64 " of %" PRIu64 " | end-of-file books as written "
                    "out (%" PRIu64 " levels, %" PRIu64 " orders) | digest %s\n",
                    runs[r].chunk, runs[r].differential ? "" : " (no differential)", rep.messages,
                    rep.cuts, rep.prefix_cuts, cmp.c_str(), rep.overflow_adds, rep.adds, levels,
                    orders, day::hex64(rep.final_digest).c_str());
    }

    // Negative control: the benchmark book, in whole cents with one band for
    // every symbol ($0.01 .. $1310.72), cannot hold this file.
    {
        std::vector<std::unique_ptr<LimitOrderBook>> lb(65536);
        for (const XBook& x : want) lb[x.locate] = std::make_unique<LimitOrderBook>(1, 1u << 17, 1u << 10, 12);
        uint64_t bad = 0, dropped = 0;
        for (const auto& [s, e] : w.spans) {
            const uint8_t* m = w.bytes.data() + s + 2;
            const uint16_t loc = static_cast<uint16_t>((m[1] << 8) | m[2]);
            if (lb[loc]) itch::dispatch_checked(*lb[loc], m, static_cast<uint16_t>(e - s - 2), bad);
        }
        for (const XBook& x : want) dropped += lb[x.locate]->dropped_out_of_band();
        size_t bid_levels = 0, ask_levels = 0;
        for (Price p = 1; p < 200; ++p) {
            if (lb[kFXS]->level_at(Side::Bid, p).second) ++bid_levels;
            if (lb[kFXS]->level_at(Side::Ask, p).second) ++ask_levels;
        }
        std::printf("  negative control: the cent-denominated benchmark book ($0.01..$1310.72) drops "
                    "%" PRIu64 " of the %" PRIu64 " adds and holds FXS in %zu bid / %zu ask levels "
                    "(exact: 4 / 3)\n", dropped, adds, bid_levels, ask_levels);
        if (dropped != 11 || bid_levels != 2 || ask_levels != 1 || bad != 0)
            fail("negative control: the crafted cases no longer trip the cent-denominated book");
    }
    std::remove(path.c_str());
    std::printf("  %s\n", failures == 0 ? "crafted-file replay: PASS" : "crafted-file replay: FAIL");
    return failures == 0 ? 0 : 1;
}

} // namespace fixture
