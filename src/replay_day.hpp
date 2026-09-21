#pragma once
// ---------------------------------------------------------------------------
// replay_day.hpp - the parts of the recorded-day tools that lob_replay
// (src/replay_main.cpp) and lob_perf (src/perf_main.cpp) share: formatting
// helpers, the chunked file reader, the untimed pre-scan (pass 1) and the
// per-locate book sizing derived from it. Both tools therefore build exactly
// the same books from the same file.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
  #include <cpuid.h>
#endif

#include "lob/book.hpp"
#include "lob/itch.hpp"

#include "ref_market.hpp"

namespace day {

using namespace lob;

// ============================================================================
// Small helpers
// ============================================================================
inline std::string num(uint64_t v) {                       // 368366634 -> "368,366,634"
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

inline std::string dollars(uint32_t wire) {                // 1652500 -> "165.2500"
    char b[32];
    std::snprintf(b, sizeof b, "%" PRIu32 ".%04" PRIu32, wire / 10000, wire % 10000);
    return b;
}

inline std::string clock_of(uint64_t ns) {                 // ns since midnight -> "HH:MM:SS.mmm"
    char b[32];
    uint64_t s = ns / 1'000'000'000ull;
    std::snprintf(b, sizeof b, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ".%03" PRIu64,
                  s / 3600, (s / 60) % 60, s % 60, (ns / 1'000'000ull) % 1000);
    return b;
}

inline bool parse_clock(const std::string& t, uint64_t& ns) {   // "HH:MM:SS" -> ns since midnight
    unsigned h = 0, m = 0, s = 0;
    if (std::sscanf(t.c_str(), "%u:%u:%u", &h, &m, &s) != 3 || h > 23 || m > 59 || s > 59) return false;
    ns = ((uint64_t{h} * 60 + m) * 60 + s) * 1'000'000'000ull;
    return true;
}

inline std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(sep, a);
        if (b == std::string::npos) b = s.size();
        if (b > a) out.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

inline std::string cpu_name() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned r[12] = {};
    if (__get_cpuid(0x80000000u, &r[0], &r[1], &r[2], &r[3]) && r[0] >= 0x80000004u) {
        for (unsigned i = 0; i < 3; ++i)
            __get_cpuid(0x80000002u + i, &r[4 * i], &r[4 * i + 1], &r[4 * i + 2], &r[4 * i + 3]);
        std::string s(reinterpret_cast<const char*>(r), 48);
        s = s.c_str();
        size_t a = s.find_first_not_of(' ');
        return a == std::string::npos ? "unknown" : s.substr(a);
    }
#endif
    return "unknown";
}

inline std::string compiler_name() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("g++ ") + __VERSION__;
#else
    return "unknown";
#endif
}

// ============================================================================
// Chunked reader: a message that straddles two chunks is carried over.
// ============================================================================
class ChunkReader {
public:
    ChunkReader(const char* path, size_t chunk) : chunk_(chunk), buf_(chunk + 70'000) {
        f_ = std::fopen(path, "rb");
    }
    ~ChunkReader() { if (f_) std::fclose(f_); }
    bool ok() const { return f_ != nullptr; }

    // Refills the buffer: the unconsumed tail of the previous chunk (from
    // `consumed` on) is moved to the front, then up to `chunk` new bytes are
    // read after it. Returns false once the file is exhausted and nothing is
    // left. [data(), data() + size()) is valid until the next call.
    bool next(const uint8_t* consumed) {
        size_t carry = 0;
        if (consumed) {
            carry = static_cast<size_t>((buf_.data() + len_) - consumed);
            std::memmove(buf_.data(), consumed, carry);
        }
        size_t n = std::fread(buf_.data() + carry, 1, chunk_, f_);
        bytes_ += n;
        len_ = carry + n;
        if (n == 0) { tail_ = carry; return false; }
        return true;
    }
    const uint8_t* data() const { return buf_.data(); }
    size_t   size()  const { return len_; }
    uint64_t bytes() const { return bytes_; }
    size_t   trailing() const { return tail_; }   // bytes left that never formed a message

private:
    FILE* f_ = nullptr;
    size_t chunk_;
    std::vector<uint8_t> buf_;
    size_t len_ = 0, tail_ = 0;
    uint64_t bytes_ = 0;
};

