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
// file never is. With --differential, the ref::Market model (src/ref_market.hpp)
// is fed the same messages after each timed segment, and at every checkpoint
// (every --checkpoint messages, at each --at time, and at the end) every book
// is compared with it in full: both sides level by level, the queue order of
// every level order by order, the BBO and the resting-order count
// (src/book_diff.hpp). Timings from a differential run are not measurements:
// the model's work between segments evicts the books from cache.
// After every chunk a digest of the full state of every book is printed
// (books_digest() in src/replay_day.hpp); lob_perf prints the same digests,
// so its runs can be checked against this one.
//
// Usage:
//   lob_replay FILE [--differential] [--checkpoint N] [--at HH:MM:SS[,...]]
//              [--symbols A,B,...] [--depth N] [--chunk-mb N] [--ladder-mb N]
//              [--cpu N]
//   lob_replay --selftest     generated multi-symbol day through 4 KB chunks,
//                             differential on (the CI target)
// ---------------------------------------------------------------------------
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lob/book.hpp"
#include "lob/engine.hpp"
#include "lob/itch.hpp"

#include "book_diff.hpp"
#include "ref_market.hpp"
#include "replay_day.hpp"
#include "wire_gen.hpp"

using namespace lob;
using namespace day;

namespace {

// ============================================================================
// Pass 2
// ============================================================================
struct Options {
    std::string path;
    bool differential = false;
    uint64_t checkpoint = 5'000'000;
    std::vector<std::string> at = {"12:00:00", "16:00:00"};
    std::vector<std::string> symbols = {"AAPL", "MSFT", "AMZN", "BRK.A", "BKNG", "WFT"};
    int depth = 5;
    size_t chunk = size_t{256} << 20;
    uint64_t ladder_mb = 1024;
    int cpu = 2;
    bool selftest = false;
    bool print_chunks = true;           // one digest line per chunk (off in the selftest)
    std::string command;                // as invoked, for the log
};

void print_book(const ExactOrderBook& b, const std::string& name, uint16_t loc, int depth) {
    BBO q = b.bbo();
    size_t nb = 0, na = 0;
    b.for_each_level(Side::Bid, [&](Price, const PriceLevel&) { ++nb; });
    b.for_each_level(Side::Ask, [&](Price, const PriceLevel&) { ++na; });
    std::printf("  %-6s locate %-5u resting %s | levels bid %zu ask %zu | overflow levels %s | ",
                name.c_str(), static_cast<unsigned>(loc), num(b.live_orders()).c_str(), nb, na,
                num(b.overflow_levels()).c_str());
    if (q.bid_qty && q.ask_qty)
        std::printf("BBO %s x %s / %s x %s\n", dollars(q.bid_price).c_str(), num(q.bid_qty).c_str(),
                    dollars(q.ask_price).c_str(), num(q.ask_qty).c_str());
    else
        std::printf("BBO %s / %s\n", q.bid_qty ? dollars(q.bid_price).c_str() : "-",
                    q.ask_qty ? dollars(q.ask_price).c_str() : "-");
    std::vector<std::pair<Price, PriceLevel>> bids, asks;
    b.for_each_level(Side::Bid, [&](Price p, const PriceLevel& L) {
        if (static_cast<int>(bids.size()) < depth) bids.push_back({p, L});
    });
    b.for_each_level(Side::Ask, [&](Price p, const PriceLevel& L) {
        if (static_cast<int>(asks.size()) < depth) asks.push_back({p, L});
    });
    for (int i = 0; i < depth && (i < static_cast<int>(bids.size()) || i < static_cast<int>(asks.size())); ++i) {
        char l[80] = "", r[80] = "";
        if (i < static_cast<int>(bids.size()))
            std::snprintf(l, sizeof l, "%12s %10s (%4u)", dollars(bids[i].first).c_str(),
                          num(bids[i].second.total_qty).c_str(), bids[i].second.count);
        if (i < static_cast<int>(asks.size()))
            std::snprintf(r, sizeof r, "%12s %10s (%4u)", dollars(asks[i].first).c_str(),
                          num(asks[i].second.total_qty).c_str(), asks[i].second.count);
        std::printf("         bid %-32s | ask %s\n", l, r);
    }
}

int replay(const Options& o) {
    using clock = std::chrono::steady_clock;
    const auto wall0 = clock::now();
    std::time_t now = std::time(nullptr);
    char when[64];
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&now));

    std::printf("=== lob_replay: NASDAQ TotalView-ITCH 5.0 day replay%s ===\n",
                o.differential ? " with ref::Market differential" : "");
    std::printf("run: %s local | cpu: %s | compiler: %s\n", when, cpu_name().c_str(),
                compiler_name().c_str());
    std::printf("file: %s\n", o.path.c_str());
    std::printf("chunk %s bytes | checkpoint every %s messages | ladder budget %s MB\n\n",
                num(o.chunk).c_str(), num(o.checkpoint).c_str(), num(o.ladder_mb).c_str());

    // ---- pass 1 -----------------------------------------------------------
    DayScan d;
    for (const std::string& t : o.at) {
        uint64_t ns;
        if (!parse_clock(t, ns)) { std::printf("bad --at time %s\n", t.c_str()); return 2; }
        d.at_ns.push_back(ns);
    }
    std::sort(d.at_ns.begin(), d.at_ns.end());
    const auto s0 = clock::now();
    prescan(o.path.c_str(), o.chunk, d);
    const double scan_s = std::chrono::duration<double>(clock::now() - s0).count();
    uint64_t dir_locates = 0, active = 0;
    for (const SymbolScan& s : d.sym) {
        if (s.directory_msgs) ++dir_locates;
        if (s.order_msgs) ++active;
    }
    std::printf("[1] pre-scan (untimed, %.1f s)\n", scan_s);
    std::printf("  bytes %s | messages %s | trailing bytes %s\n", num(d.bytes).c_str(),
                num(d.messages).c_str(), num(d.trailing).c_str());
    std::printf("  message counts by type:\n");
    int col = 0;
    for (int t = 0; t < 256; ++t) {
        if (!d.by_type[t]) continue;
        std::printf("%s    %c %13s", col == 0 ? " " : "", t, num(d.by_type[t]).c_str());
        if (++col == 4) { std::printf("\n"); col = 0; }
    }
    if (col) std::printf("\n");
    std::printf("  bad_length %s | unknown type %s\n", num(d.bad_length).c_str(),
                num(d.unknown_type).c_str());
    std::printf("  first message %s | last message %s (ET, from message timestamps) | "
                "timestamps out of order %s\n",
                clock_of(d.first_ts).c_str(), clock_of(d.last_ts).c_str(),
                num(d.ts_backwards).c_str());
    std::printf("  stock directory: %s 'R' messages for %s stock_locates (%s repeated, %s renamed) | "
                "locates with order messages: %s\n",
                num(d.by_type['R']).c_str(), num(dir_locates).c_str(),
                num(d.directory_repeats).c_str(), num(d.directory_renamed).c_str(),
                num(active).c_str());
    uint64_t adds = 0;
    for (const SymbolScan& s : d.sym) adds += s.adds;
    std::printf("  adds (A, F and the add half of U): %s | peak resting orders, whole market: %s\n",
                num(adds).c_str(), num(d.peak_live).c_str());
    std::printf("  add prices that are not whole cents: %s | outside $0.01..$1310.72: %s\n",
                num(d.subpenny_adds).c_str(), num(d.outside_cent_band).c_str());

    double ladder_mb = 0;
    const auto z0 = clock::now();
    std::vector<Sizing> z = size_books(d, o.ladder_mb * 1'000'000ull, ladder_mb);
    const double size_s = std::chrono::duration<double>(clock::now() - z0).count();
    uint64_t n_books = 0, tick1 = 0, predicted_ladder_adds = 0, pool_slots = 0, idmap_slots = 0;
    std::vector<uint32_t> bands;
    for (const Sizing& q : z) {
        if (!q.has_book) continue;
        ++n_books;
        if (q.tick == 1) ++tick1;
        predicted_ladder_adds += q.ladder_adds;
        pool_slots += q.pool;
        idmap_slots += uint64_t{1} << q.idmap_log2;
        bands.push_back(q.band);
    }
    std::sort(bands.begin(), bands.end());
    std::printf("  sizing (%.1f s): %s books | grid $0.01: %s, grid $0.0001: %s | ladder %.0f MB | "
                "band ticks min %s median %s max %s\n",
                size_s, num(n_books).c_str(), num(n_books - tick1).c_str(), num(tick1).c_str(),
                ladder_mb, num(bands.empty() ? 0 : bands.front()).c_str(),
                num(bands.empty() ? 0 : bands[bands.size() / 2]).c_str(),
                num(bands.empty() ? 0 : bands.back()).c_str());
    std::printf("  adds the chosen ladders cover: %s of %s (%.3f%%); the other %s rest in the overflow\n",
                num(predicted_ladder_adds).c_str(), num(adds).c_str(),
                adds ? 100.0 * static_cast<double>(predicted_ladder_adds) / static_cast<double>(adds) : 0.0,
                num(adds - predicted_ladder_adds).c_str());
    std::printf("  pools %s order slots (%.0f MB) | id maps %s slots (%.0f MB)\n\n",
                num(pool_slots).c_str(), static_cast<double>(pool_slots) * sizeof(Order) / 1e6,
                num(idmap_slots).c_str(), static_cast<double>(idmap_slots) * 16 / 1e6);

    // Named symbols and their crosses.
    std::unordered_map<std::string, uint16_t> by_name;
    for (int loc = 0; loc < 65536; ++loc)
        if (d.sym[loc].directory_msgs) by_name[d.sym[loc].name] = static_cast<uint16_t>(loc);
    std::vector<std::pair<std::string, int>> named;   // name, locate (-1: not in directory)
    for (const std::string& s : o.symbols) {
        auto it = by_name.find(s);
        named.push_back({s, it == by_name.end() ? -1 : it->second});
    }
    std::printf("  named symbols (Stock Directory, and the 'Q' crosses NASDAQ ran):\n");
    for (const auto& [name, loc] : named) {
        if (loc < 0) { std::printf("  %-6s not in this day's stock directory\n", name.c_str()); continue; }
        const SymbolScan& s = d.sym[loc];
        const Sizing& q = z[loc];
        std::printf("  %-6s locate %-5d adds %s | peak resting %s | grid $%s | ladder %s ticks from $%s\n",
                    name.c_str(), loc, num(s.adds).c_str(), num(s.peak_live).c_str(),
                    q.tick == 1 ? "0.0001" : "0.01", num(q.band).c_str(), dollars(q.base).c_str());
        auto cross = [&](const char* what, const Cross& c) {
            if (c.seen)
                std::printf("         %s cross %s x %s shares at %s\n", what, dollars(c.price).c_str(),
                            num(c.shares).c_str(), clock_of(c.ts).c_str());
            else
                std::printf("         %s cross: none in this file\n", what);
        };
        cross("opening", s.open_cross);
        cross("closing", s.close_cross);
    }
    std::printf("\n");

    // ---- books ------------------------------------------------------------
    std::vector<std::unique_ptr<ExactOrderBook>> owned(65536);
    std::vector<ExactOrderBook*> books(65536, nullptr);
    for (int loc = 0; loc < 65536; ++loc) {
        const Sizing& q = z[loc];
        if (!q.has_book) continue;
        owned[loc] = std::make_unique<ExactOrderBook>(
            BookParams{q.base, q.tick, q.band, q.pool, q.idmap_log2});
        books[loc] = owned[loc].get();
    }
    std::unique_ptr<ref::Market> refm;
    if (o.differential) refm = std::make_unique<ref::Market>();

    // Checkpoints: message counts after which the books are compared.
    std::vector<uint64_t> cps;
    for (uint64_t c = o.checkpoint; c < d.messages; c += o.checkpoint) cps.push_back(c);
    for (uint64_t a : d.at_index) cps.push_back(a);
    cps.push_back(d.messages);
    std::sort(cps.begin(), cps.end());
    cps.erase(std::unique(cps.begin(), cps.end()), cps.end());

