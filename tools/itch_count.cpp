// itch_count.cpp - independent recount of a NASDAQ ITCH 5.0 BinaryFILE.
//
// Written separately from include/lob and src/: it includes nothing from the
// repo and decodes every field by byte offset from the ITCH 5.0 specification.
// It exists to check lob_replay's figures with code that shares none of its
// parsing.
//
// What it reports:
//   1. Framing: total bytes, messages, bytes in length prefixes, and whether
//      the file ends exactly on a message boundary. Every message's length
//      prefix is also compared against the ITCH 5.0 length for its type.
//   2. Message counts by type byte, and timestamp order.
//   3. Distinct stock_locate values in Add Order ('A'), Add Order with MPID
//      ('F'), both, and in any order message; the Stock Directory ('R') count.
//   4. For a fixed list of symbols: every Cross Trade ('Q') with its type,
//      price, shares and time; the last closing-cross NOII ('I') before the
//      closing cross; non-printable Order Executed With Price ('C') shares
//      by price from 16:00:00 to 16:01:00; the order messages between
//      16:00:00.000 and the closing cross; and a small independent order
//      book (std::map per side) whose top levels are printed at the last
//      message before each snapshot time, plus its inside market at the
//      moment the closing cross is published.
//
// Usage: itch_count FILE [--chunk-mb N]
// Build: g++ -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -pthread -static tools/itch_count.cpp -o itch_count

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

