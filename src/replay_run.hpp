#pragma once
// ---------------------------------------------------------------------------
// replay_run.hpp - one lob_replay run over a decompressed ITCH 5.0
// BinaryFILE: pass 1 (the untimed pre-scan and book sizing of
// src/replay_day.hpp), pass 2 (the timed dispatch into one ExactOrderBook per
// stock_locate), the optional ref::Market differential, and the totals.
//
// lob_replay (src/replay_main.cpp) runs it on a recorded day; the crafted-file
// test (src/replay_fixture.hpp, run by `lob_replay --fixture` and by
// lob_bench) runs it on a small hand-built file. replay() can hand its books
// and counters back through a ReplayReport so a test can inspect them.
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

#ifndef LOB_GIT_COMMIT
  #define LOB_GIT_COMMIT "unknown"
#endif

namespace day {

struct ReplayOptions {
    std::string path;
    bool differential = false;
    uint64_t checkpoint = 5'000'000;
    std::vector<std::string> at = {"12:00:00", "16:00:00"};
    std::vector<std::string> symbols = {"AAPL", "MSFT", "AMZN", "BRK.A", "BKNG", "WFT"};
    int depth = 5;
    size_t chunk = size_t{256} << 20;
    uint64_t ladder_mb = 1024;
    Placement placement = Placement::Prescan;   // see plan_books() in src/replay_day.hpp
    uint32_t recenter_after = kRecenterAfter;   // placement causal only
    int cpu = 2;                        // < 0: leave the thread's affinity and priority alone
    bool print_chunks = true;           // one digest line per chunk
    bool print_checkpoints = true;      // one line per checkpoint (a mismatch always prints)
    bool quiet = false;                 // print mismatches only
    std::string command;                // as invoked, for the log
};

// What a run hands back when asked (replay(o, &report)).
struct ReplayReport {
    bool     pass = false;
    uint64_t messages = 0;              // pass 2
    uint64_t bytes = 0, trailing = 0;
    uint64_t by_type[256] = {};         // pass 1, by type byte
    uint64_t adds = 0, overflow_adds = 0, predicted_overflow = 0;
    uint64_t dropped = 0, unknown = 0, bad_length = 0, resting = 0;
    uint64_t checkpoints = 0, mismatches = 0;
    uint64_t touch_checked = 0;         // order messages followed by the per-message check
    uint64_t touch_mismatches = 0;      // ... that found a difference
    uint64_t chunks = 0;
    MoveTotals moves;                   // placement causal: what the moving ladders did
    uint64_t cuts = 0;                  // chunk boundaries that fell inside a message
    uint64_t prefix_cuts = 0;           // ... between the two bytes of its length prefix
    uint64_t final_digest = 0;          // books_digest() after the last message
    std::vector<Sizing> sizing;                             // by stock_locate
    std::vector<std::unique_ptr<ExactOrderBook>> books;     // by stock_locate, end-of-file state
};

inline void print_book(const ExactOrderBook& b, const std::string& name, uint16_t loc, int depth) {
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

// The whole run. Returns 0 when every check passes, 1 when one fails, 2 when
// the input cannot be read or an option is malformed.
inline int replay(const ReplayOptions& o, ReplayReport* report = nullptr) {
#define SAY(...) do { if (!o.quiet) std::printf(__VA_ARGS__); } while (0)
    using clock = std::chrono::steady_clock;
    const auto wall0 = clock::now();
    std::time_t now = std::time(nullptr);
    char when[64];
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&now));