#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif
    const bool pinned = pin_current_thread(static_cast<unsigned>(o.cpu));
    std::printf("[2] replay%s | thread %s cpu %d\n",
                o.differential ? ", differential against ref::Market at every checkpoint" : "",
                pinned ? "pinned to" : "not pinned, asked for", o.cpu);
    std::fflush(stdout);

    ChunkReader rd(o.path.c_str(), o.chunk);
    if (!rd.ok()) { std::printf("cannot open %s\n", o.path.c_str()); return 2; }
    uint64_t n = 0, bad_length = 0, mismatches = 0, checkpoints = 0, printed = 0;
    uint64_t last_ts = 0;
    uint64_t chunks = 0;
    Digest chain;                       // over every chunk boundary digest
    double timed_s = 0;
    DiffTotals total;
    size_t next_cp = 0, next_at = 0;
    const uint8_t* consumed = nullptr;
    const uint8_t* last = nullptr;

    auto checkpoint = [&](bool final_cp) {
        ++checkpoints;
        uint64_t resting = 0;
        for (const ExactOrderBook* b : books) if (b) resting += b->live_orders();
        if (!refm) {
            std::printf("  checkpoint %3" PRIu64 " | message %13s | %s | resting orders %s\n",
                        checkpoints, num(n).c_str(), clock_of(last_ts).c_str(), num(resting).c_str());
            std::fflush(stdout);
            return;
        }
        DiffTotals t;
        uint64_t bad = 0;
        for (int loc = 0; loc < 65536; ++loc) {
            if (!books[loc]) {
                if (refm->book(static_cast<uint16_t>(loc)).live != 0) {
                    ++bad;
                    if (++printed <= 20)
                        std::printf("  MISMATCH locate %d: reference has orders, no book\n", loc);
                }
                continue;
            }
            std::string diff = diff_book(*books[loc], *refm, static_cast<uint16_t>(loc), t);
            if (!diff.empty()) {
                ++bad;
                if (++printed <= 20)
                    std::printf("  MISMATCH locate %d (%s) after message %s: %s\n", loc,
                                d.sym[loc].name.c_str(), num(n).c_str(), diff.c_str());
            }
        }
        if (refm->live_orders() != resting) {
            ++bad;
            std::printf("  MISMATCH resting orders: books %s, reference %s\n", num(resting).c_str(),
                        num(refm->live_orders()).c_str());
        }
        mismatches += bad;
        total.books += t.books; total.levels += t.levels; total.orders += t.orders;
        std::printf("  checkpoint %3" PRIu64 " | message %13s | %s | books %s | levels %s | "
                    "orders %s | mismatches %" PRIu64 "%s\n",
                    checkpoints, num(n).c_str(), clock_of(last_ts).c_str(), num(t.books).c_str(),
                    num(t.levels).c_str(), num(t.orders).c_str(), bad, final_cp ? " (end of file)" : "");
        std::fflush(stdout);
    };

    while (rd.next(consumed)) {
        const uint8_t* p = rd.data();
        const uint8_t* end = p + rd.size();
        for (;;) {
            if (next_cp < cps.size() && n == cps[next_cp]) {     // compare, untimed
                ++next_cp;
                checkpoint(n == d.messages);
                while (next_at < d.at_index.size() && d.at_index[next_at] == n) {
                    std::printf("  books at %s ET (state after message %s):\n",
                                clock_of(d.at_ns[next_at]).c_str(), num(n).c_str());
                    for (const auto& [name, loc] : named)
                        if (loc >= 0 && books[loc])
                            print_book(*books[loc], name, static_cast<uint16_t>(loc), o.depth);
                    ++next_at;
                }
                continue;
            }
            const uint64_t room = next_cp < cps.size() ? cps[next_cp] - n : ~uint64_t{0};
            const uint8_t* seg = p;
            const uint64_t n0 = n;
            const auto t0 = clock::now();
            p = dispatch_segment(p, end, room, books.data(), n, bad_length, last);
            const auto t1 = clock::now();
            timed_s += std::chrono::duration<double>(t1 - t0).count();
            if (n == n0) break;                                   // nothing whole left in this chunk
            if (((last[-2] << 8) | last[-1]) >= 11) last_ts = itch::timestamp_ns(last);
            if (refm) {                                           // same messages, untimed
                for (const uint8_t* q = seg; q < p;) {
                    const uint16_t len = static_cast<uint16_t>((q[0] << 8) | q[1]);
                    refm->on_message(q + 2, len);
                    q += 2 + len;
                }
            }
        }
        // Book digest at the chunk boundary (untimed; see books_digest()).
        const uint64_t dg = books_digest(books.data());
        chain.add(dg);
        ++chunks;
        if (o.print_chunks) {
            std::printf("  chunk %3" PRIu64 " | messages %13s | book digest %s\n", chunks,
                        num(n).c_str(), hex64(dg).c_str());
            std::fflush(stdout);
        }
        consumed = p;
    }

    // ---- totals -----------------------------------------------------------
    uint64_t dropped = 0, unknown = 0, ovf_adds = 0, resting = 0;
    for (const ExactOrderBook* b : books) {
        if (!b) continue;
        dropped += b->dropped_out_of_band();
        unknown += b->unknown_id();
        ovf_adds += b->overflow_adds();
        resting += b->live_orders();
    }
    std::printf("\n[3] totals\n");
    std::printf("  messages: pass 1 %s | pass 2 %s | trailing bytes %s\n", num(d.messages).c_str(),
                num(n).c_str(), num(rd.trailing()).c_str());
    std::printf("  books: %s | resting orders at end of file: %s\n", num(n_books).c_str(),
                num(resting).c_str());
    std::printf("  bad_length: pre-scan %s, dispatch %s | dropped_out_of_band %s | unknown order id %s\n",
                num(d.bad_length).c_str(), num(bad_length).c_str(), num(dropped).c_str(),
                num(unknown).c_str());
    std::printf("  adds on the ladder %s | in the overflow %s (%.3f%% of %s; sizing predicted %s)\n",
                num(adds - ovf_adds).c_str(), num(ovf_adds).c_str(),
                adds ? 100.0 * static_cast<double>(ovf_adds) / static_cast<double>(adds) : 0.0,
                num(adds).c_str(), num(adds - predicted_ladder_adds).c_str());
    if (refm) {
        const ref::Counters& c = refm->counters();
        std::printf("  reference applied: A %s  F %s  E %s  C %s  X %s  D %s  U %s\n",
                    num(c.seen['A']).c_str(), num(c.seen['F']).c_str(), num(c.seen['E']).c_str(),
                    num(c.seen['C']).c_str(), num(c.seen['X']).c_str(), num(c.seen['D']).c_str(),
                    num(c.seen['U']).c_str());
        std::printf("  reference anomalies: unknown ref %s | duplicate ref %s | over-execution %s | "
                    "locate mismatch %s | bad side %s | bad length %s\n",
                    num(c.unknown_ref).c_str(), num(c.duplicate_ref).c_str(),
                    num(c.over_execution).c_str(), num(c.locate_mismatch).c_str(),
                    num(c.bad_side).c_str(), num(c.bad_length).c_str());
        std::printf("  checkpoints %s | book comparisons %s | levels compared %s | "
                    "orders compared in queue order %s | mismatches %s\n",
                    num(checkpoints).c_str(), num(total.books).c_str(), num(total.levels).c_str(),
                    num(total.orders).c_str(), num(mismatches).c_str());
    } else {
        std::printf("  checkpoints %s (no differential)\n", num(checkpoints).c_str());
    }
    std::printf("  dispatch time %.3f s for %s messages (%.1f ns/msg)%s\n", timed_s, num(n).c_str(),
                n ? timed_s * 1e9 / static_cast<double>(n) : 0.0,
                refm ? " - differential run: not a measurement" : "");
    std::printf("  final books (end of file):\n");
    for (const auto& [name, loc] : named)
        if (loc >= 0 && books[loc]) print_book(*books[loc], name, static_cast<uint16_t>(loc), o.depth);
    std::printf("  book digest chain over %s chunk boundaries: %s\n", num(chunks).c_str(),
                hex64(chain.h).c_str());
    std::printf("  wall time %.0f s\n", std::chrono::duration<double>(clock::now() - wall0).count());

    bool pass = n == d.messages && rd.trailing() == 0 && d.trailing == 0 && d.bad_length == 0 &&
                bad_length == 0 && dropped == 0 && unknown == 0 &&
                ovf_adds == adds - predicted_ladder_adds;
    if (refm) {
        const ref::Counters& c = refm->counters();
        pass = pass && mismatches == 0 && c.unknown_ref == 0 && c.duplicate_ref == 0 &&
               c.over_execution == 0 && c.locate_mismatch == 0 && c.bad_side == 0 &&
               c.bad_length == 0 && refm->live_orders() == resting;
        for (char t : {'A', 'F', 'E', 'C', 'X', 'D', 'U'})
            pass = pass && c.seen[static_cast<uint8_t>(t)] == d.by_type[static_cast<uint8_t>(t)];
    }
    std::printf("\n%s\n", pass ? "RESULT: PASS" : "RESULT: FAIL");
    return pass ? 0 : 1;
}

// A generated day (src/wire_gen.hpp) written to disk and replayed with the
// differential through deliberately small, odd-sized chunks, so that
// messages and even length prefixes straddle chunk boundaries thousands of
// times. The CI target: every check of a real run, on data CI can carry.
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
    Options o;
    o.path = path;
    o.differential = true;
    o.checkpoint = 100'000;
    o.at = {"09:45:00"};
    o.symbols = {"SYM0", "SYM1", "SYM5", "NOSUCH"};
    o.chunk = 4096 + 7;
    o.ladder_mb = 1;                     // small budget: many adds must take the overflow
    o.print_chunks = false;              // thousands of 4 KB chunks: the chain is printed
    int rc = replay(o);
    std::remove(path);
    return rc;
}

} // namespace

int main(int argc, char** argv) {
    Options o;
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
        if (a == "--selftest") o.selftest = true;
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
                        "       %s --selftest\n", argv[0], argv[0]);
            return 2;
        }
    }
    if (o.selftest) return selftest();
    if (o.path.empty()) { std::printf("no input file (see --help)\n"); return 2; }
    return replay(o);
}
