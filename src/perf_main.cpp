// ---------------------------------------------------------------------------
// perf_main.cpp - performance of the book on a recorded NASDAQ ITCH 5.0 day.
//
// Builds the same per-locate ExactOrderBooks as lob_replay (the pre-scan and
// sizing in src/replay_day.hpp), streams the decompressed BinaryFILE in
// chunks, and measures one of:
//
//   --mode single    single-core throughput. Every message goes through
//                    dispatch_segment() (the loop lob_replay uses) into the
//                    book of its stock_locate. Only that loop over a chunk
//                    already in memory is timed; the per-chunk times are
//                    summed. No per-message instrumentation exists in this
//                    build.
//   --mode demux     BasicParallelEngine<ExactOrderBook, WireUnits>: one
//                    demux thread reads the chunk and routes each message by
//                    stock_locate % W into W SPSC rings, one worker per ring.
//                    Timed per chunk from the first push until every ring is
//                    drained (all messages applied).
//   --mode presplit  the chunk is first split, untimed, into W per-shard
//                    buffers by stock_locate % W; then W workers apply their
//                    own buffer in parallel. Timed per chunk from the start
//                    signal until the last worker finishes.
//   --mode latency   per-message latency. Only in the lob_perf_latency
//                    build (-DLOB_PERF_LATENCY): every message is timed alone
//                    with rdtsc_begin()/rdtsc_end() (lfence; rdtsc ... rdtscp;
//                    lfence), the raw cycles go into an exact histogram per
//                    message type, and the minimum cost of an empty timer
//                    pair (the smaller of two calibrations, before and after
//                    the pass) is subtracted when the percentiles are read.
//
// After every chunk (untimed) the tool prints books_digest(), a digest of the
// full state of every book; lob_replay --differential prints the same
// digests, so every run here can be compared chunk by chunk with the run that
// was checked against the reference model.
//
// The TSC frequency is measured, not assumed: against QueryPerformanceCounter
// (std::chrono::steady_clock elsewhere) in five 1 s windows before the pass,
// and over the whole timed pass.
//
// --placement first-add places every ladder without looking ahead in the file
// (see place_at_first_add()); the default, prescan, is lob_replay's sizing.
//
// Usage:
//   lob_perf FILE [--mode single|demux|presplit] [--cpu N] [--cpus a,b,...]
//                 [--workers 1,2,4] [--chunk-mb N] [--ladder-mb N]
//                 [--placement prescan|first-add]
//   lob_perf_latency FILE [--cpu N] [--chunk-mb N] [--ladder-mb N]
//                         [--placement prescan|first-add]
// ---------------------------------------------------------------------------
#if defined(_WIN32) && !defined(_WIN32_WINNT)
  #define _WIN32_WINNT 0x0A00             // Windows 10: GetSystemCpuSetInformation
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "lob/book.hpp"
#include "lob/engine.hpp"
#include "lob/itch.hpp"

#include "replay_day.hpp"

#ifndef LOB_GIT_COMMIT
  #define LOB_GIT_COMMIT "unknown"
#endif
#ifndef LOB_BUILD_FLAGS
  #define LOB_BUILD_FLAGS "unknown"
#endif

using namespace lob;
using namespace day;

namespace {

using clk = std::chrono::steady_clock;

// ============================================================================
// Machine description
// ============================================================================
struct CpuSet {
    unsigned index = 0;         // logical processor index (affinity bit)
    unsigned core = 0;
    unsigned llc = 0;           // last-level cache index
    unsigned eff = 0;           // efficiency class: higher is the faster core type
};

std::vector<CpuSet> cpu_sets() {
    std::vector<CpuSet> out;
#ifdef _WIN32
    ULONG len = 0;
    GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
    std::vector<uint8_t> buf(len);
    auto* info = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf.data());
    if (len && GetSystemCpuSetInformation(info, len, &len, GetCurrentProcess(), 0)) {
        for (ULONG off = 0; off < len;) {
            auto* e = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf.data() + off);
            if (e->Type == CpuSetInformation)
                out.push_back({e->CpuSet.LogicalProcessorIndex, e->CpuSet.CoreIndex,
                               e->CpuSet.LastLevelCacheIndex, e->CpuSet.EfficiencyClass});
            off += e->Size;
        }
    }
#endif
    if (out.empty())
        for (unsigned i = 0; i < std::thread::hardware_concurrency(); ++i) out.push_back({i, i, 0, 0});
    std::sort(out.begin(), out.end(), [](const CpuSet& a, const CpuSet& b) { return a.index < b.index; });
    return out;
}

// Logical CPUs fastest type first: highest efficiency class, then those that
// share the fast cores' last-level cache, then by index.
std::vector<unsigned> cpu_order(const std::vector<CpuSet>& cs) {
    unsigned top = 0, top_llc = 0;
    for (const CpuSet& c : cs) if (c.eff > top) top = c.eff;
    for (const CpuSet& c : cs) if (c.eff == top) { top_llc = c.llc; break; }
    std::vector<CpuSet> v = cs;
    std::stable_sort(v.begin(), v.end(), [&](const CpuSet& a, const CpuSet& b) {
        if (a.eff != b.eff) return a.eff > b.eff;
        const bool al = a.llc == top_llc, bl = b.llc == top_llc;
        if (al != bl) return al;
        return a.index < b.index;
    });
    std::vector<unsigned> out;
    for (const CpuSet& c : v) out.push_back(c.index);
    return out;
}