// ============================================================================
// Pass 1: pre-scan
// ============================================================================
struct Cross { bool seen = false; uint32_t price = 0; uint64_t shares = 0; uint64_t ts = 0; };

struct SymbolScan {
    std::string name;
    uint32_t directory_msgs = 0;
    uint64_t order_msgs = 0;
    uint64_t adds = 0;                  // A, F and the add half of U
    uint64_t live = 0, peak_live = 0;
    std::vector<uint32_t> prices;       // every add price, for sizing
    Cross open_cross, close_cross;
};

struct DayScan {
    uint64_t bytes = 0, messages = 0, bad_length = 0, unknown_type = 0, trailing = 0;
    uint64_t by_type[256] = {};
    uint64_t directory_repeats = 0;     // an 'R' for a locate that already had one
    uint64_t directory_renamed = 0;     // ... that also changed the locate's symbol
    uint64_t ts_backwards = 0;          // a timestamp earlier than the previous one
    uint64_t live = 0, peak_live = 0;
    uint64_t subpenny_adds = 0;         // add prices that are not whole cents
    uint64_t outside_cent_band = 0;     // add prices outside $0.01 .. $1310.72
    uint64_t first_ts = 0, last_ts = 0;
    std::vector<uint64_t> at_ns;        // requested checkpoint times
    std::vector<uint64_t> at_index;     // messages with timestamp < at_ns[i]
    std::vector<SymbolScan> sym = std::vector<SymbolScan>(65536);
};

