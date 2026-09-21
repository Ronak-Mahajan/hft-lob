#pragma once
// ---------------------------------------------------------------------------
// wire_gen.hpp - synthetic multi-symbol ITCH 5.0 stream in wire units.
//
// Test input for the ExactOrderBook paths that the benchmark mock never
// reaches: prices below and above a ladder, between its grid points
// (sub-penny), on its first and last ticks and one past them, near $0 and
// above $214,748.3647 (past the int32 range); replaces that move orders
// between the ladder and the overflow; several stock_locates interleaved; and
// the non-order message types a real day carries between order messages
// ('S', 'R', 'P', 'I', 'Q'). Each symbol is generated against a ladder
// (base, tick, band) so the boundary prices are the ones a book built with
// the same parameters actually straddles. Output is the length-prefixed
// BinaryFILE framing that NASDAQ's historical files use.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct WireSymbol {
    uint16_t    locate;
    std::string name;       // up to 8 characters
    uint32_t    base;       // ladder the test's book uses: [base, base + band * tick)
    uint32_t    tick;
    uint32_t    band;
};

class WireGen {
public:
    // max_live bounds the resting-order population: at that size every order
    // message is a delete until it shrinks.
    WireGen(uint64_t seed, std::vector<WireSymbol> syms, size_t max_live = 50000)
        : s_(seed), syms_(std::move(syms)), max_live_(max_live) {}

    // Appends the start-of-day messages and n more messages, most of them
    // order messages, to `bytes`.
    void generate(size_t n) {
        system_event('O');
        for (const WireSymbol& s : syms_) directory(s);
        system_event('Q');
        for (size_t i = 0; i < n; ++i) {
            uint32_t r = below(100);
            if (r < 3) { filler(); continue; }
            const WireSymbol& s = syms_[below(static_cast<uint32_t>(syms_.size()))];
            const bool force_add = live_.size() < 64;
            const bool force_del = live_.size() >= max_live_;
            if (force_add || (!force_del && r < 40)) add(s);
            else if (force_del || r < 66)          del();
            else if (r < 76)                       execute(false);
            else if (r < 81)                       execute(true);
            else if (r < 89)                       cancel();
            else                                   replace();
        }
        system_event('M');
        system_event('C');
    }

    std::vector<uint8_t> bytes;
    size_t live_orders() const { return live_.size(); }

private:
    struct Live { uint64_t ref; uint16_t locate; uint32_t shares; };

    uint64_t next() {                                   // SplitMix64
        uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }

    void p8(uint8_t v)   { bytes.push_back(v); }
    void p16(uint16_t v) { p8(static_cast<uint8_t>(v >> 8)); p8(static_cast<uint8_t>(v)); }
    void p32(uint32_t v) { p16(static_cast<uint16_t>(v >> 16)); p16(static_cast<uint16_t>(v)); }
    void p48(uint64_t v) { p16(static_cast<uint16_t>(v >> 32)); p32(static_cast<uint32_t>(v)); }
    void p64(uint64_t v) { p32(static_cast<uint32_t>(v >> 32)); p32(static_cast<uint32_t>(v)); }
    void pstr(const std::string& s, size_t n) {
        for (size_t i = 0; i < n; ++i) p8(i < s.size() ? static_cast<uint8_t>(s[i]) : ' ');
    }
    void header(char type, uint16_t len, uint16_t locate) {
        p16(len);
        p8(static_cast<uint8_t>(type));
        p16(locate);
        p16(0);
        ts_ += 1 + below(20'000'000);             // about 10 ms apart on average
        p48(ts_);
    }

    void system_event(char code) { header('S', 12, 0); p8(static_cast<uint8_t>(code)); }
    void directory(const WireSymbol& s) {
        header('R', 39, s.locate);
        pstr(s.name, 8);
        pstr("Q N", 2);                 // market category, financial status
        p32(100);                       // round lot
        pstr("NCZ PN 1N", 9);           // round-lot flag .. ETP flag
        p32(0);                         // ETP leverage
        p8('N');                        // inverse
    }
    void filler() {
        const WireSymbol& s = syms_[below(static_cast<uint32_t>(syms_.size()))];
        switch (below(3)) {
        case 0:                         // 'P' non-cross trade: 44 bytes
            header('P', 44, s.locate);
            p64(0); p8('B'); p32(100); pstr(s.name, 8); p32(price_for(s)); p64(++match_);
            break;
        case 1:                         // 'I' NOII: 50 bytes
            header('I', 50, s.locate);
            p64(1000); p64(10); p8('B'); pstr(s.name, 8);
            p32(price_for(s)); p32(price_for(s)); p32(price_for(s)); p8('C'); p8('L');
            break;
        default:                        // 'Q' cross trade: 40 bytes
            header('Q', 40, s.locate);
            p64(500); pstr(s.name, 8); p32(price_for(s)); p64(++match_); p8('C');
            break;
        }
    }