void print_machine(const std::vector<CpuSet>& cs) {
    std::printf("machine\n");
    std::printf("  cpu: %s | logical processors: %zu\n", cpu_name().c_str(), cs.size());
    std::printf("  cpu sets (index: efficiency class / core / last-level cache):");
    for (size_t i = 0; i < cs.size(); ++i)
        std::printf("%s %u: %u/%u/%u", i % 8 == 0 ? "\n   " : "", cs[i].index, cs[i].eff, cs[i].core,
                    cs[i].llc);
    std::printf("\n");
#ifdef _WIN32
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof vi;
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
        auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")));
        if (fn) fn(&vi);
    }
    std::printf("  os: Windows %lu.%lu build %lu\n", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);
    std::printf("  memory: %.1f GB total, %.1f GB available at start\n",
                static_cast<double>(ms.ullTotalPhys) / (1ull << 30),
                static_cast<double>(ms.ullAvailPhys) / (1ull << 30));
    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps))
        std::printf("  power: AC line %s | battery %u%% | battery saver %s\n",
                    ps.ACLineStatus == 1 ? "online" : ps.ACLineStatus == 0 ? "offline (on battery)" : "unknown",
                    static_cast<unsigned>(ps.BatteryLifePercent), ps.SystemStatusFlag ? "on" : "off");
#endif
    std::printf("  compiler: %s | flags: %s\n", compiler_name().c_str(), LOB_BUILD_FLAGS);
    std::printf("  source: git commit %s\n", LOB_GIT_COMMIT);
}

// ============================================================================
// TSC
// ============================================================================
struct TscInfo {
    bool invariant = false;
    double cpuid_hz = 0;        // leaf 0x15 nominal TSC frequency (0: not reported)
    unsigned base_mhz = 0;      // leaf 0x16 base frequency
};

TscInfo tsc_info() {
    TscInfo t;
#if defined(__x86_64__) || defined(__i386__)
    unsigned a, b, c, d;
    if (__get_cpuid(0x80000007u, &a, &b, &c, &d)) t.invariant = (d >> 8) & 1;
    unsigned max_leaf = __get_cpuid_max(0, nullptr);
    if (max_leaf >= 0x15) {
        __cpuid_count(0x15, 0, a, b, c, d);
        if (a && b && c) t.cpuid_hz = static_cast<double>(c) * b / a;
    }
    if (max_leaf >= 0x16) {
        __cpuid_count(0x16, 0, a, b, c, d);
        t.base_mhz = a & 0xFFFF;
    }
#endif
    return t;
}

// One (tsc, steady_clock) pair: the clock read bracketed by two TSC reads,
// the TSC taken at their midpoint.
struct Stamp { uint64_t tsc; clk::time_point t; };
Stamp stamp() {
    uint64_t a = __rdtsc();
    clk::time_point t = clk::now();
    uint64_t b = __rdtsc();
    return {a + (b - a) / 2, t};
}
double ghz_between(const Stamp& s0, const Stamp& s1) {
    double ns = std::chrono::duration<double, std::nano>(s1.t - s0.t).count();
    return static_cast<double>(s1.tsc - s0.tsc) / ns;
}