    SAY("=== lob_replay: NASDAQ TotalView-ITCH 5.0 day replay%s ===\n",
        o.differential ? " with ref::Market differential" : "");
    SAY("run: %s local | cpu: %s | compiler: %s\n", when, cpu_name().c_str(),
        compiler_name().c_str());
    if (!o.command.empty()) SAY("command: %s\n", o.command.c_str());
    SAY("source: git commit %s\n", LOB_GIT_COMMIT);
    SAY("file: %s\n", o.path.c_str());
    SAY("chunk %s bytes | checkpoint every %s messages | ladder budget %s MB | placement %s\n\n",
        num(o.chunk).c_str(), num(o.checkpoint).c_str(), num(o.ladder_mb).c_str(),
        placement_name(o.placement));

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
    SAY("[1] pre-scan (untimed, %.1f s)\n", scan_s);
    SAY("  bytes %s | messages %s | trailing bytes %s\n", num(d.bytes).c_str(),
        num(d.messages).c_str(), num(d.trailing).c_str());
    SAY("  message counts by type:\n");
    int col = 0;
    for (int t = 0; t < 256; ++t) {
        if (!d.by_type[t]) continue;
        SAY("%s    %c %13s", col == 0 ? " " : "", t, num(d.by_type[t]).c_str());
        if (++col == 4) { SAY("\n"); col = 0; }
    }
    if (col) SAY("\n");
    SAY("  bad_length %s | unknown type %s\n", num(d.bad_length).c_str(),
        num(d.unknown_type).c_str());
    SAY("  first message %s | last message %s (ET, from message timestamps) | "
        "timestamps out of order %s\n",
        clock_of(d.first_ts).c_str(), clock_of(d.last_ts).c_str(),
        num(d.ts_backwards).c_str());
    SAY("  stock directory: %s 'R' messages for %s stock_locates (%s repeated, %s renamed) | "
        "locates with order messages: %s\n",
        num(d.by_type['R']).c_str(), num(dir_locates).c_str(),
        num(d.directory_repeats).c_str(), num(d.directory_renamed).c_str(),
        num(active).c_str());
    uint64_t adds = 0;
    for (const SymbolScan& s : d.sym) adds += s.adds;
    SAY("  adds (A, F and the add half of U): %s | peak resting orders, whole market: %s\n",
        num(adds).c_str(), num(d.peak_live).c_str());
    SAY("  add prices that are not whole cents: %s | outside $0.01..$1310.72: %s\n",
        num(d.subpenny_adds).c_str(), num(d.outside_cent_band).c_str());