    // A price drawn so that every routing case of the ExactOrderBook occurs.
    uint32_t price_for(const WireSymbol& s) {
        const uint64_t top = uint64_t{s.base} + uint64_t{s.band} * s.tick;   // one past the ladder
        uint32_t r = below(100);
        uint64_t p;
        if (r < 55)       p = s.base + uint64_t{s.tick} * below(s.band);                 // on the ladder
        else if (r < 63)  p = s.tick > 1 ? s.base + uint64_t{s.tick} * below(s.band) + 1 + below(s.tick - 1)
                                         : s.base + below(s.band);                        // between grid points
        else if (r < 71)  p = s.base > uint64_t{s.tick} * 40 ? s.base - uint64_t{s.tick} * (1 + below(40))
                                                             : below(s.base + 1);         // just below
        else if (r < 79)  p = top + uint64_t{s.tick} * below(40);                         // just above
        else if (r < 83)  p = s.base;                                                     // first tick
        else if (r < 87)  p = top - s.tick;                                               // last tick
        else if (r < 90)  p = top;                                                        // one past
        else if (r < 92)  p = s.base == 0 ? 0 : s.base - 1;                               // one below
        else if (r < 95)  p = 1 + below(100);                                             // near $0
        else if (r < 97)  p = 1'999'999'900;                                              // $199,999.99
        else if (r < 99)  p = 3'000'000'000u + below(1000) * 100;                         // past int32
        else              p = 0xFFFFFFFFu - below(3);                                     // u32 max
        return p > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(p);
    }

    void add(const WireSymbol& s) {
        uint64_t ref = ++next_ref_;
        char side = (next() & 1) ? 'B' : 'S';
        uint32_t shares = 1 + below(900);
        bool mpid = (next() & 3) == 0;
        header(mpid ? 'F' : 'A', mpid ? 40 : 36, s.locate);
        p64(ref); p8(static_cast<uint8_t>(side)); p32(shares); pstr(s.name, 8); p32(price_for(s));
        if (mpid) pstr("MPID", 4);
        pos_[ref] = live_.size();
        live_.push_back({ref, s.locate, shares});
    }
    size_t pick() { return below(static_cast<uint32_t>(live_.size())); }
    void drop(size_t i) {
        pos_.erase(live_[i].ref);
        if (i + 1 != live_.size()) {
            live_[i] = live_.back();
            pos_[live_[i].ref] = i;
        }
        live_.pop_back();
    }
    void del() {
        size_t i = pick();
        header('D', 19, live_[i].locate); p64(live_[i].ref);
        drop(i);
    }
    void execute(bool priced) {
        size_t i = pick();
        Live& o = live_[i];
        uint32_t d = (next() & 3) == 0 ? o.shares : 1 + below(o.shares);   // often full
        header(priced ? 'C' : 'E', priced ? 36 : 31, o.locate);
        p64(o.ref); p32(d); p64(++match_);
        if (priced) { p8('Y'); p32(1'000'000 + below(1000)); }
        if (d >= o.shares) drop(i); else o.shares -= d;
    }
    void cancel() {
        size_t i = pick();
        Live& o = live_[i];
        if (o.shares <= 1) { del(); return; }
        uint32_t d = 1 + below(o.shares - 1);
        header('X', 23, o.locate); p64(o.ref); p32(d);
        o.shares -= d;
    }
    void replace() {
        size_t i = pick();
        Live o = live_[i];
        const WireSymbol* s = nullptr;
        for (const WireSymbol& c : syms_) if (c.locate == o.locate) s = &c;
        uint64_t ref = ++next_ref_;
        uint32_t shares = 1 + below(900);
        header('U', 35, o.locate);
        p64(o.ref); p64(ref); p32(shares); p32(price_for(*s));
        drop(i);
        pos_[ref] = live_.size();
        live_.push_back({ref, o.locate, shares});
    }

    uint64_t s_;
    std::vector<WireSymbol> syms_;
    size_t max_live_;
    std::vector<Live> live_;
    std::unordered_map<uint64_t, size_t> pos_;
    uint64_t next_ref_ = 0, match_ = 0, ts_ = 34'200'000'000'000ull;   // 09:30:00
};