uint16_t rd16(const unsigned char* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t rd32(const unsigned char* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint64_t rd48(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v = (v << 8) | p[i];
    return v;
}
uint64_t rd64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

// Message lengths from the NASDAQ TotalView-ITCH 5.0 specification (the
// length prefix counts the message only, type byte included).
int spec_length(unsigned char t) {
    switch (t) {
        case 'S': return 12; case 'R': return 39; case 'H': return 25; case 'Y': return 20;
        case 'L': return 26; case 'V': return 35; case 'W': return 12; case 'K': return 28;
        case 'J': return 35; case 'h': return 21; case 'A': return 36; case 'F': return 40;
        case 'E': return 31; case 'C': return 36; case 'X': return 23; case 'D': return 19;
        case 'U': return 35; case 'P': return 44; case 'Q': return 40; case 'B': return 19;
        case 'I': return 50; case 'N': return 20;
        default: return -1;
    }
}

std::string sym8(const unsigned char* p) {
    std::string s(reinterpret_cast<const char*>(p), 8);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string commas(uint64_t v) {
    std::string s = std::to_string(v), o;
    int n = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (n && n % 3 == 0) o.push_back(',');
        o.push_back(*it);
        ++n;
    }
    return std::string(o.rbegin(), o.rend());
}

std::string clock_str(uint64_t ns) {
    char b[32];
    uint64_t ms = ns / 1000000, s = ms / 1000;
    std::snprintf(b, sizeof b, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ".%03" PRIu64,
                  s / 3600, (s / 60) % 60, s % 60, ms % 1000);
    return b;
}
std::string clock_ns(uint64_t ns) {
    char b[40];
    uint64_t s = ns / 1000000000ull;
    std::snprintf(b, sizeof b, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ".%09" PRIu64,
                  s / 3600, (s / 60) % 60, s % 60, ns % 1000000000ull);
    return b;
}
std::string px(uint32_t w) {
    char b[32];
    std::snprintf(b, sizeof b, "%u.%04u", w / 10000, w % 10000);
    return b;
}

const std::vector<std::string> kSymbols = {"AAPL", "MSFT", "AMZN", "GOOGL", "FB",   "INTC",
                                           "CSCO", "NVDA", "TSLA", "NFLX",  "BKNG", "WFT"};
const uint64_t kHour = 3600ull * 1000000000ull;
const std::vector<std::pair<std::string, uint64_t>> kSnaps = {{"12:00:00.000", 12 * kHour},
                                                              {"16:00:00.000", 16 * kHour}};

struct Cross { char type; uint32_t price; uint64_t shares; uint64_t ts; };
struct Noii {
    bool seen = false;
    uint64_t ts = 0, paired = 0, imbalance = 0;
    char dir = ' ', cross = ' ';
    uint32_t far = 0, near = 0, ref = 0;
};

// Independent book for one symbol: price -> (shares, orders), per side.
struct Level { uint64_t shares = 0; uint32_t orders = 0; };
struct SymBook {
    std::map<uint32_t, Level, std::greater<uint32_t>> bid;
    std::map<uint32_t, Level> ask;
};
struct Ord { uint16_t locate; char side; uint32_t price; uint32_t shares; };

struct Target {
    std::string name;
    std::vector<Cross> crosses;
    Noii last_close_noii;
    uint64_t close_cross_ts = 0;
    bool closed = false;
    // Order messages from 16:00:00.000 up to the closing Cross Trade: type -> (messages, shares)
    std::map<char, std::pair<uint64_t, uint64_t>> pre_cross;
    bool bbo_at_cross = false;
    uint32_t cross_bid = 0, cross_ask = 0;
    uint64_t cross_bid_sh = 0, cross_ask_sh = 0;
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> nprint;  // C, printable N, 16:00-16:01: price -> (shares, msgs)
    SymBook book;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s FILE [--chunk-mb N]\n", argv[0]);
        return 2;
    }
    const char* path = argv[1];
    size_t chunk = size_t{256} << 20;
    for (int i = 2; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--chunk-mb") == 0) chunk = size_t(std::strtoull(argv[i + 1], nullptr, 10)) << 20;

    std::FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("cannot open %s\n", path); return 2; }
    const char* base = std::strrchr(path, '/');
    const char* base2 = std::strrchr(path, '\\');
    if (base2 && (!base || base2 > base)) base = base2;
    base = base ? base + 1 : path;

    const auto t0 = std::chrono::steady_clock::now();
    std::printf("=== itch_count: independent recount of a NASDAQ ITCH 5.0 BinaryFILE ===\n");
    std::printf("file: %s | chunk %s bytes | no code shared with include/lob or src\n\n", base,
                commas(chunk).c_str());

    std::vector<unsigned char> buf(chunk + 65536 + 2);
    size_t have = 0;
    uint64_t file_bytes = 0, msgs = 0, prefix_bytes = 0, body_bytes = 0;
    uint64_t by_type[256] = {};
    uint64_t len_mismatch[256] = {};
    uint64_t zero_len = 0, ts_back = 0, last_ts = 0, first_ts = 0;
    std::vector<uint8_t> loc_A(65536, 0), loc_F(65536, 0), loc_any(65536, 0), loc_R(65536, 0);
    uint64_t r_msgs = 0;

    // Targets are found by name in 'R'; until then no order is tracked.
    std::vector<Target> targets(kSymbols.size());
    for (size_t i = 0; i < kSymbols.size(); ++i) targets[i].name = kSymbols[i];
    std::vector<int> loc_target(65536, -1);
    std::unordered_map<uint64_t, Ord> orders;
    orders.reserve(1 << 20);
    uint64_t unknown_ref = 0;
    size_t next_snap = 0;
    std::vector<std::string> snap_text;

    auto add_level = [&](Target& t, char side, uint32_t p, int64_t dshares, int dorders) {
        auto apply = [&](auto& m) {
            auto& L = m[p];
            L.shares = uint64_t(int64_t(L.shares) + dshares);
            L.orders = uint32_t(int(L.orders) + dorders);
            if (L.orders == 0) m.erase(p);
        };
        if (side == 'B') apply(t.book.bid); else apply(t.book.ask);
    };
    auto reduce = [&](uint64_t ref, uint32_t by, bool remove_all) {
        auto it = orders.find(ref);
        if (it == orders.end()) { ++unknown_ref; return; }
        Ord& o = it->second;
        Target& t = targets[size_t(loc_target[o.locate])];
        uint32_t d = remove_all ? o.shares : (by > o.shares ? o.shares : by);
        o.shares -= d;
        bool gone = o.shares == 0;
        add_level(t, o.side, o.price, -int64_t(d), gone ? -1 : 0);
        if (gone) orders.erase(it);
    };
    auto snapshot = [&](const std::string& label) {
        std::string out;
        char line[256];
        std::snprintf(line, sizeof line, "  independent books at %s ET (state after message %s, top 5 levels):\n",
                      label.c_str(), commas(msgs).c_str());
        out += line;
        for (auto& t : targets) {
            uint64_t resting = 0;
            for (auto& [p, L] : t.book.bid) (void)p, resting += L.orders;
            for (auto& [p, L] : t.book.ask) (void)p, resting += L.orders;
            std::snprintf(line, sizeof line, "  %-6s resting %s | levels bid %zu ask %zu\n", t.name.c_str(),
                          commas(resting).c_str(), t.book.bid.size(), t.book.ask.size());
            out += line;
            auto b = t.book.bid.begin();
            auto a = t.book.ask.begin();
            for (int i = 0; i < 5; ++i) {
                std::string l = "         ";
                if (b != t.book.bid.end()) {
                    std::snprintf(line, sizeof line, "bid %12s %10s (%4u)", px(b->first).c_str(),
                                  commas(b->second.shares).c_str(), b->second.orders);
                    l += line; ++b;
                } else l += std::string(33, ' ');
                l += "   | ";
                if (a != t.book.ask.end()) {
                    std::snprintf(line, sizeof line, "ask %12s %10s (%4u)", px(a->first).c_str(),
                                  commas(a->second.shares).c_str(), a->second.orders);
                    l += line; ++a;
                }
                out += l + "\n";
            }
        }
        snap_text.push_back(out);
    };

    for (;;) {
        size_t got = std::fread(buf.data() + have, 1, chunk, f);
        file_bytes += got;
        have += got;
        size_t pos = 0;
        for (;;) {
            if (have - pos < 2) break;
            uint16_t len = rd16(&buf[pos]);
            if (have - pos < size_t(2) + len) break;
            const unsigned char* m = &buf[pos + 2];
            pos += size_t(2) + len;
            ++msgs;
            prefix_bytes += 2;
            body_bytes += len;
            if (len == 0) { ++zero_len; continue; }
            unsigned char t = m[0];
            ++by_type[t];
            if (spec_length(t) != int(len)) { ++len_mismatch[t]; continue; }
            if (len < 11) continue;
            uint16_t loc = rd16(m + 1);
            uint64_t ts = rd48(m + 5);
            if (msgs == 1) first_ts = ts;
            if (ts < last_ts) ++ts_back;
            last_ts = ts;

            while (next_snap < kSnaps.size() && ts >= kSnaps[next_snap].second) {
                --msgs;  // state after the previous message
                snapshot(kSnaps[next_snap].first);
                ++msgs;
                ++next_snap;
            }

            switch (t) {
                case 'R': {
                    ++r_msgs;
                    loc_R[loc] = 1;
                    std::string s = sym8(m + 11);
                    for (size_t i = 0; i < targets.size(); ++i)
                        if (targets[i].name == s) loc_target[loc] = int(i);
                    break;
                }
                case 'A': case 'F': {
                    (t == 'A' ? loc_A : loc_F)[loc] = 1;
                    loc_any[loc] = 1;
                    int ti = loc_target[loc];
                    if (ti < 0) break;
                    Ord o{loc, char(m[19]), rd32(m + 32), rd32(m + 20)};
                    if (ts >= 16 * kHour && !targets[size_t(ti)].closed) {
                        auto& e = targets[size_t(ti)].pre_cross[char(t)];
                        ++e.first;
                        e.second += o.shares;
                    }
                    orders[rd64(m + 11)] = o;
                    add_level(targets[size_t(ti)], o.side, o.price, o.shares, 1);
                    break;
                }
                case 'E': case 'C': case 'X': case 'D': case 'U': {
                    loc_any[loc] = 1;
                    int ti = loc_target[loc];
                    if (ti < 0) break;
                    uint64_t ref = rd64(m + 11);
                    if (ts >= 16 * kHour && !targets[size_t(ti)].closed) {
                        auto& e = targets[size_t(ti)].pre_cross[char(t)];
                        ++e.first;
                        if (t == 'E' || t == 'C' || t == 'X') e.second += rd32(m + 19);
                        else if (t == 'U') e.second += rd32(m + 27);
                    }
                    if (t == 'E' || t == 'X') reduce(ref, rd32(m + 19), false);
                    else if (t == 'D') reduce(ref, 0, true);
                    else if (t == 'C') {
                        Target& tg = targets[size_t(ti)];
                        uint32_t sh = rd32(m + 19), p = rd32(m + 32);
                        char printable = char(m[31]);
                        if (printable == 'N' && ts >= 16 * kHour && ts < 16 * kHour + 60000000000ull) {
                            auto& e = tg.nprint[p];
                            e.first += sh;
                            ++e.second;
                        }
                        reduce(ref, sh, false);
                    } else {  // U: replace keeps side and locate, new ref, price and shares
                        auto it = orders.find(ref);
                        if (it == orders.end()) { ++unknown_ref; break; }
                        Ord o = it->second;
                        reduce(ref, 0, true);
                        Ord n{o.locate, o.side, rd32(m + 31), rd32(m + 27)};
                        orders[rd64(m + 19)] = n;
                        add_level(targets[size_t(ti)], n.side, n.price, n.shares, 1);
                    }
                    break;
                }
                case 'Q': {
                    int ti = loc_target[loc];
                    if (ti < 0) break;
                    Target& tg = targets[size_t(ti)];
                    Cross c{char(m[39]), rd32(m + 27), rd64(m + 11), ts};
                    tg.crosses.push_back(c);
                    if (c.type == 'C') { tg.closed = true; tg.close_cross_ts = ts; 
                        tg.bbo_at_cross = true;
                        if (!tg.book.bid.empty()) { tg.cross_bid = tg.book.bid.begin()->first; tg.cross_bid_sh = tg.book.bid.begin()->second.shares; }
                        if (!tg.book.ask.empty()) { tg.cross_ask = tg.book.ask.begin()->first; tg.cross_ask_sh = tg.book.ask.begin()->second.shares; }
                    }
                    break;
                }
                case 'I': {
                    int ti = loc_target[loc];
                    if (ti < 0) break;
                    Target& tg = targets[size_t(ti)];
                    if (char(m[48]) == 'C' && !tg.closed) {
                        Noii& n = tg.last_close_noii;
                        n.seen = true;
                        n.ts = ts;
                        n.paired = rd64(m + 11);
                        n.imbalance = rd64(m + 19);
                        n.dir = char(m[27]);
                        n.far = rd32(m + 36);
                        n.near = rd32(m + 40);
                        n.ref = rd32(m + 44);
                        n.cross = char(m[48]);
                    }
                    break;
                }
                default: break;
            }
        }
        size_t rest = have - pos;
        if (rest) std::memmove(buf.data(), buf.data() + pos, rest);
        have = rest;
        if (got == 0) break;
    }
    std::fclose(f);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    uint64_t len_bad = 0;
    for (auto v : len_mismatch) len_bad += v;
    auto count_set = [](const std::vector<uint8_t>& v) { uint64_t n = 0; for (auto x : v) n += x; return n; };
    uint64_t loc_AF = 0;
    for (size_t i = 0; i < 65536; ++i) loc_AF += (loc_A[i] | loc_F[i]);

    std::printf("[1] framing (2-byte big-endian length, then the message)\n");
    std::printf("  bytes read %s | messages %s\n", commas(file_bytes).c_str(), commas(msgs).c_str());
    std::printf("  length-prefix bytes %s + message bytes %s = %s\n", commas(prefix_bytes).c_str(),
                commas(body_bytes).c_str(), commas(prefix_bytes + body_bytes).c_str());
    std::printf("  bytes left after the last whole message: %s (%s)\n", commas(have).c_str(),
                have == 0 ? "file ends exactly on a message boundary" : "FILE DOES NOT END ON A MESSAGE BOUNDARY");
    std::printf("  zero-length messages %s | length prefix differs from the ITCH 5.0 length for its type: %s\n",
                commas(zero_len).c_str(), commas(len_bad).c_str());
    std::printf("  first timestamp %s | last %s | timestamps lower than the previous one: %s\n\n",
                clock_str(first_ts).c_str(), clock_str(last_ts).c_str(), commas(ts_back).c_str());

    std::printf("[2] messages by type byte\n");
    int col = 0;
    uint64_t sum = 0;
    for (int c = 0; c < 256; ++c) {
        if (!by_type[c]) continue;
        sum += by_type[c];
        if (c >= 33 && c < 127) std::printf("     %c %13s", c, commas(by_type[c]).c_str());
        else std::printf("  0x%02x %12s", c, commas(by_type[c]).c_str());
        if (spec_length((unsigned char)c) < 0) std::printf(" (not an ITCH 5.0 type)");
        if (++col % 4 == 0) std::printf("\n");
    }
    if (col % 4) std::printf("\n");
    std::printf("  sum over types %s (equals messages: %s)\n", commas(sum).c_str(), sum + zero_len == msgs ? "yes" : "NO");
    std::printf("  adds A + F + U (the add half of a replace): %s\n\n",
                commas(by_type['A'] + by_type['F'] + by_type['U']).c_str());

    std::printf("[3] stock_locate values\n");
    std::printf("  'R' messages %s for %s distinct locates\n", commas(r_msgs).c_str(), commas(count_set(loc_R)).c_str());
    std::printf("  distinct locates in 'A' %s | in 'F' %s | in 'A' or 'F' %s | in any order message (A F E C X D U) %s\n\n",
                commas(count_set(loc_A)).c_str(), commas(count_set(loc_F)).c_str(), commas(loc_AF).c_str(),
                commas(count_set(loc_any)).c_str());

    std::printf("[4] crosses for the named symbols (Cross Trade 'Q': O opening, C closing, H halt/IPO, I intraday)\n");
    for (auto& t : targets) {
        if (t.crosses.empty() && !t.last_close_noii.seen) { std::printf("  %-6s no 'Q' message\n", t.name.c_str()); continue; }
        for (auto& c : t.crosses)
            std::printf("  %-6s cross %c price %12s shares %12s at %s\n", t.name.c_str(), c.type, px(c.price).c_str(),
                        commas(c.shares).c_str(), clock_ns(c.ts).c_str());
        const Noii& n = t.last_close_noii;
        if (n.seen)
            std::printf("  %-6s last closing NOII before the cross, %s: paired %s | imbalance %s %c | "
                        "reference %s near %s far %s\n",
                        t.name.c_str(), clock_ns(n.ts).c_str(), commas(n.paired).c_str(), commas(n.imbalance).c_str(),
                        n.dir, px(n.ref).c_str(), px(n.near).c_str(), px(n.far).c_str());
        if (t.nprint.empty())
            std::printf("  %-6s no non-printable 'C' executions between 16:00:00 and 16:01:00\n", t.name.c_str());
        for (auto& [p, e] : t.nprint)
            std::printf("  %-6s non-printable 'C' executions 16:00:00-16:01:00 at %s: %s shares in %s messages\n",
                        t.name.c_str(), px(p).c_str(), commas(e.first).c_str(), commas(e.second).c_str());
        std::string pc;
        for (auto& [ty, e] : t.pre_cross)
            pc += std::string(" ") + ty + " " + commas(e.first) + " msgs/" + commas(e.second) + " sh";
        std::printf("  %-6s order messages 16:00:00.000 to the closing cross:%s\n", t.name.c_str(),
                    pc.empty() ? " none" : pc.c_str());
        if (t.bbo_at_cross)
            std::printf("  %-6s book when the closing cross was published: bid %s x %s / ask %s x %s\n", t.name.c_str(),
                        t.cross_bid ? px(t.cross_bid).c_str() : "-", commas(t.cross_bid_sh).c_str(),
                        t.cross_ask ? px(t.cross_ask).c_str() : "-", commas(t.cross_ask_sh).c_str());
    }
    std::printf("\n");

    std::printf("[5] independent books for the named symbols (%s orders tracked at end, unknown refs %s)\n",
                commas(orders.size()).c_str(), commas(unknown_ref).c_str());
    for (auto& s : snap_text) std::printf("%s", s.c_str());
    std::printf("\n  wall time %.1f s\n", secs);
    bool ok = have == 0 && len_bad == 0 && zero_len == 0 && unknown_ref == 0;
    std::printf("\nRESULT: %s\n", ok ? "CLEAN" : "PROBLEMS FOUND");
    return ok ? 0 : 1;
}