    double ladder_mb = 0;
    const auto z0 = clock::now();
    uint32_t uniform = 0;
    std::vector<Sizing> z = plan_books(d, o.placement, o.ladder_mb * 1'000'000ull, ladder_mb, uniform,
                                       o.recenter_after);
    const bool causal = o.placement == Placement::Causal;
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
    SAY("  sizing (%.1f s): %s books | grid $0.01: %s, grid $0.0001: %s | ladder %.0f MB | "
        "band ticks min %s median %s max %s\n",
        size_s, num(n_books).c_str(), num(n_books - tick1).c_str(), num(tick1).c_str(),
        ladder_mb, num(bands.empty() ? 0 : bands.front()).c_str(),
        num(bands.empty() ? 0 : bands[bands.size() / 2]).c_str(),
        num(bands.empty() ? 0 : bands.back()).c_str());
    if (o.placement == Placement::FirstAdd)
        SAY("  placement first-add: every ladder %s ticks, centered on the symbol's first add price, grid "
            "$0.01 if that price is at least $1.00 and $0.0001 below\n", num(uniform).c_str());
    if (causal)
        SAY("  placement causal: every ladder %s ticks; each book centers it on its first add and re-centers "
            "it on its midpoint after %u near-touch misses, grid $0.01 at or above $1.00 and $0.0001 below, "
            "from the messages already applied (grids and bands above are before the first add)\n"
            "  adds the ladders cover: not predicted, the ladders move\n",
            num(uniform).c_str(), o.recenter_after);
    else
        SAY("  adds the chosen ladders cover: %s of %s (%.3f%%); the other %s rest in the overflow\n",
            num(predicted_ladder_adds).c_str(), num(adds).c_str(),
            adds ? 100.0 * static_cast<double>(predicted_ladder_adds) / static_cast<double>(adds) : 0.0,
            num(adds - predicted_ladder_adds).c_str());
    SAY("  pools %s order slots (%.0f MB) | id maps %s slots (%.0f MB)\n\n",
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
    SAY("  named symbols (Stock Directory, and the 'Q' crosses NASDAQ ran):\n");
    for (const auto& [name, loc] : named) {
        if (loc < 0) { SAY("  %-6s not in this day's stock directory\n", name.c_str()); continue; }
        const SymbolScan& s = d.sym[loc];
        const Sizing& q = z[loc];
        if (causal)
            SAY("  %-6s locate %-5d adds %s | peak resting %s | ladder %s ticks, placed by the book\n",
                name.c_str(), loc, num(s.adds).c_str(), num(s.peak_live).c_str(), num(q.band).c_str());
        else
            SAY("  %-6s locate %-5d adds %s | peak resting %s | grid $%s | ladder %s ticks from $%s\n",
                name.c_str(), loc, num(s.adds).c_str(), num(s.peak_live).c_str(),
                q.tick == 1 ? "0.0001" : "0.01", num(q.band).c_str(), dollars(q.base).c_str());
        auto cross = [&](const char* what, const Cross& c) {
            if (c.seen)
                SAY("         %s cross %s x %s shares at %s\n", what, dollars(c.price).c_str(),
                    num(c.shares).c_str(), clock_of(c.ts).c_str());
            else
                SAY("         %s cross: none in this file\n", what);
        };
        cross("opening", s.open_cross);
        cross("closing", s.close_cross);
    }
    SAY("\n");

    // ---- books ------------------------------------------------------------
    std::vector<std::unique_ptr<ExactOrderBook>> owned(65536);
    std::vector<ExactOrderBook*> books(65536, nullptr);
    for (int loc = 0; loc < 65536; ++loc) {
        const Sizing& q = z[loc];
        if (!q.has_book) continue;
        owned[loc] = std::make_unique<ExactOrderBook>(book_params(q));
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

    bool pinned = false;
    if (o.cpu >= 0) {
#ifdef _WIN32
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif
        pinned = pin_current_thread(static_cast<unsigned>(o.cpu));
    }
    if (o.cpu >= 0)
        SAY("[2] replay%s | thread %s cpu %d\n",
            o.differential ? ", differential against ref::Market after every message and in full at every checkpoint" : "",
            pinned ? "pinned to" : "not pinned, asked for", o.cpu);
    else
        SAY("[2] replay%s | thread not pinned\n",
            o.differential ? ", differential against ref::Market after every message and in full at every checkpoint" : "");
    if (!o.quiet) std::fflush(stdout);

    ChunkReader rd(o.path.c_str(), o.chunk);
    if (!rd.ok()) { std::printf("cannot open %s\n", o.path.c_str()); return 2; }
    uint64_t n = 0, bad_length = 0, mismatches = 0, checkpoints = 0, printed = 0;
    uint64_t touch_mismatches = 0;      // per-message checks that found a difference
    TouchTotals touched;                // what the per-message checks compared
    uint64_t last_ts = 0;
    uint64_t chunks = 0, cuts = 0, prefix_cuts = 0;
    Digest chain;                       // over every chunk boundary digest
    double timed_s = 0;
    DiffTotals total;
    size_t next_cp = 0, next_at = 0;
    size_t carry = 0;                   // bytes of an incomplete message at the end of the last chunk
    const uint8_t* consumed = nullptr;
    const uint8_t* last = nullptr;

    auto checkpoint = [&](bool final_cp) {
        ++checkpoints;
        uint64_t resting = 0;
        for (const ExactOrderBook* b : books) if (b) resting += b->live_orders();
        if (!refm) {
            if (o.print_checkpoints) {
                SAY("  checkpoint %3" PRIu64 " | message %13s | %s | resting orders %s\n",
                    checkpoints, num(n).c_str(), clock_of(last_ts).c_str(), num(resting).c_str());
                if (!o.quiet) std::fflush(stdout);
            }
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
        if (o.print_checkpoints) {
            SAY("  checkpoint %3" PRIu64 " | message %13s | %s | books %s | levels %s | "
                "orders %s | mismatches %" PRIu64 "%s\n",
                checkpoints, num(n).c_str(), clock_of(last_ts).c_str(), num(t.books).c_str(),
                num(t.levels).c_str(), num(t.orders).c_str(), bad, final_cp ? " (end of file)" : "");
            if (!o.quiet) std::fflush(stdout);
        }
    };

    while (rd.next(consumed)) {
        if (carry) {                                              // a message straddles the boundary
            ++cuts;
            if (carry == 1) ++prefix_cuts;
        }
        const uint8_t* p = rd.data();
        const uint8_t* end = p + rd.size();
        for (;;) {
            if (next_cp < cps.size() && n == cps[next_cp]) {     // compare, untimed
                ++next_cp;
                checkpoint(n == d.messages);
                while (next_at < d.at_index.size() && d.at_index[next_at] == n) {
                    if (!o.quiet) {
                        std::printf("  books at %s ET (state after message %s):\n",
                                    clock_of(d.at_ns[next_at]).c_str(), num(n).c_str());
                        for (const auto& [name, loc] : named)
                            if (loc >= 0 && books[loc])
                                print_book(*books[loc], name, static_cast<uint16_t>(loc), o.depth);
                    }
                    ++next_at;
                }
                continue;
            }
            // With the reference, one message at a time, so that what each
            // message touched can be compared as soon as both have applied it.
            const uint64_t room = refm ? 1 : next_cp < cps.size() ? cps[next_cp] - n : ~uint64_t{0};
            const uint8_t* seg = p;
            const uint64_t n0 = n;
            const auto t0 = clock::now();
            p = dispatch_segment(p, end, room, books.data(), n, bad_length, last);
            const auto t1 = clock::now();
            timed_s += std::chrono::duration<double>(t1 - t0).count();
            if (n == n0) break;                                   // nothing whole left in this chunk
            if (((last[-2] << 8) | last[-1]) >= 11) last_ts = itch::timestamp_ns(last);
            if (refm) {                                           // the same message, untimed
                const uint16_t len = static_cast<uint16_t>((seg[0] << 8) | seg[1]);
                const Touch t = touch_before(*refm, seg + 2, len);
                refm->on_message(seg + 2, len);
                if (t.check && books[t.locate]) {
                    const std::string diff = diff_touch(*books[t.locate], *refm, t, touched);
                    if (!diff.empty()) {
                        ++touch_mismatches;
                        if (++printed <= 20)
                            std::printf("  MISMATCH after message %s (%c, locate %u, %s): %s\n",
                                        num(n).c_str(), static_cast<char>(t.type),
                                        static_cast<unsigned>(t.locate), d.sym[t.locate].name.c_str(),
                                        diff.c_str());
                    }
                } else if (t.check) {
                    ++touch_mismatches;                           // an order message with no book
                    if (++printed <= 20)
                        std::printf("  MISMATCH after message %s: locate %u has no book\n",
                                    num(n).c_str(), static_cast<unsigned>(t.locate));
                }
            }
        }
        // Book digest at the chunk boundary (untimed; see books_digest()).
        const uint64_t dg = books_digest(books.data());
        chain.add(dg);
        ++chunks;
        if (o.print_chunks) {
            SAY("  chunk %3" PRIu64 " | messages %13s | book digest %s\n", chunks,
                num(n).c_str(), hex64(dg).c_str());
            if (!o.quiet) std::fflush(stdout);
        }
        consumed = p;
        carry = static_cast<size_t>(end - p);
    }

    // ---- totals -----------------------------------------------------------
    uint64_t dropped = 0, unknown = 0, ovf_adds = 0, resting = 0;
    MoveTotals moves;
    for (const ExactOrderBook* b : books) {
        if (!b) continue;
        dropped += b->dropped_out_of_band();
        unknown += b->unknown_id();
        ovf_adds += b->overflow_adds();
        resting += b->live_orders();
        moves.add(*b);
    }
    SAY("\n[3] totals\n");
    SAY("  messages: pass 1 %s | pass 2 %s | trailing bytes %s\n", num(d.messages).c_str(),
        num(n).c_str(), num(rd.trailing()).c_str());
    SAY("  chunks %s | chunk boundaries inside a message %s (inside its length prefix %s)\n",
        num(chunks).c_str(), num(cuts).c_str(), num(prefix_cuts).c_str());
    SAY("  books: %s | resting orders at end of file: %s\n", num(n_books).c_str(),
        num(resting).c_str());
    SAY("  bad_length: pre-scan %s, dispatch %s | dropped_out_of_band %s | unknown order id %s\n",
        num(d.bad_length).c_str(), num(bad_length).c_str(), num(dropped).c_str(),
        num(unknown).c_str());
    SAY("  adds on the ladder %s | in the overflow %s (%.3f%% of %s; sizing predicted %s)\n",
        num(adds - ovf_adds).c_str(), num(ovf_adds).c_str(),
        adds ? 100.0 * static_cast<double>(ovf_adds) / static_cast<double>(adds) : 0.0,
        num(adds).c_str(), causal ? "nothing, the ladders move" : num(adds - predicted_ladder_adds).c_str());
    if (causal)
        SAY("  causal ladders: placed %s | re-centered %s times | levels moved %s | orders relinked %s\n",
            num(moves.placements).c_str(), num(moves.recenters).c_str(), num(moves.levels).c_str(),
            num(moves.orders).c_str());
    if (refm) {
        const ref::Counters& c = refm->counters();
        SAY("  reference applied: A %s  F %s  E %s  C %s  X %s  D %s  U %s\n",
            num(c.seen['A']).c_str(), num(c.seen['F']).c_str(), num(c.seen['E']).c_str(),
            num(c.seen['C']).c_str(), num(c.seen['X']).c_str(), num(c.seen['D']).c_str(),
            num(c.seen['U']).c_str());
        SAY("  reference anomalies: unknown ref %s | duplicate ref %s | over-execution %s | "
            "locate mismatch %s | bad side %s | bad length %s\n",
            num(c.unknown_ref).c_str(), num(c.duplicate_ref).c_str(),
            num(c.over_execution).c_str(), num(c.locate_mismatch).c_str(),
            num(c.bad_side).c_str(), num(c.bad_length).c_str());
        SAY("  checkpoints %s | book comparisons %s | levels compared %s | "
            "orders compared in queue order %s | mismatches %s\n",
            num(checkpoints).c_str(), num(total.books).c_str(), num(total.levels).c_str(),
            num(total.orders).c_str(), num(mismatches).c_str());
        SAY("  after every order message: %s messages checked (resting count and BBO of the book, "
            "the orders and price levels the message touched) | orders compared %s | "
            "levels compared %s | mismatches %s\n",
            num(touched.messages).c_str(), num(touched.orders).c_str(), num(touched.levels).c_str(),
            num(touch_mismatches).c_str());
    } else {
        SAY("  checkpoints %s (no differential)\n", num(checkpoints).c_str());
    }
    SAY("  dispatch time %.3f s for %s messages (%.1f ns/msg)%s\n", timed_s, num(n).c_str(),
        n ? timed_s * 1e9 / static_cast<double>(n) : 0.0,
        refm ? " - differential run: not a measurement" : "");
    if (!o.quiet) {
        std::printf("  final books (end of file):\n");
        for (const auto& [name, loc] : named)
            if (loc >= 0 && books[loc]) print_book(*books[loc], name, static_cast<uint16_t>(loc), o.depth);
    }
    SAY("  book digest chain over %s chunk boundaries: %s\n", num(chunks).c_str(),
        hex64(chain.h).c_str());
    SAY("  wall time %.0f s\n", std::chrono::duration<double>(clock::now() - wall0).count());

    bool pass = n == d.messages && rd.trailing() == 0 && d.trailing == 0 && d.bad_length == 0 &&
                bad_length == 0 && dropped == 0 && unknown == 0 &&
                (causal || ovf_adds == adds - predicted_ladder_adds);
    if (refm) {
        const ref::Counters& c = refm->counters();
        pass = pass && mismatches == 0 && c.unknown_ref == 0 && c.duplicate_ref == 0 &&
               c.over_execution == 0 && c.locate_mismatch == 0 && c.bad_side == 0 &&
               c.bad_length == 0 && refm->live_orders() == resting && touch_mismatches == 0;
        uint64_t order_msgs = 0;
        for (char t : {'A', 'F', 'E', 'C', 'X', 'D', 'U'}) {
            pass = pass && c.seen[static_cast<uint8_t>(t)] == d.by_type[static_cast<uint8_t>(t)];
            order_msgs += d.by_type[static_cast<uint8_t>(t)];
        }
        pass = pass && touched.messages == order_msgs;       // every order message was checked
    }
    SAY("\n%s\n", pass ? "RESULT: PASS" : "RESULT: FAIL");

    if (report) {
        report->pass = pass;
        report->messages = n;
        report->bytes = d.bytes;
        report->trailing = rd.trailing();
        std::copy(std::begin(d.by_type), std::end(d.by_type), std::begin(report->by_type));
        report->adds = adds;
        report->overflow_adds = ovf_adds;
        report->predicted_overflow = adds - predicted_ladder_adds;
        report->dropped = dropped;
        report->unknown = unknown;
        report->bad_length = d.bad_length + bad_length;
        report->resting = resting;
        report->checkpoints = checkpoints;
        report->mismatches = mismatches;
        report->touch_checked = touched.messages;
        report->touch_mismatches = touch_mismatches;
        report->chunks = chunks;
        report->moves = moves;
        report->cuts = cuts;
        report->prefix_cuts = prefix_cuts;
        report->final_digest = books_digest(books.data());
        report->sizing = std::move(z);
        report->books = std::move(owned);
    }
    return pass ? 0 : 1;
#undef SAY
}

} // namespace day