double calibrate_tsc(std::vector<double>& windows) {
    for (int i = 0; i < 5; ++i) {
        Stamp s0 = stamp();
        while (clk::now() - s0.t < std::chrono::milliseconds(1000)) { /* spin */ }
        windows.push_back(ghz_between(s0, stamp()));
    }
    std::vector<double> v = windows;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void print_tsc(const TscInfo& ti, const std::vector<double>& w, double med) {
    std::printf("tsc\n");
    std::printf("  invariant TSC (CPUID 80000007h EDX bit 8): %s\n", ti.invariant ? "yes" : "no");
    if (ti.cpuid_hz > 0) std::printf("  CPUID 15h nominal TSC frequency: %.6f GHz\n", ti.cpuid_hz / 1e9);
    else                 std::printf("  CPUID 15h nominal TSC frequency: not reported\n");
    if (ti.base_mhz) std::printf("  CPUID 16h base frequency: %u MHz\n", ti.base_mhz);
    std::printf("  measured against steady_clock (QueryPerformanceCounter on Windows), five 1 s windows:");
    for (double g : w) std::printf(" %.6f", g);
    std::printf(" GHz | median %.6f GHz\n", med);
}

// ============================================================================
// Options and setup shared by every mode
// ============================================================================
struct Options {
    std::string path;
    std::string mode = "single";
    int cpu = -1;                       // -1: the first cpu of cpu_order() after cpu 0's
    std::vector<unsigned> cpus;         // multi-core: [0] main/demux, then workers
    std::vector<unsigned> workers = {1, 2, 4};
    size_t chunk = size_t{256} << 20;
    uint64_t ladder_mb = 1024;
    bool first_add = false;             // --placement first-add (see place_at_first_add)
};

struct Day {
    DayScan scan;
    std::vector<Sizing> z;
    uint64_t adds = 0, predicted_ladder_adds = 0, n_books = 0;
};

// --placement first-add: ladders placed without looking ahead. size_books()
// places each ladder on the window of prices where that symbol's adds arrived
// during the day and sizes its width from the day's price spread, which a live
// feed handler cannot know in advance. This placement uses only what is known
// when a symbol's first add arrives: every book gets the same width, the
// largest power of two whose ladders fit the budget across all books; the
// grid is one cent if the first add is at or above $1.00 and $0.0001 below;
// and the ladder is centered on the first add. Pool and id-map capacities
// still come from the pre-scan (they size memory, not where a price rests),
// and the adds the ladders cover are counted again for this placement, so the
// run still checks its overflow count against a prediction.
struct FirstAdd {
    bool     has_book = false;
    uint32_t base = 0, tick = 100, band = 64;
    uint64_t covered = 0;               // the day's adds on this ladder
};

std::vector<FirstAdd> place_at_first_add(const DayScan& d, uint64_t budget_bytes, uint32_t& band_out) {
    uint64_t n_books = 0;
    for (const SymbolScan& s : d.sym) if (s.order_msgs) ++n_books;
    uint32_t band = 64;
    while (band < (64u << kMaxK) &&
           static_cast<double>(n_books) * (2.0 * band) * kBytesPerTick <= static_cast<double>(budget_bytes))
        band *= 2;
    band_out = band;
    std::vector<FirstAdd> f(65536);
    for (int loc = 0; loc < 65536; ++loc) {
        const SymbolScan& s = d.sym[loc];
        if (!s.order_msgs) continue;
        FirstAdd& a = f[loc];
        a.has_book = true;
        a.band = band;
        if (s.prices.empty()) continue;                  // no adds: any placement will do
        const uint32_t first = s.prices.front();         // the pre-scan keeps adds in file order
        a.tick = first < 10000 ? 1 : 100;
        uint64_t start = first / a.tick;
        start = start >= band / 2 ? start - band / 2 : 0;
        const uint64_t max_start = ((uint64_t{1} << 32) - uint64_t{band} * a.tick) / a.tick;
        if (start > max_start) start = max_start;
        a.base = static_cast<uint32_t>(start * a.tick);
        for (uint32_t px : s.prices)
            if (px % a.tick == 0 && px / a.tick >= start && px / a.tick < start + band) ++a.covered;
    }
    return f;
}

void setup(const Options& o, Day& day) {
    const auto s0 = clk::now();
    prescan(o.path.c_str(), o.chunk, day.scan);
    const double scan_s = std::chrono::duration<double>(clk::now() - s0).count();
    const DayScan& d = day.scan;
    for (const SymbolScan& s : d.sym) day.adds += s.adds;
    std::vector<FirstAdd> fa;
    uint32_t fa_band = 0;
    if (o.first_add) fa = place_at_first_add(d, o.ladder_mb * 1'000'000ull, fa_band);   // before size_books sorts the prices
    double ladder_mb = 0;
    day.z = size_books(day.scan, o.ladder_mb * 1'000'000ull, ladder_mb);
    if (o.first_add) {
        uint64_t n = 0;
        for (int loc = 0; loc < 65536; ++loc) {
            if (!fa[loc].has_book) continue;
            Sizing& q = day.z[loc];
            q.base = fa[loc].base;
            q.tick = fa[loc].tick;
            q.band = fa[loc].band;
            q.ladder_adds = fa[loc].covered;
            ++n;
        }
        ladder_mb = static_cast<double>(n) * fa_band * kBytesPerTick / 1e6;
    }
    uint64_t pool_slots = 0, idmap_slots = 0;
    for (const Sizing& q : day.z) {
        if (!q.has_book) continue;
        ++day.n_books;
        day.predicted_ladder_adds += q.ladder_adds;
        pool_slots += q.pool;
        idmap_slots += uint64_t{1} << q.idmap_log2;
    }
    if (!o.first_add)
        std::printf("pre-scan and sizing (untimed, %.1f s; the same as lob_replay's)\n",
                    std::chrono::duration<double>(clk::now() - s0).count());
    else
        std::printf("pre-scan and sizing (untimed, %.1f s; ladders placed at each symbol's first add, "
                    "not lob_replay's placement)\n"
                    "  placement first-add: every ladder %s ticks, centered on the symbol's first add price, "
                    "grid $0.01 if that price is at least $1.00 and $0.0001 below; pool and id-map "
                    "capacities from the pre-scan\n",
                    std::chrono::duration<double>(clk::now() - s0).count(), num(fa_band).c_str());
    std::printf("  file %s | chunk %s bytes | ladder budget %s MB\n", o.path.c_str(), num(o.chunk).c_str(),
                num(o.ladder_mb).c_str());
    std::printf("  bytes %s | messages %s | trailing bytes %s | bad_length %s | pre-scan %.1f s\n",
                num(d.bytes).c_str(), num(d.messages).c_str(), num(d.trailing).c_str(),
                num(d.bad_length).c_str(), scan_s);
    std::printf("  by type:");
    int col = 0;
    for (int t = 0; t < 256; ++t) {
        if (!d.by_type[t]) continue;
        std::printf("%s %c %s", col++ % 9 == 0 ? "\n   " : " |", t, num(d.by_type[t]).c_str());
    }
    std::printf("\n  books %s | ladders %.0f MB | pools %s slots | id maps %s slots | adds %s, "
                "on the ladders (predicted) %s\n\n",
                num(day.n_books).c_str(), ladder_mb, num(pool_slots).c_str(), num(idmap_slots).c_str(),
                num(day.adds).c_str(), num(day.predicted_ladder_adds).c_str());
    std::fflush(stdout);
}

std::unique_ptr<ExactOrderBook> make_book(const Sizing& q) {
    if (!q.has_book) return nullptr;
    return std::make_unique<ExactOrderBook>(BookParams{q.base, q.tick, q.band, q.pool, q.idmap_log2});
}

// Counters every mode checks at the end.
struct EndState {
    uint64_t messages = 0, bad_length = 0, dropped = 0, unknown = 0, ovf_adds = 0, resting = 0;
    uint64_t chunks = 0, trailing = 0;
    Digest chain;
};

void tally_books(ExactOrderBook* const* books, EndState& e) {
    for (uint32_t loc = 0; loc < 65536; ++loc) {
        const ExactOrderBook* b = books[loc];
        if (!b) continue;
        e.dropped += b->dropped_out_of_band();
        e.unknown += b->unknown_id();
        e.ovf_adds += b->overflow_adds();
        e.resting += b->live_orders();
    }
}

bool print_end(const Day& day, const EndState& e) {
    const DayScan& d = day.scan;
    std::printf("  messages %s of %s | trailing bytes %s | bad_length %s | dropped_out_of_band %s | "
                "unknown order id %s\n",
                num(e.messages).c_str(), num(d.messages).c_str(), num(e.trailing).c_str(),
                num(e.bad_length).c_str(), num(e.dropped).c_str(), num(e.unknown).c_str());
    std::printf("  adds in the overflow %s (predicted %s) | resting orders at end of file %s\n",
                num(e.ovf_adds).c_str(), num(day.adds - day.predicted_ladder_adds).c_str(),
                num(e.resting).c_str());
    std::printf("  book digest chain over %s chunk boundaries: %s\n", num(e.chunks).c_str(),
                hex64(e.chain.h).c_str());
    const bool pass = e.messages == d.messages && e.trailing == 0 && e.bad_length == 0 &&
                      e.dropped == 0 && e.unknown == 0 &&
                      e.ovf_adds == day.adds - day.predicted_ladder_adds;
    std::printf("  checks: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

void chunk_line(uint64_t k, uint64_t msgs, uint64_t total, double secs, uint64_t digest) {
    std::printf("  chunk %3" PRIu64 " | messages %11s | through %13s | timed %8.4f s | %7.1f ns/msg | "
                "book digest %s\n",
                k, num(msgs).c_str(), num(total).c_str(), secs,
                msgs ? secs * 1e9 / static_cast<double>(msgs) : 0.0, hex64(digest).c_str());
    std::fflush(stdout);
}

void boost_process() {
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    // Keep the machine from sleeping while the run lasts.
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
#endif
}

#ifndef LOB_PERF_LATENCY
// ============================================================================
// --mode single
// ============================================================================
int run_single(const Options& o, const Day& day, unsigned cpu) {
    std::vector<std::unique_ptr<ExactOrderBook>> owned(65536);
    std::vector<ExactOrderBook*> books(65536, nullptr);
    const bool pinned = pin_current_thread(cpu);
    for (uint32_t loc = 0; loc < 65536; ++loc) {         // built on the pinned core
        owned[loc] = make_book(day.z[loc]);
        books[loc] = owned[loc].get();
    }
    std::printf("single-core throughput | thread %s cpu %u | timed: dispatch_segment() over each "
                "chunk in memory, summed\n", pinned ? "pinned to" : "NOT pinned, asked for", cpu);
    std::fflush(stdout);

    ChunkReader rd(o.path.c_str(), o.chunk);
    EndState e;
    double timed_s = 0;
    uint64_t timed_cycles = 0;
    const uint8_t* consumed = nullptr;
    const uint8_t* last = nullptr;
    const Stamp s0 = stamp();
    while (rd.next(consumed)) {
        const uint8_t* p = rd.data();
        const uint8_t* end = p + rd.size();
        const uint64_t n0 = e.messages;
        const auto t0 = clk::now();
        const uint64_t c0 = __rdtsc();
        p = dispatch_segment(p, end, ~uint64_t{0}, books.data(), e.messages, e.bad_length, last);
        const uint64_t c1 = __rdtsc();
        const auto t1 = clk::now();
        const double secs = std::chrono::duration<double>(t1 - t0).count();
        timed_s += secs;
        timed_cycles += c1 - c0;
        const uint64_t dg = books_digest(books.data());
        e.chain.add(dg);
        chunk_line(++e.chunks, e.messages - n0, e.messages, secs, dg);
        consumed = p;
    }
    const Stamp s1 = stamp();
    e.trailing = rd.trailing();
    tally_books(books.data(), e);
    const double ghz = ghz_between(s0, s1);
    std::printf("\nresult (single core, cpu %u)\n", cpu);
    std::printf("  timed dispatch %.3f s for %s messages | %.2f M msgs/s | %.2f ns/msg\n", timed_s,
                num(e.messages).c_str(), static_cast<double>(e.messages) / timed_s / 1e6,
                timed_s * 1e9 / static_cast<double>(e.messages));
    std::printf("  timed dispatch in TSC cycles %s | %.2f cycles/msg\n", num(timed_cycles).c_str(),
                static_cast<double>(timed_cycles) / static_cast<double>(e.messages));
    std::printf("  TSC over the whole pass (%.1f s): %.6f GHz\n",
                std::chrono::duration<double>(s1.t - s0.t).count(), ghz);
    return print_end(day, e) ? 0 : 1;
}

// ============================================================================
// --mode demux
// ============================================================================
int run_demux(const Options& o, const Day& day, const std::vector<unsigned>& cpus, unsigned W) {
    uint16_t max_loc = 0;
    for (uint32_t loc = 0; loc < 65536; ++loc) if (day.z[loc].has_book) max_loc = static_cast<uint16_t>(loc);
    std::vector<unsigned> use(cpus.begin(), cpus.begin() + W + 1);
    BasicParallelEngine<ExactOrderBook, itch::WireUnits> eng(
        W, max_loc, [&](uint16_t loc) { return make_book(day.z[loc]); }, use);
    std::printf("demux, W = %u workers | demux cpu %u | worker cpus", W, use[0]);
    for (unsigned w = 1; w <= W; ++w) std::printf(" %u", use[w]);
    std::printf(" | shard = stock_locate %% %u | timed: first push of a chunk to all rings drained\n", W);
    std::fflush(stdout);
    eng.start();
    std::vector<ExactOrderBook*> books(65536, nullptr);
    for (uint32_t loc = 1; loc <= max_loc; ++loc) books[loc] = eng.book(static_cast<uint16_t>(loc));

    ChunkReader rd(o.path.c_str(), o.chunk);
    EndState e;
    double timed_s = 0;
    const uint8_t* consumed = nullptr;
    uint64_t total_msgs = 0;
    while (rd.next(consumed)) {
        const uint8_t* p = rd.data();
        const size_t len = rd.size();
        uint64_t k = 0;                                    // messages in this chunk (untimed count)
        size_t used = 0;
        for (const uint8_t* q = p; q + 2 <= p + len;) {
            const uint16_t ml = static_cast<uint16_t>((q[0] << 8) | q[1]);
            if (static_cast<size_t>(p + len - (q + 2)) < ml) break;
            q += 2 + ml;
            ++k;
            used = static_cast<size_t>(q - p);
        }
        const auto t0 = clk::now();
        const size_t fed = eng.feed(p, len);
        eng.wait_drained();
        const auto t1 = clk::now();
        if (fed != used) { std::printf("  feed consumed %zu bytes, expected %zu\n", fed, used); return 1; }
        const double secs = std::chrono::duration<double>(t1 - t0).count();
        timed_s += secs;
        total_msgs += k;
        const uint64_t dg = books_digest(books.data());
        e.chain.add(dg);
        chunk_line(++e.chunks, k, total_msgs, secs, dg);
        consumed = p + fed;
    }
    eng.finish();
    e.trailing = rd.trailing();
    uint64_t lo = ~uint64_t{0}, hi = 0;
    for (unsigned w = 0; w < W; ++w) {
        e.messages += eng.processed(w);
        lo = std::min(lo, eng.processed(w));
        hi = std::max(hi, eng.processed(w));
    }
    e.messages += eng.demux_bad_length();
    e.bad_length = eng.bad_length_total();
    tally_books(books.data(), e);
    std::printf("\nresult (demux, W = %u)\n", W);
    std::printf("  timed %.3f s for %s messages | aggregate %.2f M msgs/s | %.2f ns/msg\n", timed_s,
                num(total_msgs).c_str(), static_cast<double>(total_msgs) / timed_s / 1e6,
                timed_s * 1e9 / static_cast<double>(total_msgs));
    std::printf("  messages per worker: min %s max %s (max / mean %.2f)\n", num(lo).c_str(), num(hi).c_str(),
                static_cast<double>(hi) * W / static_cast<double>(e.messages));
    return print_end(day, e) ? 0 : 1;
}

// ============================================================================
// --mode presplit
// ============================================================================
int run_presplit(const Options& o, const Day& day, const std::vector<unsigned>& cpus, unsigned W) {
    std::vector<unsigned> use(cpus.begin(), cpus.begin() + W + 1);
    std::printf("presplit, W = %u workers | main cpu %u | worker cpus", W, use[0]);
    for (unsigned w = 1; w <= W; ++w) std::printf(" %u", use[w]);
    std::printf(" | shard = stock_locate %% %u | split untimed | timed: start signal to last worker done\n", W);
    std::fflush(stdout);

    std::vector<std::unique_ptr<ExactOrderBook>> owned(65536);
    std::vector<ExactOrderBook*> books(65536, nullptr);
    std::vector<std::vector<uint8_t>> shard(W);
    struct alignas(128) WorkerStat { uint64_t n = 0, bad = 0; };
    std::vector<WorkerStat> stat(W);
    alignas(128) std::atomic<uint64_t> go{0};
    alignas(128) std::atomic<uint64_t> done{0};
    std::latch ready(static_cast<ptrdiff_t>(W) + 1);
    std::vector<std::thread> threads;
    for (unsigned w = 0; w < W; ++w) {
        threads.emplace_back([&, w] {
            pin_current_thread(use[w + 1]);
            for (uint32_t loc = 0; loc < 65536; ++loc)        // first-touch on the worker's core
                if (loc % W == w) {
                    owned[loc] = make_book(day.z[loc]);
                    books[loc] = owned[loc].get();
                }
            ready.arrive_and_wait();
            uint64_t seen = 0;
            const uint8_t* last = nullptr;
            for (;;) {
                uint64_t g;
                while ((g = go.load(std::memory_order_acquire)) == seen) _mm_pause();
                if (g == ~uint64_t{0}) break;
                seen = g;
                const uint8_t* p = shard[w].data();
                dispatch_segment(p, p + shard[w].size(), ~uint64_t{0}, books.data(), stat[w].n, stat[w].bad, last);
                done.fetch_add(1, std::memory_order_acq_rel);
            }
        });
    }
    pin_current_thread(use[0]);
    ready.arrive_and_wait();

    ChunkReader rd(o.path.c_str(), o.chunk);
    EndState e;
    double timed_s = 0;
    const uint8_t* consumed = nullptr;
    uint64_t gen = 0, total_msgs = 0;
    while (rd.next(consumed)) {
        const uint8_t* p = rd.data();
        const uint8_t* end = p + rd.size();
        for (auto& s : shard) s.clear();
        uint64_t k = 0;
        while (p + 2 <= end) {                              // split, untimed
            const uint16_t ml = static_cast<uint16_t>((p[0] << 8) | p[1]);
            const uint8_t* m = p + 2;
            if (static_cast<size_t>(end - m) < ml) break;
            if (ml >= 3) {
                auto& s = shard[((m[1] << 8) | m[2]) % W];
                s.insert(s.end(), p, m + ml);
            } else {
                ++e.bad_length;
            }
            p = m + ml;
            ++k;
        }
        ++gen;
        const auto t0 = clk::now();
        go.store(gen, std::memory_order_release);
        while (done.load(std::memory_order_acquire) != gen * W) _mm_pause();
        const auto t1 = clk::now();
        const double secs = std::chrono::duration<double>(t1 - t0).count();
        timed_s += secs;
        total_msgs += k;
        const uint64_t dg = books_digest(books.data());
        e.chain.add(dg);
        chunk_line(++e.chunks, k, total_msgs, secs, dg);
        consumed = p;
    }
    go.store(~uint64_t{0}, std::memory_order_release);
    for (auto& t : threads) t.join();
    e.trailing = rd.trailing();
    uint64_t lo = ~uint64_t{0}, hi = 0;
    for (unsigned w = 0; w < W; ++w) {
        e.messages += stat[w].n;
        e.bad_length += stat[w].bad;
        lo = std::min(lo, stat[w].n);
        hi = std::max(hi, stat[w].n);
    }
    tally_books(books.data(), e);
    std::printf("\nresult (presplit, W = %u)\n", W);
    std::printf("  timed %.3f s for %s messages | aggregate %.2f M msgs/s | %.2f ns/msg\n", timed_s,
                num(total_msgs).c_str(), static_cast<double>(total_msgs) / timed_s / 1e6,
                timed_s * 1e9 / static_cast<double>(total_msgs));
    std::printf("  messages per worker: min %s max %s (max / mean %.2f)\n", num(lo).c_str(), num(hi).c_str(),
                static_cast<double>(hi) * W / static_cast<double>(e.messages));
    return print_end(day, e) ? 0 : 1;
}
#endif  // !LOB_PERF_LATENCY

#ifdef LOB_PERF_LATENCY
// ============================================================================
// --mode latency (lob_perf_latency build only)
// ============================================================================
constexpr int kSlots = 8;                                   // A F E C X D U, other
constexpr const char* kSlotName[kSlots] = {"A", "F", "E", "C", "X", "D", "U", "other"};
constexpr uint32_t kHistMax = 1u << 16;                     // exact buckets 0 .. 65535 cycles

struct Hist {
    std::vector<uint32_t> bucket = std::vector<uint32_t>(kHistMax, 0);
    std::vector<uint64_t> big;                             // exact samples >= kHistMax
    uint64_t n = 0;
    long double sum = 0;
    LOB_FORCE_INLINE void add(uint64_t c) {
        if (LOB_LIKELY(c < kHistMax)) ++bucket[c];
        else big.push_back(c);
    }
    void merge(const Hist& h) {
        for (uint32_t i = 0; i < kHistMax; ++i) bucket[i] += h.bucket[i];
        big.insert(big.end(), h.big.begin(), h.big.end());
    }
    void finish() {
        std::sort(big.begin(), big.end());
        n = big.size();
        sum = 0;
        for (uint32_t i = 0; i < kHistMax; ++i) { n += bucket[i]; sum += static_cast<long double>(bucket[i]) * i; }
        for (uint64_t v : big) sum += v;
    }
    // Nearest rank: the smallest value v with at least ceil(q * n) samples <= v.
    uint64_t quantile(double q) const {
        uint64_t rank = static_cast<uint64_t>(std::ceil(q * static_cast<double>(n)));
        if (rank < 1) rank = 1;
        uint64_t acc = 0;
        for (uint32_t i = 0; i < kHistMax; ++i) {
            acc += bucket[i];
            if (acc >= rank) return i;
        }
        return big[rank - acc - 1];
    }
    uint64_t min() const { return quantile(0); }
    uint64_t max() const {
        if (!big.empty()) return big.back();
        for (uint32_t i = kHistMax; i-- > 0;) if (bucket[i]) return i;
        return 0;
    }
    // Mean of max(sample - ovh, 0), and the number of samples <= ovh.
    long double mean_minus(uint64_t ovh) const {
        long double s = 0;
        for (uint32_t i = 0; i < kHistMax; ++i) if (i > ovh) s += static_cast<long double>(bucket[i]) * (i - ovh);
        for (uint64_t v : big) if (v > ovh) s += v - ovh;
        return n ? s / n : 0;
    }
    uint64_t at_most(uint64_t ovh) const {
        uint64_t c = 0;
        for (uint32_t i = 0; i < kHistMax && i <= ovh; ++i) c += bucket[i];
        for (uint64_t v : big) if (v <= ovh) ++c;
        return c;
    }
};

uint64_t timer_overhead(std::vector<uint64_t>& samples) {
    uint64_t best = ~0ull;
    for (int i = 0; i < 100'000; ++i) {
        uint64_t t0 = rdtsc_begin();
        uint64_t t1 = rdtsc_end();
        samples.push_back(t1 - t0);
        best = std::min(best, t1 - t0);
    }
    std::sort(samples.begin(), samples.end());
    return best;
}

// The TSC ticks at a fixed rate while the core's clock follows the power
// plan, so an empty timer pair costs about twice as many TSC cycles on a core
// running at half its clock. A calibration taken just after the pre-scan's
// file reads can catch the core before it has clocked up. So the pinned core
// first spins for 0.5 s, the pair is measured then and again after the pass,
// and the smaller of the two minimums is subtracted from every sample: it can
// under-subtract relative to the fastest state seen, never over-subtract.
void busy_spin(double seconds) {
    const auto t0 = clk::now();
    while (std::chrono::duration<double>(clk::now() - t0).count() < seconds) { /* spin */ }
}

int run_latency(const Options& o, const Day& day, unsigned cpu) {
    std::vector<std::unique_ptr<ExactOrderBook>> owned(65536);
    std::vector<ExactOrderBook*> books(65536, nullptr);
    const bool pinned = pin_current_thread(cpu);
    for (uint32_t loc = 0; loc < 65536; ++loc) {
        owned[loc] = make_book(day.z[loc]);
        books[loc] = owned[loc].get();
    }
    uint8_t slot_of[256];
    for (int t = 0; t < 256; ++t) slot_of[t] = kSlots - 1;
    const char types[] = "AFECXDU";
    for (int s = 0; s < 7; ++s) slot_of[static_cast<uint8_t>(types[s])] = static_cast<uint8_t>(s);

    std::vector<uint64_t> pre_samples;
    busy_spin(0.5);
    const uint64_t pre = timer_overhead(pre_samples);
    std::printf("per-message latency | thread %s cpu %u | every message timed\n",
                pinned ? "pinned to" : "NOT pinned, asked for", cpu);
    std::printf("  timed region per message: the book lookup by stock_locate and "
                "itch::dispatch_checked<WireUnits>(), between rdtsc_begin() (lfence; rdtsc) and "
                "rdtsc_end() (rdtscp; lfence)\n");
    std::printf("  empty timer pair, 100,000 samples: min %" PRIu64 " | p50 %" PRIu64 " | p99 %" PRIu64
                " cycles (before the pass, after 0.5 s of busy spinning; measured again after the pass)\n",
                pre, pre_samples[pre_samples.size() / 2], pre_samples[pre_samples.size() * 99 / 100]);
    std::fflush(stdout);

    std::vector<Hist> hist(kSlots);
    ChunkReader rd(o.path.c_str(), o.chunk);
    EndState e;
    const uint8_t* consumed = nullptr;
    const Stamp s0 = stamp();
    while (rd.next(consumed)) {
        const uint8_t* p = rd.data();
        const uint8_t* end = p + rd.size();
        const uint64_t n0 = e.messages;
        const auto t0 = clk::now();
        while (p + 2 <= end) {
            const uint16_t len = static_cast<uint16_t>((p[0] << 8) | p[1]);
            const uint8_t* m = p + 2;
            if (LOB_UNLIKELY(static_cast<size_t>(end - m) < len)) break;
            const uint64_t c0 = rdtsc_begin();
            if (LOB_LIKELY(len >= 3)) {
                ExactOrderBook* b = books[(m[1] << 8) | m[2]];
                if (b) itch::dispatch_checked<itch::WireUnits>(*b, m, len, e.bad_length);
            } else {
                ++e.bad_length;
            }
            const uint64_t c1 = rdtsc_end();
            hist[slot_of[len ? m[0] : 0]].add(c1 - c0);  // raw; the overhead is subtracted at the end
            p = m + len;
            ++e.messages;
        }
        const auto t1 = clk::now();
        const uint64_t dg = books_digest(books.data());
        e.chain.add(dg);
        chunk_line(++e.chunks, e.messages - n0, e.messages,
                   std::chrono::duration<double>(t1 - t0).count(), dg);
        consumed = p;
    }
    const Stamp s1 = stamp();
    e.trailing = rd.trailing();
    tally_books(books.data(), e);
    const double ghz = ghz_between(s0, s1);
    std::vector<uint64_t> post_samples;
    const uint64_t post = timer_overhead(post_samples);
    const uint64_t ovh = std::min(pre, post);

    Hist order, all;
    for (int s = 0; s < 7; ++s) order.merge(hist[s]);
    all.merge(order);
    all.merge(hist[kSlots - 1]);
    for (Hist& h : hist) h.finish();
    order.finish();
    all.finish();
    std::printf("\nresult (per-message latency, cpu %u; chunk times above include the timers and are "
                "not throughput)\n", cpu);
    std::printf("  TSC over the whole pass (%.1f s): %.6f GHz; ns = cycles / %.6f\n",
                std::chrono::duration<double>(s1.t - s0.t).count(), ghz, ghz);
    std::printf("  empty timer pair after the pass, 100,000 samples: min %" PRIu64 " | p50 %" PRIu64
                " | p99 %" PRIu64 " cycles; the smaller min (%" PRIu64 ", of %" PRIu64 " before and %" PRIu64
                " after) is subtracted from every sample, clamped at 0\n",
                post, post_samples[post_samples.size() / 2], post_samples[post_samples.size() * 99 / 100],
                ovh, pre, post);
    std::printf("  samples clamped to 0 after overhead subtraction: %s\n", num(all.at_most(ovh)).c_str());
    std::printf("  percentiles are nearest-rank over every sample\n");
    auto row = [&](const char* name, const Hist& h, bool ns) {
        auto f = [&](uint64_t raw) {
            const uint64_t c = raw > ovh ? raw - ovh : 0;
            return ns ? static_cast<double>(c) / ghz : static_cast<double>(c);
        };
        const double mean = static_cast<double>(h.mean_minus(ovh));
        std::printf("  %-6s %12s | %8.1f | %7.0f | %7.0f | %7.0f | %7.0f | %8.0f | %8.0f | %10.0f\n", name,
                    num(h.n).c_str(), ns ? mean / ghz : mean,
                    f(h.min()), f(h.quantile(0.50)), f(h.quantile(0.90)), f(h.quantile(0.99)),
                    f(h.quantile(0.999)), f(h.quantile(0.9999)), f(h.max()));
    };
    for (int unit = 0; unit < 2; ++unit) {
        std::printf("\n  %s\n", unit == 0 ? "TSC cycles" : "nanoseconds");
        std::printf("  %-6s %12s | %8s | %7s | %7s | %7s | %7s | %8s | %8s | %10s\n", "type", "messages",
                    "mean", "min", "p50", "p90", "p99", "p99.9", "p99.99", "max");
        for (int s = 0; s < kSlots; ++s) row(kSlotName[s], hist[s], unit == 1);
        row("orders", order, unit == 1);
        row("all", all, unit == 1);
    }
    std::printf("\n");
    return print_end(day, e) ? 0 : 1;
}
#endif  // LOB_PERF_LATENCY

std::vector<unsigned> parse_list(const std::string& s) {
    std::vector<unsigned> v;
    for (const std::string& x : split(s, ',')) v.push_back(static_cast<unsigned>(std::strtoul(x.c_str(), nullptr, 10)));
    return v;
}

} // namespace

int main(int argc, char** argv) {
    Options o;
#ifdef LOB_PERF_LATENCY
    o.mode = "latency";
#endif
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) { std::printf("%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--mode") o.mode = val();
        else if (a == "--cpu") o.cpu = std::atoi(val().c_str());
        else if (a == "--cpus") o.cpus = parse_list(val());
        else if (a == "--workers") o.workers = parse_list(val());
        else if (a == "--chunk-mb") o.chunk = size_t{std::max<uint64_t>(1, std::strtoull(val().c_str(), nullptr, 10))} << 20;
        else if (a == "--ladder-mb") o.ladder_mb = std::strtoull(val().c_str(), nullptr, 10);
        else if (a == "--placement") {
            const std::string p = val();
            if (p != "prescan" && p != "first-add") { std::printf("--placement: prescan or first-add\n"); return 2; }
            o.first_add = p == "first-add";
        }
        else if (!a.empty() && a[0] != '-' && o.path.empty()) o.path = a;
        else {
            std::printf("usage: %s FILE [--mode single|demux|presplit|latency] [--cpu N] [--cpus a,b,...] "
                        "[--workers 1,2,4] [--chunk-mb N] [--ladder-mb N] [--placement prescan|first-add]\n",
                        argv[0]);
            return 2;
        }
    }
    if (o.path.empty()) { std::printf("no input file\n"); return 2; }
#ifdef LOB_PERF_LATENCY
    if (o.mode != "latency") { std::printf("this build (lob_perf_latency) runs --mode latency only\n"); return 2; }
#else
    if (o.mode != "single" && o.mode != "demux" && o.mode != "presplit") {
        std::printf("unknown --mode %s (latency is the lob_perf_latency build)\n", o.mode.c_str());
        return 2;
    }
#endif

    std::time_t now = std::time(nullptr);
    char when[64];
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::string cmd;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        if (i == 0) a = a.substr(a.find_last_of("/\\") + 1);   // program name without its directory
        cmd += (i ? " " : "") + a;
    }
    std::printf("=== lob_perf: NASDAQ TotalView-ITCH 5.0 recorded day, mode %s ===\n", o.mode.c_str());
    std::printf("run: %s local | command: %s\n", when, cmd.c_str());
    const std::vector<CpuSet> cs = cpu_sets();
    const std::vector<unsigned> order = cpu_order(cs);
    print_machine(cs);
    boost_process();

    unsigned cpu = 0;
    if (o.cpu >= 0) cpu = static_cast<unsigned>(o.cpu);
    else {
        cpu = order[0];
        for (unsigned c : order) if (c != 0 && c < cs.size() && cs[c].eff == cs[order[0]].eff) { cpu = c; break; }
    }
    std::vector<unsigned> cpus = o.cpus.empty() ? order : o.cpus;
    std::printf("  cpu order, fastest type first:");
    for (unsigned c : order) std::printf(" %u", c);
    std::printf("\n");

    pin_current_thread(o.mode == "single" || o.mode == "latency" ? cpu : cpus[0]);
    std::vector<double> windows;
    const double med = calibrate_tsc(windows);
    print_tsc(tsc_info(), windows, med);
    std::printf("\n");
    std::fflush(stdout);

    Day day;
    setup(o, day);
    const auto w0 = clk::now();
    int rc = 0;
#ifdef LOB_PERF_LATENCY
    rc = run_latency(o, day, cpu);
#else
    if (o.mode == "single") {
        rc = run_single(o, day, cpu);
    } else {
        for (unsigned W : o.workers) {
            if (W == 0 || W + 1 > cpus.size()) {
                std::printf("W = %u needs %u cpus, have %zu: skipped\n\n", W, W + 1, cpus.size());
                rc = 1;
                continue;
            }
            const int r = o.mode == "demux" ? run_demux(o, day, cpus, W) : run_presplit(o, day, cpus, W);
            rc = rc ? rc : r;
            std::printf("\n");
        }
    }
#endif
    std::printf("wall time after setup %.0f s\n", std::chrono::duration<double>(clk::now() - w0).count());
    std::printf("\nRESULT: %s\n", rc == 0 ? "PASS" : "FAIL");
    return rc;
}