inline void prescan(const char* path, size_t chunk, DayScan& d) {
    ChunkReader rd(path, chunk);
    if (!rd.ok()) { std::printf("cannot open %s\n", path); std::exit(2); }
    struct Live { uint16_t locate; uint32_t shares; };
    std::unordered_map<uint64_t, Live> live;
    live.reserve(1u << 22);
    size_t next_at = 0;
    d.at_index.assign(d.at_ns.size(), 0);
    const uint8_t* consumed = nullptr;
    bool first = true;
    while (rd.next(consumed)) {
        const uint8_t* p = rd.data();
        const uint8_t* end = p + rd.size();
        while (p + 2 <= end) {
            const uint16_t len = static_cast<uint16_t>((p[0] << 8) | p[1]);
            const uint8_t* m = p + 2;
            if (static_cast<size_t>(end - m) < len) break;
            p = m + len;
            ++d.messages;
            if (len == 0) { ++d.bad_length; continue; }
            const uint8_t t = m[0];
            ++d.by_type[t];
            const uint16_t want = itch::spec_len(t);
            if (want == 0) { ++d.unknown_type; continue; }
            if (want != len) { ++d.bad_length; continue; }
            const uint64_t ts = itch::timestamp_ns(m);
            if (first) { d.first_ts = ts; first = false; }
            if (ts < d.last_ts) ++d.ts_backwards;
            d.last_ts = ts;
            while (next_at < d.at_ns.size() && ts >= d.at_ns[next_at])
                d.at_index[next_at++] = d.messages - 1;
            const uint16_t loc = ref::get16(m + 1);
            SymbolScan& s = d.sym[loc];
            auto add_price = [&](uint32_t px) {
                ++s.adds;
                s.prices.push_back(px);
                if (px % 100 != 0) ++d.subpenny_adds;
                if (px < 100 || px / 100 > 131'072) ++d.outside_cent_band;
            };
            auto gone = [&](std::unordered_map<uint64_t, Live>::iterator it) {
                --d.sym[it->second.locate].live;
                --d.live;
                live.erase(it);
            };
            switch (t) {
            case 'R': {
                std::string name(reinterpret_cast<const char*>(m + 11), 8);
                while (!name.empty() && name.back() == ' ') name.pop_back();
                if (s.directory_msgs++ > 0) {
                    ++d.directory_repeats;
                    if (name != s.name) ++d.directory_renamed;
                }
                s.name = name;
                break;
            }
            case 'Q': {
                Cross& c = m[39] == 'O' ? s.open_cross : s.close_cross;
                if (m[39] == 'O' || m[39] == 'C') {
                    c.seen = true;
                    c.shares = ref::get64(m + 11);
                    c.price = ref::get32(m + 27);
                    c.ts = ts;
                }
                break;
            }
            case 'A':
            case 'F': {
                ++s.order_msgs;
                const uint32_t px = ref::get32(m + 32);
                add_price(px);
                if (live.emplace(ref::get64(m + 11), Live{loc, ref::get32(m + 20)}).second) {
                    if (++s.live > s.peak_live) s.peak_live = s.live;
                    if (++d.live > d.peak_live) d.peak_live = d.live;
                }
                break;
            }
            case 'E':
            case 'C':
            case 'X': {
                ++s.order_msgs;
                auto it = live.find(ref::get64(m + 11));
                if (it == live.end()) break;
                const uint32_t q = ref::get32(m + 19);
                if (q >= it->second.shares) gone(it);
                else it->second.shares -= q;
                break;
            }
            case 'D': {
                ++s.order_msgs;
                auto it = live.find(ref::get64(m + 11));
                if (it != live.end()) gone(it);
                break;
            }
            case 'U': {
                ++s.order_msgs;
                auto it = live.find(ref::get64(m + 11));
                const uint32_t px = ref::get32(m + 31);
                add_price(px);
                if (it == live.end()) break;
                const uint16_t own = it->second.locate;
                gone(it);
                if (live.emplace(ref::get64(m + 19), Live{own, ref::get32(m + 27)}).second) {
                    SymbolScan& o = d.sym[own];
                    if (++o.live > o.peak_live) o.peak_live = o.live;
                    if (++d.live > d.peak_live) d.peak_live = d.live;
                }
                break;
            }
            default:
                break;
            }
        }
        consumed = p;
    }
    while (next_at < d.at_ns.size()) d.at_index[next_at++] = d.messages;
    d.bytes = rd.bytes();
    d.trailing = rd.trailing();
}

// ============================================================================
// Sizing: one ExactOrderBook per stock_locate that carries order messages.
//
// Tick. A book's grid is one cent (100 wire units) when the median of the
// symbol's add prices is at or above $1.00, and the wire resolution itself
// ($0.0001, 1 unit) below that. Reg NMS Rule 612 bars displayed quotes in
// sub-penny increments at or above $1.00, so a one-cent grid holds every
// level of such a symbol exactly; below $1.00, $0.0001 increments are legal,
// so those symbols get a grid that can hold them. Whatever the grid, no price
// is rounded: a price that is not on it (a sub-penny price below $1.00 in a
// stock that trades above it, say) rests in the exact overflow instead.
//
// Band. Ladder width is 64 << k ticks (k = 0..12, so 64 .. 262,144 ticks).
// For every symbol and every k, the pre-scan knows how many of the day's add
// prices the best-placed window of that width covers. Starting from 64 ticks
// each, widths are doubled greedily, always for the symbol whose next
// doubling covers the most additional adds per additional tick, until the
// ladder memory budget (--ladder-mb, 1024 MB by default) is spent. Each book's
// base is then the start of its best window. This puts the flat ladder where
// the day's orders actually arrive, which is what keeps the hot path covering
// the trading; the adds it does not cover are counted and reported, and they
// are exact in the overflow.
//
// Why a pre-scan and not a self-sizing book: the pool and id map have a fixed
// capacity by design (no allocation on the hot path), and a symbol's peak
// resting-order count is only known from the data; the add-price distribution
// is likewise what places the band. Both are sizes only. The overflow makes
// the reconstructed books independent of them: a different band or tick
// changes which structure a price rests in, never the book.
// ============================================================================
struct Sizing {
    bool     has_book = false;
    uint32_t base = 0, tick = 100, band = 64;
    uint32_t pool = 0;
    unsigned idmap_log2 = 4;
    uint64_t ladder_adds = 0;           // adds the chosen window covers
    uint64_t on_grid = 0;               // adds on the chosen grid
};

constexpr int kMaxK = 12;               // widths 64 << 0 .. 64 << 12
constexpr double kBytesPerTick = 2 * sizeof(PriceLevel) + 2.0 / 8.0;   // both sides + bitmap

inline unsigned ceil_log2(uint64_t v) {
    unsigned k = 0;
    while ((uint64_t{1} << k) < v) ++k;
    return k;
}

inline std::vector<Sizing> size_books(DayScan& d, uint64_t ladder_budget_bytes, double& ladder_mb_out) {
    std::vector<Sizing> z(65536);
    struct Cov { uint64_t c[kMaxK + 1]; };
    std::vector<Cov> cov(65536);
    std::vector<std::vector<uint32_t>> grid(65536);
    for (int loc = 0; loc < 65536; ++loc) {
        SymbolScan& s = d.sym[loc];
        if (s.order_msgs == 0) continue;
        Sizing& q = z[loc];
        q.has_book = true;
        const uint64_t cap = s.peak_live + s.peak_live / 8 + 16;
        q.pool = static_cast<uint32_t>(cap);
        q.idmap_log2 = std::max(4u, ceil_log2(2 * cap));
        std::vector<uint32_t>& v = s.prices;
        std::sort(v.begin(), v.end());
        const uint32_t median = v.empty() ? 10000 : v[v.size() / 2];
        q.tick = median < 10000 ? 1 : 100;
        std::vector<uint32_t>& t = grid[loc];
        t.reserve(v.size());
        for (uint32_t px : v) if (px % q.tick == 0) t.push_back(px / q.tick);
        q.on_grid = t.size();
        std::vector<uint32_t>().swap(v);
        for (int k = 0; k <= kMaxK; ++k) {
            const uint64_t B = uint64_t{64} << k;
            uint64_t best = 0;
            size_t j = 0;
            for (size_t i = 0; i < t.size(); ++i) {
                if (j < i) j = i;
                while (j < t.size() && t[j] < uint64_t{t[i]} + B) ++j;
                best = std::max<uint64_t>(best, j - i);
            }
            cov[loc].c[k] = best;
        }
    }
    // Greedy doubling under the budget.
    std::vector<int> k(65536, 0);
    double spent = 0;
    for (int loc = 0; loc < 65536; ++loc) if (z[loc].has_book) spent += 64 * kBytesPerTick;
    using Item = std::pair<double, int>;             // (adds gained per tick, locate)
    std::priority_queue<Item> pq;
    auto push = [&](int loc) {
        if (k[loc] >= kMaxK) return;
        double gain = static_cast<double>(cov[loc].c[k[loc] + 1] - cov[loc].c[k[loc]]);
        if (gain > 0) pq.push({gain / static_cast<double>(uint64_t{64} << k[loc]), loc});
    };
    for (int loc = 0; loc < 65536; ++loc) if (z[loc].has_book) push(loc);
    while (!pq.empty()) {
        const int loc = pq.top().second;
        pq.pop();
        const double extra = static_cast<double>(uint64_t{64} << k[loc]) * kBytesPerTick;
        if (spent + extra > static_cast<double>(ladder_budget_bytes)) continue;
        spent += extra;
        ++k[loc];
        push(loc);
    }
    ladder_mb_out = spent / 1e6;
    // Place each ladder on its best window.
    for (int loc = 0; loc < 65536; ++loc) {
        Sizing& q = z[loc];
        if (!q.has_book) continue;
        const uint64_t B = uint64_t{64} << k[loc];
        const std::vector<uint32_t>& t = grid[loc];
        uint64_t best = 0, start = t.empty() ? 0 : t[0];
        size_t j = 0;
        for (size_t i = 0; i < t.size(); ++i) {
            if (j < i) j = i;
            while (j < t.size() && t[j] < uint64_t{t[i]} + B) ++j;
            if (j - i > best) { best = j - i; start = t[i]; }
        }
        const uint64_t max_start = ((uint64_t{1} << 32) - B * q.tick) / q.tick;   // keep base + band * tick <= 2^32
        if (start > max_start) start = max_start;
        q.band = static_cast<uint32_t>(B);
        q.base = static_cast<uint32_t>(start * q.tick);
        // Adds the placed ladder covers (equal to the window count unless the
        // clamp above moved it): on-grid prices in [start, start + B) ticks.
        auto lo = std::lower_bound(t.begin(), t.end(), start);
        auto hi = std::lower_bound(t.begin(), t.end(), start + B);
        q.ladder_adds = static_cast<uint64_t>(hi - lo);
    }
    return z;
}

// ============================================================================
// Pass 2 building blocks
// ============================================================================
// The timed loop: at most max_msgs framed messages from [p, end) through the
// benchmark dispatch into the book of each message's stock_locate. Stops early
// at a message that is not wholly inside the chunk. Returns the first byte not
// consumed; `last` receives the start of the last message processed.
LOB_FORCE_INLINE const uint8_t* dispatch_segment(const uint8_t* p, const uint8_t* end,
                                                 uint64_t max_msgs, ExactOrderBook* const* books,
                                                 uint64_t& n, uint64_t& bad_length,
                                                 const uint8_t*& last) {
    uint64_t k = 0;
    while (k < max_msgs && p + 2 <= end) {
        const uint16_t len = static_cast<uint16_t>((p[0] << 8) | p[1]);
        const uint8_t* m = p + 2;
        if (LOB_UNLIKELY(static_cast<size_t>(end - m) < len)) break;
        if (LOB_LIKELY(len >= 3)) {
            ExactOrderBook* b = books[(m[1] << 8) | m[2]];
            if (b) itch::dispatch_checked<itch::WireUnits>(*b, m, len, bad_length);
        } else {
            ++bad_length;
        }
        last = m;
        p = m + len;
        ++k;
    }
    n += k;
    return p;
}

// A 64-bit digest of the full state of every book: for each locate with a
// book, its resting-order count, then both sides best price first, each
// level's price, shares and order count, and the level's queue in FIFO order
// (every order's reference number and remaining shares). Two sets of books
// with the same digest hold, with overwhelming probability, the same state
// down to queue position. The replay and perf tools print it after every
// chunk, so runs in different modes (single-core, sharded, instrumented, and
// the differential run checked against the reference) can be compared.
struct Digest {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    LOB_FORCE_INLINE void add(uint64_t v) {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 31;
    }
};

inline uint64_t books_digest(ExactOrderBook* const* books) {
    Digest d;
    for (uint32_t loc = 0; loc < 65536; ++loc) {
        const ExactOrderBook* b = books[loc];
        if (!b) continue;
        d.add(loc);
        d.add(b->live_orders());
        for (Side side : {Side::Bid, Side::Ask}) {
            d.add(side == Side::Bid ? 0xB1Dull : 0xA5Cull);
            b->for_each_level(side, [&](Price px, const PriceLevel& L) {
                d.add(px);
                d.add(L.total_qty);
                d.add(L.count);
                for (uint32_t oi = L.head; oi != NIL; oi = b->order_at(oi).next) {
                    d.add(b->order_at(oi).id);
                    d.add(b->order_at(oi).qty);
                }
            });
        }
    }
    return d.h;
}

inline std::string hex64(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016" PRIx64, v);
    return b;
}

} // namespace day
